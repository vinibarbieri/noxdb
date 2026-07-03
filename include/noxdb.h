/*
 * noxdb.h - Public API for the NoxDB storage engine (MVP, write-only).
 *
 * The engine routes user writes either straight to the SSD (large 4K-aligned
 * writes, via O_DIRECT) or into an in-RAM scrap buffer of 256KB pages that are
 * flushed to the SSD once full. See docs/01_architecture_noxdb.md.
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
 */
int nox_write(nox_engine_t *e, const void *buf, size_t size, uint64_t offset);

/*
 * Flush every remaining (partial) scrap page to the SSD, release all memory,
 * and close the backing file. Returns 0 on success, -1 if any flush failed.
 */
int nox_close(nox_engine_t *e);

#endif /* NOXDB_H */
