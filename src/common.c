
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

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

