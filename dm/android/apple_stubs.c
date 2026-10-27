// Darwin-only interfaces the shared sources call that have no Android counterpart yet; each says what is missing.
#include "dm.h"
// thread_selfcounts (libSystem private): the calling thread's {instructions, cycles}. Android would need perf_event_open, which apps may not have: zeros
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
