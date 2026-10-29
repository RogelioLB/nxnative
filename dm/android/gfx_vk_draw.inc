// Draws of the Android NVN replay (included by gfx_vk.c, which owns the device, textures, the command buffer and the submission).
// Shaders are translated offline (dmsh_xlate, then tools/spv_android.py): <DM_SPV_DIR>/<key>.spv, <key>.z.spv (vertex shaders, for NVN's -1..1 depth mode) and
// <key>.res.json (what each binding is: the translator's constant / storage buffers and texture handles). Every stage has its descriptor set (vertex 0, fragment 1,
// geometry 2); binding 0 of each is Ryujinx's support buffer.
//   Constant buffers: c0 and c2 are the driver's banks, made here from the bound state as the driver fills them (c2: texture handles at 0x20 + 8r, images at
//     0x120 + 8r, separate textures at 0x168 + 8r, separate samplers at 0x568 + 8r; c0: storage buffer addresses); c1 is the shader's own constant segment (in its
//     code memory, control +0x700..+0x704); c3 + r is NVN uniform slot r, copied from guest memory at the draw.
//   Textures: the translator names each sampled texture by the bank and word of its handle; the handle is read there (c2 as made, or the game's own uniform
//     buffer for a bindless handle), its IDs looked up in the texture / sampler pools the game registered (mtl_reg_tex / mtl_reg_samp).
//   Vertex and index data, uniform and storage buffers are copied from guest memory into a host-visible arena at the draw; storage buffers a shader writes are
//     copied back when the submission completes.
// Transform feedback (the Adreno has no VK_EXT_transform_feedback): a vertex shader with a feedback layout has a <key>.tf.spv variant that stores its captured
// outputs into storage buffers (set 0, binding 100 + buffer; tools/spv_android.py tf_variant); the capture draws it as points, rasterization off, in a pass
// without attachments, vertex v of the draw at v × stride from the buffer's write offset, copied back to guest memory when the chunk completes (nvn_mtl.m tf_draw:
// non-indexed, non-instanced, not fans / quads). Rasterizer discard draws nothing else.
// Compute dispatches run the same way (set 0, the compute stage's banks, c0 + 0x200 the group counts). Buffer textures and buffer images are texel buffer views over
// copies in the arena (written ones copied back); images are the textures' own images (storage usage), so what a shader writes stays on the GPU.
// Coherence: submissions are synchronous and guest memory is copied at each draw, so a draw or dispatch after one that wrote guest memory (storage buffers, buffer
// images, texture-to-buffer copies) first completes the submission; indirect arguments are then read on the CPU and the draw made direct.
// The window origin follows NVN's (mtl_set_origin_lower_left): lower left (GL, NVN's default) keeps the viewport as is, so memory rows are GL's window rows, and
// inverts the front face; upper left flips the viewport. Depth -1..1 (mtl_set_depth_zero 0, NVN's default) takes the vertex shaders' .z variants.

// ---------------- A small JSON reader (the translator's res.json) ----------------
typedef struct jv { int t; double num; char *s; struct jv *kid; char **key; int n; } jv;   // t: 0 null 1 number 2 string 3 array 4 object 5 bool
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
static const char *js_parse(const char *p, jv *v);
static const char *js_str(const char *p, char **out) {
    p++; const char *b = p; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    size_t n = (size_t)(p - b); *out = malloc(n + 1); memcpy(*out, b, n); (*out)[n] = 0;
    return *p ? p + 1 : p;
}
static const char *js_parse(const char *p, jv *v) {
    memset(v, 0, sizeof *v); p = js_ws(p);
    if (*p == '{' || *p == '[') {
        int obj = *p == '{'; v->t = obj ? 4 : 3; p = js_ws(p + 1);
        while (*p && *p != (obj ? '}' : ']')) {
            v->kid = realloc(v->kid, (v->n + 1) * sizeof *v->kid);
            if (obj) { v->key = realloc(v->key, (v->n + 1) * sizeof *v->key); p = js_str(js_ws(p), &v->key[v->n]); p = js_ws(p); if (*p == ':') p++; }
            p = js_ws(js_parse(p, &v->kid[v->n])); v->n++;
            if (*p == ',') p = js_ws(p + 1);
        }
        return *p ? p + 1 : p;
    }
    if (*p == '"') { v->t = 2; return js_str(p, &v->s); }
    if (!strncmp(p, "true", 4)) { v->t = 5; v->num = 1; return p + 4; }
    if (!strncmp(p, "false", 5)) { v->t = 5; return p + 5; }
    if (!strncmp(p, "null", 4)) return p + 4;
    char *e; v->num = strtod(p, &e); v->t = 1; return e > p ? e : p + 1;
}
static void js_free(jv *v) { for (int i = 0; i < v->n; i++) { js_free(&v->kid[i]); if (v->key) free(v->key[i]); } free(v->kid); free(v->key); free(v->s); }
static const jv *js_get(const jv *o, const char *k) { if (o->t != 4) return NULL; for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], k)) return &o->kid[i]; return NULL; }
static int js_int(const jv *o, const char *k, int def) { const jv *v = js_get(o, k); return v && (v->t == 1 || v->t == 5) ? (int)v->num : def; }

// ---------------- The pipeline cache ----------------
// The driver's compiled pipelines kept across runs: loaded from DM_PIPELINE_CACHE (front_android.c: the app's internal files) when the first pipeline is made,
// written back by vkb_pipeline_cache_save (the app paused or stopped) when it grew
static VkPipelineCache g_pc; static int g_pc_tried; static size_t g_pc_saved;
static VkPipelineCache pcache(void) {
    if (g_pc_tried) return g_pc;
    g_pc_tried = 1;
    const char *path = getenv("DM_PIPELINE_CACHE"); void *data = NULL; long n = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); data = malloc((size_t)n); if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f); }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = (size_t)n, .pInitialData = n ? data : NULL};
    if (vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pc) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pc); n = 0; }   // a cache of another driver is refused: started empty
    free(data); g_pc_saved = (size_t)n;
    if (path) LOG("[vk] pipeline cache: %ld bytes from %s", n, path);
    return g_pc;
}
// Under a lock of its own, not the replay's: the driver's cache is internally synchronized, and copying out its tens of megabytes held the game's draws up
static pthread_mutex_t g_pc_mu = PTHREAD_MUTEX_INITIALIZER;
void vkb_pipeline_cache_save(void) {
    const char *path = getenv("DM_PIPELINE_CACHE");
    if (!g_pc || !path) return;
    pthread_mutex_lock(&g_pc_mu);
    size_t n = 0; void *data = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_pc, &n, NULL) == VK_SUCCESS && n > g_pc_saved && (data = malloc(n)) && vkGetPipelineCacheData(g_vk.dev, g_pc, &n, data) == VK_SUCCESS) {
        char tmp[600]; snprintf(tmp, sizeof tmp, "%s.new", path); FILE *f = fopen(tmp, "wb");
        if (f && fwrite(data, 1, n, f) == n && !fclose(f) && !rename(tmp, path)) { LOG("[vk] pipeline cache: %zu bytes written", n); g_pc_saved = n; }
        else if (f) fclose(f);
    }
    free(data);
    pthread_mutex_unlock(&g_pc_mu);
}

// ---------------- Shaders ----------------
typedef struct { int binding, slot; uint32_t ext; } rb_cbuf;   // ext: bytes the shader reads of it (spv_cb_ext; 0 = all)
typedef struct { int binding, slot, cb_slot, cb_off, flags; } rb_sbuf;
typedef struct { int binding, type, cbuf_slot, handle, array, separate, flags; VkFormat fmt; int samp_slot; } rb_tex;   // fmt: an image's declared format (0: Unknown); cbuf_slot / samp_slot: the banks of the texture and sampler handles (rb_tex_read)
enum { SH_VS, SH_PS, SH_GS, SH_TS, SH_CS };
typedef struct {
    char key[17]; int stage, set, ok; VkShaderStageFlagBits vkstage; VkShaderModule mod, modz, modtf; uint32_t tf_stride[4]; VkDescriptorSetLayout dsl_tf;
    int ncb, nsb, ntx, nim; rb_cbuf cb[18]; rb_sbuf sb[16]; rb_tex tx[64], im[16];
    VkDescriptorSetLayout dsl;
    char vin[16];   // vertex inputs' types: 'f' float, 'u' uint, 's' int (res.json "vin")
    int vid;   // a vertex shader reading gl_VertexIndex: its draws keep their vertex numbers (bind_vbufs lo 0)
    uint32_t io_in, io_out;   // the locations of its user-defined inputs and outputs (bit n: location n)
    struct { uint32_t miss; VkShaderModule m; } var[4]; int nvar;   // fragment shaders: variants with unwritten inputs made constants (ps_variant)
} vshader;
static nvn_hmap g_sh = {.klen = 16};
static char g_spv_dir[512];
static int g_depth_zero, g_origin_ll = 1, g_queue_ll;   // g_queue_ll: the queue's raster flip, captured when it was initialized (mtl_set_queue_origin_lower_left); winding = front ^ queue ^ device, as on Metal
// ---- Inputs no earlier stage writes ----
// Vulkan wants every fragment shader input written by the stage before it (the Switch reads an unwritten one as whatever is there), and the Adreno 619 refuses such
// a pipeline (VK_ERROR_UNKNOWN): Super Mario Bros. Wonder pairs fragment shader f4b9c56566956b03 (locations 0-4) with vertex shaders that write 0, 1, 2 and 4.
// The pipeline then takes a variant of the fragment shader in which those inputs are private variables holding zero
static uint32_t *spv_read(const char *path, size_t *nw) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n + 4); size_t got = code ? fread(code, 1, (size_t)n, f) : 0; fclose(f);
    if (!code || got != (size_t)n || n % 4 || n < 20) { free(code); return NULL; }
    *nw = (size_t)n / 4; return code;
}
static void spv_io(const uint32_t *w, size_t n, uint32_t *in, uint32_t *out) {   // the location masks of the user-defined Input / Output variables
    *in = *out = 0; uint32_t bound = w[3]; int32_t *loc = malloc(bound * sizeof *loc); if (!loc) return;
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];   // OpDecorate id Location L
        i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 59 && len >= 4 && w[i + 2] < bound && loc[w[i + 2]] >= 0 && loc[w[i + 2]] < 32) { if (w[i + 3] == 1) *in |= 1u << loc[w[i + 2]]; else if (w[i + 3] == 3) *out |= 1u << loc[w[i + 2]]; }   // OpVariable Input / Output
        i += len; }
    free(loc);
}
static int spv_vid(const uint32_t *w, size_t n) {   // a variable decorated BuiltIn VertexIndex (42)
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 11 && w[i + 3] == 42) return 1;   // OpDecorate id BuiltIn VertexIndex
        i += len; }
    return 0;
}
// How many bytes of each constant buffer the shader reads (ext[i] for binding bind[i]; 0 = all of it): the translator declares every one as 4096 vec4, and the replay copied
// the bound buffer whole for every draw (up to 64 KB, six or seven a shader) where Super Mario Bros. Wonder's scene shaders read 64 bytes to 2.3 KB of each. The extent is the
// largest constant element index of the access chains on the buffer's variable; a variable index, or any other use of the variable, keeps the whole buffer
static void spv_cb_ext(const uint32_t *w, size_t n, const rb_cbuf *cb, int ncb, uint32_t *ext) {
    for (int i = 0; i < ncb; i++) ext[i] = 0;
    uint32_t bound = w[3]; int32_t *bind = malloc(bound * sizeof *bind); int64_t *cval = malloc(bound * sizeof *cval); uint8_t *isc = calloc(bound, 1);
    if (!bind || !cval || !isc) { free(bind); free(cval); free(isc); return; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 1] < bound) bind[w[i + 1]] = (int32_t)w[i + 3];   // OpDecorate id Binding b
        if (op == 43 && len == 4 && w[i + 2] < bound) { isc[w[i + 2]] = 1; cval[w[i + 2]] = (int32_t)w[i + 3]; }   // OpConstant (32-bit)
        i += len; }
    int64_t mx[18]; int full[18]; for (int k = 0; k < ncb; k++) { mx[k] = -1; full[k] = 0; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op != 71 && op != 5 && op != 6 && op != 15 && op != 59) {   // decorations, names, entry points and the variable's own declaration are not uses
            for (uint32_t q = 1; q < len; q++) { uint32_t id = w[i + q]; if (id >= bound || bind[id] < 0) continue;
                int k = 0; while (k < ncb && cb[k].binding != bind[id]) k++; if (k == ncb) continue;
                if ((op == 65 || op == 66) && q == 3 && len >= 6 && isc[w[i + 5]] && cval[w[i + 5]] >= 0) { if (cval[w[i + 5]] > mx[k]) mx[k] = cval[w[i + 5]]; }   // OpAccessChain t r var member element
                else full[k] = 1; } }
        i += len; }
    for (int k = 0; k < ncb; k++) ext[k] = full[k] || mx[k] < 0 ? 0 : (uint32_t)((mx[k] + 1) * 16 > 0x10000 ? 0x10000 : (mx[k] + 1) * 16);
    free(bind); free(cval); free(isc);
}
// the Private pointer type to pointee t: an existing one, else a new id (listed in np to be declared)
static uint32_t priv_ptr(uint32_t t, uint32_t bound, const uint32_t *pst, const uint32_t *ppt, uint32_t *privp, uint32_t *nb, uint32_t np[][2], int *nnp) {
    if (t >= bound) return 0;
    if (privp[t]) return privp[t];
    for (uint32_t q = 0; q < bound; q++) if (pst[q] == 6 && ppt[q] == t) return privp[t] = q;
    if (*nnp >= 32) return 0;
    privp[t] = (*nb)++; np[*nnp][0] = t; np[(*nnp)++][1] = privp[t];
    return privp[t];
}
// w with its Input variables at the locations in miss made Private, initialized to zero (OpConstantNull); NULL if it cannot be done
static uint32_t *spv_drop_inputs(const uint32_t *w, size_t n, uint32_t miss, size_t *on) {
    uint32_t bound = w[3]; size_t cap = n + 2048; uint32_t *o = malloc(cap * 4);   // what is inserted (pointer types, zeros, variables) is at most 32 x 12 words
    int32_t *loc = malloc(bound * 4); uint32_t *pst = calloc(bound, 4), *ppt = calloc(bound, 4), *privp = calloc(bound, 4), *nullc = calloc(bound, 4); uint8_t *der = calloc(bound, 1);
    uint32_t *res = NULL, nb = bound; size_t k = 0;
    uint32_t np[32][2]; int nnp = 0, nvar = 0; uint32_t vars[32][2];   // new Private pointer types {pointee, id}; the variables {id, pointee}
    if (!o || !loc || !pst || !ppt || !privp || !nullc || !der) goto done;
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) goto done;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];
        if (op == 32 && len == 4 && w[i + 1] < bound) { pst[w[i + 1]] = w[i + 2]; ppt[w[i + 1]] = w[i + 3]; }   // OpTypePointer id storage pointee
        i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 59 && len >= 4 && w[i + 3] == 1 && w[i + 2] < bound && w[i + 1] < bound && loc[w[i + 2]] >= 0 && loc[w[i + 2]] < 32 && (miss >> loc[w[i + 2]] & 1) && nvar < 32) {
            uint32_t t = ppt[w[i + 1]]; if (!priv_ptr(t, bound, pst, ppt, privp, &nb, np, &nnp)) goto done;
            der[w[i + 2]] = 1; vars[nvar][0] = w[i + 2]; vars[nvar++][1] = t; if (t < bound && !nullc[t]) nullc[t] = nb++; }
        if ((op == 65 || op == 66) && len >= 4 && w[i + 3] < bound && der[w[i + 3]] && w[i + 2] < bound && w[i + 1] < bound) {   // access chains into them
            der[w[i + 2]] = 1; if (!priv_ptr(ppt[w[i + 1]], bound, pst, ppt, privp, &nb, np, &nnp)) goto done; }
        i += len; }
    if (!nvar) goto done;
    memcpy(o, w, 20); k = 5; int glob = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (k + len + 512 > cap) goto done;
        if (op == 15) {   // OpEntryPoint model id "name" interface...: the variables leave the interface
            size_t s = i + 3; while (s < i + len) { uint32_t x = w[s++]; if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break; }
            size_t at = k; memcpy(o + k, w + i, (s - i) * 4); k += s - i;
            for (size_t q = s; q < i + len; q++) if (!(w[q] < bound && der[w[q]])) o[k++] = w[q];
            o[at] = (uint32_t)((k - at) << 16) | op; i += len; continue; }
        if ((op == 71 || op == 72) && w[i + 1] < bound && der[w[i + 1]]) { i += len; continue; }   // their decorations
        if (op == 59 && w[i + 2] < bound && der[w[i + 2]]) { i += len; continue; }   // declared again below
        if (op == 54 && !glob) {   // before the first function: the pointer types, the zeros and the variables
            glob = 1;
            for (int q = 0; q < nnp; q++) { o[k++] = 4u << 16 | 32; o[k++] = np[q][1]; o[k++] = 6; o[k++] = np[q][0]; }
            for (uint32_t t = 0; t < bound; t++) if (nullc[t]) { o[k++] = 3u << 16 | 46; o[k++] = t; o[k++] = nullc[t]; }
            for (int q = 0; q < nvar; q++) { o[k++] = 5u << 16 | 59; o[k++] = privp[vars[q][1]]; o[k++] = vars[q][0]; o[k++] = 6; o[k++] = nullc[vars[q][1]]; }
        }
        memcpy(o + k, w + i, len * 4);
        if ((op == 65 || op == 66) && w[i + 3] < bound && der[w[i + 3]] && w[i + 1] < bound) o[k + 1] = privp[ppt[w[i + 1]]];
        k += len; i += len;
    }
    o[3] = nb; res = o; o = NULL; *on = k;
