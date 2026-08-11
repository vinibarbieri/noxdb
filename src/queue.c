/*
 * queue.c - see queue.h.
 */
#define _GNU_SOURCE
#include "queue.h"

#include <pthread.h>
#include <stdlib.h>

struct nox_queue {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    scrap_page_t   *head;
    scrap_page_t   *tail;
    size_t          depth;
    int             shutdown;
};

nox_queue_t *nox_queue_create(void)
{
    nox_queue_t *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    return q;
}

void nox_queue_destroy(nox_queue_t *q)
{
    if (!q)
        return;
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q);
}

void nox_queue_push(nox_queue_t *q, scrap_page_t *p)
{
    p->qnext = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail)
        q->tail->qnext = p;
    else
        q->head = p;
    q->tail = p;
    q->depth++;
    /* Signal (not broadcast): one page wakes at most one consumer. */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

/* Caller must hold q->lock. Unlinks and returns the head, or NULL if empty. */
static scrap_page_t *pop_head_locked(nox_queue_t *q)
{
    scrap_page_t *p = q->head;
    if (!p)
        return NULL;
    q->head = p->qnext;
    if (!q->head)
        q->tail = NULL;
    q->depth--;
    p->qnext = NULL;
    return p;
}

scrap_page_t *nox_queue_pop(nox_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    /* while, not if: condvar waits are subject to spurious wakeups, and with
     * multiple consumers another thread may take the page before we re-acquire
     * the mutex. Re-test the predicate every time. */
    while (!q->head && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);

    scrap_page_t *p = pop_head_locked(q);   /* drain even after shutdown */
    pthread_mutex_unlock(&q->lock);
    return p;
}

scrap_page_t *nox_queue_pop_if_base(nox_queue_t *q, uint64_t want_base)
{
    pthread_mutex_lock(&q->lock);
    scrap_page_t *p = NULL;
    if (q->head && q->head->base == want_base)
        p = pop_head_locked(q);
    pthread_mutex_unlock(&q->lock);
    return p;
}

void nox_queue_shutdown(nox_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);   /* release EVERY blocked consumer */
    pthread_mutex_unlock(&q->lock);
}

size_t nox_queue_depth(nox_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    size_t d = q->depth;
    pthread_mutex_unlock(&q->lock);
    return d;
}
