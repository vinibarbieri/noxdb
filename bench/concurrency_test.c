/*
 * concurrency_test.c - C3 acceptance gate: concurrent scrap-path integrity +
 * race-freedom + throughput scaling.
 *
 * Gate (KANBAN C3-GATE): N threads write disjoint offsets concurrently;
 * TSan-clean; throughput scales with threads.
 *
 * Strategy (multi-threaded sibling of scrap_integrity_test):
 *   - Partition the file into disjoint 256KB regions. Thread t owns a contiguous
 *     block of RPT regions. No two threads share a base, so this stresses the
 *     sharded index + per-page locks, NOT cross-thread page contention.
 *   - Each thread fills its regions with small sequential scrap-path writes
 *     (WLEN bytes) so each 256KB page fills and flushes INLINE, in parallel with
 *     the other threads -> real concurrent SSD writes -> throughput scales.
 *   - PASSES rewrites give a measurable, tunable amount of work.
 *   - A per-thread shadow holds the expected final region contents.
 *   - After join + nox_close, re-read every region with a SEPARATE raw O_DIRECT
 *     fd (never verify the engine with the engine) and memcmp vs shadow.
 *   - Print aggregate bandwidth (MB/s). Run at 1/2/4/8 threads to see scaling.
 *
 * Build + run ON THE BENCH BOX against /mnt/nvme (O_DIRECT only works there):
 *   make gate-c3      && ./bench/concurrency_test      /mnt/nvme/c3gate.dat 8
 *   make gate-c3-tsan && ./bench/concurrency_test_tsan /mnt/nvme/c3gate.dat 8
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REG    NOX_DATAZONE_SIZE   /* 256KB region == one scrap page */
#define RPT    8u                  /* regions per thread */
#define PASSES 32u                 /* rewrites per region (tune for runtime/BW) */
#define WLEN   256u                /* small sequential write (scrap path; REG%WLEN==0) */

static nox_engine_t *g_engine;

typedef struct {
    int      tid;
    uint64_t first_region;   /* index of this thread's first 256KB region */
    uint8_t *shadow;         /* RPT * REG: expected final contents */
    int      err;            /* set to -1 if any nox_write failed */
} worker_t;

/* Deterministic per-region byte pattern so a mismatch is diagnosable. */
static uint8_t pat(int tid, uint64_t region, uint32_t intra)
{
    return (uint8_t)(0x40 + ((tid * 7 + (int)region * 3 + (int)intra) & 0x3f));
}

static void *worker(void *arg)
{
    worker_t *w = arg;

    /* Build this thread's shadow once: the final image is pass-independent. */
    for (uint32_t r = 0; r < RPT; r++) {
        uint64_t region = w->first_region + r;
        uint8_t *shad    = w->shadow + (size_t)r * REG;
        for (uint32_t i = 0; i < REG; i++)
            shad[i] = pat(w->tid, region, i);
    }

    for (uint32_t pass = 0; pass < PASSES; pass++) {
        for (uint32_t r = 0; r < RPT; r++) {
            uint64_t region = w->first_region + r;
            uint64_t rbase  = region * (uint64_t)REG;
            for (uint32_t off = 0; off + WLEN <= REG; off += WLEN) {
                uint8_t buf[WLEN];
                for (uint32_t i = 0; i < WLEN; i++)
                    buf[i] = pat(w->tid, region, off + i);
                /* small write -> scrap path; adjacent writes coalesce, page
                 * fills at REG/WLEN writes -> inline full flush, in parallel. */
                if (nox_write(g_engine, buf, WLEN, rbase + off) != 0)
                    w->err = -1;
            }
        }
    }
    return NULL;
}

/* Raw O_DIRECT read-back vs shadow. Returns mismatch count, or -1 on setup err. */
static int verify(const char *path, worker_t *ws, int nthreads)
{
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) { perror("open (read-back)"); return -1; }

    void *disk = NULL;
    if (posix_memalign(&disk, NOX_BLOCK_SIZE, REG) != 0) {
        fprintf(stderr, "posix_memalign read buffer failed\n");
        close(fd);
        return -1;
    }

    int fails = 0;
    for (int t = 0; t < nthreads; t++) {
        for (uint32_t r = 0; r < RPT; r++) {
            uint64_t region = ws[t].first_region + r;
            ssize_t rd = pread(fd, disk, REG, (off_t)(region * (uint64_t)REG));
            if (rd != (ssize_t)REG) {
                fprintf(stderr, "  region %llu: short/failed read (%zd, errno=%d)\n",
                        (unsigned long long)region, rd, errno);
                fails++;
                continue;
            }
            uint8_t *shad = ws[t].shadow + (size_t)r * REG;
            if (memcmp(disk, shad, REG) != 0) {
                size_t j = 0;
                while (j < REG && ((uint8_t *)disk)[j] == shad[j]) j++;
                fprintf(stderr,
                        "  region %llu: MISMATCH at +%zu (disk=0x%02x shadow=0x%02x)\n",
                        (unsigned long long)region, j,
                        ((uint8_t *)disk)[j], shad[j]);
                fails++;
            }
        }
    }
    free(disk);
    close(fd);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /mnt/nvme/c3gate.dat <nthreads>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int nthreads = atoi(argv[2]);
    if (nthreads < 1 || nthreads > 256) {
        fprintf(stderr, "nthreads must be 1..256\n");
        return 2;
    }

    unlink(path);   /* fresh file: holes read back as 0 */

    g_engine = nox_open(path);
    if (!g_engine) { perror("nox_open"); return 2; }

    worker_t  *ws = calloc((size_t)nthreads, sizeof(*ws));
    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    if (!ws || !th) { perror("calloc"); return 2; }

    for (int t = 0; t < nthreads; t++) {
        ws[t].tid          = t;
        ws[t].first_region = (uint64_t)t * RPT;
        ws[t].shadow       = malloc((size_t)RPT * REG);
        if (!ws[t].shadow) { perror("malloc shadow"); return 2; }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int t = 0; t < nthreads; t++)
        if (pthread_create(&th[t], NULL, worker, &ws[t]) != 0) {
            perror("pthread_create"); return 2;
        }
    for (int t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    int werr = 0;
    for (int t = 0; t < nthreads; t++)
        if (ws[t].err) werr = 1;
    if (werr)
        fprintf(stderr, "WARNING: a nox_write returned an error during load\n");

    if (nox_close(g_engine) != 0) {
        fprintf(stderr, "nox_close reported a flush error\n");
        werr = 1;
    }

    /* Bandwidth over the parallel phase: every byte written was flushed. */
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double bytes = (double)nthreads * RPT * PASSES * (double)REG;
    printf("threads=%d  time=%.3fs  bandwidth=%.1f MB/s\n",
           nthreads, secs, bytes / 1e6 / secs);

    int fails = verify(path, ws, nthreads);
    if (fails < 0) return 2;

    if (fails == 0 && werr == 0) {
        printf("C3-GATE PASS: %d region(s) match disk (concurrent scrap integrity)\n",
               nthreads * (int)RPT);
        return 0;
    }
    printf("C3-GATE FAIL: %d region mismatch(es), write_err=%d\n", fails, werr);
    return 1;
}
