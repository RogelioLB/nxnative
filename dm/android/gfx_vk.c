// NVN replay on Vulkan for Android: the interface of nvn_mtl.m (nvn_mtl.h, nvn_obj.h) on a Vulkan device.
// Stage 1 (this file): textures, copies, clears, CPU reads and presents. Submissions run synchronously: the replay records a command buffer and
// submits it at commit (or earlier, when the CPU needs what the GPU wrote), waits for it, then makes the CPU-visible effects in command order, as
// nvn_mtl.m's completion handlers do (events, counter reports, fences). Draws and dispatches are not rendered yet (no shader translation for Vulkan):
// they are counted and reported once.
//   Memory: Vulkan cannot address guest memory on GPUs without VK_EXT_external_memory_host (the Adreno 6xx), so pool data goes through host-visible
//     staging buffers: copied in when a command reads it (at replay time, which in this synchronous model is the GPU's time), and written back after the
//     submission completes when a command writes it (texture-to-buffer copies). A CPU operation of the replay (buffer copies, fills, uniform updates)
//     first waits for those write-backs.
//   Textures: one VkImage per NVN texture serial, always in VK_IMAGE_LAYOUT_GENERAL, zeroed at creation (fresh Switch memory reads as 0). Textures whose
//     content lives in guest memory (packaged layouts, CPU writes) are deswizzled on the CPU and uploaded, their pages write-protected (nvn_dirty.c), and
//     uploaded again on their next use after a write, unless only the pages they share with other data were written and their own bytes there are
//     unchanged (nvn_mtl.m pkg_edges_clean). BC textures are decoded on the CPU (dm/third_party/bcdec) when the GPU has no BC support.
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_timestamp.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "nvn_fmt_mtl.h"
#include "gfx_vk.h"
#include "spv_readonly.h"
#include <stdatomic.h>
#include <math.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"

static int vk_debug(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_DEBUG") != NULL; return v; }   // DM_VK_DEBUG=1: a log line per upload and texture-to-buffer copy
#define RED_ONCE(id, ...) do { static int once_; if (!once_++) LOG("[RED] [vk] " __VA_ARGS__); } while (0)
static pthread_mutex_t g_bk = PTHREAD_MUTEX_INITIALIZER;   // the replay is one thread's at a time: submissions, CPU reads, texture lifetimes

// ---------------- The device ----------------
static VkPhysicalDeviceFeatures g_vk_feat; static int g_divisor;   // what the device was created with; VK_EXT_vertex_attribute_divisor   // what the device was created with
static vkb_ctx g_vk; static pthread_once_t g_vk_once = PTHREAD_ONCE_INIT; static int g_vk_ok;
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static void vk_create(void) {
    pthread_mutex_init(&g_vk.qmu, NULL);
    uint32_t ni = 0; vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties *ie = calloc(ni ? ni : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
    const char *iext[2]; uint32_t nie = 0;
    if (has_ext(ie, ni, "VK_KHR_surface") && has_ext(ie, ni, "VK_KHR_android_surface")) { iext[nie++] = "VK_KHR_surface"; iext[nie++] = "VK_KHR_android_surface"; g_vk.has_surface = 1; }
    free(ie);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = nie, .ppEnabledExtensionNames = iext};
    if (vkCreateInstance(&ic, NULL, &g_vk.inst) != VK_SUCCESS) { LOG("[RED] [vk] no Vulkan instance"); return; }
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(g_vk.inst, &n, &g_vk.pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) { LOG("[RED] [vk] no GPU"); return; }
    vkGetPhysicalDeviceProperties(g_vk.pd, &g_vk.props); vkGetPhysicalDeviceMemoryProperties(g_vk.pd, &g_vk.mp);
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(g_vk.pd, &f);
    g_vk.has_bc = f.textureCompressionBC; g_vk.has_astc = f.textureCompressionASTC_LDR;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, qp);
    for (g_vk.qf = 0; g_vk.qf < nq && !(qp[g_vk.qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_vk.qf++) {}
    if (g_vk.qf == nq) { LOG("[RED] [vk] no graphics queue"); return; }
    uint32_t nd = 0; vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nd, NULL);
    VkExtensionProperties *de = calloc(nd ? nd : 1, sizeof *de); vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nd, de);
    const char *dext[2]; uint32_t nde = 0;
    if (has_ext(de, nd, "VK_KHR_swapchain")) { dext[nde++] = "VK_KHR_swapchain"; g_vk.has_swapchain = 1; }
    int divisor = has_ext(de, nd, "VK_EXT_vertex_attribute_divisor");   // NVN vertex streams step per N instances (gfx_vk_draw.inc pipe_get)
    if (divisor) dext[nde++] = "VK_EXT_vertex_attribute_divisor";
    free(de);
    VkPhysicalDeviceFeatures want = {.textureCompressionBC = f.textureCompressionBC, .textureCompressionASTC_LDR = f.textureCompressionASTC_LDR,
                                     .textureCompressionETC2 = f.textureCompressionETC2, .independentBlend = f.independentBlend, .geometryShader = f.geometryShader,
                                     .depthClamp = f.depthClamp, .samplerAnisotropy = f.samplerAnisotropy, .fragmentStoresAndAtomics = f.fragmentStoresAndAtomics,
                                     .shaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat, .shaderStorageImageReadWithoutFormat = f.shaderStorageImageReadWithoutFormat, .fillModeNonSolid = f.fillModeNonSolid, .logicOp = f.logicOp,
                                     .depthBiasClamp = f.depthBiasClamp, .dualSrcBlend = f.dualSrcBlend, .tessellationShader = f.tessellationShader, .imageCubeArray = f.imageCubeArray,
                                     .shaderClipDistance = f.shaderClipDistance, .shaderCullDistance = f.shaderCullDistance, .shaderInt16 = f.shaderInt16, .shaderInt64 = f.shaderInt64,
                                     .shaderFloat64 = f.shaderFloat64, .vertexPipelineStoresAndAtomics = f.vertexPipelineStoresAndAtomics, .sampleRateShading = f.sampleRateShading,
                                     .shaderImageGatherExtended = f.shaderImageGatherExtended, .shaderStorageImageExtendedFormats = f.shaderStorageImageExtendedFormats,
                                     .multiDrawIndirect = f.multiDrawIndirect, .largePoints = f.largePoints, .wideLines = f.wideLines, .occlusionQueryPrecise = f.occlusionQueryPrecise};
    g_vk_feat = want;
    float pri = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qf, .queueCount = 1, .pQueuePriorities = &pri};
    // translated shaders declare DrawParameters (Ryujinx's base instance / vertex): a Vulkan 1.1 feature of its own
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT dv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceShaderDrawParametersFeatures dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .pNext = divisor ? &dv : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dp};
    if (VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 1 || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1) vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT dve = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .vertexAttributeInstanceRateDivisor = dv.vertexAttributeInstanceRateDivisor};
    VkPhysicalDeviceShaderDrawParametersFeatures dpe = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .pNext = divisor ? &dve : NULL, .shaderDrawParameters = dp.shaderDrawParameters};
    g_divisor = divisor && dv.vertexAttributeInstanceRateDivisor;
    if (!dp.shaderDrawParameters) LOG("[RED] [vk] no shaderDrawParameters: translated vertex shaders declare DrawParameters");
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dpe, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc, .enabledExtensionCount = nde, .ppEnabledExtensionNames = dext,
                             .pEnabledFeatures = &want};
    if (vkCreateDevice(g_vk.pd, &dc, NULL, &g_vk.dev) != VK_SUCCESS) { LOG("[RED] [vk] no Vulkan device"); return; }
    vkGetDeviceQueue(g_vk.dev, g_vk.qf, 0, &g_vk.q);
    LOG("[vk] descriptor sets bound at once: %u, subgroup size / ops / stages: see vulkan probe", g_vk.props.limits.maxBoundDescriptorSets);
    LOG("[vk] %s (Vulkan %u.%u.%u): BC %s, ASTC %s", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.has_bc ? "native" : "decoded on the CPU", g_vk.has_astc ? "native" : "missing");
    g_vk_ok = 1;
}
vkb_ctx *vkb(void) { pthread_once(&g_vk_once, vk_create); return g_vk_ok ? &g_vk : NULL; }
uint32_t vkb_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_vk.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_vk.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// ---------------- Staging buffers ----------------
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize n; } stg_t;
static int stg_new(stg_t *s, VkDeviceSize n) {
    memset(s, 0, sizeof *s); if (!n) n = 16;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &s->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, s->b, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX) mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &s->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }
    vkBindBufferMemory(g_vk.dev, s->b, s->m, 0);
    if (vkMapMemory(g_vk.dev, s->m, 0, n, 0, (void **)&s->p) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); return 0; }
    s->n = n; return 1;
}
static void stg_free(stg_t *s) { if (s->b) { vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); } memset(s, 0, sizeof *s); }

