/*
 * order_repro.c - Does a base's generations reach the SSD in creation order?
 *
 * WHAT THIS EXISTS TO SETTLE
 * --------------------------
 * src/noxdb_config.h claims, in prose, that NOX_STAGE1_THREADS and
 * NOX_STAGE2_THREADS "BOTH MUST REMAIN 1 until C5 lands the per-region
 * generation counter", because a page detached for base B and a fresh page
 * later created for the same B can reach the SSD out of order. That paragraph
 * is an argument. This driver turns it into a measurement, in both directions:
 *
 *   make repro-order        1 Stage-2 thread   -> ordering must HOLD  (PASS)
 *   make repro-order-multi  4 Stage-2 threads  -> ordering must BREAK (FAIL)
 *
 * A PASS from the first build alone would be worthless: it cannot distinguish
 * "the ordering held" from "I never created the condition". The second build is
 * the control that proves the workload actually exercises the hazard, and it is
 * the evidence that justifies building the ordered wait list (C4-B8 residual).
 *
 * HOW A GENERATION IS CREATED
 * ---------------------------
 * Only one mechanism produces two live pages for one base: the entry array
 * runs out. scrap_page_merge returns SCRAP_OVERFLOW, noxdb.c seals the page,
 * detaches it from the index while it is still queued, and retries onto a fresh
 * page (src/noxdb.c, the SCRAP_OVERFLOW branch). So the workload here writes to
 * SCATTERED, non-adjacent slots: every slot is a disjoint segment, coalescing
 * cannot help, and the (NOX_MAX_ENTRIES + 1)-th distinct slot seals the page.
 * Rewriting a slot the page already covers does NOT add an entry, which is why
 * the slot count must exceed NOX_MAX_ENTRIES for sealing to ever happen.
 *
 * WHY THE STALL HOOK
 * ------------------
 * On an idle engine Stage-1 drains each page the instant it is pushed, so the
 * older generation is written back long before the next one is born and the
 * hazard window never opens. -DNOX_REPRO_STALL_STAGE1_MS (see otflush.c) holds
 * each popped page out of both queues, letting the foreground stack several
 * live generations of one base behind it. Without it this test reports PASS for
 * the wrong reason.
 *
 * THE SHADOW, AND WHY IT IS EXACT UNDER CONCURRENCY
 * ------------------------------------------------
 * Each thread owns a DISJOINT set of slots inside the shared base and is the
 * only writer of those slots, so the last value written to a slot is known
 * without any cross-thread synchronisation. The threads still contend on the
 * same page, the same page lock, the same index bucket and the same queue slot
 * -- which is the condition under test -- while the expected final state stays
 * deterministic. This is the same construction bench/otflush_soak.c uses, and
 * it deliberately violates the disjoint-base precondition in include/noxdb.h
 * for the same documented reason.
 *
 * THE DIAGNOSTIC
 * --------------
 * Each slot holds {seq, slot_id}. seq increases every time its owner rewrites
 * it, so a mismatch is self-classifying:
 *
 *   seq  <  expected  -> STALE GENERATION. An older page's writeback landed
 *                        after a newer one's. This is the lost update the
 *                        config paragraph predicts; it is the signature.
 *   slot_id wrong     -> torn or misplaced bytes, a different bug entirely.
 *   seq  >  expected   -> impossible; would mean the engine invented data.
 *
 * Reporting which of the three occurred is the whole point: "FAIL" alone would
 * not tell you whether you reproduced the ordering bug or found a new one.
 *
 * Bench box only (O_DIRECT). Run against /mnt/nvme, never the OS disk.
 *   ./bench/order_repro /mnt/nvme/order.dat [threads] [rounds]
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Slot stride. Two constraints fight here and 2048 is where they meet:
 *  - stride > 8 so consecutive slots are NOT adjacent; coalesce_insert merges
 *    segments that merely touch (end == new_off), which would collapse the
 *    entry list and make sealing unreachable.
 *  - NOX_DATAZONE_SIZE / stride must EXCEED NOX_MAX_ENTRIES, or the page can
 *    cover every slot without ever running out of entries -- again no sealing.
 *    At 64 entries this gives 128 slots, i.e. twice the headroom needed. */
#define SLOT_STRIDE   2048u
#define SLOTS         (NOX_DATAZONE_SIZE / SLOT_STRIDE)

_Static_assert(SLOTS > NOX_MAX_ENTRIES,
               "SLOTS must exceed NOX_MAX_ENTRIES or no page ever seals by "
               "entry exhaustion, and the test silently measures nothing");

/* The base under test. Non-zero so a stray write to offset 0 cannot masquerade
 * as a correct result, and 256KB-aligned so it is exactly one page base. */
#define TEST_BASE     ((uint64_t)NOX_DATAZONE_SIZE * 4u)

typedef struct {
    uint32_t seq;   /* rewrite counter for this slot, starts at 1 */
    uint32_t slot;  /* slot index, so misplaced bytes are distinguishable */
} record_t;

_Static_assert(sizeof(record_t) == 8, "record_t must be exactly 8B");