done:
    free(o); free(loc); free(pst); free(ppt); free(privp); free(nullc); free(der);
    return res;
}
static VkShaderModule sh_module_code(const uint32_t *code, size_t nw, const char *what) {
    VkShaderModule m = VK_NULL_HANDLE; VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = code};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS) { LOG("[RED] [vk] shader module %s refused by the driver", what); m = VK_NULL_HANDLE; }
    atomic_fetch_add(&g_mod_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_mod_made, 1);
    return m;
}
static VkShaderModule ps_variant(vshader *s, uint32_t miss) {
    for (int i = 0; i < s->nvar; i++) if (s->var[i].miss == miss) return s->var[i].m;
    char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
    size_t n = 0, on = 0; uint32_t *w = spv_read(p, &n), *v = w ? spv_drop_inputs(w, n, miss, &on) : NULL;
    VkShaderModule m = v ? sh_module_code(v, on, p) : VK_NULL_HANDLE;
    { const char *d = getenv("DM_DUMP_DIR"); if (v && d) { char q[700]; mkdir(d, 0777); snprintf(q, sizeof q, "%s/var_%s_%x.spv", d, s->key, miss); FILE *f = fopen(q, "wb"); if (f) { fwrite(v, 4, on, f); fclose(f); } } }   // for spirv-val
    LOG("[vk] fragment shader %s: inputs at locations %#x are written by no earlier stage of a pipeline: a variant reads them as zero%s", s->key, miss, m ? "" : " [RED] (variant failed: the pipeline keeps the shader as it is)");
    free(w); free(v);
    if (!m) m = s->mod;
    if (s->nvar < 4) { s->var[s->nvar].miss = miss; s->var[s->nvar++].m = m; }
    return m;
}
static VkShaderModule sh_module(const char *path) {
    size_t n = 0; uint32_t *code = spv_read(path, &n); if (!code) return VK_NULL_HANDLE;
    if (!g_vk_feat.vertexPipelineStoresAndAtomics) {   // the Mali-G76: the vertex pipeline's storage resources must be NonWritable (spv_readonly.h)
        size_t cap = n + 3 * (size_t)code[3], on = 0; int wr = 0;
        uint32_t *s = malloc(2 * (size_t)code[3] * 4), *o = malloc(cap * 4);
        if (s && o && (on = spv_readonly(code, n, s, o, cap, &wr))) { free(code); code = o; n = on; o = NULL; }
        if (wr) LOG("[RED] [vk] %s writes %d storage resource(s) from the vertex pipeline: the GPU has no vertexPipelineStoresAndAtomics, the driver may refuse it", path, wr);
        free(s); free(o);
    }
    VkShaderModule m = sh_module_code(code, n, path);
    free(code); return m;
}
// Ryujinx's TextureFormat names (an image's declared format) → Vulkan, with the texel size
static const struct { const char *n; VkFormat f; int bytes; } DECL_FMT[] = {
    {"R8Unorm", VK_FORMAT_R8_UNORM, 1}, {"R8Snorm", VK_FORMAT_R8_SNORM, 1}, {"R8Uint", VK_FORMAT_R8_UINT, 1}, {"R8Sint", VK_FORMAT_R8_SINT, 1},
    {"R16Float", VK_FORMAT_R16_SFLOAT, 2}, {"R16Unorm", VK_FORMAT_R16_UNORM, 2}, {"R16Snorm", VK_FORMAT_R16_SNORM, 2}, {"R16Uint", VK_FORMAT_R16_UINT, 2}, {"R16Sint", VK_FORMAT_R16_SINT, 2},
    {"R32Float", VK_FORMAT_R32_SFLOAT, 4}, {"R32Uint", VK_FORMAT_R32_UINT, 4}, {"R32Sint", VK_FORMAT_R32_SINT, 4},
    {"R8G8Unorm", VK_FORMAT_R8G8_UNORM, 2}, {"R8G8Snorm", VK_FORMAT_R8G8_SNORM, 2}, {"R8G8Uint", VK_FORMAT_R8G8_UINT, 2}, {"R8G8Sint", VK_FORMAT_R8G8_SINT, 2},
    {"R16G16Float", VK_FORMAT_R16G16_SFLOAT, 4}, {"R16G16Unorm", VK_FORMAT_R16G16_UNORM, 4}, {"R16G16Snorm", VK_FORMAT_R16G16_SNORM, 4}, {"R16G16Uint", VK_FORMAT_R16G16_UINT, 4}, {"R16G16Sint", VK_FORMAT_R16G16_SINT, 4},
    {"R32G32Float", VK_FORMAT_R32G32_SFLOAT, 8}, {"R32G32Uint", VK_FORMAT_R32G32_UINT, 8}, {"R32G32Sint", VK_FORMAT_R32G32_SINT, 8},
    {"R8G8B8A8Unorm", VK_FORMAT_R8G8B8A8_UNORM, 4}, {"R8G8B8A8Snorm", VK_FORMAT_R8G8B8A8_SNORM, 4}, {"R8G8B8A8Uint", VK_FORMAT_R8G8B8A8_UINT, 4}, {"R8G8B8A8Sint", VK_FORMAT_R8G8B8A8_SINT, 4},
    {"R16G16B16A16Float", VK_FORMAT_R16G16B16A16_SFLOAT, 8}, {"R16G16B16A16Unorm", VK_FORMAT_R16G16B16A16_UNORM, 8}, {"R16G16B16A16Snorm", VK_FORMAT_R16G16B16A16_SNORM, 8},
    {"R16G16B16A16Uint", VK_FORMAT_R16G16B16A16_UINT, 8}, {"R16G16B16A16Sint", VK_FORMAT_R16G16B16A16_SINT, 8},
    {"R32G32B32A32Float", VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {"R32G32B32A32Uint", VK_FORMAT_R32G32B32A32_UINT, 16}, {"R32G32B32A32Sint", VK_FORMAT_R32G32B32A32_SINT, 16},
    {"R10G10B10A2Unorm", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4}, {"R10G10B10A2Uint", VK_FORMAT_A2B10G10R10_UINT_PACK32, 4}, {"R11G11B10Float", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4}};
static int fmt_bytes(VkFormat f) {   // the texel size of an uncompressed colour format the views below meet (0: not known here)
    for (size_t i = 0; i < sizeof DECL_FMT / sizeof *DECL_FMT; i++) if (DECL_FMT[i].f == f) return DECL_FMT[i].bytes;
    switch (f) { case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return 4; default: return 0; }
}
static void rb_tex_read(const jv *a, rb_tex *out, int *n, int cap) {
    for (int i = 0; a && i < a->n && *n < cap; i++) {
        const jv *o = &a->kid[i]; const jv *fm = js_get(o, "format"); VkFormat f = VK_FORMAT_UNDEFINED;
        for (size_t k = 0; fm && fm->s && k < sizeof DECL_FMT / sizeof *DECL_FMT; k++) if (!strcmp(fm->s, DECL_FMT[k].n)) f = DECL_FMT[k].f;
        // the translator's cbuf_slot is Ryujinx's TextureHandle.PackSlots: texture bank in the low 16 bits, sampler bank + 1 in the high ones (0: the texture's); -1: none given (c2).
        // Read whole it named no bank for a separate texture with its sampler (0x30002: both in c2), and every such texture read 0 (Danganronpa V3, a black screen)
        int cs = js_int(o, "cbuf_slot", 2), ts = cs < 0 ? -1 : (int)(uint16_t)cs, ss = cs < 0 ? -1 : (uint32_t)cs >> 16 ? (int)((uint32_t)cs >> 16) - 1 : ts;
        out[(*n)++] = (rb_tex){js_int(o, "binding", 0), js_int(o, "type", 3), ts, js_int(o, "handle", 0), js_int(o, "array", 1), js_int(o, "separate", 0), js_int(o, "flags", 0), f, ss};
    }
}
static VkDescriptorType tex_dtype(const rb_tex *t, int image) {
    int buf = (t->type & 0xff) == 2;
    return image ? (buf ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) : (buf ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}
static vshader *sh_get(const char *key) {
    char k[16]; memcpy(k, key, 16);
    vshader *s = nvn_hm_get(&g_sh, k); if (s) return s->ok ? s : NULL;
    s = calloc(1, sizeof *s); memcpy(s->key, key, 16); nvn_hm_put(&g_sh, k, s);
    if (!g_spv_dir[0]) { const char *d = getenv("DM_SPV_DIR"); snprintf(g_spv_dir, sizeof g_spv_dir, "%s", d ? d : ""); }
    char p[700]; snprintf(p, sizeof p, "%s/%s.res.json", g_spv_dir, s->key);
    FILE *f = fopen(p, "rb");
    if (!f) { static int n; if (n++ < 16) LOG("[RED] [vk] no translated shader %s (DM_SPV_DIR %s)", s->key, g_spv_dir); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET); char *txt = calloc(1, (size_t)len + 1); fread(txt, 1, (size_t)len, f); fclose(f);
    jv r; js_parse(txt, &r); free(txt);
    const jv *st = js_get(&r, "stage"); const char *sn = st && st->s ? st->s : "vs";
    s->stage = !strcmp(sn, "ps") ? SH_PS : !strcmp(sn, "gs") ? SH_GS : !strcmp(sn, "cs") ? SH_CS : !strcmp(sn, "vs") ? SH_VS : SH_TS;
    s->vkstage = s->stage == SH_PS ? VK_SHADER_STAGE_FRAGMENT_BIT : s->stage == SH_GS ? VK_SHADER_STAGE_GEOMETRY_BIT : s->stage == SH_CS ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    s->set = js_int(&r, "set", s->stage == SH_PS ? 1 : s->stage == SH_GS ? 2 : 0);
    const jv *a = js_get(&r, "cbufs");
    for (int i = 0; a && i < a->n && s->ncb < 18; i++) s->cb[s->ncb++] = (rb_cbuf){js_int(&a->kid[i], "binding", 0), js_int(&a->kid[i], "slot", 0), 0};
    a = js_get(&r, "sbufs");
    for (int i = 0; a && i < a->n && s->nsb < 16; i++) { const jv *o = &a->kid[i]; s->sb[s->nsb++] = (rb_sbuf){js_int(o, "binding", 0), js_int(o, "slot", 0), js_int(o, "cb_slot", 0), js_int(o, "cb_offset", 0), js_int(o, "flags", 0)}; }
    rb_tex_read(js_get(&r, "textures"), s->tx, &s->ntx, 64); rb_tex_read(js_get(&r, "images"), s->im, &s->nim, 16);
    memset(s->vin, 'f', sizeof s->vin);
    a = js_get(&r, "vin");
    for (int q = 0; a && a->t == 4 && q < a->n; q++) { int l = atoi(a->key[q]); if (l >= 0 && l < 16 && a->kid[q].s) s->vin[l] = a->kid[q].s[0]; }
    a = js_get(&r, "tf");   // [[outputs, stride] or [] per feedback buffer]
    for (int b = 0; a && a->t == 3 && b < a->n && b < 4; b++) if (a->kid[b].t == 3 && a->kid[b].n == 2) s->tf_stride[b] = (uint32_t)a->kid[b].kid[1].num;
    js_free(&r);
    snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key); s->mod = sh_module(p);
    { size_t nw = 0; uint32_t *w = spv_read(p, &nw);
      if (w) { spv_io(w, nw, &s->io_in, &s->io_out); s->vid = s->stage == SH_VS && spv_vid(w, nw); uint32_t ext[18]; spv_cb_ext(w, nw, s->cb, s->ncb, ext); for (int i = 0; i < s->ncb; i++) s->cb[i].ext = ext[i]; free(w); } }
    if (s->stage == SH_VS) { snprintf(p, sizeof p, "%s/%s.z.spv", g_spv_dir, s->key); s->modz = sh_module(p); }
    if (s->tf_stride[0] | s->tf_stride[1] | s->tf_stride[2] | s->tf_stride[3]) { snprintf(p, sizeof p, "%s/%s.tf.spv", g_spv_dir, s->key); s->modtf = sh_module(p); }
    if (!s->mod) return NULL;
    // the set's layout: support buffer, constant buffers, storage buffers, textures, images
    VkDescriptorSetLayoutBinding b[1 + 18 + 16 + 64 + 16 + 4]; uint32_t nb = 0;
    b[nb++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->ncb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->cb[i].binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->nsb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->sb[i].binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->ntx; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->tx[i].binding, tex_dtype(&s->tx[i], 0), (uint32_t)(s->tx[i].array > 1 ? s->tx[i].array : 1), (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->nim; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->im[i].binding, tex_dtype(&s->im[i], 1), (uint32_t)(s->im[i].array > 1 ? s->im[i].array : 1), (VkShaderStageFlags)s->vkstage, NULL};
    VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &s->dsl) != VK_SUCCESS) { LOG("[RED] [vk] descriptor layout of shader %s failed", s->key); return NULL; }
    if (s->modtf) {   // the capture variant's set: the same bindings and the feedback buffers
        for (int q = 0; q < 4; q++) if (s->tf_stride[q]) b[nb++] = (VkDescriptorSetLayoutBinding){100u + (uint32_t)q, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        lc.bindingCount = nb;
        if (vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &s->dsl_tf) != VK_SUCCESS) s->modtf = VK_NULL_HANDLE;
    }
    s->ok = 1;
    if (vk_debug()) LOG("[vk] shader %s stage %d: %d constant buffers, %d storage buffers, %d textures, %d images%s", s->key, s->stage, s->ncb, s->nsb, s->ntx, s->nim, s->modz ? ", depth -1..1 variant" : "");
    return s;
}
static void key_hex(const uint8_t k[8], char out[17]) { static const char *hx = "0123456789abcdef"; for (int i = 0; i < 8; i++) { out[2 * i] = hx[k[i] >> 4]; out[2 * i + 1] = hx[k[i] & 15]; } out[16] = 0; }

// ---------------- The arena: host-visible memory for one submission's vertex, index, uniform and storage data ----------------
// the current chunk's (gfx_vk.c): blocks kept and reused when it completes
static int ar_new(VkDeviceSize n) {
    if (C->nar == C->car) { C->car = C->car ? 2 * C->car : 8; C->ar = realloc(C->ar, C->car * sizeof *C->ar); }
    arena_blk *a = &C->ar[C->nar]; memset(a, 0, sizeof *a);
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &a->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, a->b, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &a->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, a->b, NULL); return 0; }
    vkBindBufferMemory(g_vk.dev, a->b, a->m, 0); vkMapMemory(g_vk.dev, a->m, 0, n, 0, (void **)&a->p); a->n = n; C->nar++;
    return 1;
}
// n bytes aligned to 256 (the largest minimum uniform / storage offset alignment); the arena's blocks are reused after each submission completes
static uint8_t *ar_alloc(VkDeviceSize n, VkBuffer *b, VkDeviceSize *off) {
    n = n ? n : 16; if (!cur()) return NULL;
    for (;;) {
        if (C->ar_cur < C->nar) {
            arena_blk *a = &C->ar[C->ar_cur]; VkDeviceSize o = (a->used + 255) & ~(VkDeviceSize)255;
            if (o + n <= a->n) { a->used = o + n; *b = a->b; *off = o; C->ar_last = C->ar_cur; return a->p + o; }
            C->ar_cur++; continue;
        }
        if (!ar_new(n > (16u << 20) ? n : (16u << 20))) { RED_ONCE(80, "arena block of %llu bytes failed", (unsigned long long)n); return NULL; }
    }
}

// ---------------- Geometry kept on the GPU between frames ----------------
// A draw's indices and vertex streams were copied into the arena at every draw of every frame. Super Mario Bros. Wonder's world map with the camera far out draws 1,068 draws
// a frame reading 2.8 million indices and about 9 MB of vertices, nearly all of it the same from frame to frame: copying it (indices widened to 32 bits, their range scanned)
// was most of the render thread on an Adreno 619 (vk_draw, bind_vbufs and memcpy; 60 to 118 ms a frame). A range of guest memory is kept in a GPU buffer of its own once
// it was seen unchanged in a later frame, and used while the dirty-page tracker (nvn_dirty.c) shows no write to it since its stamp: the stamp is taken before the pages are
// protected, so any later write by the game or the host faults or pre-writes and raises the pages' generation. A range written twice is left to the per-draw copy (and no
// longer protected, so a buffer the game rewrites every frame costs no faults), and looked at again after a while. A kept copy's memory is reused only once the chunks that
// read it have completed (g_seq_done). DM_VK_GEOCACHE=0 turns it off
enum { GC_BLOCK = 32u << 20, GC_BUDGET = 256u << 20, GC_MAXN = 8u << 20, GC_NCLASS = 16, GC_RETRY = 600 };   // classes of 256 B << c up to 8 MB
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize used; } gc_blk;
typedef struct gc_reg { int blk; VkDeviceSize off; uint64_t seq; struct gc_reg *next; } gc_reg;   // a free region: reused when g_seq_done reaches seq
typedef struct { uintptr_t src; uint64_t n; uint64_t kind; } gc_key;   // kind 0: vertex bytes as they are; 1 + 2 × index size (+ 32: restart index made 0xffffffff): indices as 32 bits
typedef struct { gc_key k; const void *span; uint64_t stamp, seq, seen, dyn_at; int state, dirty, blk, cls; VkDeviceSize off; uint32_t mn, mx; } gc_ent;   // state 0 watched, 1 kept, 2 left to the copy
static gc_blk g_gc_blk[GC_BUDGET / GC_BLOCK]; static int g_gc_nblk; static gc_reg *g_gc_free[GC_NCLASS];
static nvn_hmap g_gc = {.klen = sizeof(gc_key)};
static uint64_t g_gc_hit, g_gc_made, g_gc_copy, g_gc_bytes;   // draws served from a kept copy, copies kept, draws copied as before, bytes kept
static int gc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_GEOCACHE"); v = !(e && *e == '0') && !vk_safe(); } return v; }
static int gc_class(uint64_t n) { int c = 0; while ((256ull << c) < n) c++; return c; }
static void gc_release(gc_ent *e) {   // its region back to its class's list, for when the chunks that read it have completed
    gc_reg *r = malloc(sizeof *r); if (!r) return;
    *r = (gc_reg){e->blk, e->off, e->seq, g_gc_free[e->cls]}; g_gc_free[e->cls] = r; g_gc_bytes -= 256ull << e->cls;
}
static void gc_evict(void) {   // the kept copies no draw used in the last few seconds
    uint64_t now = atomic_load(&g_presents);
    for (uint32_t i = 0; i < g_gc.cap; i++) { gc_ent *e = g_gc.vals[i]; if (e && e->state == 1 && e->seen + 120 < now) { gc_release(e); e->state = 0; e->dirty = 0; } }
}
static uint8_t *gc_alloc(int cls, int *blk, VkDeviceSize *off) {
    uint64_t done = __atomic_load_n(&g_seq_done, __ATOMIC_ACQUIRE);
    for (gc_reg **pp = &g_gc_free[cls]; *pp; pp = &(*pp)->next) if ((*pp)->seq <= done) {
        gc_reg *r = *pp; *pp = r->next; *blk = r->blk; *off = r->off; free(r); return g_gc_blk[*blk].p + *off; }
    VkDeviceSize n = 256ull << cls;
    for (int pass = 0; pass < 2; pass++) {
        if (g_gc_nblk && g_gc_blk[g_gc_nblk - 1].used + n <= GC_BLOCK) { gc_blk *b = &g_gc_blk[g_gc_nblk - 1]; *blk = g_gc_nblk - 1; *off = b->used; b->used += n; return b->p + *off; }
        if (g_gc_nblk < (int)(GC_BUDGET / GC_BLOCK)) {
            gc_blk *b = &g_gc_blk[g_gc_nblk]; memset(b, 0, sizeof *b);
            VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GC_BLOCK, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
            if (vkCreateBuffer(g_vk.dev, &bc, NULL, &b->b) != VK_SUCCESS) return NULL;
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b->b, &mr);
            uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
            if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &b->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, b->b, NULL); RED_ONCE(119, "geometry cache block failed: draws copy their geometry as before"); return NULL; }
            vkBindBufferMemory(g_vk.dev, b->b, b->m, 0); vkMapMemory(g_vk.dev, b->m, 0, GC_BLOCK, 0, (void **)&b->p); g_gc_nblk++;
            continue;
        }
        if (pass == 0) { gc_evict(); for (gc_reg **pp = &g_gc_free[cls]; *pp; pp = &(*pp)->next) if ((*pp)->seq <= done) {
            gc_reg *r = *pp; *pp = r->next; *blk = r->blk; *off = r->off; free(r); return g_gc_blk[*blk].p + *off; } }
    }
    return NULL;
}
// the kept GPU copy of guest memory [src, src + n) as kind (out_n bytes on the GPU): *fresh when the caller fills it now (from src: unchanged since the stamp); NULL: copy it per draw
static gc_ent *gc_get(uintptr_t src, uint64_t n, uint64_t kind, uint64_t out_n, uint8_t **fill) {
    *fill = NULL;
    if (!gc_on() || !C || !src || !n || out_n > GC_MAXN) return NULL;
    gc_key k; memset(&k, 0, sizeof k); k.src = src; k.n = n; k.kind = kind;
    uint64_t now = atomic_load(&g_presents); gc_ent *e = nvn_hm_get(&g_gc, &k);
    if (!e) {
        if (g_gc.n >= 65536 || !(e = calloc(1, sizeof *e))) return NULL;
        e->k = k; e->span = nvn_dirty_span((const void *)src, n); e->seen = now;
        if (!e->span) e->state = 2; else { e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); }   // untracked memory: always copied
        nvn_hm_put(&g_gc, &e->k, e); return NULL;
    }
    if (e->state == 2) {
        if (!e->span || now < e->dyn_at + GC_RETRY) return NULL;
        e->state = 0; e->dirty = 0; e->seen = now; e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); return NULL;   // looked at again
    }
    if (nvn_dirty_since_span(e->span, (const void *)src, n, e->stamp)) {   // written since the stamp
        if (e->state == 1) { gc_release(e); e->state = 0; }
        if (++e->dirty >= 2) { e->state = 2; e->dyn_at = now; return NULL; }
        e->seen = now; e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); return NULL;
    }
    if (e->state == 0) {   // kept once seen unchanged in a later frame than it was stamped in
        if (e->seen == now) return NULL;
        uint8_t *p = gc_alloc(gc_class(out_n), &e->blk, &e->off); if (!p) return NULL;
        e->cls = gc_class(out_n); e->state = 1; g_gc_made++; g_gc_bytes += 256ull << e->cls; *fill = p;
    }
    e->seen = now; e->seq = C->seq; g_gc_hit++;
    return e;
}
static VkBuffer gc_buf(const gc_ent *e) { return g_gc_blk[e->blk].b; }


// ---------------- Bound state ----------------
#define NTEX 256
static struct {
    const prog_t *bst[5];   // the program of each graphics stage (BindProgram binds per stage; nvn_mtl.m bind_stages)
    const prog_t *cprog;    // the compute program (stage bit 0x20 has a binding of its own)
    color_st color; chmask_st chmask; blend_st blend[8]; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    int tex_base, samp_base; uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][6]; int has_vp; float dr[2]; int depth_clamp; float blend_color[4]; p_prim_restart restart;
    p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on, rast_discard;
} D;
static int g_d_init;
static void draw_state_reset(void) {
    memset(&D, 0, sizeof D); g_d_init = 1;
    for (int i = 0; i < 8; i++) D.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    D.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; D.chmask.mask = 0xffffffffu;
    D.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    D.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    D.st_vmask[0] = D.st_vmask[1] = D.st_wmask[0] = D.st_wmask[1] = 0xff; D.dr[1] = 1;
}
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c
// The texture and sampler pools' entries, as the game registers them (IDs include each pool's base in the host table)
typedef struct { tex_t t; view_t v; int has_v, ok; } treg_t;
static treg_t *g_treg; static int g_ntreg; static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static void reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    treg_t *r = &g_treg[id]; if (t) { r->t = *t; r->ok = 1; } else r->ok = 0;
    r->has_v = v != NULL; if (v) r->v = *v;
    pthread_mutex_unlock(&g_reg_mu);
}
static void reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_nsreg) { int n = id + 1024; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    if (p) { g_sreg[id] = *p; g_sreg_ok[id] = 1; } else g_sreg_ok[id] = 0;
    pthread_mutex_unlock(&g_reg_mu);
}

static int draw_state_is(uint32_t op) {
    switch (op) {
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1: case OP_BIND_UBO: case OP_BIND_UBO_SMALL:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_BLEND_COLOR: case OP_PRIM_RESTART:
        case OP_TF_BIND: case OP_TF_STATE: case OP_RAST_DISCARD: return 1;
        default: return 0;
    }
}
// state records (nvn_mtl.m state_op): 1 when the op was one
static int draw_state_op(uint32_t op, const void *a) {
    if (!g_d_init) draw_state_reset();   // NVN's defaults, once (the state carries over between submissions, as in nvn_mtl.m)
    switch (op) {
        case OP_BIND_PROGRAM: { const p_bind_program *b = a; for (int st = 0; st < 5; st++) if (b->stages >> st & 1) D.bst[st] = b->prog; if (b->stages & 0x20) D.cprog = b->prog; return 1; }
        case OP_BIND_STATE: { const p_bind_state *b = a; blend_st bs;
            switch (b->kind) { case 0: memcpy(&bs, b->bytes, sizeof bs); D.blend[bs.target & 7] = bs; break; case 1: memcpy(&D.color, b->bytes, sizeof D.color); break;
                case 2: memcpy(&D.chmask, b->bytes, sizeof D.chmask); break; case 3: memcpy(&D.ms, b->bytes, sizeof D.ms); break;
                case 4: memcpy(&D.poly, b->bytes, sizeof D.poly); break; case 5: memcpy(&D.ds, b->bytes, sizeof D.ds); break; }
            return 1; }
        // the whole attribute / stream state is replaced (as nvn_mtl.m state_op): a slot past the count is unused, never a stale one read from a freed buffer
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; int n = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n; memset(D.attr, 0, sizeof D.attr); memcpy(D.attr, b->st, 4 * (size_t)n); D.nattr = n; return 1; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; int n = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n; memset(D.stream, 0, sizeof D.stream); memcpy(D.stream, b->st, sizeof *b->st * (size_t)n); D.nstream = n; return 1; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); for (int i = 0; i < b->n && b->first + i < 16; i++) D.vbuf[b->first + i] = b->r[i]; return 1; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage < 6 && b->index < 16) D.ubo[b->stage][b->index] = *b; return 1; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) D.ssbo[b->stage][b->index] = *b; return 1; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32 || b->stage >= 6 || b->first < 0 || b->first + b->n > NTEX) return 1;
            for (int k = 0; k < b->n; k++) D.tex[b->stage][b->first + k] = b->defer && b->src ? *(const uint64_t *)(uintptr_t)(b->src + 8u * k) : b->h[k];
            return 1; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) for (int k = 0; k < b->n; k++) D.samp[b->stage][b->first + k] = b->defer && b->src ? *(const uint64_t *)(uintptr_t)(b->src + 8u * k) : b->h[k]; return 1; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) for (int k = 0; k < b->n; k++) D.img[b->stage][b->first + k] = b->h[k]; return 1; }
        case OP_SET_TEX_POOL: D.tex_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 1); return 1;
        case OP_SET_SAMP_POOL: D.samp_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 0); return 1;
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) D.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) D.st_wmask[f] = (uint32_t)b->mask; else D.st_ref[f] = (uint32_t)b->ref; } return 1; }
        case OP_POLY_OFFSET: D.po = *(const p_poly_offset *)a; return 1;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(D.vp[b->first + i], b->v[i], sizeof D.vp[0]); D.has_vp = 1; return 1; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; D.dr[0] = b->n; D.dr[1] = b->f; return 1; }
        case OP_DEPTH_CLAMP: D.depth_clamp = ((const p_depth_clamp *)a)->on; return 1;
        case OP_BLEND_COLOR: memcpy(D.blend_color, ((const p_blend_color *)a)->rgba, 16); return 1;
        case OP_PRIM_RESTART: D.restart = *(const p_prim_restart *)a; return 1;
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) D.tf[b->index] = *b; return 1; }
        case OP_TF_STATE: { const p_tf_state *b = a;   // Begin writes from the start of each buffer; Pause / Resume keep the offsets (nvn_mtl.m: the game's control buffer is not used)
            if (b->what == 0) { D.tf_on = 1; memset(D.tf_off, 0, sizeof D.tf_off); } else D.tf_on = b->what == 3; return 1; }
        case OP_RAST_DISCARD: D.rast_discard = ((const p_rast_discard *)a)->on; return 1;
        default: return 0;
    }
}