// ---------------- Submissions: chunks ----------------
// The commands go to the GPU in chunks: a command buffer with everything the GPU reads through it (staging buffers, the draws' arena, descriptor pools, texel
// buffer views), what is copied back to guest memory once it is done (write-backs), the events and counters it signals (posts), and the objects to destroy after
// it. A chunk is submitted without waiting (a submission's end, a present, a CPU read); the completion thread waits for chunks in order and then writes back,
// posts, completes the NVN fence and recycles the chunk. Write-backs come before posts: a game waiting on an event reads what the GPU wrote.
// A write-back with a snapshot (the guest bytes when the GPU took its copy) writes only the bytes that differ from it: the CPU may have written the others since.
// Before the replay reads guest memory on the CPU (an upload, a draw's data, a buffer copy), pending write-backs land first (cpu_view).
typedef struct { int st; size_t off, row_bytes, rows, src_pitch, dst_pitch; uint8_t *dst, *snap; int fwd; } wb_t;   // st < 0: the chunk's arena block -1 - st; fwd: a dispatch wrote it (later draws may read it on the GPU: gfx_vk_draw.inc fwd_find)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize n, used; } arena_blk;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; uint64_t q0, q1; } post_op;   // kind 1 event, 2 counter, 3 samples passed (occlusion queries q0..q1)
typedef struct { int kind; uint64_t h; } dead_t;
typedef struct { uint64_t src, dst, n; uint32_t value; int fill; } cop_t;   // a large buffer copy or fill, done by the completion thread in its place in the queue   // kind 1 image view, 2 framebuffer, 3 image, 4 device memory
typedef struct chunk {
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; int open, busy, ts, ndraw, held; uint64_t seq, nvn_fence, hold;
    VkQueryPool qp; int nq; uint64_t qg[1024];   // occlusion queries begun in it: query i counts NVN samples-passed slot qg[i] (occl_* below)
    VkQueryPool pq; int npq;   // render pass timestamps of a profiled frame (prof_* in gfx_vk_draw.inc)
    VkQueryPool tq; int ntq; uint64_t t_end;   // timestamps of the game's counter reports (kind 4 posts); the GPU time the chunk ended (g_tsq)   // hold: a later chunk reads this one's arena on the GPU (gfx_vk_draw.inc fwd_find): kept busy (held) until that one completes   // ndraw: draws and dispatches recorded (early submission: mtl_exec)   // ts: GPU timestamps written at its start and end (g_tsq)
    stg_t *stg; int nstg, capstg; wb_t *wb; int nwb, capwb; post_op *post; int npost, cappost; dead_t *dead; int ndead, capdead; cop_t *cop; int ncop, capcop;
    arena_blk *ar; int nar, car, ar_cur, ar_last;   // gfx_vk_draw.inc
    VkDescriptorPool *dp; int ndp, cdp, dp_cur; VkBufferView *bv; int nbv, cbv;
} chunk;
enum { NCHUNK = 4 };
static chunk g_ch[NCHUNK], *C;   // C: the chunk being recorded (NULL: none yet)
static chunk *g_fifo[NCHUNK]; static int g_fifo_n;   // submitted, oldest first
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
static uint64_t g_seq, g_seq_done; static _Atomic int g_inflight_wb, g_inflight_cop; static int g_drawing;   // g_drawing: a draw is being made (its resources are in C)
static void rp_end(void); static void views_drop(VkImage img); static void draw_chunk_reset(chunk *c);   // gfx_vk_draw.inc
static void *chunk_thread(void *arg);
// ---------------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------------
// NVN's samples passed is a counter on the queue: ResetCounter zeroes it, every draw after adds its samples, ReportCounter writes the sum at that point when the GPU gets
// there. Here, as nvn_mtl.m does with visibility buffers: while counting, draws are inside an occlusion query, one per render pass stretch (a query cannot outlive its
// pass), each a numbered slot; a report notes the slots since the reset and, when its chunk completes, writes their sum. The completion thread copies every chunk's query
// results into g_qres by slot first. The replay used to write 0: everything a game tests for visibility was fully hidden (nvn_mtl.m found culled lights the same way)
enum { QRES = 1 << 16 };
static uint64_t g_qres[QRES], g_qslot;   // results by slot (a ring), the next slot
static int g_q_on = 0, g_q_act = -1; static uint64_t g_q_win;   // counting since a reset; the query open in the current pass (chunk-local index); the window's first slot
static uint64_t g_q_reports, g_q_queries, g_ts_reports;
static int chunk_make(chunk *c) {
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qf};   // one pool per chunk: reset by the completion thread
    if (vkCreateCommandPool(g_vk.dev, &pc, NULL, &c->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1024};
    if (vkCreateQueryPool(g_vk.dev, &qc, NULL, &c->qp) != VK_SUCCESS) c->qp = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo tc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 256};
    if (!g_vk.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g_vk.dev, &tc, NULL, &c->tq) != VK_SUCCESS) c->tq = VK_NULL_HANDLE;
    tc.queryCount = 512; if (!g_vk.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g_vk.dev, &tc, NULL, &c->pq) != VK_SUCCESS) c->pq = VK_NULL_HANDLE;
    return vkAllocateCommandBuffers(g_vk.dev, &ca, &c->cb) == VK_SUCCESS && vkCreateFence(g_vk.dev, &fc, NULL, &c->fence) == VK_SUCCESS;
}
// GPU time: each chunk's command buffer writes a timestamp at its start and its end; the completion thread adds up the differences (the chunks run one after
// another on the queue, so the sum is the GPU's busy time). Logged with the memory line every 300 frames (mtl_mem_stat)
static VkQueryPool g_tsq; static _Atomic uint64_t g_gpu_ns, g_gpu_chunks;
static int chunks_init(void) {
    static int ok = -1; if (ok >= 0) return ok;
    ok = 0; if (!vkb()) return 0;
    if (g_vk.props.limits.timestampComputeAndGraphics) { VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * NCHUNK};
        if (vkCreateQueryPool(g_vk.dev, &qc, NULL, &g_tsq) != VK_SUCCESS) g_tsq = VK_NULL_HANDLE; }
    for (int i = 0; i < NCHUNK; i++) if (!chunk_make(&g_ch[i])) { LOG("[RED] [vk] command buffers could not be made"); return 0; }
    pthread_t th; if (pthread_create(&th, NULL, chunk_thread, NULL)) return 0;
    pthread_detach(th);
    return ok = 1;
}
static chunk *cur(void) {   // the chunk being recorded: a free one is taken (waiting for the oldest to complete when all are in flight)
    if (C) return C;
    if (!chunks_init()) return NULL;
    pthread_mutex_lock(&g_cmu);
    for (;;) { for (int i = 0; i < NCHUNK && !C; i++) if (!g_ch[i].busy) C = &g_ch[i]; if (C) break; pthread_cond_wait(&g_ccv, &g_cmu); }
    C->busy = 1; C->seq = ++g_seq; C->nvn_fence = 0; C->ndraw = 0;
    pthread_mutex_unlock(&g_cmu);
    return C;
}
static VkCommandBuffer cb(void) {
    if (!cur()) return VK_NULL_HANDLE;
    if (!C->open) { VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vkBeginCommandBuffer(C->cb, &bi); C->open = 1;
        if (g_tsq) { uint32_t q = 2 * (uint32_t)(C - g_ch); vkCmdResetQueryPool(C->cb, g_tsq, q, 2); vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, q); C->ts = 1; }
        if (C->qp) vkCmdResetQueryPool(C->cb, C->qp, 0, 1024); C->nq = 0;
        if (C->tq) vkCmdResetQueryPool(C->cb, C->tq, 0, 256); C->ntq = 0;
        if (C->pq) vkCmdResetQueryPool(C->cb, C->pq, 0, 512); C->npq = 0; }
    rp_end();   // transfers and clears run outside a draw's render pass (only the draw path records inside one, through C->cb)
    return C->cb;
}
// Every transfer / clear is ordered after everything before it (the images stay in GENERAL; one full barrier between operations; a chunk also ends with one, which
// orders it before every later submission on the queue)
static void cb_barrier(void) {
    if (!C || !C->open) return;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static stg_t *stg_take(VkDeviceSize n) {   // a staging buffer that lives until its chunk completes
    if (!cur()) return NULL;
    if (C->nstg == C->capstg) { C->capstg = C->capstg ? 2 * C->capstg : 32; C->stg = realloc(C->stg, C->capstg * sizeof *C->stg); }
    if (!stg_new(&C->stg[C->nstg], n)) { RED_ONCE(1, "staging buffer of %llu bytes failed", (unsigned long long)n); return NULL; }
    return &C->stg[C->nstg++];
}
static wb_t *wb_add(void) {
    if (!cur()) return NULL;
    if (C->nwb == C->capwb) { C->capwb = C->capwb ? 2 * C->capwb : 16; C->wb = realloc(C->wb, C->capwb * sizeof *C->wb); }
    return &C->wb[C->nwb++];
}
static void dead_add(int kind, uint64_t h) {   // destroyed when the current chunk completes (after every chunk before it, which may use it too)
    if (!h || !cur()) return;
    if (C->ndead == C->capdead) { C->capdead = C->capdead ? 2 * C->capdead : 32; C->dead = realloc(C->dead, C->capdead * sizeof *C->dead); }
    C->dead[C->ndead++] = (dead_t){kind, h};
}
static uint64_t g_flushes;
// Submit the current chunk (an empty one too: its fence then follows the earlier ones); nvn_fence: the NVN fence it completes. → its sequence number, 0 if none
static uint64_t chunk_submit(uint64_t nvn_fence) {
    if (!C) return 0;
    if (g_drawing) RED_ONCE(3, "a chunk was closed while a draw was being made: that draw's resources went with it");
    rp_end();
    int rec = C->open;   // a chunk whose command buffer was never begun is submitted with none: the fence alone
    if (rec) { cb_barrier(); if (C->ts) vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)(C - g_ch) + 1); vkEndCommandBuffer(C->cb); C->open = 0; }
    C->nvn_fence = nvn_fence;
    chunk *c = C; C = NULL;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = rec ? 1u : 0u, .pCommandBuffers = &c->cb};
    pthread_mutex_lock(&g_vk.qmu); VkResult r = vkQueueSubmit(g_vk.q, 1, &si, c->fence); pthread_mutex_unlock(&g_vk.qmu);
    if (r != VK_SUCCESS) { RED_ONCE(2, "vkQueueSubmit failed: %d", r); }
    g_flushes++;
    pthread_mutex_lock(&g_cmu);
    if (c->nwb) atomic_fetch_add(&g_inflight_wb, 1);
    if (c->ncop) atomic_fetch_add(&g_inflight_cop, 1);
    g_fifo[g_fifo_n++] = c; uint64_t seq = c->seq;
    pthread_cond_broadcast(&g_ccv);
    pthread_mutex_unlock(&g_cmu);
    return seq;
}
static void seq_wait(uint64_t seq) { pthread_mutex_lock(&g_cmu); while (g_seq_done < seq) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu); }
static void submit_wait(void) { uint64_t s = chunk_submit(0); if (s) seq_wait(s); }   // submit and wait until the completion thread has processed it
static void flush(void) { chunk_submit(0); }
static void all_wait(void) { uint64_t s = chunk_submit(0); seq_wait(s ? s : g_seq); }   // everything submitted so far completed (submit_wait waits for nothing when no chunk is open)   // send what is recorded, without waiting
// before the replay reads or writes guest memory on the CPU: the GPU's pending writes land first (this chunk's: it is submitted and waited for; earlier ones: waited for)
// cops: the large copies and fills queued for the completion thread land too (wb_view: only the GPU's writes, for a copy queued behind the others)
static uint64_t g_vsite[8][2];   // syncs that waited, by call site (0 other, 1 draw reads in flight, 2 texture upload, 3 texture-to-buffer copy, 4 large copy, 5 buffer texture, 6 buffer image, 7 dispatch): submitted / in flight
static int g_vs;   // the call site of the next view_
static void view_(int cops) {
    int site = g_vs; g_vs = 0;
    if (C && (C->nwb || (cops && C->ncop))) { g_vsite[site][0]++; submit_wait(); return; }
    if (!atomic_load(&g_inflight_wb) && !(cops && atomic_load(&g_inflight_cop))) return;
    g_vsite[site][1]++;
    pthread_mutex_lock(&g_cmu); while (atomic_load(&g_inflight_wb) || (cops && atomic_load(&g_inflight_cop))) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu);
}
static void cpu_view(void) { view_(1); }
static void wb_view(void) { view_(0); }
// Pending guest writes by range: a draw syncs only when what it reads overlaps a write-back or a queued copy not landed yet (draw_view in gfx_vk_draw.inc).
// A sync on anything pending (cpu_view) made every draw after one storage-buffer write wait for the GPU: Super Mario Bros. Wonder spent 30% of its render thread there.
// Guest CPU reads need no gate here: the game reads GPU results only after an NVN fence, which completes after the chunk's write-backs
// DM_VK_SAFE=1 (env.txt): the earlier, coarser syncs (every draw, dispatch and CPU write waits for anything pending; no reads on the GPU of another draw's data, no early
// submissions; window acquires wait for everything submitted, hle_nvn.c): to tell a regression of these from another cause
static int vk_safe(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_SAFE") != NULL; return v; }
static uint64_t g_view_cur, g_view_fly, g_view_free;   // draws that waited for the current chunk / for chunks in flight / that needed no wait
static int wb_hits(const wb_t *w, uintptr_t lo, uintptr_t hi) { uintptr_t a = (uintptr_t)w->dst, e = a + w->dst_pitch * (w->rows ? w->rows - 1 : 0) + w->row_bytes; return a < hi && lo < e; }
static int chunk_hits(const chunk *c, uintptr_t lo, uintptr_t hi, int write) {   // write: the CPU writes the range (a queued copy's source counts too: it is read when the copy runs)
    for (int i = 0; i < c->nwb; i++) if (wb_hits(&c->wb[i], lo, hi)) return 1;
    for (int i = 0; i < c->ncop; i++) { const cop_t *o = &c->cop[i]; if ((o->dst < hi && lo < o->dst + o->n) || (write && !o->fill && o->src < hi && lo < o->src + o->n)) return 1; }
    return 0;
}
typedef struct { uintptr_t lo, hi; } rd_rng;
// the guest memory the replay is about to read (write: or write) on the CPU: when something pending overlaps it, the sync is made (the current chunk submitted and
// waited for, or the chunks in flight waited for)
static void ranges_view_(const rd_rng *r, int n, int write) {
    int cur_hit = 0, fly_hit = 0;
    if (C) for (int i = 0; i < n && !cur_hit; i++) if (r[i].hi > r[i].lo && chunk_hits(C, r[i].lo, r[i].hi, write)) cur_hit = 1;
    if (cur_hit) { g_view_cur++; submit_wait(); return; }
    if (atomic_load(&g_inflight_wb) || atomic_load(&g_inflight_cop)) {
        pthread_mutex_lock(&g_cmu);
        for (int f = 0; f < g_fifo_n && !fly_hit; f++) for (int i = 0; i < n && !fly_hit; i++) if (r[i].hi > r[i].lo && chunk_hits(g_fifo[f], r[i].lo, r[i].hi, write)) fly_hit = 1;
        pthread_mutex_unlock(&g_cmu);
    }
    if (fly_hit) { g_view_fly++; g_vs = 1; cpu_view(); return; }
    g_view_free++;
}
static void ranges_view(const rd_rng *r, int n) { ranges_view_(r, n, 0); }
static uint64_t g_mem_cur, g_mem_fly, g_mem_free;   // the same for the submission's own CPU writes and small copies (uniform buffer updates, buffer copies and fills)
static void mem_view(uint64_t rd, uint64_t rn, uint64_t wr, uint64_t wn) {
    if (vk_safe()) { cpu_view(); return; }
    uint64_t c = g_view_cur, f = g_view_fly, e = g_view_free;
    rd_rng r[1] = {{(uintptr_t)rd, (uintptr_t)(rd + rn)}}, w[1] = {{(uintptr_t)wr, (uintptr_t)(wr + wn)}};
    if (rn) ranges_view_(r, 1, 0);
    if (wn) ranges_view_(w, 1, 1);
    g_mem_cur += g_view_cur - c; g_mem_fly += g_view_fly - f; g_mem_free += g_view_free - e; g_view_cur = c; g_view_fly = f; g_view_free = e;
}
static void wb_apply(const wb_t *w, const uint8_t *s) {
    dm_guest_write_prepare(w->dst, w->dst_pitch * (w->rows ? w->rows - 1 : 0) + w->row_bytes);   // pages the dirty tracker protected become writable (and dirty) first
    for (size_t y = 0; y < w->rows; y++) {
        uint8_t *d = w->dst + y * w->dst_pitch; const uint8_t *g = s + y * w->src_pitch;
        if (!w->snap) { memcpy(d, g, w->row_bytes); continue; }
        const uint8_t *o = w->snap + y * w->row_bytes;   // only what the GPU changed
        for (size_t i = 0; i < w->row_bytes;) {
            if (i + 8 <= w->row_bytes && !memcmp(g + i, o + i, 8)) { i += 8; continue; }
            size_t e = i; while (e < w->row_bytes && g[e] != o[e]) e++;
            if (e > i) { memcpy(d + i, g + i, e - i); i = e; } else i++;
        }
    }
}
static void post_apply(const post_op *o);   // with the submission code below
static void prof_collect(chunk *c);   // gfx_vk_draw.inc
static void chunk_done(chunk *c) {
    for (int i = 0; i < c->ncop; i++) {   // queued before any write-back of the chunk (a copy is queued only once the GPU's writes before it have landed)
        const cop_t *o = &c->cop[i]; dm_guest_write_prepare((void *)(uintptr_t)o->dst, o->n);
        if (!o->fill) memmove((void *)(uintptr_t)o->dst, (const void *)(uintptr_t)o->src, o->n);
        else { uint8_t *d = (uint8_t *)(uintptr_t)o->dst; for (uint64_t k = 0; k + 4 <= o->n; k += 4) memcpy(d + k, &o->value, 4); }
    }
    for (int i = 0; i < c->nwb; i++) { const wb_t *w = &c->wb[i]; wb_apply(w, (w->st >= 0 ? c->stg[w->st].p : c->ar[-1 - w->st].p) + w->off); free(c->wb[i].snap); }
    if (c->npq) prof_collect(c);
    if (c->nq) { uint64_t r[1024]; memset(r, 0, sizeof r);
        if (vkGetQueryPoolResults(g_vk.dev, c->qp, 0, (uint32_t)c->nq, sizeof r, r, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (int i = 0; i < c->nq; i++) g_qres[c->qg[i] % QRES] = r[i];
        c->nq = 0; }
    if (c->ntq) { uint64_t r[256]; memset(r, 0, sizeof r);
        int ok = c->t_end && vkGetQueryPoolResults(g_vk.dev, c->tq, 0, (uint32_t)c->ntq, sizeof r, r, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
        uint64_t g = guest_ns(), base = g ? g : clock_gettime_nsec_np(CLOCK_MONOTONIC);
        for (int i = 0; i < c->npost; i++) { post_op *o = &c->post[i]; if (o->kind != 4) continue;
            uint64_t back = ok && o->q0 < (uint64_t)c->ntq && r[o->q0] && r[o->q0] <= c->t_end ? (uint64_t)((double)(c->t_end - r[o->q0]) * g_vk.props.limits.timestampPeriod) : 0;
            o->q1 = nvn_timestamp_from_ns(back < base ? base - back : base); }
        c->ntq = 0; }
    for (int i = 0; i < c->npost; i++) post_apply(&c->post[i]);
    for (int i = 0; i < c->nstg; i++) stg_free(&c->stg[i]);
    for (int i = 0; i < c->ndead; i++) {
        uint64_t h = c->dead[i].h;
        switch (c->dead[i].kind) { case 1: vkDestroyImageView(g_vk.dev, (VkImageView)h, NULL); break; case 2: vkDestroyFramebuffer(g_vk.dev, (VkFramebuffer)h, NULL); break;
                                   case 3: vkDestroyImage(g_vk.dev, (VkImage)h, NULL); break; case 4: vkFreeMemory(g_vk.dev, (VkDeviceMemory)h, NULL); break; }
    }
    draw_chunk_reset(c);
    vkResetCommandPool(g_vk.dev, c->pool, 0); vkResetFences(g_vk.dev, 1, &c->fence);
    c->nstg = c->npost = c->ndead = 0;
}
static _Atomic uint64_t g_fence, g_done, g_presents;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static void *chunk_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-done");
    for (;;) {
        pthread_mutex_lock(&g_cmu); while (!g_fifo_n) pthread_cond_wait(&g_ccv, &g_cmu); chunk *c = g_fifo[0]; pthread_mutex_unlock(&g_cmu);
        vkWaitForFences(g_vk.dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
        c->t_end = 0;
        if (c->ts) { uint64_t t[2]; c->ts = 0;
            if (vkGetQueryPoolResults(g_vk.dev, g_tsq, 2 * (uint32_t)(c - g_ch), 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                { atomic_fetch_add(&g_gpu_ns, (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod)); atomic_fetch_add(&g_gpu_chunks, 1); c->t_end = t[1]; } }
        int had_wb = c->nwb != 0, had_cop = c->ncop != 0;
        chunk_done(c); c->nwb = c->ncop = 0;
        if (c->nvn_fence) { pthread_mutex_lock(&g_fmu); if (atomic_load(&g_done) < c->nvn_fence) atomic_store(&g_done, c->nvn_fence); pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu); }
        pthread_mutex_lock(&g_cmu);
        memmove(g_fifo, g_fifo + 1, --g_fifo_n * sizeof *g_fifo);
        if (had_wb) atomic_fetch_sub(&g_inflight_wb, 1);
        if (had_cop) atomic_fetch_sub(&g_inflight_cop, 1);
        g_seq_done = c->seq;
        if (c->hold > c->seq) c->held = 1; else { c->busy = 0; c->hold = 0; }
        for (int i = 0; i < NCHUNK; i++) if (g_ch[i].held && g_ch[i].hold <= g_seq_done) { g_ch[i].held = 0; g_ch[i].hold = 0; g_ch[i].busy = 0; }
        pthread_cond_broadcast(&g_ccv);
        pthread_mutex_unlock(&g_cmu);
    }
    return NULL;
}

// ---------------- Memory pools (GPU address = CPU address) ----------------
typedef struct { const uint8_t *lo, *hi; } pool_rng;
static pool_rng *g_pools; static int g_npool, g_cappool; static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = realloc(g_pools, g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = (pool_rng){mem, (const uint8_t *)mem + size};
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].lo == mem) { g_pools[i] = g_pools[--g_npool]; break; }
    pthread_mutex_unlock(&g_pool_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0; pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if ((const uint8_t *)addr >= g_pools[i].lo && (const uint8_t *)addr + len <= g_pools[i].hi) {
        if (buf) *buf = (void *)g_pools[i].lo; if (off) *off = addr - (uint64_t)g_pools[i].lo; ok = 1; break; }
    pthread_mutex_unlock(&g_pool_mu);
    return ok;
}
void vt_drop_pool(const void *pool);   // below, with the textures
void mtl_alias_drop_pool(const void *pool) { vt_drop_pool(pool); }

// ---------------- Formats ----------------
// MTLPixelFormat (the number nvn_fmt_to_mtl gives, see nvn_fmt_mtl.c) → VkFormat. Metal names packed formats from the least significant bits, Vulkan from the most
static VkFormat vk_of_mtl(uint32_t m) {
    switch (m) {
        case 10: return VK_FORMAT_R8_UNORM; case 12: return VK_FORMAT_R8_SNORM; case 13: return VK_FORMAT_R8_UINT; case 14: return VK_FORMAT_R8_SINT;
        case 20: return VK_FORMAT_R16_UNORM; case 22: return VK_FORMAT_R16_SNORM; case 23: return VK_FORMAT_R16_UINT; case 24: return VK_FORMAT_R16_SINT; case 25: return VK_FORMAT_R16_SFLOAT;
        case 30: return VK_FORMAT_R8G8_UNORM; case 32: return VK_FORMAT_R8G8_SNORM; case 33: return VK_FORMAT_R8G8_UINT; case 34: return VK_FORMAT_R8G8_SINT;
        case 40: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 41: return VK_FORMAT_R5G5B5A1_UNORM_PACK16; case 42: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 43: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case 53: return VK_FORMAT_R32_UINT; case 54: return VK_FORMAT_R32_SINT; case 55: return VK_FORMAT_R32_SFLOAT;
        case 60: return VK_FORMAT_R16G16_UNORM; case 62: return VK_FORMAT_R16G16_SNORM; case 63: return VK_FORMAT_R16G16_UINT; case 64: return VK_FORMAT_R16G16_SINT; case 65: return VK_FORMAT_R16G16_SFLOAT;
        case 70: return VK_FORMAT_R8G8B8A8_UNORM; case 71: return VK_FORMAT_R8G8B8A8_SRGB; case 72: return VK_FORMAT_R8G8B8A8_SNORM; case 73: return VK_FORMAT_R8G8B8A8_UINT; case 74: return VK_FORMAT_R8G8B8A8_SINT;
        case 80: return VK_FORMAT_B8G8R8A8_UNORM; case 81: return VK_FORMAT_B8G8R8A8_SRGB;
        case 90: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 91: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 92: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 93: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case 103: return VK_FORMAT_R32G32_UINT; case 104: return VK_FORMAT_R32G32_SINT; case 105: return VK_FORMAT_R32G32_SFLOAT;
        case 110: return VK_FORMAT_R16G16B16A16_UNORM; case 112: return VK_FORMAT_R16G16B16A16_SNORM; case 113: return VK_FORMAT_R16G16B16A16_UINT; case 114: return VK_FORMAT_R16G16B16A16_SINT; case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 123: return VK_FORMAT_R32G32B32A32_UINT; case 124: return VK_FORMAT_R32G32B32A32_SINT; case 125: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 130: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 131: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 132: return VK_FORMAT_BC2_UNORM_BLOCK; case 133: return VK_FORMAT_BC2_SRGB_BLOCK;
        case 134: return VK_FORMAT_BC3_UNORM_BLOCK; case 135: return VK_FORMAT_BC3_SRGB_BLOCK; case 140: return VK_FORMAT_BC4_UNORM_BLOCK; case 141: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 142: return VK_FORMAT_BC5_UNORM_BLOCK; case 143: return VK_FORMAT_BC5_SNORM_BLOCK; case 150: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 151: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case 152: return VK_FORMAT_BC7_UNORM_BLOCK; case 153: return VK_FORMAT_BC7_SRGB_BLOCK;
        case 250: return VK_FORMAT_D16_UNORM; case 252: return VK_FORMAT_D32_SFLOAT; case 253: return VK_FORMAT_S8_UINT; case 260: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default: break;
    }
    static const uint8_t astc_idx[15] = {0, 1, 2, 3, 4, 255, 5, 6, 7, 8, 9, 10, 11, 12, 13};   // Metal's ASTC numbers skip one between 6x6 and 8x5 (nvn_fmt_mtl.c astc_off)
    if (m >= 204 && m <= 218 && astc_idx[m - 204] != 255) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * astc_idx[m - 204]);
    if (m >= 186 && m <= 200 && astc_idx[m - 186] != 255) return (VkFormat)(VK_FORMAT_ASTC_4x4_SRGB_BLOCK + 2 * astc_idx[m - 186]);
    return VK_FORMAT_UNDEFINED;
}
// BC decoded on the CPU when the GPU has no BC: the format the decoder writes
enum { CONV_NONE, CONV_BC1, CONV_BC2, CONV_BC3, CONV_BC4, CONV_BC4S, CONV_BC5, CONV_BC5S, CONV_BC6H, CONV_BC6HS, CONV_BC7 };
static VkFormat bc_target(VkFormat f, int *conv, int *px) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: *conv = CONV_BC1; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: *conv = CONV_BC1; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC2_UNORM_BLOCK: *conv = CONV_BC2; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC2_SRGB_BLOCK: *conv = CONV_BC2; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC3_UNORM_BLOCK: *conv = CONV_BC3; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC3_SRGB_BLOCK: *conv = CONV_BC3; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK: *conv = CONV_BC4; *px = 1; return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK: *conv = CONV_BC4S; *px = 1; return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK: *conv = CONV_BC5; *px = 2; return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK: *conv = CONV_BC5S; *px = 2; return VK_FORMAT_R8G8_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: *conv = CONV_BC6H; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: *conv = CONV_BC6HS; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC7_UNORM_BLOCK: *conv = CONV_BC7; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC7_SRGB_BLOCK: *conv = CONV_BC7; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        default: *conv = CONV_NONE; *px = 0; return f;
    }
}
// Decode a w×h (texels) region of BC blocks (rows of src_pitch bytes) into px-byte texels (rows of dst_pitch bytes); edge blocks are clipped
static void bc_decode(int conv, const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dst_pitch, int px) {
    size_t bsz = conv == CONV_BC1 || conv == CONV_BC4 || conv == CONV_BC4S ? 8 : 16;
    uint8_t blk[4 * 4 * 8];
    for (uint32_t by = 0; by < (h + 3) / 4; by++) for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        const uint8_t *b = src + by * src_pitch + bx * bsz; int bp = 4 * px;
        switch (conv) {
            case CONV_BC1: bcdec_bc1(b, blk, bp); break;
            case CONV_BC2: bcdec_bc2(b, blk, bp); break;
            case CONV_BC3: bcdec_bc3(b, blk, bp); break;
            case CONV_BC4: bcdec_bc4(b, blk, bp, 0); break;
            case CONV_BC4S: bcdec_bc4(b, blk, bp, 1); break;
            case CONV_BC5: bcdec_bc5(b, blk, bp, 0); break;
            case CONV_BC5S: bcdec_bc5(b, blk, bp, 1); break;
            case CONV_BC6H: case CONV_BC6HS: {   // RGB half → RGBA half (alpha 1.0)
                uint16_t rgb[16 * 3]; bcdec_bc6h_half(b, rgb, 4 * 3, conv == CONV_BC6HS);
                for (int i = 0; i < 16; i++) { uint16_t *o = (uint16_t *)(void *)(blk + i * 8); o[0] = rgb[3 * i]; o[1] = rgb[3 * i + 1]; o[2] = rgb[3 * i + 2]; o[3] = 0x3C00; }
                break; }
            case CONV_BC7: bcdec_bc7(b, blk, bp); break;
        }
        for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++) {
            uint32_t n = w - bx * 4 < 4 ? w - bx * 4 : 4;
            memcpy(dst + (size_t)(by * 4 + y) * dst_pitch + (size_t)bx * 4 * px, blk + y * bp, (size_t)n * px);
        }
    }
}

