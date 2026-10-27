// Android (bionic, aarch64) stand-ins for the Darwin interfaces the host uses. Force-included into every host source
// built for Android (CMakeLists.txt: -include dm_android.h), so the shared sources keep their Apple calls; the Apple
// header names (mach/mach.h, dispatch/dispatch.h, ...) are small files beside this one that include it.
// Only the subset the host calls is provided, with the semantics the host relies on (see each comment).
#pragma once
#ifndef __ANDROID__
#error "dm_android.h is for Android builds only"
#endif
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define TARGET_OS_IPHONE 0
#define TARGET_OS_OSX 0
#define TARGET_OS_MAC 0
#define TARGET_OS_ANDROID 1

typedef int boolean_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

// ---- Mach VM and threads ----
typedef int kern_return_t;
typedef unsigned int mach_port_t, thread_act_t, mach_msg_type_number_t, vm_prot_t, vm_inherit_t;
typedef uintptr_t vm_address_t, vm_size_t, mach_vm_address_t, mach_vm_size_t, vm_offset_t;
#define vm_page_size ((vm_size_t)sysconf(_SC_PAGESIZE))   // Darwin's page size variable (hle_thread.c)
typedef thread_act_t *thread_act_array_t;
typedef int *thread_state_t, *thread_info_t, *vm_region_info_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_INVALID_ARGUMENT 4
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
// A read that fails instead of faulting: process_vm_readv on our own process returns EFAULT for an unreadable range
static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n < 0) { if (out) *out = 0; return KERN_FAILURE; }
    if (out) *out = (vm_size_t)n;
    return (vm_size_t)n == size ? KERN_SUCCESS : KERN_FAILURE;
}
static inline kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task; (void)flags;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
static inline kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_FAILURE : KERN_SUCCESS; }
// Diagnostics that suspend another thread to read its registers (hang reports, the sampling probe) have no Linux counterpart without
// ptrace: they report failure and the callers skip that thread
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT 68
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
#define MACH_PORT_NULL 0
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 9
typedef struct { vm_prot_t protection, max_protection; } vm_region_basic_info_data_64_t;
// Region queries only name a fault's region in a log line: no answer on Android
static inline kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t i, mach_msg_type_number_t *n, mach_port_t *o) {
    (void)t; (void)a; (void)s; (void)f; (void)i; (void)n; (void)o; return KERN_FAILURE; }
#define mach_vm_region(t, a, s, f, i, n, o) vm_region_64(t, (vm_address_t *)(a), (vm_size_t *)(s), f, i, n, o)
typedef struct { struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
// Thread basic info: run state and CPU time of a thread by its id ("port" = tid on Android), from /proc and the kernel's per-thread CPU clock
#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n);   // plat_android.c
static inline kern_return_t mach_vm_remap(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_size_t m, int f, mach_port_t st, mach_vm_address_t sa, boolean_t c, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t i) {
    (void)t; (void)a; (void)s; (void)m; (void)f; (void)st; (void)sa; (void)c; (void)cur; (void)max; (void)i; return KERN_FAILURE; }
// Task info: TASK_VM_INFO's footprint is the resident set (/proc/self/statm); the power counters Darwin keeps per task have no Android source
typedef int *task_info_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 87
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT 30
typedef struct { uint64_t virtual_size, resident_size, resident_size_peak, phys_footprint; } task_vm_info_data_t;
typedef struct { struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } cpu_energy;
                 struct { uint64_t task_gpu_utilisation; } gpu_energy; uint64_t task_ptime, task_energy; } task_power_info_v2_data_t;
kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n);   // plat_android.c
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT 40
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
static inline int sysctlbyname(const char *n, void *o, size_t *on, const void *i, size_t in) { (void)n; (void)o; (void)on; (void)i; (void)in; errno = ENOENT; return -1; }
// vm_remap: an alias of [from, from + size) at a fixed address. Linux aliases only shared mappings (mremap with old size 0); a private
// anonymous range cannot be aliased and the call fails (NVN's virtual pools over such memory then keep the mapping in their page table only)
kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t st, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh);   // plat_android.c
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
// Suspending another thread (a thread's "port" is its tid): a real-time signal whose handler saves the thread's registers and waits on a futex until
// thread_resume (plat_android.c). Nesting counts as Mach's does; a thread cannot suspend itself
kern_return_t thread_suspend(mach_port_t t);
kern_return_t thread_resume(mach_port_t t);
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t s, mach_msg_type_number_t *n);
int dm_android_suspend_signal(void);   // the signal the handler takes; guest threads unblock it (thread.c)
static inline kern_return_t task_threads(mach_port_t t, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)t; *a = NULL; *n = 0; return KERN_FAILURE; }
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
// Thread CPU time: the host keeps a "port" per thread; on Android it is the thread's CPU clock id (pthread_getcpuclockid), read by dm_thread_cpu_ns
typedef clockid_t dm_cpuclock_t;

// ---- Time ----
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; }
static inline uint64_t mach_absolute_time(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static inline kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return KERN_SUCCESS;
}
// Darwin's CLOCK_UPTIME_RAW does not advance in sleep, as CLOCK_MONOTONIC on Linux
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec ts; if (clock_gettime(c, &ts)) return 0; return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull

// ---- Instruction cache ----
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }

// ---- Thread quality of service: mapped to nice values (setpriority on the thread id) ----
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11,
               QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
