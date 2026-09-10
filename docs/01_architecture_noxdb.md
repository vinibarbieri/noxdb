# 01. NoxDB Architecture & Specifications

This document serves as the absolute Source of Truth for the NoxDB architecture implementation. All C code generated must strictly adhere to the data structure layouts, sizes, and routing logic defined here.

## 1. Core Constants & Limits
*   **Logical Block Size (Alignment):** 4096 bytes (4KB). All direct SSD accesses must be perfectly aligned to this boundary.
*   **Direct I/O Threshold:** 1MB. Write requests equal to or larger than this size bypass the scrap buffer.
*   **Scrap Page Header Size:** DERIVED, not a constant: `8 + NOX_MAX_ENTRIES * 8` bytes. At this engine's chosen 64 index entries that is **520 bytes**. At WSBuffer's default of 15 entries it is the paper's 128 bytes. See §2.1 for why the two differ.
*   **Scrap Page Index Entries:** 64. Hard ceiling 255 (the `number` field is one byte). See §2.1.
*   **Scrap Page Data Zone Size:** 256 KB.

## 2. The Scrap Page Data Structure (`scrap_page_t`)
The scrap buffer takes over the write buffer handling from the traditional page cache to achieve efficient partial-page writes. Unlike standard page caches where pages are always full, the scrap buffer manages partial-page writes with diverse offsets and sizes using a specialized header.

A single `scrap_page_t` MUST be logically split into two components to satisfy alignment rules:
1.  **Header (520 Bytes at 64 entries; `8 + NOX_MAX_ENTRIES * 8`):** Must contain the following exact fields:
    *   `uint32_t counter`: 4B counter for recording the byte count of valid data within this page.
    *   `uint8_t number`: 1B field for recording the number of data-segments within this page.
    *   `uint16_t ssd_id`: 2B SSD-id field for identifying the underlying SSD.
    *   `uint8_t tag`: 1B field for recording the page flushing state.
    *   `entries`: An array of **64** index entries, each 8B in size (4B offset + 4B size), used to track data-segments. WSBuffer's default is 15; §2.1 records why this engine sets 64 and what it measured to get there.
2.  **Data Zone (256 KB):** A pointer to a 256KB memory region.
    *   *CRITICAL C IMPLEMENTATION RULE:* The Data Zone must NOT be allocated continuously with the header struct via a single `malloc`. It MUST be allocated separately using `posix_memalign(..., 4096, 256 * 1024)` to ensure the memory address satisfies `O_DIRECT` requirements.

## 2.1 Index Entry Count (`NOX_MAX_ENTRIES = 64`)

**Normative:** this engine uses **64** index entries, giving a 520-byte header. WSBuffer's stated default is 15 entries / 128 bytes. This is a *tuning of a knob the paper documents*, not a divergence from the design.

### The paper sanctions the knob

WSBuffer says both of these directly:

> "the header is 128B-sized **by default** [...] and 15 8B-sized index entries"

> "A larger header means more index entries, i.e., accommodating more discontinuous data-segments, **which applies to workloads primarily composed of small writes**. In our evaluations, the number of index entries used within a scrap-page is less than 15 in more than 95% cases."

So 15 is an *empirical result on the authors' workloads*, and the paper explicitly points at a larger header for small-write workloads. Raising it is using the mechanism as documented — but it must be justified the same way the authors justified 15: **with our own measurement**, which is what the rest of this section records.

### Why 15 is wrong here: the binding constraint

A scrap page is evicted for one of two reasons:

*   **Capacity** — the 256 KB data zone filled. This is the designed path: the page did its job.
*   **Fragmentation** — the entry array ran out. The page is sealed and a whole 256 KB is written back for however little data it holds.

Which one binds is decided by a single ratio. The crossover average segment size is `NOX_DATAZONE_SIZE / NOX_MAX_ENTRIES`; above it capacity binds, below it the entry array binds and write amplification is approximately:

```
amplification ≈ NOX_DATAZONE_SIZE / (NOX_MAX_ENTRIES * avg_segment_size)
```

| entries | crossover | header | header as % of page |
|---|---|---|---|
| 15 | 17.1 KB | 128 B | 0.049% |
| 31 | 8.3 KB | 256 B | 0.098% |
| **64** | **4.0 KB** | **520 B** | **0.198%** |
| 127 | 2.0 KB | 1024 B | 0.391% |
| 255 | 1.0 KB | 2048 B | 0.781% |

