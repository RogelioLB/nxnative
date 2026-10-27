# SPIR-V from dmsh_xlate (dm/gfx/tools/dmsh_xlate: Ryujinx's translator, Vulkan SPIR-V, every resource in set 0) made ready for the Android Vulkan replay
# (dm/android/gfx_vk.c), as dmsh_build.py post-processes the MSL for Metal; the translator itself is shared and left as it is:
#   the descriptor set becomes the stage's own (vertex 0, fragment 1, geometry 2, tessellation 3, compute 0): stages number their bindings from the same start,
#     so they cannot share one set;
#   storage images without a format get the capabilities Vulkan wants for them (StorageImageWrite / ReadWithoutFormat);
#   fragment shader interlock (FragmentShaderPixelInterlockEXT, its execution modes and the begin / end instructions) is removed: mobile GPUs lack it, and
#     the translator declares it on hosts that have it (the order it gives overlapping fragments' image accesses is lost; such a shader is reported);
#   for a vertex shader with transform feedback (res.json "tf", spv_build.py from the control section), also <key>.tf.spv: before every return, the captured
#     outputs stored as their bits into storage buffers (set 0, binding 100 + feedback buffer), vertex v at v × stride (nvn_mtl.m's main0_tf; the Adreno has no
#     VK_EXT_transform_feedback): the host draws it as points with rasterization off, the vertex buffers shifted so v counts from the draw's first vertex;
#   for a vertex shader, also <key>.z.spv: before every return of the entry point, position z becomes (z + w) / 2, the -1..1 depth of NVN's default depth mode
#     mapped to Vulkan's 0..1 (nvn_mtl.m adds the same as its main0_z entry point; Vulkan without VK_EXT_depth_clip_control has no other way).
# Usage: spv_android.py <dmsh_xlate output dir> <out dir> [key ...]   (all keys of the input dir by default); prints one line per shader and a summary
import json, struct, sys
from pathlib import Path

ANDROID_CHAIN = 3   # res.json 'android': bumped when this post-pass or spv_build.py's translation changes what they make (spv_build.py translates older outputs again)
SET_OF = {'vs': 0, 'ps': 1, 'gs': 2, 'tc': 3, 'te': 3, 'cs': 0}
OP_CAP, OP_EXT, OP_EXECMODE, OP_ENTRY, OP_DECORATE, OP_MEMBERDECORATE = 17, 10, 16, 15, 71, 72
OP_FUNCTION, OP_FUNCTIONEND, OP_RETURN, OP_VARIABLE, OP_TYPEPOINTER, OP_TYPEINT, OP_TYPEFLOAT, OP_TYPEVECTOR, OP_CONSTANT = 54, 56, 253, 59, 32, 21, 22, 23, 43
OP_ACCESSCHAIN, OP_LOAD, OP_STORE, OP_COMPOSITEEXTRACT, OP_COMPOSITEINSERT, OP_FADD, OP_FMUL, OP_LABEL = 65, 61, 62, 81, 82, 129, 133, 248
INTERLOCK_CAPS = {5363, 5372, 5378}             # FragmentShaderSampleInterlockEXT, ShadingRateInterlockEXT, FragmentShaderPixelInterlockEXT
INTERLOCK_MODES = {5366, 5367, 5368, 5369, 5370, 5371}
INTERLOCK_OPS = {5364, 5365}
OP_TYPEIMAGE, OP_IMAGEREAD, OP_IMAGEWRITE, CAP_READ_NOFMT, CAP_WRITE_NOFMT = 25, 98, 99, 55, 56                    # OpBeginInvocationInterlockEXT, OpEndInvocationInterlockEXT
STORAGE_OUTPUT, DEC_BUILTIN, DEC_DESCRIPTORSET, BUILTIN_POSITION = 3, 11, 34, 0

def parse(data):
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    assert w[0] == 0x07230203, 'not SPIR-V'
    hdr, ins, i = w[:5], [], 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        assert n and i + n <= len(w), 'bad instruction length'
        ins.append([op] + w[i + 1:i + n]); i += n
    return hdr, ins

def emit(hdr, ins):
    out = list(hdr)
    for x in ins: out.append((len(x) << 16) | x[0]); out.extend(x[1:])
    return struct.pack('<%dI' % len(out), *out)

