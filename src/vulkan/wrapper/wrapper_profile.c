/*
 * Opt-in per-entry-point call census. See wrapper_profile.h for what it is for
 * and how to read it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wrapper_profile.h"
#include "wrapper_log.h"

#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"

/* Written through the log sink directly rather than through WRAPPER_LOG(info).
 * WRAPPER_PROFILE is already the gate, and routing the output through the info
 * level would mean a profiling run had to switch on every other info message
 * too -- including ones on paths a frame touches, which would move the very
 * numbers this exists to collect. Lines are tagged "profile" in the file. */
#define WRAPPER_PROFILE_LOG(fmt, ...) write_to_logfile(fmt, "profile", ##__VA_ARGS__)

/* Open addressing, never resized, never deleted from: the key space is the
 * Vulkan entry point list, which is a few hundred names, and a run only ever
 * touches the subset one client uses. Sized well above that so probing stays
 * short and an overflow cannot happen in practice; if it ever did, the extra
 * calls are dropped rather than mis-attributed. */
#define WRAPPER_PROFILE_SLOTS 1024

struct wrapper_profile_slot {
   const char *name;      /* static storage; NULL means the slot is free */
   uint64_t count;
   uint64_t total_ns;
   uint64_t max_ns;
};

static struct wrapper_profile_slot slots[WRAPPER_PROFILE_SLOTS];
static simple_mtx_t insert_mutex = SIMPLE_MTX_INITIALIZER;
static uint64_t window_start_ns;
static uint64_t window_interval_ns;

/* -1 until the environment has been read. Written once; a racing second reader
 * computes the same value, so no lock is needed on the hot path. */
static int profile_enabled = -1;

static bool
wrapper_profile_check_enabled(void)
{
   int enabled = p_atomic_read(&profile_enabled);
   if (enabled >= 0)
      return enabled != 0;

   const char *env = getenv("WRAPPER_PROFILE");
   enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;

   if (enabled) {
      const char *seconds = getenv("WRAPPER_PROFILE_SECONDS");
      double window = seconds ? atof(seconds) : 5.0;
      if (window <= 0.0)
         window = 5.0;
      window_interval_ns = (uint64_t)(window * 1000000000.0);
      window_start_ns = os_time_get_nano();
      WRAPPER_PROFILE_LOG("Call profile enabled, window %.2f s", window);
   }

   p_atomic_set(&profile_enabled, enabled);
   return enabled != 0;
}

uint64_t
wrapper_profile_begin(void)
{
   if (!wrapper_profile_check_enabled())
      return 0;

   uint64_t now = os_time_get_nano();
   /* 0 is the disabled sentinel, so a timestamp of exactly 0 must not be
    * returned. Losing one nanosecond at the epoch costs nothing. */
   return now ? now : 1;
}

static uint32_t
name_hash(const char *name)
{
   /* FNV-1a. The keys are short ASCII identifiers. */
   uint32_t h = 2166136261u;
   for (const char *p = name; *p; p++) {
      h ^= (uint8_t)*p;
      h *= 16777619u;
   }
   return h;
}

static struct wrapper_profile_slot *
find_slot(const char *name)
{
   uint32_t idx = name_hash(name) % WRAPPER_PROFILE_SLOTS;

   for (uint32_t probe = 0; probe < WRAPPER_PROFILE_SLOTS; probe++) {
      struct wrapper_profile_slot *slot =
         &slots[(idx + probe) % WRAPPER_PROFILE_SLOTS];
      const char *existing = p_atomic_read(&slot->name);

      if (existing && !strcmp(existing, name))
         return slot;

      if (!existing) {
         /* Claim it. Two threads meeting the same free slot must not both take
          * it, and the loser must keep probing from where it was. */
         simple_mtx_lock(&insert_mutex);
         existing = slot->name;
         if (!existing)
            slot->name = name;
         simple_mtx_unlock(&insert_mutex);

         if (!existing || !strcmp(slot->name, name))
            return slot;
      }
   }
   return NULL;  /* table full: drop the sample rather than mis-attribute it */
}

void
wrapper_profile_end(uint64_t start, const char *name)
{
   if (!start)
      return;

   uint64_t now = os_time_get_nano();
   uint64_t elapsed = now > start ? now - start : 0;

   struct wrapper_profile_slot *slot = find_slot(name);
   if (slot) {
      p_atomic_inc(&slot->count);
      p_atomic_add(&slot->total_ns, elapsed);
      /* Racy on purpose: the maximum is a hint about outliers, and a lock here
       * would sit on the hottest path in the process. */
      if (elapsed > slot->max_ns)
         slot->max_ns = elapsed;
   }

   if (window_interval_ns && now - p_atomic_read(&window_start_ns) >= window_interval_ns)
      wrapper_profile_dump("window");
}

static int
by_total_desc(const void *a, const void *b)
{
   const struct wrapper_profile_slot *x = a, *y = b;
   if (x->total_ns > y->total_ns) return -1;
   if (x->total_ns < y->total_ns) return 1;
   return 0;
}

void
wrapper_profile_dump(const char *reason)
{
   if (p_atomic_read(&profile_enabled) != 1)
      return;

   /* Take the window before reading the table, so calls landing during the
    * dump are counted against the next window rather than lost. */
   uint64_t now = os_time_get_nano();
   uint64_t started = p_atomic_xchg(&window_start_ns, now);
   double seconds = (double)(now - started) / 1000000000.0;

   struct wrapper_profile_slot live[WRAPPER_PROFILE_SLOTS];
   unsigned n = 0;
   uint64_t calls = 0;

   for (unsigned i = 0; i < WRAPPER_PROFILE_SLOTS; i++) {
      if (!slots[i].name)
         continue;
      uint64_t count = p_atomic_xchg(&slots[i].count, 0);
      uint64_t total = p_atomic_xchg(&slots[i].total_ns, 0);
      uint64_t max = p_atomic_xchg(&slots[i].max_ns, 0);
      if (!count)
         continue;
      live[n].name = slots[i].name;
      live[n].count = count;
      live[n].total_ns = total;
      live[n].max_ns = max;
      calls += count;
      n++;
   }

   if (!n)
      return;

   qsort(live, n, sizeof(live[0]), by_total_desc);

   WRAPPER_PROFILE_LOG("PROFILE %s: %.2f s, %llu calls, %u entry points",
               reason, seconds, (unsigned long long)calls, n);

   for (unsigned i = 0; i < n; i++) {
      WRAPPER_PROFILE_LOG(
         "PROFILE   %-40s count=%llu per_s=%.1f total_ms=%.3f mean_us=%.2f max_us=%.1f",
         live[i].name,
         (unsigned long long)live[i].count,
         seconds > 0.0 ? (double)live[i].count / seconds : 0.0,
         (double)live[i].total_ns / 1000000.0,
         (double)live[i].total_ns / 1000.0 / (double)live[i].count,
         (double)live[i].max_ns / 1000.0);
   }
}
