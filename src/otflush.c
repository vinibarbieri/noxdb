/*
 * otflush.c - see otflush.h.
 */
#define _GNU_SOURCE
#include "otflush.h"
#include "queue.h"
#include "io_direct.h"
#include "noxdb_config.h"
#include "nox_stats.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

struct otflush {
    page_index_t *idx;
    int           fd;

    nox_queue_t  *q1;          /* partial pages awaiting Stage-1 hole fill */
    nox_queue_t  *q2;          /* assembled pages awaiting Stage-2 writeback */

    pthread_t     s1[NOX_STAGE1_THREADS];
    pthread_t     s2[NOX_STAGE2_THREADS];
    int           s1_n, s2_n;  /* how many actually started (for join) */

    /* Bcount: bytes of I/O currently in flight, per SSD. One SSD in the MVP, so
     * one counter (paper §3.4 keeps it per-SSD). */
    atomic_uint_least64_t bcount;

    /* `pending` counts pages enqueued but not yet terminally handled. Each push
     * is +1, each terminal handling (writeback+free, or a discard) is -1; a
     * Stage-1 forward is -1/+1 = net 0. otflush_drain waits for it to hit 0. */
    pthread_mutex_t  mtx;
    pthread_cond_t   idle;       /* signalled when pending reaches 0 */
    pthread_cond_t   not_busy;   /* signalled when bcount drops */
    uint64_t         pending;
    int              io_err;     /* sticky: first background I/O failure */
    int              stopping;

    /* --- per-base writeback ordering guard (see wb_guard_* below) --------- */
    pthread_cond_t   wb_done;    /* signalled when a base leaves the guard */
    uint32_t         wb[NOX_WB_GUARD_SLOTS];

    /* Latch for the queue-depth notice (warn_if_deep): once per engine, not
     * once per crossing. Atomic because the foreground calls it concurrently
     * from every writer thread with only p->lock held, and p->lock is a
     * PER-PAGE lock - two threads on different pages are not serialised
     * against each other here. */
    atomic_flag      deep_noticed;
};

/*
 * WRITEBACK ORDERING GUARD
 *
 * The hazard it closes (found by C2-GATE region 5, single-threaded):
 *
 *   A page whose entry array fills up is SEALED and DETACHED from the index,
 *   and the foreground immediately creates a fresh page P2 for the SAME base.
 *   Now two live pages cover one disk region. P1 is ahead of P2 in the FIFO, so
 *   Stage-2 writes P1 first and P2 last. But Stage-1 fills P2's holes by
 *   READING that same region — and nothing ordered that pread against P1's
 *   pwrite. If the read wins, P2's holes get the pre-P1 disk contents (zeros on
 *   a never-written region) and P2's writeback then erases every byte P1 held.
 *
 * Spec §4.2 saw the premise ("a fresh page for the same base can be created the
 * instant the old one is detached") but drew only the two-Stage-2-threads
 * conclusion from it. The Stage-1-read-versus-Stage-2-write ordering was
 * missed, and it fires with ONE user thread.
 *
 * Invariant enforced: Stage-1 never preads a base that has a page pending
 * writeback. Since Q1 is FIFO and Stage-1 is single-threaded, pages for a base
 * are hole-filled in creation order, so this is sufficient.
 *
 * Representation: a counting array indexed by a hash of the base, NOT an exact
 * set. A collision makes Stage-1 wait for an unrelated base — harmless and
 * transient. What matters is that there are no false NEGATIVES: a base with a
 * pending writeback always reads as busy. This buys the ordering guarantee with
 * a fixed 32 KB and no allocation on the I/O path.
 */
static inline uint32_t wb_slot(uint64_t base)
{
    uint64_t page_no = base / NOX_DATAZONE_SIZE;
    return (uint32_t)((page_no * 0x9E3779B97F4A7C15ull) >> (64u - NOX_WB_GUARD_BITS));
}

