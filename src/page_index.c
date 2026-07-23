/*
 * page_index.c - Chained hash table of scrap pages with SHARDED bucket locks.
 * See page_index.h. Concurrency model: docs/01 §5.
 */
#define _GNU_SOURCE
#include "page_index.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Power-of-two bucket count so the mask is a cheap AND. */
#define PI_BUCKETS 1024u
#define PI_MASK    (PI_BUCKETS - 1u)

/*
 * Cache-line-padded shard lock. Padding + alignment guarantee two distinct
 * shard mutexes never share a 64B cache line, so locking unrelated shards from
 * different cores generates no false-sharing coherence traffic. (docs/01 §5)
 */
typedef union {
    pthread_mutex_t mtx;
    char            pad[NOX_CACHELINE];
} pi_shard_t;

struct page_index {
    /* _Alignas lives on the member, not the typedef: C11 6.7.5p2 forbids an
     * alignment-specifier on a typedef declaration, so `} _Alignas(...) name;`
     * is a constraint violation, not a portability quirk (rejected by any
     * conforming C11 compiler). This placement gets the same guarantee: the
     * shards array is forced to start 64B-aligned, and since pi_shard_t is
     * exactly NOX_CACHELINE bytes wide, every subsequent shard lands on its
     * own cache line too. */
    _Alignas(NOX_CACHELINE) pi_shard_t shards[NOX_INDEX_SHARDS];  /* one mutex per shard */
    scrap_page_t *buckets[PI_BUCKETS];       /* bucket heads (chained) */
};

/* Hash a 256KB-aligned base: page number, Fibonacci-mixed, top 10 bits. */
static inline uint32_t pi_hash(uint64_t base)
{
    uint64_t page_no = base / NOX_DATAZONE_SIZE;
    return (uint32_t)((page_no * 0x9E3779B97F4A7C15ull) >> 54) & PI_MASK;
}

/* Map a bucket index to its shard. Low bits of the Fibonacci-mixed bucket are
 * well distributed, so a plain mask spreads pages evenly across shards. */
static inline uint32_t pi_shard(uint32_t bucket)
{
    return bucket & (NOX_INDEX_SHARDS - 1u);
}

page_index_t *page_index_create(void)
{
    page_index_t *idx = NULL;
    /* posix_memalign (not calloc) so the shards[] array is genuinely cache-line
     * aligned; calloc only promises 16B, which would defeat pi_shard_t padding. */
    if (posix_memalign((void **)&idx, NOX_CACHELINE, sizeof(*idx)) != 0)
        return NULL;
    memset(idx, 0, sizeof(*idx));            /* all bucket heads NULL */

    for (uint32_t s = 0; s < NOX_INDEX_SHARDS; s++) {
        if (pthread_mutex_init(&idx->shards[s].mtx, NULL) != 0) {
            for (uint32_t j = 0; j < s; j++)  /* unwind partial init */
                pthread_mutex_destroy(&idx->shards[j].mtx);
            free(idx);
            return NULL;
        }
    }
    return idx;
}

void page_index_destroy(page_index_t *idx)
{
    if (!idx)
        return;
    for (uint32_t b = 0; b < PI_BUCKETS; b++) {
        scrap_page_t *p = idx->buckets[b];
        while (p) {
            scrap_page_t *next = p->next;
            scrap_page_free(p);
            p = next;
        }
    }
    for (uint32_t s = 0; s < NOX_INDEX_SHARDS; s++)
        pthread_mutex_destroy(&idx->shards[s].mtx);
    free(idx);
}

scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created)
{
    uint32_t    b  = pi_hash(base);
    pi_shard_t *sh = &idx->shards[pi_shard(b)];

    pthread_mutex_lock(&sh->mtx);            /* guard this shard's bucket lists */

    for (scrap_page_t *p = idx->buckets[b]; p; p = p->next) {
        if (p->base == base) {
            if (created) *created = 0;
            pthread_mutex_unlock(&sh->mtx);
            return p;
        }
    }

    scrap_page_t *p = scrap_page_alloc(base, ssd_id);
    if (!p) {
        pthread_mutex_unlock(&sh->mtx);      /* release before OOM return */
        return NULL;
    }
    p->next = idx->buckets[b];               /* push onto bucket head */
    idx->buckets[b] = p;
    if (created) *created = 1;

    pthread_mutex_unlock(&sh->mtx);
    return p;
}

void page_index_remove(page_index_t *idx, uint64_t base)
{
    uint32_t    b  = pi_hash(base);
    pi_shard_t *sh = &idx->shards[pi_shard(b)];

    pthread_mutex_lock(&sh->mtx);
    scrap_page_t **link = &idx->buckets[b];
    while (*link) {
        if ((*link)->base == base) {
            scrap_page_t *victim = *link;
            *link = victim->next;            /* unlink under the shard lock */
            pthread_mutex_unlock(&sh->mtx);
            scrap_page_free(victim);         /* free outside: victim unreachable now */
            return;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&sh->mtx);
}

void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx)
{
    /* Lock-free ON PURPOSE: only nox_close calls this, after all writer threads
     * have joined, so there is no concurrent mutation. Locking here would also
     * risk deadlock if `fn` re-entered the index. (docs/01 §5) */
    for (uint32_t b = 0; b < PI_BUCKETS; b++)
        for (scrap_page_t *p = idx->buckets[b]; p; p = p->next)
            fn(p, ctx);
}
