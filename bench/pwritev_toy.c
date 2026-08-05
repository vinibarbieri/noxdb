/*
 * pwritev_toy.c — C4-S3 study toy: scatter-gather I/O and short-write fixup.
 *
 * Three independent parts:
 *
 *   A) 3 separately-allocated 4K-aligned buffers -> ONE pwritev -> read back ->
 *      memcmp. Demonstrates "gather": N scattered RAM buffers become ONE
 *      contiguous disk extent. Needs O_DIRECT => bench box only.
 *
 *   B) The same call with one iov_len deliberately NOT a multiple of 4096,
 *      while the TOTAL stays a multiple of 4096. Must fail with EINVAL. This is
 *      the proof that O_DIRECT's three alignment rules (docs/02 §1) apply
 *      PER-IOVEC, not to the aggregate. Bench box only.
 *
 *   C) The short-write fixup, driven by a fake writer so every branch is
 *      deterministic. Pure RAM: runs on the laptop, no disk, no O_DIRECT.
 *      pwritev_all() below is written in the shape it will take when it is
 *      lifted into src/io_direct.c during C4-B4.
 *
 *   Build : make toy-pwritev
 *   Run   : ./bench/pwritev_toy                      # part C only (laptop)
 *           ./bench/pwritev_toy /mnt/nvme/s3toy.dat  # A + B + C (bench box)
 *
 * ---------------------------------------------------------------------------
 * THE FIXUP RULE (the point of part C)
 *
 * pwritev() returns a BYTE count, not a count of completed iovecs. Resuming a
 * partial write therefore looks like it needs pointer arithmetic on iov_base —
 * and under O_DIRECT that arithmetic can produce an unaligned buffer address or
 * an unaligned length, which the kernel then rejects.
 *
 * We sidestep the whole problem: NEVER SPLIT AN IOVEC. Drop the iovecs that
 * completed in full and redo the first incomplete one FROM ITS START. iov_base
 * and iov_len are never modified, so all three alignment rules keep holding by
 * construction, and the "what if the returned count is not a multiple of 4096"
 * case simply stops existing — the count is only ever used to COUNT completed
 * iovecs, never to compute an address.
 *
 * The price is rewriting up to (iov_len - 1) bytes that were already on disk.
 * That is safe here, and the precondition must be stated explicitly:
 *
 *   IDEMPOTENCE. The source buffer is stable for the duration of the call (in
 *   the engine: the page is NOX_TAG_FLUSHING and already detached from the
 *   index, spec §4.2, so no writer can reach it) and the destination is a fixed
 *   offset, never an append. Re-writing a range with identical bytes is a
 *   semantic no-op. In an append-only log this trick would corrupt data.
 * ---------------------------------------------------------------------------
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "noxdb_config.h"

/* Max iovecs per Stage-2 pwritev (spec §5.7): 8 * 256KB = 2MB per syscall.
 * Defined here rather than pulled from noxdb_config.h because C4-B4 is the card
 * that adds it to the engine config; the toy must not pre-empt that. */
#define TOY_MAX_IOV      8u

/* Buffers used by parts A and C: 3 * 256KB. */
#define TOY_NBUF         3
#define TOY_BUFLEN       NOX_DATAZONE_SIZE
#define TOY_TOTAL        ((size_t)TOY_NBUF * TOY_BUFLEN)

/*
 * How many consecutive calls that make ZERO iovec-level progress we tolerate
 * before declaring the write dead. Without this the retry loop is an infinite
 * loop: a writer stuck returning the same short count would be re-issued the
 * identical call forever, and the single Stage-2 thread would never serve
 * another page — nox_close() would hang. EINTR consumes the same budget: both
 * mean "that call advanced nothing".
 */
#define TOY_MAX_STALL    4

/* ---------------------------------------------------------------- reporting */

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

