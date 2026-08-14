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
}

static uint64_t ld(const _Atomic uint64_t *c)
{
    return atomic_load_explicit(c, memory_order_relaxed);
}

static void print_hist(FILE *out, const char *label,
                       const _Atomic uint64_t *h, uint64_t total)
{
    if (total == 0) {
        fprintf(out, "  %s: no samples\n", label);
        return;
    }
    fprintf(out, "  %s (%llu pages):\n", label, (unsigned long long)total);
    for (uint32_t i = 0; i <= NOX_MAX_ENTRIES; i++) {
        uint64_t v = ld(&h[i]);
        if (v == 0)
            continue;
        double pct = 100.0 * (double)v / (double)total;
        /* Bar is capped at 40 columns; the percentage is the real datum. */
        int bars = (int)(pct * 0.4);
        fprintf(out, "    %3u entries  %10llu  %5.1f%%  ", i,
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

    fputc('\n', out);
    uint64_t t1 = 0, t2 = 0;
    for (uint32_t i = 0; i <= NOX_MAX_ENTRIES; i++) {
        t1 += ld(&s_hist_stage1[i]);
        t2 += ld(&s_hist_full[i]);
    }
    print_hist(out, "entries in use when Stage-1 took the page", s_hist_stage1, t1);
    print_hist(out, "entries needed to reach a full 256KB page", s_hist_full, t2);
    fprintf(out, "===================================\n\n");
}

#else

/* ISO C forbids an empty translation unit (C11 6.9p1). */
typedef int nox_stats_translation_unit_not_empty;

#endif /* NOX_STATS */
