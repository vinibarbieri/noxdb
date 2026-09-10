# NoxDB

**A user-space storage engine in C that routes writes around the Linux page cache on NVMe SSDs: large aligned writes go straight to the device with `O_DIRECT`; small writes are buffered in user-space pages and written back in the background with blocking I/O from a single Stage-2 thread.**

![status](https://img.shields.io/badge/status-early%20WIP-orange)
![language](https://img.shields.io/badge/language-C-blue)
![platform](https://img.shields.io/badge/platform-Linux%20%C2%B7%20O__DIRECT-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

> **Status: early and in active development.** This is a learning project: I am building it to understand how a storage engine meets a modern SSD, and how to measure what it does. The MVP is write-only and not production-ready. Interfaces and internals change cycle to cycle.

---

## Start here

**Read first**

1. [`docs/03_wsbuffer_problem.md`](docs/03_wsbuffer_problem.md): the problem (WSBuffer), the device model (PIO), and what each NoxDB mechanism is meant to answer. That mapping is design intent; no baseline exists yet to measure it against.
2. [`PERFORMANCE.md`](PERFORMANCE.md) §1 and §3: the six methodology rules, and the device ceiling every engine number has to be read against. The summary at the top of that file says what the engine numbers show and what they do not.

Then, for the design and the code: [`docs/01_architecture_noxdb.md`](docs/01_architecture_noxdb.md), [`docs/02_posix_constraints.md`](docs/02_posix_constraints.md), [`include/noxdb.h`](include/noxdb.h), and `nox_write` in [`src/noxdb.c`](src/noxdb.c).

**The queue-depth graph** is [`docs/figures/ceiling-4k-randwrite.svg`](docs/figures/ceiling-4k-randwrite.svg): 4 KiB random-write throughput and p99 against queue depth on the bench SSD (`PERFORMANCE.md` §3.1). It is drawn from the per-repetition data in [`docs/figures/ceiling-4k-randwrite.csv`](docs/figures/ceiling-4k-randwrite.csv) by `./tools/plot_ceiling.py`.

**Run the main gates.** Linux only. Each path must be on an `O_DIRECT`-capable filesystem (XFS or ext4), and the file there is overwritten.

```sh
# RAM-only unit tests: MPMC queue, Stage-1 hole filling, RAM watermark
make test-queue test-holes test-watermark

# C2: single-thread scrap-path integrity (write, flush, read back, memcmp)
make gate && ./bench/scrap_integrity_test /mnt/nvme/c2gate.dat

# C3: concurrent scrap-path integrity on disjoint regions
make gate-c3 && ./bench/concurrency_test /mnt/nvme/c3gate.dat 8

# C4: background flushing; all data lands and foreground p99.9 stays under the 200 µs stall budget
make gate-c4 && ./bench/otflush_test /mnt/nvme/c4gate.dat 8

# C4 soak: 20 minutes, 16 threads on shared regions (SOAK_SECONDS=30 for a smoke run)
make soak-c4
```

ThreadSanitizer builds exist for the concurrent pieces (`gate-c3-tsan`, `gate-c4-tsan`, `test-queue-tsan`, `test-watermark-tsan`); on the bench box they run under `setarch -R`. The comment above each target in the [`Makefile`](Makefile) has the details.

---

## The problem

On a modern PCIe NVMe SSD, the Linux **page cache** (the layer meant to make I/O fast) often becomes the ceiling: buffered I/O funnels every write through the cache on the critical path. The WSBuffer paper (Zhan et al., FAST '26, §2.3–2.4) measures this on high-bandwidth SSDs and names three challenges:

- **Over-buffering**: page caching is overused to buffer *all* incoming writes, so buffered I/O reaches lower write bandwidth than direct I/O on the same device.
- **Page-management contention**: under heavy writes, page insertions, deletions and state updates contend on the XArray's non-scalable spinlock (`xa_lock`), degrading both foreground writes and background flushing. WSBuffer measured this with one file per writer thread, deliberately keeping file-level lock contention out. NoxDB writes a single file, where the inode's `i_rwsem` is a second candidate, and **this repository has not yet separated the two**. See [`PERFORMANCE.md`](PERFORMANCE.md) §5 — it needs a profiler, not an assertion.
- **Read-before-write**: a partial-page write that misses the cache triggers a page fault and a slow SSD read to fill the page before it can be updated.

The paper labels these C1–C3; this repository does not reuse those labels, because C1–C4 name its build cycles (see the [Roadmap](#roadmap)).

## The approach

NoxDB **routes** writes instead of caching them. A thin router inspects each write and sends it down one of two paths:

```
                          nox_write(buf, size, offset)
                                      │
                       size ≥ 1MB and 4K-aligned?
                          │                     │
                         YES                    NO
                          │                     │
                    ┌─────────────┐      ┌──────────────────────────┐
                    │  FAST PATH  │      │        SCRAP PATH        │
                    │  O_DIRECT   │      │  256KB user-space pages  │
                    │  → SSD      │      │  → background flush      │
                    │ (bypass     │      │    (OTflush) → SSD       │
                    │  the cache) │      │                          │
                    └─────────────┘      └──────────────────────────┘
```

| | Fast path | Scrap path |
|---|---|---|
| **Trigger** | Large (≥ 1 MB), 4K-aligned writes | Small or unaligned writes |
| **Mechanism** | `O_DIRECT` `pwrite` straight to the SSD (through an aligned bounce buffer if the caller's buffer is not 4K-aligned) | Copied into a 256 KB in-RAM page and acknowledged without waiting on the device, unless the RAM watermark is throttling writers |
| **Flush** | Synchronous, cache-bypassed | Background **OTflush**: stage-1 fills holes via aligned `pread`, stage-2 drains full pages via `pwrite`/`pwritev`<br>*today: blocking I/O, one Stage-2 thread* |
| **Goal** | Skip page-cache management for large aligned writes ([`docs/03`](docs/03_wsbuffer_problem.md) §3) | Take read-before-write off the foreground path, and skip it entirely when a page fills ([`docs/03`](docs/03_wsbuffer_problem.md) §3) |

The two paths were chosen with a **thin LSM key-value store** in mind: an LSM produces exactly two write shapes, tiny WAL appends and large SSTable dumps, which map 1:1 onto them. That is the design argument for the router, and it is **deferred, unbuilt and therefore unmeasured** — see the [Roadmap](#roadmap).

## Repository layout

```
noxdb/
├── include/
│   └── noxdb.h            # public API (nox_open / nox_write / nox_close)
├── src/
│   ├── noxdb.c            # engine + write router
│   ├── io_direct.{c,h}    # O_DIRECT pread/pwrite primitives + EINVAL guard
│   ├── scrap_page.{c,h}   # 256KB user-space buffer page (520B header + data zone)
│   ├── page_index.{c,h}   # offset → active scrap-page map, 64 sharded buckets
│   ├── otflush.{c,h}      # two-stage background flusher (stage-1 reads, stage-2 writes)
│   ├── queue.{c,h}        # MPMC queue connecting the foreground to the stages
│   ├── watermark.{c,h}    # RAM ceiling: throttles writers at N live scrap pages
│   ├── nox_stats.{c,h}    # entry-exhaustion + write-amplification counters
│   └── noxdb_config.h     # compile-time constants (block size, thresholds)
├── bench/                 # gates, soaks, repros, and the standalone O_DIRECT probe
├── tools/                 # measurement harness: hygiene, fio sweep, report, overnight
├── docs/                  # architecture, POSIX constraints, problem statement + PIO, design notes
├── PERFORMANCE.md         # the measurement record
├── Makefile
└── LICENSE
```

## Public API

```c
#include "noxdb.h"

nox_engine_t *e = nox_open("/mnt/nvme/store.dat");   // O_DIRECT-backed file

nox_write(e, buf, size, offset);                     // routed automatically

nox_close(e);                                        // flush remaining pages + close
```

## Build & run

> **Linux only.** `O_DIRECT` is a Linux feature and requires an `O_DIRECT`-capable filesystem (XFS/EXT4). The code will not run meaningfully on macOS or over a network FS.

```sh
make          # build the benchmark harness
make probe    # build the standalone O_DIRECT alignment probe
make clean
```

Benchmarks are run on a dedicated bare-metal box against a clean NVMe SSD mounted at `/mnt/nvme`, so hypervisor jitter and a shared page cache don't poison the tail-latency and CPU numbers. See [`docs/00_project.md`](docs/00_project.md) for the full measurement setup.

## Roadmap

- [x] **C0** · `O_DIRECT` alignment probe — the kernel enforces the device's logical block size (512 on the bench box); NoxDB uses 4096 as the portable superset: 4096-aligned I/O is also 512-aligned, and it is valid on 4Kn drives ([`docs/02`](docs/02_posix_constraints.md) §1)
- [x] **C1** · Fast path — large aligned writes straight to the SSD
- [x] **C2** · Scrap page + offset→page index
- [x] **C3** · Concurrency — sharded index, TSan-clean, scaling gate
- [x] **C4** · OTflush — two-stage background flushing, RAM watermark, 20-min soak
- [ ] **C6** · Read path + `fsync`
- [ ] **C10** · Evaluation against a page-cache baseline: throughput, p99, CPU
- [ ] Fix write ordering so Stage-2 can run several flushers, then evaluate asynchronous submission (e.g. `io_uring`) to raise the queue depth the engine puts on the device — see [`docs/03`](docs/03_wsbuffer_problem.md) §3 and [`PERFORMANCE.md`](PERFORMANCE.md) §4.5–4.6
- [ ] Thin LSM key-value store (WAL + SSTable) on top of the engine — deferred

**Where the honesty is.** C0–C4 are built and gated, and the device's own
ceiling is measured — but **no baseline exists yet**, so nothing in this
repository is a comparison against the page cache. What is measured today is
the device and the engine's own behaviour. [`PERFORMANCE.md`](PERFORMANCE.md)
§5 states exactly which claims that does and does not support.

## Credit

NoxDB is a **user-space** reimplementation of ideas from the **WSBuffer** paper (Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs,"* USENIX FAST '26). WSBuffer is a Linux **kernel** filesystem module that delegates durability to the filesystem. NoxDB rebuilds its scrap-buffer and opportunistic two-stage flush mechanisms as a **standalone user-space engine over `O_DIRECT`**. The paper is the conceptual seed; the user-space engine and its measurement record are original work.

Implementation written with heavy help from Claude Code; the design study, code review and measurement design are mine.

**Durability is out of scope, deliberately.** There is no WAL, no recovery and no transactions: a crash mid-flush loses whatever had not reached the device. The scope was frozen at the I/O path so the engine could be *measured* rather than left half-built in four directions.

Paper: <https://www.usenix.org/conference/fast26/presentation/zhan>

The two device properties the design reasons with, **read/write asymmetry (α)** and **access concurrency (k)**, come from the **Parametric I/O (PIO) model** (Papon & Athanassoulis, *"A Parametric I/O Model for Modern Storage Devices,"* DaMoN '21). WSBuffer does not cite PIO; linking the two is this repository's framing. NoxDB uses α and k as vocabulary. It does not implement the model, and it has not measured either parameter by the paper's methodology.

Paper: <https://doi.org/10.1145/3465998.3466003>

## Follow along

I write up the engineering process (benchmarks, bugs, and design trade-offs) in weekly threads on X: [@ViniBarbieri_11](https://x.com/ViniBarbieri_11).

## License

[MIT](LICENSE) © 2026 Vinicius Barbieri
