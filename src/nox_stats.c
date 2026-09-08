/*
 * nox_stats.c - see nox_stats.h.
 *
 * Entirely empty unless -DNOX_STATS. The translation unit still compiles so the
 * Makefile's src wildcard needs no special case for it.
 */
#define _GNU_SOURCE
#include "nox_stats.h"

#ifdef NOX_STATS

#include "noxdb_config.h"
#include "watermark.h"

#include <stdatomic.h>

/*
 * All counters are relaxed atomics. Relaxed is correct here and not a shortcut:
 * these are independent tallies, nothing else is ordered against them, and the
 * only reader (nox_stats_dump) runs after the writer threads have joined. Using
 * seq_cst would put a full barrier on the foreground write path and change the
 * very latency distribution the engine is trying to defend.
 */
static _Atomic uint64_t s_pages_created;
static _Atomic uint64_t s_seal_entries;
static _Atomic uint64_t s_pages_full;
static _Atomic uint64_t s_user_bytes;
static _Atomic uint64_t s_disk_write;
static _Atomic uint64_t s_disk_read;

/* Index i counts pages observed with exactly i entries in use. Sized
 * NOX_MAX_ENTRIES + 1 so a full array (i == NOX_MAX_ENTRIES) has its own slot
 * and needs no clamping. */
static _Atomic uint64_t s_hist_stage1[NOX_MAX_ENTRIES + 1];
static _Atomic uint64_t s_hist_full[NOX_MAX_ENTRIES + 1];

/* Pages per Stage-2 writeback. Index 0 is unused; a writeback always covers at
 * least one page. Bounded by the iovec cap, so it is a short histogram. */
static _Atomic uint64_t s_hist_batch[NOX_PWRITEV_MAX_IOV + 1];

static inline void bump(_Atomic uint64_t *c, uint64_t n)
{
    atomic_fetch_add_explicit(c, n, memory_order_relaxed);
}

void nox_stat_page_created(void)          { bump(&s_pages_created, 1); }
void nox_stat_seal_entries(void)          { bump(&s_seal_entries, 1); }
void nox_stat_user_bytes(uint64_t n)      { bump(&s_user_bytes, n); }
void nox_stat_disk_write(uint64_t n)      { bump(&s_disk_write, n); }
void nox_stat_disk_read(uint64_t n)       { bump(&s_disk_read, n); }

void nox_stat_page_full(uint32_t entries)
{
    bump(&s_pages_full, 1);
    if (entries <= NOX_MAX_ENTRIES)
        bump(&s_hist_full[entries], 1);
}

void nox_stat_stage2_batch(uint32_t n)
{
    /* Clamp rather than drop: a sample outside the cap would mean
     * stage2_collect overran its batch array, and silently discarding it would
     * hide that. Bucketing it at the cap keeps it visible in the report. */
    if (n > NOX_PWRITEV_MAX_IOV)
        n = NOX_PWRITEV_MAX_IOV;
    bump(&s_hist_batch[n], 1);
}

void nox_stat_entries_at_stage1(uint32_t entries)
{
    if (entries <= NOX_MAX_ENTRIES)
        bump(&s_hist_stage1[entries], 1);
}

void nox_stats_reset(void)
{
    atomic_store_explicit(&s_pages_created, 0, memory_order_relaxed);
    atomic_store_explicit(&s_seal_entries,  0, memory_order_relaxed);
    atomic_store_explicit(&s_pages_full,    0, memory_order_relaxed);
    atomic_store_explicit(&s_user_bytes,    0, memory_order_relaxed);
    atomic_store_explicit(&s_disk_write,    0, memory_order_relaxed);
    atomic_store_explicit(&s_disk_read,     0, memory_order_relaxed);
    for (uint32_t i = 0; i <= NOX_MAX_ENTRIES; i++) {
        atomic_store_explicit(&s_hist_stage1[i], 0, memory_order_relaxed);
        atomic_store_explicit(&s_hist_full[i],   0, memory_order_relaxed);
    }
    for (uint32_t i = 0; i <= NOX_PWRITEV_MAX_IOV; i++)
        atomic_store_explicit(&s_hist_batch[i], 0, memory_order_relaxed);
}

static uint64_t ld(const _Atomic uint64_t *c)
{
    return atomic_load_explicit(c, memory_order_relaxed);
}

static void print_hist(FILE *out, const char *label, const char *unit,
                       const _Atomic uint64_t *h, uint32_t upto, uint64_t total)
{
    if (total == 0) {
        fprintf(out, "  %s: no samples\n", label);
        return;
    }
    fprintf(out, "  %s (%llu samples):\n", label, (unsigned long long)total);
    for (uint32_t i = 0; i <= upto; i++) {
        uint64_t v = ld(&h[i]);
        if (v == 0)
            continue;
        double pct = 100.0 * (double)v / (double)total;
        /* Bar is capped at 40 columns; the percentage is the real datum. */
        int bars = (int)(pct * 0.4);
        fprintf(out, "    %3u %-8s %10llu  %5.1f%%  ", i, unit,
                (unsigned long long)v, pct);
        for (int b = 0; b < bars; b++)
            fputc('#', out);
        fputc('\n', out);
    }
}

