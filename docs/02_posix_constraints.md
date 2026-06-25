# 02. POSIX & O_DIRECT Constraints

This document defines the strict POSIX system call rules and memory constraints required for the WSBuffer storage engine implementation.

## 1. Direct I/O (O_DIRECT) Restrictions
To successfully bypass the kernel buffer cache using the `O_DIRECT` flag, the implementation MUST adhere to three strict alignment rules:
*   **Buffer Alignment:** The data buffer address in user memory must be aligned to a multiple of the logical block size (4096 bytes). Standard `malloc()` is insufficient; you MUST use functions like `posix_memalign()` or `memalign()` to allocate the buffer.
*   **Offset Alignment:** The file offset at which data transfer begins must be an exact multiple of 4096 bytes.
*   **Length Alignment:** The length of the data being transferred must also be a multiple of 4096 bytes.
*   **Error Handling:** Failure to observe any of these alignment restrictions will cause the underlying I/O system calls to fail and return the `EINVAL` error. The engine must explicitly catch `errno == EINVAL` and print a diagnostic message indicating an `O_DIRECT` alignment violation.

## 2. Concurrent File I/O (Thread Safety)
Because WSBuffer uses background OTflush threads (pthreads) to handle I/O concurrently, we must avoid race conditions on the global file offset.
*   Do NOT use standard `read()` or `write()` combined with `lseek()`, as multiple threads in a process share the same file descriptor table and thus the same global file offset.
*   Instead, use `pread()` and `pwrite()`. These functions perform I/O at an explicitly specified offset and leave the global file offset unchanged, making them safe for multithreaded concurrent I/O.

## 3. Scatter-Gather I/O (Batching)
For OTflush Stage-2, the engine may need to batch contiguous 256KB scrap-pages to the disk efficiently.
*   Use scatter-gather I/O via the `struct iovec` array (which contains `iov_base` and `iov_len` fields) to transfer multiple buffers in a single atomic system call.
*   To combine scatter-gather functionality with thread-safe explicit offsets, use `preadv()` and `pwritev()`. These calls allow the file system to write multiple disjoint memory buffers to a specific contiguous disk location without altering the shared file offset.
