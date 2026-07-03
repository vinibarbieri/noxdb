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
 * The 128-byte header. Field ORDER is deliberate: placing the uint16_t ssd_id
 * right after the uint32_t counter (so it lands on an even, naturally-aligned
 * offset) lets the four scalar fields pack into exactly 8 bytes with NO padding.
 * 8B scalars + 15*8B entries = 128B. The static assert below enforces this.
 */
typedef struct {
    uint32_t      counter;                 /* off 0: total valid bytes in page */
    uint16_t      ssd_id;                  /* off 4: underlying SSD id */
    uint8_t       number;                  /* off 6: count of valid data-segments */
    uint8_t       tag;                     /* off 7: flush state (NOX_TAG_*) */
    scrap_entry_t entries[NOX_MAX_ENTRIES];/* off 8: 15*8 = 120B */
} scrap_header_t;                          /* total = 128B */

_Static_assert(sizeof(scrap_header_t) == NOX_HEADER_SIZE,
               "scrap_header_t must be exactly 128 bytes (docs/01 §1)");

/*
 * A live scrap page. `data` is a SEPARATE 4K-aligned 256KB allocation, never
 * inlined with the header, so its address satisfies O_DIRECT. (docs/01 §2)
 */
typedef struct scrap_page {
    scrap_header_t     hdr;
    uint8_t           *data;   /* posix_memalign(4096, 256KB) data zone */
    uint64_t           base;   /* 256KB-aligned file offset this page covers */
    pthread_mutex_t    lock;   /* per-page lock (docs/01 §5); trivial in MVP */
    struct scrap_page *next;   /* hash-bucket chain link */
} scrap_page_t;

typedef enum {
    SCRAP_OK = 0,
    SCRAP_OVERFLOW = 1   /* merge would exceed the 15-entry limit */
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
 * Flush the page to the SSD synchronously.
 *  - Full page  : pwrite the whole 256KB data zone (no read needed).
 *  - Partial page: read-before-write — pread the region, overlay valid segments,
 *    then pwrite the whole 256KB. This is the synchronous stand-in for the
 *    asynchronous OTflush Stage-1/Stage-2 split (docs/01 §4).
 * Returns 0 on success, -1 on I/O error.
 */
int scrap_page_flush(scrap_page_t *p, int fd);

#endif /* SCRAP_PAGE_H */
