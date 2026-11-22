#!/usr/bin/env python3
"""
Runs the compiled plugin (emblem.elf) in a MIPS emulator against a stand-in
sceDisplaySetFrameBuf, the way Scanline's tests do, and checks the display
hook: installing and removing it, the game's call still going through, the
card drawn on a frame exactly as the PC render draws it, the order for
immediate flips, and flips made from an interrupt handed to the plugin's
thread. Needs: pip install unicorn pyelftools; the PC render tool at
/tmp/render_tool (see tools/README.md).

  python3 tests/test_sim.py emblem.elf
"""
import struct, subprocess, sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE
from unicorn.mips_const import *

ELF = sys.argv[1] if len(sys.argv) > 1 else "emblem.elf"
FONT = sys.argv[2] if len(sys.argv) > 2 else "tools/msx.bin"
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
HALT = 0x88000000
TARGET = 0x88010000
IN_INTR = 0x88040000
TOAST = 0x09000000                               # a toast struct placed in RAM by the test

def load(uc):
    img = bytearray(IMG_END)
    for s in secs:
        if s["sh_type"] != "SHT_NOBITS": img[s["sh_addr"]: s["sh_addr"] + s["sh_size"]] = s.data()
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

def is_ctl(w):
    op, rs, fn = w >> 26, (w >> 21) & 31, w & 63
    return (op == 0 and fn in (8, 9)) or op in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) or (op in (17, 18) and rs == 8)

def allegrex_ops(uc):
    """Allegrex min/max (SPECIAL 0x2C/0x2D), which unicorn lacks; also in a branch delay slot."""
    text = elf.get_section_by_name(".text"); data, lo = text.data(), text["sh_addr"]
    sv = lambda u, r: struct.unpack("<i", struct.pack("<I", u.reg_read(UC_MIPS_REG_0 + r)))[0]
    def do(u, w):
        rs, rt, rd, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 63
        v = max(sv(u, rs), sv(u, rt)) if fn == 0x2C else min(sv(u, rs), sv(u, rt))
        if rd: u.reg_write(UC_MIPS_REG_0 + rd, v & 0xFFFFFFFF)
    for off in range(0, len(data), 4):
        w = struct.unpack_from("<I", data, off)[0]
        if not (w >> 26 == 0 and w & 0x7FF in (0x2C, 0x2D)): continue
        prev = struct.unpack_from("<I", data, off - 4)[0] if off else 0
        if not is_ctl(prev):
            def h(u, a, size, _, w=w): do(u, w); u.reg_write(UC_MIPS_REG_PC, a + 4)
            uc.hook_add(UC_HOOK_CODE, h, begin=BASE + lo + off, end=BASE + lo + off)
            continue
        op, rs, rt = prev >> 26, (prev >> 21) & 31, (prev >> 16) & 31
        simm = struct.unpack("<h", struct.pack("<H", prev & 0xFFFF))[0]
        assert op in (1, 4, 5, 6, 7, 20, 21, 22, 23)
        def hb(u, a, size, _, w=w, op=op, rs=rs, rt=rt, simm=simm):
            x, y = sv(u, rs), sv(u, rt)
            if op == 1: taken = (x >= 0) if rt & 1 else (x < 0)
            else: taken = (x == y, x != y, x <= 0, x > 0)[op & 3]
            likely = op >= 20 or (op == 1 and rt & 2)
            if taken or not likely: do(u, w)
            u.reg_write(UC_MIPS_REG_PC, (a + 4 + simm * 4) if taken else a + 8)
        uc.hook_add(UC_HOOK_CODE, hb, begin=BASE + lo + off - 4, end=BASE + lo + off - 4)

def rd(uc, a, n=4): return bytes(uc.mem_read(a & 0x1FFFFFFF, n))
def rd32(uc, a): return struct.unpack("<I", rd(uc, a))[0]
def wr32(uc, a, v): uc.mem_write(a & 0x1FFFFFFF, struct.pack("<I", v & 0xFFFFFFFF))
def J(a): return 0x08000000 | ((a >> 2) & 0x03FFFFFF)

