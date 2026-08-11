/*
 * io_direct.c - O_DIRECT I/O primitives. See io_direct.h.
 */
#define _GNU_SOURCE
#include "io_direct.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* Loud, unmissable diagnostic when the kernel rejects an unaligned O_DIRECT op.
 * Required by CLAUDE.md §2 / docs/02 §1: catch EINVAL explicitly. */
static void report_einval(const char *op, off_t off, size_t len, const void *buf)
{
    fprintf(stderr,
            "\n*** O_DIRECT alignment violation ***\n"
            "    op=%s offset=%lld len=%zu buf=%p\n"
            "    offset%%4096=%lld len%%4096=%zu buf%%4096=%lu\n"
            "    (O_DIRECT needs all three aligned to %u bytes)\n\n",
            op, (long long)off, len, buf,
            (long long)(off % NOX_BLOCK_SIZE),
            len % NOX_BLOCK_SIZE,
            (unsigned long)((uintptr_t)buf & (NOX_BLOCK_SIZE - 1)),
            NOX_BLOCK_SIZE);
}

int io_direct_open(const char *path)
{
    /* O_DIRECT bypasses the page cache so we control the SSD bandwidth directly
     * (docs/03 §3). O_CREAT lets the benchmark create a fresh backing file. */
    int fd = open(path, O_RDWR | O_DIRECT | O_CREAT, 0644);
    return fd; /* -1 with errno on failure; caller reports */
}

ssize_t io_direct_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    const void *src = buf;
    void *bounce = NULL;

    /* The user buffer may come from plain malloc() and not be 4K-aligned.
     * O_DIRECT demands an aligned source address, so copy through an aligned
     * bounce buffer when necessary. (docs/02 §1) */
    if (!NOX_IS_ALIGNED(buf)) {
        if (posix_memalign(&bounce, NOX_BLOCK_SIZE, len) != 0) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(bounce, buf, len);
        src = bounce;
    }

    /* pwrite, not write+lseek: thread-safe against the shared file offset so
     * background OTflush threads can write concurrently. (docs/02 §2) */
    ssize_t n = pwrite(fd, src, len, off);
    int saved = errno;

    free(bounce);

    if (n < 0) {
        if (saved == EINVAL)
            report_einval("pwrite", off, len, buf);
        errno = saved;
    }
    return n;
}

ssize_t io_direct_pread(int fd, void *buf, size_t len, off_t off)
{
    /* Caller guarantees buf/off/len are aligned (the data zone and scratch are
     * posix_memalign'd; offsets are 256KB-aligned page bases). */
    ssize_t n = pread(fd, buf, len, off);
    if (n < 0) {
        if (errno == EINVAL)
            report_einval("pread", off, len, buf);
    }
    return n; /* short read (past EOF) is fine and handled by the caller */
}

/*
 * Consecutive calls making ZERO iovec-level progress tolerated before declaring
 * the write dead. Without a bound this is an infinite loop: a writer stuck
 * returning the same short count gets the identical call reissued forever, the
 * single Stage-2 thread never serves another page, and nox_close() hangs.
 * EINTR spends the same budget — both mean "that call advanced nothing".
 */
#define NOX_IO_MAX_STALL 4

/* Pure: how many leading iovecs `n` bytes cover fully. *bytes_done receives the
 * sum of their lengths (what the file offset advances by). Any remainder of `n`
 * inside the next iovec is deliberately ignored — those bytes get rewritten.
 * Never modifies the array; that is the whole trick. */
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

ssize_t io_direct_pwritev_all(int fd, struct iovec *iov, int iovcnt, off_t off)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    int   i       = 0;        /* first iovec not yet fully written */
    off_t cur_off = off;      /* file offset matching iov[i]'s start */
    int   stall   = NOX_IO_MAX_STALL;

    while (i < iovcnt) {
        /* pwritev, never writev+lseek: background threads share the fd
         * (docs/02 §2). */
        ssize_t w = pwritev(fd, &iov[i], iovcnt - i, cur_off);

        if (w < 0) {
            if (errno == EINTR) {
                if (--stall < 0) { errno = EINTR; return -1; }
                continue;
            }
            if (errno == EINVAL)
                report_einval("pwritev", cur_off, total, iov[i].iov_base);
            return -1;
        }

        if (w == 0) {
            /* No error, no bytes: nothing to resume from, no reason to expect a
             * different result next time. Fatal. */
            fprintf(stderr, "io_direct_pwritev_all: 0 bytes, no error, at %lld\n",
                    (long long)cur_off);
            errno = EIO;
            return -1;
        }

        if (w % NOX_BLOCK_SIZE)
            /* Not needed for correctness (we never do arithmetic with it), but
             * under O_DIRECT it means the write was cut mid-block — ENOSPC or a
             * signal. Silencing it would be as bad as silencing EINVAL. */
            fprintf(stderr,
                    "io_direct_pwritev_all: WARNING short write of %zd is not a "
                    "multiple of %u at offset %lld\n",
                    w, NOX_BLOCK_SIZE, (long long)cur_off);

        size_t done = 0;
        int    k = iov_complete_count(&iov[i], iovcnt - i, (size_t)w, &done);

        if (k == 0) {
            /* Not even the first iovec completed: the next call would be
             * byte-for-byte identical. Bound it or spin forever. */
            if (--stall < 0) {
                fprintf(stderr,
                        "io_direct_pwritev_all: no iovec completed after %d "
                        "attempts at offset %lld\n",
                        NOX_IO_MAX_STALL + 1, (long long)cur_off);
                errno = EIO;
                return -1;
            }
            continue;
        }

        stall    = NOX_IO_MAX_STALL;   /* real progress: refill the budget */
        i       += k;
        cur_off += (off_t)done;
        /* (w - done) bytes landed inside iov[i] and are discarded; the next call
         * rewrites them. Idempotent — see the precondition above. */
    }

    return (ssize_t)total;
}

/* Single-buffer case expressed as a 1-iovec gather, so there is exactly ONE
 * short-write policy in the engine. Keeps the bounce-buffer handling of
 * io_direct_pwrite for unaligned callers. */
ssize_t io_direct_pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    struct iovec iov = { .iov_base = (void *)(uintptr_t)buf, .iov_len = len };
    return io_direct_pwritev_all(fd, &iov, 1, off);
}
