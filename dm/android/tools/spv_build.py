# A game's shaders translated for the Android Vulkan replay (dm/android/gfx_vk_draw.inc), from the package dm/gfx/tools/shader_split.py writes: what dmsh_build.py
# does for Metal, without the Metal steps. Each record of <package dir>/<stage>_<group>.bin ('BTCDBTCD', a header with +0x28 the control section's length and +0x2c
# the code area's, padded to 0x38, the code area, the control section) becomes a dmsh_xlate job as dmsh_build.py job_for makes it (the 0x30 header stripped, a
# compute area's instructions from +0x100, c1 from control +0x700..+0x704, the profile's translation facts), with softfloat64 (phones' GPUs have no FP64). The
# translator's output goes through spv_android.py: <out dir>/<key>.spv, <key>.z.spv (vertex) and <key>.res.json, which the app finds in game/spv (DM_SPV_DIR).
# The key is the host's (hle_nvn.c keys_hash): sha256 of the control section (its own length when +0x20 gives one under 0x880) and the code area, 16 hex digits.
# Vertex shaders in two parts (VertexA + VertexB, Catherine: Full Body's GLSLC) are merged into one program first, as dmsh_build.py vertex_ab does.
# Usage: spv_build.py <package dir> <out dir> --xlate <dmsh_xlate.dll or executable> [--facts <shader_facts.tsv>] [-j N]
import spv_android as sa   # beside this file
import argparse, concurrent.futures as cf, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GFX_MAGIC, CS_MAGIC, CTL_MAGIC = bytes.fromhex('78563412'), bytes.fromhex('79563412'), bytes.fromhex('34127698')

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

def split(rec):   # → (code area, control section)
    nctl, ncode = struct.unpack_from('<2I', rec, 0x28)
    return rec[0x38:0x38 + ncode], rec[0x38 + ncode:0x38 + ncode + nctl]

def key_of(ctl, code):
    own = struct.unpack_from('<I', ctl, 0x20)[0] if len(ctl) >= 0x24 else 0
    n = own if 0x760 <= own < 0x880 else 0x880
    return hashlib.sha256(ctl[:n] + code).hexdigest()[:16]

# dmsh_build.py tf_layout, the same (the capture is a .tf.spv variant here: spv_android.py tf_variant)
def tf_layout(key, ctl):
    """Transform feedback layout from a vertex shader's control section (what the NVN driver programs into the GPU's feedback state when the program is bound): +0x6dc u16 stride per buffer
    (4 buffers), +0x6e8 u8 per buffer the number of captured outputs, +0x6ec u8 buffer count and +0x6ed 1; per buffer b, +0xdc + 0x80 b lists the captured outputs, one byte each as 0x20 + n
    (generic output n), and +0x4dc + 0x80 b their component counts, one byte each, each captured from x on and packed in that order (stride = 4 x the components). Pokémon Legends: Z-A's
    28 skinning shaders capture one output per buffer (position xyz, packed normal / tangent bits); Donkey Kong Country: Tropical Freeze's fur simulation captures two in one buffer
    (`7dcdaed991c74bfe`: outputs 0 and 1, xyz each, stride 24; count 2, component bytes 03 03).
    → [[[[output, components], …], stride] per buffer, [] for an unused one], None for a shader without transform feedback. Any other form gets a red line and no capture entry point (the host
    then reports the draw, nvn_mtl.m tf_draw): it is not known how it maps, and one shader must not stop a game's build (Tears of the Kingdom and Ace Attorney have no feedback shader at all)"""
    strides = struct.unpack_from('<4H', ctl, 0x6dc)
    if not any(strides): return None
    out = []
    for b in range(4):
        if not strides[b]: out.append([]); continue
        k = ctl[0x6e8 + b]; locs = list(ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + k]); comps = list(ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + k])
        if (not 1 <= k <= 4 or any(ctl[0xdc + 0x80 * b + k:0x15c + 0x80 * b]) or any(ctl[0x4dc + 0x80 * b + k:0x4e0 + 0x80 * b])
                or not all(0x20 <= x < 0x40 for x in locs) or not all(1 <= n <= 4 for n in comps) or strides[b] != 4 * sum(comps)):
            print(f'[RED] {key}: transform feedback buffer {b} has a layout of unknown form (stride {strides[b]}, outputs {k}: {[hex(x) for x in locs]}, components {comps}): no capture entry point; see tf_layout', flush=True)
            return None
        out.append([[[x - 0x20, n] for x, n in zip(locs, comps)], strides[b]])
    return out