// ---------------- Textures ----------------
typedef struct {
    uint64_t serial; VkImage img; VkDeviceMemory mem; VkFormat fmt, nvfmt; VkImageAspectFlags aspect; int conv, px;
    const void *pool; const uint8_t *store; nvn_texdesc desc;   // what it was made from: a serial met again with other storage or another description is made anew
    uint32_t w, h, d, layers, levels; VkImageType type; VkSampleCountFlagBits samples;
    int pkg; uint64_t stamp, seen; const void *span; uint8_t *edge[2]; uint32_t edge_n[2];
    int refs, shared, mutable_; uint64_t used;   // mutable_: views in another format of the same texel size (a texture view's format)   // texture objects (serials) resolving to it; in the sharing table; g_fence at its last lookup
} vtex;
static nvn_hmap g_tex = {.klen = 8}, g_cpuw = {.klen = 8};   // serial → vtex; serials the CPU wrote (WriteTexels). Keyed by the 8-byte serial
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_edge_skips, g_up_bytes, g_draws_skipped;
static unsigned tex_layers(const nvn_texdesc *d) { return d->target == 4 || d->target == 9 || d->target == 6 ? (d->depth ? d->depth : 1) : d->target == 8 ? 6 : d->target == 3 ? (d->height ? d->height : 1) : 1; }
static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
// Linear textures (LINEAR / LINEAR_RENDER_TARGET) are the game's memory as it writes it from the CPU: Super Mario Bros. Wonder's glyph cache (2048×8192 R8, IMAGE |
// LINEAR, its font engine rasterizes glyphs into it on the CPU) and sky colour ramps (64×1 RGBA16F written every frame); as GPU-only images they stayed empty and every
// menu and dialog text was blank (nvn_mtl.m tex_linear makes them on the pool memory, zero copy). Here they go the packaged way: uploaded from guest memory, again when
// its pages were written (rows at the texture's own stride: vt_upload)
static int is_linear(const tex_t *t) { return (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0; }
static int is_packaged(const tex_t *t) { return t->mem && (t->has_layout || is_linear(t) || nvn_hm_get(&g_cpuw, &t->serial)); }
void vkb_frame_forget(uint64_t serial);
static void vt_destroy(vtex *v) {   // with the replay's lock held; the front end may be drawing the image: it is taken off the presented frame first (vkb_frame_forget waits for that)
    if (!v) return;
    vkb_frame_forget(v->serial);
    if (v->img) views_drop(v->img);   // the image, its views and framebuffers go when the current chunk completes: recorded and submitted commands may use them
    dead_add(3, (uint64_t)v->img); dead_add(4, (uint64_t)v->mem);
    free(v->edge[0]); free(v->edge[1]); free(v);
}
static void vt_clear_new(vtex *v) {   // fresh Switch memory reads as 0
    VkCommandBuffer c = cb(); if (!c) return;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = v->img,
        .subresourceRange = {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageSubresourceRange rg = {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (v->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, v->fmt, &fp);
        int compressed = v->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && v->fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        if (!compressed) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    } else { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    cb_barrier();
}
static vtex *vt_create(const tex_t *t) {
    uint32_t m = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &m, swz) || !m) { RED_ONCE(10, "texture format %#x has no Vulkan counterpart yet (first case ser=%llu)", t->d.format, (unsigned long long)t->serial); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) { RED_ONCE(11, "buffer textures are not on Vulkan yet"); return NULL; }
    VkFormat nf = vk_of_mtl(m), f = nf; int conv = CONV_NONE, px = 0;
    if (nf == VK_FORMAT_UNDEFINED) { RED_ONCE(12, "Metal format %u not mapped to Vulkan (NVN format %#x)", m, t->d.format); return NULL; }
    if (nf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && nf <= VK_FORMAT_BC7_SRGB_BLOCK && !g_vk.has_bc) f = bc_target(nf, &conv, &px);
    if (nf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && nf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !g_vk.has_astc) { RED_ONCE(13, "ASTC texture on a GPU without ASTC: not decoded yet"); return NULL; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    if (f == VK_FORMAT_D32_SFLOAT_S8_UINT && !(ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {   // Depth32F_S8 → D24S8 where the GPU has no D32S8 attachment
        VkFormatProperties f2; vkGetPhysicalDeviceFormatProperties(g_vk.pd, VK_FORMAT_D24_UNORM_S8_UINT, &f2);
        if (f2.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { f = VK_FORMAT_D24_UNORM_S8_UINT; ff = f2.optimalTilingFeatures; }
    }
    if (!(ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) && !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { RED_ONCE(14, "format %d not usable as an image on this GPU (NVN format %#x)", f, t->d.format); return NULL; }
    vtex *v = calloc(1, sizeof *v); if (!v) return NULL;
    v->serial = t->serial; v->pool = t->pool; v->store = t->mem; v->desc = t->d; v->fmt = f; v->nvfmt = nf; v->conv = conv; v->px = px; v->aspect = aspect_of(f);
    v->type = t->d.target == 0 || t->d.target == 3 ? VK_IMAGE_TYPE_1D : t->d.target == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    v->w = t->d.width ? t->d.width : 1; v->h = v->type == VK_IMAGE_TYPE_1D ? 1 : (t->d.height ? t->d.height : 1); v->d = v->type == VK_IMAGE_TYPE_3D ? (t->d.depth ? t->d.depth : 1) : 1;
    v->layers = tex_layers(&t->d); v->levels = t->d.levels ? t->d.levels : 1;
    v->samples = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags u = 0;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    // an uncompressed colour image may be viewed in another format of its texel size (NVN texture views: sRGB over UNORM and the like): mutable
    int mut = v->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !conv && !(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    v->mutable_ = mut || (u & VK_IMAGE_USAGE_STORAGE_BIT);
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = v->type, .format = f, .extent = {v->w, v->h, v->d}, .mipLevels = v->levels,
        .arrayLayers = v->layers, .samples = v->samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .flags = ((t->d.target == 8 || t->d.target == 9) && v->layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                                           (u & VK_IMAGE_USAGE_STORAGE_BIT || mut ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) |   // storage: viewed in the format a shader declares (gfx_vk_draw.inc storage_view); colour: a texture view's format
                                                           (v->type == VK_IMAGE_TYPE_3D && (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0)};   // a 3D target is drawn a slice at a time through 2D views (gfx_vk_draw.inc rp_start)
    if (v->samples > 1) ic.mipLevels = 1;
    if (vkCreateImage(g_vk.dev, &ic, NULL, &v->img) != VK_SUCCESS) { RED_ONCE(15, "vkCreateImage %ux%ux%u ×%u levels %u format %d failed", v->w, v->h, v->d, v->layers, v->levels, f); free(v); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.dev, v->img, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt == UINT32_MAX ? vkb_mem_type(mr.memoryTypeBits, 0) : mt};
    if (vkAllocateMemory(g_vk.dev, &ma, NULL, &v->mem) != VK_SUCCESS) { RED_ONCE(16, "image memory of %llu bytes failed", (unsigned long long)mr.size); vkDestroyImage(g_vk.dev, v->img, NULL); free(v); return NULL; }
    vkBindImageMemory(g_vk.dev, v->img, v->mem, 0);
    vt_clear_new(v);
    return v;
}
// Upload a texture's guest storage (deswizzled on the CPU; BC decoded when the GPU lacks it). The dirty-page stamp is taken and the pages protected before reading (nvn_mtl.m pkg_upload_)
// texture uploads' CPU time (deswizzle, BC decode); pipelines made and their creation time: the long-frame lines (hle_nvn.c)
static _Atomic uint64_t g_up_ns, g_pso_made, g_pso_ns;
static _Atomic uint64_t g_mod_made, g_mod_ns, g_cpso_made, g_cpso_ns;   // shader modules (variants too) and compute pipelines made, and their creation time (gfx_vk_draw.inc): the driver compiles in both
static int vt_upload_(vtex *v, const tex_t *t);
static int vt_upload(vtex *v, const tex_t *t) { uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); int r = vt_upload_(v, t); atomic_fetch_add(&g_up_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); return r; }
static int vt_upload_(vtex *v, const tex_t *t) {
    g_vs = 2; cpu_view();
    v->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { RED_ONCE(20, "packaged texture format %#x has no block size", t->d.format); return 0; }
    size_t total = 0;
    for (unsigned l = 0; l < v->levels; l++) {
        uint32_t w = v->w >> l ? v->w >> l : 1, h = v->h >> l ? v->h >> l : 1, z = v->type == VK_IMAGE_TYPE_3D ? (v->d >> l ? v->d >> l : 1) : 1;
        size_t pitch = v->conv ? (size_t)w * v->px : (size_t)((w + bw - 1) / bw) * bpb, rows = v->conv ? h : (h + bh - 1) / bh;
        total += pitch * rows * z * v->layers;
    }
    stg_t *s = stg_take(total); if (!s) return 0;
    VkCommandBuffer c = cb(); if (!c) return 0;
    uint8_t *tmp = NULL; size_t off = 0;
    for (unsigned l = 0; l < v->levels; l++) {
        uint32_t w = v->w >> l ? v->w >> l : 1, h = v->h >> l ? v->h >> l : 1, z = v->type == VK_IMAGE_TYPE_3D ? (v->d >> l ? v->d >> l : 1) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < v->layers; y++) {
            size_t out = v->conv ? (size_t)w * v->px * h * z : pitch * rows * z;
            if (v->conv) {
                tmp = realloc(tmp, pitch * rows * z);
                if (nvn_deswizzle(&t->d, l, y, t->mem, tmp, pitch)) { RED_ONCE(21, "deswizzle failed format %#x target %d", t->d.format, t->d.target); free(tmp); return 0; }
                for (uint32_t k = 0; k < z; k++) bc_decode(v->conv, tmp + k * pitch * rows, pitch, w, h, s->p + off + (size_t)k * w * v->px * h, (size_t)w * v->px, v->px);
            } else if (is_linear(t)) {   // linear: block rows at the texture's stride (its layer's bytes over its block rows, as GetStorageSize gives them)
                if (l || z > 1) { RED_ONCE(22, "linear texture with levels or depth: only level 0 of a 2D one is read"); off += out; continue; }
                size_t srow = t->L.layer_size / (rows ? rows : 1); const uint8_t *src = t->mem + (size_t)y * t->L.layer_size;
                if (srow < pitch) srow = pitch;
                for (size_t r = 0; r < rows; r++) memcpy(s->p + off + r * pitch, src + r * srow, pitch);
            } else if (nvn_deswizzle(&t->d, l, y, t->mem, s->p + off, pitch)) { RED_ONCE(21, "deswizzle failed format %#x target %d", t->d.format, t->d.target); free(tmp); return 0; }
            VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {v->aspect, l, y, 1}, .imageExtent = {w, h, z}};
            vkCmdCopyBufferToImage(c, s->b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            off += out;
        }
    }
    free(tmp); cb_barrier();
    if (vk_debug()) LOG("[vk] upload ser=%llu mem=%p size %#llx levels %u layers %u total %zu first bytes %02x %02x %02x %02x", (unsigned long long)v->serial, t->mem, (unsigned long long)t->L.size, v->levels, v->layers, total, s->p[0], s->p[1], s->p[2], s->p[3]);
    g_up_bytes += total;
    v->span = nvn_dirty_span(t->mem, t->L.size); v->seen = v->stamp;
    return 1;
}
// Writes beside a texture (nvn_mtl.m pkg_edge_ranges / pkg_edges_take / pkg_edges_clean): the texture's bytes in the host pages it shares with other data
static void edge_ranges(const tex_t *t, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size;
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static void edges_take(vtex *v, const tex_t *t) {
    for (int i = 0; i < 2; i++) { free(v->edge[i]); v->edge[i] = NULL; v->edge_n[i] = 0; }
    if (!v->span) return;
    uintptr_t r[2][2], ia, ie; edge_ranges(t, r, &ia, &ie);
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0]) { v->edge_n[i] = (uint32_t)(r[i][1] - r[i][0]); v->edge[i] = malloc(v->edge_n[i]); memcpy(v->edge[i], (const void *)r[i][0], v->edge_n[i]); }
    if (nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp)) for (int i = 0; i < 2; i++) { free(v->edge[i]); v->edge[i] = NULL; v->edge_n[i] = 0; }
}
static int edges_clean(vtex *v, const tex_t *t) {
    if (!v->edge_n[0] && !v->edge_n[1]) return 0;
    uintptr_t r[2][2], ia, ie; edge_ranges(t, r, &ia, &ie);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);
    if (ie > ia && nvn_dirty_since_span(v->span, (const void *)ia, ie - ia, v->stamp)) return 0;
    for (int i = 0; i < 2; i++) if (v->edge_n[i] && memcmp(v->edge[i], (const void *)r[i][0], v->edge_n[i])) return 0;
    v->stamp = st; g_pkg_edge_skips++;
    return 1;
}
// Textures shared by storage (nvn_mtl.m g_alias): an NVN texture object is only a descriptor, and an object made again over the same storage with the same
// description is the same texture. Games make their transient render targets on new objects every frame (Super Mario Bros. Wonder never finalizes them) and
// sample what was drawn through another object: one image per serial left those reads empty and leaked an image a frame. A GPU-made texture (not packaged) is
// keyed by pool, offset and description; each serial resolving to it holds a reference, and the image stays while its pool lives or until another description
// takes over its storage (share_take_over).
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } share_key;
static nvn_hmap g_share = {.klen = sizeof(share_key)};
static share_key share_of(const tex_t *t) { share_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d; return k; }
static void vt_unref(vtex *v) { if (--v->refs <= 0 && !v->shared) vt_destroy(v); }
static void vt_ref(uint64_t serial, vtex *v) { nvn_hm_put(&g_tex, &serial, v); v->refs++; }
static uint64_t g_share_taken;
// A new shared texture takes over storage: entries of the same pool overlapping it under another description go, unless an object still resolves to them or
// a lookup found them in the last 16 submissions (targets sharing storage within a frame must not evict each other: nvn_mtl.m ALIAS_IDLE)
static void share_take_over(const share_key *nk) {
    nvn_texlayout L; nvn_tex_layout(&nk->d, &L); if (!L.size) return;
    int64_t lo = nk->off, hi = nk->off + (int64_t)L.size;
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vtex *v = g_share.vals[i]; if (!v) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k);
        if (k.pool != nk->pool || !memcmp(&k, nk, sizeof k) || k.off >= hi || v->refs > 0 || atomic_load(&g_fence) - v->used < 16) continue;
        nvn_texlayout kl; nvn_tex_layout(&k.d, &kl);
        if (k.off + (int64_t)kl.size <= lo) continue;
        nvn_hm_del(&g_share, &k); v->shared = 0; vt_destroy(v); g_share_taken++;
        i = (uint32_t)-1;   // the table moved entries on delete: scan again
    }
}
// The image of a texture: made at first use; a texture whose content lives in guest memory is uploaded, and uploaded again when its pages were written
static vtex *vt_get(const tex_t *ct) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial || !vkb()) return NULL;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(30, "texture object no longer valid (magic %#x)", t->magic); return NULL; }
    vtex *v = nvn_hm_get(&g_tex, &t->serial);
    if (v && (v->store != t->mem || memcmp(&v->desc, &t->d, sizeof v->desc))) {
        static int n; if (n++ < 8) LOG("[vk] texture ser=%llu met again with %s: made anew (storage %p, now %p)", (unsigned long long)t->serial, v->store != t->mem ? "other storage" : "another description", (const void *)v->store, (const void *)t->mem);
        nvn_hm_del(&g_tex, &t->serial); vt_unref(v); v = NULL; }
    int pkg = is_packaged(t);
    if (v && v->pkg) {
        uint64_t now = nvn_dirty_now();
        int clean = v->span && (now == v->seen || !nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp));
        v->seen = now;
        if (clean || edges_clean(v, t)) return v;
        if (vt_upload(v, t)) { g_pkg_reuploads++; edges_take(v, t); }
        return v;
    }
    if (v && !pkg) { v->used = atomic_load(&g_fence); return v; }
    if (v) { nvn_hm_del(&g_tex, &t->serial); vt_unref(v); }   // written by the CPU since it was made as a GPU texture: made again from guest memory, its own
    share_key k = share_of(t);
    if (!pkg && t->pool) {   // another object over the same storage, the same description: the same texture
        vtex *s = nvn_hm_get(&g_share, &k);
        if (s) { vt_ref(t->serial, s); s->used = atomic_load(&g_fence); return s; }
    }
    if (!(v = vt_create(t))) return NULL;
    v->used = atomic_load(&g_fence);
    if (pkg) { v->pkg = 1; if (vt_upload(v, t)) { g_pkg_uploads++; edges_take(v, t); } }
    else if (t->pool) { share_take_over(&k); nvn_hm_put(&g_share, &k, v); v->shared = 1; }
    vt_ref(t->serial, v);
    return v;
}
// A memory pool's Finalize (mtl_alias_drop_pool): the textures made on its storage go, as nvn_mtl.m evicts its shared textures; their serials may be met again on other storage
void vt_drop_pool(const void *pool) {
    void vkb_frame_forget(uint64_t serial);
    pthread_mutex_lock(&g_bk);
    for (uint32_t i = 0; i < g_tex.cap; i++) {   // the objects' references, then the sharing table's entries
        vtex *v = g_tex.vals[i];
        if (!v || v->pool != pool) continue;
        uint64_t k; memcpy(&k, g_tex.keys + (size_t)i * g_tex.klen, 8); nvn_hm_del(&g_tex, &k); vt_unref(v);
        i = (uint32_t)-1;   // the table may have moved entries on delete: scan again
    }
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vtex *v = g_share.vals[i];
        if (!v || v->pool != pool) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k); nvn_hm_del(&g_share, &k); v->shared = 0; if (v->refs <= 0) vt_destroy(v);
        i = (uint32_t)-1;
    }
    pthread_mutex_unlock(&g_bk);
}
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    void vkb_frame_forget(uint64_t serial);
    pthread_mutex_lock(&g_bk);
    vtex *v = nvn_hm_del(&g_tex, &t->serial); nvn_hm_del(&g_cpuw, &t->serial);
    if (v) vt_unref(v);   // a shared image stays for the next object over its storage
    pthread_mutex_unlock(&g_bk);
}
void mtl_tex_cpu_written(const tex_t *t) { pthread_mutex_lock(&g_bk); if (!nvn_hm_get(&g_cpuw, &t->serial)) nvn_hm_put(&g_cpuw, &t->serial, (void *)1); pthread_mutex_unlock(&g_bk); }
int mtl_tex_gpu_owned(uint64_t serial) { pthread_mutex_lock(&g_bk); vtex *v = nvn_hm_get(&g_tex, &serial); int r = v && !v->pkg; pthread_mutex_unlock(&g_bk); return r; }
static void reg_tex(int id, const tex_t *t, const view_t *v); static void reg_samp(int id, const samp_par *p);   // gfx_vk_draw.inc
void mtl_reg_tex(int id, const tex_t *t, const view_t *vw) { reg_tex(id, t, vw); }
void mtl_reg_samp(int id, const samp_par *p) { reg_samp(id, p); }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }

