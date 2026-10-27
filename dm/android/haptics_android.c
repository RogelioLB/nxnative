// Game vibration (hle_haptics.h) on Android: player 1's motors mixed into one level (dm_haptics_level, as the iPhone mixes them for its one engine) and played by
// the connected controller's vibrator, else the phone's (Gamepad.java rumble: a one-shot of a second at that amplitude, made again while it lasts, cancelled at 0).
// The sink runs on the game's thread under the haptics lock: it only stores the newest state; a thread of its own calls Java. DM_RUMBLE=0 turns it off,
// DM_RUMBLE_GAIN scales it (env.txt), as on Apple.
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "dm.h"
#include "hle_haptics.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_haptic_state g_state; static int g_new; static float g_gain = 1;
static JavaVM *g_vm; static jclass g_cls; static jmethodID g_rumble;
static void sink(dm_haptic_state s) {
    if (s.player != 0) return;   // the phone or controller is player 1
    pthread_mutex_lock(&g_mu); g_state = s; g_new = 1; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}
static void *worker(void *arg) {
    (void)arg; JNIEnv *e; if ((*g_vm)->AttachCurrentThread(g_vm, &e, NULL)) return NULL;
    float last = -1; struct timespec renewed = {0};
    for (;;) {
        pthread_mutex_lock(&g_mu);
        if (!g_new) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 500000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_cond_timedwait(&g_cv, &g_mu, &ts); }
        dm_haptic_state s = g_state; g_new = 0;
        pthread_mutex_unlock(&g_mu);
        float level = dm_haptics_level(s.motor, 2, g_gain).intensity;
        if (level < 0.02f) level = 0;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int renew = level > 0 && (now.tv_sec - renewed.tv_sec) * 1000 + (now.tv_nsec - renewed.tv_nsec) / 1000000 > 800;   // the one-shot lasts a second: renewed while the game keeps it on
        if (level != last || renew) {
            (*e)->CallStaticVoidMethod(e, g_cls, g_rumble, (jfloat)level);
            if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
            last = level; renewed = now;
        }
    }
    return NULL;
}
// front_android.c, once the Java side is loaded (cls: org.nxnative.android.Gamepad, a global reference)
void dm_haptics_android_init(JavaVM *vm, jclass cls, JNIEnv *e) {
    const char *on = getenv("DM_RUMBLE"), *gain = getenv("DM_RUMBLE_GAIN");
    if (gain && atof(gain) > 0) g_gain = (float)atof(gain);
    g_vm = vm; g_cls = cls; g_rumble = (*e)->GetStaticMethodID(e, cls, "rumble", "(F)V");
    if (!g_rumble) { (*e)->ExceptionClear(e); LOG("[RED] haptics: no Gamepad.rumble: vibration off"); return; }
    pthread_t t; if (pthread_create(&t, NULL, worker, NULL)) return;
    pthread_detach(t);
    dm_haptics_init(sink, !on || atoi(on) != 0);
    dm_haptics_route(0, 1);
    LOG("haptics: game vibration on the %s (gain %.2f)%s", "phone's or controller's vibrator", g_gain, on && !atoi(on) ? ", turned off by DM_RUMBLE=0" : "");
}