// ---------------- Image views, samplers, dummies ----------------
typedef struct { VkImage img; VkImageViewType type; VkFormat fmt; uint32_t base_level, nlevels, base_layer, nlayers; VkImageAspectFlags aspect; uint8_t swz[4]; } view_key;
static int g_view_slice;   // view_get: a render target's view (rp_start): a 3D image's slice as a 2D view, not the whole volume (Bowser's Fury draws its colour-grading LUT a slice at a time)
static VkFormat g_view_fmt;   // view_get: a format other than the image's (a storage view in a shader's declared format; the image is mutable), 0 = the image's
static nvn_hmap g_views = {.klen = sizeof(view_key)};
static VkFormat view_format(const vtex *v, const view_t *vv);   // below: a texture view's own format (0: the image's)
static VkImageView view_get(const vtex *v, VkImageViewType type, uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers, VkImageAspectFlags aspect, const uint8_t swz[4]) {
    view_key k; memset(&k, 0, sizeof k);
    if (level >= v->levels) level = v->levels - 1;
    if (g_view_slice && v->type == VK_IMAGE_TYPE_3D) {   // a render target's slice of a 3D image (layer = the depth slice): a 2D view of it, which the image is 2D_ARRAY_COMPATIBLE for (gfx_vk.c vt_new)
        uint32_t dz = v->d >> level ? v->d >> level : 1; if (layer >= dz) layer = 0;
        type = VK_IMAGE_VIEW_TYPE_2D; nlevels = 1; nlayers = 1; goto make;
    }
    if (!nlevels || level + nlevels > v->levels) nlevels = v->levels - level;
    if (layer >= v->layers) layer = 0;
    if (!nlayers || layer + nlayers > v->layers) nlayers = v->layers - layer;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE && (nlayers < 6 || !(v->layers % 6 == 0))) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nlayers -= nlayers % 6;
    if ((type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) && nlayers > 1) nlayers = 1;
    if (v->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D;
    else if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D;
    if (v->type == VK_IMAGE_TYPE_1D && type != VK_IMAGE_VIEW_TYPE_1D_ARRAY) type = VK_IMAGE_VIEW_TYPE_1D;
    if (v->type == VK_IMAGE_TYPE_2D && (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) type = nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
make:
    k.img = v->img; k.type = type; k.fmt = g_view_fmt ? g_view_fmt : v->fmt; k.base_level = level; k.nlevels = nlevels; k.base_layer = layer; k.nlayers = nlayers; k.aspect = aspect;
    if (swz) memcpy(k.swz, swz, 4);
    VkImageView iv = nvn_hm_get(&g_views, &k); if (iv) return iv;
    static const VkComponentSwizzle S_[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (swz) { cm.r = swz[0] < 6 ? S_[swz[0]] : cm.r; cm.g = swz[1] < 6 ? S_[swz[1]] : cm.g; cm.b = swz[2] < 6 ? S_[swz[2]] : cm.b; cm.a = swz[3] < 6 ? S_[swz[3]] : cm.a; }
    VkImageViewCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = v->img, .viewType = type, .format = k.fmt, .components = cm,
                                .subresourceRange = {aspect, level, nlevels, layer, nlayers}};
    if (vkCreateImageView(g_vk.dev, &ic, NULL, &iv) != VK_SUCCESS) { RED_ONCE(81, "image view of format %d type %d failed", v->fmt, type); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_views, &k, iv);
    return iv;
}
// the views (and framebuffers) of an image that goes: destroyed with it
static nvn_hmap g_fbs;
static void views_drop(VkImage img) {
    for (uint32_t i = 0; i < g_views.cap; i++) {
        if (!g_views.vals[i]) continue;
        view_key *k = (view_key *)(void *)(g_views.keys + (size_t)i * g_views.klen);
        if (k->img != img) continue;
        VkImageView iv = g_views.vals[i];
        for (uint32_t j = 0; j < g_fbs.cap; j++) {   // framebuffers holding the view
            if (!g_fbs.vals[j]) continue;
            const VkImageView *fv = (const VkImageView *)(void *)(g_fbs.keys + (size_t)j * g_fbs.klen);
            int hit = 0; for (int q = 0; q < 9; q++) hit |= fv[q] == iv;
            if (hit) { uint8_t fk[9 * sizeof(VkImageView) + 12]; memcpy(fk, fv, sizeof fk); dead_add(2, (uint64_t)(VkFramebuffer)g_fbs.vals[j]); nvn_hm_del(&g_fbs, fk); j = (uint32_t)-1; }
        }
        dead_add(1, (uint64_t)iv); view_key kk = *k; nvn_hm_del(&g_views, &kk); i = (uint32_t)-1;
    }
}
static nvn_hmap g_samps = {.klen = sizeof(samp_par) + 4};
static VkSampler samp_get(const samp_par *p, int shadow) {
    struct { samp_par p; int shadow; } k; memset(&k, 0, sizeof k); if (p) k.p = *p; k.shadow = shadow;
    VkSampler s = nvn_hm_get(&g_samps, &k); if (s) return s;
    static const VkSamplerAddressMode W[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    int minf = p ? p->minf : 1, magf = p ? p->magf : 1;
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = minf == 1 || minf == 3 || minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = minf == 4 || minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = W[p ? p->ws & 7 : 7], .addressModeV = W[p ? p->wt & 7 : 7], .addressModeW = W[p ? p->wr & 7 : 7],
        .mipLodBias = p ? p->lod_bias : 0, .minLod = p ? p->lod_min : 0, .maxLod = minf >= 2 ? (p ? p->lod_max : 1000.f) : 0.25f,
        .compareEnable = shadow || (p && p->cmp_mode), .compareOp = p && p->cmp_func ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_LESS_OR_EQUAL};
    if (p && p->aniso > 1.f) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = p->aniso > g_vk.props.limits.maxSamplerAnisotropy ? g_vk.props.limits.maxSamplerAnisotropy : p->aniso; }
    if (p) { float a = p->border[3], l = p->border[0] + p->border[1] + p->border[2];   // border colours: Vulkan without VK_EXT_custom_border_color has three
        ci.borderColor = a < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : l > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; }
    if (vkCreateSampler(g_vk.dev, &ci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    nvn_hm_put(&g_samps, &k, s);
    return s;
}
// a 1×1 (×6 layers) zero texture for each view kind: unbound and unresolvable slots read 0, as on the Switch
static vtex *g_dummy;
static VkImageView dummy_view(VkImageViewType type) {
    if (!g_dummy) {
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0ull; t.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; t.d.format = NVN_FORMAT_RGBA8;
        t.d.width = t.d.height = 1; t.d.depth = 6; t.d.levels = 1;
        if (!(g_dummy = vt_create(&t))) return VK_NULL_HANDLE;
    }
    if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // a 3D slot gets a 2D array view: not a match, reported once
    return view_get(g_dummy, type, 0, 1, 0, type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
static VkImageViewType view_type_of(int st) {   // Ryujinx's SamplerType
    int arr = (st & 0x100) != 0;
    switch (st & 0xff) {
        case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 4: return VK_IMAGE_VIEW_TYPE_3D;
        case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

// ---------------- Render passes and framebuffers ----------------
typedef struct { VkFormat color[8]; VkFormat depth; uint32_t ncolor, samples; } rp_key;
static nvn_hmap g_rps = {.klen = sizeof(rp_key)}, g_fbs = {.klen = 9 * sizeof(VkImageView) + 12};
static VkRenderPass rp_get(const rp_key *k) {
    VkRenderPass rp = nvn_hm_get(&g_rps, k); if (rp) return rp;
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr; uint32_t na = 0;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!k->color[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        at[na] = (VkAttachmentDescription){0, k->color[i], (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                           VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        cr[i] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (k->depth) {
        at[na] = (VkAttachmentDescription){0, k->depth, (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
                                           VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        dr = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor, .pColorAttachments = cr, .pDepthStencilAttachment = k->depth ? &dr : NULL};
    VkSubpassDependency dep[2] = {{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
                                  {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep};
    if (vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp) != VK_SUCCESS) { RED_ONCE(82, "render pass creation failed"); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_rps, k, rp);
    return rp;
}
static struct { int active; VkRenderPass rp; rp_key key; uint32_t w, h; VkFramebuffer fb; vtex *cv[8]; uint32_t clv[8], cly[8]; } P;
static uint64_t g_passes;
// DM_VK_DUMP=<frame>: every render pass of that frame (counted in presents from 1) has its colour targets copied out when it ends, written to
// $DM_DUMP_DIR/f<frame>_p<pass>_a<attachment>_<w>x<h>_vk<format>_s<serial>.raw once the frame is presented, with a log line per draw (shaders, viewport, textures)
typedef struct { stg_t s; uint32_t w, h; VkFormat f; int pass, att; uint64_t ser; } dump_t;
static dump_t g_dumps[512]; static int g_ndump, g_dump_pass;
// or a file named dump_now in $DM_DUMP_DIR's parent: the frame two presents after it is seen (checked every 30 presents; the file is removed)
static long g_dump_at = -2;
static long dump_frame(void) { if (g_dump_at == -2) { const char *e = getenv("DM_VK_DUMP"); g_dump_at = e ? atol(e) : -1; } return g_dump_at; }
static void dump_poll(uint64_t presents) {
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../dump_now", dir);
    if (!access(p, F_OK)) { unlink(p); mkdir(dir, 0777); dump_frame(); g_dump_at = (long)presents + 2; LOG("[dump] dump_now seen: frame %ld is dumped", g_dump_at); }
}
static int dump_on(void) { long f = dump_frame(); return f > 0 && (long)atomic_load(&g_presents) + 1 == f; }
static void dump_pass_end(void) {
    g_dump_pass++;
    for (int i = 0; i < 8; i++) {
        vtex *v = P.cv[i]; if (!v || g_ndump >= (int)(sizeof g_dumps / sizeof *g_dumps)) continue;
        int bpp = fmt_bytes(v->fmt); if (!bpp) { LOG("[dump] pass %d att %d: format %d not dumped", g_dump_pass, i, v->fmt); continue; }
        uint32_t w = v->w >> P.clv[i] ? v->w >> P.clv[i] : 1, h = v->h >> P.clv[i] ? v->h >> P.clv[i] : 1;
        dump_t *d = &g_dumps[g_ndump]; if (!stg_new(&d->s, (VkDeviceSize)w * h * bpp)) continue;
        d->w = w; d->h = h; d->f = v->fmt; d->pass = g_dump_pass; d->att = i; d->ser = v->serial; g_ndump++;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        int z3 = v->type == VK_IMAGE_TYPE_3D;   // a 3D target's slice is a depth offset, not a layer
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, P.clv[i], z3 ? 0 : P.cly[i], 1}, .imageOffset = {0, 0, z3 ? (int32_t)P.cly[i] : 0}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, d->s.b, 1, &r);
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
}
static void dump_write(void) {   // after the dumped frame's commands completed
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir) dir = ".";
    for (int i = 0; i < g_ndump; i++) {
        dump_t *d = &g_dumps[i]; char path[700];
        snprintf(path, sizeof path, "%s/f%ld_p%03d_a%d_%ux%u_vk%d_s%llu.raw", dir, dump_frame(), d->pass, d->att, d->w, d->h, d->f, (unsigned long long)d->ser);
        FILE *fp = fopen(path, "wb"); if (fp) { fwrite(d->s.p, 1, (size_t)d->s.n, fp); fclose(fp); }
        memcpy(path + strlen(path) - 4, ".png", 4); png_texels(path, d->f, fmt_bytes(d->f), d->s.p, d->w, 0, 0, d->w, d->h, 0);   // beside the raw texels, in the formats gfx_vk.c px_rgb converts
        stg_free(&d->s);
    }
    LOG("[dump] frame %ld: %d targets written to %s", dump_frame(), g_ndump, dir); g_ndump = 0;
}
static void occl_end(void) { if (g_q_act >= 0 && C && C->open) vkCmdEndQuery(C->cb, C->qp, (uint32_t)g_q_act); g_q_act = -1; }   // the pass's open occlusion query (gfx_vk.c)
static void occl_begin(void) {   // a draw while samples passed are counted: inside a query of this pass
    if (!g_q_on || g_q_act >= 0 || !C || !C->qp || C->nq >= 1024) return;
    g_q_act = C->nq++; C->qg[g_q_act] = g_qslot++; g_q_queries++;
    vkCmdBeginQuery(C->cb, C->qp, (uint32_t)g_q_act, g_vk_feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
// ---- GPU time per render pass: a file named prof_now beside $DM_DUMP_DIR profiles the frame two presents later (the log lists its passes, costliest first) ----
typedef struct { chunk *c; int s0, s1, draws, ncolor, depth; uint32_t w, h; VkFormat f0; uint64_t t0, t1, seq, verts, big; const char *bigvs, *bigfs; } prof_rec;   // verts: vertices drawn (indices × instances); big: the largest draw's, and its shaders
static prof_rec g_prof[1024]; static int g_nprof, g_prof_cur = -1; static long g_prof_at = -1; static uint64_t g_prof_seq;
static int prof_on(void) { return g_prof_at > 0 && (long)atomic_load(&g_presents) + 1 == g_prof_at; }
static void prof_poll(uint64_t presents) {
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../prof_now", dir);
    if (!access(p, F_OK)) { unlink(p); g_prof_at = (long)presents + 2; g_nprof = 0; LOG("[prof] prof_now seen: frame %ld is profiled", g_prof_at); }
}
static void prof_pass_begin(void) {
    if (!prof_on() || !C || !C->pq || C->npq + 2 > 512 || g_nprof >= 1024) return;
    prof_rec *r = &g_prof[g_nprof]; memset(r, 0, sizeof *r); r->c = C; r->s0 = C->npq++; r->s1 = -1; r->w = P.w; r->h = P.h; r->ncolor = (int)P.key.ncolor; r->depth = P.key.depth != 0;
    for (uint32_t i = 0; i < P.key.ncolor; i++) if (P.key.color[i]) { r->f0 = P.key.color[i]; break; }
    r->seq = g_prof_seq++;
    vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->pq, (uint32_t)r->s0); g_prof_cur = g_nprof++;
}
static void prof_pass_end(void) {
    if (g_prof_cur < 0) return;
    prof_rec *r = &g_prof[g_prof_cur]; g_prof_cur = -1;
    if (r->c != C || C->npq >= 512) return;
    r->s1 = C->npq++; vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->pq, (uint32_t)r->s1);
}
static void prof_collect(chunk *c) {   // the completion thread: this chunk's timestamps into its records
    uint64_t t[512]; memset(t, 0, sizeof t);
    VkResult res = vkGetQueryPoolResults(g_vk.dev, c->pq, 0, (uint32_t)c->npq, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT); int got = 0, mine = 0;
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].c == c) { mine++; if (res == VK_SUCCESS && g_prof[i].s1 >= 0) { g_prof[i].t0 = t[g_prof[i].s0]; g_prof[i].t1 = t[g_prof[i].s1]; g_prof[i].c = NULL; got++; } }
    LOG("[prof] chunk %d done: %d timestamps, VkResult %d, %d of its %d records read", (int)(c - g_ch), c->npq, res, got, mine);
    c->npq = 0;
}
static int prof_cmp(const void *a, const void *b) { const prof_rec *x = a, *y = b; uint64_t dx = x->t1 - x->t0, dy = y->t1 - y->t0; return dx < dy ? 1 : dx > dy ? -1 : 0; }
static void prof_write(void) {
    double per = g_vk.props.limits.timestampPeriod, sum = 0; uint64_t lo = UINT64_MAX, hi = 0; int n = 0;
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].t1 > g_prof[i].t0) { sum += (g_prof[i].t1 - g_prof[i].t0) * per / 1e3; if (g_prof[i].t0 < lo) lo = g_prof[i].t0; if (g_prof[i].t1 > hi) hi = g_prof[i].t1; n++; }
    int unclosed = 0, unread = 0, ncs = 0; double cs = 0;
    for (int i = 0; i < g_nprof; i++) { if (g_prof[i].s1 < 0) unclosed++; else if (g_prof[i].c) unread++; if (g_prof[i].ncolor < 0 && g_prof[i].t1 > g_prof[i].t0) { ncs++; cs += (g_prof[i].t1 - g_prof[i].t0) * per / 1e3; } }
    LOG("[prof] frame %ld: %d records (%d not closed, %d not read), %d timed, %.0f us in them (%d dispatches %.0f us), first start to last end %.0f us", g_prof_at, g_nprof, unclosed, unread, n, sum, ncs, cs, hi > lo ? (hi - lo) * per / 1e3 : 0.0);
    qsort(g_prof, (size_t)g_nprof, sizeof *g_prof, prof_cmp);
    for (int i = 0; i < g_nprof && i < 40; i++) { prof_rec *r = &g_prof[i]; if (r->t1 <= r->t0) continue;
        if (r->ncolor < 0) LOG("[prof] %7.0f us  dispatch #%llu  %ux%ux%u groups", (r->t1 - r->t0) * per / 1e3, (unsigned long long)r->seq, r->w, r->h, (unsigned)r->draws);
        else LOG("[prof] %7.0f us  pass #%llu  %ux%u  colour %d (vk%d) depth %d  draws %d  vertices %llu (largest draw %llu: vs %s fs %s)", (r->t1 - r->t0) * per / 1e3, (unsigned long long)r->seq, r->w, r->h,
                 r->ncolor, r->f0, r->depth, r->draws, (unsigned long long)r->verts, (unsigned long long)r->big, r->bigvs ? r->bigvs : "-", r->bigfs ? r->bigfs : "-"); }
    g_nprof = 0; g_prof_at = -1;
}
static void rp_end(void) { if (P.active) { occl_end(); prof_pass_end(); vkCmdEndRenderPass(C->cb); P.active = 0; if (dump_on()) dump_pass_end(); } }
// the render pass of the bound render targets (R.rt): images, views and framebuffer, made before the draw's textures are resolved (that may record uploads, outside a pass)
static int rp_prepare(void) {
    if (P.active) return 1;
    if (!R.has_rt) { RED_ONCE(83, "draw with no render target set"); return 0; }
    memset(P.cv, 0, sizeof P.cv);
    rp_key k; memset(&k, 0, sizeof k); VkImageView views[9]; memset(views, 0, sizeof views); uint32_t nv = 0, w = UINT32_MAX, h = UINT32_MAX, layers = 1; k.samples = 1;
    k.ncolor = (uint32_t)(R.rt.n > 8 ? 8 : R.rt.n);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        if (!R.rt.color[i]) continue;
        vtex *v = vt_get(R.rt.color[i]); if (!v) continue;
        const view_t *cv = R.rt.has_cview[i] ? &R.rt.cview[i] : NULL;
        uint32_t lv = cv && cv->set_levels ? cv->base_level : 0, ly = cv && cv->set_layers ? cv->min_layer : 0;
        // in the format of the target's view: Super Mario Bros. Wonder's last pass writes its RGBA8 window texture through an RGBA8_SRGB view, and in the image's
        // format the linear values were stored unencoded (the whole picture darker and more saturated; nvn_mtl.m found the same)
        g_view_fmt = cv ? view_format(v, cv) : VK_FORMAT_UNDEFINED; VkFormat af = g_view_fmt ? g_view_fmt : v->fmt;
        g_view_slice = 1; VkImageView iv = view_get(v, VK_IMAGE_VIEW_TYPE_2D, lv, 1, ly, 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL); g_view_slice = 0; g_view_fmt = VK_FORMAT_UNDEFINED; if (!iv) continue;
        k.color[i] = af; k.samples = v->samples; views[nv++] = iv; P.cv[i] = v; P.clv[i] = lv; P.cly[i] = ly;
        uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1; if (lw < w) w = lw; if (lh < h) h = lh;
    }
    if (R.rt.depth) {
        vtex *v = vt_get(R.rt.depth);
        if (v && v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            const view_t *dv = R.rt.has_dview ? &R.rt.dview : NULL;
            uint32_t lv = dv && dv->set_levels ? dv->base_level : 0, ly = dv && dv->set_layers ? dv->min_layer : 0;
            g_view_slice = 1; VkImageView iv = view_get(v, VK_IMAGE_VIEW_TYPE_2D, lv, 1, ly, 1, v->aspect, NULL); g_view_slice = 0;
            if (iv) { k.depth = v->fmt; k.samples = v->samples; views[nv++] = iv; uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1; if (lw < w) w = lw; if (lh < h) h = lh; }
        }
    }
    if (!nv || w == UINT32_MAX) { RED_ONCE(84, "render targets without an image"); return 0; }
    VkRenderPass rp = rp_get(&k); if (!rp) return 0;
    struct { VkImageView v[9]; uint32_t w, h, l; } fk; memset(&fk, 0, sizeof fk); memcpy(fk.v, views, sizeof views); fk.w = w; fk.h = h; fk.l = layers;
    VkFramebuffer fb = nvn_hm_get(&g_fbs, &fk);
    if (!fb) {
        VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = nv, .pAttachments = views, .width = w, .height = h, .layers = layers};
        if (vkCreateFramebuffer(g_vk.dev, &fc, NULL, &fb) != VK_SUCCESS) { RED_ONCE(85, "framebuffer %ux%u failed", w, h); return 0; }
        nvn_hm_put(&g_fbs, &fk, fb);
    }
    P.rp = rp; P.key = k; P.w = w; P.h = h; P.fb = fb;
    return 1;
}
static int rp_start(void) {
    if (P.active) return 1;
    VkCommandBuffer c = cb(); if (!c) return 0;
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = P.rp, .framebuffer = P.fb, .renderArea = {{0, 0}, {P.w, P.h}}};
    if (prof_on()) prof_pass_begin();
    vkCmdBeginRenderPass(c, &bi, VK_SUBPASS_CONTENTS_INLINE);
    P.active = 1; g_passes++;
    return 1;
}
// the submission completed: its arena and descriptor sets are free again (not during a draw: a flush inside one keeps what the draw has made)
// a completed chunk's draw resources: arena blocks and descriptor pools kept for reuse, texel buffer views destroyed (the completion thread)
static void draw_chunk_reset(chunk *c) {
    for (int i = 0; i < c->nar; i++) c->ar[i].used = 0; c->ar_cur = 0;
    for (int i = 0; i < c->ndp; i++) vkResetDescriptorPool(g_vk.dev, c->dp[i], 0); c->dp_cur = 0;
    for (int i = 0; i < c->nbv; i++) vkDestroyBufferView(g_vk.dev, c->bv[i], NULL); c->nbv = 0;
}

// ---------------- Pipelines ----------------
static VkFormat vtx_fmt(int f);
// Vertex attributes as Maxwell's fetch hands them to the shader: each component a 32-bit register, a float-type format (unorm / snorm / half / float, 10:10:10:2,
// 11:11:10) converted to float32 bits, an integer one widened by its own signedness, a *_UI2F / *_I2F one (integers read as their float value) converted to float; missing
// components 0, 0, 0 and 1 (1.0 for the float types). The shader declares each input float, uint or int (res.json "vin": spv_android.py; tools/spv_build.py makes
// integer-only inputs uint, as dmsh_build.py int_inputs). An attribute is bound as it is when the format's kind matches the input and the GPU takes it as a vertex format;
// otherwise its register bits are made on the CPU as the vertex data is copied, into a binding of its own (16 + attribute) read as R32G32B32A32 of the input's type
// (nvn_mtl.m vfmt_for_ct / vconv: the same cases on Metal). The Adreno 619 takes no scaled format at all; a skinning shader reading bone indices declared float stretched
// characters into streaks.
// NVN's vertex formats: n components, the component kind (0 unorm 1 snorm 2 uint 3 sint 4 float, 5 uint-to-float 6 sint-to-float, 7 / 8 / 9 / 10 RGB10A2 unorm / snorm /
// uint / sint, 11 / 12 RGB10A2 uint / sint to float, 13 R11G11B10F) and its bits (8 / 16 / 32); 0 = not one
static int vfmt_desc(int f, int *kind, int *bits) {
    static const int s8[4] = {0x01, 0x0D, 0x19, 0x25}, s16[4] = {0x05, 0x11, 0x1D, 0x29}, s32[4] = {0x0A, 0x16, 0x22, 0x2E}, sc[4] = {0x51, 0x57, 0x5D, 0x63};
    for (int n = 0; n < 4; n++) {
        if (f >= s8[n] && f < s8[n] + 4) { static const int k[4] = {0, 1, 2, 3}; *kind = k[f - s8[n]]; *bits = 8; return n + 1; }
        if (f >= s16[n] && f < s16[n] + 5) { static const int k[5] = {4, 0, 1, 2, 3}; *kind = k[f - s16[n]]; *bits = 16; return n + 1; }
        if (f >= s32[n] && f < s32[n] + 3) { static const int k[3] = {4, 2, 3}; *kind = k[f - s32[n]]; *bits = 32; return n + 1; }
        if (f >= sc[n] && f < sc[n] + 6) { int i = f - sc[n]; *kind = 5 + (i & 1); *bits = 8 << (i / 2); return n + 1; }
    }
    switch (f) {
        case 0x3D: *kind = 7; break; case 0x69: *kind = 8; break; case 0x3E: *kind = 9; break; case 0x6A: *kind = 10; break;
        case 0x6B: *kind = 11; break; case 0x6C: *kind = 12; break; case 0x3F: *kind = 13; *bits = 32; return 3;
        default: return 0;
    }
    *bits = 32; return 4;
}
static char vfmt_class(int kind) { return kind == 2 || kind == 9 ? 'u' : kind == 3 || kind == 10 ? 's' : 'f'; }   // what the register holds
static float half_f(uint16_t h) { uint32_t s = (uint32_t)(h >> 15) << 31, e = h >> 10 & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13; else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f; }
static float uf_small(uint32_t v, int mbits) { uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1); return !e ? ldexpf((float)m, -14 - mbits) : e == 31 ? INFINITY : ldexpf(1.f + (float)m / (float)(1u << mbits), (int)e - 15); }
static void vfmt_regs(int kind, int bits, int n, const uint8_t *src, uint32_t out[4]) {   // one attribute element → its four registers
    float one = 1.f; uint32_t onef; memcpy(&onef, &one, 4);
    int ftype = vfmt_class(kind) == 'f';
    out[0] = out[1] = out[2] = 0; out[3] = ftype ? onef : 1;
    for (int c = 0; c < n; c++) {
        float fv = 0; int64_t iv = 0; int isf = 1;
        if (kind >= 7 && kind <= 12) {   // RGB10A2
            uint32_t v; memcpy(&v, src, 4); int b = c == 3 ? 2 : 10; uint32_t u = v >> (10 * c) & ((1u << b) - 1); int32_t sv = (int32_t)(u << (32 - b)) >> (32 - b);
            if (kind == 7) fv = (float)u / (float)((1u << b) - 1); else if (kind == 8) { fv = (float)sv / (float)((1 << (b - 1)) - 1); if (fv < -1) fv = -1; }
            else if (kind == 9) { iv = u; isf = 0; } else if (kind == 10) { iv = sv; isf = 0; } else fv = kind == 11 ? (float)u : (float)sv;
        } else if (kind == 13) {   // R11G11B10F
            uint32_t v; memcpy(&v, src, 4); fv = c == 2 ? uf_small(v >> 22, 5) : uf_small(v >> (11 * c) & 0x7ff, 6);
        } else {
            uint64_t u = 0; memcpy(&u, src + c * bits / 8, (size_t)bits / 8); int64_t sv = bits == 64 ? (int64_t)u : (int64_t)(u << (64 - bits)) >> (64 - bits);
            switch (kind) {
                case 0: fv = (float)u / (float)((1ull << bits) - 1); break;
                case 1: fv = (float)sv / (float)((1ull << (bits - 1)) - 1); if (fv < -1) fv = -1; break;
                case 2: iv = (int64_t)u; isf = 0; break; case 3: iv = sv; isf = 0; break;
                case 4: if (bits == 16) fv = half_f((uint16_t)u); else { uint32_t w = (uint32_t)u; memcpy(&fv, &w, 4); } break;
                case 5: fv = (float)u; break; case 6: fv = (float)sv; break;
            }
        }
        if (isf) memcpy(&out[c], &fv, 4); else out[c] = (uint32_t)iv;
    }
}
// whether attribute format f can be bound as it is for a shader input of type t ('f' / 'u' / 's'): its kind matches and the GPU takes the Vulkan format as a vertex format
static int vfmt_direct_(int f, char t);
static int vfmt_direct(int f, char t) {   // cached: the driver's format query took 5% of the render thread when asked for every attribute of every draw
    static int8_t memo[256][3]; int ti = t == 'u' ? 1 : t == 's' ? 2 : 0;
    if (f < 0 || f >= 256) return vfmt_direct_(f, t);
    if (!memo[f][ti]) memo[f][ti] = vfmt_direct_(f, t) ? 1 : -1;
    return memo[f][ti] > 0;
}
static int vfmt_direct_(int f, char t) {
    int kind, bits, n = vfmt_desc(f, &kind, &bits);
    if (!n || kind == 5 || kind == 6 || kind == 11 || kind == 12) return 0;   // integers read as float: no vertex format here
    if (vfmt_class(kind) != t) return 0;
    VkFormat vf = vtx_fmt(f); if (!vf) return 0;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, vf, &fp);
    return (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}
static VkFormat vtx_fmt(int f) {   // an NVNformat as a vertex attribute
    static const uint8_t k8[4] = {0, 1, 4, 5}, k16[5] = {6, 0, 1, 4, 5};   // offsets in Vulkan's per-width order: UNORM SNORM USCALED SSCALED UINT SINT (SFLOAT at 6 for 16-bit)
    static const int b8[4] = {9, 16, 23, 37}, b16[4] = {70, 77, 84, 91}, b32[4] = {98, 101, 104, 107};
    for (int n = 0; n < 4; n++) {
        static const int s8[4] = {0x01, 0x0D, 0x19, 0x25}, s16[4] = {0x05, 0x11, 0x1D, 0x29}, s32[4] = {0x0A, 0x16, 0x22, 0x2E}, sc[4] = {0x51, 0x57, 0x5D, 0x63};
        if (f >= s8[n] && f < s8[n] + 4) return (VkFormat)(b8[n] + k8[f - s8[n]]);
        if (f >= s16[n] && f < s16[n] + 5) return (VkFormat)(b16[n] + k16[f - s16[n]]);
        if (f >= s32[n] && f < s32[n] + 3) return (VkFormat)(b32[n] + (f - s32[n] == 0 ? 2 : f - s32[n] - 1));
        if (f >= sc[n] && f < sc[n] + 6) { int i = f - sc[n], width = i / 2, sg = i & 1;   // *_UI2F / *_I2F: the scaled formats; 32-bit ones read as float of the integer (UINT / SINT taken as is)
            if (width == 0) return (VkFormat)(b8[n] + 2 + sg); if (width == 1) return (VkFormat)(b16[n] + 2 + sg); return (VkFormat)(b32[n] + sg); }
    }
    switch (f) {
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
static VkPrimitiveTopology topo_of(int p) {
    switch (p) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // triangles, quads (made triangles at the draw)
    }
}
static const VkBlendFactor BF[0x65] = {[0x1] = VK_BLEND_FACTOR_ZERO, [0x2] = VK_BLEND_FACTOR_ONE, [0x3] = VK_BLEND_FACTOR_SRC_COLOR, [0x4] = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    [0x5] = VK_BLEND_FACTOR_SRC_ALPHA, [0x6] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, [0x7] = VK_BLEND_FACTOR_DST_ALPHA, [0x8] = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    [0x9] = VK_BLEND_FACTOR_DST_COLOR, [0xA] = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, [0xB] = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, [0x10] = VK_BLEND_FACTOR_SRC1_COLOR,
    [0x11] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, [0x12] = VK_BLEND_FACTOR_SRC1_ALPHA, [0x13] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, [0x61] = VK_BLEND_FACTOR_CONSTANT_COLOR,
    [0x62] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, [0x63] = VK_BLEND_FACTOR_CONSTANT_ALPHA, [0x64] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
static VkBlendFactor bfac(unsigned v, VkBlendFactor def) { return v && v < 0x65 && (v == 1 || BF[v]) ? BF[v] : def; }
static VkBlendOp bop(unsigned v) { return v >= 1 && v <= 5 ? (VkBlendOp)(v - 1) : VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(unsigned v, VkCompareOp def) { return v >= 1 && v <= 8 ? (VkCompareOp)(v - 1) : def; }
static VkStencilOp stop(unsigned v) { return v >= 1 && v <= 8 ? (VkStencilOp)(v - 1) : VK_STENCIL_OP_KEEP; }
typedef struct {
    vshader *sh[4]; int z, tf; VkRenderPass rp; rp_key rk; uint32_t ncolor;   // tf: the capture pipeline (the vertex shader's .tf.spv, no rasterization); rk: the pass's formats and samples
    uint32_t attr[16]; vs_st stream[16]; uint8_t topo, restart, depth_clamp, ll;
    color_st color; chmask_st chmask; blend_st blend[8]; ms_st ms; poly_st poly; ds_st ds;
} pipe_key;
typedef struct { VkPipeline p; VkPipelineLayout pl; } pipe_t;
static nvn_hmap g_pipes = {.klen = sizeof(pipe_key)};
// ---- Pipelines made again at start ----
// Creating a pipeline costs the Adreno driver about 2 ms (a shader compile; less when its pipeline cache has the binary): Super Mario Bros. Wonder made 122 in one
// frame entering a level (222 ms) and a few every time a new effect showed up. Every pipeline made is appended to <DM_PIPELINE_CACHE>.keys (its shaders' keys, the
// pass's formats and the rest of the key, which is plain state), and a thread makes the listed ones again when the app starts, one at a time under the replay's lock
// (between the game's submissions), so a later session meets them made
typedef struct { uint32_t magic, size; char sh[4][17]; } pk_head;
enum { PK_MAGIC = 0x50574b34 };   // 'PWK4': keys normalized (vk_draw_): blend enables, polygon offset and the state objects' unused bits too
static FILE *g_pk_out;
static nvn_hmap g_pk_listed;   // the records in the list (whole records), so a session appends only pipelines the list does not have yet (it used to grow by thousands of repeats)
static size_t pk_tail_off(void) { return offsetof(pipe_key, ncolor); }
enum { PK_TAIL = sizeof(pipe_key) - offsetof(pipe_key, ncolor), PK_REC = sizeof(pk_head) + 4 + sizeof(rp_key) + PK_TAIL };
static void pk_rec(const pipe_key *k, uint8_t *r) {   // a list record: head, z, the pass's key, the plain tail of the key
    pk_head h; memset(&h, 0, sizeof h); h.magic = PK_MAGIC; h.size = PK_TAIL;
    for (int i = 0; i < 4; i++) if (k->sh[i]) memcpy(h.sh[i], k->sh[i]->key, 17);
    int32_t z = k->z; memcpy(r, &h, sizeof h); memcpy(r + sizeof h, &z, 4); memcpy(r + sizeof h + 4, &k->rk, sizeof k->rk);
    memcpy(r + sizeof h + 4 + sizeof k->rk, (const uint8_t *)k + pk_tail_off(), PK_TAIL);
}
static FILE *pk_open(const char *mode) {   // <DM_PIPELINE_CACHE>.keys4 (earlier lists, .keys2 and .keys3, are removed: their keys are not made any more)
    const char *c = getenv("DM_PIPELINE_CACHE"); if (!c) return NULL; char p[700];
    static int old; if (!old++) for (int v = 2; v <= 3; v++) { snprintf(p, sizeof p, "%s.keys%d", c, v); remove(p); }
    snprintf(p, sizeof p, "%s.keys4", c); return fopen(p, mode); }
static void pk_record(const pipe_key *k) {   // under g_bk
    if (k->tf) return;
    uint8_t r[PK_REC]; pk_rec(k, r);
    if (!g_pk_listed.klen) g_pk_listed.klen = PK_REC;
    if (nvn_hm_get(&g_pk_listed, r)) return;
    nvn_hm_put(&g_pk_listed, r, (void *)1);
    if (!g_pk_out) { static int tried; if (tried++) return; g_pk_out = pk_open("ab"); if (!g_pk_out) return; }
    fwrite(r, PK_REC, 1, g_pk_out); fflush(g_pk_out);
}
static pipe_t *pipe_get(const pipe_key *k);
static VkResult pipe_make(const pipe_key *k, pipe_t *pp, uint32_t *ons, uint32_t *onva, VkPrimitiveTopology *otp);
static void pipe_prep(const pipe_key *k);
static VkRenderPass rp_get(const rp_key *k);
// The list is read whole first (repeats dropped, the file written again without them when it had any), then made one pipeline at a time at a low priority: the shaders and
// the pass under the replay's lock, the compile without it (pipe_make). Compiling under the lock kept the game's thread waiting for seconds in a row (menus at 1-2 s a frame),
// and later, with pauses between pipelines, still for part of every frame in a level while the list ran (10,000 pipelines and more)
static void *prewarm_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-prewarm"); setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    FILE *f = pk_open("rb"); if (!f) return NULL;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); int made = 0, had = 0, bad = 0, n = 0, rep = 0;
    uint8_t *all = NULL; size_t cnt = 0, cap = 0, rec = PK_REC;
    pthread_mutex_lock(&g_bk); if (!g_pk_listed.klen) g_pk_listed.klen = PK_REC; pthread_mutex_unlock(&g_bk);
    for (;;) {
        uint8_t r[PK_REC]; pk_head h;
        if (fread(r, rec, 1, f) != 1) break;
        memcpy(&h, r, sizeof h); if (h.magic != PK_MAGIC || h.size != PK_TAIL) break;
        n++;
        pthread_mutex_lock(&g_bk); int seen = nvn_hm_get(&g_pk_listed, r) != NULL; if (!seen) nvn_hm_put(&g_pk_listed, r, (void *)1); pthread_mutex_unlock(&g_bk);
        if (seen) { rep++; continue; }
        if (cnt == cap) { cap = cap ? cap * 2 : 1024; all = realloc(all, cap * rec); }
        memcpy(all + cnt++ * rec, r, rec);
    }
    fclose(f);
    if (rep) {   // written again without the repeats (records this session appended meanwhile are in g_pk_listed and the new file)
        pthread_mutex_lock(&g_bk);
        if (g_pk_out) { fclose(g_pk_out); g_pk_out = NULL; }
        FILE *o = pk_open("wb"); if (o) { fwrite(all, rec, cnt, o); fclose(o); }
        g_pk_out = pk_open("ab");
        pthread_mutex_unlock(&g_bk);
    }
    // At most the list's first PREWARM_MAX (DM_VK_PREWARM overrides): what every session meets early (menus, the map, common effects). The whole list of a Super Mario Bros.
    // Wonder save grew past 34,000 pipelines over a few sessions; making them all took the phone's two fast cores and about a gigabyte for most of an hour of play
    size_t lim = 3000; { const char *e = getenv("DM_VK_PREWARM"); if (e) lim = (size_t)strtoul(e, NULL, 10); } if (cnt > lim) cnt = lim;
    for (size_t i = 0; i < cnt; i++) {
        const uint8_t *r = all + i * rec; pk_head h; int32_t z; rp_key rk;
        memcpy(&h, r, sizeof h); memcpy(&z, r + sizeof h, 4); memcpy(&rk, r + sizeof h + 4, sizeof rk);
        pthread_mutex_lock(&g_bk);   // the shaders, the pass and what the pipeline shares (pipe_prep), under the lock; the compile without it
        pipe_key k; memset(&k, 0, sizeof k); int ok = 1, mk = 0;
        for (int j = 0; j < 4 && ok; j++) if (h.sh[j][0]) { h.sh[j][16] = 0; k.sh[j] = sh_get(h.sh[j]); if (!k.sh[j]) ok = 0; }
        if (ok) { k.z = z; k.rk = rk; k.rp = rp_get(&rk); memcpy((uint8_t *)&k + pk_tail_off(), r + sizeof h + 4 + sizeof rk, PK_TAIL); if (!k.rp) ok = 0; }
        if (ok) { if (nvn_hm_get(&g_pipes, &k)) had++; else { pipe_prep(&k); mk = 1; } } else bad++;
        pthread_mutex_unlock(&g_bk);
        if (!mk) continue;
        pipe_t *pp = calloc(1, sizeof *pp); uint32_t ns, nva; VkPrimitiveTopology tp;
        VkResult pr = pp ? pipe_make(&k, pp, &ns, &nva, &tp) : VK_ERROR_OUT_OF_HOST_MEMORY;
        pthread_mutex_lock(&g_bk);
        if (pr == VK_SUCCESS && !nvn_hm_get(&g_pipes, &k)) { nvn_hm_put(&g_pipes, &k, pp); made++; pp = NULL; }
        else if (pr == VK_SUCCESS) had++;   // the game made it meanwhile
        else bad++;
        pthread_mutex_unlock(&g_bk);
        if (pp) { if (pp->p) vkDestroyPipeline(g_vk.dev, pp->p, NULL); if (pp->pl) vkDestroyPipelineLayout(g_vk.dev, pp->pl, NULL); free(pp); }
        usleep(1000);
    }
    free(all);
    LOG("[vk] pipelines made again from the list: %d made, %d there already, %d failed, of the first %zu of %d records (%d repeats dropped), in %.1f s", made, had, bad, cnt, n, rep, (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e9);
    // the cache was written only when the app paused: a session that ended otherwise (killed for an update or by the system) lost what the list compiled, and the next start
    // compiled it all again (Super Mario Bros. Wonder on the Adreno 619: about 23 ms a pipeline from the stale cache, 7 from a saved one)
    if (made) vkb_pipeline_cache_save();
    return NULL;
}
static void prewarm_start(void) { static int started; if (started++) return; pthread_t th; if (!pthread_create(&th, NULL, prewarm_thread, NULL)) pthread_detach(th); }
static VkDescriptorSetLayout g_empty_dsl;
// The pipeline of key k into pp (its layout and pipeline). Under the replay's lock from pipe_get; the start-up list (prewarm_thread) calls it without the lock after pipe_prep,
// so a compile there no longer holds the game's draws up (the driver's pipeline cache is internally synchronized)
static VkResult pipe_make(const pipe_key *k, pipe_t *pp, uint32_t *ons, uint32_t *onva, VkPrimitiveTopology *otp) {
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &g_empty_dsl); }
    VkDescriptorSetLayout sets[4] = {g_empty_dsl, g_empty_dsl, g_empty_dsl, g_empty_dsl}; uint32_t nset = 1;
    VkPipelineShaderStageCreateInfo st[4]; uint32_t ns = 0;
    for (int i = 0; i < 4; i++) { vshader *s = k->sh[i]; if (!s) continue;
        sets[s->set & 3] = k->tf ? s->dsl_tf : s->dsl; if ((uint32_t)(s->set & 3) + 1 > nset) nset = (uint32_t)(s->set & 3) + 1;
        VkShaderModule mod = k->tf ? s->modtf : s->stage == SH_VS && k->z && s->modz ? s->modz : s->mod;
        if (s->stage == SH_PS) { const vshader *pv = k->sh[2] ? k->sh[2] : k->sh[0]; uint32_t miss = pv ? s->io_in & ~pv->io_out : 0; if (miss) mod = ps_variant(s, miss); }   // see spv_drop_inputs
        st[ns++] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = s->vkstage, .module = mod, .pName = "main"}; }
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nset, .pSetLayouts = sets};
    if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &pp->pl) != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    VkVertexInputBindingDescription vb[32]; VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDivisorDescriptionEXT vd[32]; uint32_t nvd = 0; uint32_t nvb = 0, nva = 0; uint32_t used = 0;
    for (int i = 0; i < 16; i++) {
        va_st a; memcpy(&a, &k->attr[i], 4);
        if (!a.fmt) continue;
        char t = k->sh[0] ? k->sh[0]->vin[i] : 'f';
        int kind, bits;
        if (vfmt_desc(a.fmt, &kind, &bits) && !vfmt_direct(a.fmt, t)) {   // its register bits, made on the CPU (see vfmt_desc), in a binding of its own
            va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, 16u + (uint32_t)i, t == 'u' ? VK_FORMAT_R32G32B32A32_UINT : t == 's' ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            vb[nvb++] = (VkVertexInputBindingDescription){16u + (uint32_t)i, 16, k->stream[a.stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (k->stream[a.stream].divisor > 1 && g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){16u + (uint32_t)i, k->stream[a.stream].divisor};
            continue;
        }
        VkFormat f = vtx_fmt(a.fmt); if (!f) { RED_ONCE(86, "vertex attribute format %#x not mapped", a.fmt); continue; }
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) RED_ONCE(119, "vertex attribute format %#x (Vulkan %d) is not a vertex format on this GPU: it reads undefined values", a.fmt, f);
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, a.stream, f, a.offset}; used |= 1u << a.stream;
    }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, k->stream[s].stride, k->stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->stream[s].divisor > 1) {
            if (g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){(uint32_t)s, k->stream[s].divisor};
            else RED_ONCE(87, "vertex stream divisor %u > 1 on a GPU without VK_EXT_vertex_attribute_divisor: read as 1", k->stream[s].divisor);
        }
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va};
    VkPrimitiveTopology tp = (VkPrimitiveTopology)k->topo;
    int strip = tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = tp, .primitiveRestartEnable = strip && k->restart};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    int front_ccw = k->poly.front == NVN_FRONT_FACE_CCW; if (k->ll) front_ccw = !front_ccw;   // see the file header: lower-left origin inverts the winding as Vulkan sees it
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = k->tf != 0, .depthClampEnable = k->depth_clamp && g_vk_feat.depthClamp,
        .polygonMode = k->poly.mode == NVN_POLYGON_MODE_LINE && g_vk_feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : k->poly.mode == NVN_POLYGON_MODE_POINT && g_vk_feat.fillModeNonSolid ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
        .cullMode = (VkCullModeFlags)(k->poly.cull & 3), .frontFace = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = k->poly.offset_en != 0, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->rk.samples ? k->rk.samples : 1), .alphaToCoverageEnable = k->ms.a2c != 0};
    VkStencilOpState sf = {stop(k->ds.fail_f), stop(k->ds.zpass_f), stop(k->ds.zfail_f), cmpop(k->ds.sf_f, VK_COMPARE_OP_ALWAYS), 0xff, 0xff, 0};
    VkStencilOpState sb = {stop(k->ds.fail_b), stop(k->ds.zpass_b), stop(k->ds.zfail_b), cmpop(k->ds.sf_b, VK_COMPARE_OP_ALWAYS), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = k->ds.dt, .depthWriteEnable = k->ds.dt && k->ds.dw,
        .depthCompareOp = cmpop(k->ds.df, VK_COMPARE_OP_LESS), .stencilTestEnable = k->ds.st, .front = sf, .back = sb, .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        const blend_st *b = &k->blend[i];
        cba[i] = (VkPipelineColorBlendAttachmentState){(k->color.blend_en >> i & 1) != 0, bfac(b->src_rgb, VK_BLEND_FACTOR_ONE), bfac(b->dst_rgb, VK_BLEND_FACTOR_ZERO), bop(b->eq_rgb),
            bfac(b->src_a, VK_BLEND_FACTOR_ONE), bfac(b->dst_a, VK_BLEND_FACTOR_ZERO), bop(b->eq_a), (VkColorComponentFlags)(k->chmask.mask >> (4 * i) & 0xf)};
        if (b->adv) RED_ONCE(88, "advanced blend mode %u: drawn with plain blending", (unsigned)b->adv);
    }
    VkPipelineColorBlendStateCreateInfo cb_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba,
        .logicOpEnable = k->color.logic_op != NVN_LOGIC_OP_COPY && g_vk_feat.logicOp, .logicOp = (VkLogicOp)(k->color.logic_op & 15)};
    if (k->color.logic_op != NVN_LOGIC_OP_COPY && !g_vk_feat.logicOp) RED_ONCE(89, "logic op %u on a GPU without logic ops: drawn as copy", (unsigned)k->color.logic_op);
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = ns, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb_, .pDynamicState = &dy, .layout = pp->pl, .renderPass = k->rp};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    VkResult pr = vkCreateGraphicsPipelines(g_vk.dev, pcache(), 1, &pc, NULL, &pp->p);
    atomic_fetch_add(&g_pso_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_pso_made, 1);
    *ons = ns; *onva = nva; *otp = tp;
    return pr;
}
static VkShaderModule pipe_ps_mod(const pipe_key *k, vshader *s) {   // the fragment shader's module: a variant when the earlier stage leaves inputs unwritten (spv_drop_inputs)
    const vshader *pv = k->sh[2] ? k->sh[2] : k->sh[0]; uint32_t miss = pv ? s->io_in & ~pv->io_out : 0;
    return miss ? ps_variant(s, miss) : s->mod;
}
static void pipe_prep(const pipe_key *k) {   // under the lock: what pipe_make would otherwise create in shared state
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &g_empty_dsl); }
    for (int i = 0; i < 4; i++) if (k->sh[i] && k->sh[i]->stage == SH_PS && !k->tf) pipe_ps_mod(k, k->sh[i]);
}
static pipe_t *pipe_get(const pipe_key *k) {
    pipe_t *pp = nvn_hm_get(&g_pipes, k); if (pp) return pp->p ? pp : NULL;
    pp = calloc(1, sizeof *pp); nvn_hm_put(&g_pipes, k, pp);
    uint32_t ns, nva; VkPrimitiveTopology tp; VkResult pr = pipe_make(k, pp, &ns, &nva, &tp);
    if (pr == VK_SUCCESS) pk_record(k);
    {   // which fields set a new pipeline apart from the last one with the same shaders and pass (the first 40): key churn
        static struct { vshader *a, *b; VkRenderPass rp; pipe_key k; } last[64]; static int nlog;
        unsigned h = (unsigned)(((uintptr_t)k->sh[0] >> 4) ^ ((uintptr_t)k->sh[1] >> 6) ^ ((uintptr_t)k->rp >> 3)) % 64;
        if (last[h].a == k->sh[0] && last[h].b == k->sh[1] && last[h].rp == k->rp && nlog < 40) {
            static const struct { const char *n; size_t o, s; } F[] = {{"z", offsetof(pipe_key, z), sizeof(int)}, {"tf", offsetof(pipe_key, tf), sizeof(int)}, {"rk", offsetof(pipe_key, rk), sizeof(rp_key)},
                {"ncolor", offsetof(pipe_key, ncolor), 4}, {"attr", offsetof(pipe_key, attr), sizeof(((pipe_key *)0)->attr)}, {"stream", offsetof(pipe_key, stream), sizeof(((pipe_key *)0)->stream)},
                {"topo", offsetof(pipe_key, topo), 1}, {"restart", offsetof(pipe_key, restart), 1}, {"depth_clamp", offsetof(pipe_key, depth_clamp), 1}, {"ll", offsetof(pipe_key, ll), 1},
                {"color", offsetof(pipe_key, color), sizeof(color_st)}, {"chmask", offsetof(pipe_key, chmask), sizeof(chmask_st)}, {"blend", offsetof(pipe_key, blend), sizeof(((pipe_key *)0)->blend)},
                {"ms", offsetof(pipe_key, ms), sizeof(ms_st)}, {"poly", offsetof(pipe_key, poly), sizeof(poly_st)}, {"ds", offsetof(pipe_key, ds), sizeof(ds_st)}};
            char d[300] = ""; for (size_t i = 0; i < sizeof F / sizeof *F; i++) if (memcmp((const uint8_t *)k + F[i].o, (const uint8_t *)&last[h].k + F[i].o, F[i].s)) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, " %s", F[i].n); }
            uint32_t c0, c1, p0, p1; memcpy(&c0, &last[h].k.color, 4); memcpy(&c1, &k->color, 4); memcpy(&p0, &last[h].k.poly, 4); memcpy(&p1, &k->poly, 4);
            nlog++; LOG("[vk] pipeline %s %s again with other:%s (colour %#x -> %#x, polygon %#x -> %#x, %u targets)", k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-", d[0] ? d : " (padding only)",
                c0, c1, p0, p1, k->ncolor); }
        last[h].a = k->sh[0]; last[h].b = k->sh[1]; last[h].rp = k->rp; last[h].k = *k;
    }
    if (pr != VK_SUCCESS) {
        static int n; if (n++ < 16) LOG("[RED] [vk] pipeline creation failed: VkResult %d (shaders %s%s %s; %u stages, %u vertex attributes, %u colour targets, topology %d, %u samples)", pr, k->sh[0] ? k->sh[0]->key : "-",
            k->z && k->sh[0] && k->sh[0]->modz ? " (.z)" : "", k->sh[1] ? k->sh[1]->key : "-", ns, nva, k->ncolor, (int)tp, k->rk.samples);
        pp->p = VK_NULL_HANDLE; return NULL;
    }
    if (vk_debug()) LOG("[vk] pipeline %u for shaders %s %s", g_pipes.n, k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-");
    return pp;
}

