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

# C0 alignment probe. Standalone: raw syscalls only, NO engine objects linked.
# Compiles straight from the single .c (pulls NOX_BLOCK_SIZE via -Isrc).
probe: $(PROBE)

$(PROBE): bench/o_direct_probe.c
	$(CC) $(CFLAGS) -o $@ $<

# Sync only source/build files to the bench box (rsync, key auth, host alias).
deploy:
	rsync -avz -m \
	    --exclude='.git' \
	    --include='*/' \
	    --include='*.c' --include='*.h' --include='Makefile' \
	    --exclude='*' \
	    ./ $(REMOTE):$(REMOTE_DIR)/

clean:
	rm -f src/*.o bench/*.o $(BENCH) $(PROBE) $(GATE) $(GATE_C3) bench/concurrency_test_tsan

.PHONY: all bench probe gate gate-c3 gate-c3-tsan deploy clean
