#!/usr/bin/env python3
"""
Runs the real compiled plugin (scanline.elf) in a MIPS32 emulator against a
stand-in sceDisplaySetFrameBuf, to check the parts that only exist as MIPS
code: the inline hook, the trampoline, the frame pass, and unhooking.

  1. hook_install() on a function with an ordinary prologue: the first two
     words become "j hook_setframebuf; nop", the trampoline gets them.
  2. A call to the function as a game would make it: the frame in VRAM is
     darkened exactly as lines.c says, and the original body still runs
     with the original arguments and returns its own result.
  3. The same buffer flipped again: nothing changes.
  4. Another plugin already put a jump at the start: we chain behind it.
  5. A function starting with a branch can't be moved: we fall back to the
     syscall table and leave the code alone.
  6. hook_remove() puts the original words back.

What this cannot check: the real sceDisplay code, real caches, timing.
"""
import struct, sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn.mips_const import *

ELF = sys.argv[1] if len(sys.argv) > 1 else "scanline.elf"
fails = 0
def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond: fails += 1

elf = ELFFile(open(ELF, "rb"))
SYM = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols() if s.name}
secs = [s for s in elf.iter_sections() if s["sh_flags"] & 2]
IMG_END = max(s["sh_addr"] + s["sh_size"] for s in secs)
R_32, R_26, R_HI16, R_LO16 = 2, 4, 5, 6

MOD_PHYS = 0x08800000
BASE = 0x80000000 | MOD_PHYS
VRAM = 0x04000000
HALT = 0x88000000            # returning here stops the emulator
TARGET = 0x88010000          # stand-in sceDisplaySetFrameBuf
OTHER = 0x88020000           # another plugin's hook
GE_SYNC = 0x88030000         # stand-in sceGeDrawSync
IN_INTR = 0x88040000         # stand-in sceKernelIsIntrContext
HCOUNT = 0x88050000          # stand-in sceDisplayGetCurrentHcount

def load(uc):
    img = bytearray(IMG_END)
    for s in secs:
        if s["sh_type"] != "SHT_NOBITS":
            img[s["sh_addr"]: s["sh_addr"] + s["sh_size"]] = s.data()
    for rs in elf.iter_sections():
        if rs["sh_type"] != "SHT_REL": continue
        tgt = elf.get_section(rs["sh_info"])
        if not (tgt["sh_flags"] & 2) or tgt.name == ".sceStub.text": continue
        for r in rs.iter_relocations():
            off, t = r["r_offset"], r["r_info_type"]
            w = struct.unpack_from("<I", img, off)[0]
            if t == R_32:     w = (w + BASE) & 0xFFFFFFFF
            elif t == R_26:   w = (w & 0xFC000000) | (((w & 0x3FFFFFF) + (BASE >> 2)) & 0x3FFFFFF)
            elif t == R_HI16: w = (w & 0xFFFF0000) | (((w & 0xFFFF) + (BASE >> 16)) & 0xFFFF)
            elif t == R_LO16: pass
            else: raise SystemExit("unhandled relocation %d" % t)
            struct.pack_into("<I", img, off, w)
    uc.mem_write(MOD_PHYS, bytes(img))

def S(name): return BASE + SYM[name]

