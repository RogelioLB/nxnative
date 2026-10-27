// Android implementations of the Darwin stand-ins declared in compat/dm_android.h, and the x18 thread-slot block (dm.h dm_slot_get).
#include "dm.h"
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <linux/fs.h>
#include <CoreFoundation/CoreFoundation.h>
// The real Linux calls under the names compat/dm_android.h redirects
#undef getxattr
#undef setxattr
#undef removexattr

// ---- Thread slots ----
// One block per guest thread: a 4 KB page whose last 32 bytes are the slots (K18, KTLS, KTLR, KGATE in that order), then SCS_SIZE bytes of shadow
// call stack. x18 points at the start of the shadow call stack, so slot k is at x18 - 32 + 8k, and system code built with ShadowCallStack (which
// pushes return addresses at [x18] and moves x18 up and back) finds a stack of its own there. A guard page follows it.
#define SLOT_PAGE 4096
#define SCS_SIZE (16 * 1024)
#define SLOT_COUNT 4
typedef struct { uint8_t *map; size_t len; uint64_t bionic_x18; } slot_block;
static __thread slot_block t_blk;
static pthread_key_t g_blk_key; static pthread_once_t g_blk_once = PTHREAD_ONCE_INIT;
static inline uint64_t *slots(void) { return t_blk.map ? (uint64_t *)(t_blk.map + SLOT_PAGE - 8 * SLOT_COUNT) : NULL; }
// Thread exit: x18 goes back to the value bionic gave the thread before the block is unmapped (code that runs after this destructor may use the shadow call stack)
static void blk_free(void *p) {
    (void)p;
    if (!t_blk.map) return;
    __asm__ volatile("mov x18, %0" :: "r"(t_blk.bionic_x18) : "memory");
    munmap(t_blk.map, t_blk.len); t_blk.map = NULL;
}
static void blk_key_init(void) { if (pthread_key_create(&g_blk_key, blk_free)) die("cannot create the slot block key"); }
void dm_slots_thread_init(void) {
    if (!t_blk.map) {
        pthread_once(&g_blk_once, blk_key_init);
        size_t len = SLOT_PAGE + SCS_SIZE + 4096;
        uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) die("thread slot block: mmap failed");
        if (mprotect(m + SLOT_PAGE + SCS_SIZE, 4096, PROT_NONE)) die("thread slot block: guard page");
        uint64_t x18; __asm__ volatile("mov %0, x18" : "=r"(x18));
        t_blk.map = m; t_blk.len = len; t_blk.bionic_x18 = x18;
        pthread_setspecific(g_blk_key, m);   // only so that the destructor runs
    }
    __asm__ volatile("mov x18, %0" :: "r"(t_blk.map + SLOT_PAGE) : "memory");
}
void dm_slots_x18_restore(void) { if (t_blk.map) __asm__ volatile("mov x18, %0" :: "r"(t_blk.bionic_x18) : "memory"); }
int64_t dm_slot_keypage_value(pthread_key_t k) { return -8 * SLOT_COUNT + 8 * (int64_t)k; }
void *dm_slot_get(pthread_key_t k) { uint64_t *s = slots(); return s ? (void *)(uintptr_t)s[k] : NULL; }
void dm_slot_set(pthread_key_t k, const void *v) {
    if (!t_blk.map) dm_slots_thread_init();
    slots()[k] = (uint64_t)(uintptr_t)v;
}

// ---- Quality of service: a nice value on the calling thread ----
static __thread qos_class_t t_qos;
int dm_android_qos_set(qos_class_t q) { t_qos = q; setpriority(PRIO_PROCESS, (id_t)gettid(), dm_qos_nice(q)); return 0; }
qos_class_t dm_android_qos_get(void) { return t_qos ? t_qos : QOS_CLASS_DEFAULT; }

size_t dm_android_stack_info(void **top) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); }
    *top = (uint8_t *)lo + n; return n;
}