void nox_stats_dump(FILE *out)
{
    uint64_t created = ld(&s_pages_created);
    uint64_t seal_e  = ld(&s_seal_entries);
    uint64_t full    = ld(&s_pages_full);
    uint64_t ub      = ld(&s_user_bytes);
    uint64_t dw      = ld(&s_disk_write);
    uint64_t dr      = ld(&s_disk_read);

    fprintf(out, "\n=== noxdb scrap-page statistics ===\n");
    fprintf(out, "  header: %u B, %u index entries, %u B data zone\n",
            NOX_HEADER_SIZE, NOX_MAX_ENTRIES, NOX_DATAZONE_SIZE);
    fprintf(out, "  capacity crossover (zone / entries): %.1f KB average write\n",
            (double)NOX_DATAZONE_SIZE / (double)NOX_MAX_ENTRIES / 1024.0);

    fprintf(out, "\n  pages created:            %llu\n",
            (unsigned long long)created);

    if (created > 0) {
        /* THE headline number. WSBuffer reports "<15 entries in >95% of cases",
         * i.e. an entry-exhaustion rate under 5% on the authors' workloads. */
        fprintf(out, "  sealed: entries exhausted %llu  (%.2f%% of pages)  <-- vs WSBuffer's <5%%\n",
                (unsigned long long)seal_e,
                100.0 * (double)seal_e / (double)created);
        fprintf(out, "  reached full 256KB        %llu  (%.2f%% of pages)\n",
                (unsigned long long)full,
                100.0 * (double)full / (double)created);
    }

    fprintf(out, "\n  user bytes accepted (scrap path): %llu\n",
            (unsigned long long)ub);
    fprintf(out, "  disk bytes written  (Stage-2):    %llu\n",
            (unsigned long long)dw);
    fprintf(out, "  disk bytes read     (Stage-1):    %llu\n",
            (unsigned long long)dr);

    if (ub > 0) {
        /* Measured, not modelled. This is the number to compare across header
         * sizes; everything else in this report explains it. */
        fprintf(out, "\n  WRITE AMPLIFICATION: %.2fx   (disk written / user accepted)\n",
                (double)dw / (double)ub);
        fprintf(out, "  READ  AMPLIFICATION: %.2fx   (disk read / user accepted)\n",
                (double)dr / (double)ub);
    }

    /* What the C4-B9 backpressure gate actually cost the foreground. These come
     * straight from watermark.c's own counters rather than through a nox_stat_*
     * hook: the gate already keeps them under its mutex, and mirroring them into
     * a second set of atomics would only create a way for the two to disagree. */
    uint64_t bc = nox_watermark_blocked_count();
    uint64_t bn = nox_watermark_blocked_ns();

    if (bc == 0) {
        /* The expected result for gate-c4 (385 pages vs a 4096-page high mark),
         * and a meaningful negative: it says the latency distribution this build
         * reports was produced with the gate never engaging, so backpressure
         * cannot be an explanation for anything in it. */
        fprintf(out, "\n  backpressure gate: NEVER ENGAGED (0 stalls, high = %u live pages)\n",
                NOX_WATERMARK_HIGH);
    } else {
        fprintf(out, "\n  backpressure stalls:       %llu\n",
                (unsigned long long)bc);
        /* Summed across every foreground thread, NOT wall-clock. Exceeding the
         * run's elapsed time is correct and expected: it is aggregate stall, not
         * a latency. */
        fprintf(out, "  total stall (all threads): %.3f ms\n",
                (double)bn / 1e6);
        fprintf(out, "  mean stall per block:      %.3f ms\n",
                (double)bn / (double)bc / 1e6);
    }

    fputc('\n', out);
    uint64_t t1 = 0, t2 = 0, t3 = 0;
    for (uint32_t i = 0; i <= NOX_MAX_ENTRIES; i++) {
        t1 += ld(&s_hist_stage1[i]);
        t2 += ld(&s_hist_full[i]);
    }
    for (uint32_t i = 0; i <= NOX_PWRITEV_MAX_IOV; i++)
        t3 += ld(&s_hist_batch[i]);

    print_hist(out, "entries in use when Stage-1 took the page", "entries",
               s_hist_stage1, NOX_MAX_ENTRIES, t1);
    print_hist(out, "entries needed to reach a full 256KB page", "entries",
               s_hist_full, NOX_MAX_ENTRIES, t2);

    /* The question this answers: does the pwritev batch EVER form? If bucket 1
     * holds essentially every sample, NOX_PWRITEV_MAX_IOV is inert and raising
     * it changes nothing -- the binding constraint is base adjacency in Q2, not
     * the iovec cap. Print the concentration explicitly so the answer does not
     * depend on reading a histogram correctly. */
    print_hist(out, "pages coalesced into one Stage-2 writeback", "pages",
               s_hist_batch, NOX_PWRITEV_MAX_IOV, t3);
    if (t3 > 0) {
        uint64_t singles = ld(&s_hist_batch[1]);
        uint64_t pages = 0;
        for (uint32_t i = 0; i <= NOX_PWRITEV_MAX_IOV; i++)
            pages += (uint64_t)i * ld(&s_hist_batch[i]);
        fprintf(out, "  batch never formed (n == 1): %.2f%% of writebacks;"
                     " mean %.2f pages/syscall (cap %u)\n",
                100.0 * (double)singles / (double)t3,
                (double)pages / (double)t3, NOX_PWRITEV_MAX_IOV);
    }
    fprintf(out, "===================================\n\n");
}

#else

/* ISO C forbids an empty translation unit (C11 6.9p1). */
typedef int nox_stats_translation_unit_not_empty;

#endif /* NOX_STATS */
