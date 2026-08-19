/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Quentin Ducasse on 2025-04-23:
 * - Removed decoder related code
 */

#ifndef CS_TRACE_UTILS_H
#define CS_TRACE_UTILS_H

#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>

#include <sys/types.h>

#include <linux/limits.h>

#define PAGE_SIZE 0x1000
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

#define RANGE_MAX (1)

/* u-dma-buf device the ETR drains into, named the same way as the fuzzsight
 * bitmap buffer: address and size come from sysfs at run time. The default is
 * UDMABUF_ETR_NAME from the build; ETR_UDMABUF_ENV overrides it per run, and
 * -u/--udmabuf overrides both. Must not name the bitmap buffer, whose own
 * setting lives in bitmap_dma.h. */
#define ETR_UDMABUF_ENV "ULTRASIGHT_UDMABUF_ETR"

#ifndef UDMABUF_ETR_NAME
#error "UDMABUF_ETR_NAME not defined: pass -DUDMABUF_ETR_NAME=\"<name>\" (see UDMABUF_ETR in the makefile)"
#endif

struct map_info {
  unsigned long start;
  unsigned long end;
  off_t offset;
  void *buf;
  char path[PATH_MAX];
};

struct mmap_params {
  void *addr;
  size_t length;
  int prot;
  int flags;
  int fd;
  off_t offset;
};

void dump_buf(void *buf, size_t buf_size, const char *buf_path);
void dump_maps(FILE *stream, pid_t pid);
void dump_map_info(FILE *stream, struct map_info *map_info, int count);
int setup_map_info(pid_t pid, struct map_info *map_info, int info_count_max);
int export_decoder_args(int trace_id, const char *trace_path,
                        const char *args_path, struct map_info *map_info,
                        int count);
int get_preferred_cpu(pid_t pid);
int find_free_cpu(void);
int set_cpu_affinity(int cpu, pid_t pid);

int get_udmabuf_info_by_name(const char *name, unsigned long *phys_addr,
                             size_t *size);
int get_ksight_info(unsigned long *phys_addr);
int ksight_set_enable(int enable);
int ksight_set_traced_pid(pid_t pid);
int setup_stm_region(int *fd, void *map_base);
void clean_stm_region(int *fd, void *map_base);

#endif /* CS_TRACE_UTILS_H */