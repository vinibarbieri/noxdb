# C4 — OTflush: asynchronous two-stage flushing

**Date:** 2026-07-24
**Cycle:** C4 (Phase C — Concurrency Abyss)
**Status:** design approved, not implemented
**Source of truth:** Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs"*, FAST '26 (`~/Documents/Research/fast26-zhan.pdf`) §3.4 + Algorithm 2; `docs/01_architecture_noxdb.md` §4; `docs/00_flow_summary.md`; `docs/02_posix_constraints.md`.

---

## 1. Goal

Replace the synchronous flush stand-in (`scrap_page.c:132-169`, called inline from
`noxdb.c:100-104`) with the real OTflush mechanism: two queues, two background stages,
foreground never touching the SSD on the scrap path.

After C4 the foreground write path is: copy into the data zone → update header entries →
enqueue a pointer → return. Every `pread`/`pwrite` for the scrap path happens on a
background thread.

## 2. What the paper actually specifies

Extracted from §3.4 and Algorithm 2 so the port can be checked against it.

**SSD busyness awareness (`Bcount`).** Per-SSD counter of the *bytes of all in-flight I/O*.
Incremented by the I/O size immediately before submission, decremented on completion. An SSD
is "busy" when `Bcount >= threshold`; the paper's evaluation uses **4 MB**, on the grounds
that a 4 MB write nearly saturates the device. It is a coarse-grained, low-cost bandwidth
signal — not a per-page hole count, and it gates *both* stages.

**Stage-1 (Algorithm 2, lines 1-10).** Loop over Queue-1:
1. Pop head.
2. If the page is **full** → discard it (it was filled by foreground writes after being
   enqueued, and the foreground has already put it in Queue-2). Lines 4-5.
3. If the corresponding SSD is **not busy** → perform the SSD-read (hole fill), update the
   page's tag, insert into Queue-2. Lines 6-8.
4. Else → re-insert at the **tail of Queue-1**, to fill later. Lines 9-10.

**Stage-2 (Algorithm 2, lines 11-24).** Loop over Queue-2:
1. Pop head.
2. If the page is **invalid** (deleted as obsolete) → discard. Lines 14-15.
3. If `SSD-id == 0` → space not yet allocated (delayed allocation) → choose the least busy
   SSD, allocate, write back, reclaim. Lines 16-19.
4. Else if the SSD is **not busy** → write back, reclaim. Lines 20-22.
5. Else → re-insert at the **tail of Queue-2**. Lines 23-24.

**Queues.** Ring queues holding only the *address of the scrap-page header*, never a copy.
Default configuration is **one queue-thread pair per stage** (one Stage-1 thread, one
Stage-2 thread). Concurrency scales by splitting Q1/Q2 into sub-queues with a thread each.

**Locking (§3.5).** Scrap-page updates and Stage-1 take the per-scrap-page lock **without**
the index lock, because they do not change the tree structure. Stage-2 takes the per-page
lock to write back, and additionally an index-entry-level lock to clear the index entry
after flushing.

## 3. Divergences from the paper (deliberate, and why)

These are recorded because they are defensible thesis material, not accidents. Each one gets
a code comment at the site where it applies.

