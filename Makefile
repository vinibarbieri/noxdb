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

REMOTE     ?= noxdb
REMOTE_DIR ?= ~/noxdb

all: $(BENCH)

$(BENCH): $(OBJ) bench/benchmark.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

bench: $(BENCH)

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
	rm -f src/*.o bench/*.o $(BENCH) $(PROBE)

.PHONY: all bench probe deploy clean