# The Allegrex has min and max (SPECIAL, funct 0x2D / 0x2C), which a stock
# MIPS32 emulator does not. Each one in the plugin's code is carried out here
# instead. None of them sits in a branch delay slot (checked below), so
# stepping the PC past them is exact.
def allegrex_ops(uc):
    text = elf.get_section_by_name(".text")
    data, lo = text.data(), text["sh_addr"]
    sv = lambda u, r: struct.unpack("<i", struct.pack("<I", u.reg_read(UC_MIPS_REG_0 + r)))[0]
    def do(u, w):
        rs, rt, rd, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 63
        v = max(sv(u, rs), sv(u, rt)) if fn == 0x2C else min(sv(u, rs), sv(u, rt))
        if rd: u.reg_write(UC_MIPS_REG_0 + rd, v & 0xFFFFFFFF)
    found = 0
    for off in range(0, len(data), 4):
        w = struct.unpack_from("<I", data, off)[0]
        if not (w >> 26 == 0 and w & 0x7FF in (0x2C, 0x2D)): continue
        found += 1
        prev = struct.unpack_from("<I", data, off - 4)[0] if off else 0
        if not is_ctl(prev):
            def h(u, a, size, _, w=w):
                do(u, w); u.reg_write(UC_MIPS_REG_PC, a + 4)
            uc.hook_add(UC_HOOK_CODE, h, begin=BASE + lo + off, end=BASE + lo + off)
            continue
        # In a branch's delay slot: carry out the branch and the slot together.
        op, rs, rt = prev >> 26, (prev >> 21) & 31, (prev >> 16) & 31
        simm = struct.unpack("<h", struct.pack("<H", prev & 0xFFFF))[0]
        assert op in (1, 4, 5, 6, 7, 20, 21, 22, 23), "min/max after an unsupported jump at %x" % (lo + off)
        def hb(u, a, size, _, w=w, op=op, rs=rs, rt=rt, simm=simm):
            x, y = sv(u, rs), sv(u, rt)
            kind = op & 3 if op != 1 else None
            if op == 1: taken = (x >= 0) if rt & 1 else (x < 0)
            else: taken = (x == y, x != y, x <= 0, x > 0)[kind]
            likely = op >= 20 or (op == 1 and rt & 2)
            if taken or not likely: do(u, w)
            u.reg_write(UC_MIPS_REG_PC, (a + 4 + simm * 4) if taken else a + 8)
        uc.hook_add(UC_HOOK_CODE, hb, begin=BASE + lo + off - 4, end=BASE + lo + off - 4)
    return found

def is_ctl(w):
    op, rs, fn = w >> 26, (w >> 21) & 31, w & 63
    return (op == 0 and fn in (8, 9)) or op in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) or (op in (17, 18) and rs == 8)
def rd(uc, a, n=4): return bytes(uc.mem_read(a & 0x1FFFFFFF, n))
def rd32(uc, a): return struct.unpack("<I", rd(uc, a))[0]
def wr32(uc, a, v): uc.mem_write(a & 0x1FFFFFFF, struct.pack("<I", v & 0xFFFFFFFF))
def J(a): return 0x08000000 | ((a >> 2) & 0x03FFFFFF)

