// Android stand-in for <sys/sysctl.h>: only the process start-time query main.c makes (KERN_PROC_PID), answered from /proc/self/stat
#pragma once
#include "dm_android.h"
#include <sys/time.h>
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
struct kinfo_proc { struct { struct { struct timeval __p_starttime; } p_un; } kp_proc; };
#define p_starttime p_un.__p_starttime
int dm_android_sysctl(int *mib, unsigned n, void *out, size_t *outn, void *in, size_t inn);
#define sysctl dm_android_sysctl
