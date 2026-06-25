# WSBuffer Full-Project Foundation

**Date:** 2026-06-24
**Status:** Foundation / roadmap. Not an implementation plan — defines *what* the full project is and *how* the pieces fit. Per-cycle implementation plans are written separately against this document.

This is the architectural source of truth for the full build, layered on top of `docs/00_project.md` and the technical references (`01`–`03`). Where this document and `docs/01`–`03` disagree on a magic number, `docs/01`–`03` win.

---

## 1. Project scope (decided)

| Decision | Choice | Consequence |
|---|---|---|
| **Deliverable boundary** | Engine **+ thin LSM** | Two systems, two evaluation stories. The LSM is not a demo bolt-on — it is the proof the routing design is correct for a real workload. |
| **Concurrency model** | **Full**: multi-writer foreground + async OTflush | The benchmark *must* scale writer threads and show no global-lock collapse. This is the paper's core (C2) claim. |
| **Evaluation baselines** | **Page-cache** (buffered `write()`) + **raw `O_DIRECT`** (no scrap buffer) | The eval harness is a first-class designed component, not an afterthought. |
| **LSM↔engine file boundary** | **(a)** one engine handle per file | WAL file + each SSTable its own `wsb_open`. Simplest; routing is homogeneous per file. Single-file block-allocator model (b) is explicitly deferred as future work. |

---

## 2. System layering & the integration spine

Two stacked systems. Engine on the bottom, thin LSM on top.

```
┌─────────────────────────────────────────────┐
│  L4  THIN LSM KV STORE                        │
│      put/get/delete · memtable · SSTables ·   │
│      WAL · size-tiered compaction · recovery  │
└───────────────┬───────────────┬───────────────┘
                │ small appends  │ big seq merges
                │ (WAL, flush)   │ (SSTable write)
┌───────────────▼───────────────▼───────────────┐
│  L0–L3  WSBUFFER ENGINE                         │
│  wsb_write ─ router ─┬─ scrap path ── scrap     │
│  wsb_read            │   buffer (RAM 256KB pgs) │
│  wsb_fsync           └─ fast path ── O_DIRECT    │
│  async OTflush (2 queues) · per-page locks      │
└─────────────────────┬───────────────────────────┘
                       │  pread/pwrite/pwritev O_DIRECT
                  ┌────▼────┐
                  │  NVMe   │
                  └─────────┘
```

### The integration spine — why these two systems belong together

An LSM has exactly **two write patterns**, and they map 1:1 onto the engine's two paths:

| LSM operation | Pattern | Engine path | PIO claim proven |
|---|---|---|---|
| WAL append + memtable flush trigger | small, frequent, unaligned | **scrap buffer** | C3 — no read-before-write penalty |
| SSTable write (memtable flush + compaction merge) | large, sequential, alignable | **fast path** `O_DIRECT` | C1/C2 — raw bandwidth, no cache lock |

The thesis in one sentence: *the engine's router is exactly the routing an LSM needs, for free.* The LSM never decides which path — it just writes, and the workload's natural shape routes itself. The LSM proves the routing is correct for a real workload, not just a synthetic benchmark.

---

## 3. WSBuffer engine internals (L0–L3)

### 3.1 OTflush — two queues, async, opportunistic

```
wsb_write → merge into page
            │
            ├─ page now full (counter == 256KB) ───────────────► Q2 (write)
            └─ page partial → enqueued to Q1 IMMEDIATELY on create/modify

Stage-1 threads: pull Q1 continuously; execute hole-fill preads
                 OPPORTUNISTICALLY — gated by SSD-busyness (Bcount byte
                 counter). SSD idle → drain Q1 in the gaps.
                 page reaches full (via fill) ──► Q2
Stage-2 threads: pull Q2 → pwrite (pwritev batch if regions contiguous)
                 → reclaim page RAM → done
```

**Opportunistic enqueue is mandatory, not pressure-triggered.** A partial page enters Q1 the moment it is created or modified. Stage-1 threads execute the hole-fill `pread`s opportunistically, gated by SSD-busyness (the `Bcount` byte counter), draining Q1 during the SSD's idle cycles.