// ---- Signal context records (Linux arm64 sigcontext.__reserved: a list of {magic, size} records ending with magic 0) ----
static void *uc_record(ucontext_t *uc, uint32_t magic) {
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved, *e = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= e) {
        uint32_t m = ((uint32_t *)p)[0], sz = ((uint32_t *)p)[1];
        if (!m || sz < 8) return NULL;
        if (m == magic) return p;
        if (m == 0x45585401) { p = (uint8_t *)(uintptr_t)*(uint64_t *)(p + 8); e = p + 0x10000; continue; }   // EXTRA_MAGIC: the list goes on elsewhere
        p += sz;
    }
    return NULL;
}
_STRUCT_ARM_NEON_STATE64 *dm_android_uc_neon(ucontext_t *uc) {
    uint8_t *r = uc_record(uc, 0x46508001);   // FPSIMD_MAGIC
    static __thread _STRUCT_ARM_NEON_STATE64 none;
    return r ? (_STRUCT_ARM_NEON_STATE64 *)(void *)(r + 8) : (memset(&none, 0, sizeof none), &none);
}
uint64_t dm_android_uc_esr(ucontext_t *uc) { uint8_t *r = uc_record(uc, 0x45535201); return r ? *(uint64_t *)(r + 8) : 0; }   // ESR_MAGIC

// ---- Sections (hle.h tables): the linker bounds each with __start_<name> / __stop_<name> ----
const struct mach_header_64 _mh_execute_header = {0};
#define SECT(n) extern const uint8_t __start_##n[] __attribute__((weak)), __stop_##n[] __attribute__((weak));
SECT(swhle) SECT(swforce) SECT(swnvn)
const void *dm_android_section(const char *name, unsigned long *size) {
    const uint8_t *b = NULL, *e = NULL;
    if (!strcmp(name, "swhle")) { b = __start_swhle; e = __stop_swhle; }
    else if (!strcmp(name, "swforce")) { b = __start_swforce; e = __stop_swforce; }
    else if (!strcmp(name, "swnvn")) { b = __start_swnvn; e = __stop_swnvn; }
    *size = b && e ? (unsigned long)(e - b) : 0;
    return *size ? b : NULL;
}

// ---- Files ----
// clonefile: Android's file systems (f2fs, ext4) have no copy-on-write clone, so the tree is copied, extended attributes included (hle_fs.c keeps
// guest names in one). Save data trees are small (megabytes)
static void copy_xattrs(const char *s, const char *d) {
    char names[1024]; ssize_t n = llistxattr(s, names, sizeof names);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[1024]; ssize_t k = lgetxattr(s, names + i, v, sizeof v);
        if (k >= 0) lsetxattr(d, names + i, v, (size_t)k, 0);
    }
}
static int copy_file(const char *s, const char *d, mode_t mode) {
    int in = open(s, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(d, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[65536]; ssize_t n; int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) for (ssize_t o = 0; o < n;) { ssize_t w = write(out, buf + o, n - o); if (w < 0) { r = -1; goto done; } o += w; }
    if (n < 0) r = -1;
done: { int e = errno; close(in); if (close(out)) r = -1; errno = e; }
    if (!r) copy_xattrs(s, d);
    return r;
}
static int copy_tree(const char *s, const char *d) {
    struct stat st; if (lstat(s, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) ? copy_file(s, d, st.st_mode) : 0;
    if (mkdir(d, st.st_mode & 07777)) return -1;
    copy_xattrs(s, d);
    DIR *dir = opendir(s); if (!dir) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", s, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", d, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; r = -1; break; }
        r = copy_tree(a, b);
    }
    closedir(dir); return r;
}
int dm_android_clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    struct stat st; if (!lstat(dst, &st)) { errno = EEXIST; return -1; }
    return copy_tree(src, dst);
}
int dm_android_renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags & RENAME_SWAP) return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCHANGE);
    return rename(from, to);
}
// Darwin xattr names are free-form; Linux needs a namespace: the guest-name attribute (hle_fs.c FS_GUEST_NAME) goes in user.
static const char *xname(const char *n, char *buf, size_t sz) { if (!strncmp(n, "user.", 5)) return n; snprintf(buf, sz, "user.%s", n); return buf; }
ssize_t dm_android_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = xname(n, b, sizeof b);
    return opt & XATTR_NOFOLLOW ? lgetxattr(p, n, v, s) : getxattr(p, n, v, s);
}
int dm_android_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = xname(n, b, sizeof b);
    return opt & XATTR_NOFOLLOW ? lsetxattr(p, n, v, s, 0) : setxattr(p, n, v, s, 0);
}
int dm_android_removexattr(const char *p, const char *n, int opt) {
    char b[256]; n = xname(n, b, sizeof b);
    return opt & XATTR_NOFOLLOW ? lremovexattr(p, n) : removexattr(p, n);
}
#undef fcntl
int dm_android_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
    if (cmd == F_GETPATH) {
        char link[64]; snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, arg, MAXPATHLEN - 1);
        if (n < 0) return -1;
        ((char *)arg)[n] = 0; return 0;
    }
    if (cmd == F_FULLFSYNC) return fsync(fd);
    return fcntl(fd, cmd, arg);
}

