/*
 * holes_test.c - unit test for OTflush Stage-1 hole filling (spec §5.4).
 *
 * Runs LOCALLY. io_direct_pread/pwrite are REPLACED by RAM-backed fakes defined
 * below, so no O_DIRECT, no NVMe, no bench box. This is deliberate: hole filling
 * is the one routine where a bug writes garbage over valid on-disk data
 * (docs/00_flow_summary.md:79), and it must be provable without hardware.
 *
 * Build: make test-holes
 */
#define _GNU_SOURCE
#include "scrap_page.h"
#include "io_direct.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_DISK_SIZE (2u * NOX_DATAZONE_SIZE)

static uint8_t *g_disk;      /* the fake SSD */
static int      g_preads;    /* how many preads Stage-1 issued */

/* --- fakes that satisfy the io_direct.h contract ------------------------- */
ssize_t io_direct_pread(int fd, void *buf, size_t len, off_t off)
{
    (void)fd;
    assert(NOX_IS_ALIGNED(off) && NOX_IS_ALIGNED(len));  /* O_DIRECT rules */
    assert((size_t)off + len <= FAKE_DISK_SIZE);
    memcpy(buf, g_disk + off, len);
    g_preads++;
    return (ssize_t)len;
}

ssize_t io_direct_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    (void)fd;
    assert(NOX_IS_ALIGNED(off) && NOX_IS_ALIGNED(len));
    assert((size_t)off + len <= FAKE_DISK_SIZE);
    memcpy(g_disk + off, buf, len);
    return (ssize_t)len;
}

/* scrap_page_writeback calls the _all wrapper (Step 4b). This test never
 * exercises the short-write retry — that path has its own dedicated coverage in
 * bench/pwritev_toy.c (C4-S3), which drives it with a scripted fake writer. */
ssize_t io_direct_pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    return io_direct_pwrite(fd, buf, len, off);
}

int io_direct_open(const char *path) { (void)path; return 7; }

/* ------------------------------------------------------------------------ */

/* Hole enumeration is the complement of the entry list within [0, 256K). */
static void test_hole_ranges(void)
{
    scrap_page_t *p = scrap_page_alloc(0, 0);
    assert(p != NULL);
    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];

    /* Empty page: exactly one hole covering everything. */
    uint32_t n = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
    assert(n == 1);
    assert(holes[0].offset == 0 && holes[0].size == NOX_DATAZONE_SIZE);

    /* One segment in the middle -> a hole before and a hole after. */
    uint8_t byte = 0xAB;
    assert(scrap_page_merge(p, &byte, 1000, 1) == SCRAP_OK);
    n = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
    assert(n == 2);
    assert(holes[0].offset == 0    && holes[0].size == 1000);
    assert(holes[1].offset == 1001 && holes[1].size == NOX_DATAZONE_SIZE - 1001);

    /* A segment touching offset 0 kills the leading hole. Note the source
     * buffer must actually be 4096 bytes: merge memcpy's `len` bytes out of it. */
    scrap_page_free(p);
    p = scrap_page_alloc(0, 0);
    uint8_t *blk = malloc(4096);
    assert(blk != NULL);
    memset(blk, 0xCD, 4096);
    assert(scrap_page_merge(p, blk, 0, 4096) == SCRAP_OK);
    free(blk);
    n = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
    assert(n == 1);
    assert(holes[0].offset == 4096 && holes[0].size == NOX_DATAZONE_SIZE - 4096);

    scrap_page_free(p);
    printf("  hole_ranges              OK\n");
}

/*
 * THE test: a partial page flushed over a region that already holds valid data
 * must preserve every byte the user did not write.
 */
static void test_fill_holes_preserves_disk(void)
{
    /* Pre-fill the whole fake disk with a known pattern. */
    for (size_t i = 0; i < FAKE_DISK_SIZE; i++)
        g_disk[i] = (uint8_t)(i & 0xff);

    uint8_t *expect = malloc(NOX_DATAZONE_SIZE);
    assert(expect != NULL);
    memcpy(expect, g_disk, NOX_DATAZONE_SIZE);

    scrap_page_t *p = scrap_page_alloc(0, 0);
    assert(p != NULL);

    /* Two sparse user writes, deliberately NOT 4K-aligned. */
    uint8_t a[100], b[7];
    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));
    assert(scrap_page_merge(p, a, 5000, sizeof(a)) == SCRAP_OK);
    assert(scrap_page_merge(p, b, 90001, sizeof(b)) == SCRAP_OK);
    memcpy(expect + 5000,  a, sizeof(a));
    memcpy(expect + 90001, b, sizeof(b));

    g_preads = 0;
    assert(scrap_page_fill_holes(p, 7) == 0);
    assert(scrap_page_writeback(p, 7) == 0);

    /* The written region must equal on-disk-before + the two user segments. */
    if (memcmp(g_disk, expect, NOX_DATAZONE_SIZE) != 0) {
        for (size_t i = 0; i < NOX_DATAZONE_SIZE; i++)
            if (g_disk[i] != expect[i]) {
                fprintf(stderr, "corruption at byte %zu: disk=0x%02x want=0x%02x\n",
                        i, g_disk[i], expect[i]);
                break;
            }
        abort();
    }
    /* The neighbouring region must be untouched. */
    for (size_t i = NOX_DATAZONE_SIZE; i < FAKE_DISK_SIZE; i++)
        assert(g_disk[i] == (uint8_t)(i & 0xff));

    /* Stage-1 must read the holes, not the whole page: 2 segments => 3 holes. */
    assert(g_preads == 3);

    /* After Stage-1 every byte of the zone is valid, so the entry array must
     * COLLAPSE to a single full-zone segment. Two things depend on it:
     * scrap_page_is_full() is `counter == NOX_DATAZONE_SIZE`, so without the
     * collapse it reports false on a page Stage-1 just completed; and the page
     * would stay pinned at NOX_MAX_ENTRIES, so the next scattered write to this
     * base would overflow, seal it and allocate another 256KB page. That churn
     * is what drove RSS to 32 GB in five seconds on the first soak run. */
    assert(p->hdr.number == 1);
    assert(p->hdr.entries[0].offset == 0);
    assert(p->hdr.entries[0].size == NOX_DATAZONE_SIZE);
    assert(p->hdr.counter == NOX_DATAZONE_SIZE);
    assert(scrap_page_is_full(p));
    scrap_entry_t after[NOX_MAX_ENTRIES + 1];
    assert(scrap_page_hole_ranges(p, after, NOX_MAX_ENTRIES + 1) == 0);

    scrap_page_free(p);
    free(expect);
    printf("  fill_holes preserves     OK (%d preads, no corruption)\n", g_preads);
}

