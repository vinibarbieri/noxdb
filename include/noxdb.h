/*
 * noxdb.h - Public API for the NoxDB storage engine (MVP, write-only).
 *
 * The engine routes user writes either straight to the SSD (large 4K-aligned
 * writes, via O_DIRECT) or into an in-RAM scrap buffer of 256KB pages that are
 * flushed to the SSD once full. See docs/01_architecture_noxdb.md.
 *
 * THREAD-SAFETY (MVP limitation, lifted in C5)
 * -------------------------------------------
 * nox_write() may be called concurrently from many threads ONLY IF those threads
 * write to DISJOINT 256KB-aligned regions — i.e. no two threads may touch the
 * same page base (NOX_PAGE_BASE(offset)) at the same time. The page index is
 * sharded and every page carries its own lock, but a page evicted by one thread
 * is freed while another thread may still hold a pointer to it, so overlapping
 * writers are a use-after-free, not just a lost update.
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
 * Concurrency: safe from multiple threads only under the disjoint-page-base
 * precondition documented at the top of this header.
 */
int nox_write(nox_engine_t *e, const void *buf, size_t size, uint64_t offset);

/*
 * Flush every remaining (partial) scrap page to the SSD, release all memory,
 * and close the backing file. Returns 0 on success, -1 if any flush failed.
 */
int nox_close(nox_engine_t *e);

#endif /* NOXDB_H */
