/*
 * otflush_soak.c - C4 soak gate (board card C4-B6 addendum / C4-G7).
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
 *   2. RSS        - a time series read from /proc/self/status, so monotonic
 *                   growth (unbounded RAM) is visible, not just a single
 *                   before/after number.
 *   3. LATENCY    - the same foreground-never-stalls check as gate-c4, run
 *                   under sustained overlapping-base contention instead of a
 *                   short disjoint-base burst.
 *
 * WHY THIS IS EXPECTED TO SURFACE KNOWN, DOCUMENTED GAPS (read before filing
 * a bug against OTflush over a soak FAIL):
 *
 *   - RSS growth -> C4-B9 (spec 3.6, the eviction watermark) is NOT built in
 *     C4. noxdb_config.h says it outright: "Real RAM bounding is the C5
 *     eviction watermark (spec 3, D5)." Nothing in C4 throttles a foreground
 *     that outruns Stage-1/Stage-2, so a long enough run at a high enough
 *     thread count WILL grow Q1/Q2 backlog RAM without bound. A growing RSS
 *     time series below is that gap made visible, not a mystery leak.
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
 * integrity pass?") and question 2 ("RAM stable?") from .dev/KANBAN.md - run
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
 * exactly the case the C3 and C4 (gate-c4) drivers partition away from. */
#define SHARED_BASES        4u

#define SOAK_MAXLEN          512u    /* max size of one chaotic write */
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

/* How often the RSS monitor thread samples /proc/self/status, in seconds. */
#define RSS_SAMPLE_INTERVAL_S 5u

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

static void *rss_monitor_loop(void *arg)
{
    rss_monitor_t *m = arg;
    uint64_t t0 = now_ns();
    int warned_unavailable = 0;

    while (!atomic_load(&g_stop)) {
        long kb = read_rss_kb();
        uint64_t elapsed_s = (now_ns() - t0) / 1000000000ull;

        if (kb < 0 && !warned_unavailable) {
            fprintf(stderr,
                    "note: RSS sampling unavailable on this platform "
                    "(no /proc/self/status) - RSS column will read -1 "
                    "throughout. This is expected off Linux; run on the "
                    "bench box for a real reading.\n");
            warned_unavailable = 1;
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

        /* Sleep in short slices so a short smoke run (SOAK_SECONDS small)
         * doesn't overshoot its deadline waiting on one long sleep. */
        for (unsigned s = 0; s < RSS_SAMPLE_INTERVAL_S && !atomic_load(&g_stop); s++)
            sleep(1);
    }
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
           "(overlapping-base contention; see file header for why)\n",
           path, (unsigned long long)seconds, (unsigned long long)nthreads,
           SHARED_BASES);

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
            for (uint32_t b = 0; b < SHARED_BASES && !bad; b++) {
                memset(whole, 0, REG);
                for (uint64_t t = 0; t < nthreads; t++)
                    if ((t % SHARED_BASES) == b)
                        memcpy(whole + w[t].stripe_start, w[t].shadow, w[t].stripe_width);

                if (pread(fd, disk, REG, (off_t)((uint64_t)b * REG)) != (ssize_t)REG) {
                    perror("verify pread");
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

    /* --- Criterion 2: RSS flat, judged from the monitor's time series. --- */
    pthread_mutex_lock(&mon.mtx);
    size_t nsamp = mon.n;
    long first_kb = nsamp ? mon.samples[0].kb : -1;
    long last_kb  = nsamp ? mon.samples[nsamp - 1].kb : -1;
    pthread_mutex_unlock(&mon.mtx);

    printf("\nRSS time series: %zu samples over %.0fs (interval %us)\n",
           nsamp, t_writes / 1e9, RSS_SAMPLE_INTERVAL_S);
    printf("RSS first=%ld kB  last=%ld kB  ", first_kb, last_kb);
    /* Growth threshold: 64MB. Chosen as "clearly not noise" - a real page
     * (256KB) leak needs only ~256 leaked pages to cross it. Not tuned
     * against a measured drain rate (that tuning is C5r-BUILD's job, see
     * .dev/KANBAN.md C5-rest); this is a smoke threshold for "is it growing
     * at all," not a precision bound. */
    long growth_kb = (first_kb >= 0 && last_kb >= 0) ? (last_kb - first_kb) : -1;
    int rss_available = (first_kb >= 0 && last_kb >= 0);
    int rss_flat = rss_available && (growth_kb <= 64 * 1024);
    if (!rss_available)
        printf("(unavailable on this platform)\n");
    else
        printf("growth=%ld kB\n", growth_kb);

    int stalled = (p999 > (uint64_t)STALL_NS);

    printf("\n=== C4 SOAK: per-criterion result ===\n");
    printf("1. INTEGRITY  (memcmp vs shadow):        %s%s\n",
           bad ? "FAIL" : "PASS",
           bad ? "  <- expected until C4-B8 (FLUSHING swap) lands; see header" : "");
    printf("2. RSS FLAT   (no monotonic growth):     %s%s\n",
           rss_available ? (rss_flat ? "PASS" : "FAIL") : "SKIP (no /proc/self/status)",
           (rss_available && !rss_flat)
               ? "  <- expected until C4-B9 (eviction watermark) lands; see header"
               : "");
    printf("3. NO STALL   (p99.9 <= %ldns):           %s\n",
           STALL_NS, stalled ? "FAIL" : "PASS");

    int overall_pass = !bad && !stalled && (!rss_available || rss_flat) && !werr && !close_err;
    printf("\nC4 SOAK: %s (see per-criterion lines above; a FAIL on 1 or 2 "
           "alone is the DOCUMENTED C5-core gap, not a mystery - see this "
           "file's header)\n", overall_pass ? "PASS" : "FAIL");

    free(all);
    for (uint64_t t = 0; t < nthreads; t++) { free(w[t].shadow); free(w[t].lat); }
    free(w); free(th);
    free(mon.samples);
    pthread_mutex_destroy(&mon.mtx);

    return overall_pass ? 0 : 1;
}
