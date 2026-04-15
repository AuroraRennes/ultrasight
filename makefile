#!/usr/bin/env make
# SPDX-License-Identifier: Apache-2.0
# Copyright 2021 Ricerca Security, Inc. All rights reserved.

SHELL:=bash

DEFAULT_BOARD?="ZCU-104"

# Check if CUSTOM_LLVM_DIR is set
ifeq ($(origin CUSTOM_CC_BUILD_DIR), undefined)
$(warning [-] CUSTOM_CC_BUILD_DIR is not defined, using base CC)
CUSTOM_CC := $(CC)
else
$(info [+] Custom build directory set!)
CUSTOM_CC := $(CUSTOM_CC_BUILD_DIR)/bin/clang
CUSTOM_LD := $(CUSTOM_CC_BUILD_DIR)/bin/ld.lld
endif

# Check if CUSTOM_LIBC is set
ifeq ($(origin CUSTOM_LIBC), undefined)
$(warning [-] CUSTOM_LIBC is not defined, using base libc.)
else
$(info [+] Custom libc set!)
TESTS_CFLAGS := \
  -static \
  -nostdlib \
  -fuse-ld=$(CUSTOM_LD) \
  -isystem $(CUSTOM_LIBC)/sysroot/include \
  $(CUSTOM_LIBC)/sysroot/lib/crt1.o \
  $(CUSTOM_LIBC)/sysroot/lib/crti.o \
  -L$(CUSTOM_LIBC)/sysroot/lib -lc -lm \
  $(CUSTOM_LIBC)/sysroot/lib/crtn.o
endif


# CSAL libraries definitions
CSAL_BASE:=csal
CSAL_ARCH:=arm64
ifneq ($(strip $(DEBUG)),)
	CSAL_BUILD:=dbg
else
	CSAL_BUILD:=rel
endif
CSAL_INC:=$(CSAL_BASE)/include
CSAL_LIB:=$(CSAL_BASE)/lib/$(CSAL_ARCH)/$(CSAL_BUILD)
CSAL_MAKE_FLAGS:=ARCH=$(CSAL_ARCH) NO_DIAG=1 CHECK=1
LIBCSACCESS:=$(CSAL_LIB)/libcsaccess.a
LIBCSACCUTIL:=$(CSAL_LIB)/libcsacc_util.a

# ultrasight files
INC:=include
SRC:=src
PROXY:=proxy
LIB:=lib

HDRS:= \
	$(INC)/config.h \
	$(INC)/common.h \
	$(INC)/utils.h \
	$(INC)/known_boards.h \
	$(INC)/ksight.h \
	$(INC)/axi_regs.h \
	$(INC)/decoder_stats.h \
	$(INC)/edge_stats.h \
	$(INC)/bitmap_dma.h \
	$(INC)/decoder_axi.h \
	$(INC)/timing.h \

OBJS:= \
	$(SRC)/common.o \
	$(SRC)/config.o \
	$(SRC)/utils.o \
	$(SRC)/axi_regs.o \
	$(SRC)/bitmap_dma.o \

CFLAGS:= \
	-std=c11 \
	-Wall \
	-DDEFAULT_BOARD_NAME=\"$(DEFAULT_BOARD)\" \
	-I$(INC) \
	-I$(CSAL_INC) \
	-lpthread \

ifneq ($(strip $(DEBUG)),)
	CFLAGS+=-O0 -g
else
	CFLAGS+=-Ofast
endif

ifneq ($(strip $(TIMING)),)
    CFLAGS += -DTIMING
endif

# cs-trace - Standalone tracer
CS_TRACE:=cs-trace
CS_TRACE_FLAGS?=--export # --ksight
CS_TRACE_OBJS:= \
	$(OBJS) \
	$(SRC)/cs-trace.o

ifneq ($(strip $(DEBUG)),)
  CS_TRACE_FLAGS+=--verbose=0
endif

# fuzzsight-proxy - AFL++ fork server proxy
FUZZSIGHT_PROXY:=$(PROXY)/fuzzsight-proxy
FUZZSIGHT_LIB:=$(LIB)/libfuzzsight.a
FUZZSIGHT_PROXY_OBJ:=$(PROXY)/fuzzsight-proxy.o

# libforksrv - LD_PRELOAD fork server library
LIBFORKSRV_DIR:=$(PROXY)/libforksrv
LIBFORKSRV:=$(LIBFORKSRV_DIR)/libforksrv.so

# make trace values, setting up a new trace folder
DATE:=$(shell date +%Y-%m-%d-%H-%M-%S)
DIR?=trace/$(DATE)
TRACEE?=$(TESTS_DIR)/fib
TRACEE_ENVS?=
TRACEE_ARGS?=
TRACEE_DUMP?=$(TRACEE).dump

