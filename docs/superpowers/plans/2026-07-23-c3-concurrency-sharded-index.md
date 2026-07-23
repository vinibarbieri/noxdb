# C3 — Concurrency + Sharded Index Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the NoxDB scrap write path race-free and scalable under N concurrent writer threads by replacing the unlocked page index with a sharded-lock index.

**Architecture:** The page index (offset → resident scrap page) gets partitioned into 64 cache-line-padded shard mutexes; a page maps to a shard by its hash, so writers on disjoint pages never serialize on the index. Per-page mutexes (already present) guard page contents. The shard lock and a page lock are never held simultaneously, so there is no lock-order inversion.

**Tech Stack:** C11, POSIX threads (`pthread_mutex_t`), `O_DIRECT` I/O, ThreadSanitizer (`-fsanitize=thread`).

## Global Constraints

- **Language:** C11. Explicit widths via `stdint.h`.
- **Alignment:** all O_DIRECT buffers 4096-aligned via `posix_memalign`; `malloc` forbidden for the data zone.
- **Thread safety:** `pread`/`pwrite` only (never `read`/`write`+`lseek`).
- **Magic numbers:** only from `docs/`. `NOX_INDEX_SHARDS=64` is an explicit engineering choice recorded in `docs/01` §5 (not a paper mandate).
- **Cannot run locally:** macOS lacks `O_DIRECT`. Every build/run happens on the bare-metal bench box (`ssh noxdb`, `~/noxdb`) against `/mnt/nvme`. Deploy with `make deploy` (rsync of `.c`/`.h`/`Makefile`).
- **Public API (unchanged):** `nox_engine_t *nox_open(const char *path)`, `int nox_write(nox_engine_t *e, const void *buf, size_t size, uint64_t offset)`, `int nox_close(nox_engine_t *e)`.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `bench/concurrency_test.c` | C3 gate driver: N threads, disjoint regions, shadow + raw read-back, throughput | **create** |
| `Makefile` | `gate-c3` (optimized) + `gate-c3-tsan` (instrumented) targets | modify |
| `src/noxdb_config.h` | `NOX_INDEX_SHARDS`, `NOX_CACHELINE` | modify |
| `src/page_index.c` | sharded locking in create/get_or_create/remove/destroy | modify |
| `src/page_index.h` | header comment: sharded locks | modify |
| `src/noxdb.c` | comment: get-then-lock window + lock-order invariant | modify |
| `src/scrap_page.c` | comment: per-page lock class | modify |

Task order is TDD-driven: **Task 1 writes the gate (fails: current index is unlocked → TSan races). Task 2 implements sharding (passes). Task 3 documents invariants.**

---

## Task 1: C3 gate driver + Makefile targets (the failing test)

**Files:**
- Create: `bench/concurrency_test.c`
- Modify: `Makefile` (add `gate-c3`, `gate-c3-tsan`, clean, .PHONY)

**Interfaces:**
- Consumes: public API `nox_open` / `nox_write` / `nox_close` (`include/noxdb.h`); `NOX_DATAZONE_SIZE`, `NOX_BLOCK_SIZE` (`src/noxdb_config.h`).
- Produces: two runnable binaries — `bench/concurrency_test` (integrity + throughput) and `bench/concurrency_test_tsan` (race detector).

- [ ] **Step 1: Write the gate driver**

Create `bench/concurrency_test.c`:

