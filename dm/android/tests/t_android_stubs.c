// What the graphics tests (dm/gfx/tests) need of the Android front end beside gfx_null.c: no window, presents only counted.
#include "dm.h"
#include "nvn_obj.h"
__attribute__((weak)) int front_has_window(void) { return 0; }
__attribute__((weak)) void front_android_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; }
// host functions the graphics tests do not link (hle_os.c, front end; nvn_mtl.m's test hook)
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }
__attribute__((weak)) void dm_guest_frame_tick(void) {}
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }
__attribute__((weak)) uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
__attribute__((weak)) void mtl_present_test_stall(uint64_t ns) { (void)ns; }
