# NoxDB — Kanban

Companion to `plan.md`. Each **cycle** = 4 post-its that travel the columns left→right.
To move a card: change its status emoji. Update the Board table + the card block together.

## Columns (status flow)

| Emoji | Column | Meaning |
|---|---|---|
| 📥 | **Backlog** | Not started |
| 📚 | **Estudando** | Reading the theory for this card |
| 🤖 | **Claude Codando** | Claude is writing / extending the code |
| 🔍 | **Revisando** | Code exists; you're reviewing + answering defense Qs |
| ✅ | **Done** | Gate passed, you can explain it, merged |

## Card types (one of each per cycle)

- 📚 **STUDY** — theory to absorb *before* Claude codes.
- 🤖 **BUILD** — what Claude writes. (📦 = already in the repo → BUILD done, go straight to REVIEW.)
- 🔍 **REVIEW** — checklist + defense questions = acceptance criteria. REVIEW cards for C0–C2 carry `file:line` targets so you don't hunt. Full map: `plan.md` → "Existing code map".
- ✅ **GATE** — the test/deliverable that proves the cycle done.

> **WIP limit: 1 cycle at a time.** Don't pull the next cycle's STUDY until the current cycle's GATE is ✅. Phase C (cycles 3–5) especially — no skipping ahead.

---

## Board (glance view)

| Cycle | 📚 STUDY | 🤖 BUILD | 🔍 REVIEW | ✅ GATE |
|---|---|---|---|---|
| **C0** Env & alignment | 📥 | 📥 (probe/Makefile new; io_direct 📦) | 📥 | 📥 |
| **C1** Fast path | 📥 | 📦 exists | 📥 | 📥 |
| **C2** Scrap page + index | 📥 | 📦 exists | 📥 | 📥 |
| **C3** Concurrency + sharding | 📥 | 📥 | 📥 | 📥 |
| **C4** OTflush (heart) | 📥 | 📥 | 📥 | 📥 |
| **C5** tag race + watermark | 📥 | 📥 | 📥 | 📥 |
| **C6** Read + fsync | 📥 | 📥 | 📥 | 📥 |
| **C7** MemTable + WAL | 📥 | 📥 | 📥 | 📥 |
| **C8** SSTable + flush | 📥 | 📥 | 📥 | 📥 |
| **C9** Compaction + recovery | 📥 | 📥 | 📥 | 📥 |
| **C10** Eval + report | 📥 | 📥 | 📥 | 📥 |