At 15 entries the crossover sits at 17.1 KB — just *above* a 16 KB InnoDB page. WSBuffer's default is sized for page-oriented databases, consistent with its own "<15 entries in >95% of cases". Common engines land below that line:

| workload | segment size | entries needed |
|---|---|---|
| InnoDB page | 16 KB | 16 |
| PostgreSQL page | 8 KB | 32 |
| SQLite page / ext4 block | 4 KB | 64 |
| WAL / redo append | any | **1** — sequential segments coalesce |
| LSM SST flush | ≥ 1 MB | 0 — fast path, never reaches the scrap buffer |

Note the WAL row: `coalesce_insert` merges adjacent and overlapping segments, so **sequential** small writes cost one entry regardless of count. The entry array is only consumed by **scattered** sub-page writes.

### Why 64 and not 255

64 is not fitted to a benchmark. It is derived from two constants already in §1:

```
NOX_DATAZONE_SIZE / NOX_BLOCK_SIZE = 262144 / 4096 = 64
```

At 64 entries the entry array **cannot** run out before capacity fills, for any workload whose segments are ≥ 4 KB: 64 segments of 4096 B put `counter` at exactly 262144, so capacity binds at the same instant, and larger segments bind earlier. Since 4096 B is the device's own addressing granularity (§1), a segment smaller than that cannot be independently placed on the SSD anyway — sub-block amplification is physics, and absorbing it is precisely what the scrap buffer exists to do.

255 (the hard ceiling — `number` is one byte) scores better on every number in the sweep below, and was rejected anyway: the C4 gate that produces those numbers issues **400 random unaligned segments per 256 KB region** (`bench/otflush_test.c`), deliberately more than any legal entry count can hold. It is a fragmentation stress test for hole-filling correctness, not a workload model. Tuning the header to it would be fitting the benchmark.

### Measured evidence

Entry-count sweep, `make stats-c4` + C4 gate workload, 8 threads, 25600 writes, 7681502 user bytes (**300 B average segment**), bare-metal bench box against `/mnt/nvme`:

| entries | pages created | sealed by exhaustion | reached full 256 KB | write amp | read amp | drain |
|---|---|---|---|---|---|---|
| 15 | 1715 | **96.27%** | 0.00% | 58.53x | 70.93x | 4.283 s |
| 63 | 385 | 83.38% | 0.00% | 13.14x | 24.73x | 1.757 s |
| 255 | 86 | 25.58% | 0.00% | 2.93x | 10.69x | 0.683 s |

Two things to read off this table:

1.  At 15 entries, **96% of pages die of fragmentation** against WSBuffer's implied <5%. The default is inverted for this workload.
2.  **`reached full 256 KB` is 0.00% at every setting, including 255.** At a 300 B average segment, filling 256 KB by capacity needs ~766 entries; the ceiling is 255. For a workload this fine-grained **no legal header reaches the designed regime.** This is a property of the paper's 1-byte `number` field, recorded here as a known limit — not a defect to fix.

Production build (`-O2`, no instrumentation), C4 gate, 8 threads:

| | 15 entries | 64 entries | gain |
|---|---|---|---|
| `nox_close` drain | 5.105 s | 1.811 s | 2.8x |
| foreground p99 | 58835 ns | 27792 ns | 2.1x |
| foreground p99.9 | 180577 ns | 81894 ns | 2.2x |
| foreground max | 261626 ns | 202791 ns | 1.3x |

Both PASS. Under ThreadSanitizer (4 threads, `setarch -R`) p99 goes 53901 ns → 39132 ns, also PASS; `max` there is *worse* (155852 → 215403 ns), a single-sample outlier not read as a trend.

The latency gain is not a coincidence of the header: 4.5x fewer pages created means 4.5x fewer 256 KB `posix_memalign` zones and the minor page faults that come with them — the same root cause that drove the C4 latency tail (see `scrap_page_alloc`).

### What it costs

Per page, `scrap_page_t` plus its data zone:

| | 15 entries | 64 entries |
|---|---|---|
| data zone | 262144 B | 262144 B |
| header | 128 B | 520 B |
| rest of struct (ptr, base, mutex, links, flags) | 84 B | 84 B |
| **total** | ~262376 B | ~262768 B |

**392 bytes per page, 0.15%.** At the 4096-page queue warning depth that is +1.6 MB on 1.07 GB.