def new_uc(intr_nid=0xFE28C6D9):
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(0x08000000, 0x02000000)
    uc.mem_map(VRAM, 0x00200000)
    load(uc)
    allegrex_ops(uc)
    calls = {"patch_syscall": [], "flush": 0, "ge": [], "ge_script": [], "sleeps": 0, "intr_nid": intr_nid}
    clock = [1000]
    def stub(name, fn):
        uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: fn(u), begin=S(name), end=S(name))
    def find(u):                 # by NID: the display function, or the GE status call
        nid = u.reg_read(UC_MIPS_REG_A2)
        u.reg_write(UC_MIPS_REG_V0, {0x289D82FE: TARGET, 0xB287BD61: GE_SYNC, calls["intr_nid"]: IN_INTR, 0x773DD3A3: HCOUNT}.get(nid, 0))
    def ge_sync(u, a, size, _):  # answers from the script, then "done"
        calls["ge"].append(u.reg_read(UC_MIPS_REG_A0))
        u.reg_write(UC_MIPS_REG_V0, calls["ge_script"].pop(0) if calls["ge_script"] else 0)
    uc.mem_write(GE_SYNC & 0x1FFFFFFF, struct.pack("<II", 0x03E00008, 0))
    uc.hook_add(UC_HOOK_CODE, ge_sync, begin=GE_SYNC, end=GE_SYNC)
    def delay(u):
        calls["sleeps"] += 1; clock[0] += u.reg_read(UC_MIPS_REG_A0); u.reg_write(UC_MIPS_REG_V0, calls.get("delay_result", 0))
    stub("sceKernelDelayThread", delay)
    def time(u): clock[0] += 50; u.reg_write(UC_MIPS_REG_V0, clock[0])
    def patch(u): calls["patch_syscall"].append((u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1)))
    def flush(u): calls["flush"] += 1
    stub("sctrlHENFindFunction", find)
    stub("sceKernelGetSystemTimeLow", time)
    stub("sctrlHENPatchSyscall", patch)
    stub("sctrlFlushCache", flush)
    calls["ranges"] = []
    def rng(u): calls["ranges"].append((u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1)))
    stub("sceKernelDcacheWritebackInvalidateRange", rng)
    calls["intr_on"] = 1; calls["in_intr"] = 0; calls["evf_set"] = []
    stub("sceKernelCpuSuspendIntr", lambda u: u.reg_write(UC_MIPS_REG_V0, calls["intr_on"]))
    calls["hcount"] = 280; calls["hcount_asked"] = 0             # 280: in the blank period
    uc.mem_write(HCOUNT & 0x1FFFFFFF, struct.pack("<II", 0x03E00008, 0))
    def hc(u, a, s, d): calls["hcount_asked"] += 1; u.reg_write(UC_MIPS_REG_V0, calls["hcount"])
    uc.hook_add(UC_HOOK_CODE, hc, begin=HCOUNT, end=HCOUNT)
    uc.mem_write(IN_INTR & 0x1FFFFFFF, struct.pack("<II", 0x03E00008, 0))
    uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.reg_write(UC_MIPS_REG_V0, calls["in_intr"]), begin=IN_INTR, end=IN_INTR)
    stub("sceKernelCreateEventFlag", lambda u: u.reg_write(UC_MIPS_REG_V0, 0x1234))
    stub("sceKernelCreateThread", lambda u: u.reg_write(UC_MIPS_REG_V0, 0xFFFFFFFF))     # the thread is not run here
    def evset(u): calls["evf_set"].append((u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1))); u.reg_write(UC_MIPS_REG_V0, 0)
    stub("sceKernelSetEventFlag", evset)
    # The kernel's memset is an import too; the compiler relies on it returning dst.
    def memset(u):
        d, c, n = (u.reg_read(x) for x in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
        u.mem_write(d & 0x1FFFFFFF, bytes([c & 0xFF]) * n)
        u.reg_write(UC_MIPS_REG_V0, d)
    if "memset" in SYM: stub("memset", memset)
    def strlen(u):               # likewise an import the compiler makes out of a counting loop
        p = u.reg_read(UC_MIPS_REG_A0) & 0x1FFFFFFF; n = 0
        while u.mem_read(p + n, 1)[0]: n += 1
        u.reg_write(UC_MIPS_REG_V0, n)
    if "strlen" in SYM: stub("strlen", strlen)
    return uc, calls

def call(uc, fn, args=(), sregs=True):
    for r, v in zip((UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3), args):
        uc.reg_write(r, v)
    sp = 0x89F00000
    uc.reg_write(UC_MIPS_REG_SP, sp)
    uc.reg_write(UC_MIPS_REG_RA, HALT)
    s = [UC_MIPS_REG_S0 + i for i in range(8)] + [UC_MIPS_REG_FP]
    for i, r in enumerate(s): uc.reg_write(r, 0x5000 + i)
    uc.emu_start(fn, HALT, count=20_000_000)
    ok = uc.reg_read(UC_MIPS_REG_SP) == sp and all(uc.reg_read(r) == 0x5000 + i for i, r in enumerate(s))
    return uc.reg_read(UC_MIPS_REG_V0), ok

# Stand-in for the system function: v0 = width + format + sync, using its own stack frame.
BODY = [0x27BDFFF0,   # addiu sp, sp, -16
        0xAFBF000C,   # sw    ra, 12(sp)
        0x00A61021,   # addu  v0, a1, a2
        0x00471021,   # addu  v0, v0, a3
        0x8FBF000C,   # lw    ra, 12(sp)
        0x03E00008,   # jr    ra
        0x27BD0010]   # addiu sp, sp, 16

def put_code(uc, addr, words):
    uc.mem_write(addr & 0x1FFFFFFF, b"".join(struct.pack("<I", w) for w in words))

def frame(uc, seed=7):
    import random
    rnd = random.Random(seed)
    data = bytes(rnd.getrandbits(8) for _ in range(512 * 272 * 2))
    uc.mem_write(VRAM, data)
    return data

def ref565(p):        # 50%: each channel halved
    r, g, b = p & 31, (p >> 5) & 63, (p >> 11) & 31
    return (r >> 1) | ((g >> 1) << 5) | ((b >> 1) << 11)

def expect(data):
    out = bytearray(data)
    for y in range(1, 272, 2):
        for x in range(480):
            o = (y * 512 + x) * 2
            struct.pack_into("<H", out, o, ref565(struct.unpack_from("<H", data, o)[0]))
    return bytes(out)

def same(a, b):
    """Equal apart from the lowest bit of each colour, which the plugin's stamps may change
    (at most 2 words in each of the 4 segments of a dark row)."""
    if len(a) != len(b): return False
    if a == b: return True
    n = 0
    for i in range(0, len(a), 2):
        if a[i:i+2] != b[i:i+2]:
            x = struct.unpack_from("<H", a, i)[0] ^ struct.unpack_from("<H", b, i)[0]
            if x & ~0x0821: return False
            n += 1
    return n <= 136 * 16

def ready(uc):
    wr32(uc, S("g_ready"), 1); wr32(uc, S("g_enabled"), 1); wr32(uc, S("g_fast"), 1)
    wr32(uc, S("g_set"), 3); wr32(uc, S("g_set") + 4, 2)

# ---- 1..3, 6: ordinary prologue ---------------------------------------------------
uc, calls = new_uc()
put_code(uc, TARGET, BODY)
r, _ = call(uc, S("hook_install"))
check(r == 0 and rd32(uc, TARGET) == J(S("hook_setframebuf")) and rd32(uc, TARGET + 4) == 0,
      "install: the function now starts with j hook_setframebuf; nop")
check([rd32(uc, S("g_tramp") + 4 * i) for i in range(4)] == [BODY[0], BODY[1], J(TARGET + 8), 0] and calls["flush"] >= 1,
      "install: the trampoline holds the two moved words and jumps back to the third; caches flushed")

ready(uc)
data = frame(uc)
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
after = rd(uc, VRAM, 512 * 272 * 2)
check(v0 == 513 and regs, "a game's call still runs the original with its own arguments (result 512+0+1) and keeps sp/s0-s7")
check(same(after, expect(data)), "the frame in VRAM is darkened: odd rows halved, even rows and padding untouched")
check(rd32(uc, S("g_st_frames")) == 1 and rd32(uc, S("g_st_rows")) == 136, "stats: one frame, 136 rows")
K0 = 0x80000000 | VRAM
rows = [(K0 + y * 1024, K0 + y * 1024 + 960) for y in range(1, 272, 2)]
got = [(a, a + n) for a, n in calls["ranges"]]
check("sceKernelDcacheWritebackInvalidateAll" not in SYM, "the whole-cache flush is no longer in the plugin at all")
check(len(got) == 272 and sorted(got) == sorted(rows + rows) and all(a % 64 == 0 and b % 64 == 0 for a, b in got),
      "cache: exactly the 136 dark rows are flushed, each once before and once after, on whole cache lines")
check(got[0] == rows[0] and max(b for a, b in got[:40]) <= K0 + 40 * 1024,
      "the frame is done from the top in bands: the first 40 rows are finished before any row below is touched")

v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and rd(uc, VRAM, 512 * 272 * 2) == after and rd32(uc, S("g_st_skipped")) == 136,
      "flipping the same buffer again changes nothing (136 rows recognised as done)")

