# NoxDB MVP — Implementation Reference

## What Was Implemented

NoxDB MVP storage engine. C11, write-only. Routes user writes two ways:

- **Big aligned writes** → straight to SSD via `O_DIRECT` (kernel bypass)
- **Small/unaligned writes** → absorbed into RAM 256 KB scrap pages, flushed to SSD when full

Goal ([docs/03](03_pio_theory.md)): dodge OS page-cache lock contention + the read-before-write penalty that kills SSD bandwidth.

### File Map

| File | Role |
|---|---|
| `noxdb_config.h` | Magic numbers from docs (4096 align, 1 MB threshold, 128 B header, 256 KB zone, 15 entries) |
| `noxdb.h` / `noxdb.c` | Public API + the router |
| `scrap_page.{h,c}` | The 256 KB page struct, segment merge, flush |
| `page_index.{h,c}` | Hash table: file offset → live page |
| `io_direct.{h,c}` | `pwrite`/`pread` `O_DIRECT` layer |
| `bench/benchmark.c` | Test driver |
| `Makefile` | Build + `make deploy` |

---

## How It Works

### 1. The Data Structure — `scrap_page_t`

One scrap page covers one 256 KB-aligned region of the file. Two parts:

**Header (exactly 128 bytes):**

```
counter (4B) | ssd_id (2B) | number (1B) | tag (1B) | entries[15] (120B)
```

Field order matters. `ssd_id` (`uint16`) sits right after `counter` so it lands on an even offset → zero padding → 8 B scalars + 15×8 B entries = 128 B exact. `_Static_assert` enforces this at compile time (already verified passing).

`entries[]` track which byte ranges inside the zone hold valid user data — `{offset, size}` pairs. The header is partial-write metadata: unlike a normal page cache where pages are always full, scrap pages hold scattered fragments.

**Data zone (256 KB):** allocated separately via `posix_memalign(4096, 256KB)`. Critical ([docs/01 §2](01_architecture_noxdb.md)) — a single `malloc` of header+data would not guarantee the 4 K alignment `O_DIRECT` needs. See `scrap_page.c:21`.

---

### 2. The Router — `nox_write` (`noxdb.c`)

```c
if (size >= 1MB && size % 4096 == 0 && offset % 4096 == 0)
    pwrite directly to SSD;      /* fast path, bypass buffer */
else
    split across 256KB boundaries → scrap path;
```

**Fast path** streams large aligned writes straight through `O_DIRECT`, no buffering — removes XArray lock contention and lets the SSD run its internal channels in parallel.

**Scrap path:** a write may straddle two pages, so it's chopped at 256 KB boundaries. Each chunk → `scrap_write_chunk`.

---

### 3. Scrap Merge — `scrap_page_merge` + `coalesce_insert`

For each chunk:

1. Find-or-create the page in the hash index (key = offset rounded down to 256 KB)
2. `memcpy` user bytes into the data zone at the intra-page offset
3. Insert `[offset, size)` into the entry list, coalescing overlapping/adjacent ranges so entries stay disjoint and sorted; recompute `counter` (sum of valid bytes) and `number`

**Overflow guard:** only 15 entries fit. If a new disjoint segment would be the 16th → `SCRAP_OVERFLOW`. Router then flushes the page, drops it, recreates empty, retries (single segment always fits). MVP simplification of real OTflush eviction.

Merge probes overflow on a **copy** of the header first, so a rejected merge leaves the page untouched for clean flush+retry (`scrap_page.c:124`).

---

### 4. Flush — `scrap_page_flush`

| Page state | Flush behavior |
|---|---|
| **Full** (`counter == 256 KB`) | Every byte valid → `pwrite` the whole zone. No read needed. |
| **Partial** (shutdown / overflow) | Read-before-write: `pread` the 256 KB region from SSD into aligned scratch (holes keep on-disk contents), overlay valid segments, `pwrite` whole region back. Fuses OTflush Stage-1 (read holes) + Stage-2 (write). |

---

### 5. I/O Layer — `io_direct.c`

- Always `pwrite`/`pread` (never `write` + `lseek`) — thread-safe against shared file offset ([docs/02 §2](02_posix_constraints.md))
- **Bounce buffer:** if a user buffer isn't 4 K-aligned, copy through a `posix_memalign`'d temp — `O_DIRECT` requires the source address aligned too
- On `EINVAL`, prints loud `"O_DIRECT alignment violation"` with the offending offset/len/addr (`CLAUDE.md §2`)

---

### 6. Lifecycle

```
nox_open  → O_DIRECT fd + hash index
nox_write → routes (fast path or scrap path)
nox_close → flushes all remaining partial pages, frees everything, closes fd
```

Full pages are flushed inline during writes; `nox_close` only handles leftover partials.

---

## How to Test

> **Cannot run locally** — `O_DIRECT` is unsupported on macOS/tmpfs. Must run on the Proxmox Linux VM with NVMe SSD on XFS/EXT4. Local check so far is syntax-only (compiled with a `-DO_DIRECT=0` shim); the 128 B header assert passed.

### Steps

**1. Deploy sources to the VM** (edit host/path):
```sh
make deploy REMOTE=you@vm-ip REMOTE_DIR=~/noxdb
```

**2. On the VM, compile and run against an NVMe-backed file:**
```sh
make
./bench/benchmark /mnt/nvme/nox_test.dat
```

**3. Expected output** — two throughput lines and `OK`:
```
fast path (aligned)       268.4 MB in  X.XXX s  =>  ... MB/s
scrap path (unaligned)     77.7 MB in  X.XXX s  =>  ... MB/s
OK
```

**4. What to confirm:**

- No `"O_DIRECT alignment violation"` printed under normal aligned ops (if it appears, an alignment invariant broke)
- `nox_close` returns `0` (all flushes succeeded)
- Data integrity — read the file back and check written bytes survived:
  ```sh
  xxd /mnt/nvme/nox_test.dat | head   # 0xAB region = fast-path data
  ```
- Compile-time: build fails if `scrap_header_t` ever drifts off 128 B (the `_Static_assert`)

---

## Benchmark Notes

The benchmark deliberately mixes:
- **4 MB aligned writes** → exercises the fast path
- **100k small 777-byte unaligned writes** → exercises scrap merge, paging, overflow eviction, read-before-write flush

> **Caveat:** the small-write loop uses stride 333 over size 777, so segments overlap heavily and pages mostly fill via coalescing — good stress for merge, but it's a synthetic pattern. Tune `SMALL_WRITE_SIZE`/stride in `benchmark.c` if you want sparser pages that exercise the partial read-before-write flush path harder.