| # | Paper | NoxDB C4 | Why |
|---|---|---|---|
| D1 | `Bcount` tracks async `submit_bio` / `bi_end_io` completions; many I/Os in flight per thread | `Bcount` brackets a **blocking** `pread`/`pwrite` on a background thread | We are user-space POSIX, not in-kernel. Consequence: max in-flight = `threads × 256 KB` = 512 KB at the 1+1 default, so a 4 MB threshold **can never trip** below ~16 background threads. Implemented faithfully anyway (it is a C4-REVIEW defense question); threshold is a config constant so C10 can exercise it. |
| D2 | Multi-SSD; `SSD-id == 0` triggers delayed allocation and least-busy-device selection | Single SSD; the file offset is chosen by the caller, never by the engine | `docs/00_flow_summary.md` "Responsibilities": the engine is "dumb and obedient" — it does not choose addresses. A page's `base` is known at allocation. Algorithm 2 lines 16-19 have no analogue; Stage-2 always takes the line 20-22 branch. |
| D3 | Requeue-on-busy loops over a ring queue (lines 9-10, 23-24) | Requeue-on-busy must **block on a condvar with backoff** | With blocking I/O and one thread per queue, a requeue loop with nothing else runnable is a hot spin burning a core. See §6. |
| D4 | SXArray with delayed tree-structure updates | Sharded hash index (`page_index.c`, 64 padded shard locks, built in C3) | Already decided and built in C3. Same goal — no single non-scalable lock. |
| D5 | Unbounded scrap-buffer growth is bounded by a separate eviction mechanism | Q1/Q2 are **unbounded** in C4 | RAM bounding is the eviction watermark, which the board assigns to C5-BUILD ("high/low throttle") and C5-GATE ("bounded RAM under overload"). Blocking the producer in C4 would contradict C4-GATE's "foreground never stalls". A soft-threshold `stderr` warning is emitted; nothing blocks. |

## 4. Page lifetime and locking (the core decision)

The hard problem: once flushing is asynchronous, a background thread holds a pointer to a
page the foreground may still be writing into.

### 4.1 Rejected: detach-then-enqueue

Removing the page from the index at enqueue time makes lifetime trivial (the flush thread
becomes sole owner), but it **destroys Queue-1's purpose**. A partial page in Q1 must stay
reachable so foreground writes keep merging into it while it waits — that is precisely why
Algorithm 2 line 4-5 (`if page is full → discard`) exists. Detaching at enqueue makes that
branch dead code and throws away the pipelining win.

### 4.2 Adopted: detach at Stage-2, before the I/O, with lock coupling

**Lock order is uniformly `shard lock → page lock`.** No code path takes them in the other
order, so there is no inversion.

- A page stays in the index for its whole queued life (Q1 and/or Q2).
- `page_index_get_or_create()` acquires `p->lock` **before releasing the shard lock**, and
  returns with the page lock **held**. This is a contract change (see §5.3). It closes the
  get-then-lock window documented at `noxdb.c:64-73` and in `page_index.h:10-25` — C3's known
  debt disappears as a side effect of C4.
- Stage-1 takes only `p->lock` (paper §3.5: no index lock needed, structure unchanged).
- **Stage-2 sequence:** shard lock → page lock → verify the index still maps this base to
  *this exact pointer* → set `tag = FLUSHING` → remove from the index → **release both
  locks** → `pwrite`/`pwritev` → free. No lock is held across an I/O, so a foreground writer
  never blocks on the SSD; and once removed, no new writer can find the page.

**Consequence — Stage-2 is pinned to one thread in C4.** A fresh page for the same base can
be created the instant the old one is detached. With two Stage-2 threads, the fresh page
could reach the disk *before* the older one → same-base write reordering → data loss. One
Stage-2 thread plus a FIFO queue makes ordering hold by construction. This matches the
paper's own default. Lifting it needs the per-region generation counter that the board
assigns to C5-BUILD.

**What C4 does NOT fix.** The C3 disjoint-base precondition still holds for *foreground*
threads: two threads writing the same 256 KB base can still lose an update (not a
use-after-free — §4.2 removes that). The tag=FLUSHING pointer-swap in C5 is what makes
same-base foreground contention safe.

### 4.3 Page states

`scrap_header_t.tag` (existing `uint8_t`, `noxdb_config.h:49-51`) carries:

- `NOX_TAG_OPEN` — accepting writes.
- `NOX_TAG_FULL` — data zone fully covered; queued to Q2.
- `NOX_TAG_SEALED` — entry array exhausted (15 entries); no further merges accepted, drains
  as-is. **New.**
- `NOX_TAG_FLUSHING` — Stage-2 owns it; detached from the index. **New.**

A `detached` flag in `scrap_page_t` (outside the header) records that the page is no longer
in the index, so Stage-2 skips the index step. This is the analogue of Algorithm 2's
`page is invalid` check (line 14).

## 5. Components

### 5.1 `src/queue.{h,c}` — MPMC FIFO

Mutex + condition variable, unbounded, **intrusive** links.

