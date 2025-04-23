/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Quentin Ducasse on 2025-04-23:
 * - Removed decoder related code
 * - Added comments for clarity
 */

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

/* ============== CPU AFFINITIES ============== */

/**
 * Allocate and initialize a CPU affinity mask for the number of processors on
 * the system
 * @param cpu_set pointer to a pointer where the allocated CPU will be stored
 * @return allocated set of CPUs
 */
static cpu_set_t *alloc_cpu_set(cpu_set_t **cpu_set, size_t *setsize)
{
  int nprocs;

  if (!cpu_set || !setsize) {
    return NULL;
  }
  /* Get the number of available CPU cores on the system */
  nprocs = get_nprocs();
  /* Allocates a set large enough to store the nprocs pCPUs */
  *cpu_set = CPU_ALLOC(nprocs);
  if (!(*cpu_set)) {
    perror("CPU_ALLOC");
    return NULL;
  }
  /* Initializes the CPU set */
  *setsize = CPU_ALLOC_SIZE(nprocs);
  CPU_ZERO_S(*setsize, *cpu_set);

  return *cpu_set;
}

/* Binds a process to a CPU */
int set_cpu_affinity(int cpu, pid_t pid)
{
  int ret;
  cpu_set_t *cpu_set;
  size_t setsize;

  ret = -1;
  /* Allocate a CPU set */
  if (!alloc_cpu_set(&cpu_set, &setsize)) {
    goto exit;
  }
  /* Sets the CPU in the set */
  CPU_SET_S(cpu, setsize, cpu_set);
  /* Applies the affinity */
  if (sched_setaffinity(pid, setsize, cpu_set) < 0) {
    perror("sched_setaffinity");
    goto exit;
  }
  ret = 0;

exit:
  /* Frees the CPU set */
  if (cpu_set) {
    CPU_FREE(cpu_set);
  }

  return ret;
}

/**
 * Find an available CPU that is not assigned to a process.
 * It does so by iterating over all processes in /proc, checking
 * its threads and their corresponding CPU affinities
 */
int find_free_cpu(void)
{
  int nprocs;
  DIR *proc_dir;
  struct dirent *proc_entry;
  char task_path[PATH_MAX];
  DIR *task_dir;
  struct dirent *task_entry;
  char status_path[PATH_MAX];
  FILE *status_fp;
  char tmp[MAX_LINE];
  bool has_vmsize;
  unsigned int hval;
  bool cpu_used[MAX_CPUS];
  int i;

  /* Get the number of online processors, see sysconf(3) */
  nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  if (nprocs < 2) {
    return 0;
  }
  /* Initialize an array to track used CPUs */
  memset(cpu_used, (int)false, sizeof(cpu_used));

  /* Iterate over all processes in /proc */
  if (!(proc_dir = opendir("/proc"))) {
    perror("opendir");
    return -1;
  }

  /* For each process, check its threads (tasks) */
  while ((proc_entry = readdir(proc_dir))) {
    if (!isdigit(proc_entry->d_name[0])) {
      continue;
    }
    memset(task_path, 0, PATH_MAX);
    snprintf(task_path, PATH_MAX, "/proc/%s/task", proc_entry->d_name);
    if (!(task_dir = opendir((const char *)task_path))) {
      perror("opendir");
      continue;
    }

    /* For each task, check its status to determine affinities */
    while ((task_entry = readdir(task_dir))) {
      if (!isdigit(task_entry->d_name[0])) {
        continue;
      }

      memset(status_path, 0, PATH_MAX);
      snprintf(status_path, PATH_MAX, "/proc/%s/task/%s/status",
               proc_entry->d_name, task_entry->d_name);
      if (!(status_fp = fopen(status_path, "r"))) {
        continue;
      }

      /* Once the status is found, gets:
       * - VmSize, ensuring it is a real process using virtual memory
       * - Cpus_allowed_list: CPUs the thread is allowed to run on
       */
      has_vmsize = false;
      while (fgets(tmp, MAX_LINE, status_fp)) {
        hval = 0;
        if (!strncmp(tmp, "VmSize:\t", 8)) {
          has_vmsize = true;
        }
        /* Ensures that no ranges (0-3) or lists (0,2,4) are used */
        if (!strncmp(tmp, "Cpus_allowed_list:\t", 19) && !strchr(tmp, '-') &&
            !strchr(tmp, ',') && sscanf(tmp + 19, "%u", &hval) == 1 &&
            hval < MAX_CPUS && has_vmsize) {
          /* Flag the corresponding CPUs in the array */
          cpu_used[hval] = true;
          break;
        }
      }
      fclose(status_fp);
    }
    closedir(task_dir);
  }
  closedir(proc_dir);

  /* Finds the first unassigned CPU in the array and returns it */
  for (i = 0; i < nprocs; i++) {
    if (!cpu_used[i]) {
      /* Free CPU found. */
      return i;
    }
  }

  /* Free CPU not found. */
  return -1;
}