/* A full page needs no Stage-1 read at all — the asymmetry win. */
static void test_full_page_needs_no_read(void)
{
    scrap_page_t *p = scrap_page_alloc(NOX_DATAZONE_SIZE, 0);
    assert(p != NULL);
    uint8_t *all = malloc(NOX_DATAZONE_SIZE);
    assert(all != NULL);
    memset(all, 0x5a, NOX_DATAZONE_SIZE);
    assert(scrap_page_merge(p, all, 0, NOX_DATAZONE_SIZE) == SCRAP_OK);
    assert(scrap_page_is_full(p));

    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
    assert(scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1) == 0);

    g_preads = 0;
    assert(scrap_page_fill_holes(p, 7) == 0);
    assert(g_preads == 0);                       /* zero reads: the whole point */
    assert(scrap_page_writeback(p, 7) == 0);
    for (size_t i = 0; i < NOX_DATAZONE_SIZE; i++)
        assert(g_disk[NOX_DATAZONE_SIZE + i] == 0x5a);

    scrap_page_free(p);
    free(all);
    printf("  full page: 0 preads      OK\n");
}

/*
 * scrap_page_alloc deliberately does NOT zero the data zone (see the comment
 * there): zeroing cost 120 us of foreground latency per page on the bench box.
 * That is only safe if EVERY byte is overwritten before the zone reaches the
 * disk — user segments plus the disk-read complement, with no gap.
 *
 * This test proves it the only way that is worth anything: poison the whole
 * zone with a byte that appears in neither the disk pattern nor the user data,
 * then assert not one of those bytes survives to the disk. If a future change
 * ever leaves a gap between the entry list and the hole list, 0xE7 lands on the
 * user's file and this fails loudly.
 */
static void test_poisoned_zone_never_leaks(void)
{
    for (size_t i = 0; i < FAKE_DISK_SIZE; i++)
        g_disk[i] = (uint8_t)(i & 0xff);

    uint8_t *expect = malloc(NOX_DATAZONE_SIZE);
    assert(expect != NULL);
    memcpy(expect, g_disk, NOX_DATAZONE_SIZE);

    scrap_page_t *p = scrap_page_alloc(0, 0);
    assert(p != NULL);
    memset(p->data, 0xE7, NOX_DATAZONE_SIZE);    /* worst case the allocator can hand us */

    /* Sparse, unaligned, and deliberately more than one segment so the hole
     * list has a leading, a middle and a trailing range. */
    uint8_t a[300], b[64], c[9];
    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));
    memset(c, 0x33, sizeof(c));
    assert(scrap_page_merge(p, a, 0,      sizeof(a)) == SCRAP_OK);  /* touches offset 0 */
    assert(scrap_page_merge(p, b, 131072, sizeof(b)) == SCRAP_OK);
    assert(scrap_page_merge(p, c, NOX_DATAZONE_SIZE - sizeof(c), sizeof(c)) == SCRAP_OK);
    memcpy(expect + 0,      a, sizeof(a));
    memcpy(expect + 131072, b, sizeof(b));
    memcpy(expect + NOX_DATAZONE_SIZE - sizeof(c), c, sizeof(c));

    assert(scrap_page_fill_holes(p, 7) == 0);
    assert(scrap_page_writeback(p, 7) == 0);

    for (size_t i = 0; i < NOX_DATAZONE_SIZE; i++) {
        if (g_disk[i] == 0xE7 && expect[i] != 0xE7) {
            fprintf(stderr, "POISON LEAKED to disk at byte %zu: the data zone is "
                            "no longer fully covered by entries + holes\n", i);
            abort();
        }
    }
    assert(memcmp(g_disk, expect, NOX_DATAZONE_SIZE) == 0);

    scrap_page_free(p);
    free(expect);
    printf("  poison never leaks       OK (unzeroed zone fully overwritten)\n");
}

int main(void)
{
    g_disk = malloc(FAKE_DISK_SIZE);
    assert(g_disk != NULL);

    printf("holes_test:\n");
    test_hole_ranges();
    test_fill_holes_preserves_disk();
    test_full_page_needs_no_read();
    test_poisoned_zone_never_leaks();
    printf("holes_test: PASS\n");

    free(g_disk);
    return 0;
}
