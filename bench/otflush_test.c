/*
 * otflush_test.c - C4 acceptance gate: asynchronous two-stage flushing.
 *
 * Gate (C4-GATE): thousands of chaotic small writes; foreground never
 * stalls; all data lands on disk; TSan-clean.
 *
 * Three properties, all checked here:
 *   1. INTEGRITY   - after nox_close, a SEPARATE raw O_DIRECT fd reads back
 *                    exactly the shadow image. Never verify the engine with the
 *                    engine.
 *   2. PRESERVATION - regions are pre-seeded with a known pattern through the
 *                    same raw fd, then partially overwritten with sparse
 *                    unaligned writes. Bytes the user never wrote must survive.
 *                    This is what fails loudly if Stage-1 hole filling is wrong.
 *   3. NO STALL    - per-call nox_write latency is sampled; the p99.9 must stay
 *                    at RAM-copy scale. An SSD write is ~4 orders of magnitude
 *                    slower, so a foreground flush shows up immediately.
 *
 * Threads write DISJOINT 256KB bases (the C3 precondition, still in force in
 * C4 - same-base contention is C5). The OVERLAPPING-base case this gate
 * deliberately does not cover is exercised instead by `bench/otflush_soak.c`
 * (make soak-c4), which also targets the still-unbuilt C5-core swap protocol
 * (C4-B8) and eviction watermark (C4-B9).
 *
 * Build + run ON THE BENCH BOX against /mnt/nvme:
 *   make gate-c4      && ./bench/otflush_test      /mnt/nvme/c4gate.dat 8
 *   make gate-c4-tsan && ./bench/otflush_test_tsan /mnt/nvme/c4gate.dat 4
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>          /* pow(): log-spaced quantile grid for the CSV dump */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REG        NOX_DATAZONE_SIZE   /* 256KB region == one scrap page */
#define RPT        8u                  /* regions per thread */
#define WRITES_PER_REGION 400u         /* chaotic small writes per region */
#define MAXLEN     600u                /* max size of one chaotic write */
#define STALL_NS   200000L             /* 200us: far above a RAM copy, far below
                                        * an O_DIRECT write. A foreground flush
                                        * cannot hide under this. */

static nox_engine_t *g_engine;
static const char   *g_path;

typedef struct {
    int       tid;
    uint64_t  first_region;
    uint8_t  *shadow;       /* RPT * REG expected final contents */
    uint64_t *lat;          /* per-write latency samples, ns */
    size_t    nlat;
    int       err;
} worker_t;

static uint8_t seed_pat(uint64_t region, uint32_t intra)
{
    return (uint8_t)(0x80 + ((region * 11 + intra * 3) & 0x3f));
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* xorshift: deterministic per-thread chaos, reproducible on failure. */
static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*s = x);
}