# dmsh_build.py vertex_ab, the same (that script runs when imported); keep the two alike
def vertex_ab(key, code, ctl):
    """A vertex shader in two parts, VertexA + VertexB, as one program (Catherine: Full Body's GLSLC, SDK 8.2.1, GPU code version 1.15, writes every vertex shader so; the other games' are one
    program of shader type 1). Part A sits at code +0, its SPH with shader type 0 (Maxwell's VERTEX_CULL_BEFORE_FETCH slot): it loads the position attribute and writes only the position.
    Part B starts at control +0x700 (its own 0x30-byte header, then an SPH of shader type 1) and writes the other outputs from the other attributes. The hardware runs A, then B; translated
    alone, A left every varying unwritten (a full-screen pass read its texture coordinate as 0 and the whole screen came out one texel's colour). Control +0x748 lists both parts as
    {SPH offset, length from the SPH, ?} for A, then for B. The merge is what Ryujinx's TranslatorContext.Translate(VertexA) builds: A's instructions, each unconditional EXIT a branch to
    B's first instruction, then B's, under B's SPH with A's flags and input / output maps ORed in and the larger local memory sizes. Measured on all 1,244 of the game's vertex shaders: A
    writes no generic output (so none of B's loads reads one of A's), A ends in two EXITs (one predicated on CC FCSM_TR, which Ryujinx never takes, left as it is) and a BRA to itself,
    and neither part has an absolute or table jump, so moving B keeps every branch. → (code area with its 0x30 header, c1), c1 being what follows part B; None for any other vertex shader"""
    if len(code) < 0x80 or len(ctl) < 0x760 or code[:4] != bytes.fromhex('78563412'): return None
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f != 1 or (w0 >> 10) & 0xf != 0: return None
    a_sph, a_len, _, b_sph, b_len, _ = struct.unpack_from('<6I', ctl, 0x748)
    if not any((a_sph, a_len, b_sph, b_len)): return None   # one program of shader type 0, no part B: translated as it is (Ryujinx reads type 0 as a vertex shader)
    bw0 =struct.unpack_from('<I', code, b_sph)[0] if 0x80 <= b_sph <= len(code) - 0x50 else 0
    if a_sph != 0x30 or b_sph != struct.unpack_from('<I', ctl, 0x700)[0] + 0x30 or bw0 & 0x1f != 1 or (bw0 >> 10) & 0xf != 1 or (a_len - 0x50) % 32 or (b_len - 0x50) % 32 or b_sph + b_len > len(code):
        raise ValueError(f'vertex shader of shader type 0 without a VertexB part in the expected place (control +0x748 {a_sph:#x} {a_len:#x} {b_sph:#x} {b_len:#x})')
    sa, sb = list(struct.unpack_from('<20I', code, a_sph)), list(struct.unpack_from('<20I', code, b_sph))
    sph = [sb[0] | sa[0] & ~(0xf << 10)] + [max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffff for i in (1, 2, 3)] + [sb[4]] + [sa[i] | sb[i] for i in range(5, 20)]
    a = bytearray(code[a_sph + 0x50:a_sph + a_len]); b = code[b_sph + 0x50:b_sph + b_len]
    for o in range(0, len(a), 8):
        if o % 32 == 0: continue   # the scheduling word of each group of three instructions
        ins = struct.unpack_from('<Q', a, o)[0]
        if ins >> 52 == 0xe30 and ins & 0x1f == 0xf:   # EXIT with CC T (any predicate) → BRA to part B, keeping the predicate and CC bits; the offset counts from the next instruction
            struct.pack_into('<Q', a, o, 0xe24 << 52 | ((len(a) - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff)
    return code[:0x30] + struct.pack('<20I', *sph) + bytes(a) + b, code[b_sph + b_len:]



def job(key, stage, code, ctl, w, facts):
    c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
    c1 = code[c1o:c1t] if c1o <= c1t <= len(code) else b''
    if stage == 'vs':
        try: ab = vertex_ab(key, code, ctl)
        except ValueError as e: return None, str(e)
        if ab: code, c1 = ab
    if stage != 'cs': body = code[0x30:]
    elif code[:4] == CS_MAGIC:
        if any(code[0x30:0x100]): return None, 'compute code area +0x30..+0x100 is not all zero'
        body = code[0x100:]
    else: body = code
    p = Path(w) / f'{key}.bin'; p.write_bytes(body)
    j = {'key': key, 'stage': stage, 'code': str(p), 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), 'softfloat64': True,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # dmsh_build.py's options and defaults: the chain is tested with them
    if key in facts: j.update(facts[key])
    else: j['no_facts'] = True
    if stage == 'cs' and 'local' not in j and len(ctl) >= 0x758:   # the thread group and shared memory from the control section, as shader_split.py writes the facts
        x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
        if x and y and z: j['local'] = [x, y, z]
        if sh and 'shared' not in j: j['shared'] = sh
    return j, None

def main():
    ap = argparse.ArgumentParser(description='translate a shader_split package for the Android Vulkan replay')
    ap.add_argument('package'); ap.add_argument('out'); ap.add_argument('--xlate', required=True); ap.add_argument('--facts'); ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    facts = {}
    if a.facts and os.path.exists(a.facts):
        for ln in open(a.facts, encoding='utf-8'):
            if ln.strip() and not ln.startswith('#'): k, v = ln.rstrip('\n').split('\t', 1); facts[k] = json.loads(v)
    w = tempfile.mkdtemp(prefix='spv_')
    jobs, skipped, have, tfs = {}, {}, 0, {}
    for f in sorted(Path(a.package).glob('*.bin')):
        stage = f.name[:2]
        if stage not in ('vs', 'ps', 'gs', 'tc', 'te', 'cs'): continue
        for rec in records(f.read_bytes()):
            code, ctl = split(rec)
            if ctl[:4] != CTL_MAGIC: skipped[f'{f.name}?'] = 'no control section magic'; continue
            key = key_of(ctl, code)
            if key in jobs: continue
            rp = out / f'{key}.res.json'   # translated by an earlier run of this chain version (the key covers the shader's bytes)
            if (out / f'{key}.spv').exists() and rp.exists() and json.loads(rp.read_text(encoding='utf-8')).get('android') == sa.ANDROID_CHAIN: have += 1; continue
            j, why = job(key, stage, code, ctl, w, facts)
            if not j: skipped[key] = why; continue
            jobs[key] = j
            if stage == 'vs' and (tf := tf_layout(key, ctl)): tfs[key] = tf
    keys = sorted(jobs); chunks = [keys[i::a.j] for i in range(a.j)]
    xl = [a.xlate] if not a.xlate.endswith('.dll') else ['dotnet', a.xlate]
    tout = Path(w) / 'out'; tout.mkdir()
    def run(ch):
        jp = Path(w) / f'job_{abs(hash(ch[0]))}.json'; jp.write_text(json.dumps([jobs[k] for k in ch]), encoding='utf-8')
        return subprocess.run(xl + [str(jp), str(tout)], capture_output=True, text=True, errors='replace')
    failed = {}
    with cf.ThreadPoolExecutor(a.j) as ex:
        for r in ex.map(run, [c for c in chunks if c]):
            for ln in r.stderr.splitlines() + r.stdout.splitlines():
                m = re.match(r'\[RED\] (\w+): (.*)', ln)
                if m: failed[m.group(1)] = m.group(2)[:160]
    # second pass for vertex shaders (dmsh_build.py's): inputs the shader only uses as integers are declared uint and the shader translated again (spv_android.py int_inputs)
    again = []
    for k in keys:
        sp = tout / f'{k}.spv'
        if jobs[k]['stage'] != 'vs' or not sp.exists(): continue
        extra = {l: t for l, t in sa.int_inputs(sa.parse(sp.read_bytes())[1]).items() if l not in jobs[k]['attr']}
        if extra: jobs[k]['attr'].update(extra); again.append(k)
    if again:
        with cf.ThreadPoolExecutor(a.j) as ex:
            for r in ex.map(run, [c for c in [again[i::a.j] for i in range(a.j)] if c]):
                for ln in r.stderr.splitlines() + r.stdout.splitlines():
                    m = re.match(r'\[RED\] (\w+): (.*)', ln)
                    if m: failed[m.group(1)] = 'integer-input retranslation: ' + m.group(2)[:130]
        print(f'spv_build: {len(again)} vertex shaders translated again with integer inputs')
    done = sorted(p.name[:-4] for p in tout.glob('*.spv') if not p.name.endswith('.reader.spv'))
    for k in done:   # the feedback layout goes with the shader's reflection: spv_android.py makes the capture variant from it
        if k in tfs: rp = tout / f'{k}.res.json'; r = json.loads(rp.read_text(encoding='utf-8')); r['tf'] = tfs[k]; rp.write_text(json.dumps(r), encoding='utf-8')
    if done: subprocess.run([sys.executable, str(HERE / 'spv_android.py'), str(tout), str(out)], check=True)   # every key in tout (this run's): no list on the command line, which Windows limits to 32 K characters
    for k in keys:
        if k not in done and k not in failed: failed[k] = 'no output from the translator'
    for k, why in sorted(skipped.items()): print(f'[RED] {k}: {why}')
    for k, why in sorted(failed.items()): print(f'[RED] {k}: {why}')
    print(f'spv_build: {len(done)} translated, {have} already there, {len(failed)} failed, {len(skipped)} skipped, {sum(1 for k in jobs if jobs[k].get("no_facts"))} without facts')
    shutil.rmtree(w, ignore_errors=True)

if __name__ == '__main__':
    main()