/* Called when a page enters Q2 (i.e. acquires a pending writeback). */
static void wb_guard_enter(otflush_t *o, uint64_t base)
{
    pthread_mutex_lock(&o->mtx);
    o->wb[wb_slot(base)]++;
    pthread_mutex_unlock(&o->mtx);
}

/* Called after that page's writeback has completed. */
static void wb_guard_leave(otflush_t *o, uint64_t base)
{
    pthread_mutex_lock(&o->mtx);
    if (o->wb[wb_slot(base)] > 0)
        o->wb[wb_slot(base)]--;
    pthread_cond_broadcast(&o->wb_done);
    pthread_mutex_unlock(&o->mtx);
}

/*
 * Block until no page for `base` is pending writeback. Bounded timedwait rather
 * than a bare wait so a lost wakeup degrades into a small delay instead of a
 * hang; `stopping` does not short-circuit it, because shutdown DRAINS (a page
 * skipped here would be the one that corrupts the file).
 *
 * Deadlock-free by construction: Stage-1 waits on Stage-2, and Stage-2 never
 * waits on Stage-1. The page Stage-1 holds is out of both queues, so it cannot
 * be waiting on itself.
 */
static void wb_guard_wait(otflush_t *o, uint64_t base)
{
    uint32_t s = wb_slot(base);

    pthread_mutex_lock(&o->mtx);
    while (o->wb[s] > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000L;                /* 1 ms backstop */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&o->wb_done, &o->mtx, &ts);
    }
    pthread_mutex_unlock(&o->mtx);
}

/* --- Bcount ------------------------------------------------------------- */

static int ssd_is_busy(otflush_t *o)
{
    return atomic_load(&o->bcount) >= NOX_BCOUNT_BUSY_THRESHOLD;
}

static void bcount_add(otflush_t *o, uint64_t bytes)
{
    atomic_fetch_add(&o->bcount, bytes);
}

static void bcount_sub(otflush_t *o, uint64_t bytes)
{
    atomic_fetch_sub(&o->bcount, bytes);
    /* Wake anyone parked in wait_not_busy(). Cheap: this fires once per I/O. */
    pthread_mutex_lock(&o->mtx);
    pthread_cond_broadcast(&o->not_busy);
    pthread_mutex_unlock(&o->mtx);
}

/*
 * Algorithm 2 re-inserts a page at the queue tail when the SSD is busy and
 * loops. With ONE thread per queue and blocking I/O, that loop has nothing else
 * to run and becomes a hot spin burning a core (spec §6, divergence D3). So:
 * park on a condvar that an I/O completion signals, with a bounded timeout as a
 * backstop in case the last completion beat us to the wait.
 */
static void wait_not_busy(otflush_t *o)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 1000000L;                    /* 1 ms backstop */
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&o->mtx);
    if (!o->stopping)
        pthread_cond_timedwait(&o->not_busy, &o->mtx, &ts);
    pthread_mutex_unlock(&o->mtx);
}

/* --- pending accounting ------------------------------------------------- */

static void pending_inc(otflush_t *o)
{
    pthread_mutex_lock(&o->mtx);
    o->pending++;
    pthread_mutex_unlock(&o->mtx);
}

static void pending_dec(otflush_t *o)
{
    pthread_mutex_lock(&o->mtx);
    if (--o->pending == 0)
        pthread_cond_broadcast(&o->idle);
    pthread_mutex_unlock(&o->mtx);
}

static void latch_io_error(otflush_t *o)
{
    pthread_mutex_lock(&o->mtx);
    o->io_err = -1;      /* sticky: surfaced by drain/stop, i.e. by nox_close */
    pthread_mutex_unlock(&o->mtx);
}

/* --- enqueue (foreground; caller holds p->lock) ------------------------- */

