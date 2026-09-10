/*
 * otflush_soak.c - C4 soak gate (C4-B6 addendum / C4-G7).
 *
 * `bench/otflush_test.c` (make gate-c4) proves integrity + no-stall on
 * DISJOINT 256KB bases, matching the plan's Task 6 text and the C3 gate's
 * scope note ("no page base is shared between threads").
 *
 * This file is the case that scope note calls out as NOT covered: many
 * threads hammering the SAME 256KB base (same page_index bucket entry, same
 * scrap_page_t, same per-page lock, same OTflush queue membership) for a
 * SUSTAINED period (>=20 minutes by default), while sampling:
 *   1. INTEGRITY  - one memcmp against a RAM shadow after the run drains.
 *   2. RSS        - a time series read from /proc/self/status, checked for a
 *                   BOUNDED plateau (ceiling + plateau sub-tests, see criterion
 *                   2 below), not for flatness.
 *   3. LATENCY    - the same foreground-never-stalls check as gate-c4, run
 *                   under sustained overlapping-base contention instead of a
 *                   short disjoint-base burst.
 *
 * WHY THIS IS EXPECTED TO SURFACE KNOWN, DOCUMENTED GAPS (read before filing
 * a bug against OTflush over a soak FAIL):
 *
 *   - RSS growth -> WAS the C4-B9 gap; B9 (spec 3.6, the eviction watermark,
 *     src/watermark.c) IS NOW BUILT and arms in nox_open. The foreground is
 *     gated at NOX_WATERMARK_HIGH live pages, so RSS ramps to a plateau
 *     instead of running to OOM. Before B9 this driver at 16 threads peaked at
 *     5.6GB in 3 seconds and the 120s run was OOM-killed; after B9 the same
 *     3s run peaks at ~158MB. CRITERION 2 NO LONGER HAS A DOCUMENTED EXCUSE -
 *     a FAIL there is a defect to chase, not a known gap to wave through.
 *
 *   - Integrity mismatches on OVERLAPPING bases -> C4-B8 (the tag=FLUSHING
 *     pointer-swap protocol) is also NOT built in C4. src/page_index.h says
 *     so explicitly: "two threads writing the SAME 256KB base concurrently
 *     can still LOSE AN UPDATE... That is a correctness limit on write
 *     ordering... The C5 tag=FLUSHING pointer swap is the general fix."
 *     Lock coupling (Task 3/B3) already closed the USE-AFTER-FREE that used
 *     to exist here, so this driver is memory-safe to run - but a lost
 *     update is still possible: a page can be detached+flushed while another
 *     writer is mid-merge into a page that already lost its index slot, and
 *     its bytes end up in a page that is enqueued and written back AFTER the
 *     one carrying a more recent write to the same location. To make any
 *     mismatch UNAMBIGUOUS instead of "which concurrent write wins is
 *     inherently undefined," each thread owns a disjoint BYTE STRIPE inside
 *     the shared 256KB page (see stripe assignment below): the contention is
 *     real (same page, same lock, same queue slot) but no two threads ever
 *     race on the same byte, so a mismatch can ONLY mean the engine dropped
 *     or reordered a write across a page-recycle boundary - i.e. exactly the
 *     B8 gap, not an inherent concurrent-write race.
 *
 * This is precisely the 17-Aug decision gate's question 1 ("does memcmp
 * integrity pass?") and question 2 ("RAM stable?") - run
 * for real and report the true PASS/FAIL, do not paper over either check.
 *
 * Usage (path required; duration/threads optional, argv wins over env, then
 * a 20min/16-thread default):
 *   ./bench/otflush_soak /mnt/nvme/c4soak.dat [seconds] [nthreads]
 *   NOX_SOAK_SECONDS=60 NOX_SOAK_THREADS=4 ./bench/otflush_soak /mnt/nvme/c4soak.dat
 *
 * Smoke run (seconds/threads made small on purpose):
 *   make soak-c4 SOAK_SECONDS=30 SOAK_THREADS=4
 *
 * Build + run ON THE BENCH BOX against /mnt/nvme:
 *   make soak-c4
 *   ./bench/otflush_soak /mnt/nvme/c4soak.dat 1200 16
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REG NOX_DATAZONE_SIZE   /* 256KB region == one scrap page */

/* Far fewer shared bases than threads ON PURPOSE: this is what forces
 * "overlapping," i.e. multiple threads resolving to the SAME page_index
 * bucket entry / scrap_page_t / per-page lock / OTflush queue slot, which is
 * exactly the case the C3 and C4 (gate-c4) drivers partition away from.
 *
 * 4 is the soak's real setting and the board's case; do not change it here.
 * Override it only to vary the OVERLAP RATIO, via `make soak-c4 SOAK_BASES=<n>`,
 * which rebuilds through the same stamp NOX_ENTRIES uses so a stale object can
 * never be relinked under a new value.
 *
 * CEILING: a thread is pinned to one base for the whole run, so the bases
 * actually used is min(nthreads, SHARED_BASES). Raising this ABOVE nthreads
 * changes nothing at all — measured the hard way: 4 threads at SHARED_BASES 4
 * and at 4096 produced a bit-for-bit identical write path, and the three runs'
 * drain times (224s / 260s / 403s) were three samples of one configuration.
 * To change the overlap, move nthreads relative to this, not this alone. */