// ---------------- Copies ----------------
static struct { int64_t row, img; } S;   // SetCopyRowStride / SetCopyImageStride
static int tex_level_layer(const vtex *v, const view_t *vw, int has, unsigned *level, unsigned *layer0) {
    *level = has && vw->set_levels ? vw->base_level : 0; *layer0 = has && vw->set_layers ? vw->min_layer : 0;
    return *level < v->levels;
}
static void copy_b2t(const p_copy_b2t *c) {
    vtex *v = vt_get(c->tex); if (!v) return;
    unsigned level, layer0; if (!tex_level_layer(v, &c->view, c->has_view, &level, &layer0)) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    size_t rb = (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh;
    size_t row = S.row ? (size_t)S.row : rb, img = S.img ? (size_t)S.img : row * rows;
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = c->r.depth ? c->r.depth : 1;
    if (!mtl_addr(c->src, img * (nz - 1) + row * (rows - 1) + rb, NULL, NULL)) { RED_ONCE(40, "buffer→texture copy source is in no memory pool"); return; }
    g_vs = 3; cpu_view();
    size_t out_row = v->conv ? (size_t)c->r.width * v->px : rb, out_rows = v->conv ? (size_t)c->r.height : rows;
    stg_t *s = stg_take(out_row * out_rows * nz); VkCommandBuffer cbuf = cb(); if (!s || !cbuf) return;
    for (int z = 0; z < nz; z++) {
        const uint8_t *src = (const uint8_t *)c->src + (size_t)z * img; uint8_t *d = s->p + (size_t)z * out_row * out_rows;
        if (v->conv) bc_decode(v->conv, src, row, (uint32_t)c->r.width, (uint32_t)c->r.height, d, out_row, v->px);
        else for (size_t y = 0; y < rows; y++) memcpy(d + y * rb, src + y * row, rb);
        VkBufferImageCopy r = {.bufferOffset = (size_t)z * out_row * out_rows, .imageSubresource = {v->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? v->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : v->aspect, level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                               .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset + z : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, 1}};
        if (v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) RED_ONCE(41, "buffer→texture copy into a depth-stencil texture: only the depth is copied, as is");
        vkCmdCopyBufferToImage(cbuf, s->b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    cb_barrier();
}
static void copy_t2b(const p_copy_b2t *c) {
    vtex *v = vt_get(c->tex); if (!v) return;
    unsigned level, layer0; if (!tex_level_layer(v, &c->view, c->has_view, &level, &layer0)) return;
    if (v->conv) { RED_ONCE(42, "texture→buffer copy of a BC texture decoded on the CPU: not copied"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    size_t rb = (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh;
    size_t row = S.row ? (size_t)S.row : rb, img = S.img ? (size_t)S.img : row * rows;
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = c->r.depth ? c->r.depth : 1;
    if (!mtl_addr(c->src, img * (nz - 1) + row * (rows - 1) + rb, NULL, NULL)) { RED_ONCE(43, "texture→buffer copy destination is in no memory pool"); return; }
    stg_t *s = stg_take(rb * rows * nz); VkCommandBuffer cbuf = cb(); if (!s || !cbuf) return;
    int st = (int)(s - C->stg);
    if (vk_debug()) LOG("[vk] t2b ser=%llu pkg %d level %u layer %u region %d,%d %dx%dx%d row %zu img %zu -> %#llx", (unsigned long long)v->serial, v->pkg, level, layer0, c->r.xoffset, c->r.yoffset, c->r.width, c->r.height, nz, row, img, (unsigned long long)c->src);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy r = {.bufferOffset = (size_t)z * rb * rows, .imageSubresource = {v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : v->aspect, level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                               .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset + z : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, 1}};
        vkCmdCopyImageToBuffer(cbuf, v->img, VK_IMAGE_LAYOUT_GENERAL, s->b, 1, &r);
        wb_t *w = wb_add(); if (w) *w = (wb_t){st, (size_t)z * rb * rows, rb, rows, rb, row, (uint8_t *)c->src + (size_t)z * img, NULL};
    }
    cb_barrier();
}
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = vt_get(c->src), *d = vt_get(c->dst); if (!s || !d) return;
    unsigned sl, sz, dl, dz;
    if (!tex_level_layer(s, &c->sv, c->has_sv, &sl, &sz) || !tex_level_layer(d, &c->dv, c->has_dv, &dl, &dz)) return;
    VkCommandBuffer cbuf = cb(); if (!cbuf) return;
    int ns = c->sr.depth ? c->sr.depth : 1, nd = c->dr.depth ? c->dr.depth : 1;
    VkImageAspectFlags as = s->aspect, ad = d->aspect;
    if ((c->flags & DM_COPY_RESOLVE) && s->samples > 1) {
        VkImageResolve r = {.srcSubresource = {as, sl, sz + c->sr.zoffset, 1}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, 0},
                            .dstSubresource = {ad, dl, dz + c->dr.zoffset, 1}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, 0}, .extent = {(uint32_t)c->dr.width, (uint32_t)c->dr.height, 1}};
        vkCmdResolveImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); cb_barrier(); return;
    }
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && ns == nd && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT))
               && (s->fmt == d->fmt || (s->conv == d->conv && aspect_of(s->fmt) == aspect_of(d->fmt) && s->px == d->px));
    int s3 = s->type == VK_IMAGE_TYPE_3D, d3 = d->type == VK_IMAGE_TYPE_3D;
    if (same && s->samples == d->samples) {
        VkImageCopy r = {.srcSubresource = {as, sl, s3 ? 0 : sz + c->sr.zoffset, s3 ? 1 : (uint32_t)ns}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
                         .dstSubresource = {ad, dl, d3 ? 0 : dz + c->dr.zoffset, d3 ? 1 : (uint32_t)nd}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
                         .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 && d3 ? (uint32_t)ns : 1}};
        vkCmdCopyImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); cb_barrier(); return;
    }
    // a format change, a scaled or mirrored region: a blit, where both formats allow it
    VkFormatProperties fs, fd; vkGetPhysicalDeviceFormatProperties(g_vk.pd, s->fmt, &fs); vkGetPhysicalDeviceFormatProperties(g_vk.pd, d->fmt, &fd);
    if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || as != ad) {
        RED_ONCE(44, "texture copy with a format change or scaling between formats %d and %d: not blittable on this GPU", s->fmt, d->fmt); return; }
    int x0 = c->dr.xoffset, x1 = c->dr.xoffset + c->dr.width, y0 = c->dr.yoffset, y1 = c->dr.yoffset + c->dr.height;
    if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int k = x0; x0 = x1; x1 = k; }
    if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int k = y0; y0 = y1; y1 = k; }
    for (int z = 0; z < (s3 ? 1 : ns); z++) {
        VkImageBlit b = {.srcSubresource = {as, sl, s3 ? 0 : sz + c->sr.zoffset + z, 1}, .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, s3 ? c->sr.zoffset + ns : 1}},
                         .dstSubresource = {ad, dl, d3 ? 0 : dz + c->dr.zoffset + z, 1}, .dstOffsets = {{x0, y0, d3 ? c->dr.zoffset : 0}, {x1, y1, d3 ? c->dr.zoffset + nd : 1}}};
        vkCmdBlitImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && as == VK_IMAGE_ASPECT_COLOR_BIT && (fs.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    cb_barrier();
}
// CPU texture read: packaged textures from guest memory (the ground truth, nvn_mtl.m mtl_read_texels); the rest copied back from the image
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = (size_t)(r->yoffset / bh);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    pthread_mutex_lock(&g_bk);
    if (is_packaged(t)) {
        uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; lin && z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(50, "ReadTexels deswizzle failed"); break; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin); pthread_mutex_unlock(&g_bk); return;
    }
    vtex *v = vt_get(t);
    if (!v || v->conv) { pthread_mutex_unlock(&g_bk); for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb); return; }
    stg_t *s = stg_take(wb * hb * nz); VkCommandBuffer cbuf = cb();
    if (s && cbuf) {
        for (int z = 0; z < nz; z++) {
            VkBufferImageCopy rc = {.bufferOffset = (size_t)z * wb * hb, .imageSubresource = {v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : v->aspect, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cbuf, v->img, VK_IMAGE_LAYOUT_GENERAL, s->b, 1, &rc);
        }
        int si = (int)(s - C->stg);
        for (int z = 0; z < nz; z++) { wb_t *w = wb_add(); if (w) *w = (wb_t){si, (size_t)z * wb * hb, wb, hb, wb, row, dst + (size_t)z * img, NULL}; }   // copied out by the completion thread
        submit_wait();
    }
    pthread_mutex_unlock(&g_bk);
}