/* Queue-depth notice. NOT a warning any more, and it fires ONCE PER ENGINE.
 *
 * Both properties changed when C4-B9 landed. The old text said "RAM is
 * unbounded until the C5 eviction watermark lands"; the watermark has landed
 * and arms in nox_open, so that sentence now describes a world the binary no
 * longer runs in. And the old `== NOX_QUEUE_WARN_DEPTH` test fired once per
 * CROSSING, which was reasonable while depth grew monotonically to OOM -- but
 * the gate makes depth oscillate around exactly this mark by design, so a
 * 120s/16-thread soak printed it 25 times. An alarm that goes off during
 * correct operation is an alarm its operator learns to skip.
 *
 * What survives is genuinely worth saying once: crossing this depth means the
 * foreground is outrunning Stage-1/Stage-2, so from here on writers WILL be
 * throttled and the latency tail is the gate, not the device. */
static void warn_if_deep(otflush_t *o, nox_queue_t *q, const char *name)
{
    size_t d = nox_queue_depth(q);
    if (d < NOX_QUEUE_WARN_DEPTH)
        return;

    /* test-and-set returns the PREVIOUS value: the first caller through gets 0
     * and prints, every later one gets 1 and returns. */
    if (atomic_flag_test_and_set(&o->deep_noticed))
        return;

    /* Whether the gate can actually bind is a BUILD question, so the notice has
     * to ask it rather than assume. NOX_WATERMARK_HIGH is overridable, and the
     * before/after measurement builds set it past any reachable depth precisely
     * to disarm the gate (see noxdb_config.h). On that build the old wording -
     * "the watermark is now throttling writers at 1000000000 live pages" - was
     * exactly the class of lie this function was just rewritten to remove.
     *
     * Comparing against the depth we just crossed is the right test: the queue
     * holds live pages, so a mark at or below this depth is a mark that binds. */
    if ((size_t)NOX_WATERMARK_HIGH <= d)
        fprintf(stderr,
                "noxdb: NOTE: OTflush %s reached %zu pages (~%zu MB of scrap "
                "RAM). The foreground is outrunning the flush threads, so the "
                "C4-B9 watermark is throttling writers at %u live pages; expect "
                "foreground stalls in the latency tail. Once per engine.\n",
                name, d, (d * (size_t)NOX_DATAZONE_SIZE) >> 20,
                NOX_WATERMARK_HIGH);
    else
        fprintf(stderr,
                "noxdb: WARNING: OTflush %s reached %zu pages (~%zu MB of scrap "
                "RAM) and NOTHING IS BOUNDING IT: the C4-B9 watermark is set to "
                "%u live pages, which this workload will not reach. RAM will grow "
                "until the OOM killer intervenes. Once per engine.\n",
                name, d, (d * (size_t)NOX_DATAZONE_SIZE) >> 20,
                NOX_WATERMARK_HIGH);
}

void otflush_enqueue_partial(otflush_t *o, scrap_page_t *p)
{
    if (p->in_queue)
        return;                       /* single membership (spec §4.4) */
    p->in_queue = 1;
    pending_inc(o);
    nox_queue_push(o->q1, p);
    warn_if_deep(o, o->q1, "Queue-1");
}

void otflush_enqueue_full(otflush_t *o, scrap_page_t *p)
{
    p->hdr.tag = NOX_TAG_FULL;
    if (p->in_queue)
        return;   /* already in Q1: Stage-1 will see it is full and forward it
                   * to Q2 with NO pread. Pushing here too would put the page in
                   * both queues -> use-after-free (spec §4.4). */
    p->in_queue = 1;
    pending_inc(o);
    wb_guard_enter(o, p->base);   /* pending writeback starts on entry to Q2 */
    nox_queue_push(o->q2, p);
    warn_if_deep(o, o->q2, "Queue-2");
}

/* --- Stage-1: fill holes ------------------------------------------------ */

