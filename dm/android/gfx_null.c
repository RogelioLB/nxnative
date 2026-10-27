// NVN replay without a GPU (the Android build's graphics backend until a Vulkan replay replaces it): the interface of nvn_mtl.m (nvn_mtl.h,
// nvn_obj.h) with every submission completed on the CPU at commit. Rendering commands are skipped; the effects a game can observe are made in
// command order, as the Metal replay's completion handlers make them (nvn_mtl.m post_run):
//   events (OP_SIGNAL_EVENT: write or add), counter reports (16 bytes {value, timestamp}; timestamps from the guest clock at 614.4 MHz,
//   samples-passed reports 0), buffer copies, fills and inline uniform updates (the GPU address of pool memory is its CPU address),
//   fences (OP_FENCE_SYNC and QueueFenceSync take the submission's fence, complete at its commit).
// Presents are counted and paced by the window's present period; front_android.c shows nothing of the game's picture yet.
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_timestamp.h"
#include <stdatomic.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_fence, g_done, g_presents;
static int g_depth_zero, g_origin_ll;

// ---- Memory pools: GPU address = CPU address, so only the address range is kept, for mtl_addr ----
typedef struct { const uint8_t *lo, *hi; } pool_rng;
static pool_rng *g_pools; static int g_npool, g_cappool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
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
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// ---- Submission ----
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;   // kind 1 event, 2 counter
static post_op *g_post; static int g_npost, g_postcap;
static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? 2 * g_postcap : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_sub_begin(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { if (n) memmove((void *)dst, (const void *)src, n); }
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) { uint8_t *d = (uint8_t *)dst; for (uint64_t i = 0; i + 4 <= n; i += 4) memcpy(d + i, &value, 4); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); return nvn_timestamp_from_ns(g ? g : clock_gettime_nsec_np(CLOCK_MONOTONIC)); }

void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (n) memcpy((void *)d, a + 16, n); break; }
            default: break;   // rendering, compute and texture commands: no GPU
        }
        if (!h.size) { LOG("[RED] gfx_null: command record of size 0 (op %u) at %p: the rest of the list is skipped", h.op, (const void *)p); return; }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    uint64_t now = gpu_ticks();
    for (int i = 0; i < g_npost; i++) {
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else { uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 0 : (o->type == 0 || o->type == 0x10) ? now : 0, now}; memcpy((void *)o->addr, rec, sizeof rec); }
    }
    g_npost = 0;
    pthread_mutex_lock(&g_mu);
    uint64_t f = atomic_fetch_add(&g_fence, 1) + 1; atomic_store(&g_done, f); pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return f;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)timeout_ns; return atomic_load(&g_done) >= fence; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) {}
void mtl_drain(void) {}

// ---- Presents: paced to the window's period, so the game's frame loop runs at its own rate ----
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)crop;
    static uint64_t next;
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

// ---- Settings, textures, programs ----
void mtl_set_depth_zero(int on) { g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { g_origin_ll = on; }
void mtl_set_queue_origin_lower_left(int on) { (void)on; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { mtl_reg_tex(id, t, v); }
unsigned mtl_cpu_tex_count(void) { return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
// A CPU read of a texture: with no GPU content, the bytes read are zero
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0;
    size_t depth = r->depth > 0 ? (size_t)r->depth : 1, h = r->height > 0 ? (size_t)r->height : 1;
    for (size_t z = 0; z < depth; z++) for (size_t y = 0; y < h; y++) memset(dst + z * img + y * row, 0, row);
}
int mtl_pso_prebuild_tool(void) { return 0; }

// ---- Statistics the interface layer reads for its log lines: nothing measured ----
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e, uint64_t *f) { *a = *b = *c = *d = *e = *f = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
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
void mtl_pkg_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
void mtl_create_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_create_ns(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_hybrid_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_barrier_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_blit_splits(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
void mtl_cshoist_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_sm_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { LOG("[nvn] frame %llu: resident %llu MB", frame, mb); }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) out[0] = 0; }
uint32_t mtl_present_qos(void) { return 0; }
// Graphics diagnostics (screenshots, texture and pass dumps): none without a GPU replay
void dm_gfx_shot_at(long k, const char *path) { (void)k; LOG("[RED] screenshot %s: no picture on Android yet", path); }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
