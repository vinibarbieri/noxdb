/*
 * page_index.h - Hash table mapping a 256KB-aligned file offset to its live
 * scrap page. Keyed by NOX_PAGE_BASE(offset). Separate chaining via
 * scrap_page_t.next. Bucket lists are guarded by SHARDED locks (docs/01 §5):
 * get_or_create/remove lock only the target shard, so concurrent writers on
 * different pages don't serialize. Locking is internal; signatures are
 * unchanged. (Index structure not specified by docs; chosen for the MVP because
 * it is sparse and tolerates large/random offsets.)
 *
 * THREAD-SAFETY CONTRACT (READ BEFORE CALLING)
 * -------------------------------------------------------------------------
 * LOCK ORDER, GLOBALLY: shard lock -> page lock. No path takes them in the
 * other order, so there is no inversion.
 *
 * page_index_get_or_create() returns with the page's OWN lock already HELD. It
 * acquires that lock before releasing the shard lock (lock coupling), which is
 * what makes the returned pointer safe: a concurrent detach cannot slip in
 * between the lookup and the caller's first use. The caller MUST unlock.
 *
 * This closes the C3-era "get-then-lock" window, where the shard lock was
 * dropped before the caller could take the page lock and a concurrent
 * page_index_remove() could free the page underneath it.
 *
 * REMAINING LIMITATION (lifted in C5): two threads writing the SAME 256KB base
 * concurrently can still LOSE AN UPDATE — the page may be detached and flushed
 * between one thread's write and another's. That is a correctness limit on
 * write ordering, no longer a use-after-free. The C5 tag=FLUSHING pointer swap
 * is the general fix.
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
 * RETURNS WITH p->lock HELD. The caller must pthread_mutex_unlock(&p->lock).
 */
scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created);

/*
 * Detach and free the page covering `base`, if present. FREES the page — any
 * pointer another thread obtained from get_or_create for this same base becomes
 * dangling (see the disjoint-base precondition at the top of this header).
 */
void page_index_remove(page_index_t *idx, uint64_t base);

/*
 * Unlink the page covering `base` ONLY IF the index still maps that base to
 * `expect`. Does NOT free — ownership transfers to the caller, which is OTflush
 * Stage-2 (spec §4.2). Returns 1 if unlinked, 0 if the base was absent or now
 * maps to a different page.
 *
 * The identity check matters: between Stage-2 popping a page and reaching this
 * call, the foreground may have detached the old page and installed a fresh one
 * for the same base. Detaching by base alone would remove the wrong page.
 */
int page_index_detach_if(page_index_t *idx, uint64_t base, scrap_page_t *expect);

/* Visit every live page (used by shutdown to flush all partials).
 * NOT thread-safe by design: takes no locks and assumes all writer threads have
 * already joined (only nox_close calls it). */
void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx);

#endif /* PAGE_INDEX_H */
