// The NativeActivity's library (apk/AndroidManifest.xml android.app.lib_name): one APK carries the host of every game packed on the computer, each built for its
// profile as lib/arm64-v8a/libdmhost_<title ID>.so (CMakeLists.txt), and this loader starts the one the launcher picked (java Launcher.java): the activity's intent
// extra "game" names it. The game runs in a process of its own (android:process=":game"): a host cannot be loaded twice, so every launch is a fresh process.
// Extra "selftest" (boolean): the picked host runs its built-in self test instead of its game (front_android.c reads DM_FORCE_SELFTEST).
#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "nxnative", __VA_ARGS__)

// the intent's extras (getIntent().getStringExtra / getBooleanExtra) on the activity object
static int intent_extras(ANativeActivity *a, char *game, size_t cap, int *selftest) {
    JNIEnv *e = a->env; jobject act = a->clazz; game[0] = 0; *selftest = 0;
    jclass ac = (*e)->GetObjectClass(e, act);
    jmethodID gi = (*e)->GetMethodID(e, ac, "getIntent", "()Landroid/content/Intent;");
    jobject in = gi ? (*e)->CallObjectMethod(e, act, gi) : NULL;
    if (!in) { (*e)->ExceptionClear(e); return 0; }
    jclass ic = (*e)->GetObjectClass(e, in);
    jmethodID gs = (*e)->GetMethodID(e, ic, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID gb = (*e)->GetMethodID(e, ic, "getBooleanExtra", "(Ljava/lang/String;Z)Z");
    jstring key = (*e)->NewStringUTF(e, "game"), skey = (*e)->NewStringUTF(e, "selftest");
    jstring v = gs ? (jstring)(*e)->CallObjectMethod(e, in, gs, key) : NULL;
    if (v) { const char *s = (*e)->GetStringUTFChars(e, v, NULL); if (s) { snprintf(game, cap, "%s", s); (*e)->ReleaseStringUTFChars(e, v, s); } }
    if (gb) *selftest = (*e)->CallBooleanMethod(e, in, gb, skey, JNI_FALSE);
    (*e)->ExceptionClear(e);
    return game[0] != 0;
}

static int valid_id(const char *s) {   // 16 hexadecimal digits: no path can be made of it
    if (strlen(s) != 16) return 0;
    for (const char *p = s; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size) {
    char game[64]; int selftest = 0;
    if (!intent_extras(a, game, sizeof game, &selftest) || !valid_id(game)) {
        LOG("[RED] loader: started without a game (intent extra \"game\" = a title ID): nothing to run"); ANativeActivity_finish(a); return; }
    char lib[96]; snprintf(lib, sizeof lib, "libdmhost_%s.so", game);
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);   // the app's native library folder is on the loader's search path
    if (!h) { LOG("[RED] loader: %s is not in this app (%s): update the app from the computer (scripts/build_android_from_nsp.py)", lib, dlerror()); ANativeActivity_finish(a); return; }
    void (*create)(ANativeActivity *, void *, size_t) = (void (*)(ANativeActivity *, void *, size_t))dlsym(h, "ANativeActivity_onCreate");
    if (!create) { LOG("[RED] loader: %s has no ANativeActivity_onCreate", lib); ANativeActivity_finish(a); return; }
    if (selftest) setenv("DM_FORCE_SELFTEST", "1", 1);
    LOG("loader: %s%s", lib, selftest ? " (self test)" : "");
    create(a, saved, saved_size);
}
