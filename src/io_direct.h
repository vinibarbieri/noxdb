/*
 * io_direct.h - Thin O_DIRECT I/O layer.
 *
 * All transfers here MUST be 4K-aligned in offset and length (docs/02 §1).
 * pwrite/pread are used (never write/read + lseek) so background OTflush threads
 * cannot race on a shared file offset (docs/02 §2).
 */
#ifndef IO_DIRECT_H
#define IO_DIRECT_H

#include <sys/types.h>
#include <sys/uio.h>
#include <stddef.h>

/* open() the path O_RDWR|O_DIRECT|O_CREAT. Returns fd or -1 (errno set). */
int io_direct_open(const char *path);

/*
 * Positioned write via pwrite(). `off` and `len` must be 4K-multiples.
 * If `buf` is not 4K-aligned, a temporary aligned bounce buffer is used, because
 * O_DIRECT requires the *user buffer address* to be aligned too (docs/02 §1).
 * On EINVAL, prints "O_DIRECT alignment violation" to stderr (docs/02 §1).
 * Returns bytes written, or -1 (errno set).
 */
ssize_t io_direct_pwrite(int fd, const void *buf, size_t len, off_t off);

/*
 * Positioned read via pread(). `buf`, `off`, `len` must all be 4K-aligned.
 * A short read (region past EOF) is NOT an error; returns the byte count.
 * On EINVAL, prints "O_DIRECT alignment violation" to stderr.
 * Returns bytes read, or -1 (errno set).
 */
ssize_t io_direct_pread(int fd, void *buf, size_t len, off_t off);

/*
 * Write EVERY byte, or fail. Returns the total byte count on success, -1 with
 * errno set on failure. A short write is resumed at IOVEC granularity: complete
 * iovecs are dropped, the first incomplete one is reissued whole. Requires the
 * source buffers to be stable and the destination to be a fixed offset (see the
 * idempotence note in io_direct.c).
 */
ssize_t io_direct_pwrite_all(int fd, const void *buf, size_t len, off_t off);
ssize_t io_direct_pwritev_all(int fd, struct iovec *iov, int iovcnt, off_t off);

#endif /* IO_DIRECT_H */
