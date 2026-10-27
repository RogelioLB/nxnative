// A check of the NVN replay's picture path in the app without a game: the NVN calls a game makes to show something (device, queue, a memory pool, two
// 1280x720 window textures, a window, a command buffer that sets the render target and clears it), a colour that changes every frame, presented to the
// NVN window (gfx_vk.c → front_android.c draws it). The NVN functions come from nvnBootstrapLoader, as a game gets them (the host's takeover in the
// swforce table, hle_nvn.c).
// With the synthetic shaders of tests/synth_shaders.py in DM_SPV_DIR (their code areas and control sections, as a game ships them, and their translation:
// the app's assets/demo), each frame also draws a triangle over the clear: vertex attribute 0 the position, 1 the colour's red and green, and the blue and
// alpha from a uniform buffer of the fragment stage. That is the draw path: program keys, translated SPIR-V, pipeline, vertex and uniform data. The last frame
// draws it through DrawArraysIndirect; two clears limited by the scissor follow (one of them red only), and a compute dispatch (cs.code) writes a constant and its
// group count to a storage buffer the CPU reads afterwards. With vstf.code (the vertex shader with a feedback layout), the triangle is drawn once more with
// rasterization off and transform feedback on: its generic output 0's x y z per vertex land in a buffer. With vsab.code (the same vertex shader in two parts,
// VertexA + VertexB) the last frame draws with it. After the frames, a probe of the texture paths through texture and sampler pools: csimg stores to an image,
// cstb fetches a texel of a buffer texture and csms a sample of a multisample texture (cleared first), into a storage buffer the CPU reads.
#include "dm.h"
#include "hle.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <math.h>
#include <sys/mman.h>

static PFNNVNDEVICEGETPROCADDRESSPROC g_gpa; static NVNdevice g_dev;
static size_t load(const char *dir, const char *name, uint8_t *to, size_t cap) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(to, 1, cap, f); fclose(f); return n;
}
static void *proc(const char *n) { void *p = (void *)g_gpa(&g_dev, n); if (!p) LOG("[RED] nvn demo: no %s", n); return p; }
#define P(name) ((NVNT_##name)proc(#name))