/* Same shape as io_direct.c's report_einval, kept loud per CLAUDE.md §2. */
static void report_einval_iov(off_t off, const struct iovec *iov, int iovcnt)
{
    fprintf(stderr,
            "\n*** O_DIRECT alignment violation ***\n"
            "    op=pwritev offset=%lld iovcnt=%d  offset%%4096=%lld\n",
            (long long)off, iovcnt, (long long)(off % NOX_BLOCK_SIZE));
    for (int i = 0; i < iovcnt; i++)
        fprintf(stderr,
                "    iov[%d] base=%p len=%zu   base%%4096=%lu len%%4096=%zu\n",
                i, iov[i].iov_base, iov[i].iov_len,
                (unsigned long)((uintptr_t)iov[i].iov_base & (NOX_BLOCK_SIZE - 1)),
                iov[i].iov_len % NOX_BLOCK_SIZE);
    fprintf(stderr,
            "    (O_DIRECT needs the offset, and EVERY iov_base and iov_len,\n"
            "     aligned to %u bytes — the total being aligned is NOT enough)\n\n",
            NOX_BLOCK_SIZE);
}

/* ------------------------------------------------------- the fixup itself */

/*
 * Pure function, no I/O: how many leading iovecs are fully covered by `n`
 * bytes. *bytes_done receives the sum of those iovecs' lengths, which is what
 * the file offset must advance by. Any remainder of `n` that falls INSIDE the
 * next iovec is deliberately ignored — those bytes get rewritten (see the
 * idempotence note at the top).
 *
 * Never modifies the iovec array. That is the whole trick.
 */
static int iov_complete_count(const struct iovec *iov, int iovcnt,
                              size_t n, size_t *bytes_done)
{
    size_t done = 0;
    int    k    = 0;

    while (k < iovcnt && n >= iov[k].iov_len) {
        n    -= iov[k].iov_len;
        done += iov[k].iov_len;
        k++;
    }
    *bytes_done = done;
    return k;
}

/*
 * Injection point for part C: the real syscall in production, a scripted fake
 * in the unit test. Same signature as pwritev plus an opaque context.
 */
typedef ssize_t (*writev_fn)(void *ctx, int fd, const struct iovec *iov,
                             int iovcnt, off_t off);

/*
 * Write every byte of every iovec, or fail. Destined for src/io_direct.c as
 * io_direct_pwritev_all() in C4-B4.
 *
 * Returns the total byte count on success, -1 (errno set) on failure. On
 * failure the caller must treat the write as lost: OTflush latches it into its
 * sticky error field, which surfaces through otflush_stop() -> nox_close()
 * (spec §5.2).
 */
static ssize_t pwritev_all(writev_fn wr, void *ctx, int fd,
                           struct iovec *iov, int iovcnt, off_t off)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    int   i       = 0;        /* first iovec not yet fully written */
    off_t cur_off = off;      /* file offset matching iov[i]'s start */
    int   stall   = TOY_MAX_STALL;

    while (i < iovcnt) {
        ssize_t w = wr(ctx, fd, &iov[i], iovcnt - i, cur_off);

        if (w < 0) {
            if (errno == EINTR) {
                /* A signal interrupted the call before any transfer. Retrying
                 * the identical call is correct, but it advanced nothing, so it
                 * spends stall budget like any other no-progress event. */
                if (--stall < 0) {
                    fprintf(stderr, "pwritev_all: EINTR storm, giving up\n");
                    errno = EINTR;
                    return -1;
                }
                continue;
            }
            if (errno == EINVAL)
                report_einval_iov(cur_off, &iov[i], iovcnt - i);
            return -1;
        }

        if (w == 0) {
            /* No error, no bytes. There is nothing to resume from and no reason
             * to expect the next attempt to differ. Fatal. */
            fprintf(stderr,
                    "pwritev_all: kernel reported 0 bytes written with no error "
                    "at offset %lld — treating as fatal\n", (long long)cur_off);
            errno = EIO;
            return -1;
        }

        if (w % NOX_BLOCK_SIZE) {
            /* We do not NEED this value to be aligned (we never do arithmetic
             * with it), but under O_DIRECT it should not happen: it means the
             * write was cut short mid-block, i.e. ENOSPC or a signal landed in
             * the middle. Silence here would be as bad as silencing EINVAL
             * (docs/02 §1), so say it out loud. */
            fprintf(stderr,
                    "pwritev_all: WARNING short write of %zd bytes is not a "
                    "multiple of %u at offset %lld (ENOSPC? signal?)\n",
                    w, NOX_BLOCK_SIZE, (long long)cur_off);
        }

        size_t done = 0;
        int    k    = iov_complete_count(&iov[i], iovcnt - i, (size_t)w, &done);

        if (k == 0) {
            /* Not even the first iovec completed. Nothing to advance past, so
             * the next call is byte-for-byte identical — bound the retries or
             * loop forever. */
            if (--stall < 0) {
                fprintf(stderr,
                        "pwritev_all: no iovec completed after %d attempts "
                        "(stuck at offset %lld), giving up\n",
                        TOY_MAX_STALL + 1, (long long)cur_off);
                errno = EIO;
                return -1;
            }
            continue;
        }

        stall    = TOY_MAX_STALL;      /* real progress: refill the budget */
        i       += k;
        cur_off += (off_t)done;
        /* Note: (w - done) bytes landed inside iov[i] and are discarded. That
         * region is rewritten by the next call. Idempotent — see header. */
    }

    return (ssize_t)total;
}

