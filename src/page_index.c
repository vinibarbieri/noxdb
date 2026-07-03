/*
 * page_index.c - Chained hash table of scrap pages. See page_index.h.
 */
#define _GNU_SOURCE
#include "page_index.h"

#include <stdlib.h>

/* Power-of-two bucket count so the mask is a cheap AND. */
#define PI_BUCKETS 1024u
#define PI_MASK    (PI_BUCKETS - 1u)

struct page_index {
    scrap_page_t *buckets[PI_BUCKETS];
};

/* Hash a 256KB-aligned base. Shift out the 18 always-zero low bits first so the
 * page number itself spreads across buckets, then mix with a Fibonacci constant. */
static inline uint32_t pi_hash(uint64_t base)
{
    uint64_t page_no = base / NOX_DATAZONE_SIZE;
    return (uint32_t)((page_no * 0x9E3779B97F4A7C15ull) >> 46) & PI_MASK;
}

page_index_t *page_index_create(void)
{
    return calloc(1, sizeof(page_index_t)); /* all bucket heads NULL */
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
    free(idx);
}

scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created)
{
    uint32_t b = pi_hash(base);

    for (scrap_page_t *p = idx->buckets[b]; p; p = p->next) {
        if (p->base == base) {
            if (created) *created = 0;
            return p;
        }
    }

    scrap_page_t *p = scrap_page_alloc(base, ssd_id);
    if (!p)
        return NULL;
    p->next = idx->buckets[b];   /* push onto bucket head */
    idx->buckets[b] = p;
    if (created) *created = 1;
    return p;
}

void page_index_remove(page_index_t *idx, uint64_t base)
{
    uint32_t b = pi_hash(base);
    scrap_page_t **link = &idx->buckets[b];
    while (*link) {
        if ((*link)->base == base) {
            scrap_page_t *victim = *link;
            *link = victim->next;        /* unlink */
            scrap_page_free(victim);
            return;
        }
        link = &(*link)->next;
    }
}

void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx)
{
    for (uint32_t b = 0; b < PI_BUCKETS; b++)
        for (scrap_page_t *p = idx->buckets[b]; p; p = p->next)
            fn(p, ctx);
}
