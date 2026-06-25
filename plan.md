# WSBuffer — Study & Supervision Plan

**Goal:** Claude Code writes the implementation. You acquire, cycle by cycle, the knowledge to **understand, review, discuss, and defend** every component — so the final system can be presented to a professor or in a talk.

**This is not a "learn to write C from scratch" plan.** It is a "learn to supervise and own the design" plan. For each cycle:

- **What Claude builds** — the deliverable code.
- **Study before / during** — exact theory you must absorb to understand that code.
- **Review checklist** — what to verify in Claude's output + concepts you must be able to explain in your own words. If you can't, you don't merge.
- **Defense questions** — what an examiner could ask. You must answer all without notes.

> **Rule of the plan:** never merge a cycle you cannot explain. After Claude writes code, ask it: *"explain why you chose X, and what breaks if I remove it."* If its answer teaches you something you didn't know, that's a study gap — close it before moving on.

---

## Calibration (your starting point)

| Axis | Level | Implication |
|---|---|---|
| Concurrency (pthreads, mutex, condvar, races) | ~zero | Heaviest study load. The thesis (C2) lives here. Cycles 3–5 are the make-or-break. |
| OS / storage (page cache, O_DIRECT, fsync, NVMe) | superficial | Foundational. Front-loaded in Cycles 0–1. |
| LSM internals (memtable, SSTable, WAL, compaction) | none | Deferred to Cycles 7–9, when the engine is solid. |

Realistic budget at ~2.5 h/day part-time: **10–12 weeks**. Do not compress the concurrency cycles to hit 8.

---

## Reference library (acquire these now)

| Ref | Use | Cycles |
|---|---|---|
| **TLPI** (The Linux Programming Interface, Kerrisk) — ch 4, 5, 13, 29, 30 | POSIX rulebook: file I/O, O_DIRECT, buffering, pthreads | 0,1,3,4,6 |
| **OSTEP** (free PDF) — Part I Concurrency (Threads/Locks/Condvars/Bugs), Part III Persistence (I/O devices, FFS, **Crash Consistency**, **LFS**) | Concepts + crash consistency + LSM ancestor | 3,4,5,6,7,9 |
| **WSBuffer paper** — "Rearchitecting Buffered I/O…" | The architecture you're implementing. Re-read per cycle. | all |
| **RocksDB / LevelDB wiki design docs** | LSM component design | 7,8,9 |
| `docs/00`–`03` + the foundation spec | Your own source of truth | all |

Tooling to learn alongside (not optional): **ThreadSanitizer** (`-fsanitize=thread`), **valgrind/helgrind**, **perf**, **iostat**, **gdb** basics.

---

## Cross-cutting study (do in parallel, low dose)

These thread through every cycle — ~15 min/day until fluent:

- **C language depth you must own to read Claude's code:** pointers & pointer arithmetic, `struct` layout & padding/alignment, `uint*_t` fixed types, function pointers (used for index callbacks), `const` correctness, heap vs stack, `errno`. You don't write it, but you must *read* it fluently.
- **Reading code you didn't write:** trace data flow by hand; for each function ask "what does it own, what does it lock, what can fail." Practice on the existing `src/*.c`.
- **Version control discipline:** one cycle = one branch = one reviewable diff.

---

## Existing code map — what to review, exactly where

The repo already contains C0–C2's code (untracked — commit it as baseline first). **Don't hunt.** This table sends you straight to the lines each Review checklist refers to. Cycles 3+ have no code yet — those files are marked NEW.