Worst-case engine RAM is therefore **not** set by the header — it is set by the number of live pages, which is unbounded until the eviction watermark lands. And for fragmented workloads the larger header *reduces* RAM, because fewer pages exist: 1715 pages × 256 KB = 440 MB at 15 entries versus 385 × 256 KB = 99 MB at 63.

Write-heavy workloads pay the 0.15% and get nothing, because for segments above the crossover the entry array was never the binding constraint. That is the whole cost.

### When this knob freezes

`NOX_MAX_ENTRIES` is a compile-time constant and the header is **RAM-only in the MVP** (`docs/00_flow_summary.md`), so nothing on disk encodes it. Changing it today is a rebuild: no migration, no format version. `NOX_HEADER_SIZE` is derived from it and two `_Static_assert`s enforce both the struct layout and the `[1, 255]` range. Build with `make <target> NOX_ENTRIES=<n>` to sweep.

It stops being free at either of these points, and both should be treated as decision gates:

1.  **Runtime tuning per workload** would require the entry array to become a variable-length allocation, changing `scrap_page_t`'s layout and the allocation path.
2.  **Persisting the header to disk** makes the value part of the on-disk format; changing it then requires a version field.

## 3. Buffer-Minimized Data Access (The Router)
The engine must implement a `write_data` mechanism that splits and routes user writes to either the SSD or the RAM Scrap Buffer to proactively leverage SSD bandwidth and minimize buffered data.

The logic MUST follow this simplified MVP flow:
*   **Fast Path (Direct I/O):** IF the `req_size >= 1MB` AND the `req_size` is a multiple of 4096 AND the `req_offset` is a multiple of 4096:
    *   Bypass the scrap buffer completely.
    *   Write data directly to the SSD via `O_DIRECT`.
*   **Scrap Path (Scrap Buffer):** IF the `req_size < 1MB` OR the request is unaligned:
    *   Route the write to the Scrap Buffer in RAM.
    *   Merge the new data with existing address-overlapping data-segments by querying and updating the scrap-page header's `NOX_MAX_ENTRIES` (64) index entries (§2.1).
    *   If the page becomes full, update the header's tag field and enqueue it to OTflush Stage-2.

## 4. Opportunistic Two-Stage Flushing (OTflush)
To prevent stalling the foreground user writes, NoxDB uses asynchronous pthreads to flush data to the SSD.
*   **Stage-1 (Queue-1 / Reads):** Background threads dequeue unfilled scrap-pages. They identify "holes" in the 256KB data-zone not covered by valid segments, and issue 4KB-aligned `pread` operations from the SSD to fill these holes (read-before-write).
*   **Stage-2 (Queue-2 / Writes):** Background threads dequeue fully assembled 256KB scrap-pages and write them back to the SSD. After the write completes, the memory is reclaimed.

## 5. Concurrency Model
To avoid the severe lock contention seen in the Linux Kernel's XArray (`xa_lock`) during intensive writes, NoxDB avoids massive global locks.
*   **Per-Page Locks:** Scrap-page updates and flushes must use fine-grained per-scrap-page locks. This ensures that background OTflush threads and foreground user writes do not block each other unnecessarily.
*   **Sharded Page Index:** The page index (offset → resident scrap page) must NOT be guarded by a single global lock — that would reintroduce the exact XArray-style bottleneck this design exists to escape. The index is partitioned into independent *shards*, each with its own mutex; a page maps to a shard by its hash. Concurrent writers touching pages in different shards never serialize on the index.

    *   **Shard count (engineering choice, NOT a spec mandate): `NOX_INDEX_SHARDS = 64`.** Rationale: the goal is only to push lock contention below the hardware's parallelism so the SSD bandwidth — not the lock — is the bottleneck. 64 comfortably exceeds both the NVMe queue depth (WD SN530 ≈ QD32) and the bench box core count, so residual false *contention* (two distinct pages colliding on one shard) is negligible for realistic writer counts. Going finer (e.g. one lock per hash bucket) buys no measurable throughput yet invites **false sharing**: `pthread_mutex_t` (~40 B) packed in an array shares 64 B cache lines, so locking two unrelated shards ping-pongs a cache line between cores. With only 64 shards each lock is cheaply padded/aligned to a full 64 B cache line (`NOX_CACHELINE`), eliminating false sharing at ~4 KB total. This value is a tunable, not dictated by the paper; revisit if the bench box gains many more cores.
