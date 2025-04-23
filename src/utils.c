/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sched.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

#include <linux/elf.h>
#include <linux/limits.h>

#include <asm/ptrace.h>
#include <asm/unistd.h>

#define MAX_LINE 8192
#define MAX_CPUS 4096

/* ============== DEBUG ============== */

/* FIXME: Note: These functions are not called directly, I guess they might
 * serve debugging purposes */

/* Debug use only, dump a buffer to a file by name */
void dump_buf(void *buf, size_t buf_size, const char *buf_path)
{
  FILE *fp;
  size_t fwrite_size;

  fp = fopen(buf_path, "wb");
  if (fp == NULL) {
    perror("fopen");
    return;
  }

  if ((fwrite_size = fwrite(buf, 1, buf_size, fp)) != buf_size) {
    fprintf(stderr, "fwrite() failed: %ld (expected: %ld)\n", fwrite_size,
            buf_size);
  }

  fclose(fp);
}

void dump_maps(FILE *stream, pid_t pid)
{
  FILE *fp;
  char maps_path[PATH_MAX];
  char *line;
  size_t n;

  memset(maps_path, 0, sizeof(maps_path));
  snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);

  fp = fopen(maps_path, "r");
  if (fp == NULL) {
    perror("fopen");
    return;
  }

  line = NULL;
  n = 0;
  while (getline(&line, &n, fp) != -1) {
    fprintf(stream, "%s", line);
  }

  if (line != NULL) {
    free(line);
  }

  fclose(fp);

  return;
}

/* ============== UDMABUF ============== */

/* Retrieve information (address and size) of a UDMA buffer */
int get_udmabuf_info(int udmabuf_num, unsigned long *phys_addr, size_t *size)
{
  const char *udmabuf_root = "/sys/class/u-dma-buf";

  int ret;
  char udmabuf_path[PATH_MAX];
  char tmp_path[PATH_MAX];
  char attr[1024];
  int fd;
  struct stat sb;

  ret = -1;

  /* Check for existence "/sys/class/u-dma-buf/udmabufX" */
  memset(udmabuf_path, '\0', sizeof(udmabuf_path));
  snprintf(udmabuf_path, sizeof(udmabuf_path), "%s/udmabuf%d", udmabuf_root,
           udmabuf_num);
  if (stat(udmabuf_path, &sb) != 0 || (!S_ISDIR(sb.st_mode))) {
    fprintf(stderr, "u-dma-buf device 'udmabuf%d' not found\n", udmabuf_num);
    return ret;
  }

  /* Read the "phys_addr" through the sysfs, opening the file in read-only */
  memset(tmp_path, '\0', sizeof(tmp_path));
  snprintf(tmp_path, sizeof(tmp_path), "%s/udmabuf%d/phys_addr", udmabuf_root,
           udmabuf_num);
  if ((fd = open(tmp_path, O_RDONLY)) < 0) {
    perror("open");
    return -1;
  }
  /* Store the address in attr */
  memset(attr, 0, sizeof(attr));
  if (read(fd, attr, sizeof(attr)) < 0) {
    perror("read");
    close(fd);
    return -1;
  }
  sscanf(attr, "%lx", phys_addr);
  close(fd);
  /* Read the "size" through the sysfs, opening the file in read-only */
  memset(tmp_path, '\0', sizeof(tmp_path));
  snprintf(tmp_path, sizeof(tmp_path), "%s/udmabuf%d/size", udmabuf_root,
           udmabuf_num);
  if ((fd = open(tmp_path, O_RDONLY)) < 0) {
    perror("open");
    return -1;
  }
  /* Store the size in attr */
  memset(attr, 0, sizeof(attr));
  if (read(fd, attr, sizeof(attr)) < 0) {
    perror("read");
    close(fd);
    return -1;
  }
  sscanf(attr, "%ld", size);
  close(fd);

  return 0;
}

