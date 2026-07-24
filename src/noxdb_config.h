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

#endif /* NOXDB_CONFIG_H */
