/*
 * io_direct.c - O_DIRECT I/O primitives. See io_direct.h.
 */
#define _GNU_SOURCE
#include "io_direct.h"
#include "wsbuffer_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
            (long long)(off % WSB_BLOCK_SIZE),
            len % WSB_BLOCK_SIZE,
            (unsigned long)((uintptr_t)buf & (WSB_BLOCK_SIZE - 1)),
            WSB_BLOCK_SIZE);
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
    if (!WSB_IS_ALIGNED(buf)) {
        if (posix_memalign(&bounce, WSB_BLOCK_SIZE, len) != 0) {
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
