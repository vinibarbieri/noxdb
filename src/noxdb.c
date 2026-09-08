/*
 * noxdb.c - Engine lifecycle and the write_data router (docs/01 §3).
 *
 * C4: the scrap path is fully asynchronous. The foreground copies into a
 * page's data zone, updates the header entries, and hands a POINTER to
 * OTflush's queues — it never issues a pread/pwrite itself. Both background
 * stages (hole-fill, writeback) run on OTflush's own threads (otflush.c).
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"
#include "io_direct.h"
#include "page_index.h"
#include "scrap_page.h"
#include "otflush.h"
#include "nox_stats.h"
#include "watermark.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct nox_engine {
    int           fd;
    page_index_t *idx;      /* hash: page-base -> resident scrap page */
    otflush_t    *ot;       /* background two-stage flusher (C4) */
    uint16_t      ssd_id;   /* single SSD in the MVP */
};

nox_engine_t *nox_open(const char *path)
{
    nox_engine_t *e = malloc(sizeof(*e));
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

    /* Start the flush threads LAST: they reference e->idx and e->fd. */
    e->ot = otflush_start(e->idx, e->fd);
    if (!e->ot) {
        page_index_destroy(e->idx);
        close(e->fd);
        free(e);
        return NULL;
    }

    /* Arm the backpressure gate only once the flusher is actually running
     * (C4-B9). The gate is released exclusively by page frees, and only OTflush
     * frees pages, so arming before otflush_start would open a window in which a
     * foreground thread could park with nothing alive to wake it. */
    nox_watermark_arm(NOX_WATERMARK_HIGH, NOX_WATERMARK_LOW);
    return e;
}

/*
 * Route one chunk that lies entirely within a single 256KB page into the scrap
 * buffer. NO DISK I/O HAPPENS HERE — pages are handed to OTflush, which flushes
 * them on background threads (docs/01 §4). Returns 0 on success, -1 on error.
 */
static int scrap_write_chunk(nox_engine_t *e, uint64_t base, uint32_t intra,
                             const void *buf, uint32_t len)
{
    for (;;) {
        /* BACKPRESSURE ADMISSION POINT (C4-B9). This placement is load-bearing:
         * here the caller holds NO lock — not this base's page lock, not an
         * index shard lock — so a thread parked on the gate blocks nothing that
         * the flusher needs in order to retire pages and release it.
         *
         * It CANNOT move below page_index_get_or_create: that function returns
         * with p->lock HELD (see page_index.h), and Stage-1/Stage-2 take that
         * same page lock to flush and free the page. Waiting while holding it
         * would park the foreground on a condition only the flusher can satisfy,
         * while holding the very lock the flusher needs — a hard deadlock, not a
         * slow path. Same argument applies to the retry iterations: both `continue`
         * paths below unlock before looping, so we re-enter here lock-free.
         *
         * No-op while the level is under the high mark, which is every run that
         * is not memory-starved. */
        nox_watermark_wait();

        int created;
        /* Lock coupling in the index closes the old get-then-lock window (C3
         * debt); see the contract block in page_index.h. Returns p->lock HELD. */
        scrap_page_t *p = page_index_get_or_create(e->idx, base, e->ssd_id,
                                                   &created);
        if (!p) {
            errno = ENOMEM;
            return -1;
        }
        if (created)
            nox_stat_page_created();

        /* A SEALED page has exhausted its 15 header entries and is on its way
         * out; it accepts no more merges. Detach it so the next lookup builds a
         * fresh page, and retry. It is already queued, so it drains on its own. */
        if (p->hdr.tag == NOX_TAG_SEALED) {
            pthread_mutex_unlock(&p->lock);
            page_index_detach_if(e->idx, base, p);
            continue;
        }

        scrap_status_t st = scrap_page_merge(p, buf, intra, len);

        if (st == SCRAP_OVERFLOW) {
            /* No room for another disjoint segment in the entry array.
             * Seal it, detach it, and retry on a fresh page. The retry merges a
             * single segment and therefore cannot overflow.
             * We do NOT flush inline here — that was the C2/C3 stand-in and is
             * exactly the SSD stall C4 exists to remove. The page is already in
             * a queue (it was enqueued when created), so OTflush drains it. */
            /* The ONE place the header size controls an eviction: this page is
             * leaving with however little data it holds, purely because the
             * entry array ran out. WSBuffer reports this happening in under 5%
             * of pages on its workloads (paper §3.2). Counting it is how we find
             * out whether that holds here. */
            nox_stat_seal_entries();
            p->hdr.tag = NOX_TAG_SEALED;
            otflush_enqueue_partial(e->ot, p);  /* no-op if already queued */
            pthread_mutex_unlock(&p->lock);
            page_index_detach_if(e->idx, base, p);
            continue;
        }

        if (scrap_page_is_full(p)) {
            /* Fully assembled: straight to Q2, skipping Stage-1 entirely. No
             * holes means no read-before-write at all — the asymmetry win
             * (docs/03 §3). Eviction by CAPACITY, which the header size does not
             * control — the design working as intended. */
            nox_stat_page_full(p->hdr.number);
            otflush_enqueue_full(e->ot, p);
        } else if (created) {
            /* Paper §3.4: "whenever an unfilled page is generated, the scrap
             * buffer inserts it to Queue-1" — IMMEDIATELY, not at a watermark.
             * It stays in the index and keeps absorbing writes while it waits;
             * Stage-1 only fills whatever holes remain when it is popped. */
            otflush_enqueue_partial(e->ot, p);
        }

        pthread_mutex_unlock(&p->lock);
        return 0;
    }
}

