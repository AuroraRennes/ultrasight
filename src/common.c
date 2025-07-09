
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Quentin Ducasse on 2025-04-23:
 * - Removed decoder related code
 * - Added comments for clarity
 */

/* for mmremap */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>

#include "csaccess.h"
#include "csregistration.h"
#include "csregisters.h"
#include "cs_util_create_snapshot.h"

#include "common.h"
#include "known_boards.h"
#include "config.h"
#include "utils.h"

#define DEFAULT_TRACE_CPU 0
#define DEFAULT_UDMABUF_NUM 0
#define DEFAULT_ETF_SIZE 0x1000
#define DEFAULT_TRACE_SIZE 0x80000
#define DEFAULT_TRACE_NAME "cstrace.bin"

#define TRACE_DISABLE_TRIAL 8
#define TRACE_DISABLE_TRIAL_USLEEP 10

/* Trace states, see set_trace_state to see their relations to events */
typedef enum {
  init_state,
  fini_state,
  ready_state,
  running_state,
  suspended_state,
} trace_state_t;

/* Trace events, see set_trace_state to see their relations to states */
typedef enum {
  init_event,
  fini_event,
  ready_event,
  start_event,
  stop_event,
  suspend_event,
  resume_event,
} trace_event_t;

/* Board info */
char *board_name = "ZCU-104";
const struct board *board;
struct cs_devices_t devices;

int udmabuf_num = DEFAULT_UDMABUF_NUM;
int trace_cpu = -1;
bool export_config = false;
unsigned long etr_ram_addr = 0;
size_t etr_ram_size = 0;
int range_count = 0;
struct map_info map_info[RANGE_MAX];

bool fifo_sw = false;

int fd = 0;
void *map_base = NULL;

unsigned char *trace_bitmap = NULL;
unsigned int trace_bitmap_size = 0;

static int trace_id = -1;
static pid_t child_pid = -1;
static bool is_first_trace = true;
static void *trace_buf = NULL;
static size_t trace_buf_size = 0;
static void *trace_buf_ptr = NULL;

static pthread_mutex_t trace_mutex;
static pthread_mutex_t trace_state_mutex;
static pthread_mutex_t trace_event_mutex;
static pthread_cond_t trace_event_cond;
static trace_state_t trace_state = init_state;
static trace_event_t trace_event = init_event;

extern int registration_verbose;

static int enable_cs_trace(pid_t pid);
static int disable_cs_trace(bool disable_all);

/* TODO: Put it elsewhere... */
int get_trace_id(int cpu)
{
  /* FIXME: Where is this value coming from? */
  return 0x10 + cpu;
}

/**
 * Update trace event and notify
 */
static void signal_trace_event(trace_event_t event)
{
  pthread_mutex_lock(&trace_event_mutex);
  trace_event = event;
  pthread_cond_broadcast(&trace_event_cond);
  pthread_mutex_unlock(&trace_event_mutex);
}

/**
 * Wait for a given trace event
 */
static void wait_trace_event(trace_event_t event)
{
  pthread_mutex_lock(&trace_event_mutex);
  while (trace_event != event) {
    pthread_cond_wait(&trace_event_cond, &trace_event_mutex);
  }
  pthread_mutex_unlock(&trace_event_mutex);
}

/**
 * Change the trace state in a thread-safe manner
 */
static void set_trace_state(trace_state_t new_state)
{
  trace_state_t old_state;
  /* Hold the mutex and extract the old state */
  pthread_mutex_lock(&trace_state_mutex);
  old_state = trace_state;
  trace_state = new_state;
  /** Follow the following finite state machine
   *      From       |       To        |  Signals
   * ----------------|-----------------|---------------
   * init_state      | any             | init_event
   * any             | fini_state      | fini_event
   * any             | ready_state     | stop_event
   * ready_state     | running_state   | start_event
   * running_state   | suspended_state | suspend_event
   * suspended_state | running_state   | resume_event
   */
  if (old_state == init_state) {
    signal_trace_event(init_event);
  } else if (new_state == fini_state) {
    signal_trace_event(fini_event);
  } else if (new_state == ready_state) {
    signal_trace_event(stop_event);
  } else if (old_state == ready_state && new_state == running_state) {
    signal_trace_event(start_event);
  } else if (old_state == running_state && new_state == suspended_state) {
    signal_trace_event(suspend_event);
  } else if (old_state == suspended_state && new_state == running_state) {
    signal_trace_event(resume_event);
  } else {
    fprintf(stderr, "Unexpected trace state transition: %d -> %d\n", old_state,
            new_state);
  }
  pthread_mutex_unlock(&trace_state_mutex);
}