```c
typedef struct nox_queue nox_queue_t;

nox_queue_t *nox_queue_create(size_t link_offset); /* offset of the link field in scrap_page_t */
void         nox_queue_destroy(nox_queue_t *q);
void         nox_queue_push(nox_queue_t *q, scrap_page_t *p);   /* never blocks */
scrap_page_t *nox_queue_pop(nox_queue_t *q);                    /* blocks; NULL on shutdown */
void         nox_queue_shutdown(nox_queue_t *q);                /* wake all consumers */
size_t       nox_queue_depth(const nox_queue_t *q);
```

Two link fields — `q1_next` and `q2_next` — live in `scrap_page_t`, **not** in
`scrap_header_t`: the header is pinned at exactly 128 bytes by the `_Static_assert` at
`scrap_page.h:39`, and a page can legitimately be in both queues at once (partial in Q1, then
filled by the foreground and pushed to Q2). Intrusive links also keep `malloc` off the
foreground enqueue path, which matters for the p99 numbers in C10.

Push is idempotent per queue: an `in_q1` / `in_q2` flag on the page, tested under `p->lock`,
prevents double-enqueue.

### 5.2 `src/otflush.{h,c}`

```c
typedef struct otflush otflush_t;

otflush_t *otflush_start(int fd);          /* create queues, spawn pools */
void otflush_enqueue_partial(otflush_t *o, scrap_page_t *p);  /* -> Q1 */
void otflush_enqueue_full(otflush_t *o, scrap_page_t *p);     /* -> Q2, skips Stage-1 */
int  otflush_drain(otflush_t *o);          /* block until Q1 and Q2 are empty */
int  otflush_stop(otflush_t *o);           /* shutdown + join; returns first I/O error */
```

Owns `Bcount` (an `_Atomic uint64_t`), the two queues, and the two thread pools. It needs the
`page_index_t *` too, for the Stage-2 detach step.

I/O errors on a background thread have no caller to return to: they are latched into a
sticky error field, reported loudly on `stderr`, and surfaced by `otflush_stop()` →
`nox_close()`'s return value.

### 5.3 `src/page_index.{h,c}` changes

- `page_index_get_or_create()` returns with `p->lock` **held**. Header contract block
  (`page_index.h:10-25`) is rewritten: the disjoint-base precondition narrows from "or you
  get a use-after-free" to "or you lose an update".
- New: `int page_index_detach_if(page_index_t *idx, uint64_t base, scrap_page_t *expect);` —
  under the shard lock, unlink only if the base still maps to `expect`. Does **not** free.
  Used by Stage-2.
- `page_index_remove()`'s free-the-victim behaviour is no longer used by the write path; keep
  it only for `page_index_destroy`.

### 5.4 `src/scrap_page.{h,c}` changes

Split the synchronous `scrap_page_flush()` (`scrap_page.c:132-169`) into the two stages it
was standing in for:

- `int scrap_page_fill_holes(scrap_page_t *p, int fd);` — Stage-1. `pread`s only the ranges
  *not* covered by the header entries, 4096-aligned, into the data zone. After it succeeds
  the page is hole-free and safe to write whole.
- `int scrap_page_writeback(scrap_page_t *p, int fd);` — Stage-2. `pwrite` of the full 256 KB
  data zone at `p->base`. Alignment holds by construction (base is a multiple of 256 KB;
  length is 256 KB; the data zone came from `posix_memalign`).

`scrap_page_flush()` itself is deleted once `nox_close` no longer needs it.

### 5.5 `src/noxdb.c` changes

`scrap_write_chunk()` (`:54-106`) rewritten:

- New partial page → `otflush_enqueue_partial()` **immediately** (paper §3.4: "whenever an
  unfilled page is generated, the scrap buffer inserts it to Queue-1"). It stays in the index
  and keeps absorbing writes.
- Page becomes full → `otflush_enqueue_full()`, **skipping Q1** — no holes means no Stage-1
  read at all. This is the asymmetry win (`docs/03` §3).
- Entry overflow (`:77-93`) → mark `NOX_TAG_SEALED` and detach, instead of flushing inline.
  The page is already in Q1 and drains from there; the foreground allocates a fresh page for
  the same base and retries the merge.

`nox_close()` becomes **drain-then-join**: stop accepting work → sweep the index and enqueue
every still-resident page → `otflush_drain()` → `otflush_stop()` (join) → `close(fd)`. The
guarantee callers have today is preserved: *`nox_close()` returned ⇒ everything has been
issued to the SSD*, which is what `bench/scrap_integrity_test.c`'s read-back `memcmp`
depends on. Note this means "issued via O_DIRECT", **not** "fsync'd" — `nox_fsync()` is C6.

### 5.6 Stage-2 scatter-gather batching

After popping a page, peek the queue head and collect the run of pages with contiguous bases
(`base + 256K`, `base + 512K`, …) that are full and detachable, up to `NOX_PWRITEV_MAX_IOV`
iovecs. One `pwritev` writes them all to a contiguous disk region; non-adjacent pages fall
back to individual `pwrite`. Per `docs/00_flow_summary.md:87` and `docs/02` §2.

### 5.7 New constants (`noxdb_config.h`)

```c
#define NOX_STAGE1_THREADS          1u          /* paper default: one queue-thread pair */
#define NOX_STAGE2_THREADS          1u          /* MUST stay 1 until C5's gen counter */
#define NOX_BCOUNT_BUSY_THRESHOLD   (4u << 20)  /* 4 MB — paper §3.4 evaluation default */
#define NOX_PWRITEV_MAX_IOV         8u          /* 8 x 256KB = 2MB per pwritev */
#define NOX_QUEUE_WARN_DEPTH        4096u       /* soft warn only; never blocks (D5) */
#define NOX_TAG_SEALED              2u
#define NOX_TAG_FLUSHING            3u
```

## 6. Not burning CPU

`NOX_STAGE2_THREADS` is 1, so when Stage-2 requeues a page for busyness there may be nothing
else runnable — Algorithm 2's tail-reinsert would spin. Rules:

- `nox_queue_pop()` blocks on a condvar when the queue is empty. No polling.
- When a pop is followed by a busy-requeue and the queue depth is 1 (the same page comes
  straight back), the thread waits on a condvar with a bounded timeout instead of spinning.
  `Bcount` dropping below the threshold signals that condvar, so the wait normally ends on a
  completion, not on the timeout.
- Shutdown broadcasts on both condvars; `nox_queue_pop()` returns `NULL` so the loops exit.

## 7. Testing

- `bench/otflush_test.c`, driven by `make gate-c4` and `make gate-c4-tsan`.
- **Integrity:** thousands of chaotic small unaligned writes across many bases, against a
  shadow buffer in RAM; after `nox_close()`, re-read the file with raw `O_DIRECT` and
  `memcmp`. Must be byte-identical.
- **Foreground non-blocking:** record per-call `nox_write()` latency; the distribution must
  stay at RAM-copy scale with no SSD-latency tail. This is the C4-GATE criterion
  ("foreground never stalls") and the p99 evidence C10 reuses.
- **Hole-fill correctness:** pre-fill a region on disk with a known pattern, then write a
  sparse partial page over it; after the flush, the untouched byte ranges must still hold the
  original pattern. This is the test that fails loudly if Stage-1 is skipped — writing
  garbage over valid data is the corruption `docs/00_flow_summary.md:79` warns about.
- **TSan:** `make gate-c4-tsan` clean, with the same disjoint-base workload shape as C3 plus
  the background threads.
- **Regression:** `make gate-c3` and `make gate-c3-tsan` must still pass — C4 rewrites
  `page_index_get_or_create`, which C3's gate exercises directly.

## 8. Sequencing

C4 branches off `main` **after C3-GATE passes and merges** (board WIP-1 rule; C3-REVIEW and
C3-GATE are still 📥 and the bench box was down as of 2026-07-23). Nothing in C4 can be
validated before then anyway, since C4-GATE reads "**C3 demonstrated.**"

All gates run on the bare-metal bench box against `/mnt/nvme` — never locally, never on the
OS disk.