wr32(uc, S("g_ready"), 0)
data = frame(uc, 9)
call(uc, TARGET, (VRAM, 512, 0, 1))
check(rd(uc, VRAM, 512 * 272 * 2) == data, "before the config is read, frames are left alone")
ready(uc)
v0, _ = call(uc, TARGET, (0x00001000, 512, 0, 1))
check(v0 == 513 and rd32(uc, S("g_st_reject")) == 1, "an address outside VRAM/RAM is refused, the call still goes through")

# ---- "change the picture now": the game's call goes through before any drawing -----
from unicorn import UC_HOOK_MEM_WRITE
order = []
h1 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("original"), begin=TARGET + 8, end=TARGET + 8)
# "draw" = the darkening pass is entered. (A memory-write hook on VRAM was used
# here before; with it this emulator ran part of a block twice once the pass
# got longer, which the plugin's code does not do.)
h2 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("draw") if not order or order[-1] != "draw" else None,
                 begin=S("sl_frame_rows"), end=S("sl_frame_rows"))
data = frame(uc, 51); calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [0]
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 0))                  # sync = 0, GE idle
check(v0 == 512 and regs and order[:2] == ["original", "draw"] and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and calls["ge"] == [1],
      "an immediate flip is passed on first and the lines drawn straight after (GE asked once, idle)")
