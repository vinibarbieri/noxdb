/*
 * pc_queue_toy.c — C4-S2 study toy: unbounded MPMC queue (mutex + condvar).
 *
 * 4 producers / 4 consumers over a single unbounded intrusive linked-list queue.
 * This is a STUDY toy for OSTEP ch.30 (Condition Variables), and a dry run for
 * C4-B1 (src/queue.{h,c}), where the same structure will link scrap_page_t
 * objects instead of malloc'd nodes.
 *
 * Pure RAM: no O_DIRECT, no disk. Runs on the laptop; also TSan-clean.
 *
 *   Build : make toy-pc  &&  make toy-pc-tsan
 *   Run   : ./bench/pc_queue_toy
 *   Modes : --if            demonstrate the `if`-instead-of-`while` bug
 *           --signal-close  demonstrate why shutdown needs broadcast (HANGS)
 *
 * Why unbounded matters here: a bounded buffer needs TWO condition variables
 * (empty/fill, OSTEP Fig 30.12) because producers can also block. Unbounded
 * means a producer never waits, so ONE condvar (not_empty) is enough. That is
 * also the OTflush case: the page already exists in RAM, there is no slot to
 * run out of.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPROD           4
#define NCONS           4
#define ITEMS_PER_PROD  100000
#define TOTAL_ITEMS     ((uint64_t)NPROD * ITEMS_PER_PROD)

/* ------------------------------------------------------------------ modes */

typedef enum {
    MODE_CORRECT = 0,  /* while + broadcast-on-close: the right thing */
    MODE_IF,           /* if instead of while in the wait loop        */
    MODE_SIGNAL_CLOSE  /* signal instead of broadcast at shutdown     */
} toy_mode_t;

static toy_mode_t g_mode = MODE_CORRECT;  /* written once in main() before any
                                       * thread is created, read-only after:
                                       * no synchronization needed, no race. */

/* ------------------------------------------------------------------ queue */

typedef struct node {
    struct node *next;
    uint64_t     value;
} node_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;

    /* --- everything below is protected by `lock` --- */
    node_t  *head;    /* pop side  */
    node_t  *tail;    /* push side */
    uint64_t count;   /* items currently queued (invariant check only) */
    bool     closed;  /* true => no producer will ever push again      */
    unsigned waiters; /* consumers currently parked in cond_wait       */
} queue_t;

