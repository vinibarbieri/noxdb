/*
 * o_direct_probe.c - The O_DIRECT alignment battle, demonstrated in 3 tests.
 *
 * Standalone teaching artifact for C0. NO engine dependency: raw open/write/
 * pwrite + malloc/posix_memalign + mincore only. Writes 10 MB three ways and
 * measures, for each, whether the data landed in the PAGE CACHE or bypassed it:
 *
 *   1. malloc + buffered write()        -> CACHED     (page cache, as designed)
 *   2. malloc(misaligned) + O_DIRECT    -> depends on kernel (see below)
 *   3. posix_memalign(4096) + O_DIRECT  -> NOT cached  (true kernel bypass)
 *
 * WHY mincore and not EINVAL:
 *   The old legacy DIO path (__blockdev_direct_IO) returned a hard EINVAL when
 *   the memory buffer was misaligned. The modern iomap path (XFS >= 4.10, ext4
 *   >= ~5.5) does NOT: on a misaligned buffer it SILENTLY FALLS BACK to buffered
 *   I/O -- no error, but O_DIRECT is lost and the write goes through the cache.
 *   For this engine that silent fallback is worse than EINVAL: we'd lose kernel
 *   bypass (and our tail-latency guarantees) with no signal. So Test 2 no longer
 *   asserts EINVAL; it asserts the OBSERVABLE consequence -- did the page cache
 *   get populated? -- which is true on BOTH kernels (fallback=cached,
 *   EINVAL=write failed=nothing written). We still print the loud EINVAL banner
 *   if the legacy kernel does throw it, so the lesson is visible either way.
 *
 * Detection: after each write we mmap the file read-only and call mincore(),
 * which reports, per page, whether it is resident in the page cache -- WITHOUT
 * faulting the pages in (we never dereference the mapping). O_DIRECT bypass
 * leaves the file's data pages out of the cache; buffered I/O leaves them in.
 *
 * O_DIRECT + mincore semantics need a real block-backed filesystem (XFS/EXT4 on
 * the NVMe). Will NOT behave correctly on macOS/tmpfs/overlay.
 *
 *   usage: ./o_direct_probe [backing-file-on-nvme]     (default: ./probe.dat)
 *
 * Exit status:
 *   0  -> Test 1 cached AND Test 3 not-cached (the two invariants that must hold
 *         on every kernel). Test 2 is diagnostic and never fails the run.
 *   1  -> an invariant was violated (wrong FS? O_DIRECT ignored? not NVMe?).
 */
#define _GNU_SOURCE          /* O_DIRECT (fcntl.h) + mincore (sys/mman.h) gating */
#include "noxdb_config.h" /* NOX_BLOCK_SIZE, NOX_IS_ALIGNED */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>        /* mmap, mincore */

/* 10 MB payload. 10 * 1024 * 1024 == 2560 * 4096, so the transfer LENGTH is
 * already a multiple of the block size (alignment rule #3 always satisfied).
 * That isolates the variable under test to the BUFFER ADDRESS (rule #1). */
#define PROBE_SIZE (10u * 1024u * 1024u)

/* Loud, explicit EINVAL banner mandated by CLAUDE.md §2 / docs/02. Only fires on
 * legacy-DIO kernels that still reject misaligned buffers with EINVAL. */
static void report_einval(const char *ctx)
{
    fprintf(stderr,
            "\n"
            "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
            "!! O_DIRECT alignment violation  (%s)\n"
            "!! errno=EINVAL: buffer addr, file offset, or length is not\n"
            "!! a multiple of %u bytes.  (docs/02 §1)\n"
            "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
            ctx, NOX_BLOCK_SIZE);
}

/*
 * Write PROBE_SIZE bytes from `buf` at offset 0 using pwrite().
 * pwrite (not write) avoids touching the shared file offset -- house rule from
 * CLAUDE.md §2, and it keeps each test independent.
 * Returns 0 on full write, -1 on error (errno preserved for the caller).
 */
static int write_all(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    const char *p = buf;
    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, (off_t)done);
        if (n < 0)
            return -1;          /* errno set by pwrite */
        done += (size_t)n;
    }
    return 0;
}