Legend: 📥 Backlog · 📚 Estudando · 🤖 Codando · 🔍 Revisando · ✅ Done · 📦 already in repo (review, don't build)

---

# PHASE A — Engine Foundation

### 🟦 C0 · Env & the alignment battle

> **Bench environment (decided):** bare-metal **Ubuntu Server 24.04 LTS** — no Proxmox/VM/LXC (hypervisor jitter + shared cache poison p99/CPU; a container can't `mkfs`/`mount`). Device under test = a **dedicated clean NVMe** (WD SN530) formatted XFS at `/mnt/nvme`, separate from the OS disk (Kingston NV2 over USB). Run all probes/benches against `/mnt/nvme/...`, never the OS disk.

**C0-STUDY** 📥
TLPI ch 4 (file I/O, fd model) + ch 5 (`O_DIRECT`, flags). `docs/02` + `docs/03`. Concept: page cache vs what O_DIRECT skips.

**C0-BUILD** 📥
Makefile + `make deploy` (rsync→bare-metal Ubuntu bench box). `o_direct_probe.c`: 10 MB three ways — malloc+write, malloc+O_DIRECT (must EINVAL), posix_memalign+O_DIRECT (ok).

**C0-REVIEW** 📥
Explain: why malloc+O_DIRECT → EINVAL (3 align rules); what posix_memalign gives; what "bypass page cache" means physically; why this project exists (C1/C2/C3).
📍 Review here: `io_direct.c` `io_direct_open` 31–37 · `report_einval` 17–29 · EINVAL catch 64–66, 77–78. `noxdb_config.h` `NOX_IS_ALIGNED` 36, `NOX_BLOCK_SIZE` 14.

**C0-GATE** 📥
Probe runs on the bench box against `/mnt/nvme`. You can demo the deliberate EINVAL + the fix.

---

### 🟦 C1 · Fast path & engine skeleton

**C1-STUDY** 📥
TLPI ch 5: `pread`/`pwrite` vs `read`+`lseek` (shared offset). NVMe read/write asymmetry (paper, `docs/03`).

**C1-BUILD** 📥
`io_direct.{h,c}` (EINVAL → loud "O_DIRECT alignment violation"). Router fast-path branch: `size≥1MB && size%4096==0 && offset%4096==0` → `pwrite`. *(Mostly exists — understand it.)*

**C1-REVIEW** 📥
Explain: why pwrite not write+lseek; why all 3 fast-path conditions; narrate `nox_write()` routing line by line; why fast path skips scrap buffer.
📍 Review here: `noxdb.c` `nox_write` 98–134 (fast-path test **106–111**). `io_direct.c` `io_direct_pwrite` 39–69 (bounce 47–54; pwrite-not-lseek 57–58). `noxdb_config.h` `NOX_DIRECT_THRESHOLD` 18.

**C1-GATE** 📥
Fast path benchmarked on the bench box ≈ raw O_DIRECT bandwidth.

---

# PHASE B — Data Structures (single-threaded)

### 🟩 C2 · Scrap page + page index

**C2-STUDY** 📥
`docs/01` §2 (128 B header layout). C: struct layout, padding, alignment. Concept: hash table basics.

**C2-BUILD** 📥
`scrap_page.{h,c}`: 128 B header + separate posix_memalign 256 KB zone; `merge`, `is_full`, `flush`. `page_index.{h,c}`: hash offset→page (single lock for now).

**C2-REVIEW** 📥
Draw struct from memory; why separate data alloc; what 15 entries track + overflow behavior; what `counter` means; trace unaligned write → page-split → merge.
📍 Review here: `scrap_page.h` `scrap_header_t` 31–37 + `_Static_assert` **39** · `scrap_entry_t` 20–23 · `scrap_page_t` 46–52. `scrap_page.c` `scrap_page_alloc` 11–38 (separate alloc **21**) · `coalesce_insert` 63–115 · `scrap_page_merge` 117–130 · `scrap_page_flush` 132–169 (sync OTflush stand-in, replaced in C4). `noxdb.c` page-split 115–132 · `scrap_write_chunk` 54–96. `page_index.c` (single global, NO locks → sharded in C3).

**C2-GATE** 📥
Single-thread write→flush→read-back→`memcmp` passes (scrap path integrity).

---

# PHASE C — Concurrency Abyss (THESIS CORE — do not rush)

### 🟥 C3 · Concurrency foundations + sharded index

**C3-STUDY** 📥
**Pre-work: write 3 toys yourself** (race / mutex fix / producer-consumer condvar). OSTEP Concurrency (Threads, Locks, Condvars, Bugs) + homeworks. TLPI ch 29–30. Learn ThreadSanitizer.

**C3-BUILD** 📥
`page_index` → sharded bucket locks (hash→shard→mutex). Per-page mutex in `scrap_page`.

**C3-REVIEW** 📥
Explain: data race in 1 sentence + show your toy; why 1 global lock = XArray bottleneck (C2 argument); how sharding restores SSD parallelism; per-page mutex rationale; run write path under TSan clean.

**C3-GATE** 📥
N threads write disjoint offsets concurrently; TSan-clean; throughput scales with threads.

---

### 🟥 C4 · OTflush — async two-stage flushing (THE HEART)

**C4-STUDY** 📥
Spec §3.1 until *opportunistic* is crisp. OSTEP producer/consumer + thread pools. `docs/01` §4. TLPI scatter-gather (`pwritev`, iovec). Concept: opportunistic vs pressure-triggered.

**C4-BUILD** 📥
`queue.{h,c}` (MPMC mutex+condvar). `otflush.{h,c}`: Q1/Q2, Stage-1/2 thread pools, `Bcount`. Router: partial→Q1 immediately, full→skip Q1→Q2. Stage-1 hole-fill pread gated by Bcount. Stage-2 pwrite/pwritev→reclaim.

**C4-REVIEW** 📥
Explain full lifecycle; why partial enters Q1 immediately not at watermark; what Bcount does; why full page skips Q1 (the win); why foreground never blocks on I/O; when pwritev batches; how condvar sleeps/wakes without burning CPU.

**C4-GATE** 📥
Thousands of chaotic small writes; foreground never stalls; all lands on disk; TSan-clean. **C3 demonstrated.**

---

### 🟥 C5 · tag=FLUSHING race + eviction watermark

**C5-STUDY** 📥
Spec §3.3 + §3.6 (read 5×). Concept: read-modify-write race; pointer swap as atomic handoff; generation counters; backpressure (high/low watermark hysteresis).

**C5-BUILD** 📥
tag=FLUSHING swap protocol (foreground hits flushing page → fresh page + swap index ptr → old drains+frees). Per-region ordering invariant (gen counter / in-flight guard). Eviction watermark (high/low throttle).

**C5-REVIEW** 📥
Narrate the swap + where race is without it; why index only points at live page; state ordering invariant + mechanism; why watermark is fallback NOT the Q1 trigger; why high+low not single threshold.

**C5-GATE** 📥
Stress: overlapping writes during active flush → integrity holds; bounded RAM under overload; TSan-clean.

---

# PHASE D — Durability & Read

### 🟪 C6 · Read path + fsync

**C6-STUDY** 📥
OSTEP Crash Consistency (fsync, what's durable when). Spec §3.4, §3.5. Concept: read-your-writes, bounce buffers, durability contract.

**C6-BUILD** 📥
`nox_read()`: aligned pread + overlay resident scrap pages (bounce buffer if unaligned). `nox_fsync()`: flush dirty → wait → `fsync(fd)`.

**C6-REVIEW** 📥
Explain: why overlay RAM on disk read; symmetry to partial-page flush; what fsync guarantees after return; why engine keeps no cross-restart state (recovery = LSM's job); bounce buffer when/why.

**C6-GATE** 📥
Write→read-immediately→correct value (still in RAM); integrity gate per path.

---

# PHASE E — Thin LSM (L4)

### 🟨 C7 · MemTable + WAL

**C7-STUDY** 📥
RocksDB/LevelDB docs (memtable, WAL). OSTEP LFS chapter. Concept: skiplist (O(log n) ordered), write-ahead logging.

**C7-BUILD** 📥
`memtable` (skiplist, size-bounded). `wal` (append-only; put → WAL append *before* memtable insert → small `nox_write` → scrap path).

**C7-REVIEW** 📥
Explain: why WAL before memtable insert; how skiplist gives ordered O(log n); why WAL appends route to scrap path (the spine); why coarse memtable lock OK (YAGNI §4.3).

**C7-GATE** 📥
put→WAL→memtable; survives restart via WAL replay (full crash test in C9).

---

### 🟨 C8 · SSTable + memtable flush

**C8-STUDY** 📥
LevelDB SSTable format (data blocks, index, footer). Concept: immutability, sparse index.

**C8-BUILD** 📥
`sstable` (immutable: data blocks + sparse index + footer). memtable-full → flush new SSTable via `nox_write` ≥1MB aligned → fast path.

**C8-REVIEW** 📥
Explain: SSTable layout + why sparse index; why flush = fast-path workload (spine); why immutable + what it buys; one nox_open per SSTable file.

**C8-GATE** 📥
Memtable flush → SSTable on disk via fast path; read a key back from it.

---

### 🟨 C9 · Compaction + manifest + get + recovery

**C9-STUDY** 📥
Size-tiered vs leveled (why tiered, §4.3). Concept: tombstones, merge iterator, manifest, read amplification (newest→oldest).

**C9-BUILD** 📥
Size-tiered `compaction` (merge N→1, drop tombstones/overwrites; read via nox_read, write big via fast path). `manifest`. Full `get` (memtable→SSTables newest→oldest, tombstone=deleted). Recovery (WAL replay + manifest load).

**C9-REVIEW** 📥
Explain: get search order; what a tombstone is + why deletes can't just remove; what compaction drops + why; why compaction hits BOTH paths (spine on real workload); recovery step by step.

**C9-GATE** 📥
Full KV store. **Crash-consistency gate:** `kill -9` mid-load → reopen → committed data survives.

---

# PHASE F — Evaluation

### ⬛ C10 · Baselines, experiments, report

**C10-STUDY** 📥
Spec §5 (E1↔C3, E2↔C2, E3↔C1, E4↔integration). `perf stat`, `iostat -x`. Concept: latency percentiles (why mean lies), warmup, repeated runs.

**C10-BUILD** 📥
`baselines.{h,c}` (page-cache write() + raw O_DIRECT). `bench_engine.c` (E1/E2/E3). `bench_lsm.c` (E4 + crash test). Harness: throughput, p50/p99/p99.9, CPU util, SSD bw.

**C10-REVIEW** 📥
Per experiment: what it proves + baseline + expected graph shape. Explain E2 money graph divergence; why TWO baselines; why p99/p99.9 not mean; CPU util = C1 proof via perf.

**C10-GATE** 📥
E1–E4 graphs on the bench box + 4–6 pg ACM/IEEE report. You can defend every number + recite the one-paragraph thesis (`plan.md` final section).

---

## How to use this board

1. Pull one cycle. Move its **STUDY** card 📥→📚. Read the theory.
2. STUDY done → 🤖, let Claude do **BUILD**.
3. Code exists → **REVIEW** 🔍: interrogate Claude ("why X? what breaks without it?"), answer defense Qs out loud.
4. Run **GATE** test. Pass + can explain → all 4 cards ✅, merge.
5. Can't answer a defense Q? → card back to 📚. Close the gap before next cycle.
