// Command-line front end (dmhost for adb shell): no window, the guest runs on the calling thread (as the Mac host without DM_WINDOW).
// Scripted input comes from DM_KEYS (hle_hid.c), as on the Mac.
#include "dm.h"
#include "front.h"
#include "nvn_obj.h"
// DM_NVN_DEMO=<frames>: after the guest, the NVN demo (nvn_demo.c; with DM_SPV_DIR, its triangle), as the app runs it after the self test
void front_run(void (*guest)(void)) {
    guest();
    const char *d = getenv("DM_NVN_DEMO"); int nvn_demo_run(int frames);
    if (d) nvn_demo_run(atoi(d) > 0 ? atoi(d) : 3);
}
void *front_metal_layer(void) { return NULL; }
int front_has_window(void) { return 0; }
int front_screen_max_fps(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) { if (title) LOG("progress: %s %ld / %ld", title, done, total); }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1; }
void front_android_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; }