```c
/*
 * concurrency_test.c - C3 acceptance gate: concurrent scrap-path integrity +
 * race-freedom + throughput scaling.
 *
 * Gate (KANBAN C3-GATE): N threads write disjoint offsets concurrently;
 * TSan-clean; throughput scales with threads.
 *
 * Strategy (multi-threaded sibling of scrap_integrity_test):
 *   - Partition the file into disjoint 256KB regions. Thread t owns a contiguous
 *     block of RPT regions. No two threads share a base, so this stresses the
 *     sharded index + per-page locks, NOT cross-thread page contention.
 *   - Each thread fills its regions with small sequential scrap-path writes
 *     (WLEN bytes) so each 256KB page fills and flushes INLINE, in parallel with
 *     the other threads -> real concurrent SSD writes -> throughput scales.
 *   - PASSES rewrites give a measurable, tunable amount of work.
 *   - A per-thread shadow holds the expected final region contents.
 *   - After join + nox_close, re-read every region with a SEPARATE raw O_DIRECT
 *     fd (never verify the engine with the engine) and memcmp vs shadow.
 *   - Print aggregate bandwidth (MB/s). Run at 1/2/4/8 threads to see scaling.
 *
 * Build + run ON THE BENCH BOX against /mnt/nvme (O_DIRECT only works there):
 *   make gate-c3      && ./bench/concurrency_test      /mnt/nvme/c3gate.dat 8
 *   make gate-c3-tsan && ./bench/concurrency_test_tsan /mnt/nvme/c3gate.dat 8
 */
#define _GNU_SOURCE
#include "noxdb.h"
#include "noxdb_config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REG    NOX_DATAZONE_SIZE   /* 256KB region == one scrap page */
#define RPT    8u                  /* regions per thread */
#define PASSES 32u                 /* rewrites per region (tune for runtime/BW) */
#define WLEN   256u                /* small sequential write (scrap path; REG%WLEN==0) */

static nox_engine_t *g_engine;

typedef struct {
    int      tid;
    uint64_t first_region;   /* index of this thread's first 256KB region */
    uint8_t *shadow;         /* RPT * REG: expected final contents */
    int      err;            /* set to -1 if any nox_write failed */
} worker_t;

/* Deterministic per-region byte pattern so a mismatch is diagnosable. */
static uint8_t pat(int tid, uint64_t region, uint32_t intra)
{
    return (uint8_t)(0x40 + ((tid * 7 + (int)region * 3 + (int)intra) & 0x3f));
}

static void *worker(void *arg)
{
    worker_t *w = arg;

    /* Build this thread's shadow once: the final image is pass-independent. */
    for (uint32_t r = 0; r < RPT; r++) {
        uint64_t region = w->first_region + r;
        uint8_t *shad    = w->shadow + (size_t)r * REG;
        for (uint32_t i = 0; i < REG; i++)
            shad[i] = pat(w->tid, region, i);
    }

    for (uint32_t pass = 0; pass < PASSES; pass++) {
        for (uint32_t r = 0; r < RPT; r++) {
            uint64_t region = w->first_region + r;
            uint64_t rbase  = region * (uint64_t)REG;
            for (uint32_t off = 0; off + WLEN <= REG; off += WLEN) {
                uint8_t buf[WLEN];
                for (uint32_t i = 0; i < WLEN; i++)
                    buf[i] = pat(w->tid, region, off + i);
                /* small write -> scrap path; adjacent writes coalesce, page
                 * fills at REG/WLEN writes -> inline full flush, in parallel. */
                if (nox_write(g_engine, buf, WLEN, rbase + off) != 0)
                    w->err = -1;
            }
        }
    }
    return NULL;
}

/* Raw O_DIRECT read-back vs shadow. Returns mismatch count, or -1 on setup err. */
static int verify(const char *path, worker_t *ws, int nthreads)
{
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) { perror("open (read-back)"); return -1; }

    void *disk = NULL;
    if (posix_memalign(&disk, NOX_BLOCK_SIZE, REG) != 0) {
        fprintf(stderr, "posix_memalign read buffer failed\n");
        close(fd);
        return -1;
    }

    int fails = 0;
    for (int t = 0; t < nthreads; t++) {
        for (uint32_t r = 0; r < RPT; r++) {
            uint64_t region = ws[t].first_region + r;
            ssize_t rd = pread(fd, disk, REG, (off_t)(region * (uint64_t)REG));
            if (rd != (ssize_t)REG) {
                fprintf(stderr, "  region %llu: short/failed read (%zd, errno=%d)\n",
                        (unsigned long long)region, rd, errno);
                fails++;
                continue;
            }
            uint8_t *shad = ws[t].shadow + (size_t)r * REG;
            if (memcmp(disk, shad, REG) != 0) {
                size_t j = 0;
                while (j < REG && ((uint8_t *)disk)[j] == shad[j]) j++;
                fprintf(stderr,
                        "  region %llu: MISMATCH at +%zu (disk=0x%02x shadow=0x%02x)\n",
                        (unsigned long long)region, j,
                        ((uint8_t *)disk)[j], shad[j]);
                fails++;
            }
        }
    }
    free(disk);
    close(fd);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s /mnt/nvme/c3gate.dat <nthreads>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int nthreads = atoi(argv[2]);
    if (nthreads < 1 || nthreads > 256) {
        fprintf(stderr, "nthreads must be 1..256\n");
        return 2;
    }

    unlink(path);   /* fresh file: holes read back as 0 */

    g_engine = nox_open(path);
    if (!g_engine) { perror("nox_open"); return 2; }

    worker_t  *ws = calloc((size_t)nthreads, sizeof(*ws));
    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    if (!ws || !th) { perror("calloc"); return 2; }

    for (int t = 0; t < nthreads; t++) {
        ws[t].tid          = t;
        ws[t].first_region = (uint64_t)t * RPT;
        ws[t].shadow       = malloc((size_t)RPT * REG);
        if (!ws[t].shadow) { perror("malloc shadow"); return 2; }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int t = 0; t < nthreads; t++)
        if (pthread_create(&th[t], NULL, worker, &ws[t]) != 0) {
            perror("pthread_create"); return 2;
        }
    for (int t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    int werr = 0;
    for (int t = 0; t < nthreads; t++)
        if (ws[t].err) werr = 1;
    if (werr)
        fprintf(stderr, "WARNING: a nox_write returned an error during load\n");

    if (nox_close(g_engine) != 0) {
        fprintf(stderr, "nox_close reported a flush error\n");
        werr = 1;
    }

    /* Bandwidth over the parallel phase: every byte written was flushed. */
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double bytes = (double)nthreads * RPT * PASSES * (double)REG;
    printf("threads=%d  time=%.3fs  bandwidth=%.1f MB/s\n",
           nthreads, secs, bytes / 1e6 / secs);

    int fails = verify(path, ws, nthreads);
    if (fails < 0) return 2;

    if (fails == 0 && werr == 0) {
        printf("C3-GATE PASS: %d region(s) match disk (concurrent scrap integrity)\n",
               nthreads * (int)RPT);
        return 0;
    }
    printf("C3-GATE FAIL: %d region mismatch(es), write_err=%d\n", fails, werr);
    return 1;
}
```