_bufs = []
def new_uc():
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(0x08000000, 0x02000000)
    # VRAM, and its uncached mirror at 0x44000000 that the plugin draws through: one buffer, two addresses
    import ctypes
    buf = ctypes.create_string_buffer(0x00200000); _bufs.append(buf)      # kept alive for the emulator
    uc.mem_map_ptr(VRAM, 0x00200000, 7, ctypes.addressof(buf))
    uc.mem_map_ptr(0x44000000, 0x00200000, 7, ctypes.addressof(buf))
    load(uc); allegrex_ops(uc)
    calls = {"patch": [], "flush": 0, "evf_set": [], "intr_on": 1, "in_intr": 0, "clock": 50_000_000}
    def stub(name, fn): uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: fn(u), begin=S(name), end=S(name))
    def find(u):
        nid = u.reg_read(UC_MIPS_REG_A2)
        u.reg_write(UC_MIPS_REG_V0, {0x289D82FE: TARGET, 0xFE28C6D9: IN_INTR}.get(nid, 0))
    stub("sctrlHENFindFunction", find)
    stub("sctrlHENPatchSyscall", lambda u: calls["patch"].append((u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1))))
    def flush(u): calls["flush"] += 1
    stub("sctrlFlushCache", flush)
    def time(u): calls["clock"] += 20; u.reg_write(UC_MIPS_REG_V0, calls["clock"] & 0xFFFFFFFF)
    stub("sceKernelGetSystemTimeLow", time)
    stub("sceKernelCpuSuspendIntr", lambda u: u.reg_write(UC_MIPS_REG_V0, calls["intr_on"]))
    stub("sceKernelCreateEventFlag", lambda u: u.reg_write(UC_MIPS_REG_V0, 0x1234))
    stub("sceKernelCreateThread", lambda u: u.reg_write(UC_MIPS_REG_V0, 0x4321))
    stub("sceKernelStartThread", lambda u: u.reg_write(UC_MIPS_REG_V0, 0))
    def evset(u): calls["evf_set"].append((u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1))); u.reg_write(UC_MIPS_REG_V0, 0)
    stub("sceKernelSetEventFlag", evset)
    uc.mem_write(IN_INTR & 0x1FFFFFFF, struct.pack("<II", 0x03E00008, 0))
    uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.reg_write(UC_MIPS_REG_V0, calls["in_intr"]), begin=IN_INTR, end=IN_INTR)
    if "strlen" in SYM:                          # the compiler turns counting loops into this kernel import
        def strlen(u):
            a = u.reg_read(UC_MIPS_REG_A0) & 0x1FFFFFFF; n = 0
            while u.mem_read(a + n, 1)[0]: n += 1
            u.reg_write(UC_MIPS_REG_V0, n)
        stub("strlen", strlen)
    if "memset" in SYM:
        def memset(u):
            d, c, n = (u.reg_read(x) for x in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
            u.mem_write(d & 0x1FFFFFFF, bytes([c & 0xFF]) * n); u.reg_write(UC_MIPS_REG_V0, d)
        stub("memset", memset)
    return uc, calls

def call(uc, fn, args=()):
    for r, v in zip((UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3), args): uc.reg_write(r, v)
    sp = 0x89F00000
    uc.reg_write(UC_MIPS_REG_SP, sp); uc.reg_write(UC_MIPS_REG_RA, HALT)
    s = [UC_MIPS_REG_S0 + i for i in range(8)] + [UC_MIPS_REG_FP]
    for i, r in enumerate(s): uc.reg_write(r, 0x5000 + i)
    uc.emu_start(fn, HALT, count=50_000_000)
    ok = uc.reg_read(UC_MIPS_REG_SP) == sp and all(uc.reg_read(r) == 0x5000 + i for i, r in enumerate(s))
    return uc.reg_read(UC_MIPS_REG_V0), ok

BODY = [0x27BDFFF0, 0xAFBF000C, 0x00A61021, 0x00471021, 0x8FBF000C, 0x03E00008, 0x27BD0010]   # v0 = width + format + sync
def put_code(uc, addr, words): uc.mem_write(addr & 0x1FFFFFFF, b"".join(struct.pack("<I", w) for w in words))

def scene_bytes(seed=5):
    import random
    rnd = random.Random(seed)
    return bytes(rnd.getrandbits(8) for _ in range(480 * 272 * 3))

def to_vram(uc, rgb, fmt):
    """The same RGB picture into VRAM at stride 512, as the render tool converts it."""
    out = bytearray(512 * 272 * (4 if fmt == 3 else 2))
    for y in range(272):
        for x in range(480):
            r, g, b = rgb[(y * 480 + x) * 3: (y * 480 + x) * 3 + 3]
            if fmt == 3: struct.pack_into("<I", out, (y * 512 + x) * 4, r | g << 8 | b << 16 | 0xFF000000)
            else: struct.pack_into("<H", out, (y * 512 + x) * 2, (r >> 3) | (g >> 2) << 5 | (b >> 3) << 11)
    uc.mem_write(VRAM, bytes(out))
    return bytes(out)

def expect(rgb, fmt, title, foot, t_ms=2000):
    """What the PC render tool draws: the reference picture."""
    open("/tmp/sim_in.rgb", "wb").write(rgb)
    subprocess.check_call(["/tmp/render_tool", "/tmp/sim_in.rgb", "/tmp/sim_out.rgb", str(fmt), FONT, "card", str(t_ms), "Achievement unlocked", title, foot])
    return open("/tmp/sim_out.rgb", "rb").read()

def vram_rgb(uc, fmt):
    bpp = 4 if fmt == 3 else 2
    raw = rd(uc, VRAM, 512 * 272 * bpp); out = bytearray(480 * 272 * 3)
    for y in range(272):
        for x in range(480):
            o = (y * 480 + x) * 3
            if fmt == 3:
                v = struct.unpack_from("<I", raw, (y * 512 + x) * 4)[0]; out[o:o + 3] = bytes((v & 255, v >> 8 & 255, v >> 16 & 255))
            else:
                v = struct.unpack_from("<H", raw, (y * 512 + x) * 2)[0]
                out[o] = (v & 31) << 3 | (v & 31) >> 2; out[o + 1] = ((v >> 5) & 63) << 2 | ((v >> 5) & 63) >> 4; out[o + 2] = ((v >> 11) & 31) << 3 | ((v >> 11) & 31) >> 2
    return bytes(out)

def set_toast(uc, calls, title, foot, t_ms=2000):
    """Fills an AcToast { head[24], title[48], foot[40], int h } and points g_toast at it."""
    blob = b"Achievement unlocked".ljust(24, b"\0") + title.encode().ljust(48, b"\0") + foot.encode().ljust(40, b"\0")
    h = 32 + 11 * (2 if len(title) > 29 else 1) + 3
    uc.mem_write(TOAST, blob + struct.pack("<i", h))
    wr32(uc, S("g_toast"), TOAST)
    wr32(uc, S("g_toast_t0"), (calls["clock"] - t_ms * 1000) & 0xFFFFFFFF)

# ---- install ------------------------------------------------------------------
uc, calls = new_uc()
put_code(uc, TARGET, BODY)
r, _ = call(uc, S("hook_install"))
check(r == 0 and rd32(uc, TARGET) == J(S("hook_setframebuf")) and rd32(uc, TARGET + 4) == 0, "install: the display function now starts with a jump to the hook")
check([rd32(uc, S("g_tramp") + 4 * i) for i in range(4)] == [BODY[0], BODY[1], J(TARGET + 8), 0] and calls["flush"] >= 1, "install: trampoline holds the two moved words, caches flushed")

# ---- idle ---------------------------------------------------------------------------
rgb = scene_bytes(); before = to_vram(uc, rgb, 0)
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and regs and rd(uc, VRAM, len(before)) == before, "nothing to show: the frame is untouched, the game's call returns its own result")
wr32(uc, S("g_ready"), 1)
v0, _ = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and rd(uc, VRAM, len(before)) == before, "ready but no card: still untouched")

