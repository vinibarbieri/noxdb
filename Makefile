# noxdb - build + deploy.
#
# NOTE: O_DIRECT is a Linux feature; the benchmark binary must be built and run
# on the bare-metal bench box against the NVMe-backed XFS/EXT4 filesystem at
# /mnt/nvme. Do not run locally (macOS/other FS will not honor O_DIRECT).

CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
LDFLAGS := -pthread -lm      # -lm for the gate's pow() in the latency CSV dump

# Header-size knob (NOX_MAX_ENTRIES, src/noxdb_config.h). Folded into CFLAGS so
# it reaches EVERY target, gates included — a gate number is only meaningful if
# the binary was really built with the entry count the command line asked for.
#   make gate-c4 NOX_ENTRIES=64
NOX_ENTRIES ?=
ENTRY_FLAGS := $(if $(NOX_ENTRIES),-DNOX_MAX_ENTRIES_OVERRIDE=$(NOX_ENTRIES))
CFLAGS += $(ENTRY_FLAGS)

SRC   := $(wildcard src/*.c)
OBJ   := $(SRC:.c=.o)
BENCH := bench/benchmark
PROBE := bench/o_direct_probe
GATE  := bench/scrap_integrity_test
GATE_C3 := bench/concurrency_test
GATE_C4 := bench/otflush_test
GATE_C4_SOAK := bench/otflush_soak

REMOTE     ?= noxdb
REMOTE_DIR ?= ~/noxdb

all: $(BENCH)

$(BENCH): $(OBJ) bench/benchmark.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Object files bake the header size in, and make cannot see that a -D changed:
# `make gate-c4 NOX_ENTRIES=64` would relink 15-entry objects and print a
# "64-entry" result that is nothing of the sort. This stamp records the current
# value and forces a rebuild whenever it moves.
.entries-stamp: FORCE
	@echo '$(NOX_ENTRIES)' | cmp -s - $@ 2>/dev/null || echo '$(NOX_ENTRIES)' > $@
FORCE:

%.o: %.c .entries-stamp
	$(CC) $(CFLAGS) -c -o $@ $<

bench: $(BENCH)

# C2 acceptance gate. Links the full engine + the gate driver.
# Run on the bench box: ./bench/scrap_integrity_test /mnt/nvme/c2gate.dat
gate: $(GATE)

$(GATE): $(OBJ) bench/scrap_integrity_test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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

# C4 acceptance gate: async two-stage flushing. Bench box only.
# Run: ./bench/otflush_test /mnt/nvme/c4gate.dat 8
gate-c4: $(GATE_C4)

$(GATE_C4): $(OBJ) bench/otflush_test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# C4 "before" build: identical tree, one #ifdef apart, with the eager 256KB
# memset put back in scrap_page_alloc. Measurement artifact only - this is the
# binary that produces the BEFORE curve of the thread-05 latency CDF. Compiled
# from sources in one shot (not from $(OBJ)) so the -DNOX_EAGER_ZERO objects can
# never be linked into a real gate binary by a stale .o.
# Run: NOX_LAT_CSV=before.csv ./bench/otflush_test_zero /mnt/nvme/c4gate.dat 8
gate-c4-zero:
	$(CC) $(CFLAGS) -DNOX_EAGER_ZERO -o bench/otflush_test_zero \
	    $(SRC) bench/otflush_test.c $(LDFLAGS)

# Entry-count + write-amplification study (src/nox_stats.h).
#
# WSBuffer justifies its 15 index entries with a measurement — "less than 15 in
# more than 95% cases" — and says outright that the 128B header is a DEFAULT and
# that small-write workloads want a larger one. These targets re-run that
# experiment on this engine, so NOX_HEADER_SIZE gets chosen from our own data
# instead of from intuition.
#
# SEPARATE BINARIES ON PURPOSE. The counters sit on the foreground write path,
# and this cycle's entire claim is a latency distribution; an atomic increment
# per write would perturb the number the gate exists to defend. A stats build is
# never a gate build.
#
# Sweep the header size without editing a file:
#   make stats-c4 NOX_ENTRIES=15  && ./bench/otflush_test_stats /mnt/nvme/c4gate.dat 8
#   make stats-c4 NOX_ENTRIES=63  && ./bench/otflush_test_stats /mnt/nvme/c4gate.dat 8
#   make stats-c4 NOX_ENTRIES=255 && ./bench/otflush_test_stats /mnt/nvme/c4gate.dat 8
# 255 is the hard ceiling: WSBuffer's `number` field is 1 byte.
# NOX_ENTRIES itself is handled up top (ENTRY_FLAGS, folded into CFLAGS).
STATS_FLAGS := -DNOX_STATS

stats-c4:
	$(CC) $(CFLAGS) $(STATS_FLAGS) -o bench/otflush_test_stats \
	    $(SRC) bench/otflush_test.c $(LDFLAGS)

stats-soak:
	$(CC) $(CFLAGS) $(STATS_FLAGS) -o bench/otflush_soak_stats \
	    $(SRC) bench/otflush_soak.c $(LDFLAGS)

# C4 race gate: single instrumented compile, TSan objects never mixed with -O2.
# Run: ./bench/otflush_test_tsan /mnt/nvme/c4gate.dat 4
gate-c4-tsan:
	$(CC) $(CFLAGS) -fsanitize=thread -g -o bench/otflush_test_tsan \
	    $(SRC) bench/otflush_test.c $(LDFLAGS)

# C4-B6 addendum / C4-G7 (.dev/KANBAN.md): >=20min soak with OVERLAPPING
# 256KB bases (many threads share one page_index slot/page-lock/queue slot -
# the case gate-c3/gate-c4 deliberately partition away from), sampling both
# memcmp integrity and RSS over time. See bench/otflush_soak.c's header for
# why a FAIL here on integrity or RSS alone is a documented, expected C5-core
# gap (board cards C4-B8/C4-B9), not a mystery bug.
# Defaults: 20 min / 16 threads against /mnt/nvme/c4soak.dat. Override for a
# short smoke run, e.g.:
#   make soak-c4 SOAK_SECONDS=30 SOAK_THREADS=4
SOAK_SECONDS ?= 1200
SOAK_THREADS ?= 16
SOAK_PATH    ?= /mnt/nvme/c4soak.dat

soak-c4: $(GATE_C4_SOAK)
	./$(GATE_C4_SOAK) $(SOAK_PATH) $(SOAK_SECONDS) $(SOAK_THREADS)

$(GATE_C4_SOAK): $(OBJ) bench/otflush_soak.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# C4-S2 study toy: unbounded MPMC queue (mutex + condvar), 4 prod / 4 cons.
# Pure RAM, no O_DIRECT: runs on the laptop. Standalone, no engine objects.
toy-pc: bench/pc_queue_toy.c
	$(CC) $(CFLAGS) -o bench/pc_queue_toy $< $(LDFLAGS)

# Same toy under ThreadSanitizer (-O1 -g: TSan needs frame pointers to be useful).
toy-pc-tsan: bench/pc_queue_toy.c
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -g -o bench/pc_queue_toy_tsan $< $(LDFLAGS)

# C4-S3 study toy: scatter-gather (pwritev) + short-write fixup.
# Part C (fixup, fake writer) is pure RAM and runs on the laptop; parts A and B
# need O_DIRECT and are compiled in only on Linux. Standalone, no engine objects.
#   ./bench/pwritev_toy                      # part C only
#   ./bench/pwritev_toy /mnt/nvme/s3toy.dat  # A + B + C
toy-pwritev: bench/pwritev_toy.c
	$(CC) $(CFLAGS) -o bench/pwritev_toy $<

# C0 alignment probe. Standalone: raw syscalls only, NO engine objects linked.
# Compiles straight from the single .c (pulls NOX_BLOCK_SIZE via -Isrc).
probe: $(PROBE)

$(PROBE): bench/o_direct_probe.c
	$(CC) $(CFLAGS) -o $@ $<

# --- Local unit tests (RAM only, no O_DIRECT) -------------------------------
# These are the ONLY targets that may be run on the dev laptop. Everything else
# needs the bench box.
test-queue:
	$(CC) $(CFLAGS) -o bench/queue_test src/queue.c bench/queue_test.c $(LDFLAGS)
	./bench/queue_test

test-queue-tsan:
	$(CC) $(CFLAGS) -fsanitize=thread -g -o bench/queue_test_tsan \
	    src/queue.c bench/queue_test.c $(LDFLAGS)
	./bench/queue_test_tsan

test-holes:
	$(CC) $(CFLAGS) -o bench/holes_test src/scrap_page.c bench/holes_test.c $(LDFLAGS)
	./bench/holes_test

# Sync only source/build files to the bench box (rsync, key auth, host alias).
deploy:
	rsync -avz -m \
	    --exclude='.git' \
	    --include='*/' \
	    --include='*.c' --include='*.h' --include='Makefile' \
	    --exclude='*' \
	    ./ $(REMOTE):$(REMOTE_DIR)/

clean:
	rm -f src/*.o bench/*.o $(BENCH) $(PROBE) $(GATE) $(GATE_C3) bench/concurrency_test_tsan \
	    $(GATE_C4) bench/otflush_test_tsan bench/otflush_test_zero $(GATE_C4_SOAK) \
	    bench/pc_queue_toy bench/pc_queue_toy_tsan bench/pwritev_toy \
	    bench/queue_test bench/queue_test_tsan bench/holes_test \
	    bench/otflush_test_stats bench/otflush_soak_stats .entries-stamp

.PHONY: FORCE all bench probe gate gate-c3 gate-c3-tsan gate-c4 gate-c4-tsan \
    gate-c4-zero soak-c4 stats-c4 stats-soak \
    toy-pc toy-pc-tsan toy-pwritev deploy clean \
    test-queue test-queue-tsan test-holes