- [ ] **Step 2: Add the Makefile targets**

In `Makefile`, after the existing `GATE := bench/scrap_integrity_test` line add:

```make
GATE_C3 := bench/concurrency_test
```

After the existing `$(GATE): ...` gate rule block add:

```make
# C3 acceptance gate: concurrent scrap path. Optimized build (throughput + integrity).
# Run on the bench box: ./bench/concurrency_test /mnt/nvme/c3gate.dat <nthreads>
gate-c3: $(GATE_C3)

$(GATE_C3): $(OBJ) bench/concurrency_test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# C3 race gate: single instrumented compile of all engine sources + the driver,
# so ThreadSanitizer objects never mix with the -O2 objects. Bench box only.
# Run: ./bench/concurrency_test_tsan /mnt/nvme/c3gate.dat <nthreads>
gate-c3-tsan:
	$(CC) $(CFLAGS) -fsanitize=thread -g -o bench/concurrency_test_tsan \
	    $(SRC) bench/concurrency_test.c $(LDFLAGS)
```

Update the `clean` rule to also remove the new binaries:

```make
clean:
	rm -f src/*.o bench/*.o $(BENCH) $(PROBE) $(GATE) $(GATE_C3) bench/concurrency_test_tsan
```

Update `.PHONY`:

```make
.PHONY: all bench probe gate gate-c3 gate-c3-tsan deploy clean
```

- [ ] **Step 3: Deploy and run the TSan gate against the CURRENT (unlocked) index — verify it FAILS**

Run (from the local repo):

```bash
make deploy
ssh noxdb 'cd ~/noxdb && make gate-c3-tsan && ./bench/concurrency_test_tsan /mnt/nvme/c3gate.dat 8'
```

Expected: ThreadSanitizer prints `WARNING: ThreadSanitizer: data race` on `page_index` bucket-list access (and possibly a crash / integrity FAIL), because `page_index` currently has **no locks**. This is the red state Task 2 fixes.

- [ ] **Step 4: Commit the gate**