// The texture paths (see the file header): ex / exa the free part of the pool (its first 64 KiB are the demo's shaders and buffers)
static void probe(NVNqueue *q, NVNcommandBuffer *cb, NVNmemoryPool *pool, uint8_t *mem, NVNbufferAddress base, size_t exo, uint8_t *ctl, size_t nctl) {
    const char *dir = getenv("DM_SPV_DIR"); uint8_t *ex = mem + exo; NVNbufferAddress exa = base + exo;
    static uint8_t c1[0x880], c2[0x880], c3[0x880];
    if (!dir || !load(dir, "csimg.code", ex + 0xA000, 0x1000) || !load(dir, "cstb.code", ex + 0xB000, 0x1000) || !load(dir, "csms.code", ex + 0xC000, 0x1000) ||
        load(dir, "csimg.ctl", c1, sizeof c1) != sizeof c1 || load(dir, "cstb.ctl", c2, sizeof c2) != sizeof c2 || load(dir, "csms.ctl", c3, sizeof c3) != sizeof c3) return;
    NVNprogram pi, pb, pm; NVNshaderData d1 = {exa + 0xA000, c1}, d2 = {exa + 0xB000, c2}, d3 = {exa + 0xC000, c3};
    P(nvnProgramInitialize)(&pi, &g_dev); P(nvnProgramInitialize)(&pb, &g_dev); P(nvnProgramInitialize)(&pm, &g_dev);
    if (!P(nvnProgramSetShaders)(&pi, 1, &d1) || !P(nvnProgramSetShaders)(&pb, 1, &d2) || !P(nvnProgramSetShaders)(&pm, 1, &d3)) return;
    // texture and sampler pools at +64 KiB; the IDs after the reserved ones
    int tds = 32, sds = 32, rt = 0, rs = 0; P(nvnDeviceGetInteger)(&g_dev, NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE, &tds); P(nvnDeviceGetInteger)(&g_dev, NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE, &sds);
    P(nvnDeviceGetInteger)(&g_dev, NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS, &rt); P(nvnDeviceGetInteger)(&g_dev, NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS, &rs);
    NVNtexturePool tp; NVNsamplerPool sp;
    P(nvnTexturePoolInitialize)(&tp, pool, (ptrdiff_t)(exo + 0x10000), rt + 8); P(nvnSamplerPoolInitialize)(&sp, pool, (ptrdiff_t)(exo + 0x18000), rs + 8);
    // a 4×4 image, a buffer texture of 4 RGBA32F texels (texel i = 4i + 1 .. 4i + 4), a 4×4 four-sample texture
    NVNtextureBuilder tb; NVNtexture ti, tt, tm;
    P(nvnTextureBuilderSetDevice)(&tb, &g_dev); P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetSize2D)(&tb, 4, 4); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_IMAGE); P(nvnTextureBuilderSetStorage)(&tb, pool, (ptrdiff_t)(exo + 0x20000)); P(nvnTextureInitialize)(&ti, &tb);
    float tex[16]; for (int i = 0; i < 16; i++) tex[i] = (float)(i + 1); memcpy(ex + 0x30000, tex, sizeof tex);
    P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_BUFFER); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA32F); P(nvnTextureBuilderSetSize1D)(&tb, 4);
    P(nvnTextureBuilderSetStorage)(&tb, pool, (ptrdiff_t)(exo + 0x30000)); P(nvnTextureInitialize)(&tt, &tb);
    P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); P(nvnTextureBuilderSetSize2D)(&tb, 4, 4); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetSamples)(&tb, 4); P(nvnTextureBuilderSetStorage)(&tb, pool, (ptrdiff_t)(exo + 0x40000)); P(nvnTextureInitialize)(&tm, &tb);
    int ii = rt, ib = rt + 1, im = rt + 2, is = rs;
    P(nvnTexturePoolRegisterImage)(&tp, ii, &ti, NULL); P(nvnTexturePoolRegisterTexture)(&tp, ib, &tt, NULL); P(nvnTexturePoolRegisterTexture)(&tp, im, &tm, NULL);
    NVNsamplerBuilder sb; NVNsampler sm; P(nvnSamplerBuilderSetDevice)(&sb, &g_dev); P(nvnSamplerBuilderSetDefaults)(&sb); P(nvnSamplerInitialize)(&sm, &sb); P(nvnSamplerPoolRegisterSampler)(&sp, is, &sm);
    memset(ex + 0x9000, 0, 64);
    P(nvnCommandBufferAddCommandMemory)(cb, pool, (ptrdiff_t)(exo - (256u << 10)), 256u << 10); P(nvnCommandBufferAddControlMemory)(cb, ctl, nctl);
    P(nvnCommandBufferBeginRecording)(cb);
    P(nvnCommandBufferSetTexturePool)(cb, &tp); P(nvnCommandBufferSetSamplerPool)(cb, &sp);
    const NVNtexture *msrt = &tm; P(nvnCommandBufferSetRenderTargets)(cb, 1, &msrt, NULL, NULL, NULL);
    static const float msc[4] = {1, 0.25f, 0.5f, 1}; P(nvnCommandBufferSetScissor)(cb, 0, 0, 4, 4); P(nvnCommandBufferClearColor)(cb, 0, msc, NVN_CLEAR_COLOR_MASK_RGBA);
    P(nvnCommandBufferBindProgram)(cb, &pi, NVN_SHADER_STAGE_COMPUTE_BIT); P(nvnCommandBufferBindImage)(cb, NVN_SHADER_STAGE_COMPUTE, 0, P(nvnDeviceGetImageHandle)(&g_dev, ii));
    P(nvnCommandBufferDispatchCompute)(cb, 4, 4, 1);
    P(nvnCommandBufferBindStorageBuffer)(cb, NVN_SHADER_STAGE_COMPUTE, 0, exa + 0x9000, 64);
    P(nvnCommandBufferBindProgram)(cb, &pb, NVN_SHADER_STAGE_COMPUTE_BIT); P(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_COMPUTE, 0, P(nvnDeviceGetTextureHandle)(&g_dev, ib, is));
    P(nvnCommandBufferDispatchCompute)(cb, 1, 1, 1);
    P(nvnCommandBufferBindProgram)(cb, &pm, NVN_SHADER_STAGE_COMPUTE_BIT); P(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_COMPUTE, 1, P(nvnDeviceGetTextureHandle)(&g_dev, im, is));
    P(nvnCommandBufferDispatchCompute)(cb, 1, 1, 1);
    NVNcommandHandle h = P(nvnCommandBufferEndRecording)(cb); P(nvnQueueSubmitCommands)(q, 1, &h); P(nvnQueueFinish)(q);
    uint32_t px = 0; NVNcopyRegion r = {2, 2, 0, 1, 1, 1}; P(nvnTextureReadTexels)(&ti, NULL, &r, &px);
    // A render target read through another texture object over the same storage (games make their transient targets anew each frame): cleared through one, read through the other
    NVNtexture ta, tb2; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetSize2D)(&tb, 8, 8); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetStorage)(&tb, pool, (ptrdiff_t)(exo + 0x50000)); P(nvnTextureInitialize)(&ta, &tb); P(nvnTextureInitialize)(&tb2, &tb);
    P(nvnCommandBufferAddCommandMemory)(cb, pool, (ptrdiff_t)(exo - (256u << 10)), 256u << 10); P(nvnCommandBufferAddControlMemory)(cb, ctl, nctl);
    P(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *art = &ta; P(nvnCommandBufferSetRenderTargets)(cb, 1, &art, NULL, NULL, NULL); P(nvnCommandBufferSetScissor)(cb, 0, 0, 8, 8);
    static const float shc[4] = {0, 0.5f, 1, 1}; P(nvnCommandBufferClearColor)(cb, 0, shc, NVN_CLEAR_COLOR_MASK_RGBA);
    h = P(nvnCommandBufferEndRecording)(cb); P(nvnQueueSubmitCommands)(q, 1, &h); P(nvnQueueFinish)(q);
    uint32_t sh = 0; NVNcopyRegion r1 = {3, 3, 0, 1, 1, 1}; P(nvnTextureReadTexels)(&tb2, NULL, &r1, &sh);
    LOG("nvn demo: target read through another object over its storage %08x (want ffff8000)", sh);
    // A LINEAR texture the CPU writes (a game's glyph cache): copied by the GPU into ta and read there; written again by the CPU, copied again: the new content
    NVNtexture tl; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetSize2D)(&tb, 8, 8); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR); P(nvnTextureBuilderSetStorage)(&tb, pool, (ptrdiff_t)(exo + 0x60000)); P(nvnTextureInitialize)(&tl, &tb);
    uint32_t lin[2];
    for (int pass = 0; pass < 2; pass++) {
        size_t stride = (size_t)P(nvnTextureGetStride)(&tl); if (stride < 32) stride = 32;
        uint32_t want = pass ? 0xff00ff00u : 0xff0000ffu;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) memcpy(ex + 0x60000 + (size_t)y * stride + 4 * (size_t)x, &want, 4);
        P(nvnCommandBufferAddCommandMemory)(cb, pool, (ptrdiff_t)(exo - (256u << 10)), 256u << 10); P(nvnCommandBufferAddControlMemory)(cb, ctl, nctl);
        P(nvnCommandBufferBeginRecording)(cb);
        NVNcopyRegion all = {0, 0, 0, 8, 8, 1}; P(nvnCommandBufferCopyTextureToTexture)(cb, &tl, NULL, &all, &ta, NULL, &all, 0);
        h = P(nvnCommandBufferEndRecording)(cb); P(nvnQueueSubmitCommands)(q, 1, &h); P(nvnQueueFinish)(q);
        P(nvnTextureReadTexels)(&ta, NULL, &r1, &lin[pass]);
    }
    LOG("nvn demo: CPU-written linear texture through the GPU %08x then %08x (want ff0000ff then ff00ff00)", lin[0], lin[1]);
    float f[8]; memcpy(f, ex + 0x9000, sizeof f);
    LOG("nvn demo: image store %08x (want ff4080ff); buffer texture texel 2 (%g %g %g %g) (want 9 10 11 12); multisample sample 3 (%.3f %.3f %.3f %.3f) (want 1 0.251 0.502 1)",
        px, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}
