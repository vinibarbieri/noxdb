# Project Vision & Foundation: High-Performance User-Space Storage Engine

## 1. Project Overview & Knowledge Base

This project builds a high-performance, asynchronous user-space I/O storage engine in C for modern Linux systems equipped with NVMe SSDs. The ultimate goal is to serve as a storage backend for **Log-Structured Merge-Tree (LSM-Tree)** databases, directly linking database transactions and compactions to hardware without OS interference.

Development is strictly guided by the following core reference documents:

| Reference | Role |
|---|---|
| **WSBuffer Paper** — *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs"* | Blueprint for data routing, `scrap_page_t`, and the OTflush mechanism |
| **CHILL Lab Research Statement** | Establishes the PIO Model: exploit SSDs via read/write asymmetry and access concurrency |
| **OSTEP** — *Part III: Persistence* | Foundation for log-structured FS principles and crash consistency (WAL, `fsync`) |
| **TLPI** — *Chapters 4, 5, 13* | Strict rulebook for POSIX: `O_DIRECT` alignment, `pread()`/`pwrite()`, scatter-gather I/O |

### Development & measurement environment (decided)

Benchmarks run on **bare-metal Ubuntu Server 24.04 LTS** — no hypervisor, no VM, no container. The earlier Proxmox VM / LXC plan is abandoned: a hypervisor adds scheduling jitter and a shared kernel/page cache that corrupt tail-latency (p99/p99.9) and CPU numbers, and an LXC container cannot `mkfs`/`mount` its own device. Bare metal gives true `iomap` direct I/O and clean `perf`/`iostat` measurements.

The benchmark target is a **dedicated, clean NVMe SSD** (WD SN530, TLC, fixed OEM BOM → reproducible) in the board's M.2 slot, formatted XFS and mounted at `/mnt/nvme`, used **only** for benchmarks. The OS lives on a **separate** disk (Kingston NV2 in a USB enclosure) so OS I/O never contends with the device under test. Code is still written locally and pushed via `make deploy` (rsync) — see CLAUDE.md §3.

---

## 2. The Problem Statement: The OS Page Cache Bottleneck

Historically, applications relied on the Linux **Page Cache** to bridge the speed gap between memory and storage. In the era of high-bandwidth PCIe NVMe SSDs, the OS page cache has become a severe bottleneck.

The **PIO Model** identifies three critical challenges:

| # | Challenge | Description |
|---|---|---|
| **C1** | Costly Over-Buffering | OS places all incoming writes into the page cache on the critical path, burning CPU cycles and failing to exploit raw sequential SSD bandwidth |
| **C2** | Concurrency Limitations | High-intensity writes cause severe lock contention in kernel memory management (e.g., XArray locks), throttling the CPU and preventing the SSD from using its internal parallel channels |
| **C3** | Read-Before-Write Penalty | Small, unaligned, or partial-page writes force the OS to synchronously read a full block from SSD before modification — prohibitively expensive latency spikes |

---

## 3. The Architectural Solution

To overcome these limitations, we implement a **Buffer-Minimized Data Access Mechanism**. The engine intercepts user writes and routes them through two distinct paths:

### 3.1. Fast Path — Kernel Bypass

Large write requests (≥ 1 MB) that are perfectly aligned to the logical block size bypass the scrap buffer entirely. The `O_DIRECT` flag sends data directly from user-space to the SSD, completely bypassing the kernel buffer cache.

> **TLPI Constraint:** To prevent `EINVAL` errors, the memory buffer, file offset, and transfer size must all be exact multiples of the disk's logical block size (4096 bytes). User-space memory for this path **must** use `posix_memalign()` — standard `malloc()` is forbidden.

### 3.2. Scrap Buffer

Small or unaligned writes are instantly routed to a custom **user-space RAM structure**, eliminating the synchronous read-before-write OS penalty.

> **NoxDB Layout:** Each `scrap_page_t` has a **128-byte header** (tracking valid bytes, segments, and tags) and a **256 KB data-zone**. The data-zone is allocated separately to preserve 4096-byte `O_DIRECT` alignment.

### 3.3. Opportunistic Two-Stage Flushing (OTflush) & Concurrency

Background `pthreads` safely flush the Scrap Buffer to disk without blocking the foreground application.

| Stage | Operation |
|---|---|
| **Stage 1** | Background threads fetch missing data from SSD using 4 KB-aligned `pread()` calls to fill "holes" in partially filled scrap pages |
| **Stage 2** | Fully assembled 256 KB pages are written back to the SSD via `pwrite()` |

> **TLPI Thread-Safety:** Standard `read()`/`write()` share a global file offset and are unsafe for multi-threaded OTflush. We strictly use `pread()`/`pwrite()` for explicit-offset I/O. To batch contiguous 256 KB pages, we use scatter-gather I/O via `pwritev()`.

---

## 4. Phase 2: LSM-Tree Database Backend

This storage engine is not a generic block manager — its end goal is to serve as the highly optimized storage backend for a **Key-Value Store based on LSM-Trees**.

The architecture directly complements LSM-Tree mechanics:

| LSM Operation | Engine Behavior |
|---|---|
| **WAL & Transactions** | Small, frequent transactions are instantly absorbed by the Scrap Buffer. On `fsync()`, only dirty scrap pages are flushed to SSD — high transaction throughput and crash consistency |
| **Compactions** | Massive sequential SSTable merges are identified by the router and sent straight down the `O_DIRECT` Fast Path — saves CPU and RAM for query analytics (HTAP) |