static void *worker(void *arg)
{
    worker_t *w = arg;
    uint32_t  s = (uint32_t)(w->tid * 2654435761u) | 1u;
    uint8_t   buf[MAXLEN];

    for (uint32_t r = 0; r < RPT; r++) {
        uint64_t region = w->first_region + r;
        uint8_t *shad   = w->shadow + (size_t)r * REG;

        for (uint32_t i = 0; i < WRITES_PER_REGION; i++) {
            uint32_t len  = 1 + (rnd(&s) % MAXLEN);
            uint32_t off  = rnd(&s) % (REG - len);   /* unaligned on purpose */
            for (uint32_t k = 0; k < len; k++)
                buf[k] = (uint8_t)(w->tid * 31 + i * 7 + k);

            uint64_t t0 = now_ns();
            int rc = nox_write(g_engine, buf, len, region * REG + off);
            uint64_t dt = now_ns() - t0;

            if (rc != 0) { w->err = -1; return NULL; }
            memcpy(shad + off, buf, len);            /* last writer wins */
            w->lat[w->nlat++] = dt;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /mnt/nvme/c4gate.dat <nthreads>\n", argv[0]);
        return 2;
    }
    g_path = argv[1];
    int nthreads = atoi(argv[2]);
    if (nthreads < 1) { fprintf(stderr, "nthreads must be >= 1\n"); return 2; }

    size_t   total_regions = (size_t)nthreads * RPT;
    uint64_t bytes_region  = REG;

    /* --- Seed the file through a RAW fd, so "preserved" bytes are real ---- */
    int raw = open(g_path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (raw < 0) { perror("open raw"); return 1; }

    void *zone = NULL;
    if (posix_memalign(&zone, NOX_BLOCK_SIZE, REG) != 0) { perror("memalign"); return 1; }

    for (size_t r = 0; r < total_regions; r++) {
        for (uint32_t i = 0; i < REG; i++)
            ((uint8_t *)zone)[i] = seed_pat(r, i);
        if (pwrite(raw, zone, REG, (off_t)(r * bytes_region)) != (ssize_t)REG) {
            perror("seed pwrite"); return 1;
        }
    }
    fsync(raw);

    /* --- Shadows start as the seeded image; writes overlay onto them ----- */
    worker_t *w = calloc((size_t)nthreads, sizeof(*w));
    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    if (!w || !th) { perror("calloc"); return 1; }

    for (int t = 0; t < nthreads; t++) {
        w[t].tid          = t;
        w[t].first_region = (uint64_t)t * RPT;
        w[t].shadow       = malloc((size_t)RPT * REG);
        w[t].lat          = malloc((size_t)RPT * WRITES_PER_REGION * sizeof(uint64_t));
        if (!w[t].shadow || !w[t].lat) { perror("malloc"); return 1; }
        for (uint32_t r = 0; r < RPT; r++)
            for (uint32_t i = 0; i < REG; i++)
                w[t].shadow[(size_t)r * REG + i] = seed_pat(w[t].first_region + r, i);
    }

    g_engine = nox_open(g_path);
    if (!g_engine) { fprintf(stderr, "nox_open failed\n"); return 1; }

    uint64_t t_start = now_ns();
    for (int t = 0; t < nthreads; t++)
        if (pthread_create(&th[t], NULL, worker, &w[t]) != 0) { perror("create"); return 1; }
    for (int t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);
    uint64_t t_writes = now_ns() - t_start;

    for (int t = 0; t < nthreads; t++)
        if (w[t].err) { fprintf(stderr, "FAIL: thread %d hit a write error\n", t); return 1; }

    uint64_t t_close0 = now_ns();
    if (nox_close(g_engine) != 0) { fprintf(stderr, "FAIL: nox_close error\n"); return 1; }
    uint64_t t_close = now_ns() - t_close0;

    /* --- Property 3: foreground never stalled -------------------------- */
    size_t nall = 0;
    for (int t = 0; t < nthreads; t++) nall += w[t].nlat;
    uint64_t *all = malloc(nall * sizeof(uint64_t));
    if (!all) { perror("malloc"); return 1; }
    size_t k = 0;
    for (int t = 0; t < nthreads; t++)
        for (size_t i = 0; i < w[t].nlat; i++) all[k++] = w[t].lat[i];
    qsort(all, nall, sizeof(uint64_t), cmp_u64);

    uint64_t p50   = all[nall / 2];
    uint64_t p99   = all[(size_t)(nall * 0.99)];
    uint64_t p999  = all[(size_t)(nall * 0.999)];
    uint64_t pmax  = all[nall - 1];

    /* Optional CDF dump for the write-up. `all` is already sorted, so the
     * empirical CDF is just an index walk; four printed quantiles cannot draw
     * one. Off unless NOX_LAT_CSV names a file, so the gate's own output and
     * timing are untouched when it is not set.
     *
     * The sampling is log-spaced in the SURVIVAL function (1-q) rather than
     * uniform in q: the whole claim of C4 is about the far tail, and a uniform
     * grid spends 99% of its points on the flat part of the curve and lands
     * exactly one sample past p99. This gives equal resolution per decade out
     * to p99.999, then appends the true max. */
    const char *csv_path = getenv("NOX_LAT_CSV");
    if (csv_path) {
        FILE *csv = fopen(csv_path, "w");
        if (!csv) {
            perror("fopen NOX_LAT_CSV");
        } else {
            fprintf(csv, "q,ns\n");
            const int PTS = 600;              /* points across 5 decades of tail */
            for (int i = 0; i <= PTS; i++) {
                /* 1-q sweeps 1e0 -> 1e-5, so q sweeps 0 -> 0.99999 */
                double surv = pow(10.0, -5.0 * (double)i / (double)PTS);
                double q    = 1.0 - surv;
                size_t idx  = (size_t)(q * (double)nall);
                if (idx >= nall) idx = nall - 1;
                fprintf(csv, "%.6f,%llu\n", q, (unsigned long long)all[idx]);
            }
            fprintf(csv, "1.000000,%llu\n", (unsigned long long)pmax);
            fclose(csv);
            fprintf(stderr, "latency CDF (%zu samples) -> %s\n", nall, csv_path);
        }
    }

    printf("writes:       %zu across %d threads in %.3f s\n",
           nall, nthreads, t_writes / 1e9);
    printf("nox_close:    %.3f s (drain + join)\n", t_close / 1e9);
    printf("fg latency:   p50=%llu ns  p99=%llu ns  p99.9=%llu ns  max=%llu ns\n",
           (unsigned long long)p50, (unsigned long long)p99,
           (unsigned long long)p999, (unsigned long long)pmax);

    int stalled = (p999 > (uint64_t)STALL_NS);
    if (stalled)
        fprintf(stderr,
                "FAIL: p99.9 foreground latency %llu ns exceeds %ld ns - the "
                "foreground is blocking on SSD I/O, which is exactly what C4 "
                "removes.\n", (unsigned long long)p999, STALL_NS);

    /* --- Properties 1 + 2: integrity and preservation, via the raw fd --- */
    int bad = 0;
    for (int t = 0; t < nthreads && !bad; t++) {
        for (uint32_t r = 0; r < RPT && !bad; r++) {
            uint64_t region = w[t].first_region + r;
            if (pread(raw, zone, REG, (off_t)(region * bytes_region)) != (ssize_t)REG) {
                perror("verify pread"); return 1;
            }
            const uint8_t *want = w[t].shadow + (size_t)r * REG;
            if (memcmp(zone, want, REG) != 0) {
                for (uint32_t i = 0; i < REG; i++)
                    if (((uint8_t *)zone)[i] != want[i]) {
                        fprintf(stderr,
                                "FAIL: region %llu byte %u: disk=0x%02x want=0x%02x%s\n",
                                (unsigned long long)region, i,
                                ((uint8_t *)zone)[i], want[i],
                                want[i] == seed_pat(region, i)
                                    ? "  (a byte the user never wrote was clobbered"
                                      " -> Stage-1 hole fill is broken)" : "");
                        break;
                    }
                bad = 1;
            }
        }
    }

    close(raw);
    free(zone); free(all); free(th);
    for (int t = 0; t < nthreads; t++) { free(w[t].shadow); free(w[t].lat); }
    free(w);

    if (bad || stalled) { printf("C4 GATE: FAIL\n"); return 1; }
    printf("C4 GATE: PASS (integrity + preservation + no foreground stall)\n");
    return 0;
}