// ---------------- Clears ----------------
static void clear_color_pass(const p_clear_color *cc); static void clear_ds_pass(const p_clear_ds *cd, VkImageAspectFlags a);   // gfx_vk_draw.inc: partial and masked ones
static int clear_in_pass(const vtex *v, unsigned level, int depth);   // gfx_vk_draw.inc: whether a full clear of v can be made in the bound targets' pass
static struct { p_set_rt rt; int has_rt; p_scissor sc; int has_sc; } R;
static int full_scissor(const vtex *v, unsigned level) {
    if (!R.has_sc) return 1;
    uint32_t w = v->w >> level ? v->w >> level : 1, h = v->h >> level ? v->h >> level : 1;
    return R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h;
}
static VkImageSubresourceRange view_range(const vtex *v, const view_t *vw, int has, VkImageAspectFlags a) {
    VkImageSubresourceRange r = {a, has && vw->set_levels ? vw->base_level : 0, has && vw->set_levels && vw->nlevels ? vw->nlevels : 1,
                                 has && vw->set_layers ? vw->min_layer : 0, has && vw->set_layers && vw->nlayers ? vw->nlayers : v->layers};
    if (r.baseMipLevel >= v->levels) r.baseMipLevel = v->levels - 1;
    if (r.baseMipLevel + r.levelCount > v->levels) r.levelCount = v->levels - r.baseMipLevel;
    if (r.baseArrayLayer >= v->layers) r.baseArrayLayer = 0;
    if (r.baseArrayLayer + r.layerCount > v->layers) r.layerCount = v->layers - r.baseArrayLayer;
    return r;
}
static VkClearColorValue clear_value(VkFormat f, const float rgba[4]) {
    VkClearColorValue c;
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            for (int i = 0; i < 4; i++) c.uint32[i] = (uint32_t)rgba[i]; break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            for (int i = 0; i < 4; i++) c.int32[i] = (int32_t)rgba[i]; break;
        default: memcpy(c.float32, rgba, sizeof c.float32);
    }
    return c;
}
static void clear_color(const p_clear_color *cc) {
    if (!R.has_rt || cc->index < 0 || cc->index >= R.rt.n || !R.rt.color[cc->index]) { RED_ONCE(60, "color clear of target %d, which is not set", cc->index); return; }
    vtex *v = vt_get(R.rt.color[cc->index]); if (!v) return;
    VkImageSubresourceRange rg = view_range(v, &R.rt.cview[cc->index], R.rt.has_cview[cc->index], VK_IMAGE_ASPECT_COLOR_BIT); rg.levelCount = 1;
    if ((cc->mask & 0xf) != 0xf || !full_scissor(v, rg.baseMipLevel)) { if (cc->mask & 0xf) clear_color_pass(cc); return; }
    // a full clear of a target the size of the bound targets' pass: in the pass (vkCmdClearAttachments), on a tiler a clear of the tile memory, instead of writing the
    // whole image to memory outside it, a full barrier, and the pass loading it back. Only one layer of one level is a pass attachment
    if (rg.layerCount == 1 && clear_in_pass(v, rg.baseMipLevel, 0)) { clear_color_pass(cc); return; }
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearColorValue cv = clear_value(v->fmt, cc->rgba);
    vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg); cb_barrier();
}
static void clear_ds(const p_clear_ds *cd) {
    if (!R.has_rt || !R.rt.depth) { RED_ONCE(62, "depth-stencil clear with no depth target set"); return; }
    vtex *v = vt_get(R.rt.depth); if (!v) return;
    VkImageAspectFlags a = (cd->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((cd->stencil_mask & 0xff) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    a &= v->aspect; if (!a) return;
    if (((cd->stencil_mask & 0xff) && (cd->stencil_mask & 0xff) != 0xff) || !full_scissor(v, 0)) { clear_ds_pass(cd, a); return; }
    VkImageSubresourceRange rg = view_range(v, &R.rt.dview, R.rt.has_dview, a); rg.levelCount = 1;
    if (rg.layerCount == 1 && clear_in_pass(v, rg.baseMipLevel, 1)) { clear_ds_pass(cd, a); return; }   // in the pass, as colour clears
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearDepthStencilValue dv = {cd->depth, (uint32_t)cd->stencil};
    vkCmdClearDepthStencilImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rg); cb_barrier();
}
static void clear_tex(const p_clear_tex *ct) {
    vtex *v = vt_get(ct->tex); if (!v) return;
    VkImageSubresourceRange rg = view_range(v, &ct->view, ct->has_view, VK_IMAGE_ASPECT_COLOR_BIT); rg.levelCount = 1;
    uint32_t w = v->w >> rg.baseMipLevel ? v->w >> rg.baseMipLevel : 1, h = v->h >> rg.baseMipLevel ? v->h >> rg.baseMipLevel : 1;
    if ((ct->mask & 0xf) != 0xf || ct->r.xoffset || ct->r.yoffset || (uint32_t)ct->r.width < w || (uint32_t)ct->r.height < h) { RED_ONCE(64, "texture clear of a region or with a channel mask: not done yet"); return; }
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { RED_ONCE(65, "ClearTexture of a depth texture: not done yet"); return; }
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearColorValue cv; if (ct->is_uint) memcpy(cv.uint32, ct->c.u, sizeof cv.uint32); else cv = clear_value(v->fmt, ct->c.f);
    vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg); cb_barrier();
}