// ---------------- Descriptor sets ----------------
static VkDescriptorSet ds_alloc(VkDescriptorSetLayout l) {
    if (!cur()) return VK_NULL_HANDLE;
    for (;;) {
        if (C->dp_cur < C->ndp) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = C->dp[C->dp_cur], .descriptorSetCount = 1, .pSetLayouts = &l};
            VkDescriptorSet s; if (vkAllocateDescriptorSets(g_vk.dev, &ai, &s) == VK_SUCCESS) return s;
            C->dp_cur++; continue;
        }
        VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
        VkDescriptorPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2048, .poolSizeCount = sizeof sz / sizeof *sz, .pPoolSizes = sz};
        if (C->ndp == C->cdp) { C->cdp = C->cdp ? 2 * C->cdp : 4; C->dp = realloc(C->dp, C->cdp * sizeof *C->dp); }
        if (vkCreateDescriptorPool(g_vk.dev, &pc, NULL, &C->dp[C->ndp]) != VK_SUCCESS) { RED_ONCE(90, "descriptor pool creation failed"); return VK_NULL_HANDLE; }
        C->ndp++;
    }
}
static const uint8_t c_bases[6] = {0x11, 0x51, 0x41, 0, 0, 0x31};   // c0 storage-buffer address table per NVN stage (×0x10): vertex 0x110, fragment 0x510, geometry 0x410, compute 0x310
// The driver banks of a stage, made from the bound state: c0 (storage buffer addresses, base instance at 0x644) and c2 (texture, image and sampler handles)
static uint32_t g_cdim[3];   // the current dispatch's group counts (c0 + 0x200 for compute)
static void bank_c0(int nst, uint8_t *b, uint32_t base_inst) {
    memset(b, 0, 0x800);
    if (nst == 5) { memcpy(b + 0x200, g_cdim, 12);   // compute: the group counts, then the uniform buffer table (0x210 + 16 r)
        for (int r = 0; r < 16; r++) { uint64_t a = D.ubo[5][r].addr; uint32_t n = (uint32_t)D.ubo[5][r].size; memcpy(b + 0x210 + 16 * r, &a, 8); memcpy(b + 0x218 + 16 * r, &n, 4); } }
    uint32_t o = (uint32_t)c_bases[nst] << 4;
    for (int r = 0; r < 16 && o && o + 16 * r + 12 <= 0x800; r++) { uint64_t a = D.ssbo[nst][r].addr; uint32_t n = (uint32_t)D.ssbo[nst][r].size; memcpy(b + o + 16 * r, &a, 8); memcpy(b + o + 16 * r + 8, &n, 4); }
    memcpy(b + 0x644, &base_inst, 4);
}
static void bank_c2(int nst, uint8_t *b) {
    memset(b, 0, 0xA00);
    // as nvn_mtl.m c2_handle: combined slots 0..40 (32..39 share the image table: a bound image is what lies there), separate textures, separate samplers
    for (int r = 0; r < 41; r++) { uint32_t h = (uint32_t)(r >= 32 && D.img[nst][r - 32] ? D.img[nst][r - 32] : D.tex[nst][r]); memcpy(b + 0x20 + 8 * r, &h, 4); }
    for (int r = 0; r < 128; r++) { uint32_t h = (uint32_t)D.tex[nst][128 + r]; memcpy(b + 0x168 + 8 * r, &h, 4); }
    for (int r = 0; r < 128; r++) { uint32_t h = (uint32_t)D.samp[nst][r]; memcpy(b + 0x568 + 8 * r, &h, 4); }
}
static int nvn_stage_of(int s) { return s == SH_PS ? 1 : s == SH_GS ? 2 : s == SH_CS ? 5 : 0; }
// a word of a constant bank: c2 as made, or a game uniform buffer (c3 + slot)
static uint32_t bank_word(int nst, const uint8_t *c2, int slot, uint32_t byte) {
    uint32_t w = 0;
    if (slot < 0) slot = 2;   // a bound texture's handle: the translator gives no bank (-1), the driver's c2
    if (slot == 2) { if (byte + 4 <= 0xA00) memcpy(&w, c2 + byte, 4); return w; }
    if (slot >= 3 && slot - 3 < 16) { const p_bind_buf *u = &D.ubo[nst][slot - 3]; if (u->addr && byte + 4 <= u->size) memcpy(&w, (const void *)(uintptr_t)(u->addr + byte), 4); }
    return w;
}
// A buffer texture (target BUFFER) as a texel buffer view over a copy of its guest memory; written (an image) → copied back when the submission completes. NULL: a dummy view
static VkBuffer g_dbuf; static VkDeviceMemory g_dmem; static VkBufferView g_dbv;
static VkBufferView dummy_tbuf(void) {
    if (g_dbv) return g_dbv;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &g_dbuf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_dbuf, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    if (vkAllocateMemory(g_vk.dev, &ma, NULL, &g_dmem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindBufferMemory(g_vk.dev, g_dbuf, g_dmem, 0);
    void *p; if (vkMapMemory(g_vk.dev, g_dmem, 0, 256, 0, &p) == VK_SUCCESS) { memset(p, 0, 256); vkUnmapMemory(g_vk.dev, g_dmem); }   // reads 0
    VkBufferViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = g_dbuf, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = VK_WHOLE_SIZE};
    vkCreateBufferView(g_vk.dev, &vc, NULL, &g_dbv);
    return g_dbv;
}
static VkBufferView tbuf_view(const tex_t *t, int write) {
    uint32_t m = 0; uint8_t swz[4];
    if (!t || t->d.target != NVN_TEXTURE_TARGET_BUFFER || !t->mem || !nvn_fmt_to_mtl(t->d.format, &m, swz) || !m) return VK_NULL_HANDLE;
    VkFormat f = vk_of_mtl(m); int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!f || bpb <= 0) return VK_NULL_HANDLE;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    if (!(fp.bufferFeatures & (write ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) { RED_ONCE(101, "buffer texture format %d (NVN %#x) not usable as a %s texel buffer here", f, t->d.format, write ? "storage" : "uniform"); return VK_NULL_HANDLE; }
    uint64_t n = (uint64_t)(t->d.width ? t->d.width : 1);
    if (n > g_vk.props.limits.maxTexelBufferElements) { RED_ONCE(102, "buffer texture of %llu elements over the GPU's %u: cut", (unsigned long long)n, g_vk.props.limits.maxTexelBufferElements); n = g_vk.props.limits.maxTexelBufferElements; }
    size_t bytes = (size_t)n * (size_t)bpb;
    VkBuffer b; VkDeviceSize off; uint8_t *d = ar_alloc(bytes, &b, &off); if (!d) return VK_NULL_HANDLE;
    int inpool = mtl_addr((uint64_t)(uintptr_t)t->mem, bytes, NULL, NULL);
    if (inpool) memcpy(d, t->mem, bytes); else { memset(d, 0, bytes); RED_ONCE(103, "buffer texture outside every memory pool: reads 0"); }
    if (write) { wb_t *w = wb_add(); uint8_t *snap = malloc(bytes); if (snap) { if (inpool) memcpy(snap, t->mem, bytes); else memset(snap, 0, bytes); }   // from guest memory: the arena is write-combined (slow to read)
        if (w) *w = (wb_t){-1 - C->ar_last, (size_t)off, bytes, 1, bytes, bytes, t->mem, snap}; }
    VkBufferViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = f, .offset = off, .range = bytes};
    VkBufferView v; if (vkCreateBufferView(g_vk.dev, &vc, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (C->nbv == C->cbv) { C->cbv = C->cbv ? 2 * C->cbv : 64; C->bv = realloc(C->bv, C->cbv * sizeof *C->bv); }
    C->bv[C->nbv++] = v;
    return v;
}
// a storage image view of a texture: its own image (created with storage usage for NVN_TEXTURE_FLAGS_IMAGE), at the view's level and layers
// A storage view: the image is viewed in the format the shader declares (Vulkan needs them to match), which must have its texel size; Unknown takes the image's own.
// 0 when it cannot be (the draw is then skipped: a wrong-format storage image is undefined, and can hang the GPU)
// A texture view's own format (view_t set_format): the image viewed in it when it is mutable and the texel sizes match; 0 = the image's
static VkFormat view_format(const vtex *v, const view_t *vv) {
    if (!vv || !vv->set_format) return VK_FORMAT_UNDEFINED;
    uint32_t m = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(vv->format, &m, sw) || !m) return VK_FORMAT_UNDEFINED;
    VkFormat f = vk_of_mtl(m);
    if (!f || f == v->fmt) return VK_FORMAT_UNDEFINED;
    if (!v->mutable_ || !fmt_bytes(f) || fmt_bytes(f) != fmt_bytes(v->fmt)) { RED_ONCE(118, "texture view in format %d over an image of format %d: read in the image's", f, v->fmt); return VK_FORMAT_UNDEFINED; }
    return f;
}
static int g_bind_bad;   // set by stage_set when a binding has no valid descriptor: the draw / dispatch is skipped
static VkImageView storage_view(vtex *v, const view_t *vv, VkImageViewType vt, VkFormat decl) {
    VkFormat f = decl ? decl : v->fmt;
    if (f != v->fmt && (v->conv || !fmt_bytes(v->fmt) || fmt_bytes(v->fmt) != fmt_bytes(f))) { RED_ONCE(110, "image of format %d bound where the shader declares format %d: skipped", v->fmt, f); return VK_NULL_HANDLE; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || v->conv) { RED_ONCE(104, "image format %d is not storage-capable here: skipped", f); return VK_NULL_HANDLE; }
    g_view_fmt = f == v->fmt ? VK_FORMAT_UNDEFINED : f;
    VkImageView iv = view_get(v, vt, vv && vv->set_levels ? vv->base_level : 0, 1, vv && vv->set_layers ? vv->min_layer : 0, vv && vv->set_layers ? vv->nlayers : 0, VK_IMAGE_ASPECT_COLOR_BIT, NULL);
    g_view_fmt = VK_FORMAT_UNDEFINED;
    return iv;
}
static uint32_t nvn_fmt_of(VkFormat f) {   // an NVN format for a dummy of a declared storage format (formats with the same texel layout)
    switch (f) {
        case VK_FORMAT_R8_UNORM: return NVN_FORMAT_R8; case VK_FORMAT_R8_SNORM: return NVN_FORMAT_R8SN; case VK_FORMAT_R8_UINT: return NVN_FORMAT_R8UI; case VK_FORMAT_R8_SINT: return NVN_FORMAT_R8I;
        case VK_FORMAT_R16_SFLOAT: return NVN_FORMAT_R16F; case VK_FORMAT_R16_UNORM: return NVN_FORMAT_R16; case VK_FORMAT_R16_SNORM: return NVN_FORMAT_R16SN; case VK_FORMAT_R16_UINT: return NVN_FORMAT_R16UI; case VK_FORMAT_R16_SINT: return NVN_FORMAT_R16I;
        case VK_FORMAT_R32_SFLOAT: return NVN_FORMAT_R32F; case VK_FORMAT_R32_UINT: return NVN_FORMAT_R32UI; case VK_FORMAT_R32_SINT: return NVN_FORMAT_R32I;
        case VK_FORMAT_R8G8_UNORM: return NVN_FORMAT_RG8; case VK_FORMAT_R8G8_SNORM: return NVN_FORMAT_RG8SN; case VK_FORMAT_R8G8_UINT: return NVN_FORMAT_RG8UI; case VK_FORMAT_R8G8_SINT: return NVN_FORMAT_RG8I;
        case VK_FORMAT_R16G16_SFLOAT: return NVN_FORMAT_RG16F; case VK_FORMAT_R16G16_UNORM: return NVN_FORMAT_RG16; case VK_FORMAT_R16G16_SNORM: return NVN_FORMAT_RG16SN; case VK_FORMAT_R16G16_UINT: return NVN_FORMAT_RG16UI; case VK_FORMAT_R16G16_SINT: return NVN_FORMAT_RG16I;
        case VK_FORMAT_R32G32_SFLOAT: return NVN_FORMAT_RG32F; case VK_FORMAT_R32G32_UINT: return NVN_FORMAT_RG32UI; case VK_FORMAT_R32G32_SINT: return NVN_FORMAT_RG32I;
        case VK_FORMAT_R8G8B8A8_UNORM: return NVN_FORMAT_RGBA8; case VK_FORMAT_R8G8B8A8_SNORM: return NVN_FORMAT_RGBA8SN; case VK_FORMAT_R8G8B8A8_UINT: return NVN_FORMAT_RGBA8UI; case VK_FORMAT_R8G8B8A8_SINT: return NVN_FORMAT_RGBA8I;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return NVN_FORMAT_RGBA16F; case VK_FORMAT_R16G16B16A16_UNORM: return NVN_FORMAT_RGBA16; case VK_FORMAT_R16G16B16A16_SNORM: return NVN_FORMAT_RGBA16SN;
        case VK_FORMAT_R16G16B16A16_UINT: return NVN_FORMAT_RGBA16UI; case VK_FORMAT_R16G16B16A16_SINT: return NVN_FORMAT_RGBA16I;
        case VK_FORMAT_R32G32B32A32_UINT: return NVN_FORMAT_RGBA32UI; case VK_FORMAT_R32G32B32A32_SINT: return NVN_FORMAT_RGBA32I;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return NVN_FORMAT_RGB10A2; case VK_FORMAT_A2B10G10R10_UINT_PACK32: return NVN_FORMAT_RGB10A2UI; case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return NVN_FORMAT_R11G11B10F;
        default: return NVN_FORMAT_RGBA32F;
    }
}
static nvn_hmap g_dummy_st = {.klen = 4};
static VkImageView dummy_storage(VkImageViewType type, VkFormat decl) {   // a 1×1 (×6) storage image in the declared format: writes to an unbound slot land there
    uint32_t nf = nvn_fmt_of(decl); vtex *d = nvn_hm_get(&g_dummy_st, &nf);
    if (!d) {
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x100ull - nf; t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.format = nf;
        t.d.width = t.d.height = 1; t.d.depth = 6; t.d.levels = 1; t.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
        if (!(d = vt_create(&t))) return VK_NULL_HANDLE;
        nvn_hm_put(&g_dummy_st, &nf, d);
    }
    if (type == VK_IMAGE_VIEW_TYPE_3D || type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) { RED_ONCE(111, "unbound 3D / cube image slot: a 2D array stand-in"); type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; }
    return storage_view(d, NULL, type, decl);
}
// a multisampled 1×1 zero texture: an unbound multisample slot (a single-sample view there is an invalid descriptor)
static vtex *g_dummy_ms[2];
static VkImageView dummy_ms(int depth) {
    if (!g_dummy_ms[depth]) {
        VkSampleCountFlags ok = depth ? g_vk.props.limits.sampledImageDepthSampleCounts : g_vk.props.limits.sampledImageColorSampleCounts;
        int n = ok & VK_SAMPLE_COUNT_4_BIT ? 4 : ok & VK_SAMPLE_COUNT_2_BIT ? 2 : 0; if (!n) return VK_NULL_HANDLE;
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x200ull - depth; t.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; t.d.format = depth ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8;
        t.d.width = t.d.height = 1; t.d.depth = 1; t.d.levels = 1; t.d.samples = n;
        if (!(g_dummy_ms[depth] = vt_create(&t))) return VK_NULL_HANDLE;
    }
    return view_get(g_dummy_ms[depth], VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, g_dummy_ms[depth]->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
// A stage's textures and images made current before any descriptor of the draw is written: their uploads read guest memory, so pending write-backs land
// first (cpu_view), and that must not close the chunk after this draw has added write-backs of its own
static void stage_textures(vshader *s) {
    int nst = nvn_stage_of(s->stage);
    {
        uint8_t c2p[0xA00]; bank_c2(nst, c2p);
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? s->nim : s->ntx); i++) {
            const rb_tex *t = pass ? &s->im[i] : &s->tx[i]; if ((t->type & 0xff) == 2) continue;
            int cnt = t->array > 1 ? (t->array > 1024 ? 1024 : t->array) : 1; uint32_t w0 = (uint32_t)t->handle & 0x3fff;
            for (int e = 0; e < cnt; e++) { int tid = (int)(bank_word(nst, c2p, t->cbuf_slot, (w0 + 2u * e) * 4) & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                pthread_mutex_lock(&g_reg_mu); tex_t tt; int ok = tid > 0 && tid < g_ntreg && g_treg[tid].ok; if (ok) tt = g_treg[tid].t; pthread_mutex_unlock(&g_reg_mu);
                if (ok) vt_get(&tt); }
        }
    }
}
// A draw's copy of a constant or storage buffer reuses the copy an earlier draw of the same chunk made of the same bytes: same source and size, and the source still
// equal to a shadow kept in host memory (the arena is write-combined: it is not read back). Super Mario Bros. Wonder's scene draws read six or seven uniform buffers
// each, mostly the same from draw to draw; copying them again for every draw was the largest cost of its render thread on an Adreno 619 (memcpy, 15%)
typedef struct { const void *src; size_t n; uint64_t seq; VkBuffer b; VkDeviceSize off; uint8_t *shadow; size_t cap; } ucopy_t;
static ucopy_t g_ucopy[1024]; static uint64_t g_ucopy_hit, g_ucopy_miss;
static int ucopy(const void *src, size_t n, size_t al, VkBuffer *b, VkDeviceSize *off) {   // src NULL: al zero bytes (not cached)
    if (!src) { uint8_t *d = ar_alloc(al, b, off); if (!d) return 0; memset(d, 0, al); return 1; }
    uintptr_t h = ((uintptr_t)src >> 4) ^ ((uintptr_t)src >> 14) ^ n * 0x9e37u; ucopy_t *e = &g_ucopy[h & 1023];
    if (C && e->src == src && e->n == n && e->seq == C->seq && !memcmp(src, e->shadow, n)) { *b = e->b; *off = e->off; g_ucopy_hit++; return 1; }
    uint8_t *d = ar_alloc(al, b, off); if (!d) return 0;
    memcpy(d, src, n); if (al > n) memset(d + n, 0, al - n);
    g_ucopy_miss++;
    if (!C) return 1;
    if (e->cap < n) { free(e->shadow); e->shadow = malloc(n); e->cap = e->shadow ? n : 0; }
    if (e->shadow) { memcpy(e->shadow, src, n); e->src = src; e->n = n; e->seq = C->seq; e->b = *b; e->off = *off; } else e->src = NULL;
    return 1;
}
static int stage_set(vshader *s, VkDescriptorSet set, const prog_t *pr, int prog_stage, uint32_t base_inst) {
    int nst = nvn_stage_of(s->stage);
    // one write per binding; every element of an array is written (an unwritten descriptor is undefined)
    VkWriteDescriptorSet wr[1 + 18 + 16 + 64 + 16]; VkDescriptorBufferInfo bi[1 + 18 + 16]; VkDescriptorImageInfo *ii; VkBufferView *bvs; uint32_t nw = 0, nbi = 0, nii = 0, nbv = 0;
    uint8_t c2[0xA00]; bank_c2(nst, c2);
    // support buffer: render scale 1, the viewport's inverse and size
    VkBuffer b; VkDeviceSize off; static uint8_t p[1392];
    float vw = D.has_vp ? D.vp[0][2] : (float)P.w, vh = D.has_vp ? D.vp[0][3] : (float)P.h;
    float vinv[4] = {vw ? 2.f / vw : 0, vh ? 2.f / vh : 0, 0, 0}, vsz[4] = {vw, vh, 1, 0}; memcpy(p + 144, vinv, 16); memcpy(p + 160, vsz, 16);
    { static int once; if (!once++) for (int i = 0; i < 73; i++) { float one = 1.f; memcpy(p + 192 + 16 * i, &one, 4); } }
    if (!ucopy(p, 1392, 1392, &b, &off)) return 0;
    bi[nbi] = (VkDescriptorBufferInfo){b, off, 1392}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi++], NULL};
    for (int i = 0; i < s->ncb; i++) {
        int slot = s->cb[i].slot; size_t n = 16; const void *src = NULL; uint8_t bank[0xA00];
        if (slot == 0) { bank_c0(nst, bank, base_inst); src = bank; n = 0x800; }
        else if (slot == 2) { src = c2; n = 0xA00; }
        else if (slot == 1) {   // the shader's own constant segment, in its code memory (control +0x700: its offset in the code, +0x704: the code's length)
            const NVNshaderData *sd = pr && prog_stage >= 0 ? &pr->sd[prog_stage] : NULL; uint32_t o = 0, e = 0;
            if (sd && sd->control) { memcpy(&o, (const uint8_t *)sd->control + 0x700, 4); memcpy(&e, (const uint8_t *)sd->control + 0x704, 4); }
            if (sd && sd->data && e > o && e - o <= 0x10000) { src = (const void *)(uintptr_t)(sd->data + o); n = e - o; }
        } else if (slot >= 3 && slot - 3 < 16) { const p_bind_buf *u = &D.ubo[nst][slot - 3];
            if (u->addr && u->size) { src = (const void *)(uintptr_t)u->addr; n = u->size > 0x10000 ? 0x10000 : (size_t)u->size;
                size_t rd = s->cb[i].ext && n > s->cb[i].ext ? s->cb[i].ext : n;   // in a memory pool still, as storage buffers below (else it reads 0)
                if (!mtl_addr(u->addr, rd, NULL, NULL)) { RED_ONCE(119, "uniform buffer slot %d of shader %s at %#llx is in no memory pool: it reads 0", slot - 3, s->key, (unsigned long long)u->addr); src = NULL; n = 16; } }
            else { static int nlog; if (nlog++ < 8) LOG("[RED] [vk] shader %s reads uniform slot %d, which has no buffer bound: it reads 0", s->key, slot - 3); } }
        if (s->cb[i].ext && n > s->cb[i].ext) n = s->cb[i].ext;   // only what the shader reads (spv_cb_ext)
        size_t al = (n + 15) & ~(size_t)15; if (!ucopy(src, n, al, &b, &off)) return 0;
        bi[nbi] = (VkDescriptorBufferInfo){b, off, al}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)s->cb[i].binding, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi++], NULL};
    }
    for (int i = 0; i < s->nsb; i++) {   // the address in c0 at the translator's offset names one of the bound storage buffers
        // where the address is (dmsh_build.py reflect): c0's storage buffer table of the stage, compute's uniform buffer table (c0 + 0x210 + 16 r: a uniform buffer read through
        // global memory), or the game's own uniform buffer (c3 + slot: {address, size} at the offset); cb_offset counts words
        const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off, base = (uint32_t)c_bases[nst] << 4;
        p_bind_buf tmp = {0}; const p_bind_buf *u = NULL;
        if (sb->cb_slot == 0 && nst == 5 && off >= 0x210 && off < 0x310 && !((off - 0x210) % 16)) u = &D.ubo[5][(off - 0x210) / 16];
        else if (sb->cb_slot == 0 && base && off >= base && off - base < 0x100 && !((off - base) % 16)) u = &D.ssbo[nst][(off - base) / 16];
        else if (sb->cb_slot >= 3 && sb->cb_slot - 3 < 16) { const p_bind_buf *g = &D.ubo[nst][sb->cb_slot - 3];
            if (g->addr && off + 12 <= g->size) { uint64_t ad; uint32_t sz; memcpy(&ad, (const void *)(uintptr_t)(g->addr + off), 8); memcpy(&sz, (const void *)(uintptr_t)(g->addr + off + 8), 4);
                if (ad && sz && mtl_addr(ad, sz, NULL, NULL)) { tmp.addr = ad; tmp.size = sz; u = &tmp; } } }
        size_t n = u && u->addr ? (size_t)u->size : 16;
        // a binding whose range is in no memory pool any more (the game finalized the pool and left the binding): the GPU would find no pages there, the replay's copy of it
        // faulted (Super Mario Bros. Wonder on Android: SIGSEGV in memcpy from stage_set); read as unbound, as copies and indirect arguments outside every pool are
        if (u && u->addr && !mtl_addr(u->addr, n, NULL, NULL)) { RED_ONCE(118, "storage buffer of shader %s at %#llx (%zu bytes) is in no memory pool: it reads 0", s->key, (unsigned long long)u->addr, n); u = NULL; n = 16; }
        if (!u || !u->addr) RED_ONCE(91, "storage buffer of shader %s (c%d +%#x) is not a bound buffer: it reads 0", s->key, sb->cb_slot, off);
        VkDeviceSize ao;   // ao: the arena offset (off above is the c0 / uniform buffer word)
        if (u && u->addr && !(sb->flags & 1)) { if (!ucopy((const void *)(uintptr_t)u->addr, n, (n + 15) & ~(size_t)15, &b, &ao)) return 0; }   // read only: shared as uniforms are
        else if (u && u->addr) { uint8_t *d = ar_alloc((n + 15) & ~(size_t)15, &b, &ao); if (!d) return 0; memcpy(d, (const void *)(uintptr_t)u->addr, n);
            if (sb->flags & 1) {   // written by the shader: what it changed is copied back when the chunk completes
                wb_t *w = wb_add(); uint8_t *snap = malloc(n); if (snap) memcpy(snap, (const void *)(uintptr_t)u->addr, n);   // from guest memory, as d was: the arena is write-combined
                if (w) { *w = (wb_t){-1 - C->ar_last, (size_t)ao, n, 1, n, n, (uint8_t *)(uintptr_t)u->addr, snap}; w->fwd = s->stage == SH_CS; } } }
        else if (!ucopy(NULL, 16, 16, &b, &ao)) return 0;
        bi[nbi] = (VkDescriptorBufferInfo){b, ao, (n + 15) & ~(size_t)15}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)sb->binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL};
    }
    size_t nel = 0; for (int i = 0; i < s->ntx; i++) nel += s->tx[i].array > 1 ? (size_t)(s->tx[i].array > 1024 ? 1024 : s->tx[i].array) : 1;
    for (int i = 0; i < s->nim; i++) nel += s->im[i].array > 1 ? (size_t)(s->im[i].array > 1024 ? 1024 : s->im[i].array) : 1;
    ii = malloc((nel + 1) * sizeof *ii); bvs = malloc((nel + 1) * sizeof *bvs);
    if (!ii || !bvs) { free(ii); free(bvs); return 0; }   // the texture and image descriptors (no early return past here)
    pthread_mutex_lock(&g_reg_mu);
    for (int i = 0; i < s->ntx; i++) {
        const rb_tex *t = &s->tx[i]; int cnt = t->array > 1 ? (t->array > 1024 ? 1024 : t->array) : 1;
        if (t->array > 1024) RED_ONCE(92, "texture array of %d elements: the first 1024 are bound", t->array);
        VkImageViewType vt = view_type_of(t->type); int tbuf = (t->type & 0xff) == 2;
        uint32_t w0 = (uint32_t)t->handle & 0x3fff, w1 = ((uint32_t)t->handle >> 14) & 0x3fff, ty = (uint32_t)t->handle >> 28;
        if (tbuf) {   // a buffer texture: a uniform texel buffer
            for (int e = 0; e < cnt; e++) {
                uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4); int tid = (int)(hw & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                VkBufferView v = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? tbuf_view(&g_treg[tid].t, 0) : VK_NULL_HANDLE;
                bvs[nbv + e] = v ? v : dummy_tbuf();
            }
            wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &bvs[nbv]};
            nbv += cnt; continue;
        }
        for (int e = 0; e < cnt; e++) {
            uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4), sw = hw;
            if (ty == 1) sw = bank_word(nst, c2, t->samp_slot, w1 * 4);
            int ubo = t->cbuf_slot >= 3, subo = (ty == 1 ? t->samp_slot : t->cbuf_slot) >= 3;
            int tid = (int)(hw & 0xfffff); if (tid && ubo) tid += D.tex_base;
            int sid = ty == 2 ? (int)w1 : (int)(sw >> 20); if (sid && subo && ty != 2) sid += D.samp_base;
            VkImageView iv = VK_NULL_HANDLE;
            int ms = (t->type & 0x200) != 0;
            if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) {
                treg_t *rg = &g_treg[tid]; vtex *v = vt_get(&rg->t);
                if (v && (v->samples > 1) != ms) { RED_ONCE(112, "a %s texture in a %s slot (shader %s): reads 0", v->samples > 1 ? "multisample" : "single-sample", ms ? "multisample" : "single-sample", s->key); v = NULL; }
                if (v) { const view_t *vv = rg->has_v ? &rg->v : NULL;
                    uint8_t swz[4]; int has_swz = rg->t.set_swz || (vv && vv->set_swizzle);
                    if (has_swz) { uint16_t sz = vv && vv->set_swizzle ? (uint16_t)(vv->swz[0] | vv->swz[1] << 3 | vv->swz[2] << 6 | vv->swz[3] << 9) : rg->t.swz; for (int q = 0; q < 4; q++) swz[q] = (sz >> (3 * q)) & 7; }
                    g_view_fmt = view_format(v, vv);
                    iv = view_get(v, vt, vv && vv->set_levels ? vv->base_level : 0, vv && vv->set_levels ? vv->nlevels : 0, vv && vv->set_layers ? vv->min_layer : 0,
                                  vv && vv->set_layers ? vv->nlayers : 0, v->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, has_swz ? swz : NULL);
                    g_view_fmt = VK_FORMAT_UNDEFINED; }
            }
            if (!iv) iv = ms ? dummy_ms((t->type & 0x400) != 0) : dummy_view(vt);
            if (!iv) { g_bind_bad = 1; RED_ONCE(113, "no stand-in for an unbound multisample slot on this GPU: skipped"); iv = dummy_view(vt); }
            const samp_par *sp = sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? &g_sreg[sid] : NULL;
            if (dump_on() && e < 4) { const vtex *dv = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? nvn_hm_get(&g_tex, &g_treg[tid].t.serial) : NULL;
                LOG("[dump]   %s binding %d[%d] type %#x tid %d sid %d -> %s s%llu %ux%ux%u layers %u levels %u vk%d pkg %d", s->key, t->binding, e, t->type, tid, sid, dv ? "tex" : "none",
                    dv ? (unsigned long long)dv->serial : 0ull, dv ? dv->w : 0, dv ? dv->h : 0, dv ? dv->d : 0, dv ? dv->layers : 0, dv ? dv->levels : 0, dv ? dv->fmt : 0, dv ? dv->pkg : 0); }
            ii[nii + e] = (VkDescriptorImageInfo){samp_get(sp, (t->type & 0x400) != 0), iv, VK_IMAGE_LAYOUT_GENERAL};
        }
        wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[nii], NULL, NULL};
        nii += cnt;
    }
    for (int i = 0; i < s->nim; i++) {   // images (load / store / atomics): the handle names a texture pool entry, as a texture's does (hle_nvn.c BindImages)
        const rb_tex *t = &s->im[i]; int cnt = t->array > 1 ? (t->array > 1024 ? 1024 : t->array) : 1, tbuf = (t->type & 0xff) == 2;
        VkImageViewType vt = view_type_of(t->type); uint32_t w0 = (uint32_t)t->handle & 0x3fff;
        for (int e = 0; e < cnt; e++) {
            uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4); int tid = (int)(hw & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
            treg_t *rg = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
            if (tbuf) { VkBufferView v = rg ? tbuf_view(&rg->t, 1) : VK_NULL_HANDLE; bvs[nbv + e] = v ? v : dummy_tbuf(); continue; }
            VkImageView iv = VK_NULL_HANDLE; vtex *v = rg ? vt_get(&rg->t) : NULL;
            if (v) { iv = storage_view(v, rg->has_v ? &rg->v : NULL, vt, t->fmt); if (!iv) g_bind_bad = 1; }
            if (!iv) iv = dummy_storage(vt, t->fmt);
            if (!iv) g_bind_bad = 1;
            ii[nii + e] = (VkDescriptorImageInfo){VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_GENERAL};
        }
        if (tbuf) { wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &bvs[nbv]}; nbv += cnt; }
        else { wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[nii], NULL, NULL}; nii += cnt; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    vkUpdateDescriptorSets(g_vk.dev, nw, wr, 0, NULL);
    free(ii); free(bvs);
    return 1;
}