| Concept (Review card) | File · symbol | Lines | Note |
|---|---|---|---|
| All magic numbers (128B, 256KB, 1MB, 4096, 15) | `src/wsbuffer_config.h` | 14–40 | Source of truth mirror of `docs/01`. Start here. |
| `O_DIRECT` open (flags) | `src/io_direct.c` · `io_direct_open` | 31–37 | `O_RDWR\|O_DIRECT\|O_CREAT` |
| EINVAL "alignment violation" diagnostic | `src/io_direct.c` · `report_einval` | 17–29 | The loud stderr message (CLAUDE.md §2) |
| EINVAL catch sites | `src/io_direct.c` · pwrite/pread | 64–66, 77–78 | Where errno==EINVAL is trapped |
| Alignment macros | `src/wsbuffer_config.h` · `WSB_IS_ALIGNED`, `WSB_PAGE_BASE` | 36, 40 | Bit-mask alignment + page-base rounding |
| **Router / fast-path branch** | `src/wsbuffer.c` · `wsb_write` | 98–134 | Fast-path test at **106–111** (the 3 conditions) |
| pwrite + bounce buffer | `src/io_direct.c` · `io_direct_pwrite` | 39–69 | Bounce alloc 47–54; why pwrite not write+lseek 57–58 |
| pread wrapper | `src/io_direct.c` · `io_direct_pread` | 71–81 | Caller guarantees alignment |
| **128B header layout (every byte)** | `src/scrap_page.h` · `scrap_header_t` | 31–37 | Field offsets in comments; `_Static_assert` at **39** proves =128B |
| Entry struct (offset+size) | `src/scrap_page.h` · `scrap_entry_t` | 20–23 | The 8B index entry |
| Page struct (data ptr / base / lock / chain) | `src/scrap_page.h` · `scrap_page_t` | 46–52 | `data` is the separate alloc; `lock` trivial in MVP |
| **Separate posix_memalign data zone** | `src/scrap_page.c` · `scrap_page_alloc` | 11–38 | The CRITICAL alloc at **21**; zero-fill 28 |
| Segment coalesce / merge logic | `src/scrap_page.c` · `coalesce_insert` | 63–115 | How 15 entries stay disjoint + overflow detection |
| Merge entry point (overflow-safe probe) | `src/scrap_page.c` · `scrap_page_merge` | 117–130 | Probes a header *copy* (122–124) before committing |
| is_full (counter == 256KB) | `src/scrap_page.c` · `scrap_page_is_full` | 49–52 | |
| **Synchronous flush = OTflush stand-in** | `src/scrap_page.c` · `scrap_page_flush` | 132–169 | Full path 134–141; partial read-before-write 143–168. **This whole function gets replaced by async Q1/Q2 in C4.** |
| Page-split loop (write straddles pages) | `src/wsbuffer.c` · `wsb_write` | 115–132 | |
| Overflow → flush → retry | `src/wsbuffer.c` · `scrap_write_chunk` | 54–96 | MVP eviction stand-in (67–83) |
| Hash index (single global, **NO locks yet**) | `src/page_index.c` | 10–87 | `pi_hash` 19–23; get_or_create 45–64. **C3 adds sharded locks here.** |
| Bench seed | `bench/benchmark.c` | all | Seeds `bench_engine.c` in C10 |