#ifndef SHARED_BASES
#define SHARED_BASES        4u
#endif

/* Max size of one chaotic write; each write is 1 + rand()%SOAK_MAXLEN, so the
 * MEAN is about half this. That mean is what decides write amplification, and
 * it is the whole point of making this overridable:
 *
 *   amplification = NOX_DATAZONE_SIZE / (NOX_MAX_ENTRIES * mean_write)
 *
 * A page is sealed by whichever runs out first, its data zone or its index. The
 * crossover is NOX_DATAZONE_SIZE / NOX_MAX_ENTRIES = 262144/64 = 4096 B exactly
 * at the current header size. Below that mean the index exhausts first and a
 * mostly-empty 256KB page goes to disk; above it the page genuinely fills.
 *
 * The 512 default puts the mean at ~256 B, 16x BELOW the crossover, which is
 * why this driver reports ~13x amplification. That is a property of the
 * benchmark, not of the engine, and the override is how you show it:
 *      make soak-c4 SOAK_MAXLEN=8192      # mean ~4KB, at the crossover
 */
#ifndef SOAK_MAXLEN
#define SOAK_MAXLEN          512u    /* max size of one chaotic write */
#endif
#define STALL_NS             200000L /* same bound as gate-c4: RAM-copy scale */
#define DEFAULT_SECONDS      1200u   /* 20 minutes */
#define DEFAULT_THREADS      16u

/* Per-thread latency reservoir cap. Bounded ON PURPOSE: if we appended every
 * sample for a 20+ minute run, OUR OWN bookkeeping would grow RSS, and that
 * growth would contaminate the exact signal this driver exists to isolate
 * (the engine's missing backpressure, C4-B9). Reservoir sampling (Algorithm
 * R) keeps a statistically representative subset in fixed memory regardless
 * of how many writes actually happen. Exact max and exact stall count are
 * tracked OUTSIDE the reservoir (below) so the reservoir's approximation
 * never hides a true worst case. */
#define LAT_RESERVOIR_CAP    200000u

/* How often the RSS monitor thread samples /proc/self/status, in seconds.
 *
 * Overridable because 5s is a sampling rate for a 20-minute run, not for a
 * chart. Drawing the pre-watermark RSS curve needs 1s: ungated, this driver
 * goes from 2.6 MB to OOM in under 6 seconds at 16 threads, which at 5s
 * resolution is two points and a dead process.
 *      make soak-c4 RSS_INTERVAL=1
 * The sample array is sized from seconds/interval, so it follows automatically. */
#ifndef RSS_SAMPLE_INTERVAL_S
#define RSS_SAMPLE_INTERVAL_S 5u
#endif

/* Headroom added on top of the computed page + driver footprint before the
 * ceiling sub-test calls a run unbounded. Covers what this driver cannot
 * enumerate: glibc arena fragmentation on a 256KB-chunk workload, OTflush
 * queue nodes, the page_index shard tables, thread stacks, libc itself.
 *
 * 64MB is deliberately generous. The failure this sub-test must catch is
 * UNBOUNDED growth, which overruns any ceiling by orders of magnitude - the
 * pre-B9 run of this very driver peaked at 5.6GB against a ~1.1GB ceiling.
 * A tight bound here would buy nothing and would turn allocator noise into a
 * red gate. The PLATEAU sub-test below is what catches the small, slow leak
 * that a loose ceiling would sail past. */
#define RSS_CEILING_SLACK_KB  (64u * 1024u)

/* Plateau sub-test: how much the late-run RSS peak may exceed the mid-run
 * peak before it counts as still-growing. Same 64MB smoke scale, but applied
 * to a DIFFERENCE BETWEEN TWO WINDOWS rather than to an absolute level, which
 * is what makes it scale-free: it asks "did RSS stop climbing", a question
 * whose answer does not depend on how big the watermark ceiling happens to be.
 *
 * Sensitivity at the 1200s C4-G7 default: each window is ~450s, so this trips
 * on a sustained leak of ~142 kB/s - roughly one 256KB scrap page every 1.8s
 * against a creation rate of ~2650 pages/s. Small leaks below that rate need a
 * longer run to surface, which is precisely why C4-G7 is 20 minutes and not 3
 * seconds. */
#define RSS_PLATEAU_SLACK_KB  (64u * 1024u)

/* Minimum samples for the plateau sub-test: 25% warm-up plus two comparison
 * windows, each needing >= 3 samples to have a meaningful peak. Below this the
 * sub-test is SKIPPED, not failed - a smoke run is too short to have a plateau,
 * and the ceiling sub-test still judges it. */
#define RSS_PLATEAU_MIN_SAMPLES 8u

static nox_engine_t *g_engine;