// ---- sysctl: only KERN_PROC_PID's start time (main.c), from /proc/self/stat's starttime (clock ticks since boot) ----
#undef sysctl
int dm_android_sysctl(int *mib, unsigned n, void *out, size_t *outn, void *in, size_t inn) {
    (void)in; (void)inn;
    if (n != 4 || mib[0] != CTL_KERN || mib[1] != KERN_PROC || mib[2] != KERN_PROC_PID || !out || *outn < sizeof(struct kinfo_proc)) { errno = ENOTSUP; return -1; }
    FILE *f = fopen("/proc/self/stat", "r"); if (!f) return -1;
    char buf[1024]; size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
    char *p = strrchr(buf, ')'); if (!p) return -1;
    unsigned long long start = 0; int field = 2;
    for (char *t = strtok(p + 1, " "); t; t = strtok(NULL, " ")) if (++field == 22) { start = strtoull(t, NULL, 10); break; }
    struct timespec boot, real, mono; clock_gettime(CLOCK_BOOTTIME, &boot); clock_gettime(CLOCK_REALTIME, &real); (void)mono;
    double since_boot = (double)start / (double)sysconf(_SC_CLK_TCK);
    double up = boot.tv_sec + boot.tv_nsec / 1e9, wall = real.tv_sec + real.tv_nsec / 1e9 - (up - since_boot);
    struct kinfo_proc *kp = out; memset(kp, 0, sizeof *kp);
    kp->kp_proc.p_starttime.tv_sec = (time_t)wall; kp->kp_proc.p_starttime.tv_usec = (suseconds_t)((wall - (double)(time_t)wall) * 1e6);
    *outn = sizeof *kp; return 0;
}

// ---- dispatch semaphores ----
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when ? when : clock_gettime_nsec_np(CLOCK_MONOTONIC);
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}
dispatch_semaphore_t dispatch_semaphore_create(long value) {
    dispatch_semaphore_t s = calloc(1, sizeof *s); if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&s->c, &a); pthread_condattr_destroy(&a);
    s->v = value; return s;
}
long dispatch_semaphore_signal(dispatch_semaphore_t s) {
    pthread_mutex_lock(&s->m); long woke = s->v < 0; s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return woke;
}
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
    pthread_mutex_lock(&s->m);
    struct timespec ts = {(time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull)};
    while (s->v <= 0) {
        if (timeout == DISPATCH_TIME_FOREVER) pthread_cond_wait(&s->c, &s->m);
        else if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT && s->v <= 0) { pthread_mutex_unlock(&s->m); return 1; }
    }
    s->v--; pthread_mutex_unlock(&s->m); return 0;
}
void dispatch_release(void *o) {
    dispatch_semaphore_t s = o; if (!s) return;
    pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s);
}

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *d, CC_LONG n) {
    const uint8_t *p = d; c->len += n;
    while (n) {
        uint32_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 64) { sha_block(c->h, c->buf); c->n = 0; }
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1);
    while (c->n != 56) CC_SHA256_Update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, lb, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *d, CC_LONG n, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, d, n); CC_SHA256_Final(md, &c); return md; }

// mach_absolute_time as a real function, for assembly that calls it (nvn_entry.S; tools/asm_elf.py renames the call)
uint64_t dm_mach_absolute_time(void) { return mach_absolute_time(); }

