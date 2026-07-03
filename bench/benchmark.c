/*
 * benchmark.c - NoxDB MVP driver.
 *
 * Issues a mix of (a) large 4K-aligned writes (fast path -> O_DIRECT) and
 * (b) small unaligned writes (scrap path), reports throughput.
 *
 * MUST run on the Proxmox Linux VM with an NVMe-backed XFS/EXT4 filesystem.
 * O_DIRECT is unsupported on macOS/tmpfs, so this will NOT run locally.
 *
 *   usage: ./benchmark <backing-file-on-nvme>
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BIG_WRITE_SIZE   (4u * 1024u * 1024u)   /* 4 MB, fast path */
#define BIG_WRITE_COUNT  64                      /* 256 MB streamed */
#define SMALL_WRITE_SIZE 777u                    /* unaligned, scrap path */
#define SMALL_WRITE_COUNT 100000

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void report(const char *label, double bytes, double secs)
{
    printf("  %-22s %8.1f MB in %6.3f s  =>  %8.1f MB/s\n",
           label, bytes / 1e6, secs, (bytes / 1e6) / secs);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <backing-file-on-nvme>\n", argv[0]);
        return 2;
    }

    nox_engine_t *e = nox_open(argv[1]);
    if (!e) {
        perror("nox_open");
        return 1;
    }

    /* Aligned source buffer for the fast path (avoids the bounce-buffer copy). */
    void *big = NULL;
    if (posix_memalign(&big, NOX_BLOCK_SIZE, BIG_WRITE_SIZE) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        nox_close(e);
        return 1;
    }
    memset(big, 0xAB, BIG_WRITE_SIZE);

    /* ---- Fast path: large aligned writes, laid down contiguously. ---- */
    double t0 = now_sec();
    uint64_t off = 0;
    for (int i = 0; i < BIG_WRITE_COUNT; i++) {
        if (nox_write(e, big, BIG_WRITE_SIZE, off) != 0) {
            perror("nox_write (big)");
            free(big);
            nox_close(e);
            return 1;
        }
        off += BIG_WRITE_SIZE;
    }
    double t1 = now_sec();
    report("fast path (aligned)", (double)BIG_WRITE_COUNT * BIG_WRITE_SIZE, t1 - t0);

    /* ---- Scrap path: many small unaligned writes scattered across pages. ---- */
    char small[SMALL_WRITE_SIZE];
    memset(small, 0xCD, sizeof(small));
    uint64_t base = off; /* start past the big region */
    double t2 = now_sec();
    for (int i = 0; i < SMALL_WRITE_COUNT; i++) {
        /* Unaligned, pseudo-scattered offsets to exercise merge + paging. */
        uint64_t soff = base + (uint64_t)i * 333u;
        if (nox_write(e, small, SMALL_WRITE_SIZE, soff) != 0) {
            perror("nox_write (small)");
            free(big);
            nox_close(e);
            return 1;
        }
    }
    double t3 = now_sec();
    report("scrap path (unaligned)",
           (double)SMALL_WRITE_COUNT * SMALL_WRITE_SIZE, t3 - t2);

    free(big);

    /* Flushes all remaining partial pages (read-before-write) and closes. */
    if (nox_close(e) != 0) {
        fprintf(stderr, "nox_close reported a flush/close error\n");
        return 1;
    }

    printf("OK\n");
    return 0;
}
