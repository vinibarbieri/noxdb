# 02. POSIX & O_DIRECT Constraints

This document defines the strict POSIX system call rules and memory constraints required for the NoxDB storage engine implementation.

## 1. Direct I/O (O_DIRECT) Restrictions
To successfully bypass the kernel buffer cache using the `O_DIRECT` flag, the implementation MUST adhere to three strict alignment rules:
*   **Buffer Alignment:** The data buffer address in user memory must be aligned to a multiple of 4096 bytes. Standard `malloc()` is insufficient; you MUST use functions like `posix_memalign()` or `memalign()` to allocate the buffer.
*   **Offset Alignment:** The file offset at which data transfer begins must be an exact multiple of 4096 bytes.
*   **Length Alignment:** The length of the data being transferred must also be a multiple of 4096 bytes.
*   **Why 4096 and not the device's logical block size.** The kernel enforces alignment against the *underlying device's* logical block size, not against our constant. Both disks on the bench box report **512** (`lsblk -o LOG-SEC`), so 512-byte-aligned I/O is legal there — measured directly at the `pwritev` level by `bench/pwritev_toy.c` part B2. **4096 is therefore NoxDB policy, stricter than the kernel minimum on this hardware, and deliberately so:** (a) it is the NAND's physical block size and the page size, so sub-4K writes cause read-modify-write inside the SSD; (b) 4Kn devices exist and reject 512-byte granularity outright, while 4096 is legal on both 512e and 4Kn. Never relax this to 512.
*   **The rules are PER-BUFFER, not per-call.** With `preadv`/`pwritev`, every `iov_base` and every `iov_len` must be aligned individually — an aligned *total* does not save a torn iovec. Demonstrated by `bench/pwritev_toy.c` part B1 (odd `iov_len`s summing to an exact multiple of 4096 → `EINVAL`).
*   **Error Handling:** Failure to observe any of these alignment restrictions will cause the underlying I/O system calls to fail and return the `EINVAL` error. The engine must explicitly catch `errno == EINVAL` and print a loud diagnostic to `stderr` containing the literal text `O_DIRECT alignment violation`, so the failure cannot be mistaken for an ordinary I/O error.

## 2. Concurrent File I/O (Thread Safety)
Because NoxDB uses background OTflush threads (pthreads) to handle I/O concurrently, we must avoid race conditions on the global file offset.
*   Do NOT use standard `read()` or `write()` combined with `lseek()`, as multiple threads in a process share the same file descriptor table and thus the same global file offset.
*   Instead, use `pread()` and `pwrite()`. These functions perform I/O at an explicitly specified offset and leave the global file offset unchanged, making them safe for multithreaded concurrent I/O.

## 3. Scatter-Gather I/O (Batching)
For OTflush Stage-2, the engine may need to batch contiguous 256KB scrap-pages to the disk efficiently.
*   Use scatter-gather I/O via the `struct iovec` array (which contains `iov_base` and `iov_len` fields) to transfer multiple buffers in a single atomic system call.
*   To combine scatter-gather functionality with thread-safe explicit offsets, use `preadv()` and `pwritev()`. These calls allow the file system to write multiple disjoint memory buffers to a specific contiguous disk location without altering the shared file offset.