```bash
git add bench/concurrency_test.c Makefile
git commit -m "test: add C3 concurrency gate (N-thread scrap integrity + TSan + throughput)

Fails against the current unlocked page_index (TSan reports data races on
the bucket lists) - the red state that C3 sharded locking makes green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Sharded page index (make the gate pass)

**Files:**
- Modify: `src/noxdb_config.h` (add constants)
- Modify: `src/page_index.c` (sharded locking)
- Modify: `src/page_index.h:1-6` (header comment)
- Test: `bench/concurrency_test_tsan` + `bench/concurrency_test` on the bench box

**Interfaces:**
- Consumes: `scrap_page_t` + `scrap_page_alloc`/`scrap_page_free` (`src/scrap_page.h`); `NOX_DATAZONE_SIZE`.
- Produces: internally-locked `page_index_create` / `page_index_get_or_create` / `page_index_remove` / `page_index_destroy` (signatures **unchanged** — locking is internal). `page_index_foreach` stays lock-free (shutdown-only).

- [ ] **Step 1: Add the shard constants to `src/noxdb_config.h`**

After the `NOX_MAX_ENTRIES` define (line ~29) add:

```c
/* Page-index shard count: the index's bucket lists are partitioned across this
 * many independent mutexes so concurrent writers on different pages don't
 * serialize on one global lock (the XArray bottleneck; docs/01 §5).
 * ENGINEERING CHOICE, not dictated by the paper: 64 > NVMe queue depth (SN530
 * ~QD32) and > bench box cores, so false contention is negligible; finer (e.g.
 * per-bucket) locking buys nothing yet risks false sharing. MUST be a power of
 * two (masked with NOX_INDEX_SHARDS-1). See docs/01 §5 for full rationale. */
#define NOX_INDEX_SHARDS     64u

/* Cache-line size: shard locks are padded to this so two distinct shard mutexes
 * never share a line (no false sharing when locked from different cores). */
#define NOX_CACHELINE        64u
```

- [ ] **Step 2: Rewrite `src/page_index.c` with sharded locking**

Replace the entire contents of `src/page_index.c` with:

```c
/*
 * page_index.c - Chained hash table of scrap pages with SHARDED bucket locks.
 * See page_index.h. Concurrency model: docs/01 §5.
 */
#define _GNU_SOURCE
#include "page_index.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Power-of-two bucket count so the mask is a cheap AND. */
#define PI_BUCKETS 1024u
#define PI_MASK    (PI_BUCKETS - 1u)

/*
 * Cache-line-padded shard lock. Padding + alignment guarantee two distinct
 * shard mutexes never share a 64B cache line, so locking unrelated shards from
 * different cores generates no false-sharing coherence traffic. (docs/01 §5)
 */
typedef union {
    pthread_mutex_t mtx;
    char            pad[NOX_CACHELINE];
} _Alignas(NOX_CACHELINE) pi_shard_t;

struct page_index {
    pi_shard_t    shards[NOX_INDEX_SHARDS];  /* one mutex per shard */
    scrap_page_t *buckets[PI_BUCKETS];       /* bucket heads (chained) */
};

/* Hash a 256KB-aligned base: page number, Fibonacci-mixed, top 10 bits. */
static inline uint32_t pi_hash(uint64_t base)
{
    uint64_t page_no = base / NOX_DATAZONE_SIZE;
    return (uint32_t)((page_no * 0x9E3779B97F4A7C15ull) >> 54) & PI_MASK;
}

/* Map a bucket index to its shard. Low bits of the Fibonacci-mixed bucket are
 * well distributed, so a plain mask spreads pages evenly across shards. */
static inline uint32_t pi_shard(uint32_t bucket)
{
    return bucket & (NOX_INDEX_SHARDS - 1u);
}

page_index_t *page_index_create(void)
{
    page_index_t *idx = NULL;
    /* posix_memalign (not calloc) so the shards[] array is genuinely cache-line
     * aligned; calloc only promises 16B, which would defeat pi_shard_t padding. */
    if (posix_memalign((void **)&idx, NOX_CACHELINE, sizeof(*idx)) != 0)
        return NULL;
    memset(idx, 0, sizeof(*idx));            /* all bucket heads NULL */

    for (uint32_t s = 0; s < NOX_INDEX_SHARDS; s++) {
        if (pthread_mutex_init(&idx->shards[s].mtx, NULL) != 0) {
            for (uint32_t j = 0; j < s; j++)  /* unwind partial init */
                pthread_mutex_destroy(&idx->shards[j].mtx);
            free(idx);
            return NULL;
        }
    }
    return idx;
}