/*
 * How many of the first `len` bytes of `path` are resident in the page cache.
 *
 * We mmap the file read-only and call mincore(), which fills a per-page vector
 * with the residency bit. CRITICAL: we never dereference the mapping -- a single
 * read would fault the page in and corrupt the very measurement we are taking.
 * Returns the count of resident pages, or -1 on error.
 */
static long resident_pages(const char *path, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open (mincore)"); return -1; }

    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) pgsz = NOX_BLOCK_SIZE;
    size_t npages = (len + (size_t)pgsz - 1) / (size_t)pgsz;

    /* MAP_SHARED so residency reflects the shared page cache, not private COW. */
    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap (mincore)"); close(fd); return -1; }

    unsigned char *vec = malloc(npages);
    if (!vec) { munmap(m, len); close(fd); return -1; }

    long resident = -1;
    if (mincore(m, len, vec) == 0) {
        resident = 0;
        for (size_t i = 0; i < npages; i++)
            if (vec[i] & 1u)      /* LSB = page is resident in core */
                resident++;
    } else {
        perror("mincore");
    }

    free(vec);
    munmap(m, len);
    close(fd);
    return resident;
}

/* Print the residency verdict for a test. `expect_cached` drives the label.
 * Returns 1 if the observed state MATCHES expectation, 0 if it violates it. */
static int verdict(const char *tag, long resident, long total, int expect_cached)
{
    /* "cached" = a clear majority of pages resident; "bypassed" = almost none.
     * We use halves rather than exact counts to tolerate a stray metadata page
     * or an evicted tail under memory pressure. */
    int is_cached = (resident > total / 2);
    const char *state = is_cached ? "CACHED (page cache populated)"
                                  : "NOT cached (true O_DIRECT bypass)";
    printf("    %s: %ld/%ld pages resident  ->  %s\n", tag, resident, total, state);

    if (expect_cached < 0)          /* diagnostic-only, never a pass/fail */
        return 1;
    return (is_cached == (expect_cached != 0));
}

/* ---- Test 1: malloc + buffered write. Must be CACHED (the baseline). --- */
static int test_buffered_malloc(const char *path, long total_pages)
{
    printf("[1] malloc + buffered write() (no O_DIRECT)\n");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }

    void *buf = malloc(PROBE_SIZE);
    if (!buf) { fprintf(stderr, "malloc failed\n"); close(fd); return -1; }
    memset(buf, 0xA1, PROBE_SIZE);

    int rc = write_all(fd, buf, PROBE_SIZE);
    if (rc != 0) perror("write_all");
    free(buf);
    close(fd);
    if (rc != 0) return -1;

    /* Buffered write goes THROUGH the page cache -> pages must be resident. */
    long r = resident_pages(path, PROBE_SIZE);
    if (r < 0) return -1;
    return verdict("[1]", r, total_pages, /*expect_cached=*/1) ? 0 : -1;
}