order.clear()
data = frame(uc, 53); calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [2, 2, 2, 0]
nb = rd32(uc, S("g_st_nowbusy"))
order.clear()
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 0))                  # sync = 0, GE still drawing this frame
calls["ge_script"].clear()
check(v0 == 512 and order[:2] == ["original", "draw"] and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and calls["ge"] == [1, 1, 1, 1]
      and calls["sleeps"] == 0 and rd32(uc, S("g_st_nowbusy")) == nb + 1,
      "an immediate flip with the GE still drawing: passed on first, then the GE is asked in quick succession (no sleep) until done, then the lines")
order.clear()
data = frame(uc, 54); calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [2] * 400
call(uc, TARGET, (VRAM, 512, 0, 0))
calls["ge_script"].clear()
check(calls["sleeps"] >= 30 and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)),
      "... and if it takes longer than 0.6 ms the waiting goes over to sleeping in steps, 12 ms at most")
order.clear()
data = frame(uc, 52)
order.clear()
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))                  # sync = 1, at the next refresh
check(v0 == 513 and order[:2] == ["draw", "original"] and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)),
      "a flip for the next refresh is still darkened before it is passed on")
uc.hook_del(h1); uc.hook_del(h2)

# ---- a frame already on screen: begin just below the row the screen is showing ------
K0 = 0x80000000 | VRAM
def flushed_rows(): return [(a - K0) // 1024 for a, n in calls["ranges"]]
for hrow, want_first in ((100, 107), (5, 1), (271, 1), (280, 1), (150, 157)):
    data = frame(uc, 60 + hrow); calls["ranges"].clear(); calls["hcount"] = hrow
    call(uc, TARGET, (VRAM, 512, 0, 0))
    got = flushed_rows()
    ok = same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and sorted(got) == sorted(list(range(1, 272, 2)) * 2) and got[0] == want_first
    if want_first > 1:      # everything from there down is finished before any row above it is touched
        k = min(i for i, y in enumerate(got) if y < want_first)
        ok = ok and sorted(set(got[:k])) == list(range(want_first, 272, 2)) and all(got[:k].count(y) == 2 for y in set(got[:k]))
    check(ok, "screen at row %d: the pass begins at row %d, runs to the bottom, then does the rows above; same picture, every dark row done once" % (hrow, want_first))
calls["hcount"] = 100; calls["hcount_asked"] = 0; frame(uc, 70); calls["ranges"].clear()
call(uc, TARGET, (VRAM, 512, 0, 1))
check(calls["hcount_asked"] == 0 and flushed_rows()[0] == 1, "a frame darkened before it is shown is not asked about the screen row and starts at the top")
calls["hcount"] = 280

# ---- the label ------------------------------------------------------------------
uc.mem_write(S("g_osd_text") & 0x1FFFFFFF, b"SCANLINES 25%  1 IN 2\0")
wr32(uc, S("g_osd_frames"), 5)
data = frame(uc, 77); calls["ranges"].clear()
call(uc, TARGET, (VRAM, 512, 0, 1))
out = rd(uc, VRAM, 512 * 272 * 2)
box = [struct.unpack_from("<H", out, (y * 512 + x) * 2)[0] for y in range(0, 14) for x in range(308, 480)]
left = [struct.unpack_from("<H", out, (y * 512 + x) * 2)[0] for y in (0, 2, 4) for x in range(200, 300)]
first = [((a - (0x80000000 | VRAM)) // 1024, n) for a, n in calls["ranges"][:3]]
check(first[0][0] == 0 and first[0][1] > 29 * 1024 and first[1] == first[0] and first[2][1] == 960,
      "label: its rows are flushed, it is drawn and flushed again before the first dark row is touched")
check(set(box) == {0, 0xFFFF} and box.count(0xFFFF) > 200 and len(set(left)) > 50 and rd32(uc, S("g_osd_frames")) == 4,
      "label: flush in the top right corner (x 308 to 479, rows 0 to 13; the % is drawn small), clean black and white, picture beside it untouched")
wr32(uc, S("g_osd_frames"), 0)

# ---- the game flips from an interrupt handler -----------------------------------
for why, key, val in (("in an interrupt handler", "in_intr", 1), ("with interrupts off", "intr_on", 0)):
    data = frame(uc, 31)
    calls[key] = val; calls["evf_set"].clear(); calls["ge"].clear(); calls["ranges"].clear()
    n0 = rd32(uc, S("g_st_intr"))
    v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
    check(v0 == 513 and regs and rd(uc, VRAM, 512 * 272 * 2) == data and calls["evf_set"] == [(0x1234, 1)] and calls["ge"] == []
          and calls["ranges"] == [] and rd32(uc, S("g_st_intr")) == n0 + 1,
          "flip %s: nothing is drawn there, our thread is woken, the original runs" % why)
    calls["in_intr"] = 0; calls["intr_on"] = 1
    calls["ge_script"][:] = [2] * 50                 # the GE is busy with the next frame by now
    calls["sleeps"] = 0
    call(uc, S("draw_pending"))
    calls["ge_script"].clear()
    check(same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and calls["ge"] == [] and calls["sleeps"] == 0,
          "  ... and the thread then draws that frame, at once, without asking the GE")
    after2 = rd(uc, VRAM, 512 * 272 * 2)
    call(uc, S("draw_pending"))
    check(rd(uc, VRAM, 512 * 272 * 2) == after2, "  ... once only")

# ---- the switch back: work in place after all -------------------------------------
wr32(uc, S("g_defer"), 0)
data = frame(uc, 33)
calls["in_intr"] = 1; calls["evf_set"].clear()
call(uc, TARGET, (VRAM, 512, 0, 1))
calls["in_intr"] = 0
check(same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and calls["evf_set"] == [], "defer off: a flip from an interrupt is drawn in place, as before")
wr32(uc, S("g_defer"), 1)

# ---- mode 2: the lines always go on before the game's call is passed on -----------
wr32(uc, S("g_mode2"), 1)
order = []
h1 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("original"), begin=TARGET + 8, end=TARGET + 8)
h2 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("draw") if not order or order[-1] != "draw" else None,
                 begin=S("sl_frame_rows"), end=S("sl_frame_rows"))
data = frame(uc, 34); n0 = rd32(uc, S("g_st_now")); calls["hcount"] = 100; calls["hcount_asked"] = 0; calls["ranges"].clear()
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 0))                  # an immediate flip
check(v0 == 512 and regs and order[:2] == ["draw", "original"] and same(rd(uc, VRAM, 512 * 272 * 2), expect(data))
      and rd32(uc, S("g_st_now")) == n0 and calls["hcount_asked"] == 0 and flushed_rows()[0] == 1,
      "mode 2: an immediate flip is darkened first, from the top, and only then passed on")
