/*
 * page_index.h - Hash table mapping a 256KB-aligned file offset to its live
 * scrap page. Keyed by NOX_PAGE_BASE(offset). Separate chaining via
 * scrap_page_t.next. Bucket lists are guarded by SHARDED locks (docs/01 §5):
 * get_or_create/remove lock only the target shard, so concurrent writers on
 * different pages don't serialize. Locking is internal; signatures are
 * unchanged. (Index structure not specified by docs; chosen for the MVP because
 * it is sparse and tolerates large/random offsets.)
 *
 * THREAD-SAFETY CONTRACT (READ BEFORE CALLING — MVP limitation, lifted in C5)
 * -------------------------------------------------------------------------
 * The shard locks protect the BUCKET LISTS ONLY. They do NOT keep a returned
 * page alive: page_index_get_or_create() releases the shard lock before it
 * returns, and page_index_remove() frees its victim. There is no refcount and
 * no hazard pointer, so a scrap_page_t * handed out by get_or_create is only
 * valid while no other thread can remove that same base.
 *
 * CALLER PRECONDITION: concurrent callers MUST operate on DISJOINT bases —
 * every 256KB page base is owned by at most one thread at a time. Violate this
 * and you get a use-after-free (and a pthread_mutex_destroy() on a lock another
 * thread is about to acquire), NOT merely a lost update.
 *
 * The general fix is the C5 tag=FLUSHING pointer-swap, which makes eviction
 * publish a new page instead of freeing the old one under a live reader.
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
 *
 * The returned pointer is UNOWNED: the shard lock is already released on return,
 * so the page stays valid only under the disjoint-base precondition documented
 * at the top of this header. Do not cache it across a possible eviction.
 */
scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created);

/*
 * Detach and free the page covering `base`, if present. FREES the page — any
 * pointer another thread obtained from get_or_create for this same base becomes
 * dangling (see the disjoint-base precondition at the top of this header).
 */
void page_index_remove(page_index_t *idx, uint64_t base);

/* Visit every live page (used by shutdown to flush all partials).
 * NOT thread-safe by design: takes no locks and assumes all writer threads have
 * already joined (only nox_close calls it). */
void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx);

#endif /* PAGE_INDEX_H */
