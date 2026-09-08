/*
 * noxdb_config.h - Compile-time constants for the NoxDB engine.
 *
 * Every magic number here is dictated by docs/01_architecture_noxdb.md and
 * docs/02_posix_constraints.md (the source of truth). Do not invent values.
 */
#ifndef NOXDB_CONFIG_H
#define NOXDB_CONFIG_H

#include <stdint.h>

/* Logical block size. ALL O_DIRECT transfers (buffer addr, file offset, length)
 * must be a multiple of this. (docs/01 §1, docs/02 §1) */
#define NOX_BLOCK_SIZE       4096u

/* Writes >= this size that are also 4K-aligned bypass the scrap buffer and go
 * straight to the SSD via O_DIRECT (the "fast path"). (docs/01 §1, §3) */
#define NOX_DIRECT_THRESHOLD (1u * 1024u * 1024u)   /* 1 MB */

/* Scrap-page header size. DERIVED from the entry count below — see there. The
 * 8 non-entry bytes are WSBuffer's own field list: 4B counter + 2B ssd_id +
 * 1B number + 1B tag, which pack with no padding (scrap_page.h enforces it). */
#define NOX_HEADER_SIZE      (8u + NOX_MAX_ENTRIES * 8u)

/* Scrap-page data zone is exactly 256 KB. Allocated SEPARATELY from the header
 * via posix_memalign(..., 4096, ...) so its address satisfies O_DIRECT.
 * (docs/01 §1, §2 CRITICAL) */
#define NOX_DATAZONE_SIZE    (256u * 1024u)         /* 262144 */

/*
 * Index entries in the header, 8B each. FULL RATIONALE IN docs/01 §2.1 — read
 * it before changing this. Summary of what is recorded there:
 *
 * 15 IS WSBUFFER'S DEFAULT, NOT AN INVARIANT. The paper says so directly ("the
 * header is 128B-sized BY DEFAULT") and points at a larger header for
 * small-write workloads. Raising this uses the knob they document.
 *
 * WHY 64. A page is evicted either because its 256KB filled (capacity, as
 * designed) or because this array ran out (fragmentation). The crossover is
 * exactly NOX_DATAZONE_SIZE / NOX_MAX_ENTRIES. 64 is not fitted to a benchmark:
 * NOX_DATAZONE_SIZE / NOX_BLOCK_SIZE == 64, so 64 segments of 4096B put counter
 * at exactly 262144 — the entry array CANNOT bind before capacity for any
 * segment >= 4KB, which is the device's own addressing granularity. Below 4KB
 * amplification is physics, and absorbing it is what the scrap buffer is for.
 *
 * Measured, C4 gate, 8 threads (docs/01 §2.1 has the full sweep): 15 entries
 * sealed 96.27% of pages by exhaustion at 58.53x write amplification; 64 cut
 * drain 5.105s -> 1.811s and foreground p99 58835ns -> 27792ns. Cost is 392B
 * per page (0.15%), and fragmented workloads use LESS total RAM because ~4.5x
 * fewer pages exist.
 *
 * 255 IS A HARD CEILING and it is the paper's, not ours: the `number` field is
 * one byte (scrap_page.h). Going past it changes WSBuffer's header layout.
 *
 * Overridable from the build so a sweep needs no source edit. Applies to every
 * target including the gates; objects rebuild automatically when it changes:
 *      make gate-c4 NOX_ENTRIES=255
 */
#ifdef NOX_MAX_ENTRIES_OVERRIDE
#define NOX_MAX_ENTRIES      ((uint32_t)(NOX_MAX_ENTRIES_OVERRIDE))
#else
#define NOX_MAX_ENTRIES      64u
#endif

/* Page-index shard count: the index's bucket lists are partitioned across this
 * many independent mutexes so concurrent writers on different pages don't
 * serialize on one global lock (the XArray bottleneck; docs/01 §5).
 * ENGINEERING CHOICE, not dictated by the paper: 64 > NVMe queue depth (SN530
 * ~QD32) and > bench box cores, so false contention is negligible; finer (e.g.
 * per-bucket) locking buys nothing yet risks false sharing. See docs/01 §5.
 *
 * Expressed in BITS because the shard is selected by a bit-slice of the page
 * hash (page_index.c pi_shard): deriving the count from the width keeps the two
 * impossible to desynchronise, and makes "power of two" true by construction
 * rather than by an assert. 2^6 = 64. */
