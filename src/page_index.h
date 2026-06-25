/*
 * page_index.h - Hash table mapping a 256KB-aligned file offset to its live
 * scrap page. Keyed by WSB_PAGE_BASE(offset). Separate chaining via
 * scrap_page_t.next. (Index structure not specified by docs; chosen for the MVP
 * because it is sparse and tolerates large/random offsets.)
 */
#ifndef PAGE_INDEX_H
#define PAGE_INDEX_H

#include <stdint.h>
#include "scrap_page.h"

typedef struct page_index page_index_t;

/* Create / destroy the index. destroy() frees every page still held. */
page_index_t *page_index_create(void);
void page_index_destroy(page_index_t *idx);

/*
 * Return the page covering `base` (must be 256KB-aligned), allocating it on
 * first touch. If `created` is non-NULL it is set to 1 when a new page was made,
 * 0 when an existing one was returned. Returns NULL on OOM.
 */
scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created);

/* Detach and free the page covering `base`, if present. */
void page_index_remove(page_index_t *idx, uint64_t base);

/* Visit every live page (used by shutdown to flush all partials). */
void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx);

#endif /* PAGE_INDEX_H */
