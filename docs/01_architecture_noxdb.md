# 01. NoxDB Architecture & Specifications

This document serves as the absolute Source of Truth for the NoxDB architecture implementation. All C code generated must strictly adhere to the data structure layouts, sizes, and routing logic defined here.

## 1. Core Constants & Limits
*   **Logical Block Size (Alignment):** 4096 bytes (4KB). All direct SSD accesses must be perfectly aligned to this boundary.
*   **Direct I/O Threshold:** 1MB. Write requests equal to or larger than this size bypass the scrap buffer.
*   **Scrap Page Header Size:** 128 bytes.
*   **Scrap Page Data Zone Size:** 256 KB.

## 2. The Scrap Page Data Structure (`scrap_page_t`)
The scrap buffer takes over the write buffer handling from the traditional page cache to achieve efficient partial-page writes. Unlike standard page caches where pages are always full, the scrap buffer manages partial-page writes with diverse offsets and sizes using a specialized header.

A single `scrap_page_t` MUST be logically split into two components to satisfy alignment rules:
1.  **Header (128 Bytes):** Must contain the following exact fields:
    *   `uint32_t counter`: 4B counter for recording the byte count of valid data within this page.
    *   `uint8_t number`: 1B field for recording the number of data-segments within this page.
    *   `uint16_t ssd_id`: 2B SSD-id field for identifying the underlying SSD.
    *   `uint8_t tag`: 1B field for recording the page flushing state.
    *   `entries`: An array of 15 index entries, each 8B in size (4B offset + 4B size), used to track data-segments.
2.  **Data Zone (256 KB):** A pointer to a 256KB memory region 3. 
    *   *CRITICAL C IMPLEMENTATION RULE:* The Data Zone must NOT be allocated continuously with the header struct via a single `malloc`. It MUST be allocated separately using `posix_memalign(..., 4096, 256 * 1024)` to ensure the memory address satisfies `O_DIRECT` requirements.

## 3. Buffer-Minimized Data Access (The Router)
The engine must implement a `write_data` mechanism that splits and routes user writes to either the SSD or the RAM Scrap Buffer to proactively leverage SSD bandwidth and minimize buffered data 6.

The logic MUST follow this simplified MVP flow:
*   **Fast Path (Direct I/O):** IF the `req_size >= 1MB` AND the `req_size` is a multiple of 4096 AND the `req_offset` is a multiple of 4096:
    *   Bypass the scrap buffer completely.
    *   Write data directly to the SSD via `O_DIRECT`.
*   **Scrap Path (Scrap Buffer):** IF the `req_size < 1MB` OR the request is unaligned:
    *   Route the write to the Scrap Buffer in RAM.
    *   Merge the new data with existing address-overlapping data-segments by querying and updating the scrap-page header's 15 index entries.
    *   If the page becomes full, update the header's tag field and enqueue it to OTflush Stage-2.

## 4. Opportunistic Two-Stage Flushing (OTflush)
To prevent stalling the foreground user writes, NoxDB uses asynchronous pthreads to flush data to the SSD.
*   **Stage-1 (Queue-1 / Reads):** Background threads dequeue unfilled scrap-pages. They identify "holes" in the 256KB data-zone not covered by valid segments, and issue 4KB-aligned `pread` operations from the SSD to fill these holes (read-before-write).
*   **Stage-2 (Queue-2 / Writes):** Background threads dequeue fully assembled 256KB scrap-pages and write them back to the SSD. After the write completes, the memory is reclaimed.

## 5. Concurrency Model
To avoid the severe lock contention seen in the Linux Kernel's XArray (`xa_lock`) during intensive writes, NoxDB avoids massive global locks, 13.
*   **Per-Page Locks:** Scrap-page updates and flushes must use fine-grained per-scrap-page locks. This ensures that background OTflush threads and foreground user writes do not block each other unnecessarily.