#define NOX_INDEX_SHARD_BITS 6u
#define NOX_INDEX_SHARDS     (1u << NOX_INDEX_SHARD_BITS)

/* Cache-line size: shard locks are padded to this so two distinct shard mutexes
 * never share a line (no false sharing when locked from different cores). */
#define NOX_CACHELINE        64u

/* Page-flush state values stored in scrap_header_t.tag. */
#define NOX_TAG_OPEN         0u   /* page still accepting writes */
#define NOX_TAG_FULL         1u   /* data zone fully covered, queued/flushed */

/* True if a pointer / offset / length is 4K-aligned. */
#define NOX_IS_ALIGNED(x)    ((((uintptr_t)(x)) & (NOX_BLOCK_SIZE - 1)) == 0)

/* Round a file offset down to the 256KB scrap-page boundary it belongs to.
 * This is the hash-index key for a page. */
#define NOX_PAGE_BASE(off)   ((uint64_t)(off) & ~((uint64_t)NOX_DATAZONE_SIZE - 1))

/* --- OTflush (C4) --------------------------------------------------------
 * Thread counts follow the paper's own default: "WSBuffer sets only two
 * queue-thread pairs by default, one for Stage-1 and the other for Stage-2".
 *
 * BOTH MUST REMAIN 1 until C5 lands the per-region generation counter. Two
 * different arguments, same conclusion:
 *
 *   NOX_STAGE2_THREADS: a page detached for base B and a FRESH page later
 *   created for the same B can reach the SSD out of order -> silent data loss.
 *   One thread + a FIFO queue makes ordering hold by construction.
 *
 *   NOX_STAGE1_THREADS: the writeback ordering guard (otflush.c) is sufficient
 *   only because "Q1 is FIFO and Stage-1 is single-threaded", so pages for one
 *   base are hole-filled in creation order. With two Stage-1 threads they can
 *   be filled out of order and pushed to Q2 swapped, reviving exactly the
 *   corruption C2-GATE caught (region @1310720). Raising this to chase
 *   throughput WILL corrupt data.
 *
 * Cost of the Stage-1 pin, measured: with the foreground no longer pre-faulting
 * data zones (see scrap_page_alloc), those faults land on this one thread and
 * C4-GATE end-to-end went 2.89 s -> 4.20 s. Latency was bought with throughput
 * deliberately; the throughput comes back in C5. */

/*
 * OVERRIDABLE FROM THE BUILD, same convention as NOX_MAX_ENTRIES_OVERRIDE.
 *
 * This is NOT a tuning dial and raising it is NOT supported for any build whose
 * output is a gate result or a published number. It exists for exactly one
 * purpose: bench/order_repro.c has to be able to BUILD the broken configuration
 * in order to demonstrate that the paragraph above is a measured fact rather
 * than an assertion. Both halves of the claim need evidence --
 *
 *   make repro-order        -> 1 Stage-2 thread, ordering must HOLD
 *   make repro-order-multi  -> 4 Stage-2 threads, ordering must BREAK
 *
 * -- and without an override the second build requires editing this file, which
 * is precisely how a "temporary" edit ends up in a measurement binary.
 *
 * The dormancy argument depends on BOTH staying 1; see the two paragraphs
 * above for why each one does, and note they fail differently (Stage-2 reorders
 * writebacks, Stage-1 reorders hole fills).
 */
#ifdef NOX_STAGE1_THREADS_OVERRIDE
#define NOX_STAGE1_THREADS   ((uint32_t)(NOX_STAGE1_THREADS_OVERRIDE))
#else
#define NOX_STAGE1_THREADS   1u
#endif

#ifdef NOX_STAGE2_THREADS_OVERRIDE
#define NOX_STAGE2_THREADS   ((uint32_t)(NOX_STAGE2_THREADS_OVERRIDE))
#else
#define NOX_STAGE2_THREADS   1u
#endif

