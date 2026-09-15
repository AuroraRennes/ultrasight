# bench/Makefile — CoreSight ETM overflow characterization binaries
#
# Cross-compiles bench_addr.c for Zynq (Cortex-A, aarch64 Linux).
#
# Targets:
#   make                          — build everything (addr_loop)
#   make addr_loop                — build all (N, S) combinations
#   make dump N=64 S=4096 I=1000  — disassemble a specific bench_addr binary
#   make clean
#
# Parameters (override on command line):
#   CROSS   — toolchain prefix     (default: aarch64-linux-gnu-)
#   N       — single N_TARGETS for 'make dump'
#   S       — single STUB_STRIDE for 'make dump'
#   I       — single ITERS for 'make dump'
#
# Every target builds its binaries into its own subdirectory of bin/:
#   bin/addr_loop/

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump

CFLAGS  := -O1 -g -Wall -Wextra
LDFLAGS := -lrt

SRC := src/bench_addr.c

# N/S/ITERS sweep axes
# ADDR_N_LIST: 1 target   = baseline (same address every call, best compression)
#              3+ targets = past the ETM exact-match history depth (3 slots)
# ADDR_STRIDE_LIST: byte alignment between stub targets, 0 up to one page
# ITERS_LIST: iteration counts
ADDR_N_LIST      := 1 2 3 4 8 16 32 64
ADDR_STRIDE_LIST := 0 64 128 256 512 1024 2048 4096
ITERS_LIST       := 100 1000 10000 100000 1000000 10000000 100000000

DEFAULT_ITERS := 1000

# Every sweep target gets its own binary output directory under bin/
BINDIR           := bin
ADDR_LOOP_DIR    := $(BINDIR)/addr_loop

BIN_DIRS := $(ADDR_LOOP_DIR)

ADDR_LOOP_BINS := $(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(ADDR_LOOP_DIR)/bench_n$(n)_s$(s)_i$(i))))


.PHONY: all addr_loop dump clean

all: addr_loop

# Directory creation
$(BIN_DIRS):
	mkdir -p $@

addr_loop: $(ADDR_LOOP_BINS)

# Generated rule for each (N, S, I) triple, $(4) is the output directory
define BUILD_RULE
$(4)/bench_n$(1)_s$(2)_i$(3): $(SRC) | $(4)
	$(CC) $(CFLAGS) -DN_TARGETS=$(1) -DSTUB_STRIDE=$(2) -DITERS=$(3) $$< $(LDFLAGS) -o $$@

endef

$(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(eval $(call BUILD_RULE,$(n),$(s),$(i),$(ADDR_LOOP_DIR))))))

# Disassemble a specific binary
N ?= 64
S ?= 4096
I ?= $(DEFAULT_ITERS)
dump: $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I)
	$(OBJDUMP) -d $< > $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I).dump
	@echo "Written $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I).dump"

clean:
	rm -rf $(BIN_DIRS)
