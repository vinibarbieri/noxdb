# WSBuffer — Project Flow (study notes)

## Context: the problem (PIO Model)

NVMe SSDs have 2 characteristics that drive the whole design (doc 03):

1. **Read/write asymmetry** — writes are expensive/slow, reads are cheap.
2. **Access concurrency** — multiple internal channels/dies process many I/Os in parallel.

Specific pain of the Linux page cache: a **partial/unaligned write** that misses the cache forces a **synchronous read-before-write** (reads the block from SSD before updating), blocking the user. On top of that, the page cache uses a global lock (XArray) that chokes SSD concurrency.

WSBuffer goal: **never make the user wait on SSD I/O**, and **saturate SSD bandwidth** by exploiting its internal parallelism.

## Core concepts

**RAM ≠ Disk.** Separate address spaces. RAM is volatile and freshly allocated (clean). Disk is persistent and accumulates data over time. The link between them is a variable we keep (`disk_offset`), copied explicitly via `pread` (disk→RAM) and `pwrite` (RAM→disk). It is never "the same address".

**Window.** The disk is sliced into a **fixed aligned grid** of 256KB. A given offset's window is pure arithmetic:
```
window = (offset / 256K) * 256K
```
Ex: offset 500K → window [256K, 512K). A write **lands in** a grid cell; it does not create a window centered on it. There is no table of all windows — it's just the formula.

**Scrap-page.** A RAM buffer bound to **one** window. Two components (doc 01):
- **Header (128B):** bookkeeping metadata. Holds `counter`, `number`, `ssd_id`, `tag`, and **15 entries** (offset+size each). Lives in RAM, does not go to disk in the MVP.
- **Data-zone (256KB):** the actual data. Allocated separately with `posix_memalign(..., 4096, 256*1024)` to satisfy O_DIRECT.

**Segment vs Hole.**
- **Segment** = a **filled** range of the data-zone (becomes one of the 15 entries). Adjacent ranges *merge* into a single entry.
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
   does the index have a scrap-page for this window?
    │                      │
   YES                    NO
    │                      │
  use it              allocate new (posix_memalign 256K)
        │
   copy data into data-zone (relative offset = offset - window)
   header records/merges the segment in the entries
   IMMEDIATE ACK to user  ◄── user leaves here, does not wait on SSD
```
If a write crosses a 256K boundary, it is **split** across 2 pages (neighboring windows).

### 3. Flush triggers
A page is enqueued for flush when:
1. **256KB full** (capacity).
2. **15 entries used** (segments exhausted, even with little data).
3. **Memory pressure** (force-flush old/full pages to free RAM).

## OTflush — two-stage flushing (background, pthreads)

Runs on separate threads. The user never blocks.

**Stage-1 (READS) — fill holes:**
- Only needed if the page has holes.
- Reads from disk (`pread`, 4096-aligned) the real content of the hole ranges → copies it into the hole positions of the buffer.
- **Why:** a hole means "I don't want to change this part". Without this, the flush would write garbage over valid data already on disk = corruption.
- A **full page (no holes)** skips Stage-1 → direct flush. This is the ideal case (fewer reads = faster, aligns with the asymmetry).

**Stage-2 (WRITES) — drain:**
- Writes the 256KB data-zone to the window's `disk_offset` (`pwrite`).
- Alignment guaranteed by construction: `disk_offset` = n×256K (multiple of 4096), size = 256K (multiple of 4096). O_DIRECT is happy.
- After completion, **frees the RAM** and removes the page from the index.

**Batching (scatter-gather):** if there are full pages for **adjacent windows** (e.g. [0,256K), [256K,512K)), use `pwritev` with an array of `iovec` — each `iovec` points to a different data-zone in RAM → **one syscall** writes them all to a contiguous disk region. Pages for non-adjacent windows require separate `pwrite` calls.

## What is being optimized

**Latency (for the user):** the write goes to RAM and gets an immediate ACK. Read-before-write was moved off the critical path (it became background Stage-1). The user sees RAM-copy latency, not SSD I/O latency.

**Throughput (for the SSD):** the key is **keeping the SSD queue deep**. Background threads push many concurrent I/Os → the SSD spreads them across its internal channels → aggregate bandwidth near peak. A shallow queue (synchronous 1-at-a-time I/O) = idle channels = wasted bandwidth.

Parallelism is **across pages (pipeline)**, not within one:
```
page A:  [Stage-1][Stage-2]
page B:       [Stage-1][Stage-2]   ← B reads while A writes
page C:            [Stage-1][Stage-2]
```
Within a single page, read→write is sequential (mandatory). Across pages, everything overlaps.

Extra gains: `pwritev` cuts the number of syscalls; O_DIRECT bypasses the page cache and eliminates XArray lock contention.

**"Opportunistic" (OTflush)** = it exploits the SSD's internal concurrency by running the flush in the background overlapped with the foreground. It is **not** waiting for the SSD to be idle — there is no bandwidth monitoring in the MVP. It fires on the 3 triggers and drains the queue as soon as possible. Adaptive throttling based on SSD latency would be future work.

## POSIX rules that constrain the design (doc 02)

- **O_DIRECT:** buffer, offset, and length all multiples of 4096. Violation → `EINVAL` → print "O_DIRECT alignment violation".
- **Thread safety:** use `pread`/`pwrite`/`preadv`/`pwritev` (explicit offset), never `read`/`write`+`lseek` (race on the shared global offset).
- **Concurrency:** fine-grained **per-scrap-page** locks, never a global lock — so as not to choke SSD parallelism.

## Responsibilities (who does what)

| Layer | Responsibility |
|---|---|
| Application (above) | Chooses **which offset** each piece of data occupies. Ensures distinct data does not collide. |
| WSBuffer engine | Writes faithfully to the requested offset. It is "dumb and obedient": it does not choose addresses, does not guess content, only optimizes **how** to write. |

The offset **is** the identity of the data. Writing B to an offset where A used to be is an intentional overwrite, ordered by the application. Accidental collision of distinct data = allocation bug in the layer above, out of the engine's scope.