static void *stage1_loop(void *arg)
{
    otflush_t *o = arg;
    scrap_page_t *p;

    /* One 4096-aligned scratch zone for the whole thread lifetime: it is an
     * O_DIRECT pread target (CLAUDE.md §2), and allocating it per page would
     * put a 256KB posix_memalign on the hot path. */
    void *scratch = NULL;
    if (posix_memalign(&scratch, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) != 0) {
        fprintf(stderr, "noxdb: Stage-1 scratch allocation failed\n");
        latch_io_error(o);
        return NULL;
    }

    while ((p = nox_queue_pop(o->q1)) != NULL) {
        /* REPRO HOOK -- measurement artifact only, never a real build.
         *
         * bench/order_repro.c needs SEVERAL live generations of one base to
         * exist at the same instant, which only happens while Stage-1 is behind
         * the foreground. On an idle engine Stage-1 drains a page the moment it
         * is pushed and the window never opens, so the experiment would report a
         * PASS that means "I never created the condition" rather than "the
         * ordering held". Sleeping here holds each popped page out of both
         * queues for a known interval and lets the foreground stack generations
         * behind it.
         *
         * Same containment as NOX_EAGER_ZERO (see scrap_page_alloc): defined
         * only by the repro targets, which compile $(SRC) in one shot so the -O2
         * objects a gate links can never carry it. Inert -- not merely cheap --
         * in every other build: the #ifdef removes the code entirely. */
#ifdef NOX_REPRO_STALL_STAGE1_MS
        {
            struct timespec st = {
                .tv_sec  =  (time_t)(NOX_REPRO_STALL_STAGE1_MS) / 1000,
                .tv_nsec = ((long)(NOX_REPRO_STALL_STAGE1_MS) % 1000) * 1000000L
            };
            nanosleep(&st, NULL);
        }
#endif
        pthread_mutex_lock(&p->lock);

        /* Alg. 2 line 4-5 discards a full page because the foreground already
         * queued it to Q2. We FORWARD instead (spec §3, D6): under single
         * membership it is NOT in Q2, and forwarding still skips the read. */
        int full = scrap_page_is_full(p);
        if (full) {
            uint64_t fb = p->base;
            pthread_mutex_unlock(&p->lock);
            wb_guard_enter(o, fb);         /* pending writeback starts now */
            nox_queue_push(o->q2, p);      /* stays in_queue; pending unchanged */
            continue;
        }

        /* Alg. 2 line 6: only read when the SSD is not busy. */
        if (ssd_is_busy(o)) {
            pthread_mutex_unlock(&p->lock);
            nox_queue_push(o->q1, p);      /* Alg. 2 lines 9-10: back to tail */
            wait_not_busy(o);              /* ...but sleep instead of spinning */
            continue;
        }

        /* Snapshot the hole list under the lock, then RELEASE it: the pread
         * below must not run with p->lock held. page_index_get_or_create takes
         * p->lock while still holding the index shard mutex, so a foreground
         * thread blocking here would block on the SSD *and* stall every other
         * page in that shard. Measured cost of getting this wrong: p99 240 us
         * on C4-GATE, against a 200 us budget. */
        scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
        uint32_t nh = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
        uint64_t base = p->base;           /* immutable after alloc */
        /* Sampled here, under the lock, BEFORE apply_holes collapses the entry
         * array to a single segment. This is the instant the page stops being
         * something the foreground fills and becomes something the background
         * assembles — one sample per page, and the population is exactly the
         * partial pages the header size governs. */
        nox_stat_entries_at_stage1(p->hdr.number);
        pthread_mutex_unlock(&p->lock);

        /* ORDERING: never read a region that still has a page pending
         * writeback, or we would fill this page's holes with contents the
         * older page is about to overwrite. See wb_guard_* above. */
        wb_guard_wait(o, base);

        uint64_t rbytes = 0;
        bcount_add(o, NOX_DATAZONE_SIZE);
        int rc = scrap_page_read_holes(base, o->fd, holes, nh, scratch, &rbytes);
        bcount_sub(o, NOX_DATAZONE_SIZE);
        nox_stat_disk_read(rbytes);

        pthread_mutex_lock(&p->lock);
        if (rc != 0) {
            latch_io_error(o);
            p->in_queue = 0;
            pthread_mutex_unlock(&p->lock);
            pending_dec(o);                /* dropped: cannot safely write it */
            continue;
        }

        /* Re-derives the holes under the lock, so a foreground merge that
         * landed during the unlocked read wins over the disk bytes. Also
         * collapses the entry array, which is what makes scrap_page_is_full()
         * true here instead of needing the tag to say so. */
        scrap_page_apply_holes(p, scratch);
        p->hdr.tag = NOX_TAG_FULL;         /* holes filled: safe to write whole */
        pthread_mutex_unlock(&p->lock);

        wb_guard_enter(o, base);           /* pending writeback starts now */
        nox_queue_push(o->q2, p);          /* Alg. 2 line 8 */
    }
    free(scratch);
    return NULL;
}

