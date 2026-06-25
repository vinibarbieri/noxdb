/*
 * wsbuffer.h - Public API for the WSBuffer storage engine (MVP, write-only).
 *
 * The engine routes user writes either straight to the SSD (large 4K-aligned
 * writes, via O_DIRECT) or into an in-RAM scrap buffer of 256KB pages that are
 * flushed to the SSD once full. See docs/01_architecture_wsbuffer.md.
 */
#ifndef WSBUFFER_H
#define WSBUFFER_H

#include <stddef.h>
#include <stdint.h>

/* Opaque engine handle. Definition lives in src/wsbuffer.c. */
typedef struct wsb_engine wsb_engine_t;

/*
 * Open (creating if needed) the backing file at `path` with O_DIRECT and return
 * an engine handle, or NULL on error (errno set).
 */
wsb_engine_t *wsb_open(const char *path);

/*
 * Route a write of `size` bytes from `buf` to logical file `offset`.
 * Returns 0 on success, -1 on error (errno set; an O_DIRECT alignment violation
 * is also reported loudly on stderr).
 */
int wsb_write(wsb_engine_t *e, const void *buf, size_t size, uint64_t offset);

/*
 * Flush every remaining (partial) scrap page to the SSD, release all memory,
 * and close the backing file. Returns 0 on success, -1 if any flush failed.
 */
int wsb_close(wsb_engine_t *e);

#endif /* WSBUFFER_H */
