#!/usr/bin/env python3
"""
End-to-end simulation of Sidequest's capture and restore paths.

The real compiled plugin (sidequest.elf) is relocated and loaded into an
emulated PSP memory map (MIPS32 CPU emulator, 64MB RAM at 0x08000000, the
4KB SRAM at 0xBFC00000). Then, with no changes to the plugin's code:

  1. "System A" (random + empty memory, ME save area, SRAM) is captured by
     calling the plugin's hook exactly as scePower would.
  2. RAM is overwritten with an unrelated "system B", with the plugin loaded
     at a DIFFERENT address, and B's hook is asked to restore A's snapshot.
  3. We check that the CPU ends up in A's kernel resume function with A's
     registers, and that lower RAM, the ME area and the SRAM are A's again.

Also checked: the idle hook writes nothing; a snapshot that does not fit is
refused without writing outside staging; a snapshot that changed in RAM is
refused and system B is left untouched.

What this cannot check: real hardware behaviour (drivers, the Media Engine,
the Memory Stick, the Allegrex cache instructions, which are no-ops here).
"""
import os, struct, sys, zlib
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.mips_const import *

ELF = sys.argv[1] if len(sys.argv) > 1 else "sidequest.elf"

LOWER, LOWER_SIZE = 0x08000000, 0x02000000
UPPER, ME, ME_SIZE = 0x0A000000, 0x0BC00000, 0x00400000
P8, P8_SIZE = 0x0BB00000, 0x00100000        # small in-use partition below the ME area (real PSP-3000 layout)
STAGE_END = P8
STAGE_CAP = STAGE_END - UPPER - 0x1000
RV, RV_SIZE = 0x1FC00000, 0x1000
MAGIC = 0x52535444
K0 = lambda p: p | 0x80000000

