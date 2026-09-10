# 03. The Problem (WSBuffer) and the Device Model (PIO)

This document states the problem NoxDB is built against and the device vocabulary it reasons with. They come from two different papers, and this document keeps them apart:

*   **The problem** — why buffered I/O cannot exploit a high-bandwidth SSD — is **WSBuffer**'s: Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs"*, USENIX FAST '26, §2.3–2.4. See §1.
*   **The device vocabulary** — read/write asymmetry (α) and access concurrency (k) — is the **Parametric I/O model (PIO)**: Papon & Athanassoulis, *"A Parametric I/O Model for Modern Storage Devices"*, DaMoN '21, [doi:10.1145/3465998.3466003](https://doi.org/10.1145/3465998.3466003). See §2.

WSBuffer does not cite PIO. Reading one through the other is this repository's framing, not either paper's claim.

## 1. The Problem: Buffered I/O on High-Bandwidth SSDs (WSBuffer)

Buffered I/O puts the page cache on the write critical path: every write is copied into cached pages and flushed later. WSBuffer measures buffered-I/O writes on high-bandwidth SSDs and names three challenges. The paper labels them C1–C3; this repository uses names instead, because C1–C4 name its build cycles (README, Roadmap).

### 1.1 Over-buffering (paper's C1, §2.3.1)

Page caching is overused to buffer **all** incoming writes. In the paper's ideal case (sequential 2 MB writes, unlimited memory, background flushing disabled) direct I/O outperformed buffered I/O in every configuration tested, by 1.10×–4.46×. The paper attributes the gap to the cost of page-cache management: page allocation, lookup, page-state maintenance, and LRU management. Direct I/O avoids that cost, but demands alignment of the I/O size, the offset, and the buffer address (`docs/02`).

### 1.2 Page-management contention (paper's C2, §2.3.2)

With page flushing enabled and memory limited, buffered-I/O write throughput depends heavily on available memory on every file system tested except BTRFS, which the paper notes is limited by its own concurrency issues. Supplying memory equal to 70% of the written data lowered throughput by up to 54.0% against the 100% case. The paper traces this to contention on the XArray's non-scalable spinlock (`xa_lock`) among free-page insertions, clean-page deletions, and page-state updates. Flushing one dirty page takes that lock several times (dirty → writeback → clean), and adding flusher threads does not help, because the lock itself is the root cause. The result is lower foreground and background throughput, and a large amount of memory frozen.

The paper ran this with 16 threads, **each on its own file**, to keep file-level lock contention out of the measurement. NoxDB writes a single file, where the inode's `i_rwsem` is a second candidate. This repository has not separated the two (`PERFORMANCE.md` §5).

### 1.3 Read-before-write (paper's C3, §2.3.3)

A partial-page write that misses the page cache triggers a page fault and a slow SSD read to fill the page before it can be updated. Single-threaded, with sufficient memory, partial-page writes measured 1.51×–84.37× the latency of the corresponding full-page writes, almost entirely due to the SSD reads. The paper notes that these small reads cannot benefit from the SSD's internal parallelism.

## 2. The Device Model: Parametric I/O (PIO)

The classical external-memory model counts storage accesses, which assumes reads and writes cost the same and that one I/O happens at a time. PIO replaces both assumptions with device parameters, `PIO(M, k_r, k_w, α)`:

*   **Read/write asymmetry, α** — the ratio of maximum read bandwidth to maximum write bandwidth, `α = BW_r,max / BW_w,max`. On NAND flash, writes are out-of-place and pay for garbage collection, so α is generally above 1.
*   **Access concurrency, k_r and k_w** — the number of concurrent I/Os needed to saturate the device's bandwidth, for reads and writes separately. The paper finds it empirically: it raises the number of threads issuing I/O and picks the point where bandwidth is close to saturation or its rate of increase drops significantly; on the latency profile, that is where latency suddenly rises.

Both parameters vary with the device, the access granularity, the access pattern, and the file system. Two of the paper's findings bear directly on this engine:

*   α is highest for I/O **smaller than the native 4 KB page**: 14.4 at 1 KB versus 2.8 at 4 KB on the paper's PCIe SSD with a file system. The paper concludes that sub-page writes should be avoided.
*   k **decreases as the I/O size grows**: larger I/Os saturate the device with fewer concurrent requests.

### 2.1 An informal observation on the bench SSD (not a measurement of k)

`PERFORMANCE.md` §3.1 sweeps queue depth for 4 KiB random writes on the bench SSD (WD SN530, XFS). Bandwidth rises 1.62× from QD1 to QD2 and only +2.8% more from QD2 to QD64, while p99 goes from 23.2 µs at QD2 to 220.2 µs at QD4. Bandwidth flattening and latency climbing is the shape PIO uses to locate k. Read informally, this drive's write concurrency for this one shape looks small, around 2.

This is **not** a measurement of `k_w` by the paper's methodology, and must not be cited as one:

*   PIO varies the number of **threads** issuing I/O, with direct I/O at queue depth 32. The §3.1 sweep varies `--iodepth` on a single `fio` job (`libaio`, `--direct=1`).
*   PIO averages 10 minutes per point after writing the device 3 times. §3.1 takes 5 × 120 s per point after the M2 preconditioning.
*   The queue depth the device saw did not match the one requested: at QD2, `iostat` reported `aqu-sz` 1.09 (§3.1, 2026-08-26 re-measurement).
*   Only writes were swept, so α is unknown for this device.
*   PIO reports that k depends on block size, access pattern, and file system. This is one shape on one file system.

Its use in this repository is narrow: it is why more Stage-2 threads are not expected to buy a multiple on this drive (`PERFORMANCE.md` §4.5–4.6).

## 3. How NoxDB Responds

Each mechanism maps to a challenge from §1 or a parameter from §2. **This section states design intent.** No page-cache baseline exists yet, so none of these responses is a measured win (README, *Where the honesty is*; `PERFORMANCE.md` §5).

*   **Fast path → over-buffering (§1.1).** Writes ≥ 1 MB and 4K-aligned in size and offset go straight to the SSD via `O_DIRECT` (`docs/01` §3). No page-cache pages are created for them, so that path does not pay the page-cache management cost §1.1 describes.
*   **Scrap buffer + OTflush → read-before-write (§1.3).** Small or unaligned writes are copied into a 256 KB user-space scrap page and acknowledged immediately. The read that fills the page's holes moves off the foreground path into OTflush Stage-1. A page that fills completely has no holes and skips Stage-1: it issues no read at all.
*   **Per-page locks + sharded index → page-management contention (§1.2).** Page tracking lives in user space under per-scrap-page mutexes and a 64-shard index (`docs/01` §5), so writers on different pages do not serialize on one lock. The single-file `i_rwsem` question from §1.2 is not addressed by this.
*   **Aligned writes only → asymmetry (§2).** NoxDB never issues a sub-4K write. Stage-2 writes whole 256 KB pages at 4K-aligned offsets, and the fast path is aligned by definition. That keeps the engine's writes out of the range where PIO measured α highest (`docs/02` §1 explains why the policy is 4096, not the device's 512).
*   **Concurrency (§2): barely exploited today.** OTflush moves device I/O off the foreground path. That decouples the application from the device; it does not make the device's I/O concurrent. Stage-2 runs on a single thread (`NOX_STAGE2_THREADS = 1`) because write ordering currently rests on Q2 having exactly one FIFO consumer, and raising the thread count reopens the ordering hazard (`PERFORMANCE.md` §4.5.1). Each `pwritev` is synchronous, so the engine puts little concurrent I/O on the device: a 300 s soak measured a median device `aqu-sz` of 1.58 (`PERFORMANCE.md` §4.6). On the bench SSD that costs little, because §2.1 puts the knee near QD2. An SSD with a larger `k_w` would need more concurrent I/O from Stage-2, and that depends on fixing write ordering first.