/* "SSD is busy" threshold for Bcount, the count of BYTES of in-flight I/O
 * (paper §3.4). 4MB is the paper's evaluation default, on the grounds that a
 * 4MB write nearly saturates the device.
 *
 * DIVERGENCE (spec §3, D1): the paper counts async submit_bio/bi_end_io, so it
 * has many I/Os in flight per thread. We bracket a BLOCKING pread/pwrite, so a
 * thread contributes only its ONE outstanding request. The per-stage ceilings
 * are NOT symmetric, because Stage-2 batches:
 *   Stage-1: 256KB per thread   (one pread of a page's holes, otflush.c:293)
 *   Stage-2: 2MB  per thread    (NOX_PWRITEV_MAX_IOV * 256KB, otflush.c:425)
 * At the default 1+1 pool the ceiling is therefore 256KB + 2MB = 2.25MB, and
 * the threshold cannot be reached. It goes live at TWO Stage-2 threads
 * (2 * 2MB = 4MB) — not at the ~16 an earlier version of this comment claimed
 * by pricing Stage-2 at 256KB and missing the batch.
 *
 * That number matters beyond arithmetic: both ssd_is_busy branches
 * (otflush.c:263 and :390) re-push to the queue TAIL, which reorders two pages
 * of the SAME base. Q1 being FIFO with a single Stage-1 thread is half of the
 * writeback-ordering invariant (otflush.c:68) — reordering there lets an older
 * page be written after a newer one and LOSE that update. So raising
 * NOX_STAGE2_THREADS in C10 arms a dormant correctness bug, and the re-push
 * must become order-preserving (per-base, not a single tail) before the pool
 * grows. bench/otflush_soak.c is the driver that would catch it.
 *
 * The mechanism is implemented faithfully anyway so it exists and can be
 * exercised once that is fixed. */
#define NOX_BCOUNT_BUSY_THRESHOLD (4u << 20)   /* 4 MB */

/* Max iovecs per Stage-2 pwritev: 8 * 256KB = 2MB per syscall (docs/02 §2). */
#define NOX_PWRITEV_MAX_IOV  8u

/* Soft warning threshold on queue depth. Warns on stderr, NEVER blocks — this
 * constant itself enforces nothing.
 *
 * SUPERSEDED IN PART by NOX_WATERMARK_HIGH below, which is set to this same
 * value and DOES block. The two are not redundant: this one counts QUEUE DEPTH
 * and only prints, the watermark counts LIVE PAGES and gates. Because they share
 * a number, the warning now fires at roughly the moment the gate engages, which
 * is the useful reading of it — it went from announcing a threshold nothing acted
 * on to annotating the instant backpressure starts.
 *
 * The original note here said RAM bounding was deferred to C5. That is what
 * C4-B9 built; see NOX_WATERMARK_HIGH for why blocking the foreground does not
 * contradict C4-GATE's "foreground never stalls". */
#define NOX_QUEUE_WARN_DEPTH 4096u