def ext_name(x):   # OpExtension's literal string
    return b''.join(struct.pack('<I', v) for v in x[1:]).split(b'\0')[0].decode()

def process(data, stage):
    hdr, ins = parse(data)
    s = SET_OF.get(stage, 0); interlock = False
    out = []
    for x in ins:
        op = x[0]
        if op == OP_DECORATE and x[2] == DEC_DESCRIPTORSET: x = x[:3] + [s]
        if op == OP_CAP and x[1] in INTERLOCK_CAPS: interlock = True; continue
        if op == OP_EXT and ext_name(x) == 'SPV_EXT_fragment_shader_interlock': continue
        if op == OP_EXECMODE and x[2] in INTERLOCK_MODES: continue
        if op in INTERLOCK_OPS: continue
        out.append(x)
    # storage images declared without a format (the translator declares them so for Metal, which reads and writes any format): Vulkan wants
    # StorageImageWriteWithoutFormat / ReadWithoutFormat for their stores / loads
    if any(x[0] == OP_TYPEIMAGE and x[7] == 2 and x[8] == 0 for x in out):
        have = {x[1] for x in out if x[0] == OP_CAP}
        need = [c for c, o in ((CAP_WRITE_NOFMT, OP_IMAGEWRITE), (CAP_READ_NOFMT, OP_IMAGEREAD)) if c not in have and any(x[0] == o for x in out)]
        at = next(i for i, x in enumerate(out) if x[0] == OP_CAP)
        out[at + 1:at + 1] = [[OP_CAP, c] for c in need]
    hdr, out = dyn_components(hdr, out)
    return hdr, out, interlock