void page_index_destroy(page_index_t *idx)
{
    if (!idx)
        return;
    for (uint32_t b = 0; b < PI_BUCKETS; b++) {
        scrap_page_t *p = idx->buckets[b];
        while (p) {
            scrap_page_t *next = p->next;
            scrap_page_free(p);
            p = next;
        }
    }
    for (uint32_t s = 0; s < NOX_INDEX_SHARDS; s++)
        pthread_mutex_destroy(&idx->shards[s].mtx);
    free(idx);
}

scrap_page_t *page_index_get_or_create(page_index_t *idx, uint64_t base,
                                       uint16_t ssd_id, int *created)
{
    uint32_t    b  = pi_hash(base);
    pi_shard_t *sh = &idx->shards[pi_shard(b)];

    pthread_mutex_lock(&sh->mtx);            /* guard this shard's bucket lists */

    for (scrap_page_t *p = idx->buckets[b]; p; p = p->next) {
        if (p->base == base) {
            if (created) *created = 0;
            pthread_mutex_unlock(&sh->mtx);
            return p;
        }
    }

    scrap_page_t *p = scrap_page_alloc(base, ssd_id);
    if (!p) {
        pthread_mutex_unlock(&sh->mtx);      /* release before OOM return */
        return NULL;
    }
    p->next = idx->buckets[b];               /* push onto bucket head */
    idx->buckets[b] = p;
    if (created) *created = 1;

    pthread_mutex_unlock(&sh->mtx);
    return p;
}