**Not built yet (don't go looking — these are NEW in their cycle):**
- `tag = FLUSHING` state — only `WSB_TAG_OPEN`/`WSB_TAG_FULL` exist today (`wsbuffer_config.h:32–33`). C5 adds FLUSHING.
- `otflush.{h,c}`, `queue.{h,c}` — C4.
- Per-shard / per-bucket locks in `page_index` — C3.
- `wsb_read()`, `wsb_fsync()` — not in `wsbuffer.c` yet. C6.
- All `lsm/` files — C7–C9. `baselines.{h,c}`, `bench_lsm.c` — C10.

> **How the existing code shifts the early cycles:** for C1 and C2 the BUILD card is essentially *already done*. Treat those cycles as **REVIEW-only** — study the theory, then read the mapped lines and answer the defense questions. C0's only real BUILD is the Makefile/deploy + the throwaway probe. The first cycle where Claude writes substantial new code is **C3**.

---

# PHASE A — ENGINE FOUNDATION (Hardware & the Fast Path)

## Cycle 0 — Environment & the alignment battle

**What Claude builds:** Makefile targets, `make deploy` (rsync to Proxmox), a throwaway `o_direct_probe.c` that writes 10 MB three ways: (1) `malloc`+`write`, (2) `malloc`+`O_DIRECT` → **must fail `EINVAL`**, (3) `posix_memalign`+`O_DIRECT` → succeeds.

**Study before:**
- TLPI ch 4 (file I/O: `open`/`read`/`write`/`close`, fd model), ch 5 (`O_DIRECT`, file flags).
- `docs/02_posix_constraints.md` + `docs/03_pio_theory.md` (why bypass the page cache at all).
- Concept: what the **kernel page cache** is, and what `O_DIRECT` skips.

**Review checklist — you must be able to explain:**
- [ ] Why `malloc` buffer + `O_DIRECT` fails with `EINVAL` (the 3 alignment rules: buffer addr, offset, length all % 4096).
- [ ] What `posix_memalign(…, 4096, …)` returns differently from `malloc`.
- [ ] What "bypassing the page cache" means physically — where does the data go instead.
- [ ] Why this project even exists (PIO model: read/write asymmetry + concurrency; C1/C2/C3).

**Defense questions:**
1. What is the OS page cache and why is it a bottleneck for NVMe?
2. State the three `O_DIRECT` alignment constraints and what enforces them.
3. Why can't you test this on your Mac — what's special about the Proxmox+XFS setup?

**Deliverable:** probe runs on the VM; you can demo the deliberate `EINVAL` and the fix.

---

## Cycle 1 — The Fast Path & engine skeleton

**What Claude builds / extends:** `io_direct.{h,c}` (`pread`/`pwrite` wrappers, `EINVAL` → loud "O_DIRECT alignment violation"), `wsbuffer.c` router fast-path branch (`size ≥ 1MB && size%4096==0 && offset%4096==0` → straight to `pwrite`). *(Much already exists — your job is to understand it, not rewrite.)*

**Study before:**
- TLPI ch 5 again: **positional I/O** — `pread`/`pwrite` vs `read`/`write`+`lseek`. Why the global file offset is shared across threads.
- NVMe read/write asymmetry (paper + `docs/03`).

**Review checklist:**
- [ ] Why `pwrite` not `write`+`lseek` — the shared-offset race (even before threads exist, it's the right habit).
- [ ] The exact fast-path condition. Why **all three** (size threshold, size alignment, offset alignment) are required — what happens if any one is dropped.
- [ ] Walk `wsb_write()` in `src/wsbuffer.c` line by line and narrate the routing decision.
- [ ] Why fast path bypasses the scrap buffer entirely (it's already aligned + big → no read-before-write, no merge needed).

**Defense questions:**
1. Why 1 MB as the threshold — what's the trade-off if it were 64 KB? 16 MB?
2. An LSM compaction writes a 200 MB SSTable. Which path, and why is that optimal?
3. What does `pwrite` guarantee about the file offset that `write` does not?

**Deliverable:** fast path benchmarked on VM ≈ raw `O_DIRECT` bandwidth.

---

# PHASE B — THE DATA STRUCTURES (single-threaded first)

## Cycle 2 — Scrap page & page index (no concurrency yet)

**What Claude builds / extends:** `scrap_page.{h,c}` — the 128-byte header (`counter`, `number`, `ssd_id`, `tag`, 15× 8-byte entries) + separately `posix_memalign`'d 256 KB data zone; `scrap_page_merge` (overlay a segment, update entries), `is_full`, `flush`. `page_index.{h,c}` — hash map offset→page (single lock for now).

**Study before:**
- `docs/01` §2 (exact struct layout) — memorize the header fields and *why* the data zone is allocated separately (alignment).
- C: struct layout, alignment/padding, why header (128 B) and data (256 KB) must not be one `malloc`.
- Concept: a **hash table** (buckets, collisions) — basic, you'll shard it later.

**Review checklist:**
- [ ] Draw the `scrap_page_t` from memory: 128 B header fields + 256 KB zone. Why exactly 128 and 256 KB (check `docs/01`, don't guess).
- [ ] Why the data zone needs its own `posix_memalign` — what breaks if it's inline with the header.
- [ ] What the 15 `entries` track (offset+size of each valid segment) and why only 15 (header size budget). What happens on the 16th disjoint segment (overflow → flush).
- [ ] What `counter` means (valid bytes) and how "full" = counter == 256 KB.
- [ ] Trace a small unaligned write through `wsb_write` → page-split loop → `scrap_page_merge`.

**Defense questions:**
1. Why a "scrap" page instead of a fixed full page like the kernel uses?
2. The header is exactly 128 bytes. Account for every byte.
3. A write straddles two 256 KB pages. How is it handled? (page-base split loop)
4. Why separate header allocation from data allocation — tie it back to `O_DIRECT`.

**Deliverable:** single-threaded write→flush→read-back→`memcmp` passes (correctness gate, scrap path).

---

# PHASE C — THE CONCURRENCY ABYSS (the thesis core — do not rush)

## Cycle 3 — Concurrency foundations + sharded page index

**Pre-work (BUILD THESE YOURSELF, no AI writing the locks):** three toy programs —
1. two threads `++` a shared int → watch it come out wrong (data race).
2. fix with `pthread_mutex_t`.
3. bounded producer/consumer queue with `pthread_cond_t`.
These three *are* OTflush in miniature. You must write them by hand.

**What Claude builds / extends:** `page_index.{h,c}` upgraded to **sharded bucket locks** (N shards, hash → shard → per-shard mutex). Per-page mutex in `scrap_page`.

**Study before:**
- OSTEP Concurrency: **Threads**, **Locks**, **Condition Variables**, **Concurrency Bugs** (races, deadlock, ordering). Do the homeworks.
- TLPI ch 29–30 (pthreads API: create/join, mutex, condvar).
- Concept: **data race vs race condition**; why a single global lock = the XArray bottleneck the paper fights; **lock sharding**; deadlock & lock ordering.
- Learn **ThreadSanitizer** now — compile a toy with `-fsanitize=thread`, watch it catch the race.

**Review checklist:**
- [ ] Explain a data race in one sentence + show the toy where you saw it.
- [ ] Why one global index lock kills SSD parallelism (CPU serializes page management → SSD channels starve). This is the C2 argument — own it cold.
- [ ] How sharding fixes it: independent offsets hit independent shards → no contention. What determines shard count.
- [ ] Per-page mutex: why page A's foreground merge must not block page B's flush.
- [ ] Run the engine's write path under TSan → clean (or explain every finding).

**Defense questions:**
1. What is the XArray and why does the kernel page cache serialize on it under concurrent writes?
2. How does sharded locking restore SSD internal parallelism? Why not just one lock?
3. Two threads write overlapping offsets in the same page — what protects correctness?
4. Could your sharding deadlock? Why/why not? (lock ordering / single lock held at a time)

**Deliverable:** N threads write disjoint offsets concurrently; TSan-clean; no thread blocks another (measure: throughput scales with threads).

---

## Cycle 4 — OTflush: async two-stage flushing (the heart)

**What Claude builds:** `queue.{h,c}` (generic MPMC queue, mutex+condvar), `otflush.{h,c}` — Q1/Q2, Stage-1 & Stage-2 thread pools, the `Bcount` SSD-busyness counter. Router rewired: partial page → **enqueue Q1 immediately on create/modify**; full page → skip Q1 → Q2. Stage-1 hole-fill `pread` **gated by `Bcount`** (opportunistic, idle-driven). Stage-2 `pwrite`/`pwritev` → reclaim RAM.

**Study before:**
- Re-read spec **§3.1** until the *opportunistic* idea is crisp.
- OSTEP producer/consumer + thread pools.
- `docs/01` §4 (OTflush stages), TLPI scatter-gather (`pwritev`, `struct iovec`).
- Concept: **why opportunistic, not pressure-triggered** — flooding the SSD with Stage-1 reads under pressure causes the exact latency spike you're trying to avoid. This distinction *is* the paper's novelty.

**Review checklist — the cycle that makes or breaks the thesis:**
- [ ] Explain the full lifecycle: `wsb_write` → merge → (full→Q2 | partial→Q1) → Stage-1 hole-fill (idle-gated) → page full → Q2 → Stage-2 pwrite → reclaim.
- [ ] Why a partial page enters Q1 **immediately**, not at a memory watermark. What the alternative would cause.
- [ ] What `Bcount` measures and how Stage-1 uses it to drain Q1 only in the SSD's idle gaps.
- [ ] Why a full page skips Q1 entirely (no read needed) — *this is the win over buffered I/O*.
- [ ] Why foreground `wsb_write` **never blocks on I/O**.
- [ ] When `pwritev` batches instead of `pwrite` (contiguous regions).
- [ ] Read the queue impl: how condvar makes a thread sleep without burning CPU and how it's woken.

**Defense questions:**
1. Walk me through what happens to a 4 KB write from `wsb_write` return to bytes-on-SSD.
2. Why is the enqueue opportunistic and not triggered by memory pressure? What fails otherwise?
3. What is the read-before-write penalty and exactly how does Stage-1 hide it?
4. How does a full page avoid the penalty entirely?
5. How do your background threads avoid busy-waiting (spinning)?

**Deliverable:** thousands of chaotic small writes; foreground never stalls; everything lands on disk; TSan-clean. **C3 demonstrated.**

---

## Cycle 5 — Flush/write race (`tag`) + eviction watermark

**What Claude builds:** the `tag=FLUSHING` swap protocol (spec §3.3) — Stage-2 locks page, sets `tag=FLUSHING`; a foreground write hitting a FLUSHING page allocs a fresh page, swaps the hash-index pointer, old page drains its single `pwrite` then frees. The **per-region write-ordering invariant** mechanism (generation counter or in-flight guard, spec §3.3 residual). Eviction watermark (spec §3.6): high/low watermark throttles foreground writes when OTflush falls behind.

**Study before:**
- Spec §3.3 and §3.6 — read 5×.
- Concept: **read-modify-write race on shared state**; pointer swap as atomic handoff; generation counters; **backpressure** (high/low watermark hysteresis).

**Review checklist — the subtle-bug cycle examiners love:**
- [ ] Narrate the FLUSHING swap: who sets the tag, what the foreground writer does instead of merging, why the index only ever points at the live page.
- [ ] Why this prevents two-writers-on-one-region.
- [ ] State the per-region ordering invariant in one sentence and the mechanism enforcing it. Why it's "near-impossible to violate but must be stated."
- [ ] Watermark: why it's a **fallback**, NOT the Q1 trigger (don't confuse with §3.1). What high vs low watermark do (hysteresis avoids thrash).

**Defense questions:**
1. A foreground write arrives while its page is mid-flush. Walk through the swap. Where's the race if you *didn't* swap?
2. What's the residual ordering invariant and how do you guarantee it?
3. Why is the eviction watermark separate from the opportunistic Q1 enqueue? What goes wrong if you merge the two concepts?
4. Why high *and* low watermark, not a single threshold?

**Deliverable:** stress test — overlapping writes during active flush; integrity holds; bounded RAM under sustained overload; TSan-clean.

---

# PHASE D — DURABILITY & READ PATH

## Cycle 6 — Read path + fsync + durability contract

**What Claude builds:** `wsb_read()` (spec §3.5) — aligned `pread` from disk (bounce buffer if user buf unaligned) + overlay every resident scrap page's valid segments (read-your-writes). `wsb_fsync()` — flush all dirty pages → wait completion → `fsync(fd)`.

**Study before:**
- OSTEP **Crash Consistency** chapter (fsync, what's durable when).
- Spec §3.4, §3.5.
- Concept: **read-your-writes**; bounce buffers for unaligned reads; the durability contract (durable = whatever was flushed; RAM lost on crash is *correct*).

**Review checklist:**
- [ ] Why a read must overlay RAM pages on top of the disk read (a just-written value may not be on disk yet).
- [ ] Why `wsb_read` is symmetric to the partial-page flush.
- [ ] What `wsb_fsync` guarantees after it returns. Why the engine keeps **no cross-restart state** and why that's correct (recovery is the LSM's job).
- [ ] Bounce buffer: when and why (user buffer not 4 KB-aligned).

**Defense questions:**
1. Write then immediately read the same offset before it's flushed — how do you return the right bytes?
2. What does `fsync` actually guarantee at the hardware level? What about the drive's write cache?
3. Why doesn't the engine do crash recovery itself? Where does recovery live?

**Deliverable:** write→read-immediately→correct value (data still in RAM); integrity gate per path.

---

# PHASE E — THE THIN LSM (L4)

## Cycle 7 — MemTable + WAL

**What Claude builds:** `memtable` (skiplist, size-bounded — Claude can generate the skiplist, but you must understand it), `wal` (append-only; every put appended **before** memtable insert → `wsb_write` small → scrap path).

**Study before:**
- RocksDB/LevelDB design docs: what a memtable and WAL are.
- OSTEP **LFS** chapter (log-structured thinking = the whole LSM idea).
- Concept: **skiplist** (probabilistic balanced structure, O(log n) ordered); **write-ahead logging** (durability before apply).

**Review checklist:**
- [ ] Why WAL append happens *before* memtable insert (crash between them = WAL replay recovers it).
- [ ] How a skiplist gives ordered O(log n) inserts/lookups — explain the levels.
- [ ] Why WAL appends are small/unaligned → naturally route to the scrap path (the spine!).
- [ ] Why the memtable can use a coarse lock (the concurrency thesis is in the *engine*, not here — YAGNI §4.3).

**Defense questions:**
1. Why log before applying to the memtable?
2. Which engine path does a WAL append take and why — without you telling the LSM?
3. Why a skiplist over a balanced BST?

**Deliverable:** put→WAL append→memtable insert; survives process restart via WAL replay (full crash test comes Cycle 9).

---

## Cycle 8 — SSTable + memtable flush

**What Claude builds:** `sstable` (immutable: data blocks + sparse index + footer), memtable-full → flush to a new SSTable file via `wsb_write` ≥1 MB aligned → **fast path**.

**Study before:**
- LevelDB SSTable format (data blocks, index block, footer).
- Concept: **immutability** (why SSTables are never updated in place); **sparse index** (one key per block, binary-search then scan).

**Review checklist:**
- [ ] SSTable layout: data blocks → sparse index → footer. Why sparse, not dense.
- [ ] Why the flush is a big aligned sequential write → fast path. Tie to the spine table (spec §2).
- [ ] Why SSTables are immutable and what that buys (no in-place update, lock-free reads, easy compaction).
- [ ] One `wsb_open` handle per SSTable file (decision 1(a)).

**Defense questions:**
1. Why is a memtable flush exactly the workload the fast path is designed for?
2. How do you find a key in an SSTable with only a sparse index?
3. Why immutable? What would in-place updates cost?

**Deliverable:** memtable flush produces an SSTable on disk via fast path; read a key back from it.

---

## Cycle 9 — Compaction + manifest + get path + recovery

**What Claude builds:** size-tiered `compaction` (merge N SSTables → one larger, drop tombstones/overwrites; reads via `wsb_read`, writes big via fast path), `manifest` (tracks live SSTables/tiers), full `get` (memtable → SSTables newest→oldest, first hit wins, tombstone = deleted), recovery (replay WAL → rebuild memtable; load SSTables from manifest).

**Study before:**
- Size-tiered compaction (vs leveled — and why you chose tiered, §4.3 YAGNI).
- Concept: **tombstones** (delete = write a marker), **merge iterator**, **manifest** as the source of truth for live files, **LSM read amplification** (why newest→oldest).

**Review checklist:**
- [ ] Why `get` searches memtable first, then SSTables newest→oldest, first hit wins.
- [ ] What a tombstone is and why deletes can't just remove data (immutable SSTables below).
- [ ] What compaction drops and why (superseded versions + tombstones) — reclaims space, bounds read amp.
- [ ] Why compaction is *both* big read (`wsb_read`) and big write (fast path) — the spine on a real workload.
- [ ] Recovery: WAL replay rebuilds the memtable; manifest names the live SSTables. Why the engine contributes nothing here.

**Defense questions:**
1. Trace a `get` for a key that was written, deleted, then the SSTables compacted.
2. Why size-tiered over leveled for a "thin" LSM?
3. After `kill -9` mid-load, exactly what recovers the un-flushed data, step by step?
4. How does compaction exercise *both* engine paths — and why does that prove the routing design on a real workload (the thesis)?

**Deliverable:** full KV store; **crash-consistency gate** — `kill -9` mid-load → reopen → committed data survives.

---

# PHASE F — EVALUATION (turn code into evidence)

## Cycle 10 — Baselines, experiments, telemetry, report

**What Claude builds:** `baselines.{h,c}` (page-cache buffered `write()` + **raw `O_DIRECT`** no-scrap reference impls), `bench_engine.c` (E1/E2/E3), `bench_lsm.c` (E4 + crash test). Measurement harness: throughput, latency **p50/p99/p99.9**, CPU util, SSD bandwidth.

**Study before:**
- Spec §5 — all four experiments and what each *proves* (E1↔C3, E2↔C2, E3↔C1, E4↔integration).
- `perf stat` (CPU cycles, context switches), `iostat -x` (SSD util/bandwidth).
- Concept: **latency percentiles** (why mean lies, why p99/p99.9 = tail matters), warmup, repeated runs, controlling variables.

**Review checklist:**
- [ ] For each experiment: what it proves, which baseline it compares against, the expected shape of the graph.
- [ ] **E2 is the money graph** — scale writer threads 1→N: page-cache plateaus (XArray), WSBuffer scales near-linear. Be able to explain *why* the lines diverge.
- [ ] Why you need **two** baselines (page-cache AND raw O_DIRECT), not one — what each isolates.
- [ ] Why report p99/p99.9, not just mean throughput.
- [ ] CPU utilization is the C1 proof (less CPU for same/more bandwidth). How `perf` shows it.

**Defense questions:**
1. Walk me through E2 and why page-cache collapses while WSBuffer scales.
2. Why is raw `O_DIRECT` a baseline if your fast path *is* O_DIRECT? (E3: fast path ≈ raw → proves no overhead; raw can't do E1's unaligned writes → proves the scrap buffer's value.)
3. Your throughput is high but p99.9 latency spikes — what would you investigate? (Stage-1 not idle-gated? watermark thrash?)
4. How do you prove the CPU saving, not just the bandwidth?

**Deliverable:** E1–E4 graphs on the VM + a 4–6 page ACM/IEEE-style report. You can defend every number.

---

## Final defense readiness — the one-paragraph thesis

You must be able to say this, unscripted:

> *"The OS page cache fails high-bandwidth NVMe on three axes (PIO model): over-buffering burns CPU (C1), the XArray lock serializes concurrent writes and starves the SSD's internal channels (C2), and partial writes pay a synchronous read-before-write penalty (C3). WSBuffer routes large aligned writes straight to the SSD via O_DIRECT (fixes C1/C2) and absorbs small/unaligned writes into a user-space scrap buffer, hiding the read-before-write penalty in the SSD's idle cycles via opportunistic two-stage flushing (fixes C3). Sharded locks replace the global XArray lock. A thin LSM proves the engine's router is exactly the routing a real database needs — for free, because the workload's natural shape routes itself."*

If any clause in that paragraph is fuzzy, the cycle that owns it isn't done.

---

## How to work with Claude each cycle

1. **Branch per cycle.** One reviewable diff.
2. **Study the cycle's theory FIRST** — at least the "Review checklist" concepts — before Claude writes code, so you can review with eyes open.
3. Let Claude implement. Then **interrogate it**: "explain why X", "what breaks if I remove the lock here", "show me where the race would be without the tag."
4. **Run the gate** (TSan / memcmp integrity / crash test) — evidence before "done."
5. **Self-test:** answer the Defense questions out loud. Can't? → study gap. Close it before the next cycle.
6. Only then merge.
