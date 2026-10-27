// Android front end (NativeActivity, libdmhost.so): the app's window, input, lifecycle and log, around the host's main() (main.c).
//   Game files: <external files>/game/ — code/ (the modules rewritten with DM_TARGET=android: <name>.patched.bin, .fixups.tsv, .gates.tsv ...),
//   RomFS/, optionally aoc/; switches in <external files>/env.txt (KEY=VALUE per line, as the iPhone's Documents/env.txt). Save data stays in the
//   app's internal storage. Without a game the app runs the built-in self test (assets/selftest: dm/android/tests/selftest_nso.py, rewritten).
//   Log: the host's stderr goes to logcat (tag nxnative) and to <external files>/log.txt.
//   Window: Vulkan, on the NVN replay's device (gfx_vk.c). The window texture the game presented last is drawn scaled to fit (vkb_frame_lock);
//   before the first one, and without a game, a status screen. The device's Vulkan capabilities are written to the log at start.
//   Input: touches are the touchscreen (hle_touch.c); a physical controller is player 1's (buttons by position, Nintendo's layout).
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "nvn_obj.h"
#include <android/bitmap.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "gfx_vk.h"
#include "game_profile.h"   // DM_GAME_PAD_LAYOUT
#include "hle_haptics.h"

int main(int argc, char **argv);   // main.c
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void dm_gfx_suspend(int on);

enum { ST_START, ST_SELFTEST, ST_PASS, ST_FAIL, ST_GAME, ST_ENDED, ST_CRASHED, ST_DEMO };
static int g_demo_frames;   // frames the NVN picture check presented (nvn_demo.c)
static _Atomic int g_state = ST_START;
static _Atomic uint64_t g_frames;
static char g_ext[512], g_int[512], g_last[160];
static pthread_mutex_t g_txt_mu = PTHREAD_MUTEX_INITIALIZER;
static struct android_app *g_app;

// ---------- Log: stderr → logcat and log.txt ----------
static void *log_reader(void *p) {
    int fd = (int)(intptr_t)p; char path[600]; snprintf(path, sizeof path, "%s/log.txt", g_ext);
    FILE *f = fopen(path, "w"); char buf[4096]; size_t n = 0;
    for (;;) {
        ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
        if (k <= 0) break;
        n += (size_t)k; buf[n] = 0;
        char *s = buf, *e;
        while ((e = memchr(s, '\n', buf + n - s))) {
            *e = 0;
            __android_log_write(strstr(s, "[RED]") || strstr(s, "FATAL") || strstr(s, "CRASH") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "nxnative", s);
            if (f) { fputs(s, f); fputc('\n', f); fflush(f); }
            if (strstr(s, "selftest: PASS")) atomic_store(&g_state, ST_PASS);
            else if (strstr(s, "selftest: FAIL")) atomic_store(&g_state, ST_FAIL);
            if (strstr(s, "CRASH") || strstr(s, "FATAL")) atomic_store(&g_state, ST_CRASHED);
            pthread_mutex_lock(&g_txt_mu); snprintf(g_last, sizeof g_last, "%s", s); pthread_mutex_unlock(&g_txt_mu);
            s = e + 1;
        }
        n -= (size_t)(s - buf); memmove(buf, s, n);
        if (n == sizeof buf - 1) n = 0;   // a line longer than the buffer: dropped
    }
    if (f) fclose(f);
    return NULL;
}
static void log_start(void) {
    int pfd[2]; if (pipe(pfd)) return;
    dup2(pfd[1], 2); close(pfd[1]); setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th; pthread_create(&th, NULL, log_reader, (void *)(intptr_t)pfd[0]); pthread_detach(th);
}

