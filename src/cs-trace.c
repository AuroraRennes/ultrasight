/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <getopt.h>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "common.h"
#include "config.h"

/**
Extern value definitions
*/
#define DEFAULT_TRACE_BITMAP_SIZE_POW2 (16)
#define DEFAULT_TRACE_BITMAP_SIZE (1U << (DEFAULT_TRACE_BITMAP_SIZE_POW2))
extern int registration_verbose;
extern char *board_name;
extern bool export_config;
extern int udmabuf_num;
extern int trace_cpu;
extern unsigned char *trace_bitmap;
extern unsigned int trace_bitmap_size;

/**
* Main logic, child is the traced program, parent controls the setup and
* launches it.
*/
void child(char *argv[])
{
  long ret;
  /* ptrace request from the tracee (this process) to process 0 */
  ret = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
  if (ret < 0) {
    perror("ptrace");
  }
  /* execute the traced program, passed as arguments after -- in the main CLI */
  execvp(argv[0], argv);
}

/**
* Parent process waiting for the child process to stop, initializing tracing,
* then sending a CONT signal to the child. When the child stops, cleans up the
* trace.
*/
void parent(pid_t pid, int *child_status)
{
  int wstatus;
  /** Wait for the child process to stop, specified by the pid.
   *  The status of the child process is stored in wstatus.
   */
  waitpid(pid, &wstatus, 0);
  /* If the child process has stopped due to a vfork() event */
  if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_EVENT_VFORK_DONE) {
    /* Initialize the trace */
    printf("Initializing trace\n");
    init_trace(getpid(), pid);
    /* Start the trace */
    printf("Starting trace\n");
    start_trace(pid, true);
    /* Send a continue ptrace request to the child pid */
    printf("Sending CONT signal to child\n");
    ptrace(PTRACE_CONT, pid, NULL, NULL);
  }

  while (1) {
    /* Wait for the child process to stop */
    waitpid(pid, &wstatus, 0);
    /** If the child process exited normally, stop and finalize the trace before
     * breaking from the loop else, if it was stopped using SIGSTOP, the
     * function triggers the callback function.
     */
    if (WIFEXITED(wstatus)) {
      printf("Child exited with status %d, stopping trace\n", wstatus);
      stop_trace(true);
      printf("Finalizing trace\n");
      fini_trace();
      printf("Done!\n");
      break;
    } else if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGSTOP) {
      trace_suspend_resume_callback();
    }
  }

  /* Store the final status of the child process */
  if (child_status) {
    *child_status = wstatus;
  }
}