calls["hcount"] = 280
order.clear(); data = frame(uc, 35); calls["in_intr"] = 1; calls["evf_set"].clear(); order.clear()
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))                  # a flip from an interrupt
calls["in_intr"] = 0
check(v0 == 513 and order[:2] == ["draw", "original"] and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)) and calls["evf_set"] == [],
      "mode 2: a flip from an interrupt is darkened in place, before it is passed on; our thread is not woken")
uc.hook_del(h1); uc.hook_del(h2)
wr32(uc, S("g_mode2"), 0)
data = frame(uc, 36); calls["in_intr"] = 1; calls["evf_set"].clear()
call(uc, TARGET, (VRAM, 512, 0, 1)); calls["in_intr"] = 0
check(rd(uc, VRAM, 512 * 272 * 2) == data and calls["evf_set"] == [(0x1234, 1)], "back in mode 1: a flip from an interrupt is handed to our thread again")
call(uc, S("draw_pending"))

# ---- the GE is still drawing when the game hands the frame over ------------------
wr32(uc, S("g_ge_wait"), 1)
data = frame(uc, 21)
calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [2, 2, 1, 0]
busy0 = rd32(uc, S("g_st_gebusy"))
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and regs and calls["ge"] == [1, 1, 1, 1] and calls["sleeps"] == 3 and same(rd(uc, VRAM, 512 * 272 * 2), expect(data))
      and rd32(uc, S("g_st_gebusy")) == busy0 + 1 and rd32(uc, S("g_st_gewait")) >= 750,
      "GE busy: asks (never the blocking form), sleeps in steps until it is done, then darkens; counted in the stats")