// ---------- Files ----------
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void mkdirs(const char *p) { char b[600]; snprintf(b, sizeof b, "%s", p); for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0755); *s = '/'; } mkdir(b, 0755); }
// <external files>/env.txt: KEY=VALUE per line; # comments. Then launcher.txt, the settings screen's choices (java Launcher.java), without overriding env.txt
static void env_file(const char *name, int over) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", g_ext, name);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0; if (!over && getenv(line)) continue;
        setenv(line, eq + 1, 1); LOG("%s: %s=%s", name, line, eq + 1);
    }
    fclose(f);
}
static int copy_assets(const char *dir, const char *to) {   // assets/<dir>/* → <to>/
    AAssetDir *d = AAssetManager_openDir(g_app->activity->assetManager, dir); if (!d) return 0;
    mkdirs(to); const char *name; int n = 0;
    while ((name = AAssetDir_getNextFileName(d))) {
        char a[300], o[700]; snprintf(a, sizeof a, "%s/%s", dir, name); snprintf(o, sizeof o, "%s/%s", to, name);
        AAsset *as = AAssetManager_open(g_app->activity->assetManager, a, AASSET_MODE_STREAMING); if (!as) continue;
        FILE *f = fopen(o, "wb"); char buf[65536]; int k;
        while (f && (k = AAsset_read(as, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)k, f);
        if (f) fclose(f);
        AAsset_close(as); n++;
    }
    AAssetDir_close(d); return n;
}

// ---------- Sandbox checks (after the self test, with the host's handlers installed): what an app process may refuse that adb's shell allowed ----------
static volatile uint8_t *g_ck_page; static volatile int g_ck_hits, g_ck_write;
static int ck_fault(void *addr, int is_write, void *uc) {   // crash.c fault hook: a write to the read-only test page is let through
    (void)uc;
    if (!g_ck_page || (uint8_t *)addr < g_ck_page || (uint8_t *)addr >= g_ck_page + 4096) return 0;
    g_ck_hits++; g_ck_write = is_write; mprotect((void *)g_ck_page, 4096, PROT_READ | PROT_WRITE); return 1;
}
static int ck_file(const char *p, const char *want) { char b[64] = ""; FILE *f = fopen(p, "r"); if (!f) return 0; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return !strcmp(b, want); }
static int ck_write(const char *p, const char *s) { FILE *f = fopen(p, "w"); if (!f) return 0; fputs(s, f); return !fclose(f); }
static void sandbox_checks(void) {
    char why[256] = ""; int ok = 1; LOG("sandbox checks: start");
#define CK(c, msg) do { if (!(c)) { ok = 0; snprintf(why + strlen(why), sizeof why - strlen(why), " %s (errno %d);", msg, errno); } } while (0)
    // 1. a write fault on a read-only page reaches the host's handler (the dirty-page tracker of the NVN layer works this way), past ART's signal chain
    g_ck_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CK(g_ck_page != MAP_FAILED, "mmap");
    if (g_ck_page != MAP_FAILED) {
        dm_fault_hook_register(ck_fault);
        mprotect((void *)g_ck_page, 4096, PROT_READ);
        g_ck_page[100] = 42;
        CK(g_ck_hits == 1 && g_ck_write && g_ck_page[100] == 42, "write fault hook");
    }
    LOG("sandbox checks: write fault hook done (%d)", ok);
    // 2. the save transaction of hle_fs.c in the app's internal storage: a tree copy with its extended attributes, an atomic swap of two directories
    char a[600], b[600], f[700];
    snprintf(a, sizeof a, "%s/sbx_a", g_int); snprintf(b, sizeof b, "%s/sbx_b", g_int);
    snprintf(f, sizeof f, "rm -rf '%s' '%s'", a, b); system(f);
    CK(!mkdir(a, 0755), "mkdir");
    snprintf(f, sizeof f, "%s/f.txt", a); CK(ck_write(f, "one"), "write");
    CK(!setxattr(f, "org.d2rmobile.guest-name", "Name", 4, 0, XATTR_NOFOLLOW), "setxattr");
    CK(!clonefile(a, b, 0), "tree copy");
    snprintf(f, sizeof f, "%s/f.txt", b); char v[16] = ""; ssize_t k = getxattr(f, "org.d2rmobile.guest-name", v, sizeof v - 1, 0, XATTR_NOFOLLOW);
    CK(ck_file(f, "one") && k == 4 && !memcmp(v, "Name", 4), "copy content and xattr");
    CK(ck_write(f, "two"), "write copy");
    CK(!renamex_np(b, a, RENAME_SWAP), "renameat2 RENAME_EXCHANGE");
    snprintf(f, sizeof f, "%s/f.txt", a); CK(ck_file(f, "two"), "swapped content");
    snprintf(f, sizeof f, "rm -rf '%s' '%s'", a, b); system(f);
    LOG("sandbox checks: save transaction done (%d)", ok);
    // 3. a read of unmapped memory fails instead of faulting (crash reports, probes)
    uint64_t w; vm_size_t got = 0; CK(vm_read_overwrite(mach_task_self(), 8, 8, (vm_address_t)&w, &got) != KERN_SUCCESS, "unmapped read");
#undef CK
    if (ok) LOG("sandbox checks: PASS (write fault hook, save tree copy + xattr + atomic swap, unmapped read)");
    else { LOG("[RED] sandbox checks: FAIL:%s", why); atomic_store(&g_state, ST_FAIL); }
}

// ---------- The guest ----------
static int g_argc; static char *g_argv[8];
static void *guest_thread(void *p) {
    (void)p;
    int rc = main(g_argc, g_argv);
    LOG("host main returned %d", rc);
    // the self test's verdict is read off the log by log_reader, on its own thread: wait for it to have seen the line (up to 2 s)
    for (int i = 0; i < 200 && atomic_load(&g_state) == ST_SELFTEST; i++) usleep(10000);
    if (atomic_load(&g_state) == ST_PASS) sandbox_checks();
    if (atomic_load(&g_state) == ST_PASS) {   // the picture path: NVN calls → the Vulkan replay → this window (about 10 s)
        int nvn_demo_run(int frames);
        char d[600]; snprintf(d, sizeof d, "%s/demo", g_int);   // the synthetic shaders and their translation (tests/synth_shaders.py, tools/spv_android.py)
        if (copy_assets("demo", d)) setenv("DM_SPV_DIR", d, 0);
        atomic_store(&g_state, ST_DEMO); g_demo_frames = nvn_demo_run(600);
        atomic_store(&g_state, g_demo_frames == 600 ? ST_PASS : ST_FAIL);
        vkb_pipeline_cache_save();   // the demo's pipelines: the next start loads them
        const char *rt = getenv("DM_RUMBLE_TEST");   // env.txt: a game's vibration, as hle_hid.c sends it: both motors at 0.8 for half a second, then off
        if (rt && atoi(rt)) {
            dm_vibration_update u[2] = {{0, 0, {0.8f, 160, 0.8f, 320}}, {0, 1, {0.8f, 160, 0.8f, 320}}}; dm_haptics_send(u, 2); LOG("rumble check: on");
            usleep(500000); u[0].value = u[1].value = (dm_vibration){0, 160, 0, 320}; dm_haptics_send(u, 2); LOG("rumble check: off");
        }
        const char *tt = getenv("DM_TEXT_TEST");   // env.txt: the software keyboard's box, as a game's swkbd call shows it (hle_swkbd.c), once after the demo
        if (tt && atoi(tt)) { char out[64] = ""; int r = front_text_input("Name", "Check of the text box", "Type a name", "Link", 10, out, sizeof out); LOG("text box check: result %d, text \"%s\"", r, out); }
    }
    int st = atomic_load(&g_state); if (st == ST_GAME || st == ST_SELFTEST) atomic_store(&g_state, ST_ENDED);
    return NULL;
}
// The guest's main thread is the thread main() runs on: it returns into bionic afterwards, so x18 goes back to bionic's value (dm.h dm_slots_x18_restore)
void front_run(void (*guest)(void)) { guest(); dm_slots_x18_restore(); }
static void start_host(void) {
    char code[600], buf[700];
    snprintf(code, sizeof code, "%s/game/code", g_ext);
    snprintf(buf, sizeof buf, "%s/save", g_int); mkdirs(buf); setenv("DM_SAVE", buf, 0);
    snprintf(buf, sizeof buf, "%s/temp", g_int); mkdirs(buf); setenv("DM_TEMP", buf, 0);
    snprintf(buf, sizeof buf, "%s/pipeline.cache", g_int); setenv("DM_PIPELINE_CACHE", buf, 0);   // gfx_vk_draw.inc
    setenv("DM_AUDIO", "1", 0);
    setenv("DM_OPMODE", "0", 0);   // a phone is a handheld: games draw at the panel's 720p, not the TV's 1080p (the Adreno 619 cannot keep docked passes at 60 fps); env.txt DM_OPMODE=1 overrides
    snprintf(buf, sizeof buf, "%s/dump", g_ext); setenv("DM_DUMP_DIR", buf, 0); if (getenv("DM_VK_DUMP")) mkdirs(buf);   // gfx_vk_draw.inc dump_write (files/dump_now asks for one)
    snprintf(buf, sizeof buf, "%s/main.fixups.tsv", code);
    if (exists(buf)) {
        snprintf(buf, sizeof buf, "%s/game", g_ext); setenv("DM_ROOT", buf, 0);
        snprintf(buf, sizeof buf, "%s/game/RomFS", g_ext);   // a RomFS folder, or the packing script's RomFS image (hle_fs.c reads one as it is)
        if (!exists(buf)) snprintf(buf, sizeof buf, "%s/game/RomFS.img", g_ext);
        if (exists(buf)) setenv("DM_ROMFS", buf, 0);
        snprintf(buf, sizeof buf, "%s/game/aoc", g_ext); if (exists(buf)) setenv("DM_AOC_DIR", buf, 0);
        snprintf(buf, sizeof buf, "%s/game/spv", g_ext); setenv("DM_SPV_DIR", buf, 0);   // the game's shaders translated for Vulkan (gfx_vk_draw.inc)
        // Shared fonts (hle_sdk16.c): the button icons from the packing script's nintendo_ext.ttf; the standard font is the system's Roboto
        snprintf(buf, sizeof buf, "%s/game/font", g_ext); if (exists(buf)) setenv("DM_FONT_DIR", buf, 0);
        if (exists("/system/fonts/Roboto-Regular.ttf")) setenv("DM_SHARED_FONT", "/system/fonts/Roboto-Regular.ttf", 0);
        g_argv[0] = "dmhost"; g_argv[1] = strdup(code); g_argc = 2;
        atomic_store(&g_state, ST_GAME); LOG("game in %s", code);
    } else {
        snprintf(buf, sizeof buf, "%s/selftest", g_int);
        if (!copy_assets("selftest", buf)) { LOG("[RED] no game in %s and no self test in the app's assets", code); atomic_store(&g_state, ST_FAIL); return; }
        setenv("DM_DBG", "1", 1);
        g_argv[0] = "dmhost"; g_argv[1] = strdup(buf); g_argv[2] = "main"; g_argc = 3;
        atomic_store(&g_state, ST_SELFTEST); LOG("no game in %s: running the self test", code);
    }
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_thread, NULL)) { LOG("[RED] cannot start the guest thread"); atomic_store(&g_state, ST_FAIL); return; }
    pthread_detach(th);
}