/* Set given cpu_set bits represent related CPU cores with a given cpu.
 * It reads the list of CPUs that belong to the same physical core as the
 * given one and sets them in a CPU affinity set.
 */
static int set_core_cpus(int cpu, cpu_set_t *cpu_set, size_t setsize)
{
  int ret;
  FILE *fp;
  char core_cpus_list_path[PATH_MAX];
  char *token;
  size_t n;
  ssize_t readn;
  long int core_cpu;

  ret = -1;
  fp = NULL;
  token = NULL;

  if (!cpu_set || !setsize) {
    goto exit;
  }
  /* Opens the /sys/devices/.../cpu<n>/core_cpus_list file and reads the cpu
   * list */
  memset(core_cpus_list_path, 0, sizeof(core_cpus_list_path));
  snprintf(core_cpus_list_path, sizeof(core_cpus_list_path),
           "/sys/devices/system/cpu/cpu%d/topology/core_cpus_list", cpu);

  fp = fopen(core_cpus_list_path, "r");
  if (!fp) {
    perror("fopen");
    goto exit;
  }

  /* Parse the comma-separated list of CPUs, converting each token into a CPU in
   * cpu_set */
  token = NULL;
  n = 0;
  while ((readn = getdelim(&token, &n, ',', fp)) != -1) {
    if (readn > 1 && token[readn - 1] != '\0') {
      token[readn - 1] = '\0';
    }
    core_cpu = strtol(token, NULL, 0);
    if (core_cpu == LONG_MIN || core_cpu == LONG_MAX) {
      perror("strtol");
      goto exit;
    }
    CPU_SET_S((int)core_cpu, setsize, cpu_set);
  }

  ret = 0;

exit:
  if (token) {
    free(token);
  }

  if (fp) {
    fclose(fp);
  }

  return ret;
}

/**
 * Determines a preferred CPU for a given process, scanning all affinities
 */
int get_preferred_cpu(pid_t pid)
{
  int ret;
  int i;
  cpu_set_t *cpu_set;
  cpu_set_t *core_cpu_set;
  size_t setsize;
  size_t core_setsize;
  int nprocs;
  int preferred_cpu;

  ret = -1;
  cpu_set = NULL;
  core_cpu_set = NULL;
  preferred_cpu = -1;

  /* Allocate a cpu_set for affinity tracking */
  if (!alloc_cpu_set(&cpu_set, &setsize)) {
    goto exit;
  }
  /* Retrieve the CPU affinity for the process */
  if (sched_getaffinity(pid, setsize, cpu_set) < 0) {
    perror("sched_getaffinity");
    goto exit;
  }
  /* Get the number of processors available, allocate a CPU set for core
   * siblings tracking */
  nprocs = get_nprocs();
  if (!alloc_cpu_set(&core_cpu_set, &core_setsize)) {
    goto exit;
  }
  /* For each processor, determine which cores the process is using,
   * if it is using CPU i, it fills the core_cpu_set with all CPUs that
   * share a physical core with the process.
   */
  for (i = 0; i < nprocs; i++) {
    if (CPU_ISSET_S(i, setsize, cpu_set)) {
      if (set_core_cpus(i, core_cpu_set, core_setsize) < 0) {
        goto exit;
      }
    }
  }

  /* Scans all CPUs and finds the first one that is NOT in core_cpu_set,
   * meaning this CPU does NOT share a physical core with any currently assigned
   * CPUs
   */
  for (i = 0; i < nprocs; i++) {
    if (!CPU_ISSET_S(i, core_setsize, core_cpu_set)) {
      preferred_cpu = i;
      break;
    }
  }

  ret = preferred_cpu;

exit:
  if (core_cpu_set) {
    CPU_FREE(core_cpu_set);
  }

  if (cpu_set) {
    CPU_FREE(cpu_set);
  }

  return ret;
}