void page_index_remove(page_index_t *idx, uint64_t base)
{
    uint32_t    b  = pi_hash(base);
    pi_shard_t *sh = &idx->shards[pi_shard(b)];

    pthread_mutex_lock(&sh->mtx);
    scrap_page_t **link = &idx->buckets[b];
    while (*link) {
        if ((*link)->base == base) {
            scrap_page_t *victim = *link;
            *link = victim->next;            /* unlink under the shard lock */
            pthread_mutex_unlock(&sh->mtx);
            scrap_page_free(victim);         /* free outside: victim unreachable now */
            return;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&sh->mtx);
}

void page_index_foreach(page_index_t *idx,
                        void (*fn)(scrap_page_t *p, void *ctx), void *ctx)
{
    /* Lock-free ON PURPOSE: only nox_close calls this, after all writer threads
     * have joined, so there is no concurrent mutation. Locking here would also
     * risk deadlock if `fn` re-entered the index. (docs/01 §5) */
    for (uint32_t b = 0; b < PI_BUCKETS; b++)
        for (scrap_page_t *p = idx->buckets[b]; p; p = p->next)
            fn(p, ctx);
}
```

- [ ] **Step 3: Update the `src/page_index.h` header comment**

Replace lines 1-6 (the top block comment) with:

```c
/*
 * page_index.h - Hash table mapping a 256KB-aligned file offset to its live
 * scrap page. Keyed by NOX_PAGE_BASE(offset). Separate chaining via
 * scrap_page_t.next. Bucket lists are guarded by SHARDED locks (docs/01 §5):
 * get_or_create/remove lock only the target shard, so concurrent writers on
 * different pages don't serialize. Locking is internal; signatures are
 * unchanged. (Index structure not specified by docs; chosen for the MVP because
 * it is sparse and tolerates large/random offsets.)
 */
```

- [ ] **Step 4: Deploy and run the TSan gate — verify it now PASSES (clean)**

```bash
make deploy
ssh noxdb 'cd ~/noxdb && make clean && make gate-c3-tsan && ./bench/concurrency_test_tsan /mnt/nvme/c3gate.dat 8'
```

Expected: **no** `ThreadSanitizer: data race` warnings, and the driver prints:
```
threads=8  time=... bandwidth=... MB/s
C3-GATE PASS: 64 region(s) match disk (concurrent scrap integrity)
```

- [ ] **Step 5: Run the optimized gate + eyeball throughput scaling**

```bash
ssh noxdb 'cd ~/noxdb && make clean && make gate-c3 && \
  for n in 1 2 4 8; do ./bench/concurrency_test /mnt/nvme/c3gate.dat $n; done'
```

Expected: every run ends `C3-GATE PASS`, and the `bandwidth=… MB/s` line **rises** from 1→2→4→8 threads (until the SSD saturates). Record the numbers for the C3-GATE / C10 report.

- [ ] **Step 6: Commit**

```bash
git add src/noxdb_config.h src/page_index.c src/page_index.h
git commit -m "feat: shard the page index with 64 cache-line-padded bucket locks

Replaces the unlocked bucket lists with NOX_INDEX_SHARDS(=64) padded shard
mutexes; get_or_create/remove lock only the target shard. Kills the global
XArray-style bottleneck (docs/01 §5) so disjoint-page writers run parallel.
C3 gate is now TSan-clean and throughput scales with threads.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Document the concurrency invariants

**Files:**
- Modify: `src/noxdb.c:54-96` (comment in `scrap_write_chunk`)
- Modify: `src/scrap_page.c:36` (comment at `pthread_mutex_init`)

**Interfaces:**
- Consumes: nothing new. Produces: nothing new. Comments only — no behavior change.

- [ ] **Step 1: Document the get-then-lock window + lock-order invariant in `scrap_write_chunk`**

In `src/noxdb.c`, replace the body comment directly above `pthread_mutex_lock(&p->lock);` (currently there is none; insert between the `page_index_get_or_create` call block and the lock) so the sequence reads:

```c
    int created;
    scrap_page_t *p = page_index_get_or_create(e->idx, base, e->ssd_id, &created);
    if (!p) {
        errno = ENOMEM;
        return -1;
    }

    /* LOCK ORDER (docs/01 §5): the index shard lock is released INSIDE
     * page_index_get_or_create before we take the per-page lock here, so the two
     * lock classes are never held at once -> no lock-order inversion.
     * GET-THEN-LOCK WINDOW: between the call above returning `p` and this lock,
     * another thread could remove+free the same page. The C3 gate writes DISJOINT
     * offsets (each base owned by one thread) so it cannot fire; the general fix
     * is the C5 tag=FLUSHING pointer-swap. */
    pthread_mutex_lock(&p->lock);
```

- [ ] **Step 2: Document the per-page lock class in `scrap_page.c`**

In `src/scrap_page.c`, replace the `pthread_mutex_init(&p->lock, NULL);` line (line ~36) with:

```c
    /* Per-page lock (docs/01 §5): guards this page's header+data during merge
     * and flush. Distinct from the index shard locks; the two are never held
     * simultaneously (see scrap_write_chunk), so there is no lock-order risk. */
    pthread_mutex_init(&p->lock, NULL);
```

- [ ] **Step 3: Verify it still builds clean on the bench box**

```bash
make deploy
ssh noxdb 'cd ~/noxdb && make clean && make gate-c3 && ./bench/concurrency_test /mnt/nvme/c3gate.dat 4'
```

Expected: compiles with no warnings (`-Wall -Wextra`), ends `C3-GATE PASS`.

- [ ] **Step 4: Commit**

```bash
git add src/noxdb.c src/scrap_page.c
git commit -m "docs: record page/shard lock-order invariant + C3 get-then-lock window

Comments only. Documents that shard and per-page locks are never co-held
(no inversion) and that the get-then-lock window is safe for disjoint bases,
with the general fix deferred to C5 (tag=FLUSHING swap).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Post-implementation: update the board

After Task 3, move C3-BUILD 🤖→ done-pending-review in `.dev/KANBAN.md` Board row (BUILD ✅) so the card state tracks reality. REVIEW/GATE stay for the human to interrogate + run.

## Self-Review notes (author)

- **Spec coverage:** config constants (T2/S1), sharded page_index (T2/S2-3), per-page mutex doc (T3), gate driver + TSan + read-back (T1), Makefile targets (T1), docs/01 §5 already committed. All spec files covered.
- **Type consistency:** `pi_shard_t`, `NOX_INDEX_SHARDS`, `NOX_CACHELINE`, `pi_hash`, `pi_shard` used consistently across T1/T2. Public API signatures unchanged (verified against `include/noxdb.h`).
- **No placeholders:** all steps carry full code/commands + expected bench-box output.
