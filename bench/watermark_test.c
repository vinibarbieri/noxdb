/*
 * watermark_test.c - unit test for the C4-B9 eviction watermark (spec 3.6).
 *
 * Runs LOCALLY: pure pthreads + RAM, no O_DIRECT, no NVMe. Build with
 * `make test-watermark` (and `make test-watermark-tsan` for the race gate).
 * Links watermark.c only -- the gate is deliberately free of engine symbols.
 *
 * The "is it still blocked?" checks are the usual negative-property problem:
 * you cannot prove a thread will never wake, only that it has not woken after a
 * grace period. SETTLE_MS is that grace period, sized far above any plausible
 * condvar wakeup latency so a false PASS would need a pathologically starved
 * scheduler. The flag the waiter sets is _Atomic because it is read by main
 * while the waiter may be writing it -- a plain int there is a data race and
 * TSan would (correctly) report it as a bug in the TEST.
 */
#define _POSIX_C_SOURCE 200809L

#include "watermark.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SETTLE_MS 120

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- shared waiter thread ------------------------------------------------ */

typedef struct {
    atomic_int woke;               /* 0 while parked in wait(), 1 after */
} waiter_t;

static void *waiter_fn(void *arg)
{
    waiter_t *w = arg;
    nox_watermark_wait();
    atomic_store(&w->woke, 1);
    return NULL;
}

static void alloc_n(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        nox_watermark_note_alloc();
}

/* ---- 1: disarmed gate is a no-op ----------------------------------------- */

static void test_disarmed_never_blocks(void)
{
    nox_watermark_reset_for_test();

    /* 10 live pages with no marks configured at all: the level is meaningless
     * until somebody arms the gate, so wait() must not even look at it. If it
     * did, this call would hang the test binary rather than fail it. */
    alloc_n(10);
    nox_watermark_wait();

    assert(nox_watermark_live() == 10);
    assert(nox_watermark_blocked_count() == 0);
    assert(nox_watermark_blocked_ns() == 0);
    printf("  disarmed passthrough      OK\n");
}

/* ---- 2: armed but below high --------------------------------------------- */

static void test_armed_below_high(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(8, 4);

    alloc_n(4);                    /* 4 < high: headroom, no stall */
    nox_watermark_wait();

    assert(nox_watermark_blocked_count() == 0);
    assert(nox_watermark_blocked_ns() == 0);
    printf("  armed, live < high        OK\n");
}

/* ---- 3: at high, block; at low, wake ------------------------------------- */

static void test_blocks_at_high_wakes_at_low(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(4, 2);
    alloc_n(4);                    /* live == high: the gate must bind */

    waiter_t w = { 0 };
    pthread_t t;
    assert(pthread_create(&t, NULL, waiter_fn, &w) == 0);

    msleep(SETTLE_MS);
    assert(atomic_load(&w.woke) == 0);

    nox_watermark_note_free();     /* 3 */
    nox_watermark_note_free();     /* 2 == low -> release */
    pthread_join(t, NULL);

    assert(atomic_load(&w.woke) == 1);
    assert(nox_watermark_blocked_count() == 1);
    assert(nox_watermark_blocked_ns() > 0);
    printf("  blocks at high / wakes    OK (%llu ns blocked)\n",
           (unsigned long long)nox_watermark_blocked_ns());
}

/* ---- 4: hysteresis -- low+1 is NOT a wakeup ------------------------------ */

static void test_hysteresis_band(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(4, 2);
    alloc_n(4);

    waiter_t w = { 0 };
    pthread_t t;
    assert(pthread_create(&t, NULL, waiter_fn, &w) == 0);
    msleep(SETTLE_MS);
    assert(atomic_load(&w.woke) == 0);

    /* Down to low+1. This is BELOW high, so a level-triggered gate would let
     * the waiter through here -- exactly the wake/re-sleep storm the band
     * exists to prevent. It must stay parked. */
    nox_watermark_note_free();
    msleep(SETTLE_MS);
    assert(atomic_load(&w.woke) == 0);
    assert(nox_watermark_live() == 3);

    nox_watermark_note_free();     /* now at low: release */
    pthread_join(t, NULL);
    assert(atomic_load(&w.woke) == 1);
    printf("  hysteresis (low+1 holds)  OK\n");
}

/* ---- 5: disarm releases everyone, permanently ---------------------------- */

static void test_disarm_releases(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(4, 2);
    alloc_n(4);

    waiter_t w = { 0 };
    pthread_t t;
    assert(pthread_create(&t, NULL, waiter_fn, &w) == 0);
    msleep(SETTLE_MS);
    assert(atomic_load(&w.woke) == 0);

    nox_watermark_disarm();        /* shutdown path: nobody is left parked */
    pthread_join(t, NULL);
    assert(atomic_load(&w.woke) == 1);

    /* Still 4 live == high, but disarm is permanent: no further gating. A hang
     * here would mean a foreground thread could outlive the flusher. */
    assert(nox_watermark_live() == 4);
    nox_watermark_wait();
    assert(nox_watermark_blocked_count() == 1);   /* the pre-disarm stall only */
    printf("  disarm wakes + stays off  OK\n");
}

/* ---- 6: a broadcast releases the whole waiting set ----------------------- */

#define NWAIT 4

