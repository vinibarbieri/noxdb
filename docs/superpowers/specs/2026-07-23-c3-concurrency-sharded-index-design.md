# C3 — Concurrency Foundations + Sharded Index (Design)

**Date:** 2026-07-23
**KANBAN card:** C3-BUILD (Phase C — Concurrency Abyss)
**Status:** approved, ready for implementation plan

## Goal

Make the NoxDB scrap write path safe and scalable under N concurrent writer
threads. Today `page_index` mutates its bucket lists with **no locking** (the C2
"single global, NO locks → sharded in C3" note). C3 replaces that with a
**sharded-lock** index so disjoint-page writers run in parallel instead of
serializing on one global lock — the XArray bottleneck the whole design exists
to escape (`docs/01` §5).

## Scope decisions (settled during brainstorming)

1. **Locking model: internal sharded locks; defer the lifetime gap to C5.**
   `page_index` functions lock their shard internally and return a page pointer.
   A window remains in `scrap_write_chunk` between `get_or_create` returning `p`
   and the caller acquiring `p->lock`; in that window another thread could
   `remove`+`free` the same page. For the C3 gate — **disjoint** offsets, so each
   256KB base is owned by exactly one thread — this window cannot fire. The full
   fix is the `tag=FLUSHING` pointer-swap protocol, which KANBAN schedules for
   **C5**. Not pulled forward.

2. **Shard count = 64, cache-line padded.** Engineering choice, not dictated by
   docs. See `docs/01` §5 for the recorded rationale. Summary: 64 > NVMe queue
   depth (SN530 ≈ QD32) and > bench box cores, so false contention is negligible;
   padding each lock to `NOX_CACHELINE` (64 B) kills false sharing that a
   per-bucket (1024-lock) scheme would introduce. Total lock memory ≈ 4 KB.

3. **Gate verification: shadow + raw O_DIRECT read-back, plus a TSan build.**
   Reuse the proven `scrap_integrity_test` pattern (per-region shadow → flush →
   re-read via a *separate* raw fd → `memcmp`) so the gate proves BOTH
   race-freedom (TSan) and data correctness (read-back), not just one.

## Components

### 1. `src/noxdb_config.h` — new constants
```c
#define NOX_INDEX_SHARDS  64u   /* power of two; > NVMe QD + cores. Engineering pick, not from docs. */
#define NOX_CACHELINE     64u   /* pad shard locks so distinct shards never share a cache line. */
```

### 2. `src/page_index.{h,c}` — sharded locks
- Cache-line-padded shard lock type and array on the index:
  ```c
  typedef union { pthread_mutex_t mtx; char pad[NOX_CACHELINE]; }
      _Alignas(NOX_CACHELINE) pi_shard_t;

  struct page_index {
      scrap_page_t *buckets[PI_BUCKETS];   /* 1024, unchanged */
      pi_shard_t    shards[NOX_INDEX_SHARDS];
  };
  ```
- Shard selection reuses the existing hash: `pi_shard(base) = pi_hash(base) & (NOX_INDEX_SHARDS-1)` → 16 buckets per shard.
- **`page_index_create`**: allocate the struct with `posix_memalign(NOX_CACHELINE, …)` + `memset` (plain `calloc` only guarantees 16 B alignment, which would make the cache-line padding a lie), then `pthread_mutex_init` each of the 64 shard locks.
- **`page_index_get_or_create`**: lock the shard, walk/insert into the bucket list, unlock. On OOM, **unlock the shard before returning NULL**.
- **`page_index_remove`**: lock the shard, unlink + free, unlock.
- **`page_index_destroy`**: free all pages, `pthread_mutex_destroy` each shard, free the struct.
- **`page_index_foreach`**: stays lock-free — called only from `nox_close`, which is single-threaded (no concurrent writers). Documented explicitly.

### 3. `src/scrap_page.{h,c}` — no functional change
The per-page `pthread_mutex_t lock` already exists and is already taken around
`scrap_page_merge` in `scrap_write_chunk`. C3 only **documents** the invariant:
the shard lock and a page lock are **never held simultaneously** (get_or_create
drops the shard lock before the caller takes the page lock), so there is no
lock-order inversion between the two lock classes.