# ---- Dynamic vector components ----
OP_TYPEBOOL, OP_TYPESTRUCT_, OP_TYPEARRAY, OP_TYPERUNTIMEARRAY_, OP_SELECT, OP_IEQUAL = 20, 30, 28, 29, 169, 170
def dyn_components(hdr, ins):
    """A load through an access chain whose last index picks a vector's component with a value (not a constant) — the translator's read of c[n] word by word,
    `cb[0][w >> 2][w & 3]`, as Nintendo's layout library reads its texture coordinates, colours and positions from the uniform buffer — becomes a load of the whole
    vector and a choice among its four components (OpCompositeExtract with constants, OpSelect on the index). The Adreno 619's driver (Vulkan 1.1.128) reads such a
    component wrong: Super Mario Bros. Wonder's title and menus came out as vertical stripes (each quad's v the same as its u, the texture read along its diagonal).
    Only pointers that are only loaded are changed"""
    bound = hdr[3]
    def new():
        nonlocal bound; bound += 1; return bound - 1
    ty = {x[1]: x for x in ins if x[0] in (OP_TYPEINT, OP_TYPEFLOAT, OP_TYPEVECTOR, OP_TYPEBOOL, OP_TYPESTRUCT_, OP_TYPEARRAY, OP_TYPERUNTIMEARRAY_)}
    ptr = {x[1]: (x[2], x[3]) for x in ins if x[0] == OP_TYPEPOINTER}
    const = {x[2]: x[3] for x in ins if x[0] == OP_CONSTANT}
    rtype = {}   # result id → its type, for the instructions with both
    var_ty = {x[2]: x[1] for x in ins if x[0] == OP_VARIABLE}
    for x in ins:
        if x[0] in (OP_ACCESSCHAIN, OP_LOAD, OP_CONSTANT, OP_BITCAST, OP_IADD, OP_IMUL, OP_COMPOSITEEXTRACT, OP_VARIABLE) or (x[0] >= 124 and x[0] <= 200 and len(x) > 2):
            if len(x) > 2: rtype[x[2]] = x[1]
    loads = {}; others = set()
    for x in ins:
        if x[0] == OP_LOAD: loads.setdefault(x[3], []).append(x)
        elif x[0] == OP_STORE: others.update(x[1:])
        elif x[0] not in (OP_DECORATE, OP_MEMBERDECORATE, 5, 6): others.update(x[3:])   # operands after the result type and id (OpName / OpMemberName: debug names only)
    chains = {}   # chain id → (vector pointer type or None, vector type, component type, the chain without its last index, index id)
    want_ptr = {}
    for x in ins:
        if x[0] != OP_ACCESSCHAIN or len(x) < 6 or x[-1] in const: continue
        if x[2] in others or x[2] not in loads: continue   # used other than by OpLoad
        base = x[3]; bt = var_ty.get(base) or rtype.get(base)
        if bt not in ptr: continue
        storage, t = ptr[bt]; ok = True
        for idx in x[4:-1]:
            d = ty.get(t)
            if d is None: ok = False; break
            if d[0] == OP_TYPESTRUCT_:
                if idx not in const: ok = False; break
                t = d[2 + const[idx]]
            elif d[0] in (OP_TYPEARRAY, OP_TYPERUNTIMEARRAY_): t = d[2]
            else: ok = False; break
        d = ty.get(t)
        if not ok or d is None or d[0] != OP_TYPEVECTOR or d[3] != 4: continue
        it = rtype.get(x[-1])
        if it not in ty or ty[it][0] != OP_TYPEINT: continue
        chains[x[2]] = (storage, t, d[2], x[:-1], x[-1], it)
    if not chains: return hdr, ins
    glob = []
    pv = {}
    for s, t, *_ in chains.values():
        if (s, t) in pv: continue
        p = next((k for k, v in ptr.items() if v == (s, t)), None)
        if p is None: p = new(); glob.append([OP_TYPEPOINTER, p, s, t])
        pv[(s, t)] = p
    tb = next((x[1] for x in ins if x[0] == OP_TYPEBOOL), None)
    if tb is None: tb = new(); glob.insert(0, [OP_TYPEBOOL, tb])
    cn = {}
    def cint(it, n):
        if (it, n) not in cn: c = new(); cn[(it, n)] = c; glob.append([OP_CONSTANT, it, c, n])
        return cn[(it, n)]
    for c in chains.values():
        for n in (1, 2, 3): cint(c[5], n)
    out, done_glob = [], False
    for x in ins:
        if x[0] == OP_FUNCTION and not done_glob: out.extend(glob); done_glob = True
        if x[0] == OP_ACCESSCHAIN and x[2] in chains:
            s, t, ct, pre, idx, it = chains[x[2]]
            out.append([OP_ACCESSCHAIN, pv[(s, t)]] + pre[2:]); continue   # the same result id, now a pointer to the vector
        if x[0] == OP_LOAD and x[3] in chains:
            s, t, ct, pre, idx, it = chains[x[3]]
            v = new(); e = [new() for _ in range(4)]; out.append([OP_LOAD, t, v, x[3]] + x[4:])
            out += [[OP_COMPOSITEEXTRACT, ct, e[i], v, i] for i in range(4)]
            cur = e[0]
            for i in (1, 2, 3):
                b = new(); r = x[2] if i == 3 else new()
                out += [[OP_IEQUAL, tb, b, idx, cint(it, i)], [OP_SELECT, ct, r, b, e[i], cur]]; cur = r
            continue
        out.append(x)
    hdr = list(hdr); hdr[3] = bound
    return hdr, out