# ---- a card on a thread flip, both pixel formats ----------------------------------
for fmt in (0, 3):
    for title in ("Smart Water", "A Harder Sinister Marauder from Afar"):
        to_vram(uc, rgb, fmt)
        set_toast(uc, calls, title, "10 pts   1 of 95")
        v0, regs = call(uc, TARGET, (VRAM, 512, fmt, 1))
        check(v0 == 512 + fmt + 1 and regs and vram_rgb(uc, fmt) == expect(rgb, fmt, title, "10 pts   1 of 95"),
              "fmt %d, '%s': the frame shows exactly the PC render's card, the call goes through" % (fmt, title[:20]))
# sliding in: half way
to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 95", t_ms=125)
call(uc, TARGET, (VRAM, 512, 0, 1))
check(vram_rgb(uc, 0) == expect(rgb, 0, "Smart Water", "10 pts   1 of 95", t_ms=125), "sliding in: drawn part-way below the screen edge, as the render")
# still fully shown after 5.2 s, gone after 5.5 s
to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 95", t_ms=5200)
call(uc, TARGET, (VRAM, 512, 0, 1))
check(vram_rgb(uc, 0) == expect(rgb, 0, "Smart Water", "10 pts   1 of 95"), "after 5.2 s the card is still fully shown")
before = to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "x", t_ms=5600)
call(uc, TARGET, (VRAM, 512, 0, 1))
check(rd(uc, VRAM, len(before)) == before, "after 5.5 s the card is off the screen")