static uint32_t g_expected[SLOTS];   /* last seq written per slot (the shadow) */

typedef struct {
    nox_engine_t *e;
    uint32_t      first_slot;   /* this thread's stripe: every nthreads-th slot */
    uint32_t      nthreads;
    uint32_t      rounds;
    int           rc;
} writer_t;

static void *writer(void *arg)
{
    writer_t *w = arg;
    /* Per-thread PRNG state: rand() is not thread-safe and its global state
     * would serialise the writers on libc's internal lock, which is exactly the
     * contention this test must NOT introduce of its own accord. */
    unsigned seed = 0x9E37u + w->first_slot;

    for (uint32_t r = 0; r < w->rounds; r++) {
        /* Pick one of MY slots. Scattered on purpose: a sequential sweep would
         * coalesce into few entries and the page would fill by capacity instead
         * of sealing by entry exhaustion, producing no second generation. */
        uint32_t k    = (uint32_t)rand_r(&seed) % (SLOTS / w->nthreads);
        uint32_t slot = w->first_slot + k * w->nthreads;

        record_t rec = { .seq = ++g_expected[slot], .slot = slot };

        if (nox_write(w->e, &rec, sizeof(rec),
                      TEST_BASE + (uint64_t)slot * SLOT_STRIDE) != 0) {
            fprintf(stderr, "order_repro: nox_write failed at slot %u: %s\n",
                    slot, strerror(errno));
            w->rc = -1;
            return NULL;
        }
    }
    return NULL;
}

/* Read the region back the way the gates do: raw O_DIRECT, bypassing the engine
 * entirely, so what is compared is what the DEVICE holds and not what any RAM
 * structure believes. */
static int read_back_direct(const char *path, void *buf)
{
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "order_repro: verify open failed: %s\n", strerror(errno));
        return -1;
    }
    ssize_t got = pread(fd, buf, NOX_DATAZONE_SIZE, (off_t)TEST_BASE);
    if (got < 0 && errno == EINVAL)
        fprintf(stderr, "noxdb: O_DIRECT alignment violation on verify pread\n");
    close(fd);
    if (got != (ssize_t)NOX_DATAZONE_SIZE) {
        fprintf(stderr, "order_repro: short verify read: %zd of %u\n",
                got, NOX_DATAZONE_SIZE);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s /mnt/nvme/order.dat [threads] [rounds]\n", argv[0]);
        return 2;
    }
    const char *path     = argv[1];
    uint32_t    nthreads = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 4u;
    uint32_t    rounds   = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 10) : 20000u;

    if (nthreads == 0 || nthreads > SLOTS)
        nthreads = 4u;
    /* Every thread must own the same number of slots, or the stripe arithmetic
     * hands some slots to nobody and they verify as never-written. */
    while (SLOTS % nthreads)
        nthreads--;

    /* --- PREDICTION, stated before the run (never after) ------------------ */
    printf("=== C4 write-ordering repro ===\n");
    printf("build:   Stage-1 threads=%u  Stage-2 threads=%u  entries=%u\n",
           NOX_STAGE1_THREADS, NOX_STAGE2_THREADS, NOX_MAX_ENTRIES);
    /* Which amplifier is armed is part of the result, not a build detail: the
     * 2026-08-20 run measured PASS where FAIL was predicted purely because the
     * Stage-1 hook was armed for a Stage-2 hazard. Printing both makes that
     * mistake visible in the output file instead of only in the Makefile. */
#ifdef NOX_REPRO_STALL_STAGE1_MS
    printf("         amplifier: Stage-1 stall=%d ms per page (producer-side)\n",
           (int)(NOX_REPRO_STALL_STAGE1_MS));
#endif
#ifdef NOX_REPRO_STALL_STAGE2_MS
    printf("         amplifier: Stage-2 jitter=0..%d ms per page (consumer-side)\n",
           (int)(NOX_REPRO_STALL_STAGE2_MS));
#endif
#if !defined(NOX_REPRO_STALL_STAGE1_MS) && !defined(NOX_REPRO_STALL_STAGE2_MS)
    printf("         amplifier: NONE  <-- generations will not stack; a PASS\n"
           "         here does NOT mean the ordering held.\n");
#endif
    /* Runtime, not #if: NOX_STAGE2_THREADS expands to a cast expression, which
     * a preprocessor conditional cannot evaluate. The compiler folds this
     * anyway, and the diagnostic has to reach the output file regardless. */
#ifdef NOX_REPRO_STALL_STAGE1_MS
    if (NOX_STAGE2_THREADS > 1u)
        printf("         WARNING: Stage-1 stall with %u Stage-2 threads throttles\n"
               "         the PRODUCER, so Q2 rarely holds two generations of one\n"
               "         base at once. This is the configuration that produced a\n"
               "         false PASS on 2026-08-20. Use the Stage-2 jitter instead.\n",
               NOX_STAGE2_THREADS);