/* ------------------------------------------------------------- test buffers */

/* Distinct, position-sensitive pattern per buffer, so memcmp catches a buffer
 * written to the wrong offset as well as plain corruption. */
static void fill_pattern(uint8_t *p, size_t len, int which)
{
    for (size_t j = 0; j < len; j++)
        p[j] = (uint8_t)((0x10 * (which + 1)) + (j & 0x0F));
}

static uint8_t *alloc_zone(void)
{
    void *p = NULL;
    if (posix_memalign(&p, NOX_BLOCK_SIZE, TOY_BUFLEN) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        exit(1);
    }
    return p;
}

/* ============================================================ PART C (RAM) */

typedef struct {
    ssize_t ret;   /* >=0: pretend this many bytes went through. -1: fail. */
    int     err;   /* errno to set when ret == -1 */
} canned_t;

typedef struct {
    uint8_t        *disk;         /* fake disk image */
    size_t          disk_len;
    const canned_t *script;
    int             nscript;
    int             repeat_last;  /* past the script, keep replaying its last entry */
    int             calls;
    off_t           seen_off[16]; /* per-call offset, so the test can assert the
                                   * resume points, not just the final bytes */
    int             seen_cnt[16]; /* per-call iovcnt */
} fake_t;

/*
 * Fake writer. Honours the script, and actually copies the bytes it claims to
 * have written into the fake disk — so the test can memcmp the final image and
 * prove the fixup produced correct CONTENT, not merely plausible offsets.
 */
static ssize_t fake_pwritev(void *ctx, int fd, const struct iovec *iov,
                            int iovcnt, off_t off)
{
    fake_t *f = ctx;
    (void)fd;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    /* Default when the script runs out: full success. Keeps the scripts short —
     * they only describe the failures under test. */
    canned_t c = { .ret = (ssize_t)total, .err = 0 };
    if (f->calls < f->nscript)
        c = f->script[f->calls];
    else if (f->repeat_last && f->nscript > 0)
        c = f->script[f->nscript - 1];

    if (f->calls < 16) {
        f->seen_off[f->calls] = off;
        f->seen_cnt[f->calls] = iovcnt;
    }
    f->calls++;

    if (c.ret < 0) {
        errno = c.err;
        return -1;
    }

    size_t n = (size_t)c.ret;
    if (n > total)
        n = total;

    /* Gather exactly n bytes across the iovecs into the disk at `off`. */
    size_t left = n;
    size_t pos  = (size_t)off;
    for (int i = 0; i < iovcnt && left > 0; i++) {
        size_t chunk = iov[i].iov_len < left ? iov[i].iov_len : left;
        if (pos + chunk <= f->disk_len)
            memcpy(f->disk + pos, iov[i].iov_base, chunk);
        pos  += chunk;
        left -= chunk;
    }
    return (ssize_t)n;
}