def z_variant(hdr, ins):
    """Before each OpReturn of the entry point: position.z = (position.z + position.w) * 0.5"""
    bound = hdr[3]
    def new():
        nonlocal bound; bound += 1; return bound - 1
    entry = next(x[2] for x in ins if x[0] == OP_ENTRY)
    member_pos = {x[1]: x[2] for x in ins if x[0] == OP_MEMBERDECORATE and x[3] == DEC_BUILTIN and x[4] == BUILTIN_POSITION}   # struct id → member index
    direct_pos = {x[1] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_BUILTIN and x[3] == BUILTIN_POSITION}
    ptr_pointee = {x[1]: (x[2], x[3]) for x in ins if x[0] == OP_TYPEPOINTER}   # pointer type → (storage, pointee)
    var = idx = None
    for x in ins:
        if x[0] == OP_VARIABLE and x[3] == STORAGE_OUTPUT:
            st, pointee = ptr_pointee[x[1]]
            if x[2] in direct_pos: var, idx = x[2], None
            elif pointee in member_pos: var, idx = x[2], member_pos[pointee]
    if var is None: return None
    tf = next((x[1] for x in ins if x[0] == OP_TYPEFLOAT and x[2] == 32), None)
    tv4 = next((x[1] for x in ins if x[0] == OP_TYPEVECTOR and x[2] == tf and x[3] == 4), None)
    ti = next((x[1] for x in ins if x[0] == OP_TYPEINT and x[2] == 32 and x[3] == 1), None)
    if tf is None or tv4 is None: return None
    glob = []   # declarations added before the first function
    pv4 = next((x[1] for x in ins if x[0] == OP_TYPEPOINTER and x[2] == STORAGE_OUTPUT and x[3] == tv4), None)
    if pv4 is None: pv4 = new(); glob.append([OP_TYPEPOINTER, pv4, STORAGE_OUTPUT, tv4])
    half = new(); glob.append([OP_CONSTANT, tf, half, struct.unpack('<I', struct.pack('<f', 0.5))[0]])
    if idx is not None:
        if ti is None: ti = new(); glob.insert(0, [OP_TYPEINT, ti, 32, 1])
        cidx = new(); glob.append([OP_CONSTANT, ti, cidx, idx])
    out, in_entry, done_glob = [], False, False
    for x in ins:
        if x[0] == OP_FUNCTION and not done_glob: out.extend(glob); done_glob = True
        if x[0] == OP_FUNCTION: in_entry = x[2] == entry
        if x[0] == OP_FUNCTIONEND: in_entry = False
        if x[0] == OP_RETURN and in_entry:
            ptr = var
            if idx is not None: ptr = new(); out.append([OP_ACCESSCHAIN, pv4, ptr, var, cidx])
            p, z, w, sm, h, q = (new() for _ in range(6))
            out += [[OP_LOAD, tv4, p, ptr], [OP_COMPOSITEEXTRACT, tf, z, p, 2], [OP_COMPOSITEEXTRACT, tf, w, p, 3], [OP_FADD, tf, sm, z, w],
                    [OP_FMUL, tf, h, sm, half], [OP_COMPOSITEINSERT, tv4, q, h, p, 2], [OP_STORE, ptr, q]]
        out.append(x)
    hdr = list(hdr); hdr[3] = bound
    return emit(hdr, out)