static int png_texels(const char *path, VkFormat f, int bpp, const uint8_t *p, uint32_t w, uint32_t x, uint32_t y, uint32_t cw, uint32_t ch, int flip);   // below (the pass dump's PNGs)
#include "gfx_vk_draw.inc"

// ---------------- Submission ----------------
static void post(post_op o) {   // done when the current chunk completes, after its write-backs
    if (!cur()) return;
    if (C->npost == C->cappost) { C->cappost = C->cappost ? 2 * C->cappost : 64; C->post = realloc(C->post, C->cappost * sizeof *C->post); }
    C->post[C->npost++] = o;
}
void mtl_sub_begin(void) { pthread_mutex_lock(&g_bk); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
// Buffer copies and fills: done on the CPU (GPU address = CPU address). A large one goes to the completion thread, in its place in the queue, so the submitting
// thread (the game's) does not stop for it, as it would not on the GPU; a small one now, after the writes before it
enum { COP_MIN = 1 << 20 };
static int cop_add(uint64_t src, uint64_t dst, uint64_t n, uint32_t value, int fill) {
    if (n < COP_MIN || !cur()) return 0;
    g_vs = 4; wb_view();   // the GPU's earlier writes land first; queued copies stay queued (done in order)
    if (!cur()) return 0;
    if (C->ncop == C->capcop) { C->capcop = C->capcop ? 2 * C->capcop : 8; C->cop = realloc(C->cop, C->capcop * sizeof *C->cop); }
    C->cop[C->ncop++] = (cop_t){src, dst, n, value, fill};
    return 1;
}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { if (!n || cop_add(src, dst, n, 0, 0)) return; mem_view(src, n, dst, n); memmove((void *)dst, (const void *)src, n); }
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) { if (cop_add(0, dst, n, value, 1)) return; mem_view(0, 0, dst, n); uint8_t *d = (uint8_t *)dst; for (uint64_t i = 0; i + 4 <= n; i += 4) memcpy(d + i, &value, 4); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); return nvn_timestamp_from_ns(g ? g : clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
enum { EARLY_DRAWS = 48, MID_DRAWS = 64 };   // MID_DRAWS: a chunk that long goes to the GPU before the next draw, in the middle of a pass (the pass is begun again with its targets
// loaded): Super Mario Bros. Wonder draws most of a frame in two passes of about 140 draws, which reached the GPU only when each ended, and the game then waited for the GPU
// (SyncWait 12.5 ms a wait) after recording 23 ms of draws
// g_mid_draws: MID_DRAWS, or a file named vk_mid beside $DM_DUMP_DIR holding a number, read every 30 presents (a test of the split's cost: each piece loads and stores the pass's targets)
static int g_mid_draws = MID_DRAWS;
static void mid_poll(uint64_t presents) {
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../vk_mid", dir); FILE *f = fopen(p, "r"); if (!f) return;
    int v = 0; if (fscanf(f, "%d", &v) == 1 && v > 0 && v != g_mid_draws) { LOG("[vk] chunks sent in the middle of a pass after %d draws (was %d)", v, g_mid_draws); g_mid_draws = v; }
    fclose(f);
}
void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        // a render pass stays open over what is done on the CPU (uniform buffer updates, buffer copies and fills, events, counters, fences: the draws took their copies of guest
        // memory when they were recorded): Super Mario Bros. Wonder updates a uniform buffer before nearly every draw, and ending the pass there made 400 passes a frame, each
        // loading and storing its targets (on a tiler GPU the whole frame's bandwidth again and again)
        int cpu_op = h.op == OP_UPDATE_UB || h.op == OP_SIGNAL_EVENT || h.op == OP_REPORT_COUNTER || h.op == OP_FENCE_SYNC || h.op == OP_COPY_STRIDE || h.op == OP_CALL
                     || h.op == OP_CLEAR_BUF || h.op == OP_COPY_B2B
                     || h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS || h.op == OP_RESET_COUNTER;   // clears of the bound targets are made in their pass when they can be (clear_color); the others end it themselves (cb())
        if (P.active && h.op != OP_DRAW && h.op != OP_DRAW_IDX && h.op != OP_SCISSOR && h.op != OP_JUMP && h.op != OP_END && !cpu_op && !draw_state_is(h.op)) rp_end();
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { occl_end(); g_q_reports++; post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, g_q_on ? g_q_win : g_qslot, g_qslot}); }
                else if (C && C->tq && C->ntq < 256 && (t == 0 || t == 0x10)) {   // a timestamp: written by the GPU at this point of the commands, so two of them time the GPU work between
                    // (Super Mario Bros. Wonder times its frame's GPU work for its dynamic resolution; the host used to write every report of a chunk with one time, its completion)
                    VkCommandBuffer cbuf = C->open ? C->cb : cb(); if (!cbuf) break;
                    g_ts_reports++; int q = C->ntq++; vkCmdWriteTimestamp(cbuf, t == 0x10 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->tq, (uint32_t)q);
                    post((post_op){4, NULL, 0, 0, ad, (uint32_t)t, (uint64_t)q, 0}); }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { occl_end(); g_q_on = 1; g_q_win = g_qslot; } break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *c = (const p_clear_buf *)a; mtl_sub_fill(c->addr, c->size, c->value); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); mem_view(0, 0, d, n); if (n) memcpy((void *)d, a + 16, n); break; }
            case OP_COPY_STRIDE: { const p_copy_stride *c = (const p_copy_stride *)a; if (c->image) S.img = c->stride; else S.row = c->stride; break; }
            case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
            case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a); break;
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_SET_RT:   // new render targets: a chunk with enough work goes to the GPU now, so that it draws while the rest of the submission is recorded
                // (one command buffer per frame kept the GPU idle until the CPU had recorded everything: Super Mario Bros. Wonder's level took CPU 29 ms + GPU 41 ms
                // a frame one after the other). A dispatch's storage buffer read by later draws on the GPU is found in the chunks in flight too (fwd_find)
                if (C && C->ndraw >= EARLY_DRAWS && !vk_safe()) flush();
                R.rt = *(const p_set_rt *)a; R.has_rt = 1; break;
            case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
            case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_DRAW: if (C && C->ndraw >= g_mid_draws && !vk_safe()) flush();
                { p_draw t; if (!vk_draw(draw_rec(a, h.size, &t), NULL)) g_draws_dropped++; break; }   // short records: draw_rec (nvn_obj.h)
            case OP_DRAW_IDX: if (C && C->ndraw >= g_mid_draws && !vk_safe()) flush();
                { p_draw_idx t; if (!vk_draw(NULL, draw_idx_rec(a, h.size, &t))) g_draws_dropped++; break; }
            case OP_DISPATCH: if (!vk_dispatch((const p_dispatch *)a)) g_draws_dropped++; break;
            default: draw_state_op(h.op, a); break;   // state for draws (programs, bindings, render state)
        }
        if (!h.size) { LOG("[RED] [vk] command record of size 0 (op %u) at %p: the rest of the list is skipped", h.op, (const void *)p); return; }
        p += h.size;
    }
}
static void post_apply(const post_op *o) {   // the completion thread, when the GPU has done the commands before it
    if (o->kind == 1) { if (o->mode == 1) __atomic_fetch_add(o->val, o->v, __ATOMIC_SEQ_CST); else __atomic_store_n(o->val, o->v, __ATOMIC_SEQ_CST); return; }
    uint64_t now = gpu_ticks(), rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 0 : (o->type == 0 || o->type == 0x10) ? now : 0, now};
    if (o->kind == 4) { rec[0] = o->type == 0 || o->type == 0x10 ? o->q1 : 0; rec[1] = o->q1; }   // a timestamp the GPU wrote where the report was (gfx_vk.c OP_REPORT_COUNTER)
    if (o->kind == 3) { uint64_t sum = 0, a = o->q1 - o->q0 > QRES ? o->q1 - QRES : o->q0; for (uint64_t q = a; q < o->q1; q++) sum += g_qres[q % QRES]; rec[0] = sum; }
    dm_guest_write_prepare((void *)(uintptr_t)o->addr, sizeof rec); memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
}
// A submission's end: its last chunk completes NVN fence f (the completion thread sets g_done); the GPU may still be at it on return
uint64_t mtl_sub_commit(void) {
    uint64_t f = atomic_fetch_add(&g_fence, 1) + 1;
    if (!cur()) { pthread_mutex_unlock(&g_bk); return f; }
    chunk_submit(f);
    pthread_mutex_unlock(&g_bk);
    return f;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    ts.tv_sec += (time_t)(t / 1000000000ull); ts.tv_nsec = (long)(t % 1000000000ull);
    pthread_mutex_lock(&g_fmu);
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_fcv, &g_fmu, &ts)) break;
    pthread_mutex_unlock(&g_fmu);
    return atomic_load(&g_done) >= fence;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) { uint64_t f = atomic_load(&g_fence); while (!mtl_wait(f, 1000000000ull)) {} }   // everything submitted so far is done
