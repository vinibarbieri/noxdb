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

/* Scrap-page header is exactly 128 bytes. (docs/01 §1, §2) */
#define NOX_HEADER_SIZE      128u

/* Scrap-page data zone is exactly 256 KB. Allocated SEPARATELY from the header
 * via posix_memalign(..., 4096, ...) so its address satisfies O_DIRECT.
 * (docs/01 §1, §2 CRITICAL) */
#define NOX_DATAZONE_SIZE    (256u * 1024u)         /* 262144 */

/* The header carries an array of exactly 15 index entries (8B each). (docs/01 §2) */
#define NOX_MAX_ENTRIES      15u

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
#define NOX_STAGE1_THREADS   1u
#define NOX_STAGE2_THREADS   1u

/* "SSD is busy" threshold for Bcount, the count of BYTES of in-flight I/O
 * (paper §3.4). 4MB is the paper's evaluation default, on the grounds that a
 * 4MB write nearly saturates the device.
 *
 * DIVERGENCE (spec §3, D1): the paper counts async submit_bio/bi_end_io, so it
 * has many I/Os in flight per thread. We bracket a BLOCKING pread/pwrite, so
 * in-flight bytes max out at threads * 256KB = 512KB at the default 1+1 pool.
 * This threshold therefore CANNOT be reached below ~16 background threads. It
 * is implemented faithfully anyway so the mechanism exists and can be exercised
 * by raising the pool size in C10. */
#define NOX_BCOUNT_BUSY_THRESHOLD (4u << 20)   /* 4 MB */

/* Max iovecs per Stage-2 pwritev: 8 * 256KB = 2MB per syscall (docs/02 §2). */
#define NOX_PWRITEV_MAX_IOV  8u

/* Soft warning threshold on queue depth. Warns on stderr, NEVER blocks —
 * blocking would violate C4-GATE's "foreground never stalls". Real RAM bounding
 * is the C5 eviction watermark (spec §3, D5). */
#define NOX_QUEUE_WARN_DEPTH 4096u

/* Writeback ordering guard (otflush.c). A counting array, not an exact set:
 * collisions cost a spurious wait, never a missed ordering constraint. 8192
 * slots x 4B = 32 KB, allocated once with the engine. */
#define NOX_WB_GUARD_BITS    13u
#define NOX_WB_GUARD_SLOTS   (1u << NOX_WB_GUARD_BITS)

/* Additional scrap_header_t.tag states (see NOX_TAG_OPEN / NOX_TAG_FULL). */
#define NOX_TAG_SEALED       2u   /* 15 entries used: no more merges accepted */
#define NOX_TAG_FLUSHING     3u   /* Stage-2 owns it; detached from the index */

#endif /* NOXDB_CONFIG_H */
