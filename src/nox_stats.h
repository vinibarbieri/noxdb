/*
 * nox_stats.h - opt-in instrumentation for the scrap-page entry-count and
 * write-amplification study.
 *
 * WHY THIS EXISTS
 *
 * WSBuffer (Zhan et al., FAST '26) states two things about the 128B header:
 *
 *   "the header is 128B-sized BY DEFAULT [...] and 15 8B-sized index entries"
 *   "A larger header means more index entries [...] which applies to workloads
 *    primarily composed of small writes. In our evaluations, the number of index
 *    entries used within a scrap-page is less than 15 in more than 95% cases."
 *
 * So 15 entries is a TUNED DEFAULT justified by a measurement on the authors'
 * workloads, not an invariant — and the paper explicitly points at a larger
 * header for small-write workloads. Before changing NOX_HEADER_SIZE we should
 * run the authors' own experiment on our engine and our workloads, rather than
 * picking a number by intuition. That is what this file measures.
 *
 * THE THREE NUMBERS, in order of how much they decide
 *
 *   1. entry-exhaustion rate = pages sealed by SCRAP_OVERFLOW / pages created.
 *      This is the direct analogue of the paper's ">95% use fewer than 15".
 *      If our rate is under 5%, the 128B default is right for that workload and
 *      the amplification story is a benchmark artifact. If it is 60%, it is not.
 *
 *   2. measured write amplification = disk bytes written / user bytes accepted.
 *      No model, no assumption, no arithmetic of mine to disbelieve. Directly
 *      comparable across header sizes.
 *
 *   3. histogram of entries in use, sampled at ONE well-defined instant (see
 *      nox_stat_entries_at_stage1). Colour for 1 and 2, not a substitute.
 *
 * COST
 *
 * Compiled out entirely unless -DNOX_STATS is given. The counters sit on the
 * foreground write path, and this engine's whole claim is a latency
 * distribution — an unconditional atomic increment per write would perturb the
 * exact number C4 exists to defend. Measurement builds and gate builds are
 * therefore different binaries, on purpose.
 */
#ifndef NOX_STATS_H
#define NOX_STATS_H

#include <stdint.h>
#include <stdio.h>

#ifdef NOX_STATS

/* A fresh scrap page was allocated and published in the index. */
void nox_stat_page_created(void);

/* A page was sealed because its entry array was exhausted (SCRAP_OVERFLOW).
 * This is the eviction reason the header size controls. */
void nox_stat_seal_entries(void);

/* A page reached full 256KB coverage and went straight to Queue-2. This is the
 * eviction reason the header size does NOT control — capacity, as designed.
 * `entries` is how many index entries it needed to get there. */
void nox_stat_page_full(uint32_t entries);

/* Bytes the caller handed to nox_write on the SCRAP path (the fast direct path
 * is excluded: it never touches a scrap page, so it cannot be amplified). */
void nox_stat_user_bytes(uint64_t n);

/* Bytes actually issued to the SSD by Stage-2, and read back by Stage-1. */
void nox_stat_disk_write(uint64_t n);
void nox_stat_disk_read(uint64_t n);

/* Entries in use at the instant Stage-1 pops a PARTIAL page off Queue-1 —
 * i.e. the moment the page stops being something the foreground is filling and
 * becomes something the background is assembling. One sample per page that
 * reaches Stage-1. Pages that never reach it (born full, or forwarded full) are
 * counted by nox_stat_page_full instead, so no page is sampled twice. */
void nox_stat_entries_at_stage1(uint32_t entries);

/* Human-readable report. Called by nox_close. */
void nox_stats_dump(FILE *out);

/* Zero every counter. For a driver that wants to exclude a warm-up phase. */
void nox_stats_reset(void);

#else  /* !NOX_STATS — every hook vanishes, arguments still type-check */

#define nox_stat_page_created()          ((void)0)
#define nox_stat_seal_entries()          ((void)0)
#define nox_stat_page_full(e)            ((void)(e))
#define nox_stat_user_bytes(n)           ((void)(n))
#define nox_stat_disk_write(n)           ((void)(n))
#define nox_stat_disk_read(n)            ((void)(n))
#define nox_stat_entries_at_stage1(e)    ((void)(e))
#define nox_stats_dump(out)              ((void)(out))
#define nox_stats_reset()                ((void)0)

#endif /* NOX_STATS */

#endif /* NOX_STATS_H */