data = frame(uc, 22)
calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [3]
call(uc, TARGET, (VRAM, 512, 0, 1))
check(calls["sleeps"] == 0 and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)), "a list paused by the game is not waited for")
data = frame(uc, 23)
calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [2] * 500
give0 = rd32(uc, S("g_st_gegiveup"))
v0, _ = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and 40 <= calls["sleeps"] <= 60 and rd32(uc, S("g_st_gegiveup")) == give0 + 1 and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)),
      "a GE that never finishes: gives up after about 12 ms and carries on")
data = frame(uc, 24)
calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [2] * 500; calls["delay_result"] = 0x80020064
v0, _ = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and calls["sleeps"] == 1, "where sleeping is refused (interrupt context), it does not spin")
calls["delay_result"] = 0; calls["ge_script"].clear()
# what the label's GE figure stands on: an error is not "idle", and "not asked" is not 0%
data = frame(uc, 26); calls["ge"].clear(); calls["sleeps"] = 0; calls["ge_script"][:] = [0x80000021]
a0, e0 = rd32(uc, S("g_st_geasked")), rd32(uc, S("g_st_geerr"))
call(uc, TARGET, (VRAM, 512, 0, 1)); calls["ge_script"].clear()
check(rd32(uc, S("g_st_geasked")) == a0 and rd32(uc, S("g_st_geerr")) == e0 + 1 and calls["sleeps"] == 0 and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)),
      "an error from the GE status call is counted as an error, not as an idle GE; the frame is still darkened")