static inline int dm_qos_nice(qos_class_t q) { return q >= QOS_CLASS_USER_INTERACTIVE ? -10 : q >= QOS_CLASS_USER_INITIATED ? -4 : q >= QOS_CLASS_DEFAULT ? 0 : q >= QOS_CLASS_UTILITY ? 5 : q ? 10 : 0; }
int dm_android_qos_set(qos_class_t q);                       // plat_android.c: records the class for pthread_get_qos_class_np
qos_class_t dm_android_qos_get(void);
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)rel; return dm_android_qos_set(q); }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = dm_android_qos_get(); if (rel) *rel = 0; return 0; }
typedef void *pthread_override_t;
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return NULL; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return (mach_port_t)gettid(); }
size_t dm_android_stack_info(void **top);                     // plat_android.c: the calling thread's stack (top address, size)
static inline void *pthread_get_stackaddr_np(pthread_t t) { (void)t; void *top; dm_android_stack_info(&top); return top; }
static inline size_t pthread_get_stacksize_np(pthread_t t) { (void)t; void *top; return dm_android_stack_info(&top); }
// Darwin's one-argument form names the calling thread; bionic's names any thread (the name is cut to 15 bytes)
static inline int dm_setname_self(const char *n) { char b[16]; strncpy(b, n, 15); b[15] = 0; return pthread_setname_np(pthread_self(), b); }
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, dm_setname_two, dm_setname_self)(__VA_ARGS__)
#define DM_SETNAME_PICK(a, b, f, ...) f
static inline int dm_setname_two(pthread_t t, const char *n) { char b[16]; strncpy(b, n, 15); b[15] = 0; return (pthread_setname_np)(t, b); }
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel->tv_sec; ts.tv_nsec += rel->tv_nsec; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, m, &ts);
}
static inline int pthread_threadid_np(pthread_t t, uint64_t *id) { (void)t; *id = (uint64_t)gettid(); return 0; }

// ---- malloc ----
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
typedef struct malloc_zone { int unused; } malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t a, unsigned f) { (void)a; (void)f; static malloc_zone_t z; return &z; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; void *p = NULL; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n) ? NULL : p; }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }

// ---- Random ----
#include <stdlib.h>   // bionic has arc4random / arc4random_buf

// ---- Signal context ----
// Darwin's thread state {x0-x28, fp, lr, sp, pc, cpsr} has the layout of Linux's sigcontext from regs[0] on {regs[0-30], sp, pc, pstate}, so the
// Darwin view is the same memory. The NEON state is the fpsimd_context record in the sigcontext's reserved area, whose body after its 8-byte
// header is {fpsr, fpcr, vregs[32]}: the fields are named as Darwin's (__fpsr, __fpcr, __v), in Linux's order
struct __darwin_arm_thread_state64 { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; };
typedef struct __darwin_arm_thread_state64 _STRUCT_ARM_THREAD_STATE64, arm_thread_state64_t;
typedef struct { uint32_t __fpsr, __fpcr; __uint128_t __v[32]; } _STRUCT_ARM_NEON_STATE64;
#define DM_SS(uc) (*(struct __darwin_arm_thread_state64 *)(void *)&(uc)->uc_mcontext.regs[0])
_STRUCT_ARM_NEON_STATE64 *dm_android_uc_neon(ucontext_t *uc);    // plat_android.c: the fpsimd record (a zeroed dummy when the kernel gave none)
#define DM_NS(uc) (*dm_android_uc_neon(uc))
uint64_t dm_android_uc_esr(ucontext_t *uc);                      // plat_android.c: the ESR from the esr_context record (0 when the kernel gave none)
#define DM_UC_ESR(uc) dm_android_uc_esr(uc)

// ---- Sections: Mach-O "__DATA,name" sections are ELF sections named "name", bounded by the linker's __start_/__stop_ symbols ----
#define DM_SECT(name) name
const void *dm_android_section(const char *name, unsigned long *size);   // plat_android.c
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
static inline const uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; return (const uint8_t *)dm_android_section(sect, size);
}

// ---- File system ----
#define XATTR_NOFOLLOW 1
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define RENAME_SWAP 2
int dm_android_clonefile(const char *src, const char *dst, int flags);   // plat_android.c: a recursive copy (no copy-on-write clone on Android's file systems)
int dm_android_renamex_np(const char *from, const char *to, unsigned flags);   // plat_android.c: RENAME_SWAP = renameat2 RENAME_EXCHANGE
#define clonefile dm_android_clonefile
#define renamex_np dm_android_renamex_np
// Darwin xattr calls take position / options arguments; the guest-name xattr lives in the user namespace. <sys/xattr.h> is included first so
// that its declarations keep their own names
#include <sys/xattr.h>
ssize_t dm_android_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt);
int dm_android_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt);
int dm_android_removexattr(const char *p, const char *n, int opt);
#define getxattr dm_android_getxattr
#define setxattr dm_android_setxattr
#define removexattr dm_android_removexattr
#define st_mtimespec st_mtim
#define st_atimespec st_atim
#define st_ctimespec st_ctim
#define O_EVTONLY O_RDONLY
// fcntl commands Darwin has and Linux lacks: F_GETPATH (the descriptor's path, from /proc/self/fd) and F_FULLFSYNC (fsync); others pass through
#define F_GETPATH 0x7d01
#define F_FULLFSYNC 0x7d02
int dm_android_fcntl(int fd, int cmd, ...);
#define fcntl dm_android_fcntl
#include <sys/param.h>
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

// mincore: Linux sets bit 0 for a resident page, as Darwin's MINCORE_INCORE
#define MINCORE_INCORE 1

// ---- strings ----
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
