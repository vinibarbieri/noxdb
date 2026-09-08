/*
 * scrap_page.c - scrap_page_t lifecycle, segment merge, and synchronous flush.
 */
#define _GNU_SOURCE
#include "scrap_page.h"
#include "io_direct.h"
#include "watermark.h"

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
    if (posix_memalign(&zone, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) != 0) {
        free(p);
        return NULL;
    }

    /* DELIBERATELY NOT ZEROED. Measured on the bench box: zeroing here cost
     * 120 us of FOREGROUND latency per page (C4-GATE, 1 thread: 25 ms of write
     * time over 208 pages; p99 = 115683 ns). The 256KB zone is fresh virtual
     * memory, so the memset was touching all 64 of its 4K pages eagerly and
     * paying 64 minor faults synchronously, on the caller's thread. Total for
     * the 8-thread gate: 116075 minor faults, ~51 ms — the entire wall time.
     *
     * It is safe to skip because every byte of the zone is overwritten before
     * it can reach the disk. The union of the two producers is exactly the
     * whole zone, with no gap:
     *   - the entry list covers what the user wrote (scrap_page_merge);
     *   - scrap_page_apply_holes fills the exact COMPLEMENT of that list from
     *     disk, and hole_ranges is defined as that complement.
     * A page that skips Stage-1 does so only when scrap_page_is_full(), i.e.
     * counter == NOX_DATAZONE_SIZE, i.e. the entries alone cover everything.
     * A page whose Stage-1 pread fails is dropped, never written.
     *
     * The faults themselves do not vanish — they move. The foreground now
     * faults only the handful of 4K pages it actually writes into, spread
     * across its writes; Stage-1's apply_holes faults the rest, on a background
     * thread. Moving work off the critical path is the entire premise of C4,
     * so paying for the whole zone up front was working against the cycle.
     *
     * NOX_EAGER_ZERO restores the old behaviour. It exists ONLY to rebuild the
     * "before" binary for the latency comparison (make gate-c4-zero), so the
     * two CDF curves come from one source tree one #ifdef apart instead of from
     * two checkouts. Never define it for a real build. */
#ifdef NOX_EAGER_ZERO
    memset(zone, 0, NOX_DATAZONE_SIZE);
#endif

    memset(&p->hdr, 0, sizeof(p->hdr));
    p->hdr.ssd_id = ssd_id;
    p->hdr.tag    = NOX_TAG_OPEN;
    p->data       = zone;
    p->base       = base;
    p->next       = NULL;
    p->qnext    = NULL;
    p->in_queue = 0;
    p->detached = 0;
    /* Per-page lock (docs/01 §5): guards this page's header+data during merge
     * and flush. Distinct from the index shard locks; the two are never held
     * simultaneously (see scrap_write_chunk), so there is no lock-order risk. */
    pthread_mutex_init(&p->lock, NULL);

    /* C4-B9: the watermark level is maintained HERE, at the two ends of the
     * page lifecycle, and deliberately NOT at the call sites. There is one alloc
     * site but THREE frees (otflush.c, and two in page_index.c), so instrumenting
     * callers means the next free added anywhere silently escapes the accounting
     * and the gate leaks its level upward until it never releases. Counting at
     * the constructor/destructor makes that structurally impossible.
     *
     * Only on the fully successful path: a failed alloc returns NULL above
     * WITHOUT having counted, so a caller that gets NULL owes no free. */
    nox_watermark_note_alloc();
    return p;
}

void scrap_page_free(scrap_page_t *p)
{
    /* Same NULL guard the function already had, so a free(NULL)-style call is
     * still a no-op and cannot decrement a level it never incremented. */
    if (!p)
        return;
    nox_watermark_note_free();
    pthread_mutex_destroy(&p->lock);
    free(p->data);
    free(p);
}

int scrap_page_is_full(const scrap_page_t *p)
{
    return p->hdr.counter == NOX_DATAZONE_SIZE;
}

/*
 * Insert [off, off+len) into the page's entry list, coalescing any overlapping
 * or directly-adjacent existing segments so entries stay disjoint and sorted by
 * offset. Recomputes counter and number. The data bytes themselves are copied
 * by the caller (scrap_page_merge) before this runs.
 *
 * Returns SCRAP_OVERFLOW (leaving the header untouched) if the result would
 * need more than NOX_MAX_ENTRIES entries.
 */