/* One writer (main, once, at the deadline), many readers (every worker + the
 * RSS monitor), no mutex on the hot path. `volatile` alone does not give the
 * needed cross-thread visibility under the C11 memory model (TLPI/OSTEP: it
 * only stops the COMPILER reordering, says nothing about other cores) -
 * stdatomic.h is the correct primitive, matching this codebase's own
 * convention for exactly this pattern (see otflush.c's atomic bcount). */
static atomic_int g_stop = 0;   /* deadline reached; workers observe and exit */

/* --- RSS sampling --------------------------------------------------------
 * Linux-only API (spec: /proc/self/status VmRSS). Guarded so this file still
 * COMPILES (type-checks, links) on macOS, where it degrades to "unavailable"
 * instead of breaking the local verification build. */
#if defined(__linux__)
static long read_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    long kb = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1)
            break;
    }
    fclose(f);
    return kb;
}
#else
static long read_rss_kb(void)
{
    return -1;   /* RSS sampling unavailable on this platform */
}
#endif

typedef struct {
    uint64_t t_s;    /* seconds since soak start */
    long     kb;     /* VmRSS at that instant, or -1 if unavailable */
} rss_sample_t;

typedef struct {
    uint64_t       deadline_ns;
    rss_sample_t  *samples;
    size_t         cap;
    size_t         n;
    pthread_mutex_t mtx;   /* guards n (monitor thread writes, main reads at end) */
} rss_monitor_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Take one RSS reading and append it to the series. Factored out of the loop
 * below so the mandatory deadline sample goes through EXACTLY the same path as
 * the periodic ones - no second copy to drift out of sync. */
static void rss_take_sample(rss_monitor_t *m, uint64_t t0, int *warned_unavailable)
{
    long kb = read_rss_kb();
    uint64_t elapsed_s = (now_ns() - t0) / 1000000000ull;

    if (kb < 0 && !*warned_unavailable) {
        fprintf(stderr,
                "note: RSS sampling unavailable on this platform "
                "(no /proc/self/status) - RSS column will read -1 "
                "throughout. This is expected off Linux; run on the "
                "bench box for a real reading.\n");
        *warned_unavailable = 1;
    }

    pthread_mutex_lock(&m->mtx);
    if (m->n < m->cap) {
        m->samples[m->n].t_s = elapsed_s;
        m->samples[m->n].kb  = kb;
        m->n++;
    }
    pthread_mutex_unlock(&m->mtx);

    printf("[rss] t=%6llus  VmRSS=%6ld kB\n",
           (unsigned long long)elapsed_s, kb);
    fflush(stdout);
}

static void *rss_monitor_loop(void *arg)
{
    rss_monitor_t *m = arg;
    uint64_t t0 = now_ns();
    int warned_unavailable = 0;

    while (!atomic_load(&g_stop)) {
        rss_take_sample(m, t0, &warned_unavailable);

        /* Sleep in short slices so a short smoke run (SOAK_SECONDS small)
         * doesn't overshoot its deadline waiting on one long sleep. */
        for (unsigned s = 0; s < RSS_SAMPLE_INTERVAL_S && !atomic_load(&g_stop); s++)
            sleep(1);
    }

    /* MANDATORY FINAL SAMPLE, taken at the write deadline.
     *
     * Without it a run shorter than RSS_SAMPLE_INTERVAL_S produced exactly ONE
     * sample - the one at t=0, before a single write - and criterion 2 then
     * compared that reading against itself, got a growth of 0 by construction,
     * and printed PASS. Measured the hard way: `soak-c4 SOAK_SECONDS=3
     * SOAK_THREADS=16` reported "RSS FLAT: PASS / growth=0 kB" on a run whose
     * true peak was ~5.9 GB, because the producer runs at ~2 GB/s of scrap RAM
     * and every byte of it accumulated between t=0 and the deadline.
     *
     * This is the real fix: it makes the short run MEASURE rather than merely
     * decline to judge. The nsamp<2 guard on the criterion is a backstop for
     * the case this cannot cover (sample array full). */
    rss_take_sample(m, t0, &warned_unavailable);
    return NULL;
}

/* --- xorshift: deterministic per-thread chaos, reproducible on failure. -- */
static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*s = x);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int       tid;
    uint64_t  base;          /* shared 256KB base this thread contends on */
    uint32_t  stripe_start;  /* disjoint byte range inside `base` this thread owns */
    uint32_t  stripe_width;
    uint8_t  *shadow;        /* stripe_width bytes: expected final contents */

    uint64_t *lat;           /* reservoir of latency samples, ns */
    uint64_t  nlat_seen;     /* total writes observed (for reservoir replacement) */
    uint64_t  lat_max_ns;    /* EXACT worst case, outside the reservoir */
    uint64_t  over_thresh;   /* EXACT count of samples > STALL_NS */
    int       err;
} worker_t;

