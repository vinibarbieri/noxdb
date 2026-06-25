/*
 * scrap_page.c - scrap_page_t lifecycle, segment merge, and synchronous flush.
 */
#define _GNU_SOURCE
#include "scrap_page.h"
#include "io_direct.h"

#include <stdlib.h>
#include <string.h>

scrap_page_t *scrap_page_alloc(uint64_t base, uint16_t ssd_id)
{
    scrap_page_t *p = malloc(sizeof(*p));
    if (!p)
        return NULL;

    /* CRITICAL (docs/01 §2): the data zone is allocated SEPARATELY from the
     * header struct with posix_memalign so its address is 4K-aligned for
     * O_DIRECT. A single malloc of header+data would NOT guarantee alignment. */
    void *zone = NULL;
    if (posix_memalign(&zone, WSB_BLOCK_SIZE, WSB_DATAZONE_SIZE) != 0) {
        free(p);
        return NULL;
    }

    /* Zero the data zone: bytes in "holes" (regions never written by the user)
     * default to 0; they get overwritten by read-before-write at flush time. */
    memset(zone, 0, WSB_DATAZONE_SIZE);

    memset(&p->hdr, 0, sizeof(p->hdr));
    p->hdr.ssd_id = ssd_id;
    p->hdr.tag    = WSB_TAG_OPEN;
    p->data       = zone;
    p->base       = base;
    p->next       = NULL;
    pthread_mutex_init(&p->lock, NULL);
    return p;
}

void scrap_page_free(scrap_page_t *p)
{
    if (!p)
        return;
    pthread_mutex_destroy(&p->lock);
    free(p->data);
    free(p);
}

int scrap_page_is_full(const scrap_page_t *p)
{
    return p->hdr.counter == WSB_DATAZONE_SIZE;
}

/*
 * Insert [off, off+len) into the page's entry list, coalescing any overlapping
 * or directly-adjacent existing segments so entries stay disjoint and sorted by
 * offset. Recomputes counter and number. The data bytes themselves are copied
 * by the caller (scrap_page_merge) before this runs.
 *
 * Returns SCRAP_OVERFLOW (leaving the header untouched) if the result would
 * need more than WSB_MAX_ENTRIES entries.
 */
static scrap_status_t coalesce_insert(scrap_header_t *h, uint32_t off, uint32_t len)
{
    uint32_t new_off = off;
    uint32_t new_end = off + len;           /* segments never exceed 256KB */

    /* Build the new entry list in a scratch array (room for one extra). */
    scrap_entry_t out[WSB_MAX_ENTRIES + 1];
    uint8_t n = 0;
    uint8_t i = 0;

    /* (1) Copy through segments that end strictly before the new one and are
     *     not adjacent (end < new_off). They cannot merge. */
    while (i < h->number && (h->entries[i].offset + h->entries[i].size) < new_off)
        out[n++] = h->entries[i++];

    /* (2) Absorb every segment that overlaps or touches the new range, growing
     *     [new_off,new_end) to cover them. Adjacency (end == new_off) merges too
     *     because step (1) only skips strictly-before segments. */
    while (i < h->number && h->entries[i].offset <= new_end) {
        uint32_t e_off = h->entries[i].offset;
        uint32_t e_end = e_off + h->entries[i].size;
        if (e_off < new_off) new_off = e_off;
        if (e_end > new_end) new_end = e_end;
        i++;
    }

    /* (3) Emit the merged range. */
    if (n >= WSB_MAX_ENTRIES + 1)
        return SCRAP_OVERFLOW;
    out[n].offset = new_off;
    out[n].size   = new_end - new_off;
    n++;

    /* (4) Copy through the remaining (strictly-after) segments. */
    while (i < h->number) {
        if (n >= WSB_MAX_ENTRIES + 1)
            return SCRAP_OVERFLOW;
        out[n++] = h->entries[i++];
    }

    if (n > WSB_MAX_ENTRIES)
        return SCRAP_OVERFLOW;

    /* Commit: copy scratch back and recompute counter (sum of disjoint sizes). */
    uint32_t total = 0;
    for (uint8_t k = 0; k < n; k++) {
        h->entries[k] = out[k];
        total += out[k].size;
    }
    h->number  = n;
    h->counter = total;
    return SCRAP_OK;
}

scrap_status_t scrap_page_merge(scrap_page_t *p, const void *buf,
                                uint32_t intra_off, uint32_t len)
{
    /* Probe overflow on a COPY of the header first, so a rejected merge leaves
     * the page (and its data zone) unchanged for the caller to flush + retry. */
    scrap_header_t probe = p->hdr;
    if (coalesce_insert(&probe, intra_off, len) == SCRAP_OVERFLOW)
        return SCRAP_OVERFLOW;

    /* Safe to commit: copy the user bytes into the data zone, then the entries. */
    memcpy(p->data + intra_off, buf, len);
    p->hdr = probe;
    return SCRAP_OK;
}

int scrap_page_flush(scrap_page_t *p, int fd)
{
    if (scrap_page_is_full(p)) {
        /* Full page: every byte is valid user data — write it straight out.
         * base is 256KB-aligned (=> 4K-aligned), len is 256KB, data is aligned. */
        ssize_t w = io_direct_pwrite(fd, p->data, WSB_DATAZONE_SIZE,
                                     (off_t)p->base);
        p->hdr.tag = WSB_TAG_FULL;
        return (w == (ssize_t)WSB_DATAZONE_SIZE) ? 0 : -1;
    }

    /* Partial page: synchronous read-before-write. Read the current 256KB
     * region from the SSD into an aligned scratch (so holes keep their on-disk
     * contents), overlay our valid segments, then write the whole region back.
     * This fuses OTflush Stage-1 (read holes) and Stage-2 (write) (docs/01 §4). */
    void *scratch = NULL;
    if (posix_memalign(&scratch, WSB_BLOCK_SIZE, WSB_DATAZONE_SIZE) != 0)
        return -1;
    memset(scratch, 0, WSB_DATAZONE_SIZE); /* default for region past EOF */

    ssize_t r = io_direct_pread(fd, scratch, WSB_DATAZONE_SIZE, (off_t)p->base);
    if (r < 0) {
        free(scratch);
        return -1; /* short read (r >= 0) is fine; only a hard error aborts */
    }

    /* Overlay valid segments on top of the on-disk image. */
    for (uint8_t k = 0; k < p->hdr.number; k++) {
        uint32_t off = p->hdr.entries[k].offset;
        uint32_t sz  = p->hdr.entries[k].size;
        memcpy((uint8_t *)scratch + off, p->data + off, sz);
    }

    ssize_t w = io_direct_pwrite(fd, scratch, WSB_DATAZONE_SIZE, (off_t)p->base);
    free(scratch);
    p->hdr.tag = WSB_TAG_FULL;
    return (w == (ssize_t)WSB_DATAZONE_SIZE) ? 0 : -1;
}