Rationale: if Q1 enqueue were triggered by a memory watermark, the system would flood the SSD with Stage-1 reads *exactly* when already under pressure → latency spike. Immediate enqueue + idle-gated execution hides the read-before-write penalty in the SSD's idle time. This is the entire point of "opportunistic" two-stage flushing.

Foreground `wsb_write` never blocks on I/O. A page that fills completely skips Q1 entirely (no read needed) → straight to Q2. That is the win over buffered I/O.

### 3.2 Concurrency — the anti-XArray design

| Structure | Protection | Why |
|---|---|---|
| Scrap page | per-page mutex | foreground merge on page A must not block flush of page B |
| Page index (hash) | **sharded bucket locks** | a single global index lock *is* the XArray bottleneck the paper fights — shard it |
| Q1 / Q2 | mutex + condvar | queue ops are cheap vs I/O; the contention that matters is index + per-page, not the queues |

### 3.3 Flush/write race — solved by the `tag` field

The 1-byte `tag` field in the 128-byte header exists precisely to record flushing state.

```
Stage-2 dequeue → lock page → tag = FLUSHING
foreground wsb_write hits a FLUSHING page → DO NOT merge
   → alloc fresh scrap page for that offset
   → swap hash-index pointer to the new page
   → old page drains its single pwrite, then frees itself
```

The hash index only ever points at the live (non-flushing) page. New writes accumulate in the fresh page while the old one drains. No two-writers-on-one-region race.

**Residual invariant for the implementation plan (not resolved here):** per-region write *ordering* — guarantee the old page's `pwrite` completes before a successor page may flush the same region. Practically near-impossible to violate (the old page is already mid-`pwrite` when the swap happens), but the thesis should state the invariant explicitly. Mechanism: generation counter or per-region in-flight guard.

### 3.4 Durability — `wsb_fsync()`

`wsb_fsync()` = flush all dirty scrap pages → wait for completion → `fsync(fd)` on the underlying file. Contract: after it returns, all prior writes are on stable storage.

The engine keeps **no cross-restart state** — the RAM buffer is lost on crash, which is correct: it is a write buffer, and durability is whatever was flushed. **Recovery is the LSM's job**, via its WAL. Clean split: engine = durable-on-fsync; LSM = replay.

### 3.5 Read path — `wsb_read()`

```
wsb_read(offset, len):
  1. pread the on-disk region (aligned; bounce buffer if user buf unaligned)
  2. for each resident scrap page overlapping [offset, len):
       lock page; overlay its valid segments onto the read buffer
  3. return merged (read-your-writes)
```

Symmetric to the partial-page flush. Required for correctness: a just-flushed SSTable may still sit in Q2 when read back.

### 3.6 Eviction — memory watermark (fallback only)

Cap total resident bytes. The watermark is **not** the Q1 trigger (see 3.1) — it is a hard fallback: if OTflush falls behind and resident bytes hit the high watermark, *throttle/block foreground writes* until back under the low watermark. Makes the buffer bounded rather than unbounded RAM.

---

## 4. Thin LSM layer (L4)

### 4.1 Components (minimal)

| Component | What | Routes to |
|---|---|---|
| **MemTable** | in-RAM sorted map (skiplist), size-bounded | — (RAM) |
| **WAL** | append-only durability log; every put appended before memtable insert | small append → **scrap path** |
| **SSTable** | immutable sorted file: data blocks + sparse index + footer | big seq write → **fast path** |
| **Compaction** | size-tiered: merge N SSTables → one larger, drop tombstones/overwrites | big read + big write → **fast path** |
| **Manifest** | tracks live SSTables + tiers (tiny file / dir convention) | — |

### 4.2 Operations → engine (the spine, concretely)

| LSM op | Engine call | Path exercised |
|---|---|---|
| `put` / `delete` → WAL append | `wsb_write` (small) | scrap |
| memtable full → flush SSTable | `wsb_write` (≥1MB aligned) | fast |
| compaction reads inputs | `wsb_read` | disk |
| compaction writes output | `wsb_write` (big) | fast |
| `get` → SSTable block | `wsb_read` | disk + buffer overlay |
| commit point | `wsb_fsync` | flush dirty |