### 4. `src/noxdb.c` — comments only
Document, at `scrap_write_chunk`, the get-then-lock window and why it is safe for
disjoint bases (C3 gate) with the full fix deferred to C5.

### 5. `bench/concurrency_test.c` — the C3 gate (new)
- Takes `path` and `nthreads` args.
- Partitions the file into disjoint 256KB regions, one contiguous block of
  regions per thread (no two threads share a base → exercises index/shard
  parallelism, not per-page contention).
- Each thread issues small / unaligned / boundary-respecting writes into its own
  regions via `nox_write` (forces the scrap path) and mirrors them into a
  per-region shadow buffer.
- After join + `nox_close` (flushes all partial pages), re-read every region with
  a **separate raw O_DIRECT fd** and `memcmp` against the shadow.
- Print aggregate throughput (MB/s). Operator runs at 1/2/4/8 threads and eyeballs
  the scaling curve.

### 6. `Makefile` — two targets
```make
gate-c3:       # optimized build, throughput + integrity
	links $(OBJ) + bench/concurrency_test.o  ->  bench/concurrency_test
gate-c3-tsan:  # single -fsanitize=thread -g compile of all src/*.c + driver
	cc ... -fsanitize=thread -g -o bench/concurrency_test_tsan $(SRC) bench/concurrency_test.c -pthread
```
TSan gets its own single-compile target (like `probe`) so instrumented objects
never mix with the `-O2` build.

## Data flow (one writer thread, scrap path)

```
nox_write
  -> scrap_write_chunk(base, intra, buf, len)
       p = page_index_get_or_create(base)      [lock shard S; walk/insert; unlock S]
       pthread_mutex_lock(&p->lock)            <-- get-then-lock window (safe: disjoint base)
       scrap_page_merge(p, ...)
       pthread_mutex_unlock(&p->lock)
       if full: scrap_page_flush(p); page_index_remove(base)   [lock shard S]
```

Disjoint offsets → distinct bases → threads seldom share a shard and **never**
share a page, so both lock classes stay uncontended in the common case.

## Error handling

- `page_index_get_or_create` releases the shard lock before any `NULL` return
  (OOM). No lock leaked on the error path.
- `pthread_mutex_init`/`destroy` return values checked on the index lifecycle
  paths.
- Existing `io_direct` behavior unchanged, including the loud
  `"O_DIRECT alignment violation"` on `EINVAL`.

## Testing / acceptance (C3-GATE)

1. `make gate-c3` on the bench box; run against `/mnt/nvme/c3gate.dat` at
   1/2/4/8 threads → all regions match disk (integrity) and MB/s rises with
   thread count (scaling).
2. `make gate-c3-tsan` on the bench box; run → ThreadSanitizer reports **no data
   races** on the concurrent write path.

Both must be run on the bare-metal bench box against `/mnt/nvme` — O_DIRECT does
not work on the local macOS filesystem.

## Files touched

| File | Change |
|---|---|
| `src/noxdb_config.h` | add `NOX_INDEX_SHARDS`, `NOX_CACHELINE` |
| `src/page_index.h` | shard lock type / doc |
| `src/page_index.c` | sharded locking in create/get_or_create/remove/destroy |
| `src/noxdb.c` | comments (window / lock-order invariant) |
| `src/scrap_page.c` | comments only |
| `bench/concurrency_test.c` | **new** — C3 gate driver |
| `Makefile` | `gate-c3`, `gate-c3-tsan` targets |
| `docs/01_architecture_noxdb.md` | §5 records the 64-shard choice + rationale |

## Explicitly out of scope (later cycles)

- `tag=FLUSHING` pointer-swap / page lifetime protocol → **C5**.
- Background OTflush thread pools, Q1/Q2, `Bcount` → **C4**.
- Eviction watermark / backpressure → **C5**.