// ---------- Front end interface (front.h) ----------
void *front_metal_layer(void) { return NULL; }
int front_has_window(void) { return 1; }
int front_screen_max_fps(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) { if (title) LOG("progress: %s %ld / %ld", title, done, total); }
static jclass g_gp;   // org.nxnative.android.Gamepad (java/), from the activity's class loader
// The software keyboard (hle_swkbd.c): Gamepad.ask shows an input box on the UI thread and this (a guest) thread waits for it
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_gp || out_cap <= 0) return -1;
    JavaVM *vm = g_app->activity->vm; JNIEnv *e; int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) { if ((*vm)->AttachCurrentThread(vm, &e, NULL)) return -1; attached = 1; }
    jmethodID m = (*e)->GetStaticMethodID(e, g_gp, "ask", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    jstring jt = (*e)->NewStringUTF(e, title ? title : ""), js = (*e)->NewStringUTF(e, sub ? sub : ""), jg = (*e)->NewStringUTF(e, guide ? guide : ""), ji = (*e)->NewStringUTF(e, initial ? initial : "");
    jstring r = m ? (*e)->CallStaticObjectMethod(e, g_gp, m, g_app->activity->clazz, jt, js, jg, ji, (jint)max_chars) : NULL;
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); r = NULL; }
    int ok = r != NULL;
    if (ok) { const char *u = (*e)->GetStringUTFChars(e, r, NULL); snprintf(out, (size_t)out_cap, "%s", u ? u : ""); (*e)->ReleaseStringUTFChars(e, r, u); }
    LOG("software keyboard: %s", ok ? "confirmed" : "cancelled");
    if (attached) (*vm)->DetachCurrentThread(vm);
    return ok;
}
void front_android_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; atomic_fetch_add(&g_frames, 1); }

