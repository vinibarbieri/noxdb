# 03. The Parametric I/O (PIO) Model & SSD Theory

This document outlines the hardware characteristics of modern NVMe SSDs as defined by the Parametric I/O (PIO) Model . Understanding these properties is essential to justify the architectural decisions in our custom storage engine, specifically the need for Kernel Bypass and the Scrap Buffer.

## 1. The Core SSD Properties
Modern Solid-State Drives (SSDs) fundamentally differ from traditional hard-disk drives (HDDs) due to two distinct characteristics:

*   **Read/Write Asymmetry:** On SSDs, write operations are significantly slower and more expensive than read operations.
*   **Access Concurrency:** Modern NVMe SSDs are built with multiple internal channels, chips, and dies, allowing multiple I/O operations to be executed simultaneously. 

If a storage engine does not explicitly utilize these two properties, it cannot fully exploit the potential of the device.

## 2. The Problem with the OS Page Cache (Buffered I/O)
The default Linux Buffered I/O architecture fails to effectively utilize high-bandwidth SSDs because it conflicts directly with the PIO properties:
*   **Concurrency Bottleneck:** To manage the page cache in RAM, the OS uses non-scalable locks (like the XArray spinlock). When high-intensity concurrent writes are issued, the CPU experiences severe lock contention, limiting the concurrency of page management and preventing the SSD from using its internal parallelism.
*   **The Read-Before-Write Penalty:** If an application issues a partial-page write (e.g., a small unaligned write) that misses the cache, the OS must trigger a slow SSD-read to fill the page before it can be updated. This read-before-write penalty causes massive latency spikes .

## 3. How WSBuffer Solves This
Our C implementation must align with the PIO model to achieve maximum performance:
*   **Maximizing Concurrency (Bypassing the Cache):** By routing large, 4KB-aligned writes (>= 1MB) directly to the SSD via `O_DIRECT`, we bypass the OS page cache. This removes the XArray lock contention, saves CPU resources, and allows the SSD to process massive concurrent writes at peak bandwidth.
*   **Mitigating Asymmetry (The Scrap Buffer):** By absorbing small and unaligned writes into the RAM-based Scrap Buffer, we immediately acknowledge the write to the user, eliminating the synchronous read-before-write penalty. The background OTflush threads then asynchronously fetch the missing data (Stage-1 reads) and flush fully assembled pages (Stage-2 writes).
