/*
 * queue.h - Intrusive MPMC FIFO of scrap pages for OTflush (spec §5.1).
 *
 * Mutex + condvar, UNBOUNDED by design: push never blocks, because C4-GATE
 * requires the foreground to never stall (spec §3, divergence D5). Bounding RAM
 * is the C5 eviction watermark's job.
 *
 * The link is intrusive (scrap_page_t.qnext) so push allocates nothing — a
 * malloc on the foreground enqueue path would show up in the C10 p99 numbers.
 * One link field is enough because of the single-membership invariant (§4.4).
 */
#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "scrap_page.h"

typedef struct nox_queue nox_queue_t;

nox_queue_t *nox_queue_create(void);

/* Frees the queue itself. Does NOT free any page still linked in it. */
void nox_queue_destroy(nox_queue_t *q);

/* Append at the tail. Never blocks, never fails. Signals one waiting consumer. */
void nox_queue_push(nox_queue_t *q, scrap_page_t *p);

/* Pop the head, blocking on the condvar while empty. Returns NULL once
 * nox_queue_shutdown() has been called AND the queue is drained. */
scrap_page_t *nox_queue_pop(nox_queue_t *q);

/*
 * Pop the head ONLY if head->base == want_base; otherwise return NULL without
 * touching the queue. Never blocks. This is how Stage-2 builds a pwritev batch
 * of contiguous pages without a peek-then-pop race (spec §5.6).
 */
scrap_page_t *nox_queue_pop_if_base(nox_queue_t *q, uint64_t want_base);

/* Wake every blocked consumer; subsequent pops drain then return NULL. */
void nox_queue_shutdown(nox_queue_t *q);

size_t nox_queue_depth(nox_queue_t *q);

#endif /* QUEUE_H */