static void *worker(void *arg)
{
    worker_t *w = arg;
    uint32_t  s = (uint32_t)(w->tid * 2654435761u) | 1u;
    uint8_t   buf[SOAK_MAXLEN];
    uint32_t  local_max = w->stripe_width < SOAK_MAXLEN ? w->stripe_width : SOAK_MAXLEN;

    while (!atomic_load(&g_stop)) {
        uint32_t len = 1 + (rnd(&s) % local_max);
        uint32_t off = rnd(&s) % (w->stripe_width - len + 1);   /* unaligned, in-stripe */
        for (uint32_t k = 0; k < len; k++)
            buf[k] = (uint8_t)(w->tid * 31 + (uint32_t)w->nlat_seen * 7 + k);

        uint64_t t0 = now_ns();
        int rc = nox_write(g_engine, buf, len, w->base + w->stripe_start + off);
        uint64_t dt = now_ns() - t0;

        if (rc != 0) { w->err = -1; return NULL; }
        memcpy(w->shadow + off, buf, len);      /* last writer wins, within OUR stripe */

        if (dt > w->lat_max_ns) w->lat_max_ns = dt;
        if (dt > (uint64_t)STALL_NS) w->over_thresh++;

        /* Reservoir sampling (Algorithm R): bounded memory regardless of how
         * long the soak runs (see LAT_RESERVOIR_CAP comment above). */
        uint64_t i = w->nlat_seen++;   /* 0-indexed draw number */
        if (i < LAT_RESERVOIR_CAP) {
            w->lat[i] = dt;
        } else {
            uint64_t j = (uint64_t)rnd(&s) | ((uint64_t)rnd(&s) << 32);
            j %= (i + 1);
            if (j < LAT_RESERVOIR_CAP)
                w->lat[j] = dt;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fprintf(stderr,
                "usage: %s /mnt/nvme/c4soak.dat [seconds] [nthreads]\n"
                "  seconds/nthreads default to env NOX_SOAK_SECONDS / "
                "NOX_SOAK_THREADS, then %u / %u.\n",
                argv[0], DEFAULT_SECONDS, DEFAULT_THREADS);
        return 2;
    }
    const char *path = argv[1];

    uint64_t seconds = DEFAULT_SECONDS;
    if (argc >= 3) {
        seconds = strtoull(argv[2], NULL, 10);
    } else if (getenv("NOX_SOAK_SECONDS")) {
        seconds = strtoull(getenv("NOX_SOAK_SECONDS"), NULL, 10);
    }
    if (seconds == 0) { fprintf(stderr, "seconds must be >= 1\n"); return 2; }

    uint64_t nthreads = DEFAULT_THREADS;
    if (argc >= 4) {
        nthreads = strtoull(argv[3], NULL, 10);
    } else if (getenv("NOX_SOAK_THREADS")) {
        nthreads = strtoull(getenv("NOX_SOAK_THREADS"), NULL, 10);
    }
    if (nthreads < 1 || nthreads > 4096) {
        fprintf(stderr, "nthreads must be 1..4096\n");
        return 2;
    }

    if (seconds < 1200)
        fprintf(stderr,
                "note: seconds=%llu is a SMOKE run (< 20 min). The C4-G7 "
                "soak criterion requires >= 1200s; this run is for driver "
                "validation only, its PASS/FAIL is not the board's answer.\n",
                (unsigned long long)seconds);

    unlink(path);   /* fresh file: zero-init holes, matches the shadow's zero start */

    /* --- Stripe assignment: SHARED_BASES pages, each shared by ~nthreads/
     * SHARED_BASES threads, each thread owning a disjoint byte range inside
     * its page. See the file header for why disjoint bytes + shared pages is
     * the design that makes a mismatch unambiguous. */
    uint32_t counts[SHARED_BASES] = {0};
    for (uint64_t t = 0; t < nthreads; t++)
        counts[t % SHARED_BASES]++;

    /* A thread is pinned to base (t % SHARED_BASES) for the WHOLE run, so the
     * number of bases actually touched is capped by nthreads: raising
     * SHARED_BASES past that spreads nothing and writes nothing to the extra
     * regions. Both numbers below are needed for honest reporting, and the
     * second one decides whether this run tests what the file header claims. */
    uint32_t used_bases   = (nthreads < (uint64_t)SHARED_BASES)
                                ? (uint32_t)nthreads : SHARED_BASES;
    uint32_t max_per_base = 0;
    for (uint32_t b = 0; b < used_bases; b++)
        if (counts[b] > max_per_base)
            max_per_base = counts[b];

    worker_t  *w  = calloc((size_t)nthreads, sizeof(*w));
    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    if (!w || !th) { perror("calloc"); return 1; }

    for (uint64_t t = 0; t < nthreads; t++) {
        uint32_t base_slot   = (uint32_t)(t % SHARED_BASES);
        uint32_t rank        = (uint32_t)(t / SHARED_BASES);
        uint32_t stripe_w    = REG / counts[base_slot];
        if (stripe_w == 0) {
            fprintf(stderr, "nthreads too large relative to SHARED_BASES: "
                            "stripe width underflowed to 0\n");
            return 2;
        }
        w[t].tid           = (int)t;
        w[t].base           = (uint64_t)base_slot * REG;
        w[t].stripe_start   = rank * stripe_w;
        w[t].stripe_width   = stripe_w;
        w[t].shadow         = calloc(1, stripe_w);      /* zero-init: matches fresh file */
        w[t].lat            = malloc((size_t)LAT_RESERVOIR_CAP * sizeof(uint64_t));
        if (!w[t].shadow || !w[t].lat) { perror("malloc"); return 1; }
    }

    g_engine = nox_open(path);
    if (!g_engine) { fprintf(stderr, "nox_open failed\n"); return 1; }

    /* RSS monitor: independent of the write workers, samples continuously
     * for the WHOLE run so growth (or its absence) is visible as a curve,
     * not a single before/after delta that could hide a mid-run spike. */
    rss_monitor_t mon;
    memset(&mon, 0, sizeof(mon));
    mon.cap     = (size_t)(seconds / RSS_SAMPLE_INTERVAL_S) + 8;
    mon.samples = calloc(mon.cap, sizeof(*mon.samples));
    if (!mon.samples) { perror("calloc rss samples"); return 1; }
    pthread_mutex_init(&mon.mtx, NULL);
    pthread_t mon_th;
    if (pthread_create(&mon_th, NULL, rss_monitor_loop, &mon) != 0) {
        perror("pthread_create (rss monitor)"); return 1;
    }

    printf("otflush soak: path=%s seconds=%llu threads=%llu shared_bases=%u "
           "used_bases=%u threads_per_base=%u\n",
           path, (unsigned long long)seconds, (unsigned long long)nthreads,
           SHARED_BASES, used_bases, max_per_base);

    /* THE banner used to claim "overlapping-base contention" unconditionally,
     * which is false whenever every base has exactly one thread — that is the
     * DISJOINT case gate-c4 already covers, and the run proves nothing this
     * driver exists to prove. It is a real trap: `SOAK_THREADS=4` with the
     * default SHARED_BASES=4 gives one thread per base, so a whole series of
     * smoke runs can look like contention testing while testing none. Say which
     * case is actually running, loudly, instead of asserting the wrong one. */
    if (max_per_base < 2)
        fprintf(stderr,
                "WARNING: NOT an overlapping-base run. Every base has exactly "
                "one thread, so no two threads share a scrap_page_t, a page "
                "lock or a queue slot — this is the DISJOINT case gate-c4 "
                "already covers. Overlap needs nthreads > SHARED_BASES (e.g. "
                "the 16/4 default = 4 threads per base).\n");
    else
        printf("  overlapping-base contention: %u threads per base, %u B stripe "
               "each (see file header for why disjoint bytes + shared pages)\n",
               max_per_base, REG / max_per_base);

    uint64_t t_start = now_ns();
    uint64_t deadline = t_start + seconds * 1000000000ull;

    for (uint64_t t = 0; t < nthreads; t++)
        if (pthread_create(&th[t], NULL, worker, &w[t]) != 0) {
            perror("pthread_create (worker)"); return 1;
        }

    /* Main thread just watches the clock; workers poll g_stop themselves. */
    while (now_ns() < deadline && !atomic_load(&g_stop))
        sleep(1);
    atomic_store(&g_stop, 1);

    for (uint64_t t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);
    uint64_t t_writes = now_ns() - t_start;

    pthread_join(mon_th, NULL);

    int werr = 0;
    for (uint64_t t = 0; t < nthreads; t++)
        if (w[t].err) { fprintf(stderr, "FAIL: thread %d hit a write error\n", (int)t); werr = 1; }

    uint64_t t_close0 = now_ns();
    int close_err = nox_close(g_engine);
    if (close_err != 0)
        fprintf(stderr, "FAIL: nox_close reported an error\n");
    uint64_t t_close = now_ns() - t_close0;

    /* --- Criterion 3: foreground latency, aggregated from every thread's
     * bounded reservoir. Percentiles below are ESTIMATES (reservoir, not
     * exhaustive); max and the exact stall count are NOT estimates. */
    uint64_t total_writes = 0, total_over = 0, global_max = 0;
    size_t   reservoir_total = 0;
    for (uint64_t t = 0; t < nthreads; t++) {
        total_writes += w[t].nlat_seen;
        total_over   += w[t].over_thresh;
        if (w[t].lat_max_ns > global_max) global_max = w[t].lat_max_ns;
        reservoir_total += (w[t].nlat_seen < LAT_RESERVOIR_CAP)
                          ? (size_t)w[t].nlat_seen : LAT_RESERVOIR_CAP;
    }
    uint64_t *all = malloc(reservoir_total * sizeof(uint64_t));
    if (!all) { perror("malloc"); return 1; }
    size_t k = 0;
    for (uint64_t t = 0; t < nthreads; t++) {
        size_t n = (w[t].nlat_seen < LAT_RESERVOIR_CAP)
                 ? (size_t)w[t].nlat_seen : LAT_RESERVOIR_CAP;
        for (size_t i = 0; i < n; i++) all[k++] = w[t].lat[i];
    }
    qsort(all, reservoir_total, sizeof(uint64_t), cmp_u64);
    uint64_t p50  = reservoir_total ? all[reservoir_total / 2] : 0;
    uint64_t p99  = reservoir_total ? all[(size_t)(reservoir_total * 0.99)] : 0;
    uint64_t p999 = reservoir_total ? all[(size_t)(reservoir_total * 0.999)] : 0;

    printf("\n=== otflush soak summary ===\n");
    printf("writes:       %llu across %llu threads in %.1f s (target %llu s)\n",
           (unsigned long long)total_writes, (unsigned long long)nthreads,
           t_writes / 1e9, (unsigned long long)seconds);
    printf("nox_close:    %.3f s (drain + join)\n", t_close / 1e9);
    printf("fg latency:   p50~%llu ns  p99~%llu ns  p99.9~%llu ns  "
           "EXACT max=%llu ns  EXACT count>%ldns=%llu (~ = reservoir estimate)\n",
           (unsigned long long)p50, (unsigned long long)p99,
           (unsigned long long)p999, (unsigned long long)global_max,
           STALL_NS, (unsigned long long)total_over);

    /* --- Criterion 1: integrity, ONE check after the final drain. A
     * continuous re-read during the run would race the async flush itself
     * (data legitimately resident in RAM, not yet on disk, is not a bug) -
     * see the file header. */
    int fd = open(path, O_RDONLY | O_DIRECT);
    int bad = 0;
    if (fd < 0) {
        perror("open (read-back)");
        bad = 1;
    } else {
        void *disk = NULL;
        if (posix_memalign(&disk, NOX_BLOCK_SIZE, REG) != 0) {
            fprintf(stderr, "posix_memalign read buffer failed\n");
            bad = 1;
        } else {
            uint8_t *whole = calloc(1, REG);   /* reassembled expected page */
            /* used_bases, NOT SHARED_BASES: no thread is pinned to a base past
             * that, so those regions were never written and the file simply
             * ends. Walking them preads past EOF, which returns a SHORT READ
             * (not an error) and used to be reported through perror — printing
             * "verify pread: Success" and failing criterion 1 on a run with no
             * corruption in it at all. */
            for (uint32_t b = 0; b < used_bases && !bad; b++) {
                memset(whole, 0, REG);
                for (uint64_t t = 0; t < nthreads; t++)
                    if ((t % SHARED_BASES) == b)
                        memcpy(whole + w[t].stripe_start, w[t].shadow, w[t].stripe_width);

                ssize_t rd = pread(fd, disk, REG, (off_t)((uint64_t)b * REG));
                if (rd < 0) {
                    perror("verify pread");
                    bad = 1;
                    break;
                }
                if (rd != (ssize_t)REG) {
                    /* Distinct from an error AND from a mismatch: the engine
                     * did not write as far as it should have. errno is stale
                     * here, so perror would be actively misleading. */
                    fprintf(stderr,
                            "verify: SHORT READ at base %u (offset %llu): got "
                            "%zd of %u bytes — the file ends before a region "
                            "the workload wrote\n",
                            b, (unsigned long long)((uint64_t)b * REG),
                            rd, (unsigned)REG);
                    bad = 1;
                    break;
                }
                if (memcmp(disk, whole, REG) != 0) {
                    size_t j = 0;
                    while (j < REG && ((uint8_t *)disk)[j] == whole[j]) j++;
                    fprintf(stderr,
                            "MISMATCH base %u byte %zu: disk=0x%02x want=0x%02x "
                            "(see file header: expected under overlapping bases "
                            "until C4-B8 lands)\n",
                            b, j, ((uint8_t *)disk)[j], whole[j]);
                    bad = 1;
                }
            }
            free(whole);
        }
        free(disk);
        close(fd);
    }

    /* --- Criterion 2: RSS BOUNDED, judged from the monitor's time series. ---
     *
     * THIS CRITERION WAS "RSS FLAT (growth <= 64MB)" UNTIL C4-B9 LANDED, and
     * that test measured the wrong property once the watermark existed. B9 does
     * not promise flat, it promises BOUNDED: the foreground is gated at
     * NOX_WATERMARK_HIGH live pages, so RSS climbs to a plateau and stays
     * there. A flat-growth test necessarily fails a working watermark - the
     * first post-B9 3s smoke run peaked at 158MB, well-bounded and 36x below
     * the 5.6GB it reached without the gate, and the old test still printed
     * FAIL. A gate that reports FAIL on correct behaviour trains its operator
     * to ignore it, which is worse than having no gate.
     *
     * So the criterion is now two sub-tests, and it takes both:
     *
     *   CEILING - peak growth must fit under a bound COMPUTED from the
     *             watermark config plus this driver's own bookkeeping. Catches
     *             the failure that actually kills the box (unbounded backlog ->
     *             OOM), and is derived rather than tuned, so it tracks
     *             NOX_WATERMARK_HIGH automatically if C5-rest retunes it.
     *
     *   PLATEAU - late-run peak must not exceed mid-run peak. Catches the slow
     *             leak that a deliberately loose ceiling would sail past, and
     *             does so without any absolute number.
     *
     * Neither alone is sufficient. Ceiling alone passes a leak that stays under
     * 1.1GB for the length of the run; plateau alone passes a run that parks at
     * a catastrophic-but-stable level. */
    pthread_mutex_lock(&mon.mtx);
    size_t nsamp = mon.n;
    long first_kb = nsamp ? mon.samples[0].kb : -1;
    long last_kb  = nsamp ? mon.samples[nsamp - 1].kb : -1;
    /* PEAK, not last. RSS is not monotonic: between two samples the flush
     * threads can drain a backlog that had already ballooned, so a last-vs-first
     * delta can read near zero on a run that really did force the machine to
     * hold gigabytes of unflushed scrap pages. The peak is what the box
     * actually had to carry, and it is the quantity C4-B9's watermark exists to
     * bound - so it is the one the criterion must judge. */
    long peak_kb = -1;
    uint64_t peak_t = 0;
    for (size_t i = 0; i < nsamp; i++) {
        if (mon.samples[i].kb > peak_kb) {
            peak_kb = mon.samples[i].kb;
            peak_t  = mon.samples[i].t_s;
        }
    }
    pthread_mutex_unlock(&mon.mtx);

    printf("\nRSS time series: %zu samples over %.0fs (interval %us)\n",
           nsamp, t_writes / 1e9, RSS_SAMPLE_INTERVAL_S);
    printf("RSS first=%ld kB  last=%ld kB  peak=%ld kB @t=%llus  ",
           first_kb, last_kb, peak_kb, (unsigned long long)peak_t);
    long growth_kb = (first_kb >= 0 && peak_kb >= 0) ? (peak_kb - first_kb) : -1;
    int rss_available = (first_kb >= 0 && peak_kb >= 0);

    /* ONE sample cannot show growth: first and peak are then the SAME reading,
     * growth is 0 by construction, and the criterion would announce PASS having
     * measured nothing at all. The deadline sample above should make this
     * unreachable for any run that does real work; it stays as a backstop (the
     * sample array filling up would also land here). INCONCLUSIVE is never a
     * PASS - see the overall verdict below. */
    int rss_conclusive = (nsamp >= 2);

    if (!rss_available)
        printf("(unavailable on this platform)\n");
    else
        printf("growth(peak-first)=%ld kB\n", growth_kb);

    /* --- 2a. CEILING -----------------------------------------------------
     *
     * DERIVED, never a literal, so that retuning NOX_WATERMARK_HIGH in
     * noxdb_config.h cannot silently leave this gate testing a stale number.
     *
     * Engine term: the watermark caps live pages at NOX_WATERMARK_HIGH, plus
     * the documented overshoot of up to one page per concurrent foreground
     * thread (watermark.h, "ACCEPTED IMPRECISION": wait() and note_alloc() are
     * not atomic, so every thread can slip through the check at once). Each
     * page is charged its FULL NOX_DATAZONE_SIZE even though a page waiting in
     * Q1 is typically only ~8% resident - Stage-1's hole fill reads the whole
     * 256KB zone in, so any page that reaches Stage-1 does become fully
     * resident, and the ceiling has to cover the worst case, not the observed
     * mix. (The observed mix is exactly why a 3s run peaks at 158MB against
     * this ~1.1GB ceiling; do not "tighten" the ceiling to match it. The two
     * numbers answer different questions.)
     *
     * Driver term: this benchmark's own bookkeeping is real RSS and must not be
     * charged to the engine - per thread, one shadow stripe plus one fixed
     * latency reservoir. Counted per-thread from w[] rather than assumed
     * uniform, because stripe width depends on how threads divide across
     * SHARED_BASES and is NOT the same for every thread when the division is
     * uneven. */
    unsigned long long engine_ceiling_kb =
        ((unsigned long long)NOX_WATERMARK_HIGH + nthreads) *
        (NOX_DATAZONE_SIZE / 1024ull);

    unsigned long long driver_ceiling_kb = 0;
    for (uint64_t t = 0; t < nthreads; t++)
        driver_ceiling_kb += ((unsigned long long)w[t].stripe_width +
                              (unsigned long long)LAT_RESERVOIR_CAP * sizeof(uint64_t)) / 1024ull;

    unsigned long long ceiling_kb =
        engine_ceiling_kb + driver_ceiling_kb + RSS_CEILING_SLACK_KB;

    int rss_ceiling_ok = rss_available && rss_conclusive &&
                         ((unsigned long long)growth_kb <= ceiling_kb);

    if (rss_available && rss_conclusive)
        printf("  ceiling: growth %ld kB vs %llu kB = "
               "(%u high + %llu overshoot) x %uB pages + %llu kB driver + %u kB slack -> %s\n",
               growth_kb, ceiling_kb, NOX_WATERMARK_HIGH,
               (unsigned long long)nthreads, NOX_DATAZONE_SIZE,
               driver_ceiling_kb, RSS_CEILING_SLACK_KB,
               rss_ceiling_ok ? "OK" : "OVER");

    /* --- 2b. PLATEAU -----------------------------------------------------
     *
     * Discard the first quarter of the series as warm-up: RSS legitimately
     * ramps from ~2MB to the watermark plateau, and including that ramp in the
     * baseline window would make every healthy run look like it is growing.
     * Then split what is left in half and compare PEAKS, not means - a leak
     * shows up as a rising ceiling, and a mean would let a single deep drain
     * mask it.
     *
     * SKIPPED, not failed, below RSS_PLATEAU_MIN_SAMPLES. A 3s smoke run has 2
     * samples and simply has no plateau to inspect; saying FAIL there would be
     * the same category error this whole criterion was just rewritten to
     * remove. The C4-G7 default of 1200s yields ~240 samples. */
    int plateau_ran = 0, rss_plateau_ok = 1;
    long peak_early_kb = -1, peak_late_kb = -1;

    if (rss_available && nsamp >= RSS_PLATEAU_MIN_SAMPLES) {
        size_t warm  = nsamp / 4;
        size_t split = warm + (nsamp - warm) / 2;

        pthread_mutex_lock(&mon.mtx);
        for (size_t i = warm; i < split; i++)
            if (mon.samples[i].kb > peak_early_kb) peak_early_kb = mon.samples[i].kb;
        for (size_t i = split; i < nsamp; i++)
            if (mon.samples[i].kb > peak_late_kb) peak_late_kb = mon.samples[i].kb;
        pthread_mutex_unlock(&mon.mtx);

        plateau_ran    = 1;
        rss_plateau_ok = (peak_late_kb - peak_early_kb) <= (long)RSS_PLATEAU_SLACK_KB;

        printf("  plateau: mid-run peak %ld kB -> late-run peak %ld kB "
               "(drift %+ld kB, slack %u kB) -> %s\n",
               peak_early_kb, peak_late_kb, peak_late_kb - peak_early_kb,
               RSS_PLATEAU_SLACK_KB, rss_plateau_ok ? "OK" : "STILL GROWING");
    } else if (rss_available) {
        printf("  plateau: SKIPPED (%zu samples < %u; run >= %us for this sub-test)\n",
               nsamp, RSS_PLATEAU_MIN_SAMPLES,
               RSS_PLATEAU_MIN_SAMPLES * RSS_SAMPLE_INTERVAL_S);
    }

    int rss_bounded = rss_ceiling_ok && (!plateau_ran || rss_plateau_ok);

    int stalled = (p999 > (uint64_t)STALL_NS);

    printf("\n=== C4 SOAK: per-criterion result ===\n");
    printf("1. INTEGRITY  (memcmp vs shadow):        %s%s\n",
           bad ? "FAIL" : "PASS",
           bad ? "  <- expected until C4-B8 (FLUSHING swap) lands; see header" : "");
    /* Four states, not two. SKIP and INCONCLUSIVE are NOT the same thing and
     * must not print the same word: SKIP means this platform can never measure
     * RSS (no /proc/self/status), so the run still validates criteria 1 and 3;
     * INCONCLUSIVE means THIS run was too short to measure it - an operator
     * error, fixable by running longer, and never a pass. */
    const char *rss_verdict, *rss_note = "";
    if (!rss_available) {
        rss_verdict = "SKIP (no /proc/self/status)";
    } else if (!rss_conclusive) {
        rss_verdict = "INCONCLUSIVE";
        rss_note = "  <- only 1 sample; run longer than RSS_SAMPLE_INTERVAL_S";
    } else if (rss_bounded) {
        rss_verdict = "PASS";
    } else {
        rss_verdict = "FAIL";
        /* C4-B9 HAS LANDED. This note used to say the FAIL was expected until
         * the watermark arrived; saying that now would excuse a real defect.
         * Which sub-test failed is on the ceiling/plateau lines printed above. */
        rss_note = !rss_ceiling_ok
                 ? "  <- REAL: backlog exceeded the watermark ceiling; the gate is not holding"
                 : "  <- REAL: RSS still climbing late in the run; suspect a leak, not the backlog";
    }
    printf("2. RSS BOUNDED (ceiling + plateau):      %s%s\n", rss_verdict, rss_note);
    printf("3. NO STALL   (p99.9 <= %ldns):           %s\n",
           STALL_NS, stalled ? "FAIL" : "PASS");

    /* A real failure outranks an inconclusive one: if integrity broke, the run
     * FAILED regardless of how much of criterion 2 we managed to measure. */
    int rss_grew      = rss_available && rss_conclusive && !rss_bounded;
    int inconclusive  = rss_available && !rss_conclusive;
    int real_fail     = bad || stalled || rss_grew || werr || close_err;
    int overall_pass  = !real_fail && !inconclusive;

    /* Criterion 1 is still the documented C4-B8 gap and may FAIL for a known
     * reason. Criterion 2 no longer has that excuse - B9 is built, so a FAIL
     * there is a defect to chase. Keep the two apart in the message. */
    printf("\nC4 SOAK: %s (see per-criterion lines above; a FAIL on 1 alone is "
           "the DOCUMENTED C4-B8 gap - a FAIL on 2 is NOT, C4-B9 has landed)\n",
           real_fail ? "FAIL" : (inconclusive ? "INCONCLUSIVE" : "PASS"));

    free(all);
    for (uint64_t t = 0; t < nthreads; t++) { free(w[t].shadow); free(w[t].lat); }
    free(w); free(th);
    free(mon.samples);
    pthread_mutex_destroy(&mon.mtx);

    return overall_pass ? 0 : 1;
}
