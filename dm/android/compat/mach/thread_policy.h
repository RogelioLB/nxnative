// Android stand-in for <mach/thread_policy.h>: Mach's time-constraint (real-time) policy has no counterpart an app may set on Android
// (SCHED_FIFO needs a privilege apps lack), so setting and reading it fail and the callers keep the thread's ordinary priority
#pragma once
#include "dm_android.h"
typedef int *thread_policy_t;
typedef int thread_policy_flavor_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 4
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
static inline kern_return_t thread_policy_set(mach_port_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(mach_port_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)f; (void)p; (void)n; (void)def; return KERN_FAILURE; }