R = {n: globals()["UC_MIPS_REG_" + n] for n in
     ["A0", "V0", "RA", "SP", "PC", "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "FP", "GP", "K1"]}
SREGS = ["S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "FP"]

fails = 0
def check(cond, msg):
    global fails
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond: fails += 1

# ----------------------------------------------------------------- ELF loading
elf_f = open(ELF, "rb")
elf = ELFFile(elf_f)
SYM = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols() if s.name}
secs = [s for s in elf.iter_sections() if s["sh_flags"] & 2]          # SHF_ALLOC
IMG_END = max(s["sh_addr"] + s["sh_size"] for s in secs)
WR_LO = min(s["sh_addr"] for s in secs if s["sh_flags"] & 1)          # first writable byte
R_32, R_26, R_HI16, R_LO16 = 2, 4, 5, 6

def load_module(uc, phys_base):
    """Write the plugin at phys_base, relocated to run at KSEG0(phys_base)."""
    base = K0(phys_base)
    assert base & 0xFFFF == 0
    img = bytearray(IMG_END)
    for s in secs:
        if s["sh_type"] != "SHT_NOBITS":
            img[s["sh_addr"]: s["sh_addr"] + s["sh_size"]] = s.data()
    for rs in elf.iter_sections():
        if rs["sh_type"] != "SHT_REL": continue
        tgt = elf.get_section(rs["sh_info"])
        if not (tgt["sh_flags"] & 2) or tgt.name == ".sceStub.text": continue   # stub relocs are linker bookkeeping
        for r in rs.iter_relocations():
            off, t = r["r_offset"], r["r_info_type"]
            w = struct.unpack_from("<I", img, off)[0]
            if t == R_32:     w = (w + base) & 0xFFFFFFFF
            elif t == R_26:   w = (w & 0xFC000000) | (((w & 0x3FFFFFF) + (base >> 2)) & 0x3FFFFFF)
            elif t == R_HI16: w = (w & 0xFFFF0000) | (((w & 0xFFFF) + (base >> 16)) & 0xFFFF)
            elif t == R_LO16: pass
            else: raise SystemExit("unhandled relocation type %d" % t)
            struct.pack_into("<I", img, off, w)
    uc.mem_write(phys_base, bytes(img))
    return base

def sym(base, name): return base + SYM[name]
def rd32(uc, a): return struct.unpack("<I", uc.mem_read(a & 0x1FFFFFFF, 4))[0]
def wr32(uc, a, v): uc.mem_write(a & 0x1FFFFFFF, struct.pack("<I", v & 0xFFFFFFFF))

# ----------------------------------------------------------------- emulator
uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
uc.mem_map(LOWER, 0x04000000)
uc.mem_map(RV, 0x1000)
uc.mem_map(0x00010000, 0x10000)

def install_libc(base):
    """The kernel's memcpy is an import; emulate it. Other imports (cache
    flushes) are 'jr ra' stubs in the ELF, which is all they need to be here."""
    addr = sym(base, "memcpy")
    def h(u, a, size, _):
        d, s, n = (u.reg_read(x) for x in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
        u.mem_write(d & 0x1FFFFFFF, bytes(u.mem_read(s & 0x1FFFFFFF, n)))
        u.reg_write(UC_MIPS_REG_V0, d)          # memcpy returns dst, and the compiler relies on it
    maddr = sym(base, "memset")
    def hs(u, a, size, _):
        d, c, n = (u.reg_read(x) for x in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2))
        u.mem_write(d & 0x1FFFFFFF, bytes([c & 0xFF]) * n)
        u.reg_write(UC_MIPS_REG_V0, d)
    return (uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr), uc.hook_add(UC_HOOK_CODE, hs, begin=maddr, end=maddr))

HALT = 0x00010000
def orig_stub(ident):
    """Stand-in for the kernel resume function: note who was called, halt."""
    return struct.pack("<IIII", 0x3C190001, 0x03200008, 0x24030000 | ident, 0)   # lui t9,1 ; jr t9 ; li v1,id

def run_hook(base, stops, limit=2_000_000_000):
    """Call sq_hook_entry(1) like scePower does; run until a resume function is reached."""
    uc.reg_write(UC_MIPS_REG_V1, 0)
    uc.reg_write(R["A0"], 1)
    uc.emu_start(sym(base, "sq_hook_entry"), HALT, count=limit)
    return {1: ORIG_A, 2: ORIG_B_ADDR}.get(uc.reg_read(UC_MIPS_REG_V1))

def unpack_ref(stream):
    """Reference unpacker: returns {addr: bytearray} per SEEK region."""
    out, cur, i = {}, None, 0
    w = lambda k: struct.unpack_from("<I", stream, k * 4)[0]
    while True:
        rec = w(i); tag, n = rec >> 30, rec & 0x3FFFFFFF; i += 1
        if tag == 1:   cur = out.setdefault(w(i), bytearray()); i += 1
        elif tag == 2: cur += struct.pack("<I", w(i)) * n; i += 1
        elif tag == 0: cur += stream[i * 4:(i + n) * 4]; i += n
        else: break
    return out, i * 4

rnd = lambda n: os.urandom(n)

# =================================================================== SYSTEM A
A_PHYS = 0x08900000
A_lower = bytearray(rnd(10 << 20) + bytes(10 << 20) + struct.pack("<I", 0xDEADBEEF) * (1 << 20) + rnd(8 << 20))
assert len(A_lower) == LOWER_SIZE
A_me = rnd(1 << 20) + bytes(3 << 20)
A_p8 = rnd(P8_SIZE // 2) + bytes(P8_SIZE // 2)
A_rv, B_rv = rnd(RV_SIZE), rnd(RV_SIZE)
uc.mem_write(LOWER, bytes(A_lower)); uc.mem_write(ME, A_me); uc.mem_write(RV, A_rv); uc.mem_write(P8, A_p8)
uc.mem_write(UPPER, b"\xEE" * (STAGE_END - UPPER))
A = load_module(uc, A_PHYS)
libcA = install_libc(A)
ORIG_A, RET_A, SP_A = K0(0x08001000), K0(0x08002340), K0(0x08004FF0)
ORIG_B_ADDR = K0(0x08F01000)
uc.mem_write(0x08001000, orig_stub(1))

def setup(base, orig, cap=STAGE_CAP):
    wr32(uc, sym(base, "g_orig_resume"), orig)
    regs = [(K0(LOWER), LOWER_SIZE // 4), (K0(P8), P8_SIZE // 4), (K0(ME), ME_SIZE // 4)]
    for i, (a, w) in enumerate(regs):
        wr32(uc, sym(base, "g_regions") + 8 * i, a); wr32(uc, sym(base, "g_regions") + 8 * i + 4, w)
    wr32(uc, sym(base, "g_nregions"), len(regs))
    wr32(uc, sym(base, "g_me_addr"), K0(ME)); wr32(uc, sym(base, "g_me_size"), ME_SIZE)
    wr32(uc, sym(base, "g_stage_base"), K0(UPPER)); wr32(uc, sym(base, "g_stage_cap"), cap)

SA = {n: 0x1000 + i * 0x111 for i, n in enumerate(SREGS)}
def set_regs(vals, sp, ra):
    for n, v in vals.items(): uc.reg_write(R[n], v)
    uc.reg_write(R["SP"], sp); uc.reg_write(R["RA"], ra)

# ---- idle: must be a pure pass-through
setup(A, ORIG_A)
writes = []
hw = uc.hook_add(UC_HOOK_MEM_WRITE, lambda u, t, a, s, v, d: writes.append(a))
set_regs(SA, SP_A, RET_A)
stop = run_hook(A, [ORIG_A])
uc.hook_del(hw)
check(stop == ORIG_A and not writes and uc.reg_read(R["A0"]) == 1 and uc.reg_read(R["SP"]) == SP_A,
      "idle hook jumps straight to the kernel and writes nothing")

# ---- capture that cannot fit: refused, nothing written past the limit
SMALL = 1 << 20
setup(A, ORIG_A, cap=SMALL)
wr32(uc, sym(A, "g_phase"), 1)
set_regs(SA, SP_A, RET_A)
stop = run_hook(A, [ORIG_A])
guard = bytes(uc.mem_read(UPPER + SMALL, 1 << 16))
check(stop == ORIG_A and rd32(uc, sym(A, "g_cap_status")) == 0xFFFFFFFF and guard == b"\xEE" * (1 << 16),
      "snapshot that does not fit is refused; nothing written past the staging limit")
check(uc.reg_read(R["SP"]) == SP_A and uc.reg_read(R["RA"]) == RET_A, "refused capture still returns to the kernel cleanly")

# ---- real capture
uc.mem_write(UPPER, b"\xEE" * (STAGE_END - UPPER))
setup(A, ORIG_A)
wr32(uc, sym(A, "g_phase"), 1)
set_regs(SA, SP_A, RET_A)
before = bytes(uc.mem_read(LOWER, LOWER_SIZE))
stop = run_hook(A, [ORIG_A])
check(stop == ORIG_A and uc.reg_read(R["A0"]) == 1, "capture ends by calling the kernel resume function with a0=1")
check(uc.reg_read(R["SP"]) == SP_A and uc.reg_read(R["RA"]) == RET_A and
      all(uc.reg_read(R[n]) == v for n, v in SA.items()), "capture leaves sp, ra and s0-s7/fp untouched")
check(rd32(uc, sym(A, "g_cap_status")) == 1, "capture reports success")
slen = rd32(uc, sym(A, "g_cap_len"))
stream = bytes(uc.mem_read(UPPER, slen))
img, used = unpack_ref(stream)
check(used == slen and set(img) == {K0(LOWER), K0(P8), K0(ME)} and len(img[K0(LOWER)]) == LOWER_SIZE
      and len(img[K0(P8)]) == P8_SIZE and len(img[K0(ME)]) == ME_SIZE,
      "packed stream covers exactly lower RAM + busy partition + ME area (%.1f MB packed from 37 MB)" % (slen / 2**20))
check(bytes(uc.mem_read(UPPER + slen, STAGE_CAP - slen + 0x1000)) == b"\xEE" * (STAGE_CAP - slen + 0x1000),
      "capture wrote nothing in staging beyond the stream")
check(bytes(uc.mem_read(ME, ME_SIZE)) == A_me and bytes(uc.mem_read(RV, RV_SIZE)) == A_rv and bytes(uc.mem_read(P8, P8_SIZE)) == A_p8,
      "capture did not modify the ME area, the busy partition or the SRAM")
wlo, whi = A_PHYS - LOWER + WR_LO, A_PHYS - LOWER + IMG_END
snap = img[K0(LOWER)]
check(snap[:wlo] == before[:wlo] and snap[whi:] == before[whi:] and bytes(img[K0(ME)]) == A_me and bytes(img[K0(P8)]) == A_p8,
      "snapshot is byte-exact outside the plugin's own variables")
A_ctx, A_jump, A_flag = sym(A, "g_ctx"), sym(A, "sq_jumpctx"), sym(A, "g_restored")

# ---- round-trip test mode: pack, then unpack over the same system, in one hook call
wr32(uc, sym(A, "g_cfg") + 16, 3)                  # g_cfg.test = 3
wr32(uc, sym(A, "g_phase"), 1)
set_regs(SA, SP_A, RET_A)
rt_before = bytes(uc.mem_read(LOWER, LOWER_SIZE))
stop = run_hook(A, [ORIG_A])
rt_after = bytes(uc.mem_read(LOWER, LOWER_SIZE))
check(stop == ORIG_A and uc.reg_read(R["A0"]) == 1 and uc.reg_read(R["SP"]) == SP_A and uc.reg_read(R["RA"]) == RET_A
      and all(uc.reg_read(R[n]) == v for n, v in SA.items()), "round trip: returns to the kernel with all registers intact")
check(rd32(uc, A_flag) == MAGIC, "round trip: the system is told it was unpacked")
check(rt_after[:wlo] == rt_before[:wlo] and rt_after[whi:] == rt_before[whi:] and bytes(uc.mem_read(ME, ME_SIZE)) == A_me
      and bytes(uc.mem_read(P8, P8_SIZE)) == A_p8 and bytes(uc.mem_read(RV, RV_SIZE)) == A_rv,
      "round trip: memory, ME area and SRAM are unchanged")
wr32(uc, sym(A, "g_cfg") + 16, 0)
[uc.hook_del(x) for x in libcA]

# =================================================================== SYSTEM B
def make_system_b(phys, corrupt=False):
    uc.mem_write(LOWER, rnd(LOWER_SIZE)); uc.mem_write(ME, rnd(ME_SIZE)); uc.mem_write(RV, B_rv); uc.mem_write(P8, rnd(P8_SIZE))
    uc.mem_write(UPPER, stream + b"\xEE" * (STAGE_END - UPPER - slen))
    if corrupt:
        uc.mem_write(UPPER + slen // 2, bytes([stream[slen // 2] ^ 0x10]))
    b = load_module(uc, phys)
    orig_b = K0(0x08F01000)
    uc.mem_write(0x08F01000, orig_stub(2))
    setup(b, orig_b)
    for n, v in (("g_rp_len", slen), ("g_rp_crc", zlib.crc32(stream)), ("g_rp_ctx", A_ctx),
                 ("g_rp_jumpctx", A_jump), ("g_rp_flag", A_flag), ("g_phase", 3)):
        wr32(uc, sym(b, n), v)
    set_regs({n: 0xBAD00000 + i for i, n in enumerate(SREGS)}, K0(0x08F04FF0), K0(0x08F02340))
    return b, orig_b

# ---- staged data changed after it was verified: refuse, B untouched
B_PHYS = 0x08A40000
B, ORIG_B = make_system_b(B_PHYS, corrupt=True)
lb = install_libc(B)
b_before = bytes(uc.mem_read(LOWER, LOWER_SIZE))
stop = run_hook(B, [ORIG_A, ORIG_B])
b_after = bytes(uc.mem_read(LOWER, LOWER_SIZE))
blo, bhi = B_PHYS - LOWER + WR_LO, B_PHYS - LOWER + IMG_END
check(stop == ORIG_B and rd32(uc, sym(B, "g_rp_result")) == (-6 & 0xFFFFFFFF),
      "restore refuses when the staged data no longer matches its checksum")
check(b_after[:blo] == b_before[:blo] and b_after[bhi:] == b_before[bhi:], "refused restore leaves the running system untouched")
[uc.hook_del(x) for x in lb]

# ---- real restore, with the plugin at a different address than in A
B, ORIG_B = make_system_b(B_PHYS)
lb = install_libc(B)
la = install_libc(A)                       # A's memcpy import, used once A is back
stop = run_hook(B, [ORIG_A, ORIG_B])
check(stop == ORIG_A, "restore ends inside system A's kernel resume function")
check(uc.reg_read(R["A0"]) == 1 and uc.reg_read(R["SP"]) == SP_A and uc.reg_read(R["RA"]) == RET_A,
      "restored system gets a0=1 and its own sp/ra back")
check(all(uc.reg_read(R[n]) == v for n, v in SA.items()), "restored system gets its own s0-s7/fp back")
low = bytes(uc.mem_read(LOWER, LOWER_SIZE))
check(low[:wlo] == bytes(snap[:wlo]) and low[whi:] == bytes(snap[whi:]),
      "lower RAM is system A again, byte for byte (outside the plugin's own variables)")
check(rd32(uc, A_flag) == MAGIC,
      "restored system is told it was restored")
check(bytes(uc.mem_read(ME, ME_SIZE)) == A_me, "ME save area is system A's again")
check(bytes(uc.mem_read(P8, P8_SIZE)) == A_p8, "busy extra-RAM partition is system A's again")
rv = bytes(uc.mem_read(RV, RV_SIZE))
check(rv[:0x200] == A_rv[:0x200] and rv[0x220:] == A_rv[0x220:] and rv[0x200:0x220] == B_rv[0x200:0x220],
      "SRAM is system A's again, except the 32 bytes owned by the wake-up code")
check(bytes(uc.mem_read(UPPER, slen)) == stream, "staged data is not modified by the restore")

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