/* ---- Test 2: malloc (forced misalign) + O_DIRECT. Diagnostic. --------- */
static int test_odirect_malloc(const char *path, long total_pages)
{
    printf("[2] malloc (misaligned) + O_DIRECT  (kernel-dependent)\n");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { perror("open O_DIRECT"); return -1; }

    /* GOTCHA: a plain malloc(10MB) on glibc is served by mmap() and comes back
     * PAGE-ALIGNED by luck -- which would let O_DIRECT wrongly succeed cleanly
     * and hide the lesson. Over-allocate and deliberately shift off the 4K
     * boundary: O_DIRECT demands EXPLICIT alignment; never trust malloc's. */
    void *raw = malloc(PROBE_SIZE + NOX_BLOCK_SIZE);
    if (!raw) { fprintf(stderr, "malloc failed\n"); close(fd); return -1; }
    char *buf = (char *)raw + 8;              /* +8 => guaranteed NOT 4K-aligned */
    if (NOX_IS_ALIGNED(buf)) buf += 8;        /* paranoia: nudge again if unlucky */
    memset(buf, 0xB2, PROBE_SIZE);

    errno = 0;
    int rc = write_all(fd, buf, PROBE_SIZE);
    int saved = errno;
    free(raw);
    close(fd);

    if (rc != 0) {
        /* LEGACY kernel: hard EINVAL on misaligned buffer. Nothing was written,
         * so there is nothing in the cache to measure. Show the banner + note. */
        if (saved == EINVAL) {
            report_einval("test 2: malloc misaligned buffer");
            printf("    [2] -> LEGACY DIO path: EINVAL, write rejected. "
                   "Alignment enforced by the kernel.\n");
            return 0;   /* expected on legacy kernels; not a failure */
        }
        errno = saved;
        perror("write_all (unexpected non-EINVAL error)");
        return -1;
    }

    /* MODERN kernel: the write "succeeded" -- but did it stay direct, or did
     * iomap silently fall back to buffered I/O? mincore tells us. Fallback =>
     * pages resident (CACHED). This is the silent-fallback demo. */
    printf("    [2] -> write returned success; checking for SILENT FALLBACK...\n");
    long r = resident_pages(path, PROBE_SIZE);
    if (r < 0) return -1;
    if (r > total_pages / 2)
        printf("    [2] !! iomap SILENTLY fell back to buffered I/O "
               "(O_DIRECT bypass LOST). This is the danger the engine "
               "guards against with NOX_IS_ALIGNED asserts.\n");
    /* Diagnostic only: never fails the probe (behaviour is kernel-dependent). */
    return verdict("[2]", r, total_pages, /*expect_cached=*/-1) ? 0 : 0;
}

/* ---- Test 3: posix_memalign + O_DIRECT. Must NOT be cached. ----------- */
static int test_odirect_aligned(const char *path, long total_pages)
{
    printf("[3] posix_memalign(4096) + O_DIRECT  (expect true bypass)\n");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { perror("open O_DIRECT"); return -1; }

    /* posix_memalign guarantees the address is a multiple of NOX_BLOCK_SIZE
     * (rule #1). Offset 0 (rule #2) and 4K-multiple length (rule #3) are already
     * met, so the kernel has no reason to fall back -- this is true O_DIRECT. */
    void *buf = NULL;
    if (posix_memalign(&buf, NOX_BLOCK_SIZE, PROBE_SIZE) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        close(fd);
        return -1;
    }
    memset(buf, 0xC3, PROBE_SIZE);

    errno = 0;
    int rc = write_all(fd, buf, PROBE_SIZE);
    if (rc != 0) {
        if (errno == EINVAL) report_einval("test 3: this should NOT happen");
        else perror("write_all");
        free(buf);
        close(fd);
        return -1;
    }
    free(buf);
    close(fd);

    /* True O_DIRECT bypasses (and invalidates) the page cache -> not resident. */
    long r = resident_pages(path, PROBE_SIZE);
    if (r < 0) return -1;
    return verdict("[3]", r, total_pages, /*expect_cached=*/0) ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "./probe.dat";

    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) pgsz = NOX_BLOCK_SIZE;
    long total_pages = (long)(PROBE_SIZE / (size_t)pgsz);

    printf("O_DIRECT cache-residency probe -- 10 MB three ways\n");
    printf("backing file: %s\n", path);
    printf("block size:   %u bytes   page size: %ld bytes   pages: %ld\n\n",
           NOX_BLOCK_SIZE, pgsz, total_pages);

    /* Test 1 and Test 3 carry the invariants; Test 2 is a kernel diagnostic. */
    int fail = 0;
    fail |= (test_buffered_malloc(path, total_pages) != 0);
    fail |= (test_odirect_malloc(path,  total_pages) != 0);
    fail |= (test_odirect_aligned(path, total_pages) != 0);

    unlink(path);   /* clean up the scratch file */

    printf("\n%s\n",
           fail ? "PROBE FAILED (an invariant was violated -- check the FS is XFS/EXT4 on NVMe)"
                : "PROBE OK (buffered=cached, aligned O_DIRECT=bypassed)");
    return fail ? 1 : 0;
}
