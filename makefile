#!/usr/bin/env make
# SPDX-License-Identifier: Apache-2.0
# Copyright 2021 Ricerca Security, Inc. All rights reserved.

SHELL:=bash

DEFAULT_BOARD?="ZCU-104"

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
CS_TRACE_FLAGS?=
CS_TRACE_OBJS:= \
	$(OBJS) \
	$(SRC)/cs-trace.o

ifneq ($(strip $(DEBUG)),)
  CS_TRACE_FLAGS+=--export --verbose=0
endif

DATE:=$(shell date +%Y-%m-%d-%H-%M-%S)
DIR?=trace/$(DATE)
TRACEE?=tests/fib
TRACEE_ARGS?=

TESTS_C:=$(wildcard tests/*.c)
TESTS:=$(patsubst tests/%.c, tests/%,$(TESTS_C))

LIB_DIR:=lib
LIBSTMPRELOAD:=$(LIB_DIR)/libstm_preload.so

$(CS_TRACE): $(CS_TRACE_OBJS) $(LIBCSACCESS) $(LIBCSACCUTIL)
	$(CC) -o $@ $^ $(CFLAGS)

trace: $(CS_TRACE) $(TESTS)
	mkdir -p $(DIR) && \
	cd $(DIR) && \
	sudo $(realpath $(CS_TRACE)) $(CS_TRACE_FLAGS) -- $(realpath $(TRACEE)) $(TRACEE_ARGS)

debug: $(CS_TRACE) $(TESTS)
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

tests/%: tests/%.c $(LIBSTMPRELOAD)
	$(CC) -o $@ $< $(CFLAGS)

format:
	clang-format -i $(INC)/*.h src/*.c

clean:
	rm -f $(CS_TRACE_OBJS) $(CS_TRACE) $(TESTS) $(LIBSTMPRELOAD)

dist-clean:
	$(MAKE) -C $(CSAL_BASE) clean $(CSAL_MAKE_FLAGS)

.PHONY: format libcsal clean trace
