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
#include "decoder_stats.h"
#include "edge_extractor.h"
#include "bitmap_dma.h"
#include "decoder_axi.h"

/**
 * Extern value definitions
 */
#define DEFAULT_TRACE_BITMAP_SIZE_POW2 (16)
#define DEFAULT_TRACE_BITMAP_SIZE (1U << (DEFAULT_TRACE_BITMAP_SIZE_POW2))
extern int registration_verbose;
extern bool use_etr;
extern bool use_stm;
extern bool teardown_etf;
extern bool single_cpu;
extern bool branch_broadcast;
extern char *board_name;
extern bool export_config;
extern bool fetcher_on;
extern bool no_trace;
extern int udmabuf_num;
extern bool ksight_on;
extern int trace_cpu;
extern unsigned char *trace_bitmap;
extern unsigned int trace_bitmap_size;

/* CSV stats logging, off by default; set via -o/--csv=PATH. Appends one
 * row per run so a benchmark suite can be run as repeated cs-trace
 * invocations and compared afterwards. */
static char *stats_csv_path = NULL;

/* Edge-hash mode selected on the command line. cs-trace sets this explicitly
 * rather than inheriting edge_extractor's reset default, so a change to that
 * default cannot silently alter what a sweep measures. STALKER matches the
 * hardware default and every sweep taken so far; fuzzsight-proxy selects
 * FUZZSIGHT. The mode only decides which index an edge maps to -- edge counts
 * are identical across modes -- but it is recorded in the CSV so a result set
 * says which one produced it. */
static edge_hash_mode_t edge_hash_mode = EDGE_HASH_STALKER;

static const char *edge_hash_mode_name(edge_hash_mode_t m)
{
  switch (m) {
    case EDGE_HASH_NONE:      return "none";
    case EDGE_HASH_FUZZSIGHT: return "fuzzsight";
    case EDGE_HASH_STALKER:   return "stalker";
    default:                  return "unknown";
  }
}

/**
 * Appends one row of ETM decoder / edge / decoder-AXI stats to the CSV at
 * `path`, writing a header line first if the file doesn't exist yet.
 */
static void write_stats_csv(const char *path, const char *binary_name,
                             decoder_stats_t *etm, decoder_axi_t *axi,
                             edge_extractor_t *edge,
                             double child_s, double instr_s, double global_s)
{
  int need_header = (access(path, F_OK) != 0);
  FILE *f = fopen(path, "a");
  if (!f) {
    perror("[!] Could not open stats CSV for append");
    return;
  }
  if (need_header) {
    fprintf(f,
            "binary,child_time_s,instr_time_s,global_time_s,"
            "edges_total,edges_fifo_overflow,edges_freeze_drop,hash_mode,"
            DECODER_STATS_CSV_HEADER "," DECODER_AXI_CSV_HEADER "\n");
  }
  fprintf(f, "%s,%.6f,%.6f,%.6f,%" PRIu64 ",%u,%u,%s,",
          binary_name, child_s, instr_s, global_s,
          edge_extractor_read_edges_total(edge),
          edge_extractor_read(edge, EDGE_EXTRACTOR_OVERFLOW),
          edge_extractor_read(edge, EDGE_EXTRACTOR_FREEZE_DROP),
          /* read back from the hardware, not the requested value, so a
             rejected or clobbered write shows up in the data */
          edge_hash_mode_name(edge_extractor_get_hash_mode(edge)));
  decoder_stats_write_csv_row(etm, f);
  fprintf(f, ",");
  decoder_axi_write_csv_row(axi, f);
  fprintf(f, "\n");
  fclose(f);
}

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
  printf("[+] Child: TRACEME setup\n");

  /* Redefine LD_PRELOAD to map/unmap the stm region in the process */
  // setenv("LD_PRELOAD", "/home/aurora/qtests/coresight/ultrasight/lib/libstm_preload.so", 1);
  /* Check file existence and executability */
  if (access(argv[0], F_OK) != 0) {
    perror("[!] Tracee program not found");
    exit(1);
  }
  if (access(argv[0], X_OK) != 0) {
    perror("[!] Tracee program not executable");
    exit(1);
  }

  printf("[+] child: launching program with execve\n");
  /* execute the traced program, passed as arguments after -- in the main CLI */
  execvpe(argv[0], argv, environ);
}

/**
 * Parent process waiting for the child process to stop, initializing tracing,
 * then sending a CONT signal to the child. When the child stops, cleans up the
 * trace.
 */
