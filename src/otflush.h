/*
 * otflush.h - Opportunistic Two-stage Flushing (spec §5.2; paper §3.4 + Alg. 2).
 *
 * Two queues, two background stages. The foreground never issues an SSD I/O on
 * the scrap path: it copies into the data zone and pushes a POINTER.
 *
 *   Stage-1 (reads):  pop Q1 -> full? forward to Q2 -> else fill holes -> Q2
 *   Stage-2 (writes): pop Q2 -> detach from index -> pwrite/pwritev -> free
 *
 * INVARIANT (spec §4.4): a page is in AT MOST ONE queue at a time. Stage-2 is
 * the only site that frees a page, and only after detaching it, so at free time
 * the page is unreachable from both the index and both queues.
 */
#ifndef OTFLUSH_H
#define OTFLUSH_H

#include "page_index.h"
#include "scrap_page.h"

typedef struct otflush otflush_t;

/* Create both queues and spawn both pools. Returns NULL on failure. */
otflush_t *otflush_start(page_index_t *idx, int fd);

/*
 * Queue a page that still has holes -> Queue-1 (paper §3.4: "whenever an
 * unfilled page is generated, the scrap buffer inserts it to Queue-1").
 * The page STAYS in the index and keeps absorbing foreground writes while it
 * waits — that is the whole point of the immediate enqueue.
 * CALLER MUST HOLD p->lock.
 */
void otflush_enqueue_partial(otflush_t *o, scrap_page_t *p);

/*
 * Queue a page whose data zone is fully covered -> Queue-2, skipping Stage-1
 * entirely (no holes => nothing to read). If the page is ALREADY queued it is
 * left where it is (single-membership, spec §4.4); Stage-1 will notice it is
 * full and forward it to Q2 without issuing a read.
 * CALLER MUST HOLD p->lock.
 */
void otflush_enqueue_full(otflush_t *o, scrap_page_t *p);

/* Block until every enqueued page has been written back and reclaimed.
 * Returns 0, or -1 if any background I/O has failed since startup. */
int otflush_drain(otflush_t *o);

/* Drain, stop both pools, join them, destroy everything.
 * Returns 0, or -1 if any background I/O failed. */
int otflush_stop(otflush_t *o);

#endif /* OTFLUSH_H */