static void part_c(void)
{
    printf("\n=== PART C — short-write fixup (fake writer, pure RAM) ===\n");

    uint8_t *buf[TOY_NBUF];
    uint8_t *expect = malloc(TOY_TOTAL);
    uint8_t *disk   = malloc(TOY_TOTAL);
    if (!expect || !disk) { fprintf(stderr, "OOM\n"); exit(1); }

    for (int b = 0; b < TOY_NBUF; b++) {
        buf[b] = alloc_zone();
        fill_pattern(buf[b], TOY_BUFLEN, b);
        memcpy(expect + (size_t)b * TOY_BUFLEN, buf[b], TOY_BUFLEN);
    }

    struct iovec iov[TOY_NBUF];

    /* Re-arm the iovecs before every case: pwritev_all must not mutate them,
     * and rebuilding here means a bug that DOES mutate them cannot hide. */
#define ARM()                                              \
    do {                                                   \
        for (int b = 0; b < TOY_NBUF; b++) {                \
            iov[b].iov_base = buf[b];                       \
            iov[b].iov_len  = TOY_BUFLEN;                   \
        }                                                   \
        memset(disk, 0xAA, TOY_TOTAL);                      \
    } while (0)

    /* ---- C1: happy path — one call, everything through ------------------ */
    {
        ARM();
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C1 happy path (no short write)\n");
        check(rc == (ssize_t)TOY_TOTAL, "returns the full byte count");
        check(f.calls == 1,             "exactly 1 syscall");
        check(memcmp(disk, expect, TOY_TOTAL) == 0, "disk image byte-identical");
    }

    /* ---- C2: short write landing exactly on an iovec boundary ----------- */
    {
        ARM();
        const canned_t s[] = { { TOY_BUFLEN, 0 } };   /* first call: 1 iovec only */
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C2 short write on an exact iovec boundary (256KB of 768KB)\n");
        check(rc == (ssize_t)TOY_TOTAL, "returns the full byte count");
        check(f.calls == 2,             "2 syscalls");
        check(f.seen_off[1] == (off_t)TOY_BUFLEN, "resumes at offset 256KB");
        check(f.seen_cnt[1] == TOY_NBUF - 1,      "resumes with 2 iovecs left");
        check(memcmp(disk, expect, TOY_TOTAL) == 0, "disk image byte-identical");
    }

    /* ---- C3: short write stopping in the MIDDLE of the third iovec ------ */
    {
        ARM();
        /* 700KB = 2 full iovecs (512KB) + 188KB into the third. The 188KB is
         * discarded and rewritten: that is the idempotence bet, made explicit. */
        const canned_t s[] = { { 700 * 1024, 0 } };
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C3 short write mid-iovec (700KB of 768KB -> 188KB rewritten)\n");
        check(rc == (ssize_t)TOY_TOTAL, "returns the full byte count");
        check(f.calls == 2,             "2 syscalls");
        check(f.seen_off[1] == (off_t)(2 * TOY_BUFLEN),
              "resumes at 512KB — the START of the partial iovec, not 700KB");
        check(f.seen_cnt[1] == 1,       "resumes with 1 iovec left");
        check(memcmp(disk, expect, TOY_TOTAL) == 0,
              "disk image byte-identical despite the overlap rewrite");
        check(iov[2].iov_base == buf[2] && iov[2].iov_len == TOY_BUFLEN,
              "iovec array untouched (no iov_base arithmetic)");
    }

    /* ---- C4: short count that is NOT a multiple of 4096 ----------------- */
    {
        ARM();
        /* 100003 is prime-ish garbage: not 4K-aligned, and smaller than one
         * iovec, so ZERO iovecs completed. Under a byte-granular fixup this is
         * the case that produces an unaligned iov_base and then EINVAL forever.
         * Here it costs one stall and a retry. Expect the WARNING on stderr. */
        const canned_t s[] = { { 100003, 0 } };
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C4 short count not a multiple of 4096 (100003 bytes)\n");
        check(rc == (ssize_t)TOY_TOTAL, "still completes");
        check(f.calls == 2,             "2 syscalls (retry of the identical call)");
        check(f.seen_off[1] == 0,       "retry is at the SAME offset 0");
        check(f.seen_cnt[1] == TOY_NBUF, "retry carries all 3 iovecs");
        check(memcmp(disk, expect, TOY_TOTAL) == 0, "disk image byte-identical");
    }

    /* ---- C5: zero bytes, no error -> fatal, must not spin --------------- */
    {
        ARM();
        const canned_t s[] = { { 0, 0 } };
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1, .repeat_last = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C5 writer returns 0 with no error\n");
        check(rc == -1,     "fails instead of looping");
        check(f.calls == 1, "gives up immediately — 0 is not retryable");
    }

    /* ---- C6: permanently stuck partial -> budget runs out --------------- */
    {
        ARM();
        /* Always 1000 bytes: forward progress in BYTES, zero progress in
         * IOVECS. The naive loop never terminates; the stall budget does. */
        const canned_t s[] = { { 1000, 0 } };
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1, .repeat_last = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C6 writer permanently stuck at 1000 bytes\n");
        check(rc == -1, "fails instead of hanging the Stage-2 thread");
        check(f.calls == TOY_MAX_STALL + 1, "bounded to TOY_MAX_STALL+1 attempts");
    }

    /* ---- C7: EINTR then success ----------------------------------------- */
    {
        ARM();
        const canned_t s[] = { { -1, EINTR } };
        fake_t f = { .disk = disk, .disk_len = TOY_TOTAL,
                     .script = s, .nscript = 1 };
        ssize_t rc = pwritev_all(fake_pwritev, &f, -1, iov, TOY_NBUF, 0);

        printf("C7 EINTR on the first call, then success\n");
        check(rc == (ssize_t)TOY_TOTAL, "retries and completes");
        check(f.calls == 2,             "2 syscalls");
        check(memcmp(disk, expect, TOY_TOTAL) == 0, "disk image byte-identical");
    }

#undef ARM

    for (int b = 0; b < TOY_NBUF; b++)
        free(buf[b]);
    free(expect);
    free(disk);
}

