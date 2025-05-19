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
CSAL_MAKE_FLAGS:=ARCH=$(CSAL_ARCH) NO_CHECK=1 NO_DIAG=1
LIBCSACCESS:=$(CSAL_LIB)/libcsaccess.a
LIBCSACCUTIL:=$(CSAL_LIB)/libcsacc_util.a

# ultrasight files
INC:=include
SRC:=src

HDRS:= \
	$(INC)/config.h \
	$(INC)/common.h \
	$(INC)/utils.h \
	$(INC)/known_boards.h \

OBJS:= \
	$(SRC)/common.o \
	$(SRC)/config.o \
	$(SRC)/utils.o \

CFLAGS:= \
	-std=c11 \
	-Wall \
	-DDEFAULT_BOARD_NAME=\"$(DEFAULT_BOARD)\" \
	-I$(INC) \
	-I$(CSAL_INC) \
	-lpthread \

ifneq ($(strip $(DEBUG)),)
	CFLAGS+=-O0
else
	CFLAGS+=-Ofast
endif

CS_TRACE:=cs-trace
CS_TRACE_FLAGS?=--export
CS_TRACE_OBJS:= \
	$(OBJS) \
	$(SRC)/cs-trace.o

ifneq ($(strip $(DEBUG)),)
  CS_TRACE_FLAGS+=--verbose=0
endif

# make trace values, setting up a new trace folder
DATE:=$(shell date +%Y-%m-%d-%H-%M-%S)
DIR?=trace/$(DATE)
TRACEE?=$(TESTS_DIR)/fib
TRACEE_ARGS?=

# test flags and compilation instructions
TESTS_DIR:= tests
TESTS_C:=$(wildcard $(TESTS_DIR)/*.c)
TESTS:=$(patsubst $(TESTS_DIR)/%.c, $(TESTS_DIR)/%,$(TESTS_C))
TESTS_CFLAGS+= \
	-std=c11 \
	-Wall \

# Decoder from the OpenCSD test examples
DECODER := trc_pkt_lister
DECODED_TRACE := $(DIR)/trace.opencsd
LATEST := trace/latest.opencsd

LIB_DIR:=lib
LIBSTMPRELOAD:=$(LIB_DIR)/libstm_preload.so

$(CS_TRACE): $(CS_TRACE_OBJS) $(LIBCSACCESS) $(LIBCSACCUTIL)
	$(CC) -o $@ $^ $(CFLAGS)

trace: $(CS_TRACE) $(TESTS) disable_aslr
	mkdir -p $(DIR) && \
	cd $(DIR) && \
	sudo $(realpath $(CS_TRACE)) $(CS_TRACE_FLAGS) -- $(realpath $(TRACEE)) $(TRACEE_ARGS)
	$(realpath $(DECODER)) -ss_dir $(DIR) -logfile -logfilename $(DECODED_TRACE)
	rm -f $(LATEST)
	ln $(DECODED_TRACE) $(LATEST)

debug: $(CS_TRACE) $(TESTS) disable_aslr
	mkdir -p $(DIR) && \
	cd $(DIR) && \
	sudo gdb --args $(realpath $(CS_TRACE)) $(CS_TRACE_FLAGS) -- $(realpath $(TRACEE)) $(TRACEE_ARGS)


libcsal:
	$(MAKE) -C $(CSAL_BASE) $(CSAL_MAKE_FLAGS)

$(LIBCSACCESS): libcsal
$(LIBCSACCUTIL): libcsal

$(LIBSTMPRELOAD): src/stm_preload.c
	mkdir -p lib
	$(CC) -fPIC -shared $^ -o $@ -g -ffixed-x28

$(TESTS_DIR)/%: $(TESTS_DIR)/%.c $(LIBSTMPRELOAD)
	$(CUSTOM_CC) $(TESTS_CFLAGS) -o $@ $<

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

clean-trace:
	rm -rf trace

dist-clean:
	$(MAKE) -C $(CSAL_BASE) clean $(CSAL_MAKE_FLAGS)

.PHONY: format libcsal clean trace
