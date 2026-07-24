/*
 * page_index.c - Chained hash table of scrap pages with SHARDED bucket locks.
 * See page_index.h. Concurrency model: docs/01 §5.
 */
#define _GNU_SOURCE
#include "page_index.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* PI_BITS is the single source of truth for the index geometry: the Fibonacci
 * hash keeps the TOP PI_BITS bits of the 64-bit product, so the shift is exactly
 * 64 - PI_BITS and the bucket count is exactly 2^PI_BITS. Deriving both (instead
 * of hardcoding 1024 and 54 independently) makes the two impossible to
 * desynchronise: change PI_BITS and the shift follows.
 * No mask is needed - the shift alone bounds the result to [0, PI_BUCKETS). */
#define PI_BITS    10u
#define PI_BUCKETS (1u << PI_BITS)
#define PI_SHIFT   (64u - PI_BITS)

/*
 * Cache-line-padded shard lock. Padding + alignment guarantee two distinct
 * shard mutexes never share a 64B cache line, so locking unrelated shards from
 * different cores generates no false-sharing coherence traffic. (docs/01 §5)
 */
typedef union {
    pthread_mutex_t mtx;
    char            pad[NOX_CACHELINE];
} pi_shard_t;

/* pi_shard slices NOX_INDEX_SHARD_BITS out of a PI_BITS-wide bucket index, so
 * the bucket index must be at least as wide as the shard index. (Power-of-two
 * shard count needs no assert: NOX_INDEX_SHARDS is defined as 1u << BITS.) */
_Static_assert(PI_BITS >= NOX_INDEX_SHARD_BITS,
               "PI_BITS must be >= NOX_INDEX_SHARD_BITS (else shards outnumber buckets)");
_Static_assert(sizeof(pi_shard_t) == NOX_CACHELINE,
               "shard lock must be exactly one cache line (no false sharing)");

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

/* Hash a 256KB-aligned base: page number, Fibonacci-mixed, top PI_BITS bits.
 * Multiplying by 2^64/phi spreads the input's entropy toward the HIGH bits, so
 * the top slice is the well-mixed one; shifting it down to bit 0 yields a value
 * already in [0, PI_BUCKETS) with no mask. */
static inline uint32_t pi_hash(uint64_t base)
{
    uint64_t page_no = base / NOX_DATAZONE_SIZE;
    return (uint32_t)((page_no * 0x9E3779B97F4A7C15ull) >> PI_SHIFT);
}

/*
 * Map a bucket index to its shard: the TOP NOX_INDEX_SHARD_BITS of the bucket.
 *
 * Deliberately the top slice, not `bucket & (NOX_INDEX_SHARDS-1)`. Fibonacci
 * hashing only guarantees good mixing in the HIGH bits of the product; the low
 * bits of the retained slice are its weakest. Masking them made sequentially
 * numbered pages - the common case, and exactly what the C3 gate writes - land
 * on far fewer shards than exist: measured over 64 consecutive page numbers,
 * the low-bit mask used 20 of 64 shards, the top slice uses 55. Over a large
 * working set both converge, so this was a small-working-set contention loss,
 * not a correctness bug.
 */
static inline uint32_t pi_shard(uint32_t bucket)
{
    return bucket >> (PI_BITS - NOX_INDEX_SHARD_BITS);
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
