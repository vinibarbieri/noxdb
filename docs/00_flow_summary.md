# NoxDB — Project Flow (study notes)

## Context: the device model (PIO) and the problem (WSBuffer)

Two device properties frame the design. They are the two parameters of the **PIO model** (Papon & Athanassoulis, DaMoN '21; doc 03):

1. **Read/write asymmetry (α)** — writes cost more than reads.
2. **Access concurrency (k)** — multiple internal channels/dies process many I/Os in parallel.

The page-cache problem itself comes from **WSBuffer** (Zhan et al., FAST '26, §2.3), not from PIO. Specific pain: a **partial/unaligned write** that misses the cache triggers a page fault and a **slow SSD read to fill the page before updating it** (read-before-write), blocking the user. On top of that, page-management updates contend on the XArray's non-scalable spinlock (`xa_lock`), which limits concurrent page updates under heavy writes.

NoxDB goal: **acknowledge small writes without waiting on SSD I/O** (a writer blocks only when the RAM watermark is throttling it), and **skip page-cache management** for large aligned writes. Using the SSD's internal parallelism is not something the engine does yet (see *What is being optimized*).

## Core concepts

**RAM ≠ Disk.** Separate address spaces. RAM is volatile and freshly allocated (clean). Disk is persistent and accumulates data over time. The link between them is a variable we keep (`disk_offset`), copied explicitly via `pread` (disk→RAM) and `pwrite` (RAM→disk). It is never "the same address".

**Window.** The disk is sliced into a **fixed aligned grid** of 256KB. A given offset's window is pure arithmetic:
```
window = (offset / 256K) * 256K
```
Ex: offset 500K → window [256K, 512K). A write **lands in** a grid cell; it does not create a window centered on it. There is no table of all windows — it's just the formula.

**Scrap-page.** A RAM buffer bound to **one** window. Two components (doc 01):
- **Header (520B):** bookkeeping metadata. Holds `counter`, `number`, `ssd_id`, `tag`, and **64 entries** (offset+size each). Lives in RAM, does not go to disk in the MVP. Size is derived (`8 + NOX_MAX_ENTRIES * 8`); WSBuffer's default is 15 entries / 128B, and `docs/01 §2.1` records why this engine sets 64.
- **Data-zone (256KB):** the actual data. Allocated separately with `posix_memalign(..., 4096, 256*1024)` to satisfy O_DIRECT.

**Segment vs Hole.**
- **Segment** = a **filled** range of the data-zone (becomes one of the 64 entries). Adjacent ranges *merge* into a single entry, so sequential writes cost one entry no matter how many they are.
- **Hole** = an **empty** range (no entry). It is untouched space in our own buffer — not another program's data.

**Working set, not full mapping.** Disk may be 500GB, RAM 8GB. Only windows with an **active write right now** get a scrap-page in RAM. An index (hash) maps `disk_offset → scrap_page*` for active ones only. After a flush, the RAM is freed. It behaves like a cache: RAM holds the hot fraction and recycles.

## Write flow

### 1. Routing (the Router, doc 01 §3)
```
write arrives (offset, size, data)
        │
   size >= 1MB AND 4096-aligned?
    │                    │
   YES                  NO
    │                    │
 FAST PATH            SCRAP PATH
 (direct to SSD,      (RAM buffer)
  O_DIRECT,
  bypass cache)
```

### 2. Scrap path (foreground)
```
compute window = (offset/256K)*256K
        │
   RAM watermark: wait if too many pages await flush (no-op otherwise)
        │
   does the index have a scrap-page for this window?
    │                      │
   YES                    NO
    │                      │
  use it              allocate new (posix_memalign 256K)
        │
   copy data into data-zone (relative offset = offset - window)
   header records/merges the segment in the entries
   ACK to user  ◄── returns without waiting on the SSD
```
If a write crosses a 256K boundary, it is **split** across 2 pages (neighboring windows).

### 3. Flush triggers
A page is enqueued for flush when:
1. **256KB full** (capacity).
2. **64 entries used** (segments exhausted, even with little data).
3. **Memory pressure** (force-flush old/full pages to free RAM).

## OTflush — two-stage flushing (background, pthreads)

Runs on separate threads, so the user does not wait on the SSD. A writer can still block at the RAM watermark when too many scrap pages are waiting to be flushed.

**Stage-1 (READS) — fill holes:**
- Only needed if the page has holes.
- Reads from disk (`pread`, 4096-aligned) the real content of the hole ranges → copies it into the hole positions of the buffer.
- **Why:** a hole means "I don't want to change this part". Without this, the flush would write garbage over valid data already on disk = corruption.
- A **full page (no holes)** skips Stage-1 → direct flush. This is the ideal case: no holes means no read-before-write, so the page is written back without a single `pread`.

**Stage-2 (WRITES) — drain:**
- Writes the 256KB data-zone to the window's `disk_offset` (`pwrite`).
- Alignment guaranteed by construction: `disk_offset` = n×256K (multiple of 4096), size = 256K (multiple of 4096). O_DIRECT is happy.
- After completion, **frees the RAM** and removes the page from the index.

**Batching (scatter-gather):** if there are full pages for **adjacent windows** (e.g. [0,256K), [256K,512K)), use `pwritev` with an array of `iovec` — each `iovec` points to a different data-zone in RAM → **one syscall** writes them all to a contiguous disk region. Pages for non-adjacent windows require separate `pwrite` calls.

## What is being optimized

**Latency (for the user):** the write goes to RAM and is acknowledged without waiting on the device, except when the RAM watermark is throttling writers. Read-before-write was moved off the critical path (it became background Stage-1). Normally the user sees RAM-copy latency, not SSD I/O latency; under backpressure a writer waits for the flush to catch up (the slowest write in the 20-minute soak took 9.77 s, `PERFORMANCE.md` §4.3).

**Throughput (for the SSD):** today the engine does **not** keep the SSD queue deep. OTflush takes device I/O off the foreground path, but each stage runs on one thread issuing blocking `pread`/`pwrite`/`pwritev`, so the device sees little concurrent I/O: a 300 s soak measured a median `aqu-sz` of 1.58 (`PERFORMANCE.md` §4.6). On the bench SSD that costs little, because its bandwidth stops scaling past QD2 (`docs/03` §2.1). A device with more write concurrency would need several Stage-2 flushers, and that depends on fixing write ordering first (`docs/03` §3).

Parallelism is **across pages (pipeline)**, not within one:
```
page A:  [Stage-1][Stage-2]
page B:       [Stage-1][Stage-2]   ← B reads while A writes
page C:            [Stage-1][Stage-2]
```
Within a single page, read→write is sequential (mandatory). Across pages the stages overlap, but with one thread per stage today that means at most one Stage-1 read alongside one Stage-2 write.

Extra gains: `pwritev` cuts the number of syscalls when adjacent full pages can be batched; O_DIRECT bypasses the page cache, so the engine's own writes do not pay page-cache management (`docs/03` §3).

**"Opportunistic" (OTflush)** = the flush runs in the background, overlapped with the foreground, instead of on the write path. It does **not** wait for the SSD to be idle. The paper's busy check is implemented (`Bcount`, bytes of I/O in flight, against a 4 MB threshold), but with one thread per stage in-flight I/O peaks at 2.25 MB, so the check cannot fire today (`src/noxdb_config.h`). It fires on the 3 triggers and drains the queue as soon as possible. Adaptive throttling based on SSD latency would be future work.

## POSIX rules that constrain the design (doc 02)

- **O_DIRECT:** buffer, offset, and length all multiples of 4096. Violation → `EINVAL` → print "O_DIRECT alignment violation".
- **Thread safety:** use `pread`/`pwrite`/`preadv`/`pwritev` (explicit offset), never `read`/`write`+`lseek` (race on the shared global offset).
- **Concurrency:** fine-grained **per-scrap-page** locks, never a global lock — so as not to choke SSD parallelism.

## Responsibilities (who does what)

| Layer | Responsibility |
|---|---|
| Application (above) | Chooses **which offset** each piece of data occupies. Ensures distinct data does not collide. |
| NoxDB engine | Writes faithfully to the requested offset. It is "dumb and obedient": it does not choose addresses, does not guess content, only optimizes **how** to write. |

The offset **is** the identity of the data. Writing B to an offset where A used to be is an intentional overwrite, ordered by the application. Accidental collision of distinct data = allocation bug in the layer above, out of the engine's scope.