/**
 * Allocate the trace buffer through mmap
 */
static int alloc_trace_buf(void)
{
  trace_buf = mmap(NULL, DEFAULT_TRACE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (trace_buf == MAP_FAILED) {
    fprintf(stderr, "[!] mmap failed when allocating trace buffer\n");
    return -1;
  }
  /* FIXME: Do not initialize global variables in the function */
  trace_buf_size = DEFAULT_TRACE_SIZE;
  trace_buf_ptr = trace_buf;
  return 0;
}

/**
 * Free trace buffer, emptying the device.etb then unmapping the corresponding
 * buffer.
 */
static void free_trace_buf(void)
{
  /* FIXME: Make it better */
  if (devices.etb) {
    cs_empty_trace_buffer(devices.etb);
  }

  if (trace_buf) {
    munmap(trace_buf, trace_buf_size);
    trace_buf_ptr = NULL;
  }
}

/**
 * Export trace to file name "cwd/trace_name"
 */
static int export_trace(const char *trace_name)
{
  int ret;
  char *cwd;
  char trace_path[PATH_MAX];
  FILE *fp;

  ret = -1;

  /* Get the current directory */
  cwd = getcwd(NULL, 0);
  if (!cwd) {
    perror("getcwd");
    goto exit;
  }
  /* Construct the file path "cwd/trace_name" */
  memset(trace_path, 0, sizeof(trace_path));
  snprintf(trace_path, sizeof(trace_path), "%s/%s", cwd, trace_name);
  /* Open file, write the trace buffer and close it */
  fp = fopen(trace_path, "wb");
  if (!fp) {
    perror("fopen");
    goto exit;
  }
  fwrite(trace_buf, (size_t)((char *)trace_buf_ptr - (char *)trace_buf), 1, fp);
  fclose(fp);
  ret = 0;
exit:
  if (cwd) {
    free(cwd);
  }
  return ret;
}

/**
 * Enable the CoreSight trace, locking the mutex
 */
static int enable_cs_trace(pid_t pid)
{
  int ret;
  ret = -1;

  /* Acquire the trace mutex */
  pthread_mutex_lock(&trace_mutex);

  if (is_first_trace) {
    /* Do not specify traced PID in forkserver mode */
    if (configure_trace(board, &devices, map_info, range_count, pid) < 0) {
      fprintf(stderr, "configure_trace() failed\n");
      goto exit;
    }
    /* Enable ETMs and trace sinks for the first time */
    if (enable_trace(board, &devices) < 0) {
      fprintf(stderr, "enable_trace() failed\n");
      goto exit;
    }
    is_first_trace = false;
  } else {
    /* Enable trace sinks only once the ETMs enabled */
    if (enable_trace_sinks_only(&devices) < 0) {
      fprintf(stderr, "enable_trace_sinks_only() failed\n");
      goto exit;
    }
  }

  /* Export the config in snapshot format if needed */
  if (export_config) {
    do_dump_config(board, &devices, 1);
  }

  ret = 0;

exit:
  /* Shutdown properly if configuration or enable failed */
  if (ret < 0) {
    cs_shutdown();
  }
  /* Release trace mutex */
  pthread_mutex_unlock(&trace_mutex);

  return ret;
}

/**
 * Disable CoreSight trace, retries several times before giving up
 */
static int disable_cs_trace(bool disable_all)
{
  int ret;
  int disable_trial;

  /* Acquire trace mutex */
  pthread_mutex_lock(&trace_mutex);

  /* Tries to disable TRACE_DISABLE_TRIAL times before failing */
  disable_trial = 0;
  while (disable_trial++ < TRACE_DISABLE_TRIAL) {
    if (disable_all) {
      if ((ret = disable_trace(board, &devices)) < 0) {
        fprintf(stderr, "disable_trace() failed\n");
      }
    } else {
      if ((ret = disable_trace_sinks_only(&devices)) < 0) {
        fprintf(stderr, "disable_trace_sinks_only() failed\n");
      }
    }

    /* If there is no error, break out of the trial loop */
    if (!(ret < 0)) {
      break;
    }

    /* Sleep for TRACE_DISABLE_TRIAL_USLEEP before next try */
    usleep(TRACE_DISABLE_TRIAL_USLEEP);
    /* Reset error count */
    cs_reset_error_count();
  }

  /* Release the trace mutex */
  pthread_mutex_unlock(&trace_mutex);

  return ret;
}

// void *fetch_trace_sw(void *arg)
// {
//   long int count = 0;
//   fprintf(stderr, "launched.\n");
//   cs_device_t d = devices.trace_sinks[0];
//   unsigned int reg = _cs_read(d, CS_ETB_STATUS);
//   fprintf(stderr, "Status: %b\n", reg);
//   reg = _cs_read(d, CS_ETB_CTRL);
//   fprintf(stderr, "CTRL: %b\n", reg);
//   reg = _cs_read(d, CS_ETB_RAM_MODE);
//   fprintf(stderr, "MODE: %b\n", reg);
//   reg = _cs_read(d, CS_LSR);
//   fprintf(stderr, "lock: %b\n", reg);
//   reg = _cs_read(d, CS_ETMOSLAR);
//   fprintf(stderr, "FFSR: %b\n", reg);
//   reg = _cs_read(d, CS_ETMOSLSR);
//   fprintf(stderr, "FFCR: %b\n", reg);

//   _cs_unlock(d);  // important!
//   reg = _cs_read(d, CS_LSR);
//   fprintf(stderr, "lock: %b\n", reg);

//   d = devices.ptm[0];

//   for (int cpu = 0; cpu < 4; cpu++) {
//     fprintf(stderr, "Source config cpu %d:\n", cpu);
//     vslog(d, CS_ETMV4_PRGCTLR, "CS_ETMV4_PRGCTLR");
//     vslog(d, CS_ETMV4_STATR, "CS_ETMV4_STATR");
//     vslog(d, CS_ETMV4_CONFIGR, "CS_ETMV4_CONFIGR");
//     vslog(d, CS_ETMV4_BBCTLR, "CS_ETMV4_BBCTLR");
//     vslog(d, CS_ETMV4_STATR, "CS_ETMV4_STATR");
//     vslog(d, CS_ETMV4_TRACEIDR, "CS_ETMV4_TRACEIDR");
//     vslog(d, CS_ETMV4_ACVR(0), "CS_ETMV4_ACVR(0)");
//     vslog(d, CS_ETMV4_ACVR(1), "CS_ETMV4_ACVR(1)");
//     vslog(d, CS_ETMV4_ACATR(0), "CS_ETMV4_ACATR(0)");  // several!
//     vslog(d, CS_ETMV4_CIDCVR(0), "CS_ETMV4_CIDCVR(0)");
//     d = devices.trace_sinks[0];
//   }

//   bool running = true;
//   while (running) {
//     unsigned int x = _cs_read(d, CS_ETB_RAM_DATA);
//     fprintf(stdout, "%08x\n", x);
//     count++;
//     if (x == 0xFFFFFFFF) {
//       if (_cs_isset(d, CS_ETB_FLFMT_CTRL, CS_ETB_FLFMT_CTRL_StopFl)) {
//         break;
//       }

//       unsigned int reg = _cs_read(d, CS_ETB_STATUS);
//       fprintf(stderr, "Status: %08b\n", reg);
//     } else {
//     }
//   }

//   return NULL;
// }

/**
 * Fetch the trace data from the ETB
 */
int fetch_trace(void)
{
  int ret;
  cs_device_t etb;
  int len;
  size_t buf_remain;
  void *new_trace_buf;
  size_t new_trace_buf_size;
  int n;

  ret = -1;

  /* Acquire trace mutex */
  pthread_mutex_lock(&trace_mutex);

  /* Get the number of bytes that have not yet been destructively read from the
   * buffer */
  etb = devices.etb;
  len = cs_get_buffer_unread_bytes(etb);

  /* Align the value of the new trace pointer */
  trace_buf_ptr = (void *)ALIGN_UP((unsigned long)trace_buf_ptr, 0x8);

  /* Compute the remaining space in the buffer */
  buf_remain =
      trace_buf_size - (size_t)((char *)trace_buf_ptr - (char *)trace_buf);
  /* If there is no space left, buffer size is doubled and remapped */
  while ((size_t)len > buf_remain) {
    new_trace_buf_size = trace_buf_size * 2;
    new_trace_buf = mremap(trace_buf, trace_buf_size, new_trace_buf_size, MREMAP_MAYMOVE);

    /* Check for a remap error */
    if (new_trace_buf == (void *) -1) {
      fprintf(stderr, "[!] mremap call failed when resizing trace buffer\n");
      goto exit;
    }
    /* Adjust new trace buffer pointers and values */
    trace_buf_ptr = (void *)((char *)new_trace_buf +
                             ((char *)trace_buf_ptr - (char *)trace_buf));
    trace_buf = new_trace_buf;
    trace_buf_size = new_trace_buf_size;
    buf_remain = trace_buf_size - ((char *)trace_buf_ptr - (char *)trace_buf);
  }

  /* Get the trace data */
  n = cs_get_trace_data(etb, trace_buf_ptr, buf_remain);
  if (n <= 0) {
    fprintf(stderr, "[!] Failed to get trace\n");
  } else if (n < len) {
    fprintf(stderr, "[!] Got incomplete trace\n");
  }
  /* Empty the trace buffer, resetting read and write pointers */
  cs_empty_trace_buffer(etb);
  trace_buf_ptr = (void *)((char *)trace_buf_ptr + n);

  ret = 0;

exit:
  /* Release trace mutex */
  pthread_mutex_unlock(&trace_mutex);
  return ret;
}

/**
 * Set the state of the trace to suspended
 */
void trace_suspend_resume_callback(void) { set_trace_state(suspended_state); }

/**
 * Start a trace session. CoreSight and decoder must be initialized.
 */
int start_trace(pid_t pid, bool use_pid_trace)
{
  int ret;
  /* Set the cpu affinity, binding the process to the corresponding cpu */
  if ((ret = set_cpu_affinity(trace_cpu, pid)) < 0) {
    fprintf(stderr, "set_cpu_affinity() failed\n");
    goto exit;
  }

  /* Allocate the trace buffer */
  alloc_trace_buf();

  /* Enable the CoreSight trace */
  child_pid = pid;
  if ((ret = enable_cs_trace(use_pid_trace ? pid : 0)) < 0) {
    fprintf(stderr, "enable_cs_trace() failed\n");
    goto exit;
  }

  //   if (fifo_sw) {
  //     ret = pthread_create(&decoder_thread, NULL, fetch_trace_sw, NULL);
  //     if (ret != 0) {
  //       fprintf(stderr, "pthread_create() failed: %d\n", ret);
  //       goto exit;
  //     } else {
  //       fprintf(stderr, "Started sw-fetcher\n");
  //     }
  //   }
  //   sleep(5);

  /* Set the trace to running, effectively launching collection */
  set_trace_state(running_state);

exit:
  return ret;
}

/**
 * Stop the trace session. CoreSight and decoder are still available.
 */
int stop_trace(bool disable_all)
{
  int ret;

  /* Disable all components */
  if ((ret = disable_cs_trace(disable_all)) < 0) {
    fprintf(stderr, "disable_cs_trace() failed\n");
    goto exit;
  }

  /* Set the trace to ready */
  set_trace_state(ready_state);

exit:
  return ret;
}

/**
 * Initialize trace. Called on the first time and only once.
 */
int init_trace(pid_t parent_pid, pid_t pid)
{
  int ret;
  int preferred_cpu;

  ret = -1;

  /* Initialize mutexes and condition variables */
  pthread_mutex_init(&trace_mutex, NULL);
  pthread_mutex_init(&trace_state_mutex, NULL);
  pthread_mutex_init(&trace_event_mutex, NULL);
  pthread_cond_init(&trace_event_cond, NULL);

  /* If the trace cpu is not set, tries to link the parent pid to its preferred
   * CPU (if there is no, use the first one)*/
  if (trace_cpu < 0) {
    if ((preferred_cpu = get_preferred_cpu(parent_pid)) < 0) {
      fprintf(stderr, "INFO: Failed to get preferred CPU\n");
      /* Some boards is not supported by get_preferred_cpu() */
      if ((preferred_cpu = find_free_cpu() < 0)) {
        fprintf(stderr, "WARNING: Failed to find free CPU. Use #%d\n",
                DEFAULT_TRACE_CPU);
      }
    }
    trace_cpu = preferred_cpu >= 0 ? preferred_cpu : DEFAULT_TRACE_CPU;
  }

  /* Get udmabuf information (address and size), storing them in their
   * respective variables */
  if (get_udmabuf_info(udmabuf_num, &etr_ram_addr, &etr_ram_size) < 0) {
    fprintf(stderr, "Failed to get u-dma-buf info\n");
    goto exit;
  }

  /* Extract and store memory mapping information */
  if ((range_count = setup_map_info(pid, map_info, RANGE_MAX)) < 0) {
    fprintf(stderr, "setup_map_info() failed\n");
    goto exit;
  }

  /* Setup board variables for a given board defined in known_board.h */
  if (setup_named_board(board_name, &board, &devices, known_boards) < 0) {
    fprintf(stderr, "setup_named_board() failed\n");
    goto exit;
  }

  /** /!\ NOTE: This part was intended to setup the STM region mapping through
   * the parent but the mapping is not accessible from the child process. It has
   * been moved to a preload library instead.
   */

  /* Setup STM region */
#if 0
  if (setup_stm_region(&fd, map_base) < 0) {
    fprintf(stderr, "setup_stm_region() failed\n");
    goto exit;
  }
#endif

  /* Get the trace ID */
  if ((trace_id = get_trace_id(trace_cpu)) < 0) {
    goto exit;
  }

  /* Mark the trace as ready */
  set_trace_state(ready_state);
  ret = 0;

exit:
  /* If any of the above steps failed, run the shutdown function */
  if (ret != 0) {
    cs_shutdown();
  }

  return ret;
}

/**
 * Finalize trace. Called after all trace sessions finished.
 */
void fini_trace(void)
{
  /* Fetch the trace in the buffer */
  fetch_trace();

  /* Export the trace to a file */
  export_trace(DEFAULT_TRACE_NAME);

  /* If needed, dump memory mappings to stderr */
  if (registration_verbose > 0) {
    dump_map_info(stderr, map_info, range_count);
  }

  /* Free the trace buffer */
  free_trace_buf();

  /* Shutdown all CoreSight components */
  cs_shutdown();

  /** /!\ NOTE: This part was intended to unmap the STM region mapping through
   * the parent but the mapping is not accessible from the child process. It has
   * been moved to a preload library instead.
   */

  /* Cleanup the STM region */
  clean_stm_region(&fd, map_base);

  /* Destroy mutexes and conditional variables */
  pthread_cond_destroy(&trace_event_cond);
  pthread_mutex_destroy(&trace_event_mutex);
  pthread_mutex_destroy(&trace_state_mutex);
  pthread_mutex_destroy(&trace_mutex);
}