- `get`: memtable → SSTables newest→oldest; first hit wins; tombstone = deleted.
- Recovery: replay WAL → rebuild memtable; SSTables loaded from manifest.
- File boundary: one `wsb_open` handle per file (decision 1(a)) — WAL is one file, each SSTable its own file.

### 4.3 YAGNI — explicitly cut for "thin"

Leveled compaction (→ use size-tiered), bloom filters (stretch goal only), concurrent memtable (a coarse lock is fine — the concurrency thesis lives in the *engine*), MVCC/snapshots, range scans (compaction iterator only), block compression.

---

## 5. Evaluation design

Baselines: **page-cache** (buffered `write()`) and **raw `O_DIRECT`** (no scrap buffer). Experiments map 1:1 to the PIO claims.

| # | Proves | Workload | Expected result |
|---|---|---|---|
| **E1** | C3 read-before-write | small unaligned writes | scrap path ≫ page-cache at scale; raw `O_DIRECT` cannot do unaligned (forces RMW / fails alignment) |
| **E2** | C2 concurrency | fixed work, **scale writer threads 1→N** | page-cache plateaus/collapses (XArray contention); WSBuffer scales near-linear to SSD saturation. **The money graph.** |
| **E3** | C1 bandwidth + CPU | big aligned sequential writes | fast path ≈ raw `O_DIRECT` (both bypass); both ≫ page-cache; WSBuffer/raw burn far less CPU |
| **E4** | integration | thin LSM, write-heavy mixed | LSM-on-WSBuffer vs LSM-on-buffered-backend → real ops/s win |

**Metrics:** throughput (MB/s, ops/s), latency p50/p99/p99.9, **CPU utilization** (proves the C1 saving), SSD bandwidth utilization. Tools: the bench harness + `iostat` / `perf` on the Proxmox VM.

**Correctness gates (pass/fail — they underpin the credibility of every perf number):**
- **Integrity:** write → read back → `memcmp`, per path.
- **Crash consistency:** `kill -9` mid-load → reopen → committed data survives (LSM WAL replay).

---

## 6. Module structure (full target)

Dependency direction is strict — **downward only**. The LSM sees only the engine's public API (`wsb_open` / `wsb_write` / `wsb_read` / `wsb_fsync` / `wsb_close`), never its internals.

```
lsm/   ─► wsbuffer.h  ─► { scrap_page · page_index · otflush · io_direct } ─► config
bench/ ─► lsm.h + wsbuffer.h + baselines

ENGINE
  wsbuffer_config.h     constants                                    (exists)
  wsbuffer.{h,c}        router + wsb_open/write/READ/FSYNC/close      (extend)
  scrap_page.{h,c}      + per-page mutex, tag=FLUSHING protocol       (extend)
  page_index.{h,c}      + sharded bucket locks (anti-XArray)          (extend)
  otflush.{h,c}         Q1/Q2, thread pool, Stage-1/2, Bcount         (NEW)
  queue.{h,c}           generic MPMC queue for otflush                (NEW)
  io_direct.{h,c}       pread/pwrite/pwritev O_DIRECT                 (exists)
LSM (lsm/)
  memtable · wal · sstable · compaction · manifest · lsm   (all NEW)
BENCH (bench/)
  baselines.{h,c}       page-cache + raw O_DIRECT reference impls     (NEW)
  bench_engine.c        E1 / E2 / E3                                  (NEW, seeded from benchmark.c)
  bench_lsm.c           E4 + crash-consistency test                  (NEW)
```

---

## 7. What this document does *not* decide

Deferred to the implementation plan(s):
- Build sequencing / milestones / cycle ordering and timeline.
- The per-region write-ordering invariant mechanism (§3.3 residual).
- Concrete struct layouts beyond the already-fixed 128-byte header.
- The AI development workflow (skills / agents / MCPs) for executing the build.