# test flags and compilation instructions
TESTS_DIR:= tests
TESTS_C:=$(wildcard $(TESTS_DIR)/*.c)
TESTS:=$(patsubst $(TESTS_DIR)/%.c, $(TESTS_DIR)/%,$(TESTS_C))
TESTS_DUMPS:=$(patsubst $(TESTS_DIR)/%.c, $(TESTS_DIR)/%.dump,$(TESTS_C))
TESTS_CFLAGS+= \
	-std=c11 \
	-Wall \
	-O0 \
	-g \
	-Wl,-z,relro,-z,now

# Decoder from the OpenCSD test examples
DECODER := trc_pkt_lister
DECODED_TRACE := $(DIR)/trace.opencsd
LATEST := trace/latest.opencsd

LIBSTMPRELOAD:=$(LIB)/libstm_preload.so

# ----------------------------------------------------------------------------
# Build targets
# ----------------------------------------------------------------------------

all: $(CS_TRACE) $(FUZZSIGHT_LIB) $(FUZZSIGHT_PROXY)

$(CS_TRACE): $(CS_TRACE_OBJS) $(LIBCSACCESS) $(LIBCSACCUTIL)
	$(CC) -o $@ $^ $(CFLAGS)

$(FUZZSIGHT_LIB): $(OBJS)
	$(AR) rcs $@ $^

# fuzzsight-proxy.c needs the AFL++ include path
$(FUZZSIGHT_PROXY_OBJ): $(PROXY)/fuzzsight-proxy.c
	$(CC) $(CFLAGS) -I$(AFL_INC) -c $< -o $@


$(FUZZSIGHT_PROXY): $(FUZZSIGHT_PROXY_OBJ) $(FUZZSIGHT_LIB) $(LIBCSACCESS) $(LIBCSACCUTIL)
	$(CC) -o $@ $^ $(CFLAGS) -lpthread

# ----------------------------------------------------------------------------
# Trace / debug helpers
# ----------------------------------------------------------------------------

trace: $(CS_TRACE) $(TESTS) disable_aslr
	mkdir -p $(DIR) && \
	cd $(DIR) && \
	sudo $(realpath $(CS_TRACE)) $(CS_TRACE_FLAGS) -- $(TRACEE_ENVS) $(realpath $(TRACEE)) $(TRACEE_ARGS)
	cp $(realpath $(TRACEE)) $(DIR)
	objdump -d $(realpath $(TRACEE)) > $(DIR)/$(notdir $(basename $(TRACEE))).dump
	$(realpath $(DECODER)) -ss_dir $(DIR) -stats -logfile -logfilename $(DECODED_TRACE)
	rm -f $(LATEST)
	ln $(DECODED_TRACE) $(LATEST)

debug: $(CS_TRACE) $(TESTS_DUMPS) disable_aslr
	mkdir -p $(DIR) && \
	cd $(DIR) && \
	sudo gdb --args $(realpath $(CS_TRACE)) $(CS_TRACE_FLAGS) -- $(realpath $(TRACEE)) $(TRACEE_ARGS)

proxy: $(LIBFORKSRV)
	rm -f $(FUZZSIGHT_PROXY_OBJ) $(FUZZSIGHT_PROXY)
	$(MAKE) $(FUZZSIGHT_PROXY) AFL_INC=$(AFL_INC) DEBUG=$(DEBUG) TIMING=$(TIMING)

# ----------------------------------------------------------------------------
# Libraries
# ----------------------------------------------------------------------------

libcsal:
	$(MAKE) -C $(CSAL_BASE) $(CSAL_MAKE_FLAGS)

$(LIBCSACCESS): libcsal
$(LIBCSACCUTIL): libcsal

$(LIBSTMPRELOAD): src/stm_preload.c
	mkdir -p lib
	$(CC) -fPIC -shared $^ -o $@ -g -ffixed-x26

libforksrv: $(LIBFORKSRV)

$(LIBFORKSRV): $(LIBFORKSRV_DIR)/libforksrv.c
	$(MAKE) -C $(LIBFORKSRV_DIR)

# ----------------------------------------------------------------------------
# Test programs
# ----------------------------------------------------------------------------

$(TESTS_DIR)/%: $(TESTS_DIR)/%.c $(LIBSTMPRELOAD)
	$(CUSTOM_CC) $(TESTS_CFLAGS) -o $@ $<

# ----------------------------------------------------------------------------
# Utilities
# ----------------------------------------------------------------------------

format:
	clang-format -i $(INC)/*.h src/*.c

disable_aslr:
	@echo "Checking ASLR status..."
	@if [ "$$(cat /proc/sys/kernel/randomize_va_space)" -ne 0 ]; then \
		echo "ASLR is enabled, disabling it..."; \
		sudo sysctl -w kernel.randomize_va_space=0; \
	else \
		echo "ASLR is already disabled."; \
	fi

clean:
	rm -f $(CS_TRACE_OBJS) $(CS_TRACE) $(TESTS) $(LIBSTMPRELOAD)
	$(MAKE) -C $(LIBFORKSRV_DIR) clean

clean-trace:
	rm -rf trace

clean-dist:
	$(MAKE) -C $(CSAL_BASE) clean $(CSAL_MAKE_FLAGS)

clean-test:
	rm -f $(TESTS)

clean-all: clean clean-trace clean-dist

.PHONY: format libcsal clean trace libforksrv proxy
