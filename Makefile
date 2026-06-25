# WSBuffer - build + deploy.
#
# NOTE: O_DIRECT is a Linux feature; the benchmark binary must be built and run
# on the Proxmox VM with an NVMe-backed XFS/EXT4 filesystem. Do not run locally.

CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc
LDFLAGS := -pthread

SRC   := $(wildcard src/*.c)
OBJ   := $(SRC:.c=.o)
BENCH := bench/benchmark

# Override on the command line, e.g.:
#   make deploy REMOTE=vini@10.0.0.20 REMOTE_DIR=~/wsbuffer
REMOTE     ?= user@proxmox-vm
REMOTE_DIR ?= ~/wsbuffer

all: $(BENCH)

$(BENCH): $(OBJ) bench/benchmark.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

bench: $(BENCH)

# Sync only source/build files to the Proxmox VM (CLAUDE.md §3).
deploy:
	rsync -avz \
	    --include='*/' \
	    --include='*.c' --include='*.h' --include='Makefile' \
	    --exclude='*' \
	    ./ $(REMOTE):$(REMOTE_DIR)/

clean:
	rm -f src/*.o bench/*.o $(BENCH)

.PHONY: all bench deploy clean
