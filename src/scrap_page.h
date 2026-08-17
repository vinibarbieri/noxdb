/*
 * scrap_page.h - The scrap_page_t data structure and its operations.
 *
 * A scrap page absorbs small / unaligned writes that target a single 256KB
 * region of the file, mitigating the SSD read-before-write penalty (docs/03 §3).
 * Layout is dictated exactly by docs/01 §2.
 */
#ifndef SCRAP_PAGE_H
#define SCRAP_PAGE_H

#include <pthread.h>
#include <stdint.h>

#include "noxdb_config.h"

/*
 * One index entry: a valid data-segment inside the data zone.
 * 4B intra-page offset + 4B size = 8B. (docs/01 §2)
 */
typedef struct {
    uint32_t offset;   /* start within the 256KB data zone (0 .. 256K-1) */
    uint32_t size;     /* segment length in bytes */
} scrap_entry_t;

/*
 * The header. Field ORDER is deliberate: placing the uint16_t ssd_id right
 * after the uint32_t counter (so it lands on an even, naturally-aligned offset)
 * lets the four scalar fields pack into exactly 8 bytes with NO padding, making
 * the size 8B + NOX_MAX_ENTRIES*8B exactly. The static assert below enforces
 * it. No literal size is written here on purpose: the entry count is tunable,
 * so any number in a comment is a number free to drift. (docs/01 §2)
 */
typedef struct {
    uint32_t      counter;                 /* off 0: total valid bytes in page */
    uint16_t      ssd_id;                  /* off 4: underlying SSD id */
    uint8_t       number;                  /* off 6: count of valid data-segments */
    uint8_t       tag;                     /* off 7: flush state (NOX_TAG_*) */
    scrap_entry_t entries[NOX_MAX_ENTRIES];/* off 8: NOX_MAX_ENTRIES * 8B */
} scrap_header_t;                          /* total = NOX_HEADER_SIZE */

_Static_assert(sizeof(scrap_header_t) == NOX_HEADER_SIZE,
               "scrap_header_t must be exactly 8B of scalars + NOX_MAX_ENTRIES*8B "
               "with no padding (docs/01 §1; 520B at this engine's 64 entries)");

/* The ceiling is WSBuffer's, not ours: `number` is one byte, so a page can hold
 * at most 255 data-segments. Past this the paper's header layout changes. */
_Static_assert(NOX_MAX_ENTRIES >= 1 && NOX_MAX_ENTRIES <= 255,
               "NOX_MAX_ENTRIES must fit in the 1-byte hdr.number field");

/*
 * A live scrap page. `data` is a SEPARATE 4K-aligned 256KB allocation, never
 * inlined with the header, so its address satisfies O_DIRECT. (docs/01 §2)
 */
typedef struct scrap_page {
    scrap_header_t     hdr;
    uint8_t           *data;   /* posix_memalign(4096, 256KB) data zone */
    uint64_t           base;   /* 256KB-aligned file offset this page covers */
    pthread_mutex_t    lock;   /* per-page lock (docs/01 §5); trivial in MVP */
    struct scrap_page *next;   /* hash-bucket chain link (page_index) */

    /* --- OTflush (C4) ------------------------------------------------------
     * These live OUTSIDE scrap_header_t on purpose: the header is pinned at
     * exactly NOX_HEADER_SIZE by the _Static_assert above, and it is written to
     * disk-adjacent structures. Queue state is pure RAM bookkeeping.
     *
     * qnext is a single link because of the single-membership invariant (spec
     * §4.4): a page is in AT MOST ONE queue at a time. Dual membership would be
     * a use-after-free — Stage-2 frees the page while the other queue still
     * holds the pointer. Both flags are written only under `lock`. */
    struct scrap_page *qnext;    /* intrusive link, valid only while in_queue */
    int                in_queue; /* 1 while sitting in Q1 or Q2 */
    int                detached; /* 1 once removed from the page index */
    int                ssd_id_seen; /* test-only: times this page was popped */
} scrap_page_t;

typedef enum {
    SCRAP_OK = 0,
    SCRAP_OVERFLOW = 1   /* merge would exceed NOX_MAX_ENTRIES entries */
} scrap_status_t;

/* Allocate a zeroed page covering `base`. Returns NULL on OOM. */
scrap_page_t *scrap_page_alloc(uint64_t base, uint16_t ssd_id);

/* Free the data zone, destroy the lock, free the struct. */
void scrap_page_free(scrap_page_t *p);