/* ====================================================== PARTS A + B (disk) */

#if defined(__linux__) && defined(O_DIRECT)

static void part_a_b(const char *path)
{
    printf("\n=== PARTS A/B — real pwritev with O_DIRECT on %s ===\n", path);

    int fd = open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        perror("open");
        g_fail++;
        return;
    }

    uint8_t *buf[TOY_NBUF];
    uint8_t *expect = malloc(TOY_TOTAL);
    if (!expect) { fprintf(stderr, "OOM\n"); exit(1); }

    for (int b = 0; b < TOY_NBUF; b++) {
        buf[b] = alloc_zone();
        fill_pattern(buf[b], TOY_BUFLEN, b);
        memcpy(expect + (size_t)b * TOY_BUFLEN, buf[b], TOY_BUFLEN);
    }

    /* The three buffers came from three independent posix_memalign calls, so
     * they are NOT adjacent in RAM — printing the addresses makes the "gather"
     * concrete: scattered memory, one contiguous disk extent. */
    printf("  buffers (separate allocations, non-adjacent in RAM):\n");
    for (int b = 0; b < TOY_NBUF; b++)
        printf("    buf[%d] = %p  (%%4096 = %lu)\n", b, (void *)buf[b],
               (unsigned long)((uintptr_t)buf[b] & (NOX_BLOCK_SIZE - 1)));

    /* ---- PART A: one pwritev, then read back and compare ---------------- */
    {
        struct iovec iov[TOY_NBUF];
        for (int b = 0; b < TOY_NBUF; b++) {
            iov[b].iov_base = buf[b];
            iov[b].iov_len  = TOY_BUFLEN;   /* 256KB = 64 * 4096: aligned */
        }

        /* pwritev, never writev+lseek: the fd is shared by background threads
         * in the engine, so the offset must be explicit (docs/02 §2). */
        ssize_t w = pwritev(fd, iov, TOY_NBUF, 0);
        if (w < 0 && errno == EINVAL)
            report_einval_iov(0, iov, TOY_NBUF);

        printf("A  one pwritev of %d x 256KB at offset 0\n", TOY_NBUF);
        check(w == (ssize_t)TOY_TOTAL, "pwritev wrote all 768KB in one syscall");

        /* Read the whole extent back in one aligned pread and compare against
         * the concatenation: proves the gather order is iov[0], iov[1], iov[2]
         * and that the disk region really is contiguous. */
        void *rb = NULL;
        if (posix_memalign(&rb, NOX_BLOCK_SIZE, TOY_TOTAL) != 0) {
            fprintf(stderr, "posix_memalign(readback) failed\n");
            exit(1);
        }
        memset(rb, 0, TOY_TOTAL);
        ssize_t r = pread(fd, rb, TOY_TOTAL, 0);
        check(r == (ssize_t)TOY_TOTAL, "read the 768KB back");
        check(memcmp(rb, expect, TOY_TOTAL) == 0,
              "read-back is byte-identical, in iovec order");
        free(rb);
    }

    /* ---- PART B1: per-iovec alignment rule, hard case ------------------- */
    {
        /* Total = 4097 + 4095 + 4096 = 12288 = 3 * 4096, PERFECTLY aligned in
         * aggregate. But iov[0] and iov[1] are odd — not a multiple of 4096,
         * of 512, or of anything. If the rule were about the TOTAL this would
         * succeed. It must not, on any device. */
        struct iovec iov[TOY_NBUF];
        iov[0].iov_base = buf[0]; iov[0].iov_len = 4097;
        iov[1].iov_base = buf[1]; iov[1].iov_len = 4095;
        iov[2].iov_base = buf[2]; iov[2].iov_len = 4096;

        size_t total = iov[0].iov_len + iov[1].iov_len + iov[2].iov_len;
        printf("B1 torn iov_len, odd sizes (total = %zu = %zu * 4096)\n",
               total, total / NOX_BLOCK_SIZE);

        errno = 0;
        ssize_t w = pwritev(fd, iov, TOY_NBUF, 1024 * 1024);   /* 1MB offset */
        int saved = errno;
        if (w < 0 && saved == EINVAL)
            report_einval_iov(1024 * 1024, iov, TOY_NBUF);

        check(w < 0 && saved == EINVAL,
              "rejected with EINVAL — alignment is PER-IOVEC, not on the total");
        if (w >= 0)
            fprintf(stderr,
                    "  !! wrote %zd bytes instead of failing. This fs is not\n"
                    "     honouring O_DIRECT, or the file is not on it.\n", w);
    }

    /* ---- PART B2: what IS the device's alignment unit? ------------------ */
    {
        /*
         * INFORMATIONAL, not pass/fail. These lengths are multiples of 512 but
         * NOT of 4096. The kernel checks O_DIRECT alignment against the
         * underlying device's LOGICAL block size (and the fs sector size) — not
         * against our 4096. So the outcome here measures the hardware:
         *
         *   accepted -> logical block size is 512 (a "512e" device). Our 4096
         *               is then a POLICY, stricter than the kernel demands.
         *   EINVAL   -> logical block size is 4096 (a "4Kn" device), and our
         *               constant coincides with the hard requirement.
         *
         * Either way NoxDB is correct: 4096 is a multiple of 512, so every I/O
         * the engine issues is legal on both kinds of device. The reason to
         * keep 4096 regardless: it is the NAND's physical block size and the
         * page size, so sub-4K writes trigger read-modify-write inside the SSD;
         * and 4Kn devices exist, where 512 would fail outright.
         */
        struct iovec iov[TOY_NBUF];
        iov[0].iov_base = buf[0]; iov[0].iov_len = 4096 + 512;   /* 4608 = 9*512 */
        iov[1].iov_base = buf[1]; iov[1].iov_len = 4096 - 512;   /* 3584 = 7*512 */
        iov[2].iov_base = buf[2]; iov[2].iov_len = 4096;

        printf("B2 probe: iov_len multiples of 512 but not of 4096\n");

        errno = 0;
        ssize_t w = pwritev(fd, iov, TOY_NBUF, 2 * 1024 * 1024); /* 2MB offset */
        int saved = errno;

        if (w < 0 && saved == EINVAL) {
            printf("  [INFO] EINVAL -> this device/fs demands 4096 (4Kn). Our\n"
                   "         NOX_BLOCK_SIZE matches the hard requirement.\n");
        } else if (w >= 0) {
            printf("  [INFO] accepted (%zd bytes) -> logical block size is 512\n"
                   "         (512e device). NOX_BLOCK_SIZE=4096 is a stricter\n"
                   "         POLICY, not the kernel minimum. Confirm with:\n"
                   "           lsblk -o NAME,LOG-SEC,PHY-SEC\n"
                   "           xfs_info /mnt/nvme | grep sectsz\n", w);
        } else {
            printf("  [INFO] failed with errno=%d (%s) — unexpected, investigate\n",
                   saved, strerror(saved));
        }
    }

    for (int b = 0; b < TOY_NBUF; b++)
        free(buf[b]);
    free(expect);
    close(fd);
}

#else  /* not Linux, or no O_DIRECT */

static void part_a_b(const char *path)
{
    (void)path;
    printf("\n=== PARTS A/B — SKIPPED ===\n");
    printf("  O_DIRECT is a Linux feature (macOS has F_NOCACHE, which is NOT\n"
           "  equivalent — it does not impose the alignment rules). Build and\n"
           "  run this part on the bench box against /mnt/nvme.\n");
}

#endif

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    printf("C4-S3 toy — scatter-gather + short-write fixup\n");

    if (argc > 1)
        part_a_b(argv[1]);
    else
        printf("\n(no path given: running part C only. Pass a file on /mnt/nvme\n"
               " to run parts A and B on the bench box.)\n");

    part_c();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