void parent(pid_t pid, int *child_status, const char *binary_name)
{
  int ret;
  int wstatus;
  struct timespec global_start, global_end;
  struct timespec instr_start, instr_end;
  struct timespec child_start, child_end;
  double child_elapsed = 0.0, instr_elapsed = 0.0, global_elapsed = 0.0;
  decoder_stats_t dec_stats_etm_handle, dec_stats_stm_handle;
  edge_extractor_t edge_handle;
  bitmap_dma_t dma_handle;
  decoder_axi_t dec_axi_handle;

  /* Global timer starts before anything, including the initial waitpid */
  clock_gettime(CLOCK_MONOTONIC, &global_start);

  waitpid(pid, &wstatus, 0);
  /* If the child process has stopped due to a vfork() event */
  if (WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == SIGTRAP) {
    if (no_trace) {
      /* Baseline mode: ptrace attach/detach only, no tracing infrastructure */
      printf("[+] No-trace mode: detaching immediately\n");
      clock_gettime(CLOCK_MONOTONIC, &child_start);
      ptrace(PTRACE_DETACH, pid, NULL, NULL);
    } else {
      /* Instrumentation setup timer starts here */
      clock_gettime(CLOCK_MONOTONIC, &instr_start);

      printf("[+] Initializing trace\n");
      init_trace(getpid(), pid);
      if (ksight_on) {
        printf("[+] Enabling ksight tracing (pid %d)\n", pid);
        ksight_set_traced_pid(pid);
        ksight_set_enable(1);
      }

      ret = decoder_stats_open(&dec_stats_etm_handle);
      if (ret < 0) perror("[!] ETM AXI stats mapping issue");

      ret = edge_extractor_open(&edge_handle);
      if (ret < 0) perror("[!] EDGE AXI stats mapping issue");
      /* Select the hash mode */
      edge_extractor_set_hash_mode(&edge_handle, edge_hash_mode);
      {
        /* Confirm the write landed */
        edge_hash_mode_t got = edge_extractor_get_hash_mode(&edge_handle);
        if (got != edge_hash_mode)
          fprintf(stderr,
                  "[!] Edge hash mode not applied: asked for %s, hardware reports %s. "
                  "The loaded bitstream likely predates the hash-mode register; "
                  "the CSV records what the hardware reports.\n",
                  edge_hash_mode_name(edge_hash_mode), edge_hash_mode_name(got));
      }

      ret = bitmap_dma_open(&dma_handle, DEFAULT_TRACE_BITMAP_SIZE);
      if (ret < 0) perror("[!] Bitmap DMA setup issue");

      ret = decoder_axi_open(&dec_axi_handle);
      if (ret < 0) perror("[!] Decoder AXI errors setup issue");

      // decoder_axi_soft_reset(&dec_axi_handle);
      decoder_stats_enable(&dec_stats_etm_handle);
      edge_extractor_reset(&edge_handle);
      decoder_axi_stats_reset(&dec_axi_handle);
      bitmap_dma_transfer(&dma_handle); // Clearing DMA

      /* Enable the ETM as late as possible as it emits its one and only
       * A-sync when it is enabled (syncpr = 0 disables periodic sync) */
      printf("[+] Starting trace for pid: %d\n", pid);
      ret = start_trace(pid, true);
      if (ret < 0) {
        perror("[!] Trace could not start");
      }

      printf("[+] Sending CONT signal to child\n");

      /* Child timer starts as child is released */
      clock_gettime(CLOCK_MONOTONIC, &child_start);
      ptrace(PTRACE_DETACH, pid, NULL, NULL);
    } /* end if/else no_trace */
  }

  while (1) {
    /* Wait for the child process to stop */
    waitpid(pid, &wstatus, WUNTRACED | WCONTINUED);

    /** If the child process exited normally, stop and finalize the trace before
     * breaking from the loop else, if it was stopped using SIGSTOP, the
     * function triggers the callback function.
     */
    if (WIFEXITED(wstatus)) {
      /* Child timer ends the moment the child exits */
      clock_gettime(CLOCK_MONOTONIC, &child_end);

      if (wstatus == 0) {
        printf("[+] Child exited with status %d\n", wstatus);

        if (!no_trace) {
          if (ksight_on) {
            printf("[+] Disabling ksight tracing\n");
            ksight_set_enable(0);
            ksight_set_traced_pid(0);
          }

          printf("[+] Stopping and cleaning up trace\n");
          stop_trace(true);
          fini_trace();
          printf("[+] Done!\n");

          decoder_stats_disable(&dec_stats_etm_handle);

          printf("========== DEC STATS ============\n");
          decoder_stats_print(&dec_stats_etm_handle);
          printf("============= EDGES =============\n");
          edge_extractor_print(&edge_handle);
          printf("============ DEC ERR ============\n");
          decoder_axi_print(&dec_axi_handle);

          if (stats_csv_path) {
            struct timespec csv_now;
            clock_gettime(CLOCK_MONOTONIC, &csv_now);
            double csv_child_s = (child_end.tv_sec - child_start.tv_sec) +
                                  (child_end.tv_nsec - child_start.tv_nsec) / 1e9;
            /* instr/global here are measured a little before the "official"
             * timers below (which also cover the DMA readout that follows),
             * so they'll read a touch lower -- fine for stats comparison. */
            double csv_instr_s = (csv_now.tv_sec - instr_start.tv_sec) +
                                  (csv_now.tv_nsec - instr_start.tv_nsec) / 1e9;
            double csv_global_s = (csv_now.tv_sec - global_start.tv_sec) +
                                   (csv_now.tv_nsec - global_start.tv_nsec) / 1e9;
            write_stats_csv(stats_csv_path, binary_name, &dec_stats_etm_handle,
                             &dec_axi_handle, &edge_handle,
                             csv_child_s, csv_instr_s, csv_global_s);
          }

          printf("============== DMA ==============\n");
          printf("[+] Triggering bitmap DMA readout\n");
          struct timespec dma_start, dma_end;
          clock_gettime(CLOCK_MONOTONIC, &dma_start);
          ret = bitmap_dma_transfer(&dma_handle);
          clock_gettime(CLOCK_MONOTONIC, &dma_end);
          double dma_elapsed_us = (dma_end.tv_sec - dma_start.tv_sec) * 1e6 +
                                  (dma_end.tv_nsec - dma_start.tv_nsec) / 1e3;
          if (ret < 0) perror("[!] Bitmap DMA transfer failed");
          printf("[+] Bitmap DMA complete in %.2f us, %zu bytes in udmabuf\n",
                 dma_elapsed_us, dma_handle.buf_size);

          printf("[+] Bitmap non-zero entries:\n");
          unsigned char *bmap = (unsigned char *)dma_handle.buf;
          for (size_t i = 0; i < dma_handle.buf_size; i++) {
              if (bmap[i] != 0) {
                  printf("  [0x%04zx] = 0x%02x\n", i, bmap[i]);
              }
          }

          printf("============= DMA 2 =============\n");
          printf("[+] Triggering bitmap DMA readout\n");
          ret = bitmap_dma_transfer(&dma_handle);
          printf("[+] Bitmap non-zero entries:\n");
          bmap = (unsigned char *)dma_handle.buf;
          for (size_t i = 0; i < dma_handle.buf_size; i++) {
              if (bmap[i] != 0) {
                  printf("  [0x%04zx] = 0x%02x\n", i, bmap[i]);
              }
          }


          decoder_stats_close(&dec_stats_etm_handle);
          edge_extractor_close(&edge_handle);
          decoder_axi_close(&dec_axi_handle);

          /* Instrumentation timer ends after full teardown */
          clock_gettime(CLOCK_MONOTONIC, &instr_end);
          instr_elapsed = (instr_end.tv_sec - instr_start.tv_sec) +
                          (instr_end.tv_nsec - instr_start.tv_nsec) / 1e9;
        }

        /* Global timer ends after everything */
        clock_gettime(CLOCK_MONOTONIC, &global_end);

        child_elapsed = (child_end.tv_sec - child_start.tv_sec) +
                        (child_end.tv_nsec - child_start.tv_nsec) / 1e9;
        global_elapsed = (global_end.tv_sec - global_start.tv_sec) +
                         (global_end.tv_nsec - global_start.tv_nsec) / 1e9;

        /* Print all timers together */
        printf("============= TIMING ============\n");
        printf("[+] Child execution time:          %.6f seconds\n", child_elapsed);
        if (!no_trace) {
          printf("[+] Instrumentation time:          %.6f seconds\n", instr_elapsed);
          printf("[+] Instrumentation overhead:      %.6f seconds\n", instr_elapsed - child_elapsed);
          printf("[+] Fork/exec/waitpid stall:       %.6f seconds\n", global_elapsed - instr_elapsed);
        } else {
          printf("[+] Fork/exec/waitpid stall:       %.6f seconds\n", global_elapsed - child_elapsed);
        }
        printf("[+] Global time:                   %.6f seconds\n", global_elapsed);

        break;
      }
    } else if (WIFCONTINUED(wstatus)) {
      // printf("[+] Child resumed by kernel\n");
    } else if (WIFSTOPPED(wstatus)) {
      int sig = WSTOPSIG(wstatus);
      /* If the fetcher is setup and uses ptrace, all signals are stops */
      if (fetcher_on) {
        if (sig == SIGSTOP) {
          printf("[~] Fetcher: Trying to stop child\n");
          trace_suspend_callback();
        } else if (sig == SIGCONT) {
          printf("[~] Fetcher: Trying to resume child\n");
          trace_resume_callback();
        }
      } else {
        if (sig == SIGSTOP) {
          // printf("[+] Child stopped by kernel\n");
        }
      }
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
  fprintf(stderr, "  -f, --fetcher\t\tenable fetcher worker (default: %d)\n",
          fetcher_on);
  fprintf(stderr,
          "  -u, --udmabuf=INT\t\tspecify u-dma-buf device number to use "
          "(default: %d)",
          udmabuf_num);
  fprintf(stderr, "  -k, --ksight\t\tenable ksight kernel tag events tracing (default: %d)\n",
          ksight_on);
  fprintf(stderr, "  -r, --useetr\t\tuse the ETR sink (in SDRAM), (default %d)\n",
          ksight_on);
  fprintf(stderr, "  -s, --usestm\t\tenable STM/ITM tracing (default %d)\n",
          use_stm);
  fprintf(stderr, "  -t, --teardownetf\t\tdisable ETFs on trace teardown (default %d)\n",
          teardown_etf);
  fprintf(stderr, "  -m, --singlecpu\t\tonly enable/disable the traced CPU's ETM, not every CPU (default %d)\n",
          single_cpu);
  fprintf(stderr, "  -a, --branchbroadcast\t\tenable ETM branch broadcasting (default %d)\n",
          branch_broadcast);
  fprintf(stderr,
          "  -v, --verbose[=INT]\t\tverbose output level (default: %d)\n",
          registration_verbose);
  fprintf(stderr, "  -n, --no-trace\t\tdisable tracing (default: %d)\n",
          no_trace);
  fprintf(stderr,
          "  -o, --csv=PATH\t\tappend decoder/edge stats as a CSV row to "
          "PATH (default: disabled)\n");
  fprintf(stderr, "  -g, --hashmode=MODE\t\tedge hash mode: none, fuzzsight or "
                  "stalker (default %s)\n", edge_hash_mode_name(edge_hash_mode));
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
      {"fetcher", no_argument, NULL, 'f'},
      {"udmabuf", required_argument, NULL, 'u'},
      {"ksight", no_argument, NULL, 'k'},
      {"useetr", required_argument, NULL, 'r'},
      {"usestm", required_argument, NULL, 's'},
      {"teardownetf", required_argument, NULL, 't'},
      {"singlecpu", required_argument, NULL, 'm'},
      {"branchbroadcast", required_argument, NULL, 'a'},
      {"verbose", optional_argument, NULL, 'v'},
      {"notrace", optional_argument, NULL, 'n'},
      {"csv", required_argument, NULL, 'o'},
      {"hashmode", required_argument, NULL, 'g'},
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
  while ((opt = getopt_long(argc, argv, "b:c:e:f:u:k:r:s:t:m:a:v:n::o:g:h", long_options,
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
      case 'f':
        fetcher_on = true;
        break;
      /* udmabuf number */
      case 'u':
        udmabuf_num = atoi(optarg);
        break;
      case 'k':
        ksight_on = true;
        break;
      case 'r':
        use_etr = atoi(optarg);
        break;
      case 's':
        use_stm = atoi(optarg);
        break;
      case 't':
        teardown_etf = atoi(optarg);
        break;
      case 'm':
        single_cpu = atoi(optarg);
        break;
      case 'a':
        branch_broadcast = atoi(optarg);
        break;
      /* Verbose option */
      case 'v':
        if (optarg) {
          registration_verbose = atoi(optarg);
        } else {
          registration_verbose = 1;
        }
        break;
      /* No trace option */
      case 'n':
        no_trace = true;
        break;
      /* CSV stats output path */
      case 'o':
        stats_csv_path = optarg;
        break;
      /* Help display */
      case 'g':
        if (!strcmp(optarg, "none"))
          edge_hash_mode = EDGE_HASH_NONE;
        else if (!strcmp(optarg, "fuzzsight"))
          edge_hash_mode = EDGE_HASH_FUZZSIGHT;
        else if (!strcmp(optarg, "stalker"))
          edge_hash_mode = EDGE_HASH_STALKER;
        else {
          fprintf(stderr, "[!] Unknown hash mode '%s' "
                          "(expected none, fuzzsight or stalker)\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;
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
      parent(pid, NULL, argvp[0]);
      wait(NULL);
      break;
  }

  return 0;
}
