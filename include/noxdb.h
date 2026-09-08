/*
 * noxdb.h - Public API for the NoxDB storage engine (MVP, write-only).
 *
 * The engine routes user writes either straight to the SSD (large 4K-aligned
 * writes, via O_DIRECT) or into an in-RAM scrap buffer of 256KB pages that are
 * flushed to the SSD once full. See docs/01_architecture_noxdb.md.
 *
 * THREAD-SAFETY
 * -------------
 * nox_write() is safe to call concurrently from many threads, INCLUDING threads
 * writing to the same 256KB page base, PROVIDED no two of them write overlapping
 * BYTE RANGES. What each guarantee rests on:
 *
 *   - No use-after-free. page_index_get_or_create takes the page lock before
 *     releasing the shard lock (lock coupling), so a concurrent detach cannot
 *     free the page between the lookup and the caller's first use. See the
 *     contract block in src/page_index.h. This is a change from the C3-era
 *     contract, which this comment previously still described.
 *
 *   - No lost update across an eviction. A page taken for writeback is marked
 *     tag=FLUSHING and unlinked from the index (otflush.c stage2_detach), so a
 *     writer that arrives afterwards misses and builds a fresh page rather than
 *     writing into one that is in flight.
 *
 *   - Evidence, not just argument: 1200 s, 16 threads over 4 SHARED page bases
 *     with disjoint byte stripes inside each — integrity PASS, TSan clean.
 *     PERFORMANCE.md section 4.3.
 *
 * WHAT IS STILL NOT GUARANTEED
 *
 *   - Overlapping byte ranges. Two threads writing the same bytes of the same
 *     base have no defined outcome and no defined order. The engine writes what
 *     it is told to the offset it is told; choosing offsets is the caller's job.
 *
 *   - Cross-page atomicity. A single nox_write() spanning a 256KB boundary is
 *     split into per-page merges that are not atomic with respect to each other.
 *
 *   - Write ordering under a grown flusher pool. Ordering between two
 *     generations of one base currently rests on Q2 being FIFO with a SINGLE
 *     consumer, not on the writeback guard. Do not raise NOX_STAGE2_THREADS
 *     without reading PERFORMANCE.md section 4.5.1 first.
 *
 * Big 4K-aligned writes (>= NOX_DIRECT_THRESHOLD) take the direct path and never
 * touch the scrap buffer, so they are unrestricted.
 *
 * nox_open() and nox_close() are NOT thread-safe: call them from a single thread,
 * with nox_close() only after every writer thread has joined.
 */
#ifndef NOXDB_H
#define NOXDB_H

#include <stddef.h>
#include <stdint.h>

/* Opaque engine handle. Definition lives in src/noxdb.c. */
typedef struct nox_engine nox_engine_t;

/*
 * Open (creating if needed) the backing file at `path` with O_DIRECT and return
 * an engine handle, or NULL on error (errno set).
 */
nox_engine_t *nox_open(const char *path);

/*
 * Route a write of `size` bytes from `buf` to logical file `offset`.
 * Returns 0 on success, -1 on error (errno set; an O_DIRECT alignment violation
 * is also reported loudly on stderr).
 *
 * Concurrency: safe from multiple threads, including on a shared page base, so
 * long as no two of them write overlapping byte ranges. See the top of this
 * header for what that does and does not guarantee.
 */
int nox_write(nox_engine_t *e, const void *buf, size_t size, uint64_t offset);

/*
 * Flush every remaining (partial) scrap page to the SSD, release all memory,
 * and close the backing file. Returns 0 on success, -1 if any flush failed.
 */
int nox_close(nox_engine_t *e);

#endif /* NOXDB_H */