/*
 * Merge `len` bytes from `buf` into the page at intra-page offset `intra_off`.
 * Copies the bytes into the data zone and updates the coalesced index entries,
 * counter and number. Returns SCRAP_OVERFLOW (without modifying entries) if the
 * coalesced segment count would exceed NOX_MAX_ENTRIES.
 */
scrap_status_t scrap_page_merge(scrap_page_t *p, const void *buf,
                                uint32_t intra_off, uint32_t len);

/* True once every byte of the data zone is covered by valid segments. */
int scrap_page_is_full(const scrap_page_t *p);

/*
 * Enumerate the page's HOLES — the complement of the valid segments within
 * [0, 256KB). Writes at most `max` ranges into `out` and returns how many.
 * A full page yields 0. An empty page yields 1 range covering the whole zone.
 * `out` must have room for NOX_MAX_ENTRIES + 1 ranges (n segments => n+1 holes).
 */
uint32_t scrap_page_hole_ranges(const scrap_page_t *p, scrap_entry_t *out,
                                uint32_t max);

/*
 * OTflush Stage-1, split in two halves. The split exists so that NO LOCK IS
 * HELD ACROSS THE pread: a foreground thread that touches this page while
 * Stage-1 is reading would otherwise block on the SSD, and it would do so while
 * holding the index shard mutex (page_index_get_or_create takes p->lock before
 * releasing the shard lock), stalling every other page in that shard too. That
 * is precisely the millisecond spike C4-GATE G4 exists to catch.
 *
 * Usage from Stage-1:
 *      lock(p); nh = scrap_page_hole_ranges(p, holes, ...); unlock(p);
 *      scrap_page_read_holes(p->base, fd, holes, nh, scratch);   // no lock
 *      lock(p); scrap_page_apply_holes(p, scratch); unlock(p);
 *
 * `scratch` is a 4096-aligned NOX_DATAZONE_SIZE buffer indexed by intra-page
 * offset: read_holes deposits each (block-widened) hole read at its own offset,
 * and apply_holes copies back only the bytes that are STILL holes.
 *
 * Re-deriving the hole list under the lock in apply_holes is what makes the
 * unlocked read safe. Segments only ever grow (coalesce_insert never shrinks
 * coverage), so the hole set only shrinks: every hole seen by apply_holes is a
 * subset of one seen by hole_ranges, hence its bytes were definitely read. A
 * foreground merge that landed mid-read therefore wins, instead of being
 * clobbered by older disk contents.
 *
 * `bytes_read` (may be NULL) receives the total the reads actually asked the
 * device for — the BLOCK-WIDENED total, not the sum of hole sizes, because
 * widening to 4K boundaries is real traffic the SSD serves. It is reported here
 * rather than recomputed by the caller so the widening arithmetic lives in
 * exactly one place and the two can never drift apart.
 *
 * Widened ranges that touch or overlap are COALESCED into a single pread, so a
 * 4K block straddled by two holes is fetched once, not twice. Merging never
 * spans a real gap, so the reads cover exactly the union of the widened holes:
 * `bytes_read` is therefore always <= NOX_DATAZONE_SIZE, and the number of
 * preads is at most the 64 blocks of the zone regardless of NOX_MAX_ENTRIES.
 */
int  scrap_page_read_holes(uint64_t base, int fd, const scrap_entry_t *holes,
                           uint32_t nh, void *scratch, uint64_t *bytes_read);
void scrap_page_apply_holes(scrap_page_t *p, const void *scratch);

/*
 * Convenience wrapper: hole_ranges + read_holes + apply_holes in one call, with
 * its own scratch allocation. Single-threaded callers and tests only — Stage-1
 * must use the split form above so it does not hold p->lock across the pread.
 *
 * A hole means "the user did not write here", so the bytes on disk must survive
 * the writeback. Without this, Stage-2 would write zeros over valid data
 * (docs/00_flow_summary.md:79). A FULL page has no holes and returns
 * immediately without issuing a single pread — the read/write asymmetry win
 * (docs/03 §3).
 * Returns 0 on success, -1 on I/O error.
 */
int scrap_page_fill_holes(scrap_page_t *p, int fd);

/*
 * OTflush Stage-2: pwrite the whole 256KB data zone at p->base.
 * Alignment holds by construction: base is a multiple of 256KB (=> of 4096),
 * length is 256KB, and the data zone came from posix_memalign.
 * PRECONDITION: scrap_page_fill_holes() has succeeded on this page, or the page
 * is full. Writing a page with unfilled holes CORRUPTS the file.
 * Returns 0 on success, -1 on I/O error.
 */
int scrap_page_writeback(scrap_page_t *p, int fd);

#endif /* SCRAP_PAGE_H */
