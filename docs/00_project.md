# Project Vision & Foundation: High-Performance User-Space Storage Engine

## 1. Project Overview & Knowledge Base

This project builds a high-performance, asynchronous user-space I/O storage engine in C for modern Linux systems equipped with NVMe SSDs. The ultimate goal is to serve as a storage backend for **Log-Structured Merge-Tree (LSM-Tree)** databases, directly linking database transactions and compactions to hardware without OS interference.

Development is strictly guided by the following core reference documents:

| Reference | Role |
|---|---|
| **WSBuffer** — Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs"*, USENIX FAST '26 | Problem statement (the three buffered-I/O challenges, §2.3–2.4) and blueprint for data routing, `scrap_page_t`, and the OTflush mechanism |
| **PIO model** — Papon & Athanassoulis, *"A Parametric I/O Model for Modern Storage Devices"*, DaMoN '21, [doi:10.1145/3465998.3466003](https://doi.org/10.1145/3465998.3466003) | Device vocabulary: read/write asymmetry (α) and access concurrency (k). Not cited by WSBuffer; the link is this project's framing |
| **OSTEP** — *Part III: Persistence* | Foundation for log-structured FS principles and crash consistency (WAL, `fsync`) |
| **TLPI** — *Chapters 4, 5, 13* | Strict rulebook for POSIX: `O_DIRECT` alignment, `pread()`/`pwrite()`, scatter-gather I/O |

### Development & measurement environment (decided)

Benchmarks run on **bare-metal Ubuntu Server 24.04 LTS** — no hypervisor, no VM, no container. The earlier Proxmox VM / LXC plan is abandoned: a hypervisor adds scheduling jitter and a shared kernel/page cache that corrupt tail-latency (p99/p99.9) and CPU numbers, and an LXC container cannot `mkfs`/`mount` its own device. Bare metal gives true `iomap` direct I/O and clean `perf`/`iostat` measurements.

The benchmark target is a **dedicated, clean NVMe SSD** (WD SN530, TLC, fixed OEM BOM → reproducible) in the board's M.2 slot, formatted XFS and mounted at `/mnt/nvme`, used **only** for benchmarks. The OS lives on a **separate** disk (Kingston NV2 in a USB enclosure) so OS I/O never contends with the device under test. Code is still written locally and pushed via `make deploy` (rsync; see the `deploy` target in the `Makefile`).

---

## 2. The Problem Statement: The OS Page Cache Bottleneck

Historically, applications relied on the Linux **Page Cache** to bridge the speed gap between memory and storage. In the era of high-bandwidth PCIe NVMe SSDs, the OS page cache has become a severe bottleneck.

**WSBuffer** (§2.3, summarized in §2.4) identifies three challenges of buffered I/O on high-bandwidth SSDs:

| Challenge | Description (as measured in WSBuffer) |
|---|---|
| **Over-buffering** | Page caching is overused to buffer all incoming writes. In the paper's ideal-case test (unlimited memory, background flushing disabled), buffered I/O reaches lower write bandwidth than direct I/O (§2.3.1) |
| **Page-management contention** | Under heavy writes, concurrent free-page insertions, clean-page deletions and page-state updates contend on XArray's non-scalable spinlock (`xa_lock`), degrading foreground writes and background flushing and freezing a large amount of memory (§2.3.2). The paper measured this with one file per writer thread; NoxDB's single-file case adds the inode's `i_rwsem` as a second candidate (see `PERFORMANCE.md` §5) |
| **Read-before-write** | A partial-page write that misses the page cache triggers a page fault and a slow SSD read to fill the page before it can be updated (§2.3.3) |

> The paper labels these C1–C3. This repository does not reuse those labels: C1–C4 name its build cycles (README, Roadmap).

---

## 3. The Architectural Solution

To overcome these limitations, we implement a **Buffer-Minimized Data Access Mechanism**. The engine intercepts user writes and routes them through two distinct paths:

### 3.1. Fast Path — Kernel Bypass

Large write requests (≥ 1 MB) that are perfectly aligned to the logical block size bypass the scrap buffer entirely. The `O_DIRECT` flag sends data directly from user-space to the SSD, completely bypassing the kernel buffer cache.

> **TLPI Constraint:** To prevent `EINVAL` errors, the memory buffer, file offset, and transfer size must all be exact multiples of 4096 bytes. User-space memory for this path **must** use `posix_memalign()` — standard `malloc()` is forbidden. (4096 is NoxDB policy, not the bench box's device minimum, which is 512 — see `docs/02` §1 for why the stricter value is deliberate.)

### 3.2. Scrap Buffer

Small or unaligned writes are instantly routed to a custom **user-space RAM structure**, eliminating the synchronous read-before-write OS penalty.

> **NoxDB Layout:** Each `scrap_page_t` has a **520-byte header** (tracking valid bytes, segments, and tags across 64 index entries) and a **256 KB data-zone**. The data-zone is allocated separately to preserve 4096-byte `O_DIRECT` alignment. The header size is derived from the entry count — WSBuffer's default is 128 B / 15 entries, and `docs/01 §2.1` records the measurements behind this engine's 64.

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
