# noxdb - build + deploy.
#
# NOTE: O_DIRECT is a Linux feature; the benchmark binary must be built and run
# on the bare-metal bench box against the NVMe-backed XFS/EXT4 filesystem at
# /mnt/nvme. Do not run locally (macOS/other FS will not honor O_DIRECT).

CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
LDFLAGS := -pthread

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

%.o: %.c
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
	    $(GATE_C4) bench/otflush_test_tsan $(GATE_C4_SOAK) \
	    bench/pc_queue_toy bench/pc_queue_toy_tsan bench/pwritev_toy \
	    bench/queue_test bench/queue_test_tsan bench/holes_test

.PHONY: all bench probe gate gate-c3 gate-c3-tsan gate-c4 gate-c4-tsan soak-c4 \
    toy-pc toy-pc-tsan toy-pwritev deploy clean \
    test-queue test-queue-tsan test-holes