def window(script, intr=0):                     # 64 fresh frames, then the label's state and figure
    wr32(uc, S("g_gen"), rd32(uc, S("g_gen")) + 1)
    for i in range(64):
        frame(uc, 200 + i); calls["ge_script"][:] = list(script)
        if intr:
            calls["in_intr"] = 1; call(uc, TARGET, (VRAM, 512, 0, 1)); calls["in_intr"] = 0; call(uc, S("draw_pending"))
        else: call(uc, TARGET, (VRAM, 512, 0, 1))
    calls["ge_script"].clear()
    return rd32(uc, S("g_now_ge")), rd32(uc, S("g_now_busy"))
check(window([0]) == (0, 0) and window([2, 0]) == (0, 100) and window([0x80000021]) == (2, 0) and window([2] * 5, intr=1) == (1, 0),
      "label: a percentage when the GE was asked and answered (0% idle, 100% busy), ERR when the call fails, -- when it was not asked")
window([2, 2, 0])
check(500 <= rd32(uc, S("g_now_wait")) <= 800, "label: the wait is the time per frame spent waiting for the GE (two 250 us sleeps here)")
gs = rd32(uc, S("g_ge_sync")); wr32(uc, S("g_ge_sync"), 0)
check(window([0])[0] == 3, "label: N/A when the status call was not found")
wr32(uc, S("g_ge_sync"), gs)
wr32(uc, S("g_ge_wait"), 0)
calls["ge"].clear(); frame(uc, 25); call(uc, TARGET, (VRAM, 512, 0, 1))
check(calls["ge"] == [], "ge_wait = 0: the GE is not asked at all")
wr32(uc, S("g_ge_wait"), 1)

call(uc, S("hook_remove"))
check(rd32(uc, TARGET) == BODY[0] and rd32(uc, TARGET + 4) == BODY[1], "remove: the original words are back")
data = frame(uc, 11)
v0, _ = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and rd(uc, VRAM, 512 * 272 * 2) == data, "after remove, calls are untouched")

# ---- the system has no "in an interrupt?" call under either number -------------------
check("sceKernelIsIntrContext" not in SYM, "the interrupt check is looked up at run time, not asked for at load time")
uc2, calls2 = new_uc(intr_nid=0x12345678)
put_code(uc2, TARGET, BODY)
r, _ = call(uc2, S("hook_install"))
ready(uc2)
data = frame(uc2, 41)
v0, _ = call(uc2, TARGET, (VRAM, 512, 0, 1))
check(r == 0 and v0 == 513 and same(rd(uc2, VRAM, 512 * 272 * 2), expect(data)), "interrupt check not found: still installs and draws from a thread flip")
data = frame(uc2, 42)
calls2["intr_on"] = 0
call(uc2, TARGET, (VRAM, 512, 0, 1))
calls2["intr_on"] = 1
check(rd(uc2, VRAM, 512 * 272 * 2) == data and calls2["evf_set"] == [(0x1234, 1)], "  ... and a flip with interrupts off is still handed to our thread")

# ---- 4: another plugin's jump already at the start ---------------------------------
uc, calls = new_uc()
put_code(uc, TARGET, [J(OTHER), 0] + BODY[2:])
put_code(uc, OTHER, [0x24020000 | 77, 0x03E00008, 0])        # li v0,77 ; jr ra ; nop
r, _ = call(uc, S("hook_install"))
check(r == 0 and rd32(uc, TARGET) == J(S("hook_setframebuf")), "chained: installs in front of another plugin's jump")
ready(uc)
data = frame(uc, 3)
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 77 and regs and same(rd(uc, VRAM, 512 * 272 * 2), expect(data)), "chained: our pass runs, then the other plugin's hook runs")

# ---- 5: a branch first: syscall table instead --------------------------------------
uc, calls = new_uc()
put_code(uc, TARGET, [0x10800003, 0] + BODY[2:])             # beqz a0, +3 ; nop
r, _ = call(uc, S("hook_install"))
check(r == 1 and rd32(uc, TARGET) == 0x10800003 and calls["patch_syscall"] == [(TARGET, S("hook_setframebuf"))],
      "a branch at the start: code left alone, syscall table used instead")

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