// Data a compute dispatch of the current chunk wrote to a storage buffer, read by a later draw as a vertex stream (Super Mario Bros. Wonder: a few hundred bytes of
// vertices made by a compute shader, about once a frame): the draw binds the dispatch's own arena copy instead of waiting for the chunk to complete and copying it
// back (that sync emptied the GPU queue every frame). The dispatch ends with a full memory barrier (vk_dispatch cb_barrier). Only when the write-back alone covers the
// stream's bytes, no queued copy writes them, and every attribute of the stream is read as it is (a converted one needs the bytes on the CPU)
static int g_fwd_nwb = -1; static uint64_t g_fwd;   // the chunk's write-backs counted when the draw began (its own come after); streams bound so
// chunk c's newest write to [lo, hi): 1 when one decides (*res: whether it can be read on the GPU, *b / *o where), 0 when the chunk does not write the range
static int fwd_hit(chunk *c, int nwb, uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o, int *res) {
    for (int k = 0; k < c->ncop; k++) if (c->cop[k].dst < hi && lo < c->cop[k].dst + c->cop[k].n) { *res = 0; return 1; }
    for (int i = nwb - 1; i >= 0; i--) { const wb_t *w = &c->wb[i]; if (!wb_hits(w, lo, hi)) continue;
        *res = w->fwd && w->st < 0 && w->rows == 1 && lo >= (uintptr_t)w->dst && hi <= (uintptr_t)w->dst + w->row_bytes;
        if (*res) { *b = c->ar[-1 - w->st].b; *o = (VkDeviceSize)w->off + (VkDeviceSize)(lo - (uintptr_t)w->dst); }
        return 1; }
    return 0;
}
// the current chunk first (its write-backs from before this draw), then the chunks in flight, newest first. A chunk in flight whose arena is read is held: it is
// not recorded again until the reading chunk completes (chunk_thread), so its memory stays as the dispatch left it
static int fwd_find(uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o) {
    if (!C || hi <= lo) return 0;
    int n = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb, r = 0;
    if (fwd_hit(C, n, lo, hi, b, o, &r)) return r;
    if (!atomic_load(&g_inflight_wb) && !atomic_load(&g_inflight_cop)) return 0;
    pthread_mutex_lock(&g_cmu);
    for (int f = g_fifo_n - 1; f >= 0; f--) { chunk *w = g_fifo[f]; int nw = w->nwb;
        if (fwd_hit(w, nw, lo, hi, b, o, &r)) { if (r && w->hold < C->seq) w->hold = C->seq; break; } }
    pthread_mutex_unlock(&g_cmu);
    return r;
}
static int stream_direct(const vshader *vs, int s) {   // every attribute of stream s bound as it is (vfmt_direct)
    for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (a.fmt && a.stream == s && !vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f')) return 0; }
    return 1;
}
static int stream_fwd(const vshader *vs, int s, VkBuffer *b, VkDeviceSize *o) {
    if (vk_safe()) return 0;
    const NVNbufferRange *r = &D.vbuf[s]; if (!r->address || !r->size || !stream_direct(vs, s)) return 0;
    return fwd_find((uintptr_t)r->address, (uintptr_t)(r->address + r->size), b, o);
}
// ---------------- The draw ----------------
// vertex data: each stream's bytes from vertex lo to the last vertex (or instance) the draw reads; the bindings start at vertex lo, so the draw numbers its vertices
// from lo (firstVertex / vertexOffset less lo). Copying from vertex 0 instead made a draw late in a large vertex buffer copy (and convert) all of it before its
// vertices: Hades II draws its sprites from one growing buffer, and its render thread spent 61% of its time here (bind_vbufs and memcpy) at about 3 fps
static int bind_vbufs(VkCommandBuffer c, const vshader *vs, int64_t lo, int64_t vhi, uint32_t base_inst, uint32_t inst) {
    if (lo < 0) lo = 0;
    uint32_t used = 0; for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (a.fmt) used |= 1u << a.stream; }
    if (dump_on()) for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (!a.fmt) continue;
        const NVNbufferRange *r = &D.vbuf[a.stream]; uint32_t st = D.stream[a.stream].stride; char hx[200] = "";
        for (int v = 0; v < 4; v++) { size_t l = strlen(hx); uint64_t at = (uint64_t)v * st + a.offset; float f[2] = {0, 0};
            if (r->address && at + 8 <= r->size) memcpy(f, (const void *)(uintptr_t)(r->address + at), 8); snprintf(hx + l, sizeof hx - l, " (%g %g)", f[0], f[1]); }
        LOG("[dump]   attr %d fmt %#x stream %d offset %d stride %u divisor %u vin %c direct %d size %llu as f32:%s", i, a.fmt, a.stream, a.offset, st, D.stream[a.stream].divisor,
            vs ? vs->vin[i] : '?', vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f'), (unsigned long long)r->size, hx); }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        const NVNbufferRange *r = &D.vbuf[s]; uint32_t stride = D.stream[s].stride; uint64_t need = r->size;
        if (stride) { uint32_t dv = D.stream[s].divisor; int64_t last = dv ? (int64_t)base_inst + (dv > 1 && g_divisor ? (inst + dv - 1) / dv : inst) : vhi; uint64_t want = (uint64_t)(last > 0 ? last : 0) * stride + 64; if (want < need) need = want; }
        uint64_t from = D.stream[s].divisor ? 0 : (uint64_t)lo * stride; if (from > need) from = need;   // instanced streams count from base_inst, not lo
        VkBuffer vb; VkDeviceSize vo;
        if (stream_fwd(vs, s, &vb, &vo)) { g_fwd++; vo += from; vkCmdBindVertexBuffers(c, (uint32_t)s, 1, &vb, &vo); continue; }   // on the GPU: fwd_find
        uint8_t *fill = NULL; gc_ent *ge = r->address ? gc_get((uintptr_t)r->address, r->size, 0, r->size, &fill) : NULL;   // the whole buffer kept, bound at the draw's first vertex
        if (ge) {
            if (fill) memcpy(fill, (const void *)(uintptr_t)r->address, r->size);
            vb = gc_buf(ge); vo = ge->off + from; vkCmdBindVertexBuffers(c, (uint32_t)s, 1, &vb, &vo); continue;
        }
        g_gc_copy++;
        uint64_t n = need - from; uint8_t *d = ar_alloc(n ? n : 16, &vb, &vo); if (!d) return 0;
        if (r->address && n) memcpy(d, (const void *)(uintptr_t)(r->address + from), n); else memset(d, 0, 16);
        vkCmdBindVertexBuffers(c, (uint32_t)s, 1, &vb, &vo);
    }
    for (int i = 0; i < 16; i++) {   // attributes as register bits (vfmt_desc): each vertex (or instance) of the range, from its stream
        va_st a; memcpy(&a, &D.attr[i], 4); int kind, bits, cn = a.fmt ? vfmt_desc(a.fmt, &kind, &bits) : 0;
        if (!cn || vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f')) continue;
        const NVNbufferRange *r = &D.vbuf[a.stream]; uint32_t stride = D.stream[a.stream].stride, dv = D.stream[a.stream].divisor;
        int64_t last = dv ? (int64_t)base_inst + (dv > 1 && g_divisor ? (inst + dv - 1) / dv : inst) : vhi; if (last < 1) last = 1;
        if (!stride) last = 1;
        int bytes = kind >= 7 ? 4 : cn * bits / 8;
        int64_t v0 = dv || !stride ? 0 : lo < last ? lo : last - 1;   // the first vertex converted: the binding's vertex 0
        VkBuffer vb; VkDeviceSize vo;
        // the whole buffer converted once and kept (gc_get): every element the buffer holds, bound at v0
        uint64_t nall = !r->address ? 0 : !stride ? 1 : r->size >= (uint64_t)a.offset + (uint64_t)bytes ? (r->size - a.offset - bytes) / stride + 1 : 0;
        if (nall && (uint64_t)last <= nall) {
            uint8_t *fill = NULL; gc_ent *ge = gc_get((uintptr_t)r->address, r->size, 1ull << 63 | (uint64_t)a.fmt | (uint64_t)a.offset << 16 | (uint64_t)stride << 32, nall * 16, &fill);
            if (ge) {
                if (fill) for (uint64_t v = 0; v < nall; v++) vfmt_regs(kind, bits, cn, (const uint8_t *)(uintptr_t)(r->address + v * stride + a.offset), (uint32_t *)fill + v * 4);
                vb = gc_buf(ge); vo = ge->off + (VkDeviceSize)v0 * 16; vkCmdBindVertexBuffers(c, 16u + (uint32_t)i, 1, &vb, &vo); continue;
            }
        }
        uint32_t *d = (uint32_t *)ar_alloc((VkDeviceSize)(last - v0) * 16, &vb, &vo); if (!d) return 0;
        for (int64_t v = v0; v < last; v++) {
            uint64_t at = (uint64_t)v * stride + a.offset; uint32_t *o = d + (v - v0) * 4;
            if (r->address && at + (uint64_t)bytes <= r->size) vfmt_regs(kind, bits, cn, (const uint8_t *)(uintptr_t)(r->address + at), o);
            else memset(o, 0, 16);
        }
        vkCmdBindVertexBuffers(c, 16u + (uint32_t)i, 1, &vb, &vo);
    }
    return 1;
}
static uint64_t g_captures;
static VkRenderPass g_null_rp; static VkFramebuffer g_null_fb;
static int tf_capture(vshader *vs, const prog_t *pr, int pst, const p_draw *dw, const p_draw_idx *di) {
    if (!D.tf_on || !vs->modtf) { if (D.tf_on) RED_ONCE(114, "transform feedback with vertex shader %s, which has no capture variant: nothing captured", vs->key); return 1; }
    if (!g_vk_feat.vertexPipelineStoresAndAtomics) { RED_ONCE(115, "transform feedback: the GPU has no vertex-stage storage writes"); return 0; }
    if (!dw || dw->instances > 1 || dw->prim == 6 || dw->prim == 7) { RED_ONCE(116, "transform feedback draw that is indexed, instanced or a fan / quad list (vertex %s): not captured", vs->key); return 0; }
    if (dw->count <= 0) return 1;
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        uint64_t n = (uint64_t)dw->count * vs->tf_stride[b];
        if (!D.tf[b].addr || D.tf_off[b] + n > D.tf[b].size || !mtl_addr(D.tf[b].addr + D.tf_off[b], n, NULL, NULL)) {
            RED_ONCE(117, "transform feedback buffer %d of vertex %s is unbound, too small or in no memory pool: not captured", b, vs->key); return 0; }
    }
    if (!g_null_rp) {   // a pass and framebuffer without attachments: the capture rasterizes nothing
        VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
        VkRenderPassCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &sp};
        if (vkCreateRenderPass(g_vk.dev, &rc, NULL, &g_null_rp) != VK_SUCCESS) return 0;
        VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_null_rp, .width = 1, .height = 1, .layers = 1};
        if (vkCreateFramebuffer(g_vk.dev, &fc, NULL, &g_null_fb) != VK_SUCCESS) return 0;
    }
    stage_textures(vs);
    pipe_key k; memset(&k, 0, sizeof k); k.sh[0] = vs; k.tf = 1; k.rp = g_null_rp; k.topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; k.poly = D.poly; k.poly.cull = 0; k.color = D.color; k.color.logic_op = NVN_LOGIC_OP_COPY;   // no colour target: blending and logic ops do not apply
    memcpy(k.attr, D.attr, sizeof k.attr); memcpy(k.stream, D.stream, sizeof k.stream);
    pipe_t *pp = pipe_get(&k); if (!pp) return 0;
    g_drawing = 1;
    g_bind_bad = 0; VkDescriptorSet set = ds_alloc(vs->dsl_tf); if (!set || !stage_set(vs, set, pr, pst, (uint32_t)dw->base_instance) || g_bind_bad) return 0;
    VkDescriptorBufferInfo bi[4]; VkWriteDescriptorSet wr[4]; uint32_t nw = 0;
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {   // vertex i of the draw writes at i × stride from the write offset; copied back whole
        uint64_t n = (uint64_t)dw->count * vs->tf_stride[b]; VkBuffer buf; VkDeviceSize off; uint8_t *d = ar_alloc(n, &buf, &off); if (!d) return 0;
        uint8_t *dst = (uint8_t *)(uintptr_t)(D.tf[b].addr + D.tf_off[b]); memcpy(d, dst, n);
        wb_t *w = wb_add(); if (w) *w = (wb_t){-1 - C->ar_last, (size_t)off, (size_t)n, 1, (size_t)n, (size_t)n, dst, NULL};
        bi[nw] = (VkDescriptorBufferInfo){buf, off, n}; wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 100u + (uint32_t)b, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nw], NULL}; nw++;
        D.tf_off[b] += n;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, wr, 0, NULL);
    VkCommandBuffer c = cb(); if (!c) return 0;   // ends the targets' pass
    VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_null_rp, .framebuffer = g_null_fb, .renderArea = {{0, 0}, {1, 1}}};
    vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->p);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl, 0, 1, &set, 0, NULL);
    VkViewport v = {0, 0, 1, 1, 0, 1}; VkRect2D sc = {{0, 0}, {1, 1}}; vkCmdSetViewport(c, 0, 1, &v); vkCmdSetScissor(c, 0, 1, &sc);
    int ok = bind_vbufs(c, vs, dw->first, (int64_t)dw->first + dw->count, (uint32_t)dw->base_instance, 1);
    if (ok) vkCmdDraw(c, (uint32_t)dw->count, 1, 0, (uint32_t)dw->base_instance);   // gl_VertexIndex counts from 0: the vertex buffers start at the first vertex
    vkCmdEndRenderPass(c); cb_barrier();
    g_drawing = 0;
    { static int once; if (!once++) LOG("[vk] transform feedback: first capture (vertex %s, %d vertices)", vs->key, dw->count); }
    g_captures++;
    return ok;
}
static uint64_t g_draws, g_draws_dropped;
static int vk_draw_(const p_draw *dw, const p_draw_idx *di);
static int ind_read(uint64_t addr, void *out, size_t n) {   // indirect arguments (and multi-draw counts), read on the CPU once the GPU's writes have landed
    if (!addr || !mtl_addr(addr, n, NULL, NULL)) { RED_ONCE(105, "indirect arguments at %#llx are in no memory pool: the draw is skipped", (unsigned long long)addr); return 0; }
    memcpy(out, (const void *)(uintptr_t)addr, n); return 1;
}
static int vk_draw__(const p_draw *dw, const p_draw_idx *di) { int r = vk_draw_(dw, di); g_drawing = 0; g_fwd_nwb = -1; return r; }
// What a draw copies from guest memory lands first when an earlier draw, dispatch or copy wrote it and that write is still pending (gfx_vk.c ranges_view):
// the vertex streams, the index buffer, each stage's uniform slots and storage buffers. A buffer texture or image reads memory found only in the texture
// pool, so a shader with one syncs on anything pending (cpu_view). Called before anything of the draw is recorded
static void draw_view(vshader *const sh[4], const p_draw_idx *di, int vtx) {   // vtx: the vertex streams too (a draw; 0 for a dispatch)
    if (vk_safe()) { g_fwd_nwb = 0; cpu_view(); return; }
    rd_rng r[16 + 1 + 4 * 32 + 4 * 32]; char kd[16 + 1 + 4 * 32 + 4 * 32]; char K = 'v'; int n = 0;
    #define RNG(a, s) do { uintptr_t a_ = (uintptr_t)(a); uint64_t s_ = (s); if (a_ && s_ && n < (int)(sizeof r / sizeof *r)) { kd[n] = K; r[n++] = (rd_rng){a_, a_ + (uintptr_t)s_}; } } while (0)
    g_fwd_nwb = C ? C->nwb : 0;
    if (vtx) for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); VkBuffer fb; VkDeviceSize fo;
        if (a.fmt && !stream_fwd(sh[0], a.stream, &fb, &fo)) RNG(D.vbuf[a.stream].address, D.vbuf[a.stream].size); }
    K = 'i'; if (di && di->ibuf) RNG(di->ibuf, (uint64_t)(di->count > 0 ? di->count : 0) * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) + 4);
    for (int k = 0; k < 4; k++) { const vshader *s = sh[k]; if (!s) continue; int nst = nvn_stage_of(s->stage);
        for (int i = 0; i < s->ntx; i++) if ((s->tx[i].type & 0xff) == 2) { g_vs = 5; cpu_view(); return; }
        for (int i = 0; i < s->nim; i++) if ((s->im[i].type & 0xff) == 2) { g_vs = 6; cpu_view(); return; }
        K = 'u'; for (int i = 0; i < s->ncb; i++) { int sl = s->cb[i].slot; if (sl >= 3 && sl - 3 < 16) RNG(D.ubo[nst][sl - 3].addr, D.ubo[nst][sl - 3].size); }
        K = 's'; for (int i = 0; i < s->nsb; i++) { const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off, base = (uint32_t)c_bases[nst] << 4;
            if (sb->cb_slot == 0 && nst == 5 && off >= 0x210 && off < 0x310 && !((off - 0x210) % 16)) { const p_bind_buf *u = &D.ubo[5][(off - 0x210) / 16]; RNG(u->addr, u->size); }   // compute: its uniform buffer table (stage_set)
            else if (sb->cb_slot == 0 && base && off >= base && off - base < 0x100 && !((off - base) % 16)) { const p_bind_buf *u = &D.ssbo[nst][(off - base) / 16]; RNG(u->addr, u->size); } } }
    { static int nlog; if (C && nlog < 40) for (int q = 0; q < n; q++) for (int w = 0; w < C->nwb; w++) if (wb_hits(&C->wb[w], r[q].lo, r[q].hi) && nlog++ < 40) {
        const wb_t *b = &C->wb[w]; LOG("[vk] draw dependency: shader %s reads %c %#lx+%#lx, pending write-back %d of %d: %s dst %p rows %zu x %zu", sh[0] ? sh[0]->key : "-", kd[q], (unsigned long)r[q].lo,
            (unsigned long)(r[q].hi - r[q].lo), w, C->nwb, b->st >= 0 ? "staging (texture copy)" : b->snap ? "arena with snapshot (storage buffer)" : "arena (feedback / buffer image)", (void *)b->dst, b->rows, b->row_bytes); } }
    int before = (int)(g_view_cur + g_view_fly); ranges_view(r, n);
    if ((int)(g_view_cur + g_view_fly) != before) { g_fwd_nwb = 0; return; }   // synced: everything landed (nothing to read on the GPU)
    n = 0;   // storage buffers named by the game's own uniform buffers: their addresses are read from them (landed, as checked above)
    for (int k = 0; k < 4; k++) { const vshader *s = sh[k]; if (!s) continue; int nst = nvn_stage_of(s->stage);
        for (int i = 0; i < s->nsb; i++) { const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off;
            if (sb->cb_slot < 3 || sb->cb_slot - 3 >= 16) continue;
            const p_bind_buf *g = &D.ubo[nst][sb->cb_slot - 3]; uint64_t ad; uint32_t sz;
            if (g->addr && off + 12 <= g->size) { memcpy(&ad, (const void *)(uintptr_t)(g->addr + off), 8); memcpy(&sz, (const void *)(uintptr_t)(g->addr + off + 8), 4); RNG(ad, sz); } } }
    #undef RNG
    if (n) { g_view_free--; before = (int)(g_view_cur + g_view_fly); ranges_view(r, n); if ((int)(g_view_cur + g_view_fly) != before) g_fwd_nwb = 0; }   // counted once
}
static int vk_draw(const p_draw *dw, const p_draw_idx *di) {
    { uintptr_t ia = dw ? dw->ind : di->ind, ma = dw ? dw->mdi_cnt : di->mdi_cnt; rd_rng r[2]; int n = 0;   // indirect arguments, read on the CPU below
      if (ia) r[n++] = (rd_rng){ia, ia + 20}; if (ma) r[n++] = (rd_rng){ma, ma + 4};
      if (n) ranges_view(r, n); }
    uint64_t mdi = dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    if (mdi) { uint32_t n = 0; if (!ind_read(mdi, &n, 4)) return 0; if ((uint32_t)mdi_i >= n) return 1; }   // past the count the GPU wrote: not drawn
    p_draw d2; p_draw_idx i2;
    if (dw && dw->ind) { uint32_t a[4]; if (!ind_read(dw->ind, a, 16)) return 0;   // count, instances, first, base instance
        d2 = *dw; d2.ind = 0; d2.count = (int32_t)a[0]; d2.instances = (int32_t)a[1]; d2.first = (int32_t)a[2]; d2.base_instance = (int32_t)a[3]; dw = &d2; if (!a[0] || !a[1]) return 1; }
    if (di && di->ind) { uint32_t a[5]; if (!ind_read(di->ind, a, 20)) return 0;   // count, instances, first index, base vertex, base instance
        i2 = *di; i2.ind = 0; i2.count = (int32_t)a[0]; i2.instances = (int32_t)a[1]; i2.ibuf = di->ibuf + (uint64_t)a[2] * (di->itype == 0 ? 1u : di->itype == 1 ? 2u : 4u);
        i2.base_vertex = (int32_t)a[3]; i2.base_instance = (int32_t)a[4]; di = &i2; if (!a[0] || !a[1]) return 1; }
    return vk_draw__(dw, di);
}
static uint64_t g_drop[8];   // draws not made, by reason: 0 shader missing, 1 targets, 2 pipeline, 3 descriptors, 4 an invalid binding, 5 pass, 6 vertex buffers
static int vk_draw_(const p_draw *dw, const p_draw_idx *di) {
    if (!g_d_init) draw_state_reset();
    if (!D.bst[0]) { RED_ONCE(95, "draw with no vertex program bound"); return 0; }
    pipe_key k; memset(&k, 0, sizeof k); int pst[4] = {-1, -1, -1, -1}; const prog_t *pprog[4] = {0};
    for (int b = 0; b < 5; b++) {   // each bound program gives the stages it is bound for
        const prog_t *pr = D.bst[b]; if (!pr) continue;
        int dup = 0; for (int q = 0; q < b; q++) dup |= D.bst[q] == pr; if (dup) continue;
        for (unsigned i = 0; i < pr->nstage && i < 6; i++) {
            uint8_t key[8]; char hx[17]; if (!prog_key(pr, i, key)) continue; key_hex(key, hx);
            vshader *s = sh_get(hx); if (!s) { g_drop[0]++; return 0; }
            if (s->stage == SH_CS) continue;
            if (s->stage == SH_TS) { RED_ONCE(100, "tessellation shaders are not on Vulkan yet: draw skipped (shader %s)", s->key); return 0; }
            int slot = s->stage == SH_VS ? 0 : s->stage == SH_PS ? 1 : s->stage == SH_GS ? 2 : 3;
            if (D.bst[slot] != pr) continue;
            k.sh[slot] = s; pst[slot] = (int)i; pprog[slot] = pr;
        }
    }
    if (!k.sh[0]) { RED_ONCE(96, "draw with no vertex shader"); return 0; }
    draw_view(k.sh, di, 1);   // what this draw reads of guest memory: pending GPU writes to it land first
    if (D.rast_discard) return tf_capture(k.sh[0], pprog[0], pst[0], dw, di);   // the vertex stage only: a capture, or nothing
    for (int i = 0; i < 4; i++) if (k.sh[i]) stage_textures(k.sh[i]);
    if (!rp_prepare()) { g_drop[1]++; return 0; }
    g_drawing = 1;   // from here no chunk is closed (chunk_submit reports one)
    k.z = !g_depth_zero; k.rp = P.rp; k.rk = P.key; k.ncolor = P.key.ncolor; k.ll = (uint8_t)(g_origin_ll ^ g_queue_ll);
    memcpy(k.attr, D.attr, sizeof k.attr); memcpy(k.stream, D.stream, sizeof k.stream);
    int prim = dw ? dw->prim : di->prim; k.topo = (uint8_t)topo_of(prim); k.restart = (uint8_t)D.restart.on; k.depth_clamp = (uint8_t)D.depth_clamp;
    // the state objects field by field over zeroed bits: bind_state copies the game's objects whole, and the bits the fields leave over (16 of colour's 32, 24 of polygon's,
    // 18 of each blend's 64, 25 of depth / stencil's) are whatever the game's object held (on its stack, without SetDefaults): each value made a new pipeline key
    k.color.blend_en = D.color.blend_en; k.color.logic_op = D.color.logic_op; k.color.alpha_func = D.color.alpha_func;
    k.chmask = D.chmask; k.ms = D.ms;
    k.poly.cull = D.poly.cull; k.poly.front = D.poly.front; k.poly.mode = D.poly.mode; k.poly.offset_en = D.poly.offset_en;
    for (int i = 0; i < 8; i++) { const blend_st *b = &D.blend[i]; blend_st *o = &k.blend[i]; o->target = b->target; o->src_rgb = b->src_rgb; o->dst_rgb = b->dst_rgb; o->src_a = b->src_a;
        o->dst_a = b->dst_a; o->eq_rgb = b->eq_rgb; o->eq_a = b->eq_a; o->adv = b->adv; o->adv_npm = b->adv_npm; o->adv_ovl = b->adv_ovl; }
    { const ds_st *d = &D.ds; ds_st *o = &k.ds; o->dt = d->dt; o->dw = d->dw; o->df = d->df; o->st = d->st; o->sf_f = d->sf_f; o->sf_b = d->sf_b;
      o->fail_f = d->fail_f; o->zfail_f = d->zfail_f; o->zpass_f = d->zpass_f; o->fail_b = d->fail_b; o->zfail_b = d->zfail_b; o->zpass_b = d->zpass_b; }
    if (!P.key.depth) { k.ds.dt = 0; k.ds.st = 0; }
    // only what the pipeline uses goes in its key: state left over from other draws (attributes the shader does not read, streams no read attribute uses, blend of
    // targets that are not there or not blended, write masks of absent targets, depth / stencil functions while their test is off, primitive restart of a list topology,
    // multisample and alpha-test fields the pipeline does not take) made a new pipeline of the same one again and again: 54,164 pipelines listed in a session of
    // Super Mario Bros. Wonder, a creation (about 2 ms on the Adreno) every few frames
    { uint32_t vin = k.sh[0]->io_in, streams = 0;
      for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &k.attr[i], 4); if (!(vin >> i & 1)) k.attr[i] = 0; else if (a.fmt) streams |= 1u << a.stream; }
      for (int s = 0; s < 16; s++) if (!(streams >> s & 1)) k.stream[s] = (vs_st){0, 0};
      for (uint32_t i = 0; i < 8; i++) if (i >= k.ncolor || !(k.color.blend_en >> i & 1)) memset(&k.blend[i], 0, sizeof k.blend[i]);
      if (k.ncolor < 8) k.chmask.mask &= (1u << (4 * k.ncolor)) - 1;
      k.color.alpha_func = 0;
      if (k.ncolor < 8) k.color.blend_en &= (1u << k.ncolor) - 1;   // enables of targets the pass does not have, and which of the three polygon offsets is on (the pipeline
      k.poly.offset_en = k.poly.offset_en != 0;                       // takes one depth-bias switch): 186,825 keys listed after a few sessions of Super Mario Bros. Wonder
      if (k.color.logic_op != NVN_LOGIC_OP_COPY && !g_vk_feat.logicOp) k.color.logic_op = NVN_LOGIC_OP_COPY;   // drawn as copy anyway (pipe_make)
      if (!k.ds.dt) { k.ds.df = 0; k.ds.dw = 0; }
      if (!k.ds.st) { k.ds.sf_f = k.ds.sf_b = 0; k.ds.fail_f = k.ds.zfail_f = k.ds.zpass_f = k.ds.fail_b = k.ds.zfail_b = k.ds.zpass_b = 0; }
      VkPrimitiveTopology tp = (VkPrimitiveTopology)k.topo;
      if (!(tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)) k.restart = 0;
      ms_st m; memset(&m, 0, sizeof m); m.a2c = k.ms.a2c; k.ms = m;
      if (!k.sh[0]->modz) k.z = 0; }
    pipe_t *pp = pipe_get(&k); if (!pp) { g_drop[2]++; return 0; }
    uint32_t base_inst = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    VkDescriptorSet sets[4] = {0};
    g_bind_bad = 0;
    for (int i = 0; i < 4; i++) if (k.sh[i]) { sets[i] = ds_alloc(k.sh[i]->dsl); if (!sets[i] || !stage_set(k.sh[i], sets[i], pprog[i], pst[i], base_inst)) { g_drop[3]++; return 0; } }
    if (g_bind_bad) { g_drop[4]++; static int nb; if (nb++ < 10) LOG("[RED] [vk] draw skipped: a binding has no valid descriptor (shaders %s %s)", k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-"); return 0; }
    if (!rp_start()) { g_drop[5]++; return 0; }   // after the textures: their uploads end a pass begun before (it is begun again on the same targets)
    VkCommandBuffer c = C->cb;
    occl_begin();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->p);
    for (int i = 0; i < 4; i++) if (k.sh[i]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl, (uint32_t)(k.sh[i]->set & 3), 1, &sets[i], 0, NULL);
    // dynamic state: the viewport (see the file header for the origin), the scissor, stencil, blend constants, depth bias
    float vx = 0, vy = 0, vw = (float)P.w, vh = (float)P.h; if (D.has_vp) { vx = D.vp[0][0]; vy = D.vp[0][1]; vw = D.vp[0][2]; vh = D.vp[0][3]; }
    float n = D.dr[0] < 0 ? 0 : D.dr[0] > 1 ? 1 : D.dr[0], f = D.dr[1] < 0 ? 0 : D.dr[1] > 1 ? 1 : D.dr[1];
    VkViewport v = g_origin_ll ? (VkViewport){vx, vy, vw, vh, n, f} : (VkViewport){vx, vy + vh, vw, -vh, n, f};
    vkCmdSetViewport(c, 0, 1, &v);
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) { int x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y; int64_t x1 = (int64_t)R.sc.x + R.sc.w, y1 = (int64_t)R.sc.y + R.sc.h;
        if (x1 > (int64_t)P.w) x1 = P.w; if (y1 > (int64_t)P.h) y1 = P.h;
        sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}}; }
    vkCmdSetScissor(c, 0, 1, &sc);
    vkCmdSetBlendConstants(c, D.blend_color);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, D.st_vmask[0]); vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, D.st_vmask[1]);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, D.st_wmask[0]); vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, D.st_wmask[1]);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, D.st_ref[0]); vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, D.st_ref[1]);
    vkCmdSetDepthBias(c, D.po.units, g_vk_feat.depthBiasClamp ? D.po.clamp : 0, D.po.factor);
    // indices: copied, quads made triangles; vertex range for the vertex data copy
    int64_t vlo = 0, vhi = 0; uint32_t count = dw ? (uint32_t)dw->count : (uint32_t)di->count, inst = (uint32_t)(dw ? dw->instances : di->instances); if (!inst) inst = 1;
    int quads = prim == 7, qstrip = prim == 8; VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ioff = 0; uint32_t icount = count; int i32 = 0;
    if (di) {
        int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf; uint32_t mx = 0, mn = UINT32_MAX;
        if (!src) { RED_ONCE(98, "indexed draw with no index buffer"); return 0; }
        uint32_t nq = quads ? count / 4 : qstrip ? (count >= 4 ? (count - 2) / 2 : 0) : 0;
        icount = quads || qstrip ? nq * 6 : count;
        uint32_t rv = D.restart.on ? D.restart.index : UINT32_MAX;
        // kept 16- and 32-bit indices stay in their own size (Vulkan's restart index is the type's all-ones, as NVN's 0xffff / 0xffffffff): half the index reads of a 16-bit
        // draw on the GPU, which the far map view is bound by (2.8 million indices a frame); 8-bit ones are widened, as Vulkan takes them only with VK_EXT_index_type_uint8
        int native = isz != 1;
        uint8_t *fill = NULL; gc_ent *ge = quads || qstrip ? NULL : gc_get((uintptr_t)src, (uint64_t)count * isz, 1u + 2u * isz + ((isz == 2 && rv == 0xffff) || (isz == 1 && rv == 0xff) ? 32u : 0u), (uint64_t)count * (native ? isz : 4), &fill);
        if (ge && fill && native) {   // copied as they are, the vertex range scanned once
            memcpy(fill, src, (size_t)count * isz);
            if (isz == 2) { const uint16_t *s16 = (const uint16_t *)(const void *)src; int rs = D.restart.on && rv == 0xffff;
                for (uint32_t j = 0; j < count; j++) { uint32_t x = s16[j]; if (rs && x == 0xffff) continue; if (x > mx) mx = x; if (x < mn) mn = x; } }
            else { const uint32_t *s32 = (const uint32_t *)(const void *)src; for (uint32_t j = 0; j < count; j++) { uint32_t x = s32[j]; if (x == UINT32_MAX) continue; if (x > mx) mx = x; if (x < mn) mn = x; } }
            if (mn == UINT32_MAX) mn = mx = 0;
            ge->mn = mn; ge->mx = mx; fill = NULL;
        }
        if (ge && !fill) {   // kept on the GPU, with its vertex range
            ib = gc_buf(ge); ioff = ge->off; i32 = !native || isz == 4; mn = ge->mn; mx = ge->mx;
            vlo = (int64_t)mn + di->base_vertex; vhi = (int64_t)mx + di->base_vertex + 1;
            goto indexed;
        }
        uint32_t *o = fill ? (uint32_t *)fill : (uint32_t *)ar_alloc((VkDeviceSize)icount * 4, &ib, &ioff); if (!o) return 0; i32 = 1;
        if (fill) { ib = gc_buf(ge); ioff = ge->off; } else g_gc_copy++;
        #define IDX(j) (isz == 1 ? (uint32_t)src[j] : isz == 2 ? (uint32_t)((const uint16_t *)(const void *)src)[j] : ((const uint32_t *)(const void *)src)[j])
        // the vertex range is taken from the indices as they are written: the arena is write-combined memory, and reading the indices back from it took over half
        // of the render thread in a Super Mario Bros. Wonder level (uncached reads)
        #define MM(x) do { uint32_t x_ = (x); if (x_ != UINT32_MAX) { if (x_ > mx) mx = x_; if (x_ < mn) mn = x_; } } while (0)
        if (quads || qstrip) for (uint32_t q = 0; q < nq; q++) { uint32_t a0, a1, a2, a3; if (quads) { a0 = IDX(4 * q); a1 = IDX(4 * q + 1); a2 = IDX(4 * q + 2); a3 = IDX(4 * q + 3); } else { a0 = IDX(2 * q); a1 = IDX(2 * q + 1); a2 = IDX(2 * q + 3); a3 = IDX(2 * q + 2); }
                uint32_t t6[6] = {a0, a1, a2, a0, a2, a3}; memcpy(o + 6 * q, t6, sizeof t6); MM(a0); MM(a1); MM(a2); MM(a3); }
        else if (isz == 2) { const uint16_t *s16 = (const uint16_t *)(const void *)src; int rs = D.restart.on && rv == 0xffff;
            for (uint32_t j = 0; j < count; j++) { uint32_t x = s16[j]; if (rs && x == 0xffff) x = UINT32_MAX; o[j] = x; MM(x); } }
        else if (isz == 4) { const uint32_t *s32 = (const uint32_t *)(const void *)src;
            for (uint32_t j = 0; j < count; j++) { uint32_t x = s32[j]; o[j] = x; MM(x); } }
        else for (uint32_t j = 0; j < count; j++) { uint32_t x = src[j]; if (x == 0xff && D.restart.on && rv == 0xff) x = UINT32_MAX; o[j] = x; MM(x); }
        #undef MM
        #undef IDX
        if (mn == UINT32_MAX) mn = mx = 0;
        if (fill) { ge->mn = mn; ge->mx = mx; }
        vlo = (int64_t)mn + di->base_vertex; vhi = (int64_t)mx + di->base_vertex + 1;
        indexed:;
    } else {
        vlo = dw->first; vhi = (int64_t)dw->first + count;
        if (quads || qstrip) {   // non-indexed quads: indices 0,1,2 0,2,3 per quad from first
            uint32_t nq = quads ? count / 4 : (count >= 4 ? (count - 2) / 2 : 0); icount = nq * 6;
            uint32_t *o = (uint32_t *)ar_alloc((VkDeviceSize)icount * 4, &ib, &ioff); if (!o) return 0; i32 = 1;
            for (uint32_t q = 0; q < nq; q++) { uint32_t b0 = (uint32_t)dw->first + (quads ? 4 * q : 2 * q); uint32_t t6[6] = {b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3}; if (qstrip) { t6[2] = b0 + 3; t6[4] = b0 + 3; t6[5] = b0 + 2; } memcpy(o + 6 * q, t6, sizeof t6); }
        }
    }
    if (prim == 2 || prim == 9) RED_ONCE(99, "line loop / polygon drawn as a strip / fan");
    int64_t lo = k.sh[0]->vid || vlo < 0 ? 0 : vlo;   // the bindings start at vertex lo, and the draw counts from there; a shader reading gl_VertexIndex sees its own numbers
    if (!bind_vbufs(c, k.sh[0], lo, vhi, base_inst, inst)) { g_drop[6]++; return 0; }
    if (ib) { vkCmdBindIndexBuffer(c, ib, ioff, i32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16); vkCmdDrawIndexed(c, icount, inst, 0, (int32_t)((di ? di->base_vertex : 0) - lo), base_inst); }
    else vkCmdDraw(c, count, inst, (uint32_t)(dw->first - lo), base_inst);
    g_draws++; C->ndraw++;
    if (g_prof_cur >= 0) { prof_rec *pr = &g_prof[g_prof_cur]; uint64_t nv = (uint64_t)(ib ? icount : count) * inst; pr->draws++; pr->verts += nv;
        if (nv > pr->big) { pr->big = nv; pr->bigvs = k.sh[0]->key; pr->bigfs = k.sh[1] ? k.sh[1]->key : NULL; } }
    if (dump_on()) {
        char tg[160] = ""; for (int i = 0; i < 8; i++) if (P.cv[i]) { size_t l = strlen(tg); snprintf(tg + l, sizeof tg - l, " a%d=s%llu(%ux%u vk%d)", i, (unsigned long long)P.cv[i]->serial, P.cv[i]->w, P.cv[i]->h, P.cv[i]->fmt); }
        LOG("[dump] pass %d draw %llu: vs %s fs %s prim %d count %u inst %u%s vp %.0f,%.0f %.0fx%.0f sc %d,%d %ux%u ll %d targets%s", g_dump_pass + 1, (unsigned long long)g_draws,
            k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-", prim, count, inst, di ? " indexed" : "", v.x, v.y, v.width, v.height, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, g_origin_ll, tg);
    }
    if (D.tf_on && k.sh[0]->modtf) { g_drawing = 0; return tf_capture(k.sh[0], pprog[0], pst[0], dw, di); }   // drawn and captured (nvn_mtl.m draw)
    return 1;
}