/* --- Stage-2: write back ------------------------------------------------ */

/*
 * Take ownership of a page: mark it FLUSHING and unlink it from the index, so
 * no foreground writer can find it while it is in flight. Returns 0 on success.
 *
 * LOCK ORDER: shard lock (inside page_index_detach_if) is taken with p->lock
 * NOT held, then p->lock afterwards — the same shard -> page order used
 * everywhere else. NEITHER lock is held across the I/O that follows.
 */
static void stage2_detach(otflush_t *o, scrap_page_t *p)
{
    if (!p->detached) {
        page_index_detach_if(o->idx, p->base, p);
        p->detached = 1;
    }
    pthread_mutex_lock(&p->lock);
    p->hdr.tag  = NOX_TAG_FLUSHING;
    p->in_queue = 0;
    pthread_mutex_unlock(&p->lock);
}

/* Collect up to NOX_PWRITEV_MAX_IOV pages with contiguous bases, starting at
 * `first`. Returns how many pages are in `batch` (always >= 1). */
static uint32_t stage2_collect(otflush_t *o, scrap_page_t *first,
                               scrap_page_t **batch)
{
    uint32_t n = 0;
    batch[n++] = first;

    while (n < NOX_PWRITEV_MAX_IOV) {
        uint64_t want = first->base + (uint64_t)n * NOX_DATAZONE_SIZE;
        /* pop_if_base is atomic under the queue lock: no peek-then-pop race. */
        scrap_page_t *nx = nox_queue_pop_if_base(o->q2, want);
        if (!nx)
            break;
        /* Read the header under nx->lock. The page is still in the index at
         * this point (stage2_detach has not run yet), so a foreground merge can
         * be rewriting hdr concurrently — reading it unlocked is a real data
         * race, reported by TSan at scrap_page.c:57 / otflush.c:240.
         * No other lock is held here (pop_if_base released the queue lock), so
         * taking a page lock cannot invert the shard -> page -> queue order. */
        pthread_mutex_lock(&nx->lock);
        int ready = scrap_page_is_full(nx) || nx->hdr.tag == NOX_TAG_FULL;
        pthread_mutex_unlock(&nx->lock);

        /* A page that somehow arrived unfilled goes back: only Stage-1 may fill
         * holes, and writing it here would put zeros over live data. It cannot
         * happen by construction (every Q2 entry passed through Stage-1 or was
         * born full) — this is a guard, not a routine branch. */
        if (!ready) {
            nox_queue_push(o->q2, nx);
            break;
        }
        batch[n++] = nx;
    }
    return n;
}