static void test_multiple_waiters(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(4, 2);
    alloc_n(4);

    waiter_t w[NWAIT] = { { 0 }, { 0 }, { 0 }, { 0 } };
    pthread_t t[NWAIT];
    for (int i = 0; i < NWAIT; i++)
        assert(pthread_create(&t[i], NULL, waiter_fn, &w[i]) == 0);

    msleep(SETTLE_MS);
    for (int i = 0; i < NWAIT; i++)
        assert(atomic_load(&w[i].woke) == 0);

    /* Two frees reach low. If note_free() signalled instead of broadcasting,
     * three of these four would still be parked and the join would hang. */
    nox_watermark_note_free();
    nox_watermark_note_free();
    for (int i = 0; i < NWAIT; i++)
        pthread_join(t[i], NULL);

    for (int i = 0; i < NWAIT; i++)
        assert(atomic_load(&w[i].woke) == 1);
    assert(nox_watermark_blocked_count() == NWAIT);
    printf("  %d waiters, one broadcast  OK\n", NWAIT);
}

/* ---- 7: stress ----------------------------------------------------------- */

/*
 * 8 threads x 2000 iterations of the real foreground shape:
 *     wait(); note_alloc(); ...hold...; note_free()
 *
 * WHY THE MARKS ARE THIS SMALL. In this test every free is issued by a thread
 * that is itself subject to the gate -- there is no independent flusher -- and
 * that constrains the marks from both sides. Let H_i be the pages thread i
 * holds at its peak, so it holds at most H_i - 1 when it next calls wait():
 *
 *   (a) LIVENESS. Pages held by a blocked thread are frozen. If every thread
 *       parks, the level sticks at sum(H_i - 1), and the waiters only release
 *       at `low`. So sum(H_i - 1) <= low is required, or the run hangs.
 *   (b) COVERAGE. The gate only binds if the level can reach `high`, i.e.
 *       sum(H_i) >= high.
 *
 * Together those need low + N >= high: the band cannot be wider than the thread
 * count. high=64/low=48 with 8 threads fails that by a mile (56 < 64) -- the
 * level provably never reaches 64, and forcing it to (bigger per-thread holds)
 * buys a deadlock instead. Hence high=6/low=3 with each thread holding exactly
 * one page: sum(H_i - 1) = 0 <= 3 and sum(H_i) = 8 >= 6, both satisfied with
 * room to spare, so the gate binds constantly and can always release.
 *
 * This sizing also makes the test exercise the accepted overshoot from
 * watermark.h rather than merely tolerating it: threads clear the gate at
 * live <= 5 and only then allocate, so all 8 can slip through and push the
 * level to high + 2. That is why the bound below is high + THREADS and not
 * high -- an exact quota is not what this gate promises.
 */
#define ST_THREADS 8
#define ST_ITERS   2000
#define ST_HIGH    6u
#define ST_LOW     3u

typedef struct {
    uint32_t peak;                 /* max live() this thread ever observed */
} stress_arg_t;

static void *stress_fn(void *arg)
{
    stress_arg_t *a = arg;

    for (int i = 0; i < ST_ITERS; i++) {
        nox_watermark_wait();
        nox_watermark_note_alloc();

        /* Sampled immediately after our own alloc: this thread's own
         * contribution is guaranteed to be included, so the max across threads
         * is a real lower bound on the true peak. */
        uint32_t lv = nox_watermark_live();
        if (lv > a->peak)
            a->peak = lv;

        /* Hold the page across a yield. Without it a thread's alloc/free pair
         * is so short that holds almost never overlap, the level never climbs
         * to `high`, and the stress case degenerates into a lock benchmark
         * that never touches the gate at all. */
        sched_yield();

        nox_watermark_note_free();
    }
    return NULL;
}

static void test_stress(void)
{
    nox_watermark_reset_for_test();
    nox_watermark_arm(ST_HIGH, ST_LOW);

    pthread_t t[ST_THREADS];
    stress_arg_t a[ST_THREADS];

    for (int i = 0; i < ST_THREADS; i++) {
        a[i].peak = 0;
        assert(pthread_create(&t[i], NULL, stress_fn, &a[i]) == 0);
    }
    for (int i = 0; i < ST_THREADS; i++)
        pthread_join(t[i], NULL);     /* reaching here == no deadlock */

    uint32_t peak = 0;
    for (int i = 0; i < ST_THREADS; i++)
        if (a[i].peak > peak)
            peak = a[i].peak;

    assert(nox_watermark_live() == 0);            /* balanced accounting */
    assert(peak <= ST_HIGH + ST_THREADS);         /* bounded overshoot */

    printf("  stress %dx%d iters        OK (peak live %u <= %u, %llu stalls, "
           "%llu ms blocked)\n",
           ST_THREADS, ST_ITERS, peak, ST_HIGH + ST_THREADS,
           (unsigned long long)nox_watermark_blocked_count(),
           (unsigned long long)(nox_watermark_blocked_ns() / 1000000ull));
}

int main(void)
{
    printf("watermark_test:\n");
    test_disarmed_never_blocks();
    test_armed_below_high();
    test_blocks_at_high_wakes_at_low();
    test_hysteresis_band();
    test_disarm_releases();
    test_multiple_waiters();
    test_stress();
    printf("watermark_test: PASS\n");
    return 0;
}