# ---- an immediate flip: the game's call goes first -----------------------------------
order = []
h1 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("original"), begin=TARGET + 8, end=TARGET + 8)
h2 = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: order.append("draw"), begin=S("dr_card"), end=S("dr_card"))
to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 95")
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 0))
check(v0 == 512 and regs and order == ["original", "draw"] and vram_rgb(uc, 0) == expect(rgb, 0, "Smart Water", "10 pts   1 of 95"),
      "immediate flip (sync 0): passed on first, card drawn straight after")
order.clear(); to_vram(uc, rgb, 0)
call(uc, TARGET, (VRAM, 512, 0, 1))
check(order == ["draw", "original"], "next-refresh flip (sync 1): card drawn first, then passed on")
uc.hook_del(h1); uc.hook_del(h2)

# ---- a flip from an interrupt (Felghana) -------------------------------------------
for why, key, val in (("in an interrupt handler", "in_intr", 1), ("with interrupts off", "intr_on", 0)):
    before = to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 95")
    calls[key] = val; calls["evf_set"].clear()
    v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
    calls["in_intr"] = 0; calls["intr_on"] = 1
    check(v0 == 513 and regs and rd(uc, VRAM, len(before)) == before and calls["evf_set"] == [(0x1234, 1)],
          "flip %s: nothing drawn there, the plugin's thread is woken, the call goes through" % why)
    call(uc, S("draw_pending"))
    check(vram_rgb(uc, 0) == expect(rgb, 0, "Smart Water", "10 pts   1 of 95"), "  ... and the thread then draws the card on that frame")
    after = rd(uc, VRAM, len(before)); to_vram(uc, rgb, 0)
    call(uc, S("draw_pending"))
    check(rd(uc, VRAM, len(before)) == before, "  ... once only")

# ---- the viewer open: the hook draws nothing -----------------------------------------------
before = to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 89")
wr32(uc, S("g_view_open"), 1)
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(v0 == 513 and regs and rd(uc, VRAM, len(before)) == before, "while the viewer is open the hook leaves the frame alone")
wr32(uc, S("g_view_open"), 0)

# ---- odd frames are refused ---------------------------------------------------------------
set_toast(uc, calls, "Smart Water", "x"); n0 = rd32(uc, S("g_st_reject"))
v0, _ = call(uc, TARGET, (0x00001000, 512, 0, 1))
check(v0 == 513 and rd32(uc, S("g_st_reject")) == n0 + 1, "a frame address outside VRAM/RAM is refused, the call still goes through")

# ---- remove -------------------------------------------------------------------------------
call(uc, S("hook_remove"))
check(rd32(uc, TARGET) == BODY[0] and rd32(uc, TARGET + 4) == BODY[1], "remove: the original words are back")