static void *stage2_loop(void *arg)
{
    otflush_t *o = arg;
    scrap_page_t *p;
    scrap_page_t *batch[NOX_PWRITEV_MAX_IOV];
    struct iovec  iov[NOX_PWRITEV_MAX_IOV];

    /* Per-thread PRNG state for the repro jitter below. Seeded from the thread's
     * own stack address so the Stage-2 threads do not all draw the same
     * sequence, which would re-serialise exactly what the jitter exists to
     * scramble. Unused (and untouched) in a normal build. */
    unsigned repro_seed = (unsigned)(uintptr_t)&p;
    (void)repro_seed;

    while ((p = nox_queue_pop(o->q2)) != NULL) {
        /* REPRO HOOK -- measurement artifact only, never a real build. See the
         * matching hook in stage1_loop for the containment argument.
         *
         * WHY THIS ONE EXISTS: the Stage-1 hook was the wrong amplifier for the
         * Stage-2 reordering hazard, and the 2026-08-20 run proved it. Stalling
         * Stage-1 throttles the PRODUCER, so Q2 receives one page at a time and
         * the Stage-2 threads never hold two pages of one base concurrently --
         * the window the experiment needs was closed by the very hook meant to
         * open it (predicted FAIL, measured PASS, exit 0).
         *
         * Stalling the CONSUMER is the correct amplifier: pages pile up in Q2,
         * several generations of one base become claimable at once, and the
         * jitter decides which thread reaches its pwrite first. RANDOM, not
         * fixed: a uniform sleep delays all four threads equally and preserves
         * their pop order, which is the ordering under test. */
#ifdef NOX_REPRO_STALL_STAGE2_MS
        {
            long ms = (long)(rand_r(&repro_seed) % ((NOX_REPRO_STALL_STAGE2_MS) + 1));
            struct timespec st = { .tv_sec  = ms / 1000,
                                   .tv_nsec = (ms % 1000) * 1000000L };
            nanosleep(&st, NULL);
        }
#endif
        /* Alg. 2 line 20: defer to a later slot if the SSD is saturated. */
        if (ssd_is_busy(o)) {
            nox_queue_push(o->q2, p);      /* Alg. 2 lines 23-24 */
            wait_not_busy(o);
            continue;
        }

        /* Alg. 2 lines 16-19 (SSD-id == 0 -> delayed allocation, pick the least
         * busy device) has no analogue here: NoxDB has one SSD and the caller
         * owns the offset, so p->base is known at allocation (spec §3, D2). */

        uint32_t n = stage2_collect(o, p, batch);
        /* Sampled here, once per writeback, BEFORE the branch below splits the
         * n == 1 and n > 1 cases -- otherwise the single-page path (which is the
         * one we suspect dominates) would have to be counted separately and the
         * two could drift apart. Background thread, so this costs the foreground
         * nothing. */
        nox_stat_stage2_batch(n);
        for (uint32_t i = 0; i < n; i++)
            stage2_detach(o, batch[i]);

        int rc;
        if (n == 1) {
            bcount_add(o, NOX_DATAZONE_SIZE);
            rc = scrap_page_writeback(batch[0], o->fd);
            bcount_sub(o, NOX_DATAZONE_SIZE);
            nox_stat_disk_write(NOX_DATAZONE_SIZE);
        } else {
            /* Scatter-gather: n data zones in RAM -> ONE syscall covering a
             * contiguous n*256KB disk region (docs/00_flow_summary.md:87).
             * pwritev, never writev+lseek: background threads share the fd
             * (docs/02 §2). */
            size_t total = 0;
            for (uint32_t i = 0; i < n; i++) {
                iov[i].iov_base = batch[i]->data;
                iov[i].iov_len  = NOX_DATAZONE_SIZE;
                total += NOX_DATAZONE_SIZE;
            }
            /* Bcount brackets the WHOLE batch, retries included: it measures
             * bytes this thread has outstanding, and a resumed write is still
             * the same outstanding work. Note 8 x 256KB = 2MB is the largest
             * single request the 1+1 pool can produce (spec D1). */
            bcount_add(o, total);
            /* _all: resumes a short write at iovec granularity instead of
             * reporting it as a lost 2MB. EINVAL is reported inside
             * io_direct.c, keeping every alignment diagnostic in one place
             * (plan constraint above, docs/02 §1). */
            ssize_t w = io_direct_pwritev_all(o->fd, iov, (int)n,
                                              (off_t)batch[0]->base);
            bcount_sub(o, total);
            nox_stat_disk_write(total);
            rc = (w == (ssize_t)total) ? 0 : -1;
        }

        if (rc != 0)
            latch_io_error(o);

        for (uint32_t i = 0; i < n; i++) {
            uint64_t done_base = batch[i]->base;
            scrap_page_free(batch[i]);     /* Alg. 2 lines 19/22: reclaim */
            /* Release the ordering guard only AFTER the writeback landed and
             * the page is gone: Stage-1 may now safely read this region. */
            wb_guard_leave(o, done_base);
            pending_dec(o);
        }
    }
    return NULL;
}

