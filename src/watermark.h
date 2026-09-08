/*
 * watermark.h - C4-B9 eviction watermark: high/low backpressure gate (spec 3.6).
 *
 * WHAT THIS IS
 * ------------
 * A single process-wide gate that stalls the FOREGROUND when the number of live
 * (RAM-resident, not-yet-reclaimed) scrap pages runs away from the flusher. It
 * is the answer to the RSS growth the C4 soak exposes: the OTflush queue is
 * unbounded by design (queue.h, divergence D5), so nothing else in the engine
 * puts a ceiling on resident pages. This module is that ceiling.
 *
 * HYSTERESIS, NOT A LEVEL TRIGGER
 * -------------------------------
 * Two marks, low < high. A thread only starts waiting at `high`, and once it is
 * waiting it sleeps all the way down to `low` -- it is deliberately NOT woken at
 * high-1. A single-mark gate degenerates into a wake/re-sleep storm around the
 * mark: every free wakes a waiter, that waiter allocates, the level crosses back
 * up, and the next free wakes it again. One condvar broadcast per allocation is
 * exactly the kind of foreground syscall traffic the C10 p99 numbers cannot
 * absorb. The (high - low) band amortises that: one broadcast releases the whole
 * waiting set, and they get a full band of headroom before anyone stalls again.
 *
 * THIS IS A FALLBACK, NOT THE NORMAL FLUSH PATH
 * ---------------------------------------------
 * Do NOT conflate this with the Queue-1 enqueue trigger. A partial page enters
 * Q1 at CREATION time (paper 3.4) -- flushing is driven by page lifecycle, not
 * by memory level, and in a healthy run this gate never fires at all. The
 * watermark only exists for the pathological case where the SSD cannot retire
 * pages as fast as the foreground mints them. If your workload trips this gate
 * routinely, the device is the bottleneck and the gate is reporting it (see
 * blocked_count/blocked_ns); it is not a tuning dial for steady state.
 *
 * ACCEPTED IMPRECISION: live CAN OVERSHOOT high
 * ---------------------------------------------
 * wait() and note_alloc() are NOT atomic with respect to each other: a thread
 * observes headroom, returns from wait(), and only then goes off and allocates a
 * page. Every foreground thread can slip through that window at once, so `live`
 * may legitimately exceed `high` by up to the number of concurrent foreground
 * threads. This is intentional and accepted. Closing it would mean holding the
 * watermark mutex across the whole page allocation (posix_memalign of a 256KB
 * data zone plus index setup), which serialises the entire foreground on one
 * lock -- trading a bounded, small RAM overshoot for a hard throughput ceiling.
 * The gate's job is to bound RAM to within a constant of `high`, not to enforce
 * an exact quota. Callers must size `high` with that slack in mind.
 *
 * MODULE-LEVEL STATE
 * ------------------
 * One instance per process (file-scope statics), no create/destroy: there is one
 * page pool, so a second gate would have nothing distinct to gate. Starts
 * DISARMED so that unit tests and benchmarks that never call arm() pay nothing
 * beyond a counter update. All state -- marks, level, stats -- is guarded by one
 * mutex; the condvar requires that mutex anyway, so atomics would buy nothing
 * and would only create a second, weaker view of the same variables.
 */
#ifndef WATERMARK_H
#define WATERMARK_H

#include <stdint.h>

/* Enable gating at these marks. Requires low < high (asserted). */
void nox_watermark_arm(uint32_t high, uint32_t low);

/* Permanent: wakes every waiter and makes all future wait() calls no-ops.
 * Used at shutdown, so a foreground thread parked on the gate can never
 * outlive the flusher that was supposed to release it. */
void nox_watermark_disarm(void);

/* Foreground admission point: call BEFORE allocating a page. Returns at once
 * while disarmed or below `high`; otherwise blocks until the level falls to
 * `low` (or the gate is disarmed). See the overshoot note above. */
void nox_watermark_wait(void);

/* Level accounting. note_alloc() never blocks -- the gate is wait()'s job, and
 * blocking here would stall a thread that has already committed to a page. */
void nox_watermark_note_alloc(void);
void nox_watermark_note_free(void);

uint32_t nox_watermark_live(void);

/* Backpressure telemetry: how often the foreground actually stalled, and for
 * how long in total across all threads (CLOCK_MONOTONIC, summed). A run with
 * blocked_count == 0 means the gate never bound. */
uint64_t nox_watermark_blocked_count(void);
uint64_t nox_watermark_blocked_ns(void);

/* Tests only: back to pristine disarmed state, live = 0, stats zeroed. */
void nox_watermark_reset_for_test(void);

#endif /* WATERMARK_H */
