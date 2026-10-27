// Android stand-in for libdispatch: counting semaphores only (the host's other dispatch uses are under #ifdef __APPLE__)
#pragma once
#include "dm_android.h"
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
typedef struct dm_dispatch_sem { pthread_mutex_t m; pthread_cond_t c; long v; } *dispatch_semaphore_t;
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);    // plat_android.c: CLOCK_MONOTONIC nanoseconds
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout);   // 0 when taken, non-zero on timeout
void dispatch_release(void *o);