void mtl_drain(void) { mtl_finish(); pthread_mutex_lock(&g_cmu); while (g_fifo_n) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu); }

// ---------------- Screenshots and PNG ----------------
// A minimal PNG writer (8-bit RGB, deflate's stored blocks: no compression library in the app) for screenshots and the pass dump's targets
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1; t[i] = x; }
    c = ~c; while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t h[8]; be32(h, (uint32_t)n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint8_t c[4]; be32(c, png_crc(png_crc(0, (const uint8_t *)type, 4), d, n)); fwrite(c, 1, 4, f);
}
static int png_write(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {   // rgb: w * h * 3 bytes, top row first
    size_t row = (size_t)w * 3 + 1, raw = row * h, nblk = (raw + 65534) / 65535, zn = 2 + raw + 5 * nblk + 4;
    uint8_t *z = malloc(zn); FILE *f = z ? fopen(path, "wb") : NULL;
    if (!f) { free(z); return 0; }
    uint8_t *o = z; *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0; size_t left = raw, pos = 0;   // the stored blocks cut the filtered rows (filter byte 0 + pixels) at 65535 bytes; adler32 over the same bytes
    while (left) {
        uint16_t n = (uint16_t)(left < 65535 ? left : 65535); left -= n;
        *o++ = left ? 0 : 1; *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8); *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        for (uint16_t i = 0; i < n; i++, pos++) { size_t y = pos / row, x = pos % row; uint8_t v = x ? rgb[y * (row - 1) + x - 1] : 0; *o++ = v; a = (a + v) % 65521; b = (b + a) % 65521; }
    }
    be32(o, b << 16 | a); o += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0; png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", z, (size_t)(o - z)); png_chunk(f, "IEND", NULL, 0);
    int ok = !ferror(f); fclose(f); free(z); return ok;
}
static float uf_f(uint32_t bits, int mb) { uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1); return e == 31 ? 1.0f : e ? ldexpf((float)(m | 1u << mb), (int)e - 15 - mb) : ldexpf((float)m, -14 - mb); }   // B10G11R11's unsigned floats (5-bit exponent)
static uint8_t f_u8(float v) { return v <= 0 || v != v ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }
static int px_rgb(VkFormat f, const uint8_t *s, uint8_t *o) {   // one texel → 8-bit RGB (float formats clamped to [0, 1], no tone mapping); 0: format not converted
    uint32_t u; memcpy(&u, s, 4);
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; return 1;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: o[0] = (uint8_t)((u & 1023) >> 2); o[1] = (uint8_t)((u >> 10 & 1023) >> 2); o[2] = (uint8_t)((u >> 20 & 1023) >> 2); return 1;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: o[0] = f_u8(uf_f(u & 2047, 6)); o[1] = f_u8(uf_f(u >> 11 & 2047, 6)); o[2] = f_u8(uf_f(u >> 22, 5)); return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t c[3]; memcpy(c, s, 6); for (int i = 0; i < 3; i++) o[i] = f_u8(half_f(c[i])); return 1; }
    case VK_FORMAT_R32G32B32A32_SFLOAT: { float c[3]; memcpy(c, s, 12); for (int i = 0; i < 3; i++) o[i] = f_u8(c[i]); return 1; }
    case VK_FORMAT_R8_UNORM: o[0] = o[1] = o[2] = s[0]; return 1;
    case VK_FORMAT_R16_SFLOAT: o[0] = o[1] = o[2] = f_u8(half_f((uint16_t)u)); return 1;
    case VK_FORMAT_R32_SFLOAT: { float c; memcpy(&c, s, 4); o[0] = o[1] = o[2] = f_u8(c); return 1; }
    default: return 0;
    }
}
// texels (w * h, tightly packed, bpp bytes each) → PNG, the rectangle x, y, cw, ch of them; flip: bottom row first (a lower-left origin)
static int png_texels(const char *path, VkFormat f, int bpp, const uint8_t *p, uint32_t w, uint32_t x, uint32_t y, uint32_t cw, uint32_t ch, int flip) {
    uint8_t t[3], z[16] = {0}; if (!px_rgb(f, z, t)) return 0;
    uint8_t *rgb = malloc((size_t)cw * ch * 3); if (!rgb) return 0;
    for (uint32_t r = 0; r < ch; r++) { const uint8_t *s = p + ((size_t)(y + (flip ? ch - 1 - r : r)) * w + x) * bpp; for (uint32_t c = 0; c < cw; c++) px_rgb(f, s + (size_t)c * bpp, rgb + ((size_t)r * cw + c) * 3); }
    int ok = png_write(path, rgb, cw, ch); free(rgb); return ok;
}
// The presented image (its crop rectangle) → PNG, with the replay's lock held: copied to a staging buffer and waited for
static void shot_write(vtex *v, const int crop[4], const char *path) {
    int bpp = fmt_bytes(v->fmt); uint8_t t[3], z[16] = {0};
    if (!bpp || !px_rgb(v->fmt, z, t)) { LOG("[RED] screenshot %s: format %d not converted", path, v->fmt); return; }
    stg_t s; if (!stg_new(&s, (VkDeviceSize)v->w * v->h * bpp)) { LOG("[RED] screenshot %s: no staging buffer", path); return; }
    VkCommandBuffer c = cb(); if (!c) { stg_free(&s); return; }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {v->w, v->h, 1}};
    vkCmdCopyImageToBuffer(c, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &r);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    all_wait();
    uint32_t x = 0, y = 0, w = v->w, h = v->h;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= v->w && (uint32_t)(crop[1] + crop[3]) <= v->h) { x = (uint32_t)crop[0]; y = (uint32_t)crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3]; }
    uint64_t sum = 0; for (VkDeviceSize i = 0; i < s.n; i += 4 * 97) sum += s.p[i] + s.p[i + 1 < s.n ? i + 1 : i];   // a coarse "is anything there" figure for the log
    if (png_texels(path, v->fmt, bpp, s.p, v->w, x, y, w, h, g_origin_ll)) LOG("[shot] frame %llu → %s (%ux%u of %ux%u, vk%d, sampled byte sum %llu)", (unsigned long long)atomic_load(&g_presents) + 1, path, w, h, v->w, v->h, v->fmt, (unsigned long long)sum);
    else LOG("[RED] screenshot %s: cannot write", path);
    stg_free(&s);
}
// Requests: dm_gfx_shot_at (k presents from now), DM_NVN_SHOT=<frame>[,<frame>...]:<path> (%d = the frame), and a file named shot_now beside $DM_DUMP_DIR
// (checked every 30 presents; removed): the next present to <DM_DUMP_DIR>/shot_<frame>.png
#define SHOTQ_MAX 16
static struct { uint64_t frame; char path[700]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static void shot_queue(uint64_t frame, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = frame; snprintf(g_shotq[g_nshotq].path, sizeof g_shotq[0].path, "%s", path); g_nshotq++; }
    else LOG("[RED] more than %d screenshot requests: %s dropped", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
static void shot_poll(uint64_t presents, vtex *v, const int crop[4]) {   // presents: before this one; with the replay's lock held
    static long frames[256]; static int nf = -1; static const char *fpath;
    if (nf < 0) { nf = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { fpath = c + 1; for (const char *q = e; q < c && nf < 256; q = strchr(q, ',') && strchr(q, ',') < c ? strchr(q, ',') + 1 : c) frames[nf++] = atol(q); } }
    const char *dir = getenv("DM_DUMP_DIR"); char p[700];
    if (dir && !(presents % 30)) { snprintf(p, sizeof p, "%s/../shot_now", dir);
        if (!access(p, F_OK)) { unlink(p); mkdir(dir, 0777); snprintf(p, sizeof p, "%s/shot_%llu.png", dir, (unsigned long long)presents + 1); shot_queue(presents + 1, p); }
        snprintf(p, sizeof p, "%s/../threads_now", dir);   // a file named threads_now: every guest thread's state, what it waits on and its backtrace (a game that stopped moving on)
        if (!access(p, F_OK)) { unlink(p); void svc_thread_dump(void); LOG("[threads] threads_now seen at present %llu", (unsigned long long)presents); svc_thread_dump(); } }
    if (!v) return;
    for (int i = 0; i < nf; i++) if ((long)presents + 1 == frames[i]) { char path[700]; snprintf(path, sizeof path, fpath, (int)presents + 1); shot_write(v, crop, path); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (presents + 1 >= g_shotq[i].frame) { char path[700]; memcpy(path, g_shotq[i].path, sizeof path); g_shotq[i] = g_shotq[--g_nshotq];
        pthread_mutex_unlock(&g_shotq_mu); shot_write(v, crop, path); pthread_mutex_lock(&g_shotq_mu); } else i++;
    pthread_mutex_unlock(&g_shotq_mu);
}
void dm_gfx_shot_at(long k, const char *path) { shot_queue(atomic_load(&g_presents) + 1 + (uint64_t)(k > 0 ? k : 0), path); }

// ---------------- Presents ----------------
static pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER; static vkb_frame g_frame; static uint64_t g_frame_serial;
int vkb_frame_lock(vkb_frame *f) { pthread_mutex_lock(&g_frame_mu); if (!g_frame.img) { pthread_mutex_unlock(&g_frame_mu); return 0; } *f = g_frame; return 1; }
void vkb_frame_unlock(void) { pthread_mutex_unlock(&g_frame_mu); }
void vkb_frame_forget(uint64_t serial) { pthread_mutex_lock(&g_frame_mu); if (g_frame_serial == serial) { memset(&g_frame, 0, sizeof g_frame); g_frame_serial = 0; } pthread_mutex_unlock(&g_frame_mu); }
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    static uint64_t next;
    pthread_mutex_lock(&g_bk);
    vtex *v = vt_get(t); flush();
    dump_poll(atomic_load(&g_presents)); prof_poll(atomic_load(&g_presents)); mid_poll(atomic_load(&g_presents));
    if (prof_on()) { all_wait(); prof_write(); }
    if (atomic_load(&g_presents) == 30) prewarm_start();   // the pipelines of earlier sessions (gfx_vk_draw.inc), once the game draws
    if (dump_on()) { if (v) LOG("[dump] frame %ld presents ser=%llu %ux%u vk%d", dump_frame(), (unsigned long long)v->serial, v->w, v->h, v->fmt); all_wait(); dump_write();
        if (v) { char p[700]; snprintf(p, sizeof p, "%s/f%ld_present.png", getenv("DM_DUMP_DIR") ? getenv("DM_DUMP_DIR") : ".", dump_frame()); shot_write(v, crop, p); } }
    shot_poll(atomic_load(&g_presents), v, crop);
    if (v) {
        pthread_mutex_lock(&g_frame_mu);
        g_frame = (vkb_frame){v->img, v->fmt, v->w, v->h, {0, 0, (int)v->w, (int)v->h}, atomic_load(&g_presents) + 1, g_origin_ll}; g_frame_serial = v->serial;
        if (crop && crop[2] > 0 && crop[3] > 0) memcpy(g_frame.crop, crop, sizeof g_frame.crop);
        pthread_mutex_unlock(&g_frame_mu);
    }
    pthread_mutex_unlock(&g_bk);
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), per = period_s > 0 ? (uint64_t)(period_s * 1e9) : 16666667ull;
    if (next > now && next - now < 1000000000ull) mach_wait_until(next); else next = now;
    next += per;
    void front_android_present(const tex_t *t, const int crop[4]); front_android_present(t, crop);
    return atomic_fetch_add(&g_presents, 1) + 1;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
int mtl_display_hz(void) { return 60; }
int mtl_has_window(void) { int front_has_window(void); return front_has_window(); }
int mtl_fps_target(void) { return 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
int mtl_link_rate(void) { return 60; }
int mtl_link_stalled(void) { return 0; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_set_depth_zero(int on) { g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { if (g_origin_ll != on) LOG("[vk] window origin → %s (presents so far %llu)", on ? "lower left" : "upper left", (unsigned long long)atomic_load(&g_presents)); g_origin_ll = on; }   // as nvn_mtl.m logs it
void mtl_set_queue_origin_lower_left(int on) { g_queue_ll = on != 0; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) {   // RegisterImage declares image use even when the builder left IMAGE out (as nvn_mtl.m)
    if (!t) { mtl_reg_tex(id, NULL, v); return; }
    tex_t image = *t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; mtl_reg_tex(id, &image, v);
}
unsigned mtl_cpu_tex_count(void) { return 0; }   // Metal's CPU-written texture set: the replay has none
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }   // no display link
void mtl_set_frame(uint64_t f) { (void)f; }
int mtl_pso_prebuild_tool(void) { return 0; }

// ---------------- Statistics the interface layer reads ----------------
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_edge_skips(void) { return g_pkg_edge_skips; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return g_up_bytes; }
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e, uint64_t *f) { *a = *b = *c = *d = *e = *f = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_passes; *draws = g_draws; }
void mtl_args_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_use_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return -1; }
void mtl_ab_gpu(uint64_t f, double g, double fr) { (void)f; (void)g; (void)fr; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_ub_present(double iv) { (void)iv; }
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_up_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
// the long-frame line's "new pipelines + shader libraries": graphics and compute pipelines, and shader modules (Vulkan's counterpart of Metal's libraries)
void mtl_create_stats(uint64_t *a, uint64_t *b) { *a = atomic_load(&g_mod_made); *b = atomic_load(&g_pso_made) + atomic_load(&g_cpso_made); }
void mtl_create_ns(uint64_t *a, uint64_t *b) { *a = atomic_load(&g_mod_ns); *b = atomic_load(&g_pso_ns) + atomic_load(&g_cpso_ns); }
void mtl_hybrid_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_barrier_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_blit_splits(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
uint64_t mtl_cut_count(void) { return g_flushes; }
void mtl_cshoist_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_sm_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { LOG("[vk] frame %llu: resident %llu MB, textures %u, uploads %llu (%llu again), %llu MB uploaded", frame, mb, g_tex.n,
    (unsigned long long)g_pkg_uploads, (unsigned long long)g_pkg_reuploads, (unsigned long long)(g_up_bytes >> 20));
    LOG("[vk] guest reads of draws: waited for the current submission %llu, for submissions in flight %llu, no wait %llu; uniform updates and small copies: %llu / %llu / %llu; submissions %llu (cumulative)",
        (unsigned long long)g_view_cur, (unsigned long long)g_view_fly, (unsigned long long)g_view_free, (unsigned long long)g_mem_cur, (unsigned long long)g_mem_fly, (unsigned long long)g_mem_free,
        (unsigned long long)g_flushes);
    LOG("[vk] constant and storage buffer copies: %llu reused, %llu made (cumulative; gfx_vk_draw.inc ucopy)", (unsigned long long)g_ucopy_hit, (unsigned long long)g_ucopy_miss);
    LOG("[vk] geometry kept on the GPU: %llu index / vertex bindings from kept copies, %llu copies kept (%llu MB now), %llu copied per draw (cumulative; gfx_vk_draw.inc gc_get)",
        (unsigned long long)g_gc_hit, (unsigned long long)g_gc_made, (unsigned long long)(g_gc_bytes >> 20), (unsigned long long)g_gc_copy);
    { static unsigned long long f0; static uint64_t g0; uint64_t g = atomic_load(&g_gpu_ns);
      if (frame > f0) LOG("[vk] GPU busy %.2f ms per frame over the last %llu frames (%llu command buffers in all)", (double)(g - g0) / 1e6 / (double)(frame - f0), frame - f0, (unsigned long long)atomic_load(&g_gpu_chunks));
      f0 = frame; g0 = g; }
    LOG("[vk] occlusion queries %llu, samples-passed reports %llu, timestamp reports %llu, graphics pipelines made %llu (%.0f ms), compute pipelines %llu (%.0f ms), shader modules %llu (%.0f ms) (cumulative)", (unsigned long long)g_q_queries, (unsigned long long)g_q_reports, (unsigned long long)g_ts_reports,
        (unsigned long long)atomic_load(&g_pso_made), atomic_load(&g_pso_ns) / 1e6, (unsigned long long)atomic_load(&g_cpso_made), atomic_load(&g_cpso_ns) / 1e6, (unsigned long long)atomic_load(&g_mod_made), atomic_load(&g_mod_ns) / 1e6);
    LOG("[vk] draws not made: shader missing %llu, targets %llu, pipeline %llu, descriptors %llu, invalid binding %llu, pass %llu, vertex buffers %llu; dropped in all %llu (cumulative)", (unsigned long long)g_drop[0], (unsigned long long)g_drop[1], (unsigned long long)g_drop[2], (unsigned long long)g_drop[3], (unsigned long long)g_drop[4], (unsigned long long)g_drop[5], (unsigned long long)g_drop[6], (unsigned long long)g_draws_dropped);
    LOG("[vk] vertex streams read on the GPU from a dispatch's storage buffer: %llu (cumulative)", (unsigned long long)g_fwd);
    LOG("[vk] syncs by site (submitted / in flight): other %llu/%llu, draw %llu/%llu, upload %llu/%llu, t2b %llu/%llu, large copy %llu/%llu, buffer texture %llu/%llu, buffer image %llu/%llu, dispatch %llu/%llu",
        (unsigned long long)g_vsite[0][0], (unsigned long long)g_vsite[0][1], (unsigned long long)g_vsite[1][0], (unsigned long long)g_vsite[1][1], (unsigned long long)g_vsite[2][0], (unsigned long long)g_vsite[2][1],
        (unsigned long long)g_vsite[3][0], (unsigned long long)g_vsite[3][1], (unsigned long long)g_vsite[4][0], (unsigned long long)g_vsite[4][1], (unsigned long long)g_vsite[5][0], (unsigned long long)g_vsite[5][1],
        (unsigned long long)g_vsite[6][0], (unsigned long long)g_vsite[6][1], (unsigned long long)g_vsite[7][0], (unsigned long long)g_vsite[7][1]); }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "textures %u", g_tex.n); }
uint32_t mtl_present_qos(void) { return 0; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