/* --- lifecycle ---------------------------------------------------------- */

otflush_t *otflush_start(page_index_t *idx, int fd)
{
    otflush_t *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->idx = idx;
    o->fd  = fd;
    atomic_init(&o->bcount, 0);
    /* Explicit clear, not the calloc: C11 does not promise an all-zero object
     * is a cleared atomic_flag, only ATOMIC_FLAG_INIT or this call does. */
    atomic_flag_clear(&o->deep_noticed);

    if (pthread_mutex_init(&o->mtx, NULL) != 0)   { free(o); return NULL; }
    if (pthread_cond_init(&o->idle, NULL) != 0)   { goto fail_mtx; }
    if (pthread_cond_init(&o->not_busy, NULL) != 0) { goto fail_idle; }
    if (pthread_cond_init(&o->wb_done, NULL) != 0) { goto fail_not_busy; }

    o->q1 = nox_queue_create();
    o->q2 = nox_queue_create();
    if (!o->q1 || !o->q2)
        goto fail_q;

    for (int i = 0; i < (int)NOX_STAGE1_THREADS; i++) {
        if (pthread_create(&o->s1[i], NULL, stage1_loop, o) != 0)
            goto fail_threads;
        o->s1_n++;
    }
    for (int i = 0; i < (int)NOX_STAGE2_THREADS; i++) {
        if (pthread_create(&o->s2[i], NULL, stage2_loop, o) != 0)
            goto fail_threads;
        o->s2_n++;
    }
    return o;

fail_threads:
    nox_queue_shutdown(o->q1);
    nox_queue_shutdown(o->q2);
    for (int i = 0; i < o->s1_n; i++) pthread_join(o->s1[i], NULL);
    for (int i = 0; i < o->s2_n; i++) pthread_join(o->s2[i], NULL);
fail_q:
    nox_queue_destroy(o->q1);
    nox_queue_destroy(o->q2);
    pthread_cond_destroy(&o->wb_done);
fail_not_busy:
    pthread_cond_destroy(&o->not_busy);
fail_idle:
    pthread_cond_destroy(&o->idle);
fail_mtx:
    pthread_mutex_destroy(&o->mtx);
    free(o);
    return NULL;
}

int otflush_drain(otflush_t *o)
{
    pthread_mutex_lock(&o->mtx);
    while (o->pending > 0)
        pthread_cond_wait(&o->idle, &o->mtx);
    int err = o->io_err;
    pthread_mutex_unlock(&o->mtx);
    return err;
}

int otflush_stop(otflush_t *o)
{
    if (!o)
        return 0;

    int err = otflush_drain(o);

    pthread_mutex_lock(&o->mtx);
    o->stopping = 1;
    pthread_cond_broadcast(&o->not_busy);   /* release any busy-waiter */
    pthread_mutex_unlock(&o->mtx);

    /* Shut Q1 first: a Stage-1 thread can still push into Q2, so Q2 must stay
     * open until every Stage-1 thread has exited. */
    nox_queue_shutdown(o->q1);
    for (int i = 0; i < o->s1_n; i++) pthread_join(o->s1[i], NULL);
    nox_queue_shutdown(o->q2);
    for (int i = 0; i < o->s2_n; i++) pthread_join(o->s2[i], NULL);

    pthread_mutex_lock(&o->mtx);
    if (o->io_err) err = -1;
    pthread_mutex_unlock(&o->mtx);

    nox_queue_destroy(o->q1);
    nox_queue_destroy(o->q2);
    pthread_cond_destroy(&o->wb_done);
    pthread_cond_destroy(&o->not_busy);
    pthread_cond_destroy(&o->idle);
    pthread_mutex_destroy(&o->mtx);
    free(o);
    return err;
}