OP_TYPEINT_, OP_TYPESTRUCT, OP_TYPERUNTIMEARRAY, OP_BITCAST, OP_IMUL, OP_IADD = 21, 30, 29, 124, 132, 128
STORAGE_INPUT, STORAGE_SB, DEC_LOCATION, DEC_BLOCK, DEC_ARRAYSTRIDE, DEC_OFFSET, DEC_BINDING, BUILTIN_VERTEXINDEX = 1, 12, 30, 2, 6, 35, 33, 42
def tf_variant(hdr, ins, tf):
    """Before each OpReturn of the entry point: buffer b's captured outputs (generic output n, its first c components) stored as uint bits at vertex × stride / 4 + their place"""
    bound = hdr[3]
    def new():
        nonlocal bound; bound += 1; return bound - 1
    entry_ins = next(x for x in ins if x[0] == OP_ENTRY); entry = entry_ins[2]
    loc = {x[1]: x[3] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_LOCATION}
    builtin = {x[1]: x[3] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_BUILTIN}
    ptr = {x[1]: (x[2], x[3]) for x in ins if x[0] == OP_TYPEPOINTER}
    outs, vidx = {}, None
    for x in ins:
        if x[0] != OP_VARIABLE: continue
        if x[3] == STORAGE_OUTPUT and x[2] in loc: outs[loc[x[2]]] = (x[2], ptr[x[1]][1])
        if x[3] == STORAGE_INPUT and builtin.get(x[2]) == BUILTIN_VERTEXINDEX: vidx = (x[2], ptr[x[1]][1])
    tf_ = next((x[1] for x in ins if x[0] == OP_TYPEFLOAT and x[2] == 32), None)
    tv4 = next((x[1] for x in ins if x[0] == OP_TYPEVECTOR and x[2] == tf_ and x[3] == 4), None)
    ti = next((x[1] for x in ins if x[0] == OP_TYPEINT_ and x[2] == 32 and x[3] == 1), None)
    tu = next((x[1] for x in ins if x[0] == OP_TYPEINT_ and x[2] == 32 and x[3] == 0), None)
    glob, deco, iface = [], [], []
    if ti is None: ti = new(); glob.append([OP_TYPEINT_, ti, 32, 1])
    if tu is None: tu = new(); glob.append([OP_TYPEINT_, tu, 32, 0])
    if vidx is None:   # gl_VertexIndex, declared and listed in the entry point's interface
        pin = new(); v = new(); glob += [[OP_TYPEPOINTER, pin, STORAGE_INPUT, ti], [OP_VARIABLE, pin, v, STORAGE_INPUT]]
        deco.append([OP_DECORATE, v, DEC_BUILTIN, BUILTIN_VERTEXINDEX]); iface.append(v); vidx = (v, ti)
    consts = {}
    def cu(n):
        if n not in consts: c = new(); consts[n] = c; glob.append([OP_CONSTANT, tu, c, n])
        return consts[n]
    ci0 = new(); glob.append([OP_CONSTANT, ti, ci0, 0])
    rta, st, pst, pu = new(), new(), new(), new()
    glob += [[OP_TYPERUNTIMEARRAY, rta, tu], [OP_TYPESTRUCT, st, rta], [OP_TYPEPOINTER, pst, STORAGE_SB, st], [OP_TYPEPOINTER, pu, STORAGE_SB, tu]]
    deco += [[OP_DECORATE, rta, DEC_ARRAYSTRIDE, 4], [OP_DECORATE, st, DEC_BLOCK], [OP_MEMBERDECORATE, st, 0, DEC_OFFSET, 0]]
    bufs = {}
    for b, e in enumerate(tf):
        if not e: continue
        v = new(); glob.append([OP_VARIABLE, pst, v, STORAGE_SB]); deco += [[OP_DECORATE, v, DEC_DESCRIPTORSET, 0], [OP_DECORATE, v, DEC_BINDING, 100 + b]]; bufs[b] = v
    cu(0)
    for b, e in enumerate(tf):   # make the constants before the code uses them
        if not e: continue
        cu(e[1] // 4); at = 0
        for o, n in e[0]:
            for i in range(n): cu(at + i)
            at += n
    out, in_entry, done_glob, done_deco = [], False, False, False
    for x in ins:
        if x[0] == OP_ENTRY and x[2] == entry: x = x + iface
        if x[0] == OP_DECORATE and not done_deco: out.extend(deco); done_deco = True
        if x[0] == OP_FUNCTION and not done_glob: out.extend(glob); done_glob = True
        if x[0] == OP_FUNCTION: in_entry = x[2] == entry
        if x[0] == OP_FUNCTIONEND: in_entry = False
        if x[0] == OP_RETURN and in_entry:
            vi, vu = new(), new(); out += [[OP_LOAD, vidx[1], vi, vidx[0]], [OP_BITCAST, tu, vu, vi]]
            for b, e in enumerate(tf):
                if not e: continue
                base = new(); out.append([OP_IMUL, tu, base, vu, cu(e[1] // 4)]); at = 0
                for o, n in e[0]:
                    src = outs.get(o)
                    val = None
                    if src and src[1] == tv4: val = new(); out.append([OP_LOAD, tv4, val, src[0]])
                    for i in range(n):
                        bits = new()
                        if val is not None: c = new(); out += [[OP_COMPOSITEEXTRACT, tf_, c, val, i], [OP_BITCAST, tu, bits, c]]
                        else: bits = cu(0)   # an output the shader does not write: 0, as main0_tf stores
                        idx, p_ = new(), new()
                        out += [[OP_IADD, tu, idx, base, cu(at + i)], [OP_ACCESSCHAIN, pu, p_, bufs[b], ci0, idx], [OP_STORE, p_, bits]]
                    at += n
        out.append(x)
    hdr = list(hdr); hdr[3] = bound
    return emit(hdr, out)

# ---- Vertex inputs (gfx_vk_draw.inc pipe_get matches the vertex formats to them; spv_build.py re-translates integer inputs) ----
OP_TYPEPTR_, OP_COPYOBJECT = 32, 83
def vin_types(ins):
    """{location: 'f' | 'u' | 's'}: the component type of each vertex input as the shader declares it"""
    loc = {x[1]: x[3] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_LOCATION}
    builtin = {x[1] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_BUILTIN}
    scal = {}
    for x in ins:
        if x[0] == OP_TYPEFLOAT: scal[x[1]] = 'f'
        elif x[0] == OP_TYPEINT_: scal[x[1]] = 's' if x[3] else 'u'
        elif x[0] == OP_TYPEVECTOR and x[2] in scal: scal[x[1]] = scal[x[2]]
    ptr = {x[1]: x[3] for x in ins if x[0] == OP_TYPEPTR_}
    return {str(loc[x[2]]): scal.get(ptr.get(x[1]), 'f') for x in ins if x[0] == OP_VARIABLE and x[3] == STORAGE_INPUT and x[2] in loc and x[2] not in builtin}
def int_inputs(ins):
    """Float-declared vertex inputs that the shader only uses as integers (every use a bit cast to an integer type, or a copy): {location: 'uint'} — dmsh_build.py
    int_inputs on the SPIR-V itself. Maxwell's fetch puts an integer format's bits into the register as they are (bone indices RGBA8UI); under a float declaration the
    translator's bit cast of a converted float gives garbage and skinned models stretch into streaks"""
    loc = {x[1]: x[3] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_LOCATION}
    builtin = {x[1] for x in ins if x[0] == OP_DECORATE and x[2] == DEC_BUILTIN}
    intty = {x[1] for x in ins if x[0] == OP_TYPEINT_}
    intty |= {x[1] for x in ins if x[0] == OP_TYPEVECTOR and x[2] in intty}
    fvars = {x[2] for x in ins if x[0] == OP_VARIABLE and x[3] == 7}   # Function storage: temporaries of the translator's debug mode
    vin = vin_types(ins); out = {}
    for x in ins:
        if not (x[0] == OP_VARIABLE and x[3] == STORAGE_INPUT and x[2] in loc and x[2] not in builtin and vin.get(str(loc[x[2]])) == 'f'): continue
        ptrs, vals, nint, bad = {x[2]}, set(), 0, False
        for _ in range(8):   # to a fixed point: chains, loads, extracts, copies through temporaries
            n0 = len(ptrs) + len(vals)
            for y in ins:
                op = y[0]
                if op == OP_ACCESSCHAIN and y[3] in ptrs: ptrs.add(y[2])
                elif op == OP_LOAD and y[3] in ptrs: vals.add(y[2])
                elif op in (OP_COMPOSITEEXTRACT, OP_COPYOBJECT) and y[3] in vals: vals.add(y[2])
                elif op == OP_STORE and y[2] in vals and y[1] in fvars: ptrs.add(y[1])
            if len(ptrs) + len(vals) == n0: break
        for y in ins:
            op = y[0]
            if op in (OP_ACCESSCHAIN, OP_LOAD, OP_COMPOSITEEXTRACT, OP_COPYOBJECT, OP_DECORATE, OP_ENTRY, OP_VARIABLE, OP_MEMBERDECORATE): continue
            if op == OP_STORE:
                if y[2] in vals and y[1] not in fvars: bad = True   # stored elsewhere as a float
                continue
            if op == OP_BITCAST and y[3] in vals:
                if y[1] in intty: nint += 1
                else: bad = True
                continue
            if any(w in vals for w in y[3:]): bad = True
        if not bad and nint: out[str(loc[x[2]])] = 'uint'
    return out

def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2]); dst.mkdir(parents=True, exist_ok=True)
    keys = sys.argv[3:] or sorted(p.stem for p in src.glob('*.spv') if not p.name.endswith('.reader.spv'))
    n = nz = bad = 0
    for k in keys:
        res = json.loads((src / f'{k}.res.json').read_text(encoding='utf-8'))
        stage = res.get('stage', 'vs')
        hdr, ins, interlock = process((src / f'{k}.spv').read_bytes(), stage)
        (dst / f'{k}.spv').write_bytes(emit(hdr, ins)); n += 1
        if interlock: res.setdefault('notes', []).append('android: fragment shader interlock removed (no ordering of overlapping fragments)')
        res['set'] = SET_OF.get(stage, 0); res['android'] = ANDROID_CHAIN
        if stage == 'vs': res['vin'] = vin_types(ins)
        (dst / f'{k}.res.json').write_text(json.dumps(res), encoding='utf-8')
        if stage == 'vs' and res.get('tf'):
            (dst / f'{k}.tf.spv').write_bytes(tf_variant(hdr, ins, res['tf']))
        if stage == 'vs':
            z = z_variant(hdr, ins)
            if z: (dst / f'{k}.z.spv').write_bytes(z); nz += 1
            else: bad += 1; print(f'[RED] {k}: no position output found, no depth -1..1 variant')
    print(f'spv_android: {n} shaders, {nz} depth variants, {bad} without one')

if __name__ == '__main__':
    main()
