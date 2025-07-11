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
 * Extern value definitions
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
    perror("[!] Ptrace request traceme failed");
    exit(1);
  }

  /* Redefine LD_PRELOAD to map/unmap the stm region in the process */
  setenv("LD_PRELOAD", "/home/aurora/qtests/coresight/ultrasight/lib/libstm_preload.so",
         1);
  /* Check file existence and executability */
  if (access(argv[0], F_OK) != 0) {
    perror("[!] Tracee program not found");
    exit(1);
  }
  if (access(argv[0], X_OK) != 0) {
    perror("[!] Tracee program not executable");
    exit(1);
  }
  /* execute the traced program, passed as arguments after -- in the main CLI */
  execvpe(argv[0], argv, environ);
}

/**
 * Parent process waiting for the child process to stop, initializing tracing,
 * then sending a CONT signal to the child. When the child stops, cleans up the
 * trace.
 */
void parent(pid_t pid, int *child_status)
{
  int wstatus;
  struct timespec start_time, end_time;
  /** Wait for the child process to stop, specified by the pid.
   *  The status of the child process is stored in wstatus.
   */
  waitpid(pid, &wstatus, 0);
  /* If the child process has stopped due to a vfork() event */
  if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_EVENT_VFORK_DONE) {
    /* Initialize the trace */
    printf("[+] Initializing trace\n");
    init_trace(getpid(), pid);
    /* Start the trace */
    printf("[+] Starting trace\n");
    start_trace(pid, true);
    /* Send a continue ptrace request to the child pid */
    printf("[+] Sending CONT signal to child\n");
    /* Capture the start timestamp */
    clock_gettime(CLOCK_MONOTONIC, &start_time);
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
      /* Capture the end time */
      clock_gettime(CLOCK_MONOTONIC, &end_time);
      if (wstatus == 0) {
        printf("[+] Child exited with status %d, stopping trace\n", wstatus);
        /* Print elapsed time */
        double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                  (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
        printf("[+] Child execution time (traced): %.6f seconds\n", elapsed);

        stop_trace(true);
        printf("[+] Finalizing trace\n");
        fini_trace();
        printf("[+] Done!\n");
        break;
      } else if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGSTOP) {
        trace_suspend_resume_callback();
      } else {
        printf("[~] Child exited with status %d, stopping trace\n", wstatus);
        stop_trace(true);
        printf("[~] Finalizing trace\n");
        fini_trace();
        printf("[~] Done!\n");
        break;
      }

    } else if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGSTOP) {
      trace_suspend_resume_callback();
    }
  }

  /* Store the final status of the child process */
  if (child_status) {
    *child_status = wstatus;
  }
}

/**
 * CLI usage display
 */
static void usage(char *argv0)
{
  fprintf(stderr, "Usage: %s [OPTIONS] -- EXE [ARGS]\n", argv0);
  fprintf(stderr, "CoreSight process tracer\n");
  fprintf(stderr, "[OPTIONS]\n");
  fprintf(stderr, "  -b, --board=NAME\t\tspecify board name (default: %s)\n",
          board_name);
  fprintf(stderr,
          "  -c, --cpu=INT\t\t\tbind traced process to CPU (default: %d)\n",
          trace_cpu);
  fprintf(stderr, "  -e, --export\t\tenable config export (default: %d)\n",
          export_config);
  fprintf(stderr,
          "  -u, --udmabuf=INT\t\tspecify u-dma-buf device number to use "
          "(default: %d)",
          udmabuf_num);
  fprintf(stderr,
          "  -v, --verbose[=INT]\t\tverbose output level (default: %d)\n",
          registration_verbose);
  fprintf(stderr, "  -h, --help\t\t\tshow this help\n");
}

/**
 * Main function, parses the options using getopt, extracts the tracee program
 * and launch it using the parent and child functions
 */
int main(int argc, char *argv[])
{
  const struct option long_options[] = {
      {"board", required_argument, NULL, 'b'},
      {"cpu", required_argument, NULL, 'c'},
      {"export", no_argument, NULL, 'e'},
      {"udmabuf", required_argument, NULL, 'u'},
      {"verbose", optional_argument, NULL, 'v'},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0},
  };

  char **argvp;
  pid_t pid;
  int opt;
  int option_index;

  argvp = NULL;
  registration_verbose = 0;
  trace_bitmap_size = DEFAULT_TRACE_BITMAP_SIZE;

  /* Check argument count */
  if (argc < 3) {
    usage(argv[0]);
    exit(EXIT_SUCCESS);
  }
  /* Parse CLI elements */
  while ((opt = getopt_long(argc, argv, "b:c:e:v::h", long_options,
                            &option_index)) != -1) {
    switch (opt) {
      /* Board name */
      case 'b':
        board_name = optarg;
        break;
      /* CPU number */
      case 'c':
        trace_cpu = atoi(optarg);
        break;
      /* Export to snapshot format */
      case 'e':
        export_config = true;
        break;
      /* udmabuf number */
      case 'u':
        udmabuf_num = atoi(optarg);
        break;
      /* Verbose option */
      case 'v':
        if (optarg) {
          registration_verbose = atoi(optarg);
        } else {
          registration_verbose = 1;
        }
        break;
      /* Help display */
      case 'h':
        usage(argv[0]);
        exit(EXIT_SUCCESS);
        break;
      default:
        break;
    }
  }

  /* Check for missing tracee program */
  if (argc <= optind || strcmp(argv[optind - 1], "--")) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  /* Extract tracee program name */
  argvp = &argv[optind];
  if (!argvp) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  /* For the child and parent program */
  pid = fork();
  switch (pid) {
    case 0:
      child(argvp);
      break;
    case -1:
      perror("fork");
      exit(EXIT_FAILURE);
      break;
    default:
      parent(pid, NULL);
      wait(NULL);
      break;
  }

  return 0;
}