// ---------- Vulkan ----------
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkSurfaceKHR g_surf; static VkSwapchainKHR g_sc; static VkImage g_img[8]; static uint32_t g_nimg; static VkFormat g_fmt; static VkExtent2D g_ext2;
static VkCommandPool g_cpool; static VkCommandBuffer g_cb; static VkBuffer g_stg; static VkDeviceMemory g_stgm; static void *g_stgp; static VkDeviceSize g_stg_sz;
static VkFence g_fence; static VkSemaphore g_acq, g_done;
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("[RED] Vulkan: %s = %d", #x, r_); return 0; } } while (0)

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
// What the NVN replay will need from this GPU (BC textures, bindless descriptor pools, geometry and transform feedback, border colors)
static void vk_probe(void) {
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(g_pd, &pr);
    VkPhysicalDeviceDescriptorIndexingFeatures di = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &di};
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    VkPhysicalDeviceFeatures *f = &f2.features;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, e);
    LOG("vulkan: %s, API %u.%u.%u, driver %#x, vendor %#x device %#x", pr.deviceName, VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion),
        VK_API_VERSION_PATCH(pr.apiVersion), pr.driverVersion, pr.vendorID, pr.deviceID);
    LOG("vulkan textures: BC %d, ETC2 %d, ASTC LDR %d; geometry shader %d, tessellation %d, storage image write without format %d, fragment stores %d, independent blend %d, depth clamp %d, logic op %d, multi viewport %d, wide lines %d",
        f->textureCompressionBC, f->textureCompressionETC2, f->textureCompressionASTC_LDR, f->geometryShader, f->tessellationShader, f->shaderStorageImageWriteWithoutFormat,
        f->fragmentStoresAndAtomics, f->independentBlend, f->depthClamp, f->logicOp, f->multiViewport, f->wideLines);
    LOG("vulkan descriptor indexing: runtime arrays %d, partially bound %d, update after bind sampled images %d, nonuniform sampled image indexing %d, variable count %d",
        di.runtimeDescriptorArray, di.descriptorBindingPartiallyBound, di.descriptorBindingSampledImageUpdateAfterBind, di.shaderSampledImageArrayNonUniformIndexing, di.descriptorBindingVariableDescriptorCount);
    LOG("vulkan limits: per stage samplers %u, sampled images %u, storage buffers %u, uniform buffers %u; max image 2D %u, push constants %u, compute shared memory %u",
        pr.limits.maxPerStageDescriptorSamplers, pr.limits.maxPerStageDescriptorSampledImages, pr.limits.maxPerStageDescriptorStorageBuffers, pr.limits.maxPerStageDescriptorUniformBuffers,
        pr.limits.maxImageDimension2D, pr.limits.maxPushConstantsSize, pr.limits.maxComputeSharedMemorySize);
    static const char *want[] = {"VK_EXT_custom_border_color", "VK_EXT_transform_feedback", "VK_EXT_descriptor_indexing", "VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state",
                                 "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3", "VK_KHR_push_descriptor", "VK_EXT_index_type_uint8", "VK_EXT_depth_clip_enable",
                                 "VK_EXT_provoking_vertex", "VK_EXT_vertex_attribute_divisor", "VK_KHR_draw_indirect_count", "VK_EXT_conditional_rendering", "VK_KHR_shader_float16_int8",
                                 "VK_EXT_external_memory_host", "VK_KHR_buffer_device_address", "VK_EXT_descriptor_buffer"};
    char line[1024]; int k = snprintf(line, sizeof line, "vulkan extensions (%u):", ne);
    for (unsigned i = 0; i < sizeof want / sizeof *want && k < (int)sizeof line - 64; i++) k += snprintf(line + k, sizeof line - k, " %s %s", want[i] + 3, has_ext(e, ne, want[i]) ? "yes" : "no");
    LOG("%s", line); free(e);
}
static int vk_init(void) {   // the NVN replay's device (gfx_vk.c vkb): the window's swapchain and the game's textures are on one queue
    if (g_inst) return 1;
    vkb_ctx *c = vkb();
    if (!c || !c->has_surface || !c->has_swapchain) { LOG("[RED] Vulkan: no device with a window surface"); return 0; }
    g_inst = c->inst; g_pd = c->pd; g_dev = c->dev; g_q = c->q; g_qf = c->qf;
    vk_probe();
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf};
    VKC(vkCreateCommandPool(g_dev, &pc, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKC(vkAllocateCommandBuffers(g_dev, &ca, &g_cb));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VKC(vkCreateFence(g_dev, &fc, NULL, &g_fence));
    VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_acq)); VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_done));
    return 1;
}
static void vk_window_down(void) {
    if (!g_dev) return;
    pthread_mutex_lock(&vkb()->qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&vkb()->qmu);
    if (g_stg) { vkDestroyBuffer(g_dev, g_stg, NULL); vkFreeMemory(g_dev, g_stgm, NULL); g_stg = VK_NULL_HANDLE; g_stgp = NULL; }
    if (g_sc) { vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
    if (g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
}
static int vk_window_up(ANativeWindow *w) {
    if (!vk_init()) return 0;
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = w};
    VKC(vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &g_surf));
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &ok); if (!ok) { LOG("[RED] Vulkan: the queue cannot present"); return 0; }
    VkSurfaceCapabilitiesKHR cap; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap));
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fm);
    g_fmt = fm[0].format; VkColorSpaceKHR cs = fm[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { g_fmt = fm[i].format; cs = fm[i].colorSpace; }
    g_ext2 = cap.currentExtent.width != 0xFFFFFFFFu ? cap.currentExtent : (VkExtent2D){(uint32_t)ANativeWindow_getWidth(w), (uint32_t)ANativeWindow_getHeight(w)};
    // The picture is drawn upright in the window's orientation (the extent above) with the identity transform, and the compositor rotates it for the
    // display; the current transform (90° in landscape) would ask for a picture drawn pre-rotated
    VkSurfaceTransformFlagBitsKHR xf = cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = cap.minImageCount < 3 && cap.maxImageCount >= 3 ? 3 : cap.minImageCount,
        .imageFormat = g_fmt, .imageColorSpace = cs, .imageExtent = g_ext2, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf, .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = 1};
    if (!(cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VKC(vkCreateSwapchainKHR(g_dev, &sc, NULL, &g_sc));
    g_nimg = 8; VKC(vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img));
    // Staging buffer the status picture is drawn into (host visible), copied to the swapchain image each refresh
    g_stg_sz = (VkDeviceSize)g_ext2.width * g_ext2.height * 4;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = g_stg_sz, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VKC(vkCreateBuffer(g_dev, &bc, NULL, &g_stg));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_stg, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t mt = 0; for (; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    VKC(vkAllocateMemory(g_dev, &ma, NULL, &g_stgm)); VKC(vkBindBufferMemory(g_dev, g_stg, g_stgm, 0)); VKC(vkMapMemory(g_dev, g_stgm, 0, g_stg_sz, 0, &g_stgp));
    LOG("window %ux%u, swapchain %u images, format %d", g_ext2.width, g_ext2.height, g_nimg, g_fmt);
    return 1;
}

// ---------- Status picture: text drawn with android.graphics (JNI) into a bitmap, copied into the staging buffer ----------
static JNIEnv *g_env; static jobject g_bmp, g_canvas, g_paint; static jmethodID g_drawText, g_drawColor, g_setColor, g_setTextSize;
static int status_canvas(uint32_t w, uint32_t h) {
    JNIEnv *e = g_env;
    if (g_bmp) { (*e)->DeleteGlobalRef(e, g_bmp); (*e)->DeleteGlobalRef(e, g_canvas); (*e)->DeleteGlobalRef(e, g_paint); g_bmp = NULL; }
    jclass cfg = (*e)->FindClass(e, "android/graphics/Bitmap$Config"), bmp = (*e)->FindClass(e, "android/graphics/Bitmap");
    jclass cv = (*e)->FindClass(e, "android/graphics/Canvas"), pt = (*e)->FindClass(e, "android/graphics/Paint");
    if (!cfg || !bmp || !cv || !pt) { (*e)->ExceptionClear(e); return 0; }
    jobject argb = (*e)->GetStaticObjectField(e, cfg, (*e)->GetStaticFieldID(e, cfg, "ARGB_8888", "Landroid/graphics/Bitmap$Config;"));
    jobject b = (*e)->CallStaticObjectMethod(e, bmp, (*e)->GetStaticMethodID(e, bmp, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"), (jint)w, (jint)h, argb);
    jobject c = (*e)->NewObject(e, cv, (*e)->GetMethodID(e, cv, "<init>", "(Landroid/graphics/Bitmap;)V"), b);
    jobject p = (*e)->NewObject(e, pt, (*e)->GetMethodID(e, pt, "<init>", "(I)V"), (jint)1);   // ANTI_ALIAS_FLAG
    if ((*e)->ExceptionCheck(e) || !b || !c || !p) { (*e)->ExceptionClear(e); return 0; }
    g_bmp = (*e)->NewGlobalRef(e, b); g_canvas = (*e)->NewGlobalRef(e, c); g_paint = (*e)->NewGlobalRef(e, p);
    g_drawText = (*e)->GetMethodID(e, cv, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    g_drawColor = (*e)->GetMethodID(e, cv, "drawColor", "(I)V");
    g_setColor = (*e)->GetMethodID(e, pt, "setColor", "(I)V");
    g_setTextSize = (*e)->GetMethodID(e, pt, "setTextSize", "(F)V");
    return 1;
}
static void text(const char *s, float x, float y, float size, uint32_t argb) {
    JNIEnv *e = g_env; jstring js = (*e)->NewStringUTF(e, s);
    (*e)->CallVoidMethod(e, g_paint, g_setColor, (jint)argb); (*e)->CallVoidMethod(e, g_paint, g_setTextSize, (jfloat)size);
    (*e)->CallVoidMethod(e, g_canvas, g_drawText, js, (jfloat)x, (jfloat)y, g_paint); (*e)->DeleteLocalRef(e, js);
}
static void status_draw(void) {
    if (!g_sc || !g_stgp) return;
    static uint32_t cw, ch; if ((cw != g_ext2.width || ch != g_ext2.height) && status_canvas(g_ext2.width, g_ext2.height)) { cw = g_ext2.width; ch = g_ext2.height; }
    if (!g_bmp) return;
    JNIEnv *e = g_env; int st = atomic_load(&g_state);
    static const uint32_t bg[] = {0xFF101820, 0xFF101820, 0xFF0E3B1E, 0xFF4A1010, 0xFF101820, 0xFF202020, 0xFF4A1010, 0xFF101820};
    (*e)->CallVoidMethod(e, g_canvas, g_drawColor, (jint)bg[st]);
    float u = (float)(g_ext2.width < g_ext2.height ? g_ext2.width : g_ext2.height) / 20.f, y = 2.2f * u;
    text("nxnative for Android", u, y, 1.4f * u, 0xFFFFFFFF); y += 1.6f * u;
    static const char *what[] = {"starting", "self test running", "self test PASSED", "self test FAILED", "game running", "host finished", "host crashed: see log.txt", "NVN picture check"};
    char line[256]; snprintf(line, sizeof line, "state: %s", what[st]); text(line, u, y, 0.9f * u, 0xFFE0E0E0); y += 1.2f * u;
    if (st == ST_PASS) { text("Switch code ran natively: x18 slots, TLS, gates, imports, threads", u, y, 0.6f * u, 0xFFB0FFB0); y += 0.9f * u; }
    if (g_demo_frames) { snprintf(line, sizeof line, "NVN \u2192 Vulkan picture path: %d frames presented", g_demo_frames); text(line, u, y, 0.6f * u, 0xFFB0FFB0); y += 0.9f * u; }
    if (st == ST_GAME) {
        snprintf(line, sizeof line, "frames presented: %llu", (unsigned long long)atomic_load(&g_frames)); text(line, u, y, 0.9f * u, 0xFFE0E0E0); y += 1.2f * u;
        text("Loading the game...", u, y, 0.55f * u, 0xFFFFD080); y += 0.8f * u;
    }
    if (st == ST_SELFTEST || st == ST_PASS) { text("No game in Android/data/org.nxnative.android/files/game/", u, y, 0.55f * u, 0xFFA0A0A0); y += 0.8f * u; }
    pthread_mutex_lock(&g_txt_mu); snprintf(line, sizeof line, "%.120s", g_last); pthread_mutex_unlock(&g_txt_mu);
    text(line, u, (float)g_ext2.height - u, 0.45f * u, 0xFF909090);
    // bitmap → staging buffer (RGBA bytes; swapped to BGRA for a BGRA swapchain)
    void *px = NULL; AndroidBitmapInfo bi;
    if (AndroidBitmap_getInfo(e, g_bmp, &bi) || AndroidBitmap_lockPixels(e, g_bmp, &px) || !px) return;
    uint8_t *dst = g_stgp; const uint8_t *src = px; int swap = g_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t r = 0; r < g_ext2.height && r < bi.height; r++) {
        const uint8_t *s = src + (size_t)r * bi.stride; uint8_t *d = dst + (size_t)r * g_ext2.width * 4;
        if (!swap) memcpy(d, s, (size_t)g_ext2.width * 4);
        else for (uint32_t x = 0; x < g_ext2.width; x++) { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = s[4 * x + 3]; }
    }
    AndroidBitmap_unlockPixels(e, g_bmp);
    // present
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, g_acq, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(g_dev, 1, &g_fence);
    vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb, &cbi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_img[idx],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g_ext2.width, g_ext2.height, 1}};
    vkCmdCopyBufferToImage(g_cb, g_stg, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &g_done};
    pthread_mutex_lock(&vkb()->qmu); vkQueueSubmit(g_q, 1, &si, g_fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_done, .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&vkb()->qmu);
}

// ---------- The game's picture: its last presented window texture, scaled to fit the window (16:9 letterboxed), on black ----------
static uint64_t g_shown;
// The swapchain follows the window: made again when the window's size differs from it, or when presenting reports it out of date. Launched while the screen was
// locked, the app got a 1x1 window first and kept a 1x1 swapchain, which the compositor stretched over the screen (one texel of the picture: whole-screen colour flicker)
static int g_sc_stale;
static void sc_follow_window(void) {
    if (!g_app || !g_app->window || !g_sc) return;
    int ww = ANativeWindow_getWidth(g_app->window), wh = ANativeWindow_getHeight(g_app->window);
    if (!g_sc_stale && (ww <= 1 || wh <= 1 || ((uint32_t)ww == g_ext2.width && (uint32_t)wh == g_ext2.height))) return;
    LOG("window now %dx%d (swapchain %ux%u%s): swapchain made again", ww, wh, g_ext2.width, g_ext2.height, g_sc_stale ? ", out of date" : "");
    g_sc_stale = 0; vk_window_down(); vk_window_up(g_app->window); g_shown = 0;
}
static int frame_draw(void) {
    int st = atomic_load(&g_state);
    sc_follow_window();
    if (!g_sc || (st != ST_GAME && st != ST_DEMO)) return 0;
    vkb_frame fr; if (!vkb_frame_lock(&fr)) return 0;
    if (fr.n == g_shown) { vkb_frame_unlock(); return 1; }
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, g_acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc_stale = 1;   // made again before the next frame (sc_follow_window); not on SUBOPTIMAL, which the identity transform in landscape gives every frame
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vkb_frame_unlock(); return 1; }
    vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb, &cbi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_img[idx], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    // the crop rectangle into the window, scaled to fit with its aspect kept
    int cw = fr.crop[2] > 0 ? fr.crop[2] : (int)fr.w, ch = fr.crop[3] > 0 ? fr.crop[3] : (int)fr.h;
    double sx = (double)g_ext2.width / cw, sy = (double)g_ext2.height / ch, sc = sx < sy ? sx : sy;
    int dw = (int)(cw * sc), dh = (int)(ch * sc), dx = ((int)g_ext2.width - dw) / 2, dy = ((int)g_ext2.height - dh) / 2;
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{fr.crop[0], fr.crop[1], 0}, {fr.crop[0] + cw, fr.crop[1] + ch, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (fr.flip) { bl.srcOffsets[0].y = fr.crop[1] + ch; bl.srcOffsets[1].y = fr.crop[1]; }   // the window image is stored bottom-up (NVN's lower-left origin)
    vkCmdBlitImage(g_cb, fr.img, VK_IMAGE_LAYOUT_GENERAL, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &g_done};
    pthread_mutex_lock(&vkb()->qmu); vkQueueSubmit(g_q, 1, &si, g_fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_done, .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&vkb()->qmu);
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);   // the texture is held until the GPU has read it
    g_shown = fr.n; vkb_frame_unlock();
    return 1;
}

