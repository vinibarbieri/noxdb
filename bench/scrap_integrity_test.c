/*
 * scrap_integrity_test.c - C2 acceptance gate: single-thread scrap-path integrity.
 *
 * Gate (KANBAN C2-GATE): write -> flush -> read-back -> memcmp passes.
 *
 * Strategy:
 *   - Drive small / unaligned / boundary-straddling writes through nox_write so
 *     every one takes the SCRAP path (never the >=1MB 4K-aligned fast path).
 *   - Maintain a RAM "shadow" image of what each touched 256KB region SHOULD
 *     contain. The shadow uses the SAME 256KB page-split loop as the engine, so
 *     a boundary-straddling write is mirrored automatically.
 *   - nox_close() flushes all partial pages (read-before-write) to the SSD.
 *   - Re-read every touched region from the SSD with a SEPARATE raw O_DIRECT fd
 *     (NOT the engine - we must not verify the engine using the engine) and
 *     memcmp against the shadow.
 *
 * Zero-initialised shadow is correct: the backing file is unlinked first (fresh,
 * so holes read back as 0), scrap_page_alloc zeroes the data zone, and the
 * partial-flush read-before-write preserves untouched hole bytes.
 *
 * Each case lives in its own 256KB region so a failure names the exact behaviour
 * that broke. Build + run ON THE BENCH BOX against /mnt/nvme (O_DIRECT only works
 * on the real NVMe filesystem):
 *
 *     make gate && ./bench/scrap_integrity_test /mnt/nvme/c2gate.dat
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REG NOX_DATAZONE_SIZE   /* 256KB, the scrap-page region size */

/* ---- shadow image: one zeroed 256KB buffer per region we touch ------------ */

typedef struct {
    uint64_t base;      /* 256KB-aligned region base */
    uint8_t *shadow;    /* expected on-disk contents of this region */
} region_t;

static region_t regs[64];
static int      nregs;

/* Find (or lazily create) the shadow buffer for the region covering `base`. */
static region_t *reg_for(uint64_t base)
{
    for (int i = 0; i < nregs; i++)
        if (regs[i].base == base)
            return &regs[i];

    if (nregs == (int)(sizeof(regs) / sizeof(regs[0]))) {
        fprintf(stderr, "region table full\n");
        exit(2);
    }
    void *buf = calloc(1, REG);   /* zero-init == fresh-file holes */
    if (!buf) {
        perror("calloc shadow");
        exit(2);
    }
    regs[nregs].base   = base;
    regs[nregs].shadow = buf;
    return &regs[nregs++];
}

/*
 * Issue a write to the engine AND mirror it into the shadow. The shadow update
 * walks the SAME per-256KB-page split the engine uses, so a write straddling a
 * region boundary lands in both regions' shadows exactly as it will on disk.
 */
static int do_write(nox_engine_t *e, uint64_t off, const void *buf, uint32_t len)
{
    const uint8_t *src = buf;
    uint64_t cur = off;
    uint32_t rem = len;

    while (rem > 0) {
        uint64_t base  = NOX_PAGE_BASE(cur);
        uint32_t intra = (uint32_t)(cur - base);
        uint32_t chunk = REG - intra;
        if (chunk > rem)
            chunk = rem;
        memcpy(reg_for(base)->shadow + intra, src, chunk);
        src += chunk;
        cur += chunk;
        rem -= chunk;
    }
    return nox_write(e, buf, len, off);
}

/* Fill a malloc'd buffer with a repeating byte and write it via do_write. */
static int write_pattern(nox_engine_t *e, uint64_t off, uint32_t len, uint8_t byte)
{
    uint8_t *b = malloc(len);
    if (!b) {
        perror("malloc pattern");
        exit(2);
    }
    memset(b, byte, len);
    int rc = do_write(e, off, b, len);
    free(b);
    return rc;
}

/* ---- verification: raw O_DIRECT read-back vs shadow ------------------------ */

