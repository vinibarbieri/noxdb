/*
 * watermark.c - C4-B9 eviction watermark (spec 3.6). See watermark.h for the
 * mechanism, the "this is a fallback, not the Q1 trigger" caveat, and the
 * accepted `live > high` overshoot.
 *
 * Portability: _POSIX_C_SOURCE 200809L is what makes clock_gettime() and
 * CLOCK_MONOTONIC visible under -std=c11 (strict ISO hides everything POSIX).
 * CLOCK_MONOTONIC specifically, never CLOCK_REALTIME: this measures a stall
 * DURATION, and a wall-clock step (NTP slew, operator setting the date mid-run)
 * would corrupt the blocked_ns telemetry a soak run is being judged on.
 */
#define _POSIX_C_SOURCE 200809L

#include "watermark.h"

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

/*
 * ONE mutex + ONE condvar for the whole module. The counters live under that
 * mutex rather than in stdatomic: pthread_cond_wait() already demands the mutex
 * for its own lost-wakeup guarantee, so an atomic level would be a second,
 * racier view of a variable we must hold the lock to inspect anyway.
 *
 * Static initialisers rather than a nox_watermark_init(): there is exactly one
 * instance and no owner obvious enough to be trusted with calling init() before
 * the first foreground thread starts.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static int      g_armed;           /* 0 until arm(); back to 0 on disarm() */
static uint32_t g_high;
static uint32_t g_low;
static uint32_t g_live;            /* pages currently resident */

static uint64_t g_blocked_count;   /* wait() calls that ACTUALLY slept */
static uint64_t g_blocked_ns;      /* summed across threads, so it can exceed
                                    * wall time; that is the point -- it is
                                    * aggregate foreground stall, not latency */

static uint64_t now_ns(void)
{
    struct timespec ts;
    /* Cannot fail for CLOCK_MONOTONIC with a valid pointer; asserting keeps the
     * hot path free of an error branch that could never be exercised. */
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);
    (void)rc;                      /* NDEBUG builds: rc is otherwise unused */
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void nox_watermark_arm(uint32_t high, uint32_t low)
{
    /* low < high is the hysteresis band itself. low == high collapses the gate
     * into the wake/re-sleep storm described in watermark.h, and low > high is
     * a level that can never be reached from above -- a guaranteed deadlock.
     * Both are caller bugs, not runtime conditions, hence assert. */
    assert(low < high);

    pthread_mutex_lock(&g_lock);
    g_high  = high;
    g_low   = low;
    g_armed = 1;
    /* No broadcast: arming can only ADD a reason to sleep, never remove one. */
    pthread_mutex_unlock(&g_lock);
}

void nox_watermark_disarm(void)
{
    pthread_mutex_lock(&g_lock);
    g_armed = 0;
    /* Broadcast, not signal: disarm invalidates the sleep condition for EVERY
     * waiter at once, and a lone signal would strand the rest forever -- there
     * is no later event to hand the wakeup on to, since disarm is permanent. */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
}

void nox_watermark_wait(void)
{
    pthread_mutex_lock(&g_lock);

    /* Disarmed, or headroom left: the overwhelmingly common case. One
     * uncontended lock/unlock and out -- no condvar touched, no stat written. */
    if (!g_armed || g_live < g_high) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Committed to sleeping. Charge it to the stats exactly once, here, so that
     * blocked_count counts STALLS and not calls (spurious wakeups below must
     * not inflate it). */
    g_blocked_count++;
    uint64_t t0 = now_ns();

    /* Hysteresis lives in this predicate: entry needed live >= high, but the
     * loop only releases at live <= low. Testing against g_high here instead
     * would wake the sleeper at high-1, i.e. straight back into the storm. The
     * loop also absorbs spurious wakeups, which pthread_cond_wait permits. */
    while (g_armed && g_live > g_low)
        pthread_cond_wait(&g_cv, &g_lock);

    g_blocked_ns += now_ns() - t0;

    pthread_mutex_unlock(&g_lock);
}

void nox_watermark_note_alloc(void)
{
    pthread_mutex_lock(&g_lock);
    g_live++;
    /* Deliberately no gate check: the caller already passed wait() and owns a
     * page by now. Blocking here would strand a half-built allocation, and it
     * is the source of the accepted overshoot documented in watermark.h. */
    pthread_mutex_unlock(&g_lock);
}

void nox_watermark_note_free(void)
{
    pthread_mutex_lock(&g_lock);
    assert(g_live > 0);            /* unbalanced free: a page-lifecycle bug */
    g_live--;

    /* Broadcast only on the DOWNWARD crossing into the release band. Signalling
     * on every free would be one syscall per reclaimed page on the flusher's
     * hot path; and it must be a broadcast because the whole waiting set is
     * released together -- that batching is what the band buys us. */
    if (g_armed && g_live <= g_low)
        pthread_cond_broadcast(&g_cv);

    pthread_mutex_unlock(&g_lock);
}

uint32_t nox_watermark_live(void)
{
    pthread_mutex_lock(&g_lock);
    uint32_t v = g_live;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t nox_watermark_blocked_count(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_blocked_count;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t nox_watermark_blocked_ns(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_blocked_ns;
    pthread_mutex_unlock(&g_lock);
    return v;
}

void nox_watermark_reset_for_test(void)
{
    pthread_mutex_lock(&g_lock);
    g_armed         = 0;
    g_high          = 0;
    g_low           = 0;
    g_live          = 0;
    g_blocked_count = 0;
    g_blocked_ns    = 0;
    /* Broadcast anyway: a leaked waiter from a previous case would otherwise
     * sleep forever and hang the whole test binary instead of failing loudly. */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
}