// ---------------- Compute ----------------
typedef struct { VkPipeline p; VkPipelineLayout pl; } cpipe_t;
static nvn_hmap g_cpipes = {.klen = 8};
static uint64_t g_dispatches;
static cpipe_t *cpipe_get(vshader *s) {
    cpipe_t *cp = nvn_hm_get(&g_cpipes, &s); if (cp) return cp->p ? cp : NULL;
    cp = calloc(1, sizeof *cp); nvn_hm_put(&g_cpipes, &s, cp);
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &s->dsl};
    if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &cp->pl) != VK_SUCCESS) return NULL;
    VkComputePipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = cp->pl,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = s->mod, .pName = "main"}};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); VkResult pr = vkCreateComputePipelines(g_vk.dev, pcache(), 1, &pc, NULL, &cp->p);
    atomic_fetch_add(&g_cpso_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_cpso_made, 1);
    if (pr != VK_SUCCESS) { static int n; if (n++ < 16) LOG("[RED] [vk] compute pipeline of shader %s failed", s->key); cp->p = VK_NULL_HANDLE; return NULL; }
    if (vk_debug()) LOG("[vk] compute pipeline for shader %s", s->key);
    return cp;
}
static int vk_dispatch(const p_dispatch *dp) {
    if (!g_d_init) draw_state_reset();
    rp_end();
    const prog_t *pr = D.cprog; if (!pr) { RED_ONCE(106, "dispatch with no compute program bound"); return 0; }
    vshader *s = NULL; int pst = -1;
    for (unsigned i = 0; i < pr->nstage && i < 6 && !s; i++) { uint8_t key[8]; char hx[17]; if (!prog_key(pr, i, key)) continue; key_hex(key, hx); vshader *x = sh_get(hx); if (x && x->stage == SH_CS) { s = x; pst = (int)i; } }
    if (!s) { RED_ONCE(107, "compute program without a translated compute shader"); return 0; }
    uint32_t g[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    { vshader *sh[4] = {s, NULL, NULL, NULL}; rd_rng r[1] = {{(uintptr_t)dp->ind, (uintptr_t)dp->ind + 12}};   // what the dispatch reads of guest memory lands first (draw_view), not everything pending
      if (dp->ind) ranges_view(r, 1); draw_view(sh, NULL, 0); }
    if (dp->ind && !ind_read(dp->ind, g, 12)) return 0;
    if (!g[0] || !g[1] || !g[2]) return 1;
    cpipe_t *cp = cpipe_get(s); if (!cp) return 0;
    stage_textures(s);
    memcpy(g_cdim, g, sizeof g_cdim);
    g_bind_bad = 0; VkDescriptorSet set = ds_alloc(s->dsl); int ok = set && stage_set(s, set, pr, pst, 0) && !g_bind_bad;
    if (!ok) return 0;
    VkCommandBuffer c = cb(); if (!c) return 0;
    cb_barrier();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pl, 0, 1, &set, 0, NULL);
    int pr_i = -1;
    if (prof_on() && C->pq && C->npq + 2 <= 512 && g_nprof < 1024) { prof_rec *r = &g_prof[pr_i = g_nprof++]; memset(r, 0, sizeof *r); r->c = C; r->ncolor = -1; r->w = g[0]; r->h = g[1]; r->draws = (int)g[2]; r->seq = g_prof_seq++;
        r->s0 = C->npq++; vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->pq, (uint32_t)r->s0); }
    vkCmdDispatch(c, g[0], g[1], g[2]);
    if (pr_i >= 0) { g_prof[pr_i].s1 = C->npq++; vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->pq, (uint32_t)g_prof[pr_i].s1); }
    cb_barrier();
    g_dispatches++; C->ndraw++;
    return 1;
}

