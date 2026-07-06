# NoxDB

**A user-space asynchronous I/O storage engine in C that bypasses the Linux page cache to keep modern NVMe SSDs saturated.**

![status](https://img.shields.io/badge/status-early%20WIP-orange)
![language](https://img.shields.io/badge/language-C-blue)
![platform](https://img.shields.io/badge/platform-Linux%20%C2%B7%20O__DIRECT-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

> **Status: early and in active development.** This is a research/portfolio project built in public. The MVP is write-only and not production-ready. Interfaces and internals change cycle to cycle.
>
> **Follow the build:** I document the engineering process (benchmarks, bugs, and design trade-offs) in weekly threads on X: **[@ViniBarbieri_11](https://x.com/ViniBarbieri_11)**. See the [Roadmap](#roadmap) below for where it's headed.

---

## The problem

On a modern PCIe NVMe SSD, the Linux **page cache** (the layer meant to make I/O fast) often becomes the ceiling. At millions of IOPS, the OS still funnels every write through the cache on the critical path. Three costs (the *PIO model*) scale **against** you as the drive gets faster:

- **Over-buffering**: CPU burned copying data into the cache instead of exploiting raw sequential bandwidth.
- **Concurrency limits**: a global kernel lock (XArray) chokes concurrent writers and starves the SSD's internal parallel channels.
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

The end goal is to serve as the storage backend for a **thin LSM key-value store**: an LSM produces exactly two write shapes, tiny WAL appends and large SSTable dumps, which map 1:1 onto the two paths.

## Repository layout

```
noxdb/
├── include/
│   └── noxdb.h            # public API (nox_open / nox_write / nox_close)
├── src/
│   ├── noxdb.c            # engine + write router
│   ├── io_direct.{c,h}    # O_DIRECT pread/pwrite primitives + EINVAL guard
│   ├── scrap_page.{c,h}   # 256KB user-space buffer page (128B header + data zone)
│   ├── page_index.{c,h}   # offset → active scrap-page map
│   └── noxdb_config.h     # compile-time constants (block size, thresholds)
├── bench/                 # benchmark harness + standalone O_DIRECT probe
├── docs/                  # architecture, POSIX constraints, PIO theory, design notes
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

- [x] **C0**: `O_DIRECT` alignment probe (prove the 4K constraint end-to-end)
- [ ] **C1**: Fast path, large aligned writes straight to the SSD *(in progress)*
- [ ] Scrap buffer + OTflush two-stage asynchronous flushing
- [ ] Thin LSM key-value store (WAL + SSTable) on top of the engine
- [ ] Evaluation vs. the page cache: throughput, p99 latency, CPU

## Credit

NoxDB is a **user-space** reimplementation of ideas from the **WSBuffer** paper (Zhan et al., *"Rearchitecting Buffered I/O in the Era of High-Bandwidth SSDs,"* USENIX FAST '26). WSBuffer is a Linux **kernel** filesystem module that delegates durability to the filesystem. NoxDB rebuilds its scrap-buffer and opportunistic two-stage flush mechanisms as a **standalone user-space engine over `O_DIRECT`**, with its own WAL-based recovery, as the backend of a thin LSM store. The paper is the conceptual seed; the user-space engine, the explicit LSM integration, and the recovery path are original work.

Paper: <https://www.usenix.org/conference/fast26/presentation/zhan>

## License

[MIT](LICENSE) © 2026 Vinicius Barbieri