/* C4-B9 (spec §3.6): eviction watermark. The high/low marks of the foreground
 * backpressure gate implemented in watermark.c.
 *
 * THIS IS A MEMORY-PRESSURE FALLBACK, NOT THE FLUSH TRIGGER. Do not conflate it
 * with the Queue-1 enqueue decision: a partial page enters Q1 at CREATION time
 * (paper §3.4), driven by page lifecycle, not by how much RAM is resident. In a
 * healthy run these marks are never reached and the gate costs one counter
 * update per page. They exist only for the pathological case where the device
 * cannot retire pages as fast as the foreground mints them.
 *
 * THE UNIT IS LIVE PAGES, NOT QUEUE DEPTH. A page occupies its 256KB data zone
 * from scrap_page_alloc until scrap_page_free, and for part of that life it is
 * resident in the index and NOT yet in either queue (it is still absorbing
 * merges). Gating on queue depth would therefore undercount exactly the pages
 * that are costing RAM without being anybody's work item. Hence:
 *   4096 pages x 256KB = 1 GiB resident   (high)
 *   3072 pages x 256KB = 768 MiB resident (low)
 *
 * HIGH DELIBERATELY EQUALS NOX_QUEUE_WARN_DEPTH above. That number was already
 * the documented soft cap, it was just never enforced by anything — the warning
 * fired and the engine kept allocating. Reusing it means the stderr warning now
 * marks the instant the gate actually engages instead of announcing a threshold
 * nothing acts on.
 *
 * WHY A BAND AND NOT A SINGLE MARK. With one threshold, a free at `high` wakes a
 * sleeper, that sleeper allocates, the level crosses back up, and the next free
 * wakes it again: a wake/re-sleep storm, one broadcast per allocation, on the
 * foreground path. The band batches releases — one broadcast frees the whole
 * waiting set and gives it a full band of headroom before anyone stalls again.
 *
 * BAND SIZING, from measurements on the bench box (16 threads, swap off, XFS on
 * NVMe): the flusher drains 1935 pages/s, so the 1024-page band is ~0.53 s of
 * drain. That is the FLOOR on how long a stalled foreground thread waits once it
 * parks — shrink the band and you trade stall length for wake frequency. The
 * producer side measured 9585 pages/s against that 1935: a 5.0x imbalance, which
 * is precisely the ratio the backpressure has to impose on the foreground for
 * the run to reach steady state at all.
 *
 * WHY gate-c4's EVIDENCE STILL STANDS: it creates 385 pages against a 4096-page
 * mark, so the gate never engages there and its p99.9 latency numbers are
 * unaffected by this change. bench/otflush_soak.c is the binding case — it peaks
 * near 22400 live pages, ~5.5x over the high mark.
 *
 * These are a defensible STARTING POINT, not a tuned result. Tuning them against
 * the soak's RSS/throughput curve is the C5-rest card, not this one.
 *
 * OVERRIDABLE FROM THE BUILD, same convention as NOX_MAX_ENTRIES_OVERRIDE
 * above, for two uses:
 *
 *   1. Tuning sweeps, once C5-rest gets to them.
 *   2. Building the "before" binary for a before/after measurement. Set the
 *      high mark past any depth the run can reach and the gate is effectively
 *      disarmed while every other line of the engine stays identical:
 *          make soak-c4 NOX_WATERMARK=1000000000
 *      One source tree, one -D apart -- the same discipline that made the C4
 *      eager-zeroing comparison (gate-c4-zero) trustworthy. Do NOT reconstruct
 *      a "before" by checking out an older commit: the rest of the tree moved.
 *
 * The low mark tracks the high one at 3/4 unless overridden separately, so the
 * disarm-by-override case cannot accidentally invert the two and trip the
 * `low < high` assert in nox_watermark_arm. */
#ifdef NOX_WATERMARK_HIGH_OVERRIDE
#define NOX_WATERMARK_HIGH   ((uint32_t)(NOX_WATERMARK_HIGH_OVERRIDE))
#else
#define NOX_WATERMARK_HIGH   4096u
#endif

#ifdef NOX_WATERMARK_LOW_OVERRIDE
#define NOX_WATERMARK_LOW    ((uint32_t)(NOX_WATERMARK_LOW_OVERRIDE))
#else
#define NOX_WATERMARK_LOW    ((NOX_WATERMARK_HIGH) / 4u * 3u)
#endif

/* Writeback ordering guard (otflush.c). A counting array, not an exact set:
 * collisions cost a spurious wait, never a missed ordering constraint. 8192
 * slots x 4B = 32 KB, allocated once with the engine. */
#define NOX_WB_GUARD_BITS    13u
#define NOX_WB_GUARD_SLOTS   (1u << NOX_WB_GUARD_BITS)

/* Additional scrap_header_t.tag states (see NOX_TAG_OPEN / NOX_TAG_FULL). */
#define NOX_TAG_SEALED       2u   /* entry array exhausted: no more merges */
#define NOX_TAG_FLUSHING     3u   /* Stage-2 owns it; detached from the index */

#endif /* NOXDB_CONFIG_H */