// ---------------- Clears inside a render pass ----------------
// A clear limited by the scissor is vkCmdClearAttachments on the bound targets' pass. One with a channel mask, or a partial stencil mask, is a draw: a full-target
// triangle (shaders/clear.vert, clear.frag; glslc --target-env=vulkan1.1 -mfmt=num into shaders/clear_*.inc) whose pipeline writes only those channels / bits
static const uint32_t CLEAR_VS[] = {
#include "shaders/clear_vert.inc"
}, CLEAR_FS[] = {
#include "shaders/clear_frag.inc"
};
static VkRect2D clear_rect(void) {   // the scissor on the pass's targets (window rows are memory rows in both origin modes, as for draws)
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) { int x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y; int64_t x1 = (int64_t)R.sc.x + R.sc.w, y1 = (int64_t)R.sc.y + R.sc.h;
        if (x1 > (int64_t)P.w) x1 = P.w; if (y1 > (int64_t)P.h) y1 = P.h;
        sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}}; }
    return sc;
}
typedef struct { VkRenderPass rp; uint32_t ncolor, idx, mask, samples, stencil_mask, depth; } cpk_t;
static nvn_hmap g_clr_pipes = {.klen = sizeof(cpk_t)};
static VkPipelineLayout g_clr_pl; static VkShaderModule g_clr_vs, g_clr_fs;
static VkPipeline clear_pipe(const cpk_t *k) {
    VkPipeline p = nvn_hm_get(&g_clr_pipes, k); if (p) return p;
    if (!g_clr_pl) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
        VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VkShaderModuleCreateInfo v = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof CLEAR_VS, .pCode = CLEAR_VS};
        VkShaderModuleCreateInfo f = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof CLEAR_FS, .pCode = CLEAR_FS};
        if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &g_clr_pl) != VK_SUCCESS || vkCreateShaderModule(g_vk.dev, &v, NULL, &g_clr_vs) != VK_SUCCESS || vkCreateShaderModule(g_vk.dev, &f, NULL, &g_clr_fs) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_clr_vs, .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_clr_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1)};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, k->stencil_mask, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = k->depth, .depthWriteEnable = k->depth, .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = k->stencil_mask != 0, .front = so, .back = so, .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState cba[8]; memset(cba, 0, sizeof cba);
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) cba[i].colorWriteMask = i == k->idx ? (VkColorComponentFlags)(k->mask & 0xf) : 0;
    VkPipelineColorBlendStateCreateInfo cb_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb_, .pDynamicState = &dy, .layout = g_clr_pl, .renderPass = k->rp};
    if (vkCreateGraphicsPipelines(g_vk.dev, pcache(), 1, &pc, NULL, &p) != VK_SUCCESS) { RED_ONCE(108, "clear pipeline creation failed"); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_clr_pipes, k, p);
    return p;
}
static int is_int_fmt(VkFormat f) {
    VkClearColorValue t = clear_value(f, (const float[4]){1.5f, 0, 0, 0}); return t.uint32[0] == 1;   // clear_value converts only for integer formats
}
static int clear_in_pass(const vtex *v, unsigned level, int depth) {
    if (!rp_prepare() || (depth && !P.key.depth)) return 0;
    return (v->w >> level ? v->w >> level : 1) == P.w && (v->h >> level ? v->h >> level : 1) == P.h;
}
// A colour clear of target idx limited by the scissor and / or a channel mask (gfx_vk.c clear_color takes the full ones)
static void clear_color_pass(const p_clear_color *cc) {
    if (!rp_prepare() || !rp_start()) return;
    VkRect2D r = clear_rect(); if (!r.extent.width || !r.extent.height) return;
    VkFormat f = P.key.color[cc->index];
    if ((cc->mask & 0xf) == 0xf) {
        VkClearAttachment at = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)cc->index, {.color = clear_value(f, cc->rgba)}};
        VkClearRect cr = {r, 0, 1}; vkCmdClearAttachments(C->cb, 1, &at, 1, &cr); return;
    }
    if (is_int_fmt(f)) { RED_ONCE(109, "masked clear of an integer target (format %d): not done", f); return; }
    cpk_t k; memset(&k, 0, sizeof k); k.rp = P.rp; k.ncolor = P.key.ncolor; k.idx = (uint32_t)cc->index; k.mask = (uint32_t)cc->mask & 0xf; k.samples = P.key.samples;
    VkPipeline p = clear_pipe(&k); if (!p) return;
    float pc[5] = {cc->rgba[0], cc->rgba[1], cc->rgba[2], cc->rgba[3], 0};
    VkViewport vp = {0, 0, (float)P.w, (float)P.h, 0, 1};
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); vkCmdSetViewport(C->cb, 0, 1, &vp); vkCmdSetScissor(C->cb, 0, 1, &r);
    vkCmdPushConstants(C->cb, g_clr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc); vkCmdDraw(C->cb, 3, 1, 0, 0);
}
// A depth-stencil clear limited by the scissor and / or a partial stencil mask
static void clear_ds_pass(const p_clear_ds *cd, VkImageAspectFlags a) {
    if (!rp_prepare() || !rp_start() || !P.key.depth) return;
    VkRect2D r = clear_rect(); if (!r.extent.width || !r.extent.height) return;
    uint32_t sm = (uint32_t)cd->stencil_mask & 0xff;
    if (!(a & VK_IMAGE_ASPECT_STENCIL_BIT) || sm == 0xff) {
        VkClearAttachment at = {a, 0, {.depthStencil = {cd->depth, (uint32_t)cd->stencil}}};
        VkClearRect cr = {r, 0, 1}; vkCmdClearAttachments(C->cb, 1, &at, 1, &cr); return;
    }
    cpk_t k; memset(&k, 0, sizeof k); k.rp = P.rp; k.ncolor = P.key.ncolor; k.idx = 99; k.samples = P.key.samples; k.stencil_mask = sm; k.depth = (a & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    VkPipeline p = clear_pipe(&k); if (!p) return;
    float pc[5] = {0, 0, 0, 0, cd->depth};
    VkViewport vp = {0, 0, (float)P.w, (float)P.h, 0, 1};
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); vkCmdSetViewport(C->cb, 0, 1, &vp); vkCmdSetScissor(C->cb, 0, 1, &r);
    vkCmdSetStencilReference(C->cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)cd->stencil);
    vkCmdPushConstants(C->cb, g_clr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc); vkCmdDraw(C->cb, 3, 1, 0, 0);
}
