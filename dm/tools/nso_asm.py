# AArch64 instruction encoders of the NSO rewriter (dm/tools/patch_nso.py) and of the per-build patch files (dm/tools/sdk_patches.py, dm/games/<id>/patches.py),
# which assemble their stubs with them. Each returns the instruction word; a branch asserts its reach
import os
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
# The base of the thread slots. Apple: mrs xt, tpidrro_el0 (the pthread TSD). Android (DM_TARGET=android): mov xt, x18, the slot block the host points x18 at
# (dm.h dm_slot_get; Linux preserves x18, and every guest use of x18 is rewritten to slot K18, so the guest never changes it)
TARGET = os.environ.get('DM_TARGET', 'apple')
assert TARGET in ('apple', 'android'), f'DM_TARGET must be apple or android, not {TARGET}'
def MRS_TPIDRRO(t): return (0xAA1203E0 | t) if TARGET == 'android' else (0xD53BD060 | t)
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def CMPR(n, m): return 0xEB00001F | (m << 16) | (n << 5)   # cmp xn, xm (subs xzr)
def BEQ(frm, to):
    off = to - frm; assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return 0x54000000 | (((off >> 2) & 0x7FFFF) << 5)
def BRK(imm): return 0xD4200000 | ((imm & 0xFFFF) << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F
