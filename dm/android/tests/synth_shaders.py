# Synthetic Maxwell shaders for the Android NVN replay's draw path, without game files: a vertex shader that passes attribute 0 to the position and
# attribute 1 to generic output 0, and a fragment shader whose colour is red and green from that generic input (IPA) and blue and alpha from NVN
# uniform slot 0 (c[3], LDC). Encodings from Ryujinx's InstTable.cs (opcode bits) and InstDecoders.cs (fields), as dm/tools/tests/test_image_atomic_bounds.py
# builds its fixtures. Each shader is a 0x50-byte SPH header and the instructions, a scheduling word before every three.
# Each stage is also written as a game ships it, for the app's NVN demo (nvn_demo.c): <stage>.code, the code area (a 0x30 header with the graphics magic, then
# the SPH and instructions), and <stage>.ctl, a 0x880-byte control section (magic, +0x20 its length, +0x700 / +0x704 the c1 segment's start and the code
# area's length: no c1 here). The translation jobs are keyed as the host keys the program (hle_nvn.c keys_hash: sha256 of control + code area, 8 bytes).
# A compute shader writes a constant and its dispatch's group count to storage buffer 0. vstf is the vertex shader with a transform feedback layout (output 0's xyz), vsint reads its colour attribute as integers (no facts: tools/spv_build.py finds the integer input).
# csimg stores to an image, cstb / csms fetch from a buffer texture / a multisample texture, vsab is a two-part vertex shader; facts.tsv holds their texture and image
# types as the profile's answer table would, and pkg/ the shader_split.py package of all of them (for tools/spv_build.py).
# Usage: synth_shaders.py <out dir>   → vs/ps/cs .bin, jobs.json for dmsh_xlate, <stage>.code / <stage>.ctl, facts.tsv, pkg/
import hashlib, json, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TABLE = (ROOT / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
RZ, PT = 255, 7

def base(name):
    return int(re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', TABLE)[1].replace('x', '0'), 2)

def ald128(dst, attr): return base('Ald') | dst | RZ << 8 | PT << 16 | attr << 20 | RZ << 39 | 3 << 47                 # ALD.128 Rdst, a[attr]
def ast128(attr, src): return base('Ast') | src | RZ << 8 | PT << 16 | attr << 20 | RZ << 39 | 3 << 47                 # AST.128 a[attr], Rsrc
def ipa(dst, attr): return base('Ipa') | dst | RZ << 8 | PT << 16 | RZ << 20 | attr << 28 | RZ << 39 | PT << 47     # IPA.PASS Rdst, a[attr]
def ldc32(dst, slot, off): return base('Ldc') | dst | RZ << 8 | PT << 16 | off << 20 | slot << 36 | 4 << 48          # LDC.32 Rdst, c[slot][off]
def exit_(): return base('Exit') | PT << 16 | 15
def ldc64(dst, slot, off): return base('Ldc') | dst | RZ << 8 | PT << 16 | off << 20 | slot << 36 | 5 << 48          # LDC.64 Rdst, c[slot][off]
def mov32i(dst, imm): return base('Mov32i') | dst | 0xF << 12 | PT << 16 | (imm & 0xffffffff) << 20                 # MOV32I Rdst, imm
def s2r(dst, sreg): return base('S2r') | dst | PT << 16 | sreg << 20                                               # S2R Rdst, SR (37 / 38: CTAID.X / Y)
def sust_rgba(data, coord, tid, dim=3): return base('Sust') | data | coord << 8 | PT << 16 | 0xF << 20 | dim << 33 | tid << 36   # SUST.P.2D.RGBA [Rcoord], Rdata, handle word tid
def tld(dst, a, b, tid, dim, ms=0): return base('Tld') | dst | a << 8 | PT << 16 | b << 20 | dim << 28 | 0xF << 31 | tid << 36 | ms << 50   # TLD.LZ(.MS) Rdst, Ra, Rb
def i2f_u32(dst, src): return base('I2f') | dst | 2 << 8 | 2 << 10 | PT << 16 | src << 20                      # I2F.F32.U32 Rdst, Rsrc
def stg32(src, addr, off): return base('Stg') | src | addr << 8 | PT << 16 | off << 20 | 1 << 45 | 4 << 48          # STG.E.32 [Raddr + off], Rsrc

def pack(header, ins):
    words = []
    for i, x in enumerate(ins):
        if i % 3 == 0: words.append(0)
        words.append(x)
    return bytes(header) + struct.pack('<%dQ' % len(words), *words)

def vertex():
    h = bytearray(0x50); struct.pack_into('<I', h, 0, 1 | 3 << 5 | 1 << 10)   # SPH type 1 (VTG), version 3, vertex
    return pack(h, [ald128(0, 0x80), ast128(0x70, 0), ald128(4, 0x90), ast128(0x80, 4), exit_()])

def fragment():
    h = bytearray(0x50); struct.pack_into('<I', h, 0, 2 | 3 << 5 | 5 << 10)   # SPH type 2 (PS), version 3, fragment
    h[6 * 4] = 0xAA                                                            # generic input 0: x y z w perspective
    struct.pack_into('<I', h, 18 * 4, 0xF)                                     # render target 0: r g b a from R0..R3
    return pack(h, [ipa(0, 0x80), ipa(1, 0x84), ldc32(2, 3, 0x8), ldc32(3, 3, 0xC), exit_()])

def compute():
    # writes 0x600df00d and the dispatch's group count x (c[0][0x200]) to storage buffer 0, whose address the driver keeps at c[0][0x310]
    return pack(b'', [ldc64(0, 0, 0x310), mov32i(2, 0x600df00d), ldc32(3, 0, 0x200), stg32(2, 0, 0), stg32(3, 0, 4), exit_()])

def vertex_int():
    # vertex(), but attribute 1 is read as integers and converted (I2F), as skinning shaders read bone indices: the colour stream is RGBA8UI
    h = bytearray(0x50); struct.pack_into('<I', h, 0, 1 | 3 << 5 | 1 << 10)
    return pack(h, [ald128(0, 0x80), ast128(0x70, 0), ald128(4, 0x90), i2f_u32(4, 4), i2f_u32(5, 5), i2f_u32(6, 6), i2f_u32(7, 7), ast128(0x80, 4), exit_()])

def compute_image():
    # writes (1, 0.5, 0.25, 1) to image slot 0 (c2 0x120: handle word 72) at the workgroup's (x, y)
    return pack(b'', [s2r(0, 37), s2r(1, 38), mov32i(4, 0x3f800000), mov32i(5, 0x3f000000), mov32i(6, 0x3e800000), mov32i(7, 0x3f800000), sust_rgba(4, 0, 72), exit_()])

def compute_fetch_buffer():
    # texel 2 of the buffer texture in texture slot 0 (word 8), to storage buffer 0 +0
    return pack(b'', [mov32i(0, 2), tld(4, 0, RZ, 8, 0), ldc64(8, 0, 0x310), stg32(4, 8, 0), stg32(5, 8, 4), stg32(6, 8, 8), stg32(7, 8, 12), exit_()])

def compute_fetch_ms():
    # sample 3 of texel (1, 1) of the multisample texture in texture slot 1 (word 10), to storage buffer 0 +16 (one shader reading both kills the translator:
    # a KeyNotFoundException in its structured IR, the chain shared with the Mac is left as it is)
    return pack(b'', [mov32i(1, 1), mov32i(2, 1), mov32i(3, 3), tld(12, 1, 3, 10, 2, 1), ldc64(8, 0, 0x310), stg32(12, 8, 16), stg32(13, 8, 20), stg32(14, 8, 24), stg32(15, 8, 28), exit_()])

def vertex_ab():
    # a vertex shader in two parts (Catherine: Full Body's GLSLC; dmsh_build.py vertex_ab): part A (shader type 0) writes the position from attribute 0, part B
    # (its own 0x30 header and SPH of type 1, at control +0x700 + 0x30) generic output 0 from attribute 1; merged, the same as vertex()
    sa = bytearray(0x50); struct.pack_into('<I', sa, 0, 1 | 3 << 5 | 0 << 10)
    sb = bytearray(0x50); struct.pack_into('<I', sb, 0, 1 | 3 << 5 | 1 << 10)
    a = pack(sa, [ald128(0, 0x80), ast128(0x70, 0), exit_()]); b = pack(sb, [ald128(4, 0x90), ast128(0x80, 4), exit_()])
    code = struct.pack('<I', 0x12345678) + bytes(0x2c) + a
    b_at = len(code); code += struct.pack('<I', 0x12345678) + bytes(0x2c) + b
    ctl = bytearray(0x880); struct.pack_into('<I', ctl, 0, 0x98761234); struct.pack_into('<I', ctl, 0x20, 0x880)
    struct.pack_into('<2I', ctl, 0x700, b_at, len(code)); struct.pack_into('<6I', ctl, 0x748, 0x30, len(a), 0, b_at + 0x30, len(b), 0)
    return code, bytes(ctl)

def shipped(sph_code, cs=False, tf=False):
    if cs:   # compute: the 0x12345679 header, zeros up to +0x100, the instructions; control +0x748 the thread group (1, 1, 1)
        code = struct.pack('<I', 0x12345679) + bytes(0xfc) + sph_code
        ctl = bytearray(0x880); struct.pack_into('<I', ctl, 0, 0x98761234); struct.pack_into('<I', ctl, 0x20, 0x880)
        struct.pack_into('<2I', ctl, 0x700, len(code), len(code)); struct.pack_into('<3I', ctl, 0x748, 1, 1, 1)
        return code, bytes(ctl), hashlib.sha256(bytes(ctl) + code).hexdigest()[:16]
    code = struct.pack('<I', 0x12345678) + bytes(0x2c) + sph_code
    ctl = bytearray(0x880); struct.pack_into('<I', ctl, 0, 0x98761234); struct.pack_into('<I', ctl, 0x20, 0x880)
    struct.pack_into('<2I', ctl, 0x700, len(code), len(code))
    if tf:   # transform feedback (dmsh_build.py tf_layout): buffer 0 captures generic output 0's x y z, stride 12
        struct.pack_into('<H', ctl, 0x6dc, 12); ctl[0x6e8] = 1; ctl[0x6ec] = 1; ctl[0x6ed] = 1; ctl[0xdc] = 0x20; ctl[0x4dc] = 3
    return code, bytes(ctl), hashlib.sha256(bytes(ctl) + code).hexdigest()[:16]

if __name__ == '__main__':
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    jobs = []
    for st, body, attr, fout in (('vs', vertex(), {'0': 'float', '1': 'float'}, {}), ('ps', fragment(), {}, {'0': 'float'}), ('cs', compute(), {}, {}), ('vstf', vertex(), {'0': 'float', '1': 'float'}, {}), ('vsint', vertex_int(), {}, {})):
        (out / f'{st}.bin').write_bytes(body)
        code, ctl, key = shipped(body, st == 'cs', st == 'vstf')
        (out / f'{st}.code').write_bytes(code); (out / f'{st}.ctl').write_bytes(ctl)
        # softfloat64: phones' GPUs have no FP64 (dmsh_xlate lowers double operations to float, as for Metal)
        jobs.append(dict(key=key, stage=st[:2], code=str(out / f'{st}.bin'), tex={}, img={}, attr=attr, fout=fout, c1='', softfloat64=True))
        print(st, key)
    facts = {}   # what shader_split.py would write to the profile's shader_facts.tsv for these
    for st, body, f in (('csimg', compute_image(), {'img': {'0': {'type': '2d', 'fmt': 'R8G8B8A8Unorm'}}}), ('cstb', compute_fetch_buffer(), {'tex': {'0': 'buffer'}}), ('csms', compute_fetch_ms(), {'tex': {'1': '2dms'}})):
        code, ctl, key = shipped(body, True); (out / f'{st}.code').write_bytes(code); (out / f'{st}.ctl').write_bytes(ctl); facts[key] = f; print(st, key)
    code, ctl = vertex_ab(); key = hashlib.sha256(ctl + code).hexdigest()[:16]
    (out / 'vsab.code').write_bytes(code); (out / 'vsab.ctl').write_bytes(ctl); print('vsab', key)
    (out / 'facts.tsv').write_text(''.join(f'{k}' + chr(9) + json.dumps(v, sort_keys=True, separators=(',', ':')) + chr(10) for k, v in facts.items()))
    # the package shader_split.py would make: 'BTCDBTCD', the lengths at +0x28 / +0x2c, the code area, the control section
    (out / 'pkg').mkdir(exist_ok=True)
    for st in ('vs', 'ps', 'cs', 'vstf', 'csimg', 'cstb', 'csms', 'vsab', 'vsint'):
        code, ctl = (out / f'{st}.code').read_bytes(), (out / f'{st}.ctl').read_bytes()
        h = bytearray(0x38); h[:8] = b'BTCDBTCD'; struct.pack_into('<2I', h, 0x28, len(ctl), len(code))
        (out / 'pkg' / f'{st[:2]}_{st}.bin').write_bytes(bytes(h) + code + ctl)
    (out / 'jobs.json').write_text(json.dumps(jobs))
    print(out / 'jobs.json')
