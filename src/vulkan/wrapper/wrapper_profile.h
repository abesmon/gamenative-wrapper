#ifndef WRAPPER_PROFILE_H
#define WRAPPER_PROFILE_H

/*
 * Opt-in call census: how many Vulkan calls a client makes through the wrapper
 * and how long the wrapper spends inside each one.
 *
 * This answers a question neither of the other instruments can. WRAPPER_DEBUG=
 * trace prints only the calls the wrapper rewrites, and the api_dump layer
 * prints everything at a volume no running workload survives. What is missing
 * when one client is fast and another is slow through the same wrapper is the
 * shape of what each of them asks for: which entry points, how often, and
 * where the time goes.
 *
 * Enable with WRAPPER_PROFILE=1. Every window seconds (WRAPPER_PROFILE_SECONDS,
 * default 5) and again at vkDestroyDevice, the counters are written out sorted
 * by total time and then reset, so each window stands alone:
 *
 *   PROFILE window 5.00 s, 41231 calls
 *   PROFILE   vkMapMemory              count=302 total_ms=1503.2 mean_us=4977.5
 *   PROFILE   vkCmdDraw                count=9060 total_ms=12.4 mean_us=1.4
 *
 * Coverage is complete for every call the wrapper passes straight through:
 * those go through the generated trampolines and the generator instruments all
 * of them. The entry points the wrapper implements itself are instrumented by
 * hand, so the hot ones are covered and the rest read as absent rather than as
 * free. WRAPPER_PROFILE_SELF lists which ones are in.
 *
 * Disabled, the cost is one relaxed load of a cached int per call.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 when profiling is off, otherwise a monotonic timestamp in nanoseconds.
 * Zero doubles as "not being profiled", which is what keeps the disabled path
 * to a single branch. */
uint64_t wrapper_profile_begin(void);

/* start must be the value wrapper_profile_begin returned; 0 is ignored. name
 * must have static storage duration -- the table keeps the pointer. */
void wrapper_profile_end(uint64_t start, const char *name);

/* Write and reset the counters. reason names the trigger in the output. */
void wrapper_profile_dump(const char *reason);

/* Scope guard for the entry points the wrapper implements itself. Those have
 * several returns and goto-based error paths, so a timer that has to be
 * stopped by hand would be wrong on exactly the paths worth measuring. */
struct wrapper_profile_scope {
   uint64_t start;
   const char *name;
};

static inline void
wrapper_profile_scope_end(struct wrapper_profile_scope *scope)
{
   wrapper_profile_end(scope->start, scope->name);
}

#ifdef __cplusplus
}
#endif

#endif /* WRAPPER_PROFILE_H */