int nox_write(nox_engine_t *e, const void *buf, size_t size, uint64_t offset)
{
    if (size == 0)
        return 0;

    /* Fast path (docs/01 §3): big AND 4K-aligned in both size and offset -> skip
     * the scrap buffer entirely and stream to the SSD via O_DIRECT. This is what
     * removes XArray lock contention and exploits SSD parallelism (docs/03 §3). */
    if (size >= NOX_DIRECT_THRESHOLD &&
        (size % NOX_BLOCK_SIZE) == 0 &&
        (offset % NOX_BLOCK_SIZE) == 0) {
        ssize_t w = io_direct_pwrite(e->fd, buf, size, (off_t)offset);
        return (w == (ssize_t)size) ? 0 : -1;
    }

    /* Scrap path: small or unaligned. Split across 256KB page boundaries since a
     * single user write may straddle two pages.
     *
     * Only scrap-path bytes are counted for amplification: the fast path above
     * writes the user's buffer straight through at 1.00x by construction, so
     * folding it in would dilute the number we are trying to measure. */
    nox_stat_user_bytes(size);

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t cur = offset;
    size_t remaining = size;

    while (remaining > 0) {
        uint64_t base  = NOX_PAGE_BASE(cur);
        uint32_t intra = (uint32_t)(cur - base);
        uint32_t chunk = NOX_DATAZONE_SIZE - intra;     /* room left in this page */
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

/* foreach callback: hand every still-resident page to OTflush at shutdown. */
static void enqueue_one(scrap_page_t *p, void *ctx)
{
    otflush_t *ot = ctx;
    pthread_mutex_lock(&p->lock);
    if (scrap_page_is_full(p))
        otflush_enqueue_full(ot, p);
    else
        otflush_enqueue_partial(ot, p);
    pthread_mutex_unlock(&p->lock);
}

int nox_close(nox_engine_t *e)
{
    if (!e)
        return 0;

    /* CONTRACT: when nox_close returns 0, every byte the caller wrote has been
     * issued to the SSD. bench/scrap_integrity_test.c's read-back memcmp relies
     * on this. Note "issued via O_DIRECT", NOT "fsync'd" — nox_fsync is C6.
     *
     * ORDER MATTERS. page_index_foreach walks the bucket chains with NO locks
     * (by design — see page_index.h), so it must not run while a Stage-2 thread
     * is detaching pages and mutating those same chains. So:
     *   1. drain: after this, pending == 0, both queues are empty, and no
     *      background thread is touching the index.
     *   2. sweep: hand over anything still resident (pages that were never
     *      queued). All caller threads have finished by definition — a thread
     *      cannot be inside nox_write and nox_close at once.
     *   3. drain again, then stop and join. */
    otflush_t *ot = e->ot;

    /* DISARM FIRST, before any drain (C4-B9). The gate's only release mechanism
     * is a page free, and only the flusher frees pages; once shutdown starts,
     * that source is going away. A foreground thread still parked at the high
     * mark would then have nobody left to wake it and would hang forever, taking
     * the join in otflush_stop down with it. disarm() is permanent: it wakes
     * every waiter and turns all subsequent wait() calls into no-ops, so a late
     * write racing shutdown passes straight through instead of blocking. */
    nox_watermark_disarm();

    int err = otflush_drain(ot);            /* 1 */
    page_index_foreach(e->idx, enqueue_one, ot);  /* 2 */
    if (otflush_stop(ot) != 0)              /* 3: drains, then joins */
        err = -1;

    /* Every page OTflush handled was detached and freed by Stage-2. Anything
     * still in the index is a page it never saw; destroy() frees those. */
    page_index_destroy(e->idx);
    if (close(e->fd) != 0)
        err = -1;
    free(e);

    /* After every background thread has joined, so the relaxed counters are
     * stable without needing any ordering of their own. No-op unless -DNOX_STATS. */
    nox_stats_dump(stderr);
    return err;
}