// ---------- Input ----------
// The touch controls (Gamepad.java) and a physical controller (the native input queue) are merged as dm/ios's DmGamepad.m does: buttons ORed, a tilted physical stick wins
static int g_ctl_used;
static uint64_t g_pad_btn; static int32_t g_axis[4];
static pthread_mutex_t g_in_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_touch_btn, g_shake_btn, g_pad_now; static int32_t g_touch_ax[4];
static int tilted(int32_t x, int32_t y) { return (int64_t)x * x + (int64_t)y * y > (int64_t)4000 * 4000; }
static void publish(void) {
    pthread_mutex_lock(&g_in_mu);
    int32_t lx = g_touch_ax[0], ly = g_touch_ax[1], rx = g_touch_ax[2], ry = g_touch_ax[3];
    if (tilted(g_axis[0], g_axis[1])) { lx = g_axis[0]; ly = g_axis[1]; }
    if (tilted(g_axis[2], g_axis[3])) { rx = g_axis[2]; ry = g_axis[3]; }
    dm_input_set(g_touch_btn | g_pad_now | g_shake_btn, lx, ly, rx, ry);
    pthread_mutex_unlock(&g_in_mu);
}
static void pad_now(uint64_t b) { pthread_mutex_lock(&g_in_mu); g_pad_now = b; pthread_mutex_unlock(&g_in_mu); publish(); }
static void JNICALL n_touchpad(JNIEnv *e, jclass c, jlong b, jint lx, jint ly, jint rx, jint ry) {
    (void)e; (void)c; pthread_mutex_lock(&g_in_mu); g_touch_btn = (uint64_t)b; g_touch_ax[0] = lx; g_touch_ax[1] = ly; g_touch_ax[2] = rx; g_touch_ax[3] = ry; pthread_mutex_unlock(&g_in_mu); publish();
    if (atomic_load(&g_state) != ST_GAME) LOG("input (no game): buttons %#llx, left stick %d %d, right stick %d %d", (unsigned long long)b, lx, ly, rx, ry);   // the controls' check without a game
}
static void JNICALL n_gametouch(JNIEnv *e, jclass c, jintArray ids, jfloatArray xs, jfloatArray ys, jint n) {
    (void)c; dm_touch t[16]; if (n > 16) n = 16; if (n < 0) n = 0;
    jint id[16]; jfloat x[16], y[16];
    if (n) { (*e)->GetIntArrayRegion(e, ids, 0, n, id); (*e)->GetFloatArrayRegion(e, xs, 0, n, x); (*e)->GetFloatArrayRegion(e, ys, 0, n, y); }
    for (int i = 0; i < n; i++) t[i] = (dm_touch){(uint32_t)id[i], x[i], y[i]};
    if (atomic_load(&g_state) != ST_GAME) LOG("touch screen (no game): %d finger(s)%s", n, n ? "" : " up");
    dm_touch_set(t, n);
}
static void JNICALL n_shake(JNIEnv *e, jclass c, jboolean on) { (void)e; (void)c; pthread_mutex_lock(&g_in_mu); g_shake_btn = on ? DM_BTN_SHAKE : 0; pthread_mutex_unlock(&g_in_mu); publish(); }
int dm_opmode(void);   // hle_oe.c
static jint JNICALL n_opmode(JNIEnv *e, jclass c) { (void)e; (void)c; return dm_opmode(); }
static jint JNICALL n_layout(JNIEnv *e, jclass c) { (void)e; (void)c; return DM_GAME_PAD_LAYOUT; }
static jstring JNICALL n_env(JNIEnv *e, jclass c, jstring n) { (void)c; const char *u = (*e)->GetStringUTFChars(e, n, NULL); const char *v = u ? getenv(u) : NULL; (*e)->ReleaseStringUTFChars(e, n, u); return v ? (*e)->NewStringUTF(e, v) : NULL; }
static jlong JNICALL n_frames(JNIEnv *e, jclass c) { (void)e; (void)c; return (jlong)atomic_load(&g_frames); }   // the frame-rate counter (DM_SHOW_FPS, Gamepad.java)
static void JNICALL n_log(JNIEnv *e, jclass c, jstring s) { (void)c; const char *u = (*e)->GetStringUTFChars(e, s, NULL); LOG("%s", u ? u : ""); (*e)->ReleaseStringUTFChars(e, s, u); }
// The Java side from the APK's classes.dex (java/, tools/build_apk.py), through the activity's class loader (a native thread's FindClass sees only the system's)
static void gamepad_start(void) {
    JNIEnv *e = g_env; jobject act = g_app->activity->clazz;
    jobject cl = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, (*e)->GetObjectClass(e, act), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jstring name = (*e)->NewStringUTF(e, "org.nxnative.android.Gamepad");
    jclass c = cl ? (*e)->CallObjectMethod(e, cl, (*e)->GetMethodID(e, (*e)->FindClass(e, "java/lang/ClassLoader"), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name) : NULL;
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); c = NULL; }
    if (!c) { LOG("[RED] no Java side in the app (classes.dex): no touch controls, no text input"); return; }
    static const JNINativeMethod nm[] = {{"nativeTouchPad", "(JIIII)V", (void *)n_touchpad}, {"nativeGameTouch", "([I[F[FI)V", (void *)n_gametouch},
        {"nativeShake", "(Z)V", (void *)n_shake}, {"nativeOpMode", "()I", (void *)n_opmode}, {"nativeDefaultLayout", "()I", (void *)n_layout}, {"nativeLog", "(Ljava/lang/String;)V", (void *)n_log}, {"nativeEnv", "(Ljava/lang/String;)Ljava/lang/String;", (void *)n_env}, {"nativeFrames", "()J", (void *)n_frames}};
    if ((*e)->RegisterNatives(e, c, nm, sizeof nm / sizeof *nm)) { (*e)->ExceptionClear(e); LOG("[RED] Gamepad natives not registered"); return; }
    g_gp = (*e)->NewGlobalRef(e, c);
    void dm_haptics_android_init(JavaVM *vm, jclass cls, JNIEnv *e); dm_haptics_android_init(g_app->activity->vm, g_gp, e);
    (*e)->CallStaticVoidMethod(e, g_gp, (*e)->GetStaticMethodID(e, g_gp, "start", "(Landroid/app/Activity;)V"), act);
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); }
}
static void gamepad_reset(void) {   // the app went away: nothing stays held
    if (g_gp) { JNIEnv *e = g_env; (*e)->CallStaticVoidMethod(e, g_gp, (*e)->GetStaticMethodID(e, g_gp, "reset_", "()V")); if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }
    g_pad_btn = 0; memset(g_axis, 0, sizeof g_axis); pad_now(0);
}
static uint64_t key_button(int32_t code) {
    switch (code) {   // by position: the bottom face button is the Switch's B, the right one A, the left one Y, the top one X
        case AKEYCODE_BUTTON_A: return DM_BTN_B; case AKEYCODE_BUTTON_B: return DM_BTN_A; case AKEYCODE_BUTTON_X: return DM_BTN_Y; case AKEYCODE_BUTTON_Y: return DM_BTN_X;
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS; case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: return DM_BTN_MINUS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
        default: return 0;
    }
}
static int32_t ax(float v) { if (v > -0.12f && v < 0.12f) v = 0; return (int32_t)(v * DM_STICK_MAX); }
static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    if (atomic_load(&g_state) != ST_GAME) return 0;
    int src = AInputEvent_getSource(ev);
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_button(AKeyEvent_getKeyCode(ev)); if (!b) return 0;
        if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN) g_pad_btn |= b; else if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP) g_pad_btn &= ~b;
        g_ctl_used = 1; pad_now(g_pad_btn); return 1;
    }
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if (src & AINPUT_SOURCE_JOYSTICK) {
        g_axis[0] = ax(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0)); g_axis[1] = -ax(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0));
        g_axis[2] = ax(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0)); g_axis[3] = -ax(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0));
        float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
        uint64_t b = g_pad_btn & ~(uint64_t)(DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_DOWN);
        if (hx < -0.5f) b |= DM_BTN_LEFT; if (hx > 0.5f) b |= DM_BTN_RIGHT; if (hy < -0.5f) b |= DM_BTN_UP; if (hy > 0.5f) b |= DM_BTN_DOWN;
        if (AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0) > 0.5f) b |= DM_BTN_ZL; else if (!(g_pad_btn & DM_BTN_ZL)) b &= ~(uint64_t)DM_BTN_ZL;
        if (AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0) > 0.5f) b |= DM_BTN_ZR; else if (!(g_pad_btn & DM_BTN_ZR)) b &= ~(uint64_t)DM_BTN_ZR;
        g_ctl_used = 1; pad_now(b); return 1;
    }
    if (src & AINPUT_SOURCE_TOUCHSCREEN) {   // the touch points as fractions of the window (the 16:9 picture once it is drawn)
        int act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK, up = AMotionEvent_getAction(ev) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        size_t n = AMotionEvent_getPointerCount(ev); dm_touch t[16]; int k = 0;
        float w = (float)ANativeWindow_getWidth(g_app->window), h = (float)ANativeWindow_getHeight(g_app->window);
        if (act != AMOTION_EVENT_ACTION_UP && act != AMOTION_EVENT_ACTION_CANCEL)
            for (size_t i = 0; i < n && k < 16; i++) {
                if (act == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == up) continue;
                float x = AMotionEvent_getX(ev, i) / w, y = AMotionEvent_getY(ev, i) / h;
                t[k++] = (dm_touch){(uint32_t)AMotionEvent_getPointerId(ev, i) & 15, x < 0 ? 0 : x > 0.999f ? 0.999f : x, y < 0 ? 0 : y > 0.999f ? 0.999f : y};
            }
        dm_touch_set(t, k); return 1;
    }
    return 0;
}

