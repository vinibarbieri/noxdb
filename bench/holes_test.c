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

/* How many times each 4K block of the fake disk was read. Stage-1 coalesces its
 * block-widened hole reads, so no block may be fetched twice in one pass. */
#define FAKE_DISK_BLOCKS (FAKE_DISK_SIZE / NOX_BLOCK_SIZE)
static int g_block_reads[FAKE_DISK_BLOCKS];

/* --- fakes that satisfy the io_direct.h contract ------------------------- */
ssize_t io_direct_pread(int fd, void *buf, size_t len, off_t off)
{
    (void)fd;
    assert(NOX_IS_ALIGNED(off) && NOX_IS_ALIGNED(len));  /* O_DIRECT rules */
    assert((size_t)off + len <= FAKE_DISK_SIZE);
    memcpy(buf, g_disk + off, len);
    for (size_t b = (size_t)off / NOX_BLOCK_SIZE;
         b < ((size_t)off + len) / NOX_BLOCK_SIZE; b++)
        g_block_reads[b]++;
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

    /* 2 segments => 3 holes, but ONE pread. The two user segments are tiny and
     * far apart, so widening the three holes to 4K boundaries makes them all
     * touch: [0,8192) + [4096,90112) + [86016,262144) is a single run covering
     * the zone. Before coalescing this cost 3 preads and 270336 B asked for a
     * 262144 B zone — the same blocks fetched twice. Reading LESS than the whole
     * page is what test_read_holes_skips_gaps proves; this case shows the merge
     * never asks for more than the zone. */
    assert(g_preads == 1);

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

/*
 * Coalescing must never bridge a REAL gap: a run of blocks fully covered by user
 * segments still must not be read. One 64K-aligned segment in the middle leaves
 * two holes that are already block-aligned and separated by 64K of valid data,
 * so they stay two preads and the middle is never fetched.
 */
static void test_read_holes_skips_gaps(void)
{
    scrap_page_t *p = scrap_page_alloc(0, 0);
    assert(p != NULL);

    uint8_t *mid = malloc(65536);
    assert(mid != NULL);
    memset(mid, 0x77, 65536);
    assert(scrap_page_merge(p, mid, 65536, 65536) == SCRAP_OK);
    free(mid);

    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
    uint32_t nh = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);
    assert(nh == 2);

    void *scratch = NULL;
    assert(posix_memalign(&scratch, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) == 0);

    g_preads = 0;
    memset(g_block_reads, 0, sizeof(g_block_reads));
    uint64_t asked = 0;
    assert(scrap_page_read_holes(p->base, 7, holes, nh, scratch, &asked) == 0);

    assert(g_preads == 2);                            /* the gap was not bridged */
    assert(asked == NOX_DATAZONE_SIZE - 65536);       /* exactly the two holes */
    for (uint32_t b = 65536 / NOX_BLOCK_SIZE; b < 131072 / NOX_BLOCK_SIZE; b++)
        assert(g_block_reads[b] == 0);                /* the covered middle */

    free(scratch);
    scrap_page_free(p);
    printf("  gaps not bridged         OK (2 preads, %llu B)\n",
           (unsigned long long)asked);
}

/*
 * The read-amplification test. A maximally fragmented page makes consecutive
 * holes widen onto the SAME 4K block; one pread per hole then sends that block
 * to the device repeatedly. Measured at 255 entries before the fix: 954749 B
 * asked per 262144 B zone (3.64x) from ~233 block reads over a 64-block zone,
 * which is why read amp (10.69x) ran so far above write amp (2.93x).
 *
 * The invariant that kills it for good: total asked <= NOX_DATAZONE_SIZE, and no
 * block fetched more than once. Stage-1 is single-threaded, so every duplicate
 * round trip landed directly on drain time.
 */
static void test_read_holes_no_duplicate_blocks(void)
{
    for (size_t i = 0; i < FAKE_DISK_SIZE; i++)
        g_disk[i] = (uint8_t)(i & 0xff);

    uint8_t *expect = malloc(NOX_DATAZONE_SIZE);
    assert(expect != NULL);
    memcpy(expect, g_disk, NOX_DATAZONE_SIZE);

    scrap_page_t *p = scrap_page_alloc(0, 0);
    assert(p != NULL);
    memset(p->data, 0xE7, NOX_DATAZONE_SIZE);

    /* Stride 4093, not 4096: a prime-ish stride keeps every segment straddling a
     * block boundary, which is exactly the shape that produced the duplicates. */
    uint8_t seg[9];
    memset(seg, 0x44, sizeof(seg));
    uint32_t nseg = 0;
    for (uint32_t k = 0; k < NOX_MAX_ENTRIES; k++) {
        uint32_t off = k * 4093u;
        if (off + sizeof(seg) > NOX_DATAZONE_SIZE)
            break;
        assert(scrap_page_merge(p, seg, off, sizeof(seg)) == SCRAP_OK);
        memcpy(expect + off, seg, sizeof(seg));
        nseg++;
    }
    assert(nseg >= 2);

    scrap_entry_t holes[NOX_MAX_ENTRIES + 1];
    uint32_t nh = scrap_page_hole_ranges(p, holes, NOX_MAX_ENTRIES + 1);

    void *scratch = NULL;
    assert(posix_memalign(&scratch, NOX_BLOCK_SIZE, NOX_DATAZONE_SIZE) == 0);

    g_preads = 0;
    memset(g_block_reads, 0, sizeof(g_block_reads));
    uint64_t asked = 0;
    assert(scrap_page_read_holes(p->base, 7, holes, nh, scratch, &asked) == 0);

    assert(asked <= NOX_DATAZONE_SIZE);          /* read amp <= 1x per pass */
    assert(g_preads <= (int)(NOX_DATAZONE_SIZE / NOX_BLOCK_SIZE));
    for (uint32_t b = 0; b < NOX_DATAZONE_SIZE / NOX_BLOCK_SIZE; b++)
        assert(g_block_reads[b] <= 1);           /* no block fetched twice */

    /* Coalescing must not change WHAT lands in the page: same correctness bar as
     * the poison test, on the fragmented shape. */
    scrap_page_apply_holes(p, scratch);
    assert(scrap_page_writeback(p, 7) == 0);
    assert(memcmp(g_disk, expect, NOX_DATAZONE_SIZE) == 0);

    free(scratch);
    free(expect);
    scrap_page_free(p);
    printf("  no duplicate blocks      OK (%u segs, %u holes, %d preads, %llu B)\n",
           nseg, nh, g_preads, (unsigned long long)asked);
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
    test_read_holes_skips_gaps();
    test_read_holes_no_duplicate_blocks();
    printf("holes_test: PASS\n");

    free(g_disk);
    return 0;
}