// ---- thread_info: THREAD_BASIC_INFO from /proc/self/task/<tid>/stat (state) and the thread's CPU clock (user time; system time is not split) ----
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n) {
    if (flavor != THREAD_BASIC_INFO || !out) return KERN_INVALID_ARGUMENT;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)(void *)out; memset(bi, 0, sizeof *bi);
    clockid_t ck = (clockid_t)((~(unsigned)t << 3) | 6);   // MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)
    struct timespec ts; if (clock_gettime(ck, &ts)) return KERN_FAILURE;
    bi->user_time.seconds = (int)ts.tv_sec; bi->user_time.microseconds = (int)(ts.tv_nsec / 1000);
    char path[64], buf[256]; snprintf(path, sizeof path, "/proc/self/task/%u/stat", t);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t k = read(fd, buf, sizeof buf - 1); close(fd);
        if (k > 0) { buf[k] = 0; const char *p = strrchr(buf, ')');
            if (p && p[1] == ' ') switch (p[2]) {
                case 'R': bi->run_state = TH_STATE_RUNNING; break; case 'D': bi->run_state = TH_STATE_UNINTERRUPTIBLE; break;
                case 'T': case 't': bi->run_state = TH_STATE_STOPPED; break; case 'Z': case 'X': bi->run_state = TH_STATE_HALTED; break;
                default: bi->run_state = TH_STATE_WAITING; } }
    }
    if (n) *n = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}

// ---- CFMutableDictionary with pointer keys (open addressing, linear probing, tombstones) ----
struct dm_cfdict { const void **k, **v; size_t cap, n, used; };
#define CF_TOMB ((const void *)1)
static size_t cf_h(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k * 0x9E3779B97F4A7C15ull; return (size_t)(x >> 17) & (cap - 1); }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    struct dm_cfdict *d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); return d;
}
static const void **cf_slot(struct dm_cfdict *d, const void *k, int insert) {
    const void **tomb = NULL;
    for (size_t i = cf_h(k, d->cap);; i = (i + 1) & (d->cap - 1)) {
        if (d->k[i] == k) return &d->k[i];
        if (d->k[i] == CF_TOMB) { if (!tomb) tomb = &d->k[i]; continue; }
        if (!d->k[i]) return insert ? (tomb ? tomb : &d->k[i]) : NULL;
    }
}
static void cf_grow(struct dm_cfdict *d) {
    const void **ok = d->k, **ov = d->v; size_t oc = d->cap;
    d->cap *= 2; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->used = d->n;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ok[i] != CF_TOMB) { const void **s = cf_slot(d, ok[i], 1); *s = ok[i]; d->v[s - d->k] = ov[i]; }
    free(ok); free(ov);
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); return s ? d->v[s - d->k] : NULL; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    if ((d->used + 1) * 4 > d->cap * 3) cf_grow(d);
    const void **s = cf_slot(d, k, 1);
    if (*s != k) { if (!*s) d->used++; *s = k; d->n++; }
    d->v[s - d->k] = v;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); if (s) { *s = CF_TOMB; d->v[s - d->k] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(CFTypeRef o) { struct dm_cfdict *d = (struct dm_cfdict *)o; if (d) { free(d->k); free(d->v); free(d); } }

kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n) {
    (void)t; (void)n;
    if (flavor != TASK_VM_INFO) return KERN_FAILURE;
    FILE *f = fopen("/proc/self/statm", "r"); unsigned long long vs = 0, rs = 0;
    if (!f) return KERN_FAILURE;
    int ok = fscanf(f, "%llu %llu", &vs, &rs) == 2; fclose(f);
    if (!ok) return KERN_FAILURE;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)(void *)out; long pg = sysconf(_SC_PAGESIZE);
    vi->virtual_size = vs * pg; vi->resident_size = vi->phys_footprint = rs * pg; vi->resident_size_peak = 0;
    return KERN_SUCCESS;
}

kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t st, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)t; (void)mask; (void)st; (void)inh;
    if (copy) return KERN_FAILURE;
    void *r = mremap((void *)from, 0, size, MREMAP_MAYMOVE | ((flags & VM_FLAGS_ANYWHERE) ? 0 : MREMAP_FIXED), (void *)*to);
    if (r == MAP_FAILED) return KERN_FAILURE;
    *to = (vm_address_t)r; if (cur) *cur = VM_PROT_READ | VM_PROT_WRITE; if (max) *max = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    return KERN_SUCCESS;
}