static scrap_status_t coalesce_insert(scrap_header_t *h, uint32_t off, uint32_t len)
{
    uint32_t new_off = off;
    uint32_t new_end = off + len;           /* segments never exceed 256KB */

    /* Build the new entry list in a scratch array (room for one extra).
     *
     * Counters are uint32_t, NOT uint8_t. NOX_MAX_ENTRIES is tunable up to 255
     * (the width of hdr.number), and at 255 a uint8_t counter is a live bug: the
     * guard `n >= NOX_MAX_ENTRIES + 1` compares against 256, which a uint8_t can
     * never reach, so instead of returning SCRAP_OVERFLOW the index wraps to 0
     * and the loop runs forever. Same for `i` against h->number. */
    scrap_entry_t out[NOX_MAX_ENTRIES + 1];
    uint32_t n = 0;
    uint32_t i = 0;

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
    if (n >= NOX_MAX_ENTRIES + 1)
        return SCRAP_OVERFLOW;
    out[n].offset = new_off;
    out[n].size   = new_end - new_off;
    n++;

    /* (4) Copy through the remaining (strictly-after) segments. */
    while (i < h->number) {
        if (n >= NOX_MAX_ENTRIES + 1)
            return SCRAP_OVERFLOW;
        out[n++] = h->entries[i++];
    }

    if (n > NOX_MAX_ENTRIES)
        return SCRAP_OVERFLOW;

    /* Commit: copy scratch back and recompute counter (sum of disjoint sizes). */
    uint32_t total = 0;
    for (uint32_t k = 0; k < n; k++) {
        h->entries[k] = out[k];
        total += out[k].size;
    }
    h->number  = (uint8_t)n;   /* n <= NOX_MAX_ENTRIES <= 255, checked above */
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

uint32_t scrap_page_hole_ranges(const scrap_page_t *p, scrap_entry_t *out,
                                uint32_t max)
{
    uint32_t n      = 0;
    uint32_t cursor = 0;   /* first byte not yet accounted for */

    /* coalesce_insert keeps entries disjoint AND sorted by offset, so a single
     * forward walk yields the complement directly. `k` is uint32_t, not uint8_t:
     * at NOX_MAX_ENTRIES == 255 a uint8_t k wraps to 0 after the last entry and
     * this loop never terminates. */
    for (uint32_t k = 0; k < p->hdr.number; k++) {
        uint32_t seg_off = p->hdr.entries[k].offset;
        if (seg_off > cursor) {
            if (n >= max)
                return n;
            out[n].offset = cursor;
            out[n].size   = seg_off - cursor;
            n++;
        }
        cursor = seg_off + p->hdr.entries[k].size;
    }

    if (cursor < NOX_DATAZONE_SIZE && n < max) {
        out[n].offset = cursor;
        out[n].size   = NOX_DATAZONE_SIZE - cursor;
        n++;
    }
    return n;
}

/*
 * Issue ONE device read for the block-aligned range [off, end) of the zone.
 *
 * The read lands at its own intra-page offset inside the scratch zone, so
 * apply_holes can index scratch exactly like it indexes p->data.
 *
 * Zero first: a short read means the region is past EOF, and those bytes must
 * read back as 0, not as stale scratch contents. This memset is load-bearing
 * CORRECTNESS, not hygiene — the return value `r` is only checked for a hard
 * error, so a short read is silently accepted and the zeroed tail is what makes
 * that safe.
 */
static int read_run(uint64_t base, int fd, uint8_t *dst,
                    uint32_t off, uint32_t end)
{
    uint32_t len = end - off;

    memset(dst + off, 0, len);

    /* pread, never read+lseek: Stage-1 shares the fd with every foreground
     * thread, so a global file offset would be a race (CLAUDE.md §2). */
    ssize_t r = io_direct_pread(fd, dst + off, len, (off_t)(base + off));
    return (r < 0) ? -1 : 0;   /* short read is fine, hard error is not */
}

int scrap_page_read_holes(uint64_t base, int fd, const scrap_entry_t *holes,
                          uint32_t nh, void *scratch, uint64_t *bytes_read)
{
    uint8_t *dst = scratch;
    uint64_t asked = 0;

    /* The open run of block-aligned bytes not yet issued to the device. */
    uint32_t run_off = 0, run_end = 0;
    int have_run = 0;

    for (uint32_t i = 0; i < nh; i++) {
        /* O_DIRECT needs 4K-aligned offset AND length, but the hole itself is
         * arbitrary. Widen the READ outward to block boundaries. The copy back
         * (apply_holes) stays at exact hole width — widening the COPY would
         * clobber the user's valid segments sitting just outside the hole. */
        uint32_t h_off  = holes[i].offset;
        uint32_t h_end  = h_off + holes[i].size;
        uint32_t a_off  = h_off & ~(NOX_BLOCK_SIZE - 1);
        uint32_t a_end  = (h_end + NOX_BLOCK_SIZE - 1) & ~(NOX_BLOCK_SIZE - 1);
        if (a_end > NOX_DATAZONE_SIZE)
            a_end = NOX_DATAZONE_SIZE;   /* zone size is a 4K multiple */

        /* COALESCE THE WIDENED RANGES. hole_ranges returns holes sorted and
         * disjoint, and widening preserves the order — but it can make two
         * neighbours touch or overlap, because a hole ending mid-block and the
         * next hole starting inside that same block both widen onto it. Issuing
         * one pread per hole then sends the SAME 4K block to the device several
         * times. Measured at 255 entries: 954749 B asked per 262144 B zone, a
         * 3.64x read amplification against a zone that is only 64 blocks wide.
         *
         * Merging is pure waste removal, never a widening: a_off <= run_end
         * means the two ranges touch or overlap, so the merged run covers
         * exactly their union and not one byte more. Ranges separated by a real
         * gap still get their own pread. The total asked is therefore bounded by
         * NOX_DATAZONE_SIZE, i.e. read amplification can no longer exceed 1x per
         * Stage-1 pass. Stage-1 is single-threaded, so every syscall and round
         * trip removed here comes straight off the drain time. */
        if (have_run && a_off <= run_end) {
            if (a_end > run_end)
                run_end = a_end;         /* extend the open run, no new pread */
            continue;
        }

        if (have_run) {
            if (read_run(base, fd, dst, run_off, run_end) < 0)
                return -1;
            asked += run_end - run_off;
        }
        run_off  = a_off;
        run_end  = a_end;
        have_run = 1;
    }

    if (have_run) {
        if (read_run(base, fd, dst, run_off, run_end) < 0)
            return -1;
        asked += run_end - run_off;
    }

    if (bytes_read)
        *bytes_read = asked;
    return 0;
}

void scrap_page_apply_holes(scrap_page_t *p, const void *scratch)
{
    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
    uint32_t nh = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);

    /* Re-derived under the caller's lock: see the contract in scrap_page.h.
     * Holes only shrink, so each of these is inside a range read_holes covered. */
    for (uint32_t i = 0; i < nh; i++)
        memcpy(p->data + holes[i].offset,
               (const uint8_t *)scratch + holes[i].offset, holes[i].size);

    /* COLLAPSE THE ENTRY ARRAY. Every byte of the zone is now valid — user data
     * in the segments, disk data in the holes — so the page's true coverage is
     * the single segment [0, 256KB). Recording that is not cosmetic:
     *
     *   1. scrap_page_is_full() tests `counter == NOX_DATAZONE_SIZE`. Leaving
     *      the old fragmented entries makes it report FALSE on a page Stage-1
     *      just completed, forcing every caller to second-guess it with a tag
     *      check.
     *   2. It frees all but one of the entry slots. Without the collapse the page is
     *      stuck at NOX_MAX_ENTRIES forever, so the very next scattered write to
     *      this base overflows, seals the page and allocates another 256KB one.
     *      That is a page-per-15-writes churn rate, and it is what drove RSS to
     *      32 GB in five seconds on the first overlapping-base soak. */
    p->hdr.entries[0].offset = 0;
    p->hdr.entries[0].size   = NOX_DATAZONE_SIZE;
    p->hdr.number            = 1;
    p->hdr.counter           = NOX_DATAZONE_SIZE;
}

int scrap_page_fill_holes(scrap_page_t *p, int fd)
{
    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
    uint32_t nh = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
    if (nh == 0)
        return 0;                     /* full page: no Stage-1 read at all */

    /* posix_memalign (not malloc) because this buffer is a direct O_DIRECT
     * pread target (CLAUDE.md §2). */
    void *scratch = NULL;
    if (posix_memalign(&scratch, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) != 0)
        return -1;

    int rc = scrap_page_read_holes(p->base, fd, holes, nh, scratch, NULL);
    if (rc == 0)
        scrap_page_apply_holes(p, scratch);

    free(scratch);
    return rc;
}

int scrap_page_writeback(scrap_page_t *p, int fd)
{
    /* _all, not the bare pwrite: a partial write must be resumed, not silently
     * accepted as a success (Step 4b). Same policy as the Stage-2 batch. */
    ssize_t w = io_direct_pwrite_all(fd, p->data, NOX_DATAZONE_SIZE,
                                     (off_t)p->base);
    return (w == (ssize_t)NOX_DATAZONE_SIZE) ? 0 : -1;
}