/* Returns 0 if every touched region matches disk, else the count of mismatches. */
static int verify(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open (read-back)");
        return -1;
    }

    void *disk = NULL;
    if (posix_memalign(&disk, NOX_BLOCK_SIZE, REG) != 0) {
        fprintf(stderr, "posix_memalign read buffer failed\n");
        close(fd);
        return -1;
    }

    int fails = 0;
    for (int i = 0; i < nregs; i++) {
        ssize_t r = pread(fd, disk, REG, (off_t)regs[i].base);
        if (r != (ssize_t)REG) {
            fprintf(stderr, "  region @%llu: short/failed read (%zd, errno=%d)\n",
                    (unsigned long long)regs[i].base, r, errno);
            fails++;
            continue;
        }
        if (memcmp(disk, regs[i].shadow, REG) != 0) {
            /* Locate first mismatch for a useful diagnostic. */
            size_t j = 0;
            while (j < REG && ((uint8_t *)disk)[j] == regs[i].shadow[j])
                j++;
            fprintf(stderr,
                    "  region @%llu: MISMATCH at intra +%zu (disk=0x%02x shadow=0x%02x)\n",
                    (unsigned long long)regs[i].base, j,
                    ((uint8_t *)disk)[j], regs[i].shadow[j]);
            fails++;
        }
    }

    free(disk);
    close(fd);
    return fails;
}

/* ---- the gate ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /mnt/nvme/c2gate.dat\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    /* Fresh file so holes read back as 0 (io_direct_open has no O_TRUNC). */
    unlink(path);

    nox_engine_t *e = nox_open(path);
    if (!e) {
        perror("nox_open");
        return 2;
    }

    int rc = 0;

    /* Case 1 - adjacent segments fuse (region 0). */
    rc |= write_pattern(e, 0 * REG + 1000, 1000, 'A');
    rc |= write_pattern(e, 0 * REG + 2000, 1000, 'B');   /* touches [1000,3000) */

    /* Case 2 - hole between two segments stays zero (region 1). */
    rc |= write_pattern(e, 1 * REG + 1000, 1000, 'C');
    rc |= write_pattern(e, 1 * REG + 5000, 1000, 'D');   /* gap [2000,5000) = 0 */

    /* Case 3 - overlapping write overwrites the middle (region 2). */
    rc |= write_pattern(e, 2 * REG + 1000, 2000, 'E');   /* [1000,3000) */
    rc |= write_pattern(e, 2 * REG + 2000, 2000, 'F');   /* [2000,4000) over E */

    /* Case 4 - write straddling a 256KB boundary splits across regions 3+4. */
    rc |= write_pattern(e, 3 * REG + (REG - 144), 500, 'G'); /* 144 -> r3, 356 -> r4 */

    /* Case 5 - 16 disjoint segments force overflow (region 5): flush + fresh
     * page + retry. Final region must hold all 16 segments. */
    for (int k = 0; k < 16; k++)
        rc |= write_pattern(e, 5 * REG + (uint64_t)k * 1000, 100, (uint8_t)('a' + k));

    /* Case 6 - full 256KB region takes the full-flush path (no read-before-write)
     * and is flushed inline (region 8). */
    rc |= write_pattern(e, 8 * REG, REG, 'Z');

    if (rc != 0)
        fprintf(stderr, "WARNING: a nox_write returned an error during load\n");

    /* Flush all remaining partial pages to the SSD and close. */
    if (nox_close(e) != 0) {
        fprintf(stderr, "nox_close reported a flush error\n");
        rc = 1;
    }

    /* Read back from the SSD (raw O_DIRECT) and compare. */
    int fails = verify(path);
    if (fails < 0)
        return 2;

    if (fails == 0 && rc == 0) {
        printf("C2-GATE PASS: %d region(s) match disk (scrap path integrity)\n",
               nregs);
        return 0;
    }
    printf("C2-GATE FAIL: %d region mismatch(es), write_rc=%d\n", fails, rc);
    return 1;
}