// ---- Thread suspension (thread_suspend / thread_get_state / thread_resume): a real-time signal sent to the thread; its handler saves the registers in Darwin's
// layout (DM_SS) and waits on a futex word until the thread is resumed. Used by the host's hang reports and the sampling probe, and by the preemption tests ----
#include <linux/futex.h>
#define SUSP_MAX 128
typedef struct { _Atomic int tid; int count; _Atomic int stopped, go; struct __darwin_arm_thread_state64 ss; } susp_t;
static susp_t g_susp[SUSP_MAX];
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_susp_once = PTHREAD_ONCE_INIT;
static int fx_wait(_Atomic int *w, int v, const struct timespec *t) { return (int)syscall(SYS_futex, (int *)w, FUTEX_WAIT_PRIVATE, v, t, NULL, 0); }
static void fx_wake(_Atomic int *w) { syscall(SYS_futex, (int *)w, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0); }
int dm_android_suspend_signal(void) { return SIGRTMIN + 6; }
static susp_t *susp_of(int tid) { for (int i = 0; i < SUSP_MAX; i++) if (atomic_load(&g_susp[i].tid) == tid) return &g_susp[i]; return NULL; }
static void on_suspend(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; int e0 = errno;
    susp_t *s = susp_of(gettid());
    if (s) {
        s->ss = DM_SS((ucontext_t *)uc_);
        atomic_store(&s->stopped, 1); fx_wake(&s->stopped);
        while (!atomic_load(&s->go)) fx_wait(&s->go, 0, NULL);
        atomic_store(&s->stopped, 0); fx_wake(&s->stopped);
    }
    errno = e0;
}
static void susp_install(void) {
    struct sigaction sa = {.sa_sigaction = on_suspend, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigfillset(&sa.sa_mask);
    sigaction(dm_android_suspend_signal(), &sa, NULL);
}
kern_return_t thread_suspend(mach_port_t t) {
    if ((int)t == gettid() || !t) return KERN_INVALID_ARGUMENT;
    pthread_once(&g_susp_once, susp_install);
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t);
    if (s && s->count++ > 0) { pthread_mutex_unlock(&g_susp_mu); return KERN_SUCCESS; }
    if (!s) {
        for (int i = 0; i < SUSP_MAX && !s; i++) if (!atomic_load(&g_susp[i].tid)) s = &g_susp[i];
        if (!s) { pthread_mutex_unlock(&g_susp_mu); return KERN_FAILURE; }
        atomic_store(&s->stopped, 0); atomic_store(&s->go, 0); s->count = 1; atomic_store(&s->tid, (int)t);
    }
    pthread_mutex_unlock(&g_susp_mu);
    if (syscall(SYS_tgkill, getpid(), (int)t, dm_android_suspend_signal())) goto fail;
    // the thread is stopped once its handler runs; a thread that has the signal blocked or has exited never gets there (1 s)
    for (int i = 0; i < 100 && !atomic_load(&s->stopped); i++) { struct timespec ts = {0, 10000000}; fx_wait(&s->stopped, 0, &ts); }
    if (atomic_load(&s->stopped)) return KERN_SUCCESS;
fail:
    pthread_mutex_lock(&g_susp_mu); atomic_store(&s->go, 1); fx_wake(&s->go); atomic_store(&s->tid, 0); s->count = 0; pthread_mutex_unlock(&g_susp_mu);
    return KERN_FAILURE;
}
kern_return_t thread_resume(mach_port_t t) {
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t);
    if (!s || s->count <= 0) { pthread_mutex_unlock(&g_susp_mu); return KERN_FAILURE; }
    if (--s->count == 0) {
        atomic_store(&s->go, 1); fx_wake(&s->go);
        while (atomic_load(&s->stopped)) { struct timespec ts = {0, 1000000}; fx_wait(&s->stopped, 1, &ts); }   // out of the handler before the slot is reused
        atomic_store(&s->tid, 0);
    }
    pthread_mutex_unlock(&g_susp_mu);
    return KERN_SUCCESS;
}
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t out, mach_msg_type_number_t *n) {
    if (flavor != ARM_THREAD_STATE64) return KERN_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t); int ok = s && atomic_load(&s->stopped);
    if (ok) { memcpy(out, &s->ss, sizeof s->ss); if (n) *n = ARM_THREAD_STATE64_COUNT; }
    pthread_mutex_unlock(&g_susp_mu);
    return ok ? KERN_SUCCESS : KERN_FAILURE;
}