/* ============== MAP INFO ============== */

/* Dump all information stored in the map_info struct to a stream */
void dump_map_info(FILE *stream, struct map_info *map_info, int count)
{
  int i;

  for (i = 0; i < count; i++) {
    fprintf(stream, "[0x%lx-0x%lx]@0x%lx: %s\n", map_info[i].start,
            map_info[i].end, map_info[i].offset, map_info[i].path);
  }
}

/**
 * Extracts memory mapping information of a given process from
 * /proc/<pid>/maps getting executable regions, mapping those
 * in memory.
 */
int setup_map_info(pid_t pid, struct map_info *map_info, int info_count_max)
{
  FILE *fp;
  char maps_path[PATH_MAX];
  char *line;
  size_t n;
  ssize_t readn;
  int count;
  char *path;
  int fd;
  size_t buf_size;
  void *buf;
  int i;

  /* Memory mapping placeholder */
  unsigned long start; /* Start of the region  */
  unsigned long end;   /* End of the region    */
  off_t offset;        /* Offset in the region */
  char x;              /* eXecutable character */
  char c;              /* other characters     */

  /* Open the /proc/<pid>/maps file */
  memset(maps_path, 0, sizeof(maps_path));
  snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
  fp = fopen(maps_path, "r");
  if (fp == NULL) {
    perror("fopen");
    return -1;
  }
  /* Parse memory mappings */
  line = NULL;
  n = 0;
  count = 0;
  while ((readn = getline(&line, &n, fp)) != -1) {
    if (readn > 0 && line[readn - 1] == '\n') {
      line[readn - 1] = '\0';
      readn--;
    }
    /* Extract address range and permission, avoiding non-executable regions */
    sscanf(line, "%lx-%lx %c%c%c%c %lx", &start, &end, &c, &c, &x, &c, &offset);
    if (x != 'x') {
      /* Not an executable region */
      continue;
    }
    /* Too many map_info */
    if (count >= info_count_max) {
      fprintf(stderr, "INFO: [0x%lx-0x%lx] will not be traced\n", start, end);
      continue;
    }
    /* Search absolute path, excluding anonymous mappings */
    path = strchr(line, '/');
    if (!path) {
      continue;
    }
    /* Extract the information in the structure */
    map_info[count].start = start;
    map_info[count].end = end;
    map_info[count].offset = offset;
    map_info[count].buf = NULL;
    strncpy(map_info[count].path, path, PATH_MAX - 1);
    count++;
  }

  /* Cleanup */
  if (line != NULL) {
    free(line);
  }
  fclose(fp);

  for (i = 0; i < count; i++) {
    /* Open the mapped files */
    if ((fd = open(map_info[i].path, O_RDONLY | O_SYNC)) < -1) {
      perror("open");
      return -1;
    }
    /* Mapping the regions into read-only mappings of the executable memory */
    buf_size = (size_t)ALIGN_UP(map_info[i].end - map_info[i].start, PAGE_SIZE);
    buf = mmap(NULL, buf_size, PROT_READ, MAP_PRIVATE, fd, map_info[i].offset);
    if (!buf) {
      perror("mmap");
      close(fd);
      return -1;
    }
    map_info[i].buf = buf;
    close(fd);
  }

  return count;
}
