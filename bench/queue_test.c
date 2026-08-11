/*
 * queue_test.c - unit test for the OTflush intrusive MPMC queue (spec §5.1).
 *
 * Runs LOCALLY: pure RAM, no O_DIRECT, no NVMe. Build with `make test-queue`.
 * Links queue.c only; scrap_page_t is used as a bare struct (no data zone
 * allocated), so no engine I/O symbols are pulled in.
 */
#define _GNU_SOURCE
#include "queue.h"
#include "scrap_page.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROD    4
#define NCONS    4
#define PER_PROD 2000
#define TOTAL    (NPROD * PER_PROD)

static scrap_page_t *g_pages;      /* TOTAL bare pages, index == payload id */
static nox_queue_t  *g_q;

/* FIFO order, depth accounting, and pop_if_base selectivity. */
static void test_fifo_and_depth(void)
{
    nox_queue_t *q = nox_queue_create();
    assert(q != NULL);
    assert(nox_queue_depth(q) == 0);

    for (int i = 0; i < 3; i++) {
        g_pages[i].base = (uint64_t)i * NOX_DATAZONE_SIZE;
        nox_queue_push(q, &g_pages[i]);
    }
    assert(nox_queue_depth(q) == 3);

    /* pop_if_base must REFUSE a non-matching head and leave the queue intact. */
    assert(nox_queue_pop_if_base(q, 999 * NOX_DATAZONE_SIZE) == NULL);
    assert(nox_queue_depth(q) == 3);

    /* ...and pop the head when the base matches. */
    scrap_page_t *p = nox_queue_pop_if_base(q, 0);
    assert(p == &g_pages[0]);
    assert(nox_queue_depth(q) == 2);

    assert(nox_queue_pop(q) == &g_pages[1]);   /* FIFO, not LIFO */
    assert(nox_queue_pop(q) == &g_pages[2]);
    assert(nox_queue_depth(q) == 0);

    nox_queue_shutdown(q);
    assert(nox_queue_pop(q) == NULL);          /* shutdown unblocks with NULL */
    nox_queue_destroy(q);
    printf("  fifo/depth/pop_if_base   OK\n");
}

static void *producer(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < PER_PROD; i++)
        nox_queue_push(g_q, &g_pages[id * PER_PROD + i]);
    return NULL;
}

static void *consumer(void *arg)
{
    long *got = arg;
    scrap_page_t *p;
    while ((p = nox_queue_pop(g_q)) != NULL) {
        /* ssd_id doubles as a "seen" marker; each page must be popped exactly
         * once across ALL consumers, which is what makes this an MPMC test. */
        p->ssd_id_seen++;
        (*got)++;
    }
    return NULL;
}

/* Every pushed page is popped exactly once, by exactly one consumer. */
static void test_mpmc_no_loss_no_dup(void)
{
    g_q = nox_queue_create();
    assert(g_q != NULL);

    pthread_t prod[NPROD], cons[NCONS];
    long counts[NCONS] = {0};

    for (long i = 0; i < NCONS; i++)
        assert(pthread_create(&cons[i], NULL, consumer, &counts[i]) == 0);
    for (long i = 0; i < NPROD; i++)
        assert(pthread_create(&prod[i], NULL, producer, (void *)i) == 0);

    for (int i = 0; i < NPROD; i++) pthread_join(prod[i], NULL);

    /* Drain, then release the consumers. */
    while (nox_queue_depth(g_q) > 0)
        ;
    nox_queue_shutdown(g_q);
    for (int i = 0; i < NCONS; i++) pthread_join(cons[i], NULL);

    long total = 0;
    for (int i = 0; i < NCONS; i++) total += counts[i];
    assert(total == TOTAL);

    for (int i = 0; i < TOTAL; i++) {
        if (g_pages[i].ssd_id_seen != 1) {
            fprintf(stderr, "page %d popped %d times (want 1)\n",
                    i, g_pages[i].ssd_id_seen);
            abort();
        }
    }
    nox_queue_destroy(g_q);
    printf("  mpmc %d prod / %d cons    OK (%ld pages, no loss, no dup)\n",
           NPROD, NCONS, total);
}

int main(void)
{
    g_pages = calloc(TOTAL, sizeof(*g_pages));
    assert(g_pages != NULL);

    printf("queue_test:\n");
    test_fifo_and_depth();
    test_mpmc_no_loss_no_dup();
    printf("queue_test: PASS\n");

    free(g_pages);
    return 0;
}