static void queue_init(queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void queue_destroy(queue_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

/*
 * Push one item. Never blocks: the queue is unbounded, so there is no "full"
 * condition to wait on (contrast OSTEP Fig 30.12, where the producer waits on
 * `empty` while count == MAX).
 */
static void queue_push(queue_t *q, uint64_t value)
{
    node_t *n = malloc(sizeof(*n));
    if (!n) {
        fprintf(stderr, "OOM in queue_push\n");
        abort();
    }
    n->value = value;
    n->next  = NULL;

    pthread_mutex_lock(&q->lock);

    if (q->tail)
        q->tail->next = n;
    else
        q->head = n;        /* queue was empty: n is both head and tail */
    q->tail = n;
    q->count++;

    /*
     * signal(), NOT broadcast(): this push made exactly ONE unit of work
     * available, and every consumer is interchangeable — waking one is the
     * complete amount of progress the state change can support. Waking all
     * four would be correct but wasteful (thundering herd: N wake, N-1 fail
     * the while-test and go straight back to sleep).
     *
     * Signalling with the lock HELD (OSTEP tip, §30.1): strictly optional for
     * a signal, but it removes any window where a consumer evaluates the
     * condition, gets preempted, and only then goes to sleep — after the
     * signal already fired and evaporated.
     */
    pthread_cond_signal(&q->not_empty);

    pthread_mutex_unlock(&q->lock);
}

/*
 * Pop one item. Returns false only when the queue is drained AND closed,
 * i.e. no item will ever arrive again — that is the consumer's exit condition.
 */
static bool queue_pop(queue_t *q, uint64_t *out)
{
    pthread_mutex_lock(&q->lock);

    if (g_mode == MODE_IF) {
        /*
         * BROKEN ON PURPOSE (OSTEP Fig 30.6). Two independent reasons this
         * fails even though it "looks" equivalent:
         *
         *   1. Mesa semantics: a signal only makes the waiter runnable; it does
         *      not hand over the lock. Another consumer can grab the lock first
         *      and steal the item, so the condition is false again by the time
         *      wait() returns.
         *   2. Spurious wakeups: POSIX allows cond_wait to return with no
         *      signal at all.
         *
         * Either way the thread proceeds with head == NULL.
         */
        if (q->head == NULL && !q->closed) {
            q->waiters++;
            pthread_cond_wait(&q->not_empty, &q->lock);
            q->waiters--;
        }
    } else {
        /*
         * CORRECT. wait() returning means "you are awake and you hold the
         * lock" — it does NOT mean "your condition is true". The state
         * variable is the only source of truth, and it may have changed
         * between the signal and this thread actually running. So re-test in
         * a loop, always.
         */
        while (q->head == NULL && !q->closed) {
            /* waiters is bookkeeping for the --signal-close demo only; the
             * correctness of the queue does not depend on it. Kept under the
             * same lock as everything else, so it costs nothing extra. */
            q->waiters++;
            pthread_cond_wait(&q->not_empty, &q->lock);
            q->waiters--;
        }
    }

    if (q->head == NULL) {
        if (!q->closed) {
            /* Only reachable in MODE_IF: we woke on a stale hint. In real code
             * this is where the NULL deref / assert(count == 1) would fire. */
            fprintf(stderr,
                    "BUG: woke from cond_wait with an EMPTY, still-open queue.\n"
                    "     This is exactly why the wait must be a `while`, not an `if`.\n");
            abort();
        }
        pthread_mutex_unlock(&q->lock);
        return false;               /* drained + closed => real end of stream */
    }

    node_t *n = q->head;
    q->head = n->next;
    if (q->head == NULL)
        q->tail = NULL;             /* queue went empty: keep tail consistent */
    q->count--;

    pthread_mutex_unlock(&q->lock);

    *out = n->value;
    free(n);                        /* freed outside the lock: no shared state */
    return true;
}

/*
 * TEST SCAFFOLDING — not part of the queue's contract.
 *
 * Block until the queue is drained AND all `n` consumers are parked inside
 * cond_wait. Without this, the --signal-close bug almost never reproduces:
 * when the producers finish there is still a backlog, so the consumers are
 * BUSY rather than asleep. Each one then drains, evaluates the while-predicate,
 * sees closed == true and leaves WITHOUT ever calling cond_wait — so a lost
 * signal costs nothing. The lost wakeup only strands a thread that was already
 * asleep at the instant close() ran, which is exactly the state we force here.
 *
 * That is the real lesson: a missing broadcast is a LATENT bug. It hides behind
 * a busy queue and only fires under the interleaving where every waiter is
 * parked — i.e. in production, under a lull, not in your test run.
 */
static void queue_wait_until_all_parked(queue_t *q, unsigned n)
{
    for (;;) {
        pthread_mutex_lock(&q->lock);
        bool parked = (q->head == NULL && q->waiters == n);
        pthread_mutex_unlock(&q->lock);

        if (parked)
            return;
        usleep(1000);   /* polling is fine: test-only, off the hot path */
    }
}

/*
 * Shutdown. `closed` is a COVERING CONDITION (OSTEP §30.3): the state change
 * satisfies the wait predicate of EVERY sleeping consumer at once, not one
 * unit of work for one of them. Hence broadcast.
 */
static void queue_close(queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = true;

    if (g_mode == MODE_SIGNAL_CLOSE) {
        /* BROKEN ON PURPOSE: wakes ONE consumer. The other three stay parked
         * on the condvar forever and pthread_join never returns. */
        pthread_cond_signal(&q->not_empty);
    } else {
        pthread_cond_broadcast(&q->not_empty);
    }

    pthread_mutex_unlock(&q->lock);
}

/* ---------------------------------------------------------------- threads */

typedef struct {
    pthread_t tid;
    queue_t  *q;
    int       id;
    uint64_t  items;  /* per-thread accumulators: no shared counters, so no  */
    uint64_t  sum;    /* atomics and nothing for TSan to complain about.     */
} worker_t;           /* main reads these only after pthread_join (happens-before). */

static void *producer(void *arg)
{
    worker_t *w = arg;

    for (int i = 0; i < ITEMS_PER_PROD; i++) {
        /* Globally unique values so the consumers' total sum is checkable. */
        uint64_t v = (uint64_t)w->id * ITEMS_PER_PROD + (uint64_t)i;
        queue_push(w->q, v);
        w->items++;
        w->sum += v;
    }
    return NULL;
}

static void *consumer(void *arg)
{
    worker_t *w = arg;
    uint64_t  v;

    while (queue_pop(w->q, &v)) {
        w->items++;
        w->sum += v;
    }
    return NULL;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--if") == 0) {
            g_mode = MODE_IF;
        } else if (strcmp(argv[i], "--signal-close") == 0) {
            g_mode = MODE_SIGNAL_CLOSE;
        } else {
            fprintf(stderr, "usage: %s [--if | --signal-close]\n", argv[0]);
            return 2;
        }
    }

    if (g_mode == MODE_IF)
        printf("MODE: broken `if` in cond_wait — expect an abort (or a TSan-visible mess).\n");
    if (g_mode == MODE_SIGNAL_CLOSE)
        printf("MODE: broken signal-on-close — expect a HANG in join. Ctrl-C to kill.\n");

    queue_t q;
    queue_init(&q);

    worker_t prod[NPROD] = {0};
    worker_t cons[NCONS] = {0};

    /* Consumers first, so they are already parked on the condvar when the
     * producers start — that is the interleaving that exercises the wait path. */
    for (int i = 0; i < NCONS; i++) {
        cons[i].q = &q;
        cons[i].id = i;
        pthread_create(&cons[i].tid, NULL, consumer, &cons[i]);
    }
    for (int i = 0; i < NPROD; i++) {
        prod[i].q = &q;
        prod[i].id = i;
        pthread_create(&prod[i].tid, NULL, producer, &prod[i]);
    }

    for (int i = 0; i < NPROD; i++)
        pthread_join(prod[i].tid, NULL);

    /* All producers are done => no further pushes are possible. Only now is it
     * safe to close; closing earlier would let consumers exit on a queue that
     * is still going to receive items. */
    if (g_mode == MODE_SIGNAL_CLOSE) {
        /* Force the interleaving the bug needs: every consumer asleep. */
        queue_wait_until_all_parked(&q, NCONS);
        printf("all %d consumers parked; closing with signal (expect %d stranded)\n",
               NCONS, NCONS - 1);
    }
    queue_close(&q);

    for (int i = 0; i < NCONS; i++)
        pthread_join(cons[i].tid, NULL);

    /* ------------------------------------------------------------ verify */
    uint64_t produced = 0, produced_sum = 0;
    uint64_t consumed = 0, consumed_sum = 0;

    for (int i = 0; i < NPROD; i++) {
        produced     += prod[i].items;
        produced_sum += prod[i].sum;
    }
    for (int i = 0; i < NCONS; i++) {
        printf("consumer %d: %8llu items\n", i, (unsigned long long)cons[i].items);
        consumed     += cons[i].items;
        consumed_sum += cons[i].sum;
    }

    printf("produced: %llu items (sum %llu)\n",
           (unsigned long long)produced, (unsigned long long)produced_sum);
    printf("consumed: %llu items (sum %llu)\n",
           (unsigned long long)consumed, (unsigned long long)consumed_sum);
    printf("queue residual count: %llu (expect 0)\n",
           (unsigned long long)q.count);

    bool ok = (produced == TOTAL_ITEMS) &&
              (consumed == TOTAL_ITEMS) &&
              (produced_sum == consumed_sum) &&
              (q.count == 0) &&
              (q.head == NULL) && (q.tail == NULL);

    queue_destroy(&q);

    /* Not just "no crash": every item produced was consumed exactly once (the
     * sums catch both loss and duplication, which a bare count would miss). */
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