int nvn_demo_run(int frames) {
    unsigned long n = 0; const hle_force_ent *f = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &n);
    PFNNVNBOOTSTRAPLOADERPROC boot = NULL;
    for (unsigned long i = 0; f && i < n / sizeof *f; i++) if (!strcmp(f[i].name, "nvnBootstrapLoader")) boot = (PFNNVNBOOTSTRAPLOADERPROC)f[i].fn;
    if (!boot || !(g_gpa = (PFNNVNDEVICEGETPROCADDRESSPROC)boot("nvnDeviceGetProcAddress"))) { LOG("[RED] nvn demo: no nvnBootstrapLoader"); return 0; }
    NVNdeviceBuilder db; P(nvnDeviceBuilderSetDefaults)(&db); P(nvnDeviceBuilderSetFlags)(&db, 0);
    if (!P(nvnDeviceInitialize)(&g_dev, &db)) return 0;
    // memory: the two window textures and the command memory
    enum { W = 1280, H = 720 }; size_t tsz = 4u << 20, cmd_sz = 256u << 10, pool_sz = 2 * tsz + cmd_sz + (1u << 20);
    // mapped, as a game's memory is: malloc's pointers carry a tag in their top byte on Android, and a write fault's address (untagged) would not match the
    // dirty tracker's range for the pool (nvn_dirty.c): the CPU write to the linear texture below would crash
    uint8_t *mem = mmap(NULL, pool_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (mem == MAP_FAILED) return 0;
    memset(mem, 0, pool_sz);
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    P(nvnMemoryPoolBuilderSetDevice)(&pb, &g_dev); P(nvnMemoryPoolBuilderSetDefaults)(&pb);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, mem, pool_sz); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    if (!P(nvnMemoryPoolInitialize)(&pool, &pb)) return 0;
    NVNtextureBuilder tb; NVNtexture tex[2]; NVNtexture *tp[2] = {&tex[0], &tex[1]};
    P(nvnTextureBuilderSetDevice)(&tb, &g_dev); P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetSize2D)(&tb, W, H); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    for (int i = 0; i < 2; i++) { P(nvnTextureBuilderSetStorage)(&tb, &pool, (ptrdiff_t)(i * tsz)); P(nvnTextureInitialize)(&tex[i], &tb); }
    NVNwindowBuilder wb; NVNwindow win; P(nvnWindowBuilderSetDevice)(&wb, &g_dev); P(nvnWindowBuilderSetDefaults)(&wb);
    P(nvnWindowBuilderSetTextures)(&wb, 2, tp);
    if (!P(nvnWindowInitialize)(&win, &wb)) return 0;
    NVNqueueBuilder qb; NVNqueue q; P(nvnQueueBuilderSetDevice)(&qb, &g_dev); P(nvnQueueBuilderSetDefaults)(&qb);
    size_t qsz = P(nvnQueueBuilderGetQueueMemorySize)(&qb); void *qmem = NULL; if (posix_memalign(&qmem, 4096, qsz)) return 0;
    P(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    if (!P(nvnQueueInitialize)(&q, &qb)) return 0;
    NVNcommandBuffer cb; P(nvnCommandBufferInitialize)(&cb, &g_dev);
    // DM_NVN_DEMO_MODE (the command-line check): "ul" = what most games set at start (upper-left window origin, depth 0..1) with back faces culled, "ulf" the same culling
    // front faces (the triangle, counter-clockwise as NVN's default front face, disappears), "llf" front faces culled at the default lower-left origin
    const char *mode = getenv("DM_NVN_DEMO_MODE"); int ul = mode && mode[0] == 'u', cull = !mode ? NVN_FACE_NONE : strchr(mode, 'f') ? NVN_FACE_FRONT : NVN_FACE_BACK;
    if (ul) { P(nvnDeviceSetWindowOriginMode)(&g_dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT); P(nvnDeviceSetDepthMode)(&g_dev, NVN_DEPTH_MODE_NEAR_IS_ZERO); }
    NVNpolygonState ps; P(nvnPolygonStateSetDefaults)(&ps); P(nvnPolygonStateSetCullFace)(&ps, (NVNface)cull);
    // the triangle: shaders, vertex data and uniform buffer after the command memory
    uint8_t *ex = mem + 2 * tsz + cmd_sz; NVNbufferAddress exa = P(nvnMemoryPoolGetBufferAddress)(&pool) + 2 * tsz + cmd_sz;
    static uint8_t vctl[0x880], pctl[0x880]; const char *dir = getenv("DM_SPV_DIR");
    size_t nvc = dir ? load(dir, "vs.code", ex, 0x1000) : 0, npc = dir ? load(dir, "ps.code", ex + 0x1000, 0x1000) : 0;
    int tri = nvc && npc && load(dir, "vs.ctl", vctl, sizeof vctl) == sizeof vctl && load(dir, "ps.ctl", pctl, sizeof pctl) == sizeof pctl;
    NVNprogram prog, cprog; NVNvertexAttribState va[2], va2[2]; NVNvertexStreamState vs, vs2[2]; static uint8_t cctl[0x880];
    int comp = dir && load(dir, "cs.code", ex + 0x4000, 0x1000) && load(dir, "cs.ctl", cctl, sizeof cctl) == sizeof cctl;
    if (comp) { P(nvnProgramInitialize)(&cprog, &g_dev); NVNshaderData cd = {exa + 0x4000, cctl}; comp = P(nvnProgramSetShaders)(&cprog, 1, &cd); }
    static uint8_t tctl[0x880]; NVNprogram tprog;
    int tfb = tri && load(dir, "vstf.code", ex + 0x7000, 0x1000) && load(dir, "vstf.ctl", tctl, sizeof tctl) == sizeof tctl;
    if (tfb) { P(nvnProgramInitialize)(&tprog, &g_dev); NVNshaderData td[2] = {{exa + 0x7000, tctl}, {exa + 0x1000, pctl}}; tfb = P(nvnProgramSetShaders)(&tprog, 2, td); }
    static uint8_t abctl[0x880]; NVNprogram abprog;
    int ab = tri && load(dir, "vsab.code", ex + 0xD000, 0x1000) && load(dir, "vsab.ctl", abctl, sizeof abctl) == sizeof abctl;
    if (ab) { P(nvnProgramInitialize)(&abprog, &g_dev); NVNshaderData sd2[2] = {{exa + 0xD000, abctl}, {exa + 0x1000, pctl}}; ab = P(nvnProgramSetShaders)(&abprog, 2, sd2); }
    // vsint.code: the vertex shader reading its colour attribute as integers (I2F), as skinning shaders read bone indices; with it the colour stream is RGBA8UI
    static uint8_t ictl[0x880]; NVNprogram iprog;
    int vi = tri && load(dir, "vsint.code", ex + 0xE000, 0x1000) && load(dir, "vsint.ctl", ictl, sizeof ictl) == sizeof ictl;
    if (vi) { P(nvnProgramInitialize)(&iprog, &g_dev); NVNshaderData sd3[2] = {{exa + 0xE000, ictl}, {exa + 0x1000, pctl}}; vi = P(nvnProgramSetShaders)(&iprog, 2, sd3); }
    static const uint32_t ind[4] = {3, 1, 0, 0}; memcpy(ex + 0x5000, ind, sizeof ind);   // DrawArraysIndirect: count, instances, first, base instance
    if (tri) {
        P(nvnProgramInitialize)(&prog, &g_dev);
        NVNshaderData sd[2] = {{exa, vctl}, {exa + 0x1000, pctl}};
        tri = P(nvnProgramSetShaders)(&prog, 2, sd);
        static const float v[3][8] = {{-0.6f, -0.6f, 0.5f, 1, 1, 0, 0, 0}, {0.6f, -0.6f, 0.5f, 1, 0, 1, 0, 0}, {0, 0.7f, 0.5f, 1, 1, 1, 0, 0}};   // position, then red / green
        memcpy(ex + 0x2000, v, sizeof v);
        static const float ub[4] = {0, 0, 0.8f, 1}; memcpy(ex + 0x3000, ub, sizeof ub);   // c[3][8], c[3][0xC]: blue, alpha
        for (int i = 0; i < 2; i++) { P(nvnVertexAttribStateSetDefaults)(&va[i]); P(nvnVertexAttribStateSetFormat)(&va[i], NVN_FORMAT_RGBA32F, 16 * i); P(nvnVertexAttribStateSetStreamIndex)(&va[i], 0); }
        P(nvnVertexStreamStateSetDefaults)(&vs); P(nvnVertexStreamStateSetStride)(&vs, 32);
        // the last frame reads the colour from a stream of its own as RGBA16_UI2F (integers read as their float value: a format the Adreno takes as no vertex format)
        static const uint16_t col[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}}; memcpy(ex + 0x2100, col, sizeof col);
        static const uint8_t coli[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}}; memcpy(ex + 0x2200, coli, sizeof coli);
        P(nvnVertexAttribStateSetDefaults)(&va2[0]); P(nvnVertexAttribStateSetFormat)(&va2[0], NVN_FORMAT_RGBA32F, 0); P(nvnVertexAttribStateSetStreamIndex)(&va2[0], 0);
        P(nvnVertexAttribStateSetDefaults)(&va2[1]); P(nvnVertexAttribStateSetFormat)(&va2[1], NVN_FORMAT_RGBA16_UI2F, 0); P(nvnVertexAttribStateSetStreamIndex)(&va2[1], 1);
        P(nvnVertexStreamStateSetDefaults)(&vs2[0]); P(nvnVertexStreamStateSetStride)(&vs2[0], 32); P(nvnVertexStreamStateSetDefaults)(&vs2[1]); P(nvnVertexStreamStateSetStride)(&vs2[1], 8);
    }
    LOG("nvn demo: %s", tri ? "clear and a triangle each frame" : "clear only (no synthetic shaders in DM_SPV_DIR)");
    static uint8_t ctl[16384];
    NVNsync acq; P(nvnSyncInitialize)(&acq, &g_dev);
    int presented = 0, last = 0;
    for (int fr = 0; fr < frames; fr++) {
        int idx = 0; P(nvnWindowAcquireTexture)(&win, &acq, &idx);
        P(nvnSyncWait)(&acq, ~0ull);
        P(nvnCommandBufferAddCommandMemory)(&cb, &pool, (ptrdiff_t)(2 * tsz), 256u << 10);
        P(nvnCommandBufferAddControlMemory)(&cb, ctl, sizeof ctl);
        P(nvnCommandBufferBeginRecording)(&cb);
        const NVNtexture *rt = &tex[idx];
        P(nvnCommandBufferSetRenderTargets)(&cb, 1, &rt, NULL, NULL, NULL);
        float t = fr / 60.f, c[4] = {0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.1f), 0.5f + 0.5f * sinf(t + 4.2f), 1.f};
        P(nvnCommandBufferClearColor)(&cb, 0, c, NVN_CLEAR_COLOR_MASK_RGBA);
        if (tri) {
            P(nvnCommandBufferBindPolygonState)(&cb, &ps);
            P(nvnCommandBufferBindProgram)(&cb, vi && fr == frames - 1 ? &iprog : ab && fr == frames - 2 ? &abprog : &prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
            if (vi && fr == frames - 1) {   // the integer colour: RGBA8UI, 4 bytes a vertex
                P(nvnVertexAttribStateSetFormat)(&va2[1], NVN_FORMAT_RGBA8UI, 0); P(nvnVertexStreamStateSetStride)(&vs2[1], 4);
            }
            if (fr == frames - 1) { P(nvnCommandBufferBindVertexAttribState)(&cb, 2, va2); P(nvnCommandBufferBindVertexStreamState)(&cb, 2, vs2); P(nvnCommandBufferBindVertexBuffer)(&cb, 1, vi ? exa + 0x2200 : exa + 0x2100, vi ? 12 : 24); }
            else { P(nvnCommandBufferBindVertexAttribState)(&cb, 2, va); P(nvnCommandBufferBindVertexStreamState)(&cb, 1, &vs); }
            P(nvnCommandBufferBindVertexBuffer)(&cb, 0, exa + 0x2000, 96);
            P(nvnCommandBufferBindUniformBuffer)(&cb, NVN_SHADER_STAGE_FRAGMENT, 0, exa + 0x3000, 256);
            if (fr == frames - 1) P(nvnCommandBufferDrawArraysIndirect)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, exa + 0x5000);
            else P(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        }
        static const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
        P(nvnCommandBufferSetScissor)(&cb, 1200, 650, 80, 70); P(nvnCommandBufferClearColor)(&cb, 0, green, NVN_CLEAR_COLOR_MASK_RGBA);
        P(nvnCommandBufferSetScissor)(&cb, 1200, 0, 80, 70); P(nvnCommandBufferClearColor)(&cb, 0, red, NVN_CLEAR_COLOR_MASK_R_BIT);
        P(nvnCommandBufferSetScissor)(&cb, 0, 0, W, H);
        if (comp) {
            memset(ex + 0x6000, 0, 16);
            P(nvnCommandBufferBindProgram)(&cb, &cprog, NVN_SHADER_STAGE_COMPUTE_BIT);
            P(nvnCommandBufferBindStorageBuffer)(&cb, NVN_SHADER_STAGE_COMPUTE, 0, exa + 0x6000, 16);
            P(nvnCommandBufferDispatchCompute)(&cb, 5, 1, 1);
        }
        if (tfb && fr == frames - 1) {
            memset(ex + 0x8000, 0xee, 64);
            P(nvnCommandBufferBindProgram)(&cb, &tprog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
            P(nvnCommandBufferBindVertexAttribState)(&cb, 2, va); P(nvnCommandBufferBindVertexStreamState)(&cb, 1, &vs);   // the float colour again (the frame's draw read it as RGBA8UI)
            P(nvnCommandBufferSetRasterizerDiscard)(&cb, 1);
            P(nvnCommandBufferBindTransformFeedbackBuffer)(&cb, 0, exa + 0x8000, 36);
            P(nvnCommandBufferBeginTransformFeedback)(&cb, exa + 0x8100);
            P(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
            P(nvnCommandBufferEndTransformFeedback)(&cb, exa + 0x8100);
            P(nvnCommandBufferSetRasterizerDiscard)(&cb, 0);
        }
        NVNcommandHandle h = P(nvnCommandBufferEndRecording)(&cb);
        P(nvnQueueSubmitCommands)(&q, 1, &h);
        if (comp) { uint32_t mark = 0xc0ffee; memcpy(ex + 0x600c, &mark, 4); }   // the CPU writes a word the dispatch does not, while the GPU may still run it: it must stay
        P(nvnQueuePresentTexture)(&q, &win, idx); last = idx;
        presented++;
    }
    P(nvnQueueFinish)(&q);
    probe(&q, &cb, &pool, mem, P(nvnMemoryPoolGetBufferAddress)(&pool), 2 * tsz + cmd_sz, ctl, sizeof ctl);
    if (ab) LOG("nvn demo: the frame before the last drawn with the two-part vertex shader");
    if (vi) LOG("nvn demo: the last frame's colour read as RGBA8UI by a shader that takes it as integers (the centre must stay ffccbbba / ffccbaba)");
    // what the last frame holds: the triangle covers the centre, the clear colour a corner. With NVN's lower-left window origin, memory row 0 is NDC y = -1:
    // at NDC x = 0.3 the triangle (apex up, at y = 0.7) covers y = -0.5 (row 180) and not y = 0.5 (row 540); with the upper-left origin the rows are the other way
    uint32_t px[4] = {0}; NVNcopyRegion at[4] = {{W / 2, H / 2, 0, 1, 1, 1}, {8, 8, 0, 1, 1, 1}, {832, 180, 0, 1, 1, 1}, {832, 540, 0, 1, 1, 1}};
    for (int i = 0; i < 4 && presented; i++) P(nvnTextureReadTexels)(&tex[last], NULL, &at[i], &px[i]);
    LOG("nvn demo%s%s: last frame RGBA8 (little-endian ABGR) centre %08x corner %08x; x 0.3: row 180 %08x, row 540 %08x", mode ? " mode " : "", mode ? mode : "", px[0], px[1], px[2], px[3]);
    uint32_t sc[2] = {0}; NVNcopyRegion sr[2] = {{1240, 690, 0, 1, 1, 1}, {1240, 30, 0, 1, 1, 1}};
    for (int i = 0; i < 2 && presented; i++) P(nvnTextureReadTexels)(&tex[last], NULL, &sr[i], &sc[i]);
    LOG("nvn demo: scissored clear %08x (want ff00ff00), red-only clear %08x (want the corner with red ff)", sc[0], sc[1]);
    if (comp) { uint32_t w[4]; memcpy(w, ex + 0x6000, 16); LOG("nvn demo: compute wrote %08x %u (want 600df00d 5), the CPU's word beside it %08x (want 00c0ffee)", w[0], w[1], w[3]); }
    if (tfb) { float f[9]; uint32_t after; memcpy(f, ex + 0x8000, 36); memcpy(&after, ex + 0x8024, 4);
        LOG("nvn demo: transform feedback captured (%g %g %g) (%g %g %g) (%g %g %g), want (1 0 0) (0 1 0) (1 1 0); the word after %08x (want eeeeeeee)", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], after); }
    LOG("nvn demo: %d frames cleared and presented through the NVN window", presented);
    return presented;
}
