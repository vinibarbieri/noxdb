# NoxDB

**A user-space asynchronous I/O storage engine in C that bypasses the Linux page cache to keep modern NVMe SSDs saturated.**

![status](https://img.shields.io/badge/status-early%20WIP-orange)
![language](https://img.shields.io/badge/language-C-blue)
![platform](https://img.shields.io/badge/platform-Linux%20%C2%B7%20O__DIRECT-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

> **Status: early and in active development.** This is a research/portfolio project built in public. The MVP is write-only and not production-ready. Interfaces and internals change cycle to cycle.
>
> **Follow the build:** I document the engineering process (benchmarks, bugs, and design trade-offs) in biweekly threads on X: **[@ViniBarbieri_11](https://x.com/ViniBarbieri_11)**. See the [Roadmap](#roadmap) below for where it's headed.

---

## The problem

On a modern PCIe NVMe SSD, the Linux **page cache** (the layer meant to make I/O fast) often becomes the ceiling. At millions of IOPS, the OS still funnels every write through the cache on the critical path. Three costs (the *PIO model*) scale **against** you as the drive gets faster:

- **Over-buffering**: CPU burned copying data into the cache instead of exploiting raw sequential bandwidth.
- **Concurrency limits**: kernel-side serialization chokes concurrent writers and starves the SSD's internal parallel channels. The literature names the page cache's XArray lock; on a single file the inode's `i_rwsem` is a second candidate, and **this repository has not yet separated the two**. See [`PERFORMANCE.md`](PERFORMANCE.md) §5 — it needs a profiler, not an assertion.
- **Read-before-write**: a small, unaligned write forces a synchronous full-block read from disk before it can be modified.

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
                    │  → SSD      │      │  → async two-stage flush │
                    │ (bypass     │      │    (OTflush) → SSD       │
                    │  the cache) │      │                          │
                    └─────────────┘      └──────────────────────────┘
```

| | Fast path | Scrap path |
|---|---|---|
| **Trigger** | Large (≥ 1 MB), 4K-aligned writes | Small or unaligned writes |
| **Mechanism** | `O_DIRECT` `pwrite` straight to the SSD | Copied into a 256 KB in-RAM page, ACK'd immediately |
| **Flush** | Synchronous, cache-bypassed | Background **OTflush**: stage-1 fills holes via aligned `pread`, stage-2 drains full pages via `pwrite`/`pwritev` |
| **Goal** | Saturate sequential bandwidth, zero CPU copy | Move read-before-write off the critical path; keep the SSD queue deep |

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
├── docs/                  # architecture, POSIX constraints, PIO theory, design notes
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

- [x] **C0** · `O_DIRECT` alignment probe — prove the 4K constraint end-to-end
- [x] **C1** · Fast path — large aligned writes straight to the SSD
- [x] **C2** · Scrap page + offset→page index
- [x] **C3** · Concurrency — sharded index, TSan-clean, scaling gate
- [x] **C4** · OTflush — two-stage async flushing, RAM watermark, 20-min soak
- [ ] **C6** · Read path + `fsync`
- [ ] **C10** · Evaluation against a page-cache baseline: throughput, p99, CPU
- [ ] Thin LSM key-value store (WAL + SSTable) on top of the engine — deferred

**Where the honesty is.** C0–C4 are built and gated, and the device's own
ceiling is measured — but **no baseline exists yet**, so nothing in this
repository is a comparison against the page cache. What is measured today is
the device and the engine's own behaviour. [`PERFORMANCE.md`](PERFORMANCE.md)
§5 states exactly which claims that does and does not support.

## Credit

NoxDB is a **user-space** reimplementation of ideas from the **WSBuffer** paper (Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs,"* USENIX FAST '26). WSBuffer is a Linux **kernel** filesystem module that delegates durability to the filesystem. NoxDB rebuilds its scrap-buffer and opportunistic two-stage flush mechanisms as a **standalone user-space engine over `O_DIRECT`**. The paper is the conceptual seed; the user-space engine and its measurement record are original work.

**Durability is out of scope, deliberately.** There is no WAL, no recovery and no transactions: a crash mid-flush loses whatever had not reached the device. The scope was frozen at the I/O path so the engine could be *measured* rather than left half-built in four directions.

Paper: <https://www.usenix.org/conference/fast26/presentation/zhan>

## License

[MIT](LICENSE) © 2026 Vinicius Barbieri
