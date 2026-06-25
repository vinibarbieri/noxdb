/*
 * wsbuffer.c - Engine lifecycle and the write_data router (docs/01 §3).
 *
 * MVP: the scrap path is synchronous (a full page is flushed inline, a partial
 * page is flushed at shutdown via read-before-write). Background OTflush threads
 * and real lock contention handling are deferred to a later phase.
 */
#define _GNU_SOURCE
#include "wsbuffer.h"
#include "wsbuffer_config.h"
#include "io_direct.h"
#include "page_index.h"
#include "scrap_page.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

struct wsb_engine {
    int           fd;
    page_index_t *idx;
    uint16_t      ssd_id;   /* single SSD in the MVP */
};

wsb_engine_t *wsb_open(const char *path)
{
    wsb_engine_t *e = malloc(sizeof(*e));
    if (!e)
        return NULL;

    e->fd = io_direct_open(path);
    if (e->fd < 0) {
        free(e);
        return NULL;
    }

    e->idx = page_index_create();
    if (!e->idx) {
        close(e->fd);
        free(e);
        return NULL;
    }

    e->ssd_id = 0;
    return e;
}

/*
 * Route one chunk that lies entirely within a single 256KB page into the scrap
 * buffer. Handles the entry-overflow case by flushing the current page and
 * starting a fresh one (an MVP simplification of OTflush eviction).
 * Returns 0 on success, -1 on I/O error.
 */
static int scrap_write_chunk(wsb_engine_t *e, uint64_t base, uint32_t intra,
                             const void *buf, uint32_t len)
{
    int created;
    scrap_page_t *p = page_index_get_or_create(e->idx, base, e->ssd_id, &created);
    if (!p) {
        errno = ENOMEM;
        return -1;
    }

    pthread_mutex_lock(&p->lock);
    scrap_status_t st = scrap_page_merge(p, buf, intra, len);

    if (st == SCRAP_OVERFLOW) {
        /* No room for another disjoint segment in the 15-entry header. Flush the
         * page out, drop it, and re-create an empty page for this chunk. The
         * retry merges a single segment and therefore cannot overflow. */
        pthread_mutex_unlock(&p->lock);
        if (scrap_page_flush(p, e->fd) != 0)
            return -1;
        page_index_remove(e->idx, base);

        p = page_index_get_or_create(e->idx, base, e->ssd_id, &created);
        if (!p) {
            errno = ENOMEM;
            return -1;
        }
        pthread_mutex_lock(&p->lock);
        (void)scrap_page_merge(p, buf, intra, len); /* fits: single segment */
    }

    int full = scrap_page_is_full(p);
    pthread_mutex_unlock(&p->lock);

    /* A fully-assembled 256KB page goes straight to the SSD and is reclaimed.
     * (In the bg phase this enqueues to OTflush Stage-2 instead of flushing.) */
    if (full) {
        if (scrap_page_flush(p, e->fd) != 0)
            return -1;
        page_index_remove(e->idx, base);
    }
    return 0;
}

int wsb_write(wsb_engine_t *e, const void *buf, size_t size, uint64_t offset)
{
    if (size == 0)
        return 0;

    /* Fast path (docs/01 §3): big AND 4K-aligned in both size and offset -> skip
     * the scrap buffer entirely and stream to the SSD via O_DIRECT. This is what
     * removes XArray lock contention and exploits SSD parallelism (docs/03 §3). */
    if (size >= WSB_DIRECT_THRESHOLD &&
        (size % WSB_BLOCK_SIZE) == 0 &&
        (offset % WSB_BLOCK_SIZE) == 0) {
        ssize_t w = io_direct_pwrite(e->fd, buf, size, (off_t)offset);
        return (w == (ssize_t)size) ? 0 : -1;
    }

    /* Scrap path: small or unaligned. Split across 256KB page boundaries since a
     * single user write may straddle two pages. */
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t cur = offset;
    size_t remaining = size;

    while (remaining > 0) {
        uint64_t base  = WSB_PAGE_BASE(cur);
        uint32_t intra = (uint32_t)(cur - base);
        uint32_t chunk = WSB_DATAZONE_SIZE - intra;     /* room left in this page */
        if (chunk > remaining)
            chunk = (uint32_t)remaining;

        if (scrap_write_chunk(e, base, intra, src, chunk) != 0)
            return -1;

        src       += chunk;
        cur       += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* foreach callback: flush one remaining (partial) page at shutdown. */
struct flush_ctx { int fd; int err; };
static void flush_one(scrap_page_t *p, void *ctx)
{
    struct flush_ctx *c = ctx;
    if (scrap_page_flush(p, c->fd) != 0)
        c->err = -1;
}

int wsb_close(wsb_engine_t *e)
{
    if (!e)
        return 0;

    /* Every page still in the index is partial (full pages were flushed inline).
     * Read-before-write flush them all, then tear everything down. */
    struct flush_ctx c = { .fd = e->fd, .err = 0 };
    page_index_foreach(e->idx, flush_one, &c);

    page_index_destroy(e->idx);
    if (close(e->fd) != 0)
        c.err = -1;
    free(e);
    return c.err;
}
