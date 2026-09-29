# bench/Makefile — CoreSight ETM overflow characterization binaries
#
# Cross-compiles bench_addr.c and bench_call.c for Zynq (Cortex-A, aarch64 Linux).
#
# Targets:
#   make                          — build everything (addr_loop, addr_chain, call)
#   make addr_loop                — build all (N, S) combinations
#   make addr_chain               — chain-mode equivalent of 'addr_loop'
#   make call                     — bench_call.c, every (CALL_K, CALL_LEN)
#   make dump N=64 S=4096 I=1000  — disassemble a specific bench_addr binary
#   make clean
#
# Chain mode (bench_chain_nN_sS_iI) replaces the call/return loop with a
# hand-written asm stub chain (see tracecliff/gen_chain.py)
#
# Call mode (bench_call_kK_lL_iI, bench_call.c): a PLT call into libc, strlen
# over CALL_LEN bytes, on CALL_K of every CALL_P iterations, the only family whose execution leaves the tracee text.
# Built dynamically linked on purpose, see the header of src/bench_call.c.
#
# Parameters (override on command line):
#   CROSS   — toolchain prefix     (default: aarch64-linux-gnu-)
#   N       — single N_TARGETS for 'make dump'
#   S       — single STUB_STRIDE for 'make dump'
#   I       — single ITERS for 'make dump'
#
# Every target builds its binaries into its own subdirectory of bin/:
#   bin/addr_loop/, bin/addr_chain/, bin/call/

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump

CFLAGS  := -O1 -g -Wall -Wextra
LDFLAGS := -lrt

SRC := src/bench_addr.c
CALL_SRC := src/bench_call.c

# N/S/ITERS sweep axes, not defined here: axes.mk is generated from
# tracecliff/benches.py, which is also what the sweeps iterate over, so the
# binaries built here and the grid swept there cannot drift apart. Edit the
# axes in benches.py; make regenerates axes.mk and restarts itself.
#   ADDR_N_LIST: 1 target   = baseline (same address every call, best compression)
#                3+ targets = past the ETM exact-match history depth (3 slots)
#   ADDR_STRIDE_LIST: byte alignment between stub targets, 0 up to one page
#   ITERS_LIST: iteration counts
#   CALL_K_LIST: calling iterations out of every CALL_P (crossing density)
#   CALL_P: the period CALL_K counts over
#   CALL_LEN_LIST: bytes strlen scans per call (time spent in libc)
#   CALL_ITERS_LIST: iteration counts for the call family

# axes.mk's rule is the first one in this file, it would otherwise become the
# default goal and bare 'make' would build nothing else.
.DEFAULT_GOAL := all

axes.mk: tracecliff/benches.py
	python3 -m tracecliff axes --make > $@

include axes.mk

DEFAULT_ITERS := 1000

# Every sweep target gets its own binary output directory under bin/
BINDIR           := bin
ADDR_LOOP_DIR    := $(BINDIR)/addr_loop
ADDR_CHAIN_DIR   := $(BINDIR)/addr_chain
CALL_DIR         := $(BINDIR)/call

BIN_DIRS := $(ADDR_LOOP_DIR) $(ADDR_CHAIN_DIR) $(CALL_DIR)

# Generated chain-mode stub assembly (see gen_chain.py)
CHAINS_DIR := chains

ADDR_LOOP_BINS := $(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(ADDR_LOOP_DIR)/bench_n$(n)_s$(s)_i$(i))))

CHAIN_ASMS := $(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(CHAINS_DIR)/chain_n$(n)_s$(s).S))
ADDR_CHAIN_BINS := $(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(ADDR_CHAIN_DIR)/bench_chain_n$(n)_s$(s)_i$(i))))

CALL_BINS := $(foreach k,$(CALL_K_LIST),$(foreach l,$(CALL_LEN_LIST),$(foreach i,$(CALL_ITERS_LIST),$(CALL_DIR)/bench_call_k$(k)_l$(l)_i$(i))))

.PHONY: all addr_loop addr_chain call dump clean

all: addr_loop addr_chain call

# Directory creation
$(BIN_DIRS) $(CHAINS_DIR):
	mkdir -p $@

addr_loop: $(ADDR_LOOP_BINS)

addr_chain: $(ADDR_CHAIN_BINS)

call: $(CALL_BINS)

# Generated rule for each (N, S, I) triple, $(4) is the output directory
define BUILD_RULE
$(4)/bench_n$(1)_s$(2)_i$(3): $(SRC) | $(4)
	$(CC) $(CFLAGS) -DN_TARGETS=$(1) -DSTUB_STRIDE=$(2) -DITERS=$(3) $$< $(LDFLAGS) -o $$@

endef

$(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(eval $(call BUILD_RULE,$(n),$(s),$(i),$(ADDR_LOOP_DIR))))))

# Generated chain assembly, one per (N, S), reused across every ITERS value
define CHAIN_ASM_RULE
$(CHAINS_DIR)/chain_n$(1)_s$(2).S: tracecliff/gen_chain.py | $(CHAINS_DIR)
	python3 -m tracecliff gen-chain $(1) $(2) > $$@

endef

$(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(eval $(call CHAIN_ASM_RULE,$(n),$(s)))))

# Generated rule for each chain-mode (N, S, I) triple, $(4) is the output directory
define CHAIN_BUILD_RULE
$(4)/bench_chain_n$(1)_s$(2)_i$(3): $(SRC) $(CHAINS_DIR)/chain_n$(1)_s$(2).S | $(4)
	$(CC) $(CFLAGS) -DCHAIN -DN_TARGETS=$(1) -DSTUB_STRIDE=$(2) -DITERS=$(3) $(SRC) $(CHAINS_DIR)/chain_n$(1)_s$(2).S $(LDFLAGS) -o $$@

endef

$(foreach n,$(ADDR_N_LIST),$(foreach s,$(ADDR_STRIDE_LIST),$(foreach i,$(ITERS_LIST),$(eval $(call CHAIN_BUILD_RULE,$(n),$(s),$(i),$(ADDR_CHAIN_DIR))))))

# Generated rule for each call-mode (CALL_K, CALL_LEN, ITERS) triple. No -static: the
# callee has to sit in a mapping the ETM range does not cover. -fno-builtin
# keeps strlen a real PLT call rather than an inline expansion.
define CALL_BUILD_RULE
$(CALL_DIR)/bench_call_k$(1)_l$(2)_i$(3): $(CALL_SRC) | $(CALL_DIR)
	$(CC) $(CFLAGS) -fno-builtin -DCALL_K=$(1) -DCALL_P=$(CALL_P) -DCALL_LEN=$(2) -DITERS=$(3) $$< $(LDFLAGS) -o $$@

endef

$(foreach k,$(CALL_K_LIST),$(foreach l,$(CALL_LEN_LIST),$(foreach i,$(CALL_ITERS_LIST),$(eval $(call CALL_BUILD_RULE,$(k),$(l),$(i))))))


# Disassemble a specific binary
N ?= 64
S ?= 4096
I ?= $(DEFAULT_ITERS)
dump: $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I)
	$(OBJDUMP) -d $< > $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I).dump
	@echo "Written $(ADDR_LOOP_DIR)/bench_n$(N)_s$(S)_i$(I).dump"

clean:
	rm -rf $(BIN_DIRS)
	rm -f $(CHAIN_ASMS)
	rm -f axes.mk