# ---- behind another plugin's jump (Scanline or MissyHUD loaded first) ------------------------
OTHER = 0x88020000
uc, calls = new_uc()
put_code(uc, TARGET, [J(OTHER), 0] + BODY[2:])
put_code(uc, OTHER, [0x24020000 | 77, 0x03E00008, 0])
r, _ = call(uc, S("hook_install"))
wr32(uc, S("g_ready"), 1); to_vram(uc, rgb, 0); set_toast(uc, calls, "Smart Water", "10 pts   1 of 95")
v0, regs = call(uc, TARGET, (VRAM, 512, 0, 1))
check(r == 0 and v0 == 77 and regs and vram_rgb(uc, 0) == expect(rgb, 0, "Smart Water", "10 pts   1 of 95"),
      "chained in front of another plugin's jump: card drawn, then the other plugin runs")

# ---- a branch first: syscall table ------------------------------------------------------------
uc, calls = new_uc()
put_code(uc, TARGET, [0x10800003, 0] + BODY[2:])
r, _ = call(uc, S("hook_install"))
check(r == 1 and rd32(uc, TARGET) == 0x10800003 and calls["patch"] == [(TARGET, S("hook_setframebuf"))], "a branch at the start: code left alone, syscall table used")

# ---- the rule engine as compiled for the PSP, on the real Felghana file ------------------------
# (the PC tests cover the rules; this checks the MIPS build reads the file and the game's memory the same way)
import os
GAMEFILE = sys.argv[3] if len(sys.argv) > 3 else "games/ULUS10558.txt"
if os.path.exists(GAMEFILE) and "g_em" in SYM:
    uc, calls = new_uc()
    text = open(GAMEFILE, "rb").read()
    TXT, OUT, G = 0x09100000, 0x09200000, S("g_em")
    uc.mem_write(TXT, text + b"\0")
    icount = [0]
    uc.hook_add(UC_HOOK_CODE, lambda u, a, sz, d: icount.__setitem__(0, icount[0] + 1))
    def em_step(ms):
        uc.reg_write(UC_MIPS_REG_T0, 259)                         # fifth argument (the PSP passes it in a register): room in out[]
        icount[0] = 0
        n, ok = call(uc, S("em_step"), (G, ms, 0, OUT))
        return [struct.unpack_from("<i", bytes(uc.mem_read(OUT, 4 * n)), 4 * i)[0] for i in range(n)], ok, icount[0]
    def g32(a, v): uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    n, ok = call(uc, S("em_load"), (G, TXT, len(text), 0))
    check(n == 89 and ok, "engine on MIPS: the Felghana file loads, 89 achievements")
    g32(0x08BB9B30, 8); g32(0x08BB9AFC, 0x42C80000); g32(0x08BB9B2C, 1585); g32(0x08BB9E38, 0xFFFFFFFF)   # as on the user's PSP
    for a in (0x08BB9B8E, 0x08BB9B8F, 0x08BB9B91, 0x08BB9B93) + tuple(range(0x08BB9B98, 0x08BB9BA4)): uc.mem_write(a, b"\x01")   # 16 characters met
    ms, seen, regs = 1, [], True
    for t in range(59):
        got, ok, _ = em_step(ms); ms += 16; seen += got; regs &= ok
    check(seen == [] and regs, "engine on MIPS: nothing in the first second")
    got, ok, _ = em_step(ms); ms += 16
    check(got == [47] and ok, "engine on MIPS: then 'Getting to Know You' (16 characters met), as on the PSP, and nothing else")
    g32(0x08BB9B30, 10); cost = []; seen = []
    for t in range(64):
        got, ok, c = em_step(ms); ms += 16; seen += got; cost.append(c); regs &= ok
    check(57 in seen and 58 not in seen and regs, "engine on MIPS: level 10 gives 'Adol the Shipwrecker'")
    g32(0x08BB9B2C, 1999999); em_step(ms); ms += 16
    g32(0x08BB9B2C, 1000000); g32(0x08BB9E38, 1); got, ok, _ = em_step(ms); ms += 16
    check(73 in got, "engine on MIPS: Smart Water on the tick the gold is paid")
    avg = sum(cost) // len(cost)
    print("  (one look at the game: %d MIPS instructions on average, %d at most)" % (avg, max(cost)))
    check(avg < 20000, "engine on MIPS: a look at the game stays under 20,000 instructions on average")

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