// ---------- Lifecycle ----------
static int g_started;
static void on_cmd(struct android_app *app, int32_t cmd) {
    int game = atomic_load(&g_state) == ST_GAME;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: if (app->window) vk_window_up(app->window); if (!g_started) { g_started = 1; start_host(); gamepad_start(); } break;
        case APP_CMD_TERM_WINDOW: vk_window_down(); break;
        case APP_CMD_GAINED_FOCUS: if (game) dm_focus_set(1); break;
        case APP_CMD_LOST_FOCUS: gamepad_reset(); if (game) dm_focus_set(2); break;
        case APP_CMD_PAUSE: vkb_pipeline_cache_save(); if (game) { dm_focus_set(3); dm_audio_suspend(1); } break;
        case APP_CMD_RESUME: if (game) { dm_audio_suspend(0); dm_focus_set(1); } break;
        default: break;
    }
}

void android_main(struct android_app *app) {
    g_app = app;
    snprintf(g_ext, sizeof g_ext, "%s", app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath);
    snprintf(g_int, sizeof g_int, "%s", app->activity->internalDataPath);
    mkdirs(g_ext); mkdirs(g_int);
    log_start();
    LOG("nxnative for Android: files %s, internal %s", g_ext, g_int);
    env_file("env.txt", 1); env_file("launcher.txt", 0);
    (*app->activity->vm)->AttachCurrentThread(app->activity->vm, &g_env, NULL);
    app->onAppCmd = on_cmd; app->onInputEvent = on_input;
    for (;;) {
        int events; struct android_poll_source *src;
        int game = atomic_load(&g_state) == ST_GAME || atomic_load(&g_state) == ST_DEMO;
        while (ALooper_pollOnce(g_sc ? (game ? 4 : 100) : -1, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { vk_window_down(); (*app->activity->vm)->DetachCurrentThread(app->activity->vm); _exit(0); }
            if (g_sc) break;
        }
        if (!frame_draw()) status_draw();
    }
}