#endif
    printf("load:    %u threads, %u rounds each, %u slots of %u B stride,\n"
           "         all on ONE base at offset %" PRIu64 "\n",
           nthreads, rounds, SLOTS, SLOT_STRIDE, TEST_BASE);
    printf("PREDICTION: %s\n",
           (NOX_STAGE2_THREADS == 1u && NOX_STAGE1_THREADS == 1u)
               ? "PASS. Q1/Q2 are FIFO and each stage is pinned to one thread,\n"
                 "            so pages for a base are written in creation order."
               : "FAIL, with STALE GENERATION mismatches. Concurrent stage\n"
                 "            threads reorder the writebacks of one base.");
    printf("================================\n\n");
    fflush(stdout);

    /* Preallocate: the region must exist before Stage-1 preads holes from it,
     * and a hole in a sparse file must read as zeros rather than short. */
    int pf = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (pf < 0) {
        fprintf(stderr, "order_repro: create %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (ftruncate(pf, (off_t)(TEST_BASE + NOX_DATAZONE_SIZE)) != 0) {
        fprintf(stderr, "order_repro: ftruncate: %s\n", strerror(errno));
        close(pf);
        return 1;
    }
    close(pf);

    nox_engine_t *e = nox_open(path);
    if (!e) {
        fprintf(stderr, "order_repro: nox_open %s: %s\n", path, strerror(errno));
        return 1;
    }

    writer_t  *w  = calloc(nthreads, sizeof(*w));
    pthread_t *th = calloc(nthreads, sizeof(*th));
    if (!w || !th) {
        fprintf(stderr, "order_repro: out of memory\n");
        return 1;
    }

    for (uint32_t t = 0; t < nthreads; t++) {
        w[t] = (writer_t){ .e = e, .first_slot = t,
                           .nthreads = nthreads, .rounds = rounds, .rc = 0 };
        if (pthread_create(&th[t], NULL, writer, &w[t]) != 0) {
            fprintf(stderr, "order_repro: pthread_create failed\n");
            return 1;
        }
    }

    int wr_rc = 0;
    for (uint32_t t = 0; t < nthreads; t++) {
        pthread_join(th[t], NULL);
        if (w[t].rc != 0)
            wr_rc = -1;
    }

    /* Drains every queue and flushes the residual partial pages. The comparison
     * below is only meaningful after this returns. */
    if (nox_close(e) != 0) {
        fprintf(stderr, "order_repro: nox_close reported a flush error\n");
        wr_rc = -1;
    }

    void *buf = NULL;
    if (posix_memalign(&buf, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) != 0) {
        fprintf(stderr, "order_repro: verify buffer alloc failed\n");
        return 1;
    }
    if (read_back_direct(path, buf) != 0)
        return 1;

    /* --- classify every slot --------------------------------------------- */
    uint32_t stale = 0, torn = 0, impossible = 0, missing = 0;
    uint32_t worst_gap = 0;

    for (uint32_t s = 0; s < SLOTS; s++) {
        record_t got;
        memcpy(&got, (uint8_t *)buf + (size_t)s * SLOT_STRIDE, sizeof(got));
        uint32_t want = g_expected[s];

        if (want == 0)                       /* never written by anyone */
            continue;
        if (got.seq == want && got.slot == s)
            continue;

        if (got.slot != s) {
            torn++;
        } else if (got.seq == 0) {
            missing++;                       /* the slot's writes vanished */
        } else if (got.seq < want) {
            stale++;
            if (want - got.seq > worst_gap)
                worst_gap = want - got.seq;
        } else {
            impossible++;
        }

        if (stale + torn + impossible + missing <= 5)
            fprintf(stderr,
                    "  slot %4u: got {seq=%u, slot=%u}  want {seq=%u, slot=%u}"
                    "  -> %s\n",
                    s, got.seq, got.slot, want, s,
                    got.slot != s     ? "TORN/MISPLACED"
                    : got.seq == 0    ? "MISSING"
                    : got.seq < want  ? "STALE GENERATION"
                                      : "IMPOSSIBLE (seq from the future)");
    }

    uint32_t bad = stale + torn + impossible + missing;

    printf("\n--- result ---\n");
    printf("slots written: %u of %u\n", SLOTS, SLOTS);
    printf("mismatches:    %u  (stale=%u torn=%u missing=%u impossible=%u)\n",
           bad, stale, torn, missing, impossible);
    if (stale)
        printf("worst staleness: %u rewrites behind\n", worst_gap);

    if (wr_rc != 0) {
        printf("VERDICT: ERROR - the run itself failed; the comparison above "
               "says nothing about ordering.\n");
        return 1;
    }
    if (bad == 0) {
        printf("VERDICT: PASS - every slot holds its final value; no generation "
               "of this base overtook another.\n");
        return 0;
    }
    if (stale == bad)
        printf("VERDICT: FAIL - write ordering violated. Every mismatch is an "
               "OLDER generation landing after a newer one, which is exactly "
               "the hazard NOX_STAGE2_THREADS==1 exists to prevent.\n");
    else
        printf("VERDICT: FAIL - and NOT purely the ordering hazard: %u of %u "
               "mismatches are torn/missing/impossible. Investigate those "
               "before attributing anything to ordering.\n",
               bad - stale, bad);
    return 1;
}
