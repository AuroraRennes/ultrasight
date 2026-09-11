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
#include <errno.h>

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
#include "trace_capture.h"
#include "decoder_axi.h"

/**
 * Extern value definitions
 */
#define DEFAULT_TRACE_BITMAP_SIZE_POW2 (16)
#define DEFAULT_TRACE_BITMAP_SIZE (1U << (DEFAULT_TRACE_BITMAP_SIZE_POW2))
/* Same name fini_trace() uses for the ETR dump, so a capture is a drop-in
 * replacement in the snapshot directory */
#define DEFAULT_CAPTURE_NAME "cstrace.bin"
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
extern const char *udmabuf_name;
extern bool ksight_on;
extern int trace_cpu;
extern unsigned char *trace_bitmap;
extern unsigned int trace_bitmap_size;
extern int range_count;
extern struct map_info map_info[];

static const char *addr_filter_name(addr_filter_t f)
{
  switch (f) {
    case ADDR_FILTER_PL:   return "pl";
    case ADDR_FILTER_ETM:  return "etm";
    case ADDR_FILTER_NONE: return "none";
  }
  return "?";
}

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

/* What a run is for. The fuzzsight_tri bitstream carries both paths but they
 * are never used together: FUZZ hashes edges into the bitmap and reads it out
 * over the bitmap DMA, CAPTURE streams the raw TPIU frames into DDR over the
 * trace DMA and writes them as cstrace.bin. Each mode only opens its own
 * blocks, so the other DMA is never programmed. */
typedef enum { RUN_MODE_FUZZ, RUN_MODE_CAPTURE } run_mode_t;
static run_mode_t run_mode = RUN_MODE_FUZZ;

static const char *run_mode_name(run_mode_t m)
{
  return m == RUN_MODE_CAPTURE ? "capture" : "fuzz";
}

#define S2MM_STATUS_HALTED (1 << 0)

/* An S2MM channel is mid-transfer when it is neither halted nor idle */
static int s2mm_busy(unsigned long base, const char *what)
{
  axi_regs_t regs;
  if (axi_regs_open(&regs, base, DMA_MAP_SIZE) < 0) {
    perror("[!] axi_regs_open");
    return -1;
  }
  uint32_t st = axi_regs_read(&regs, S2MM_STATUS_REGISTER);
  axi_regs_close(&regs);
  if (!(st & S2MM_STATUS_HALTED) && !(st & STATUS_IDLE)) {
    fprintf(stderr, "[!] %s is mid-transfer (S2MM status 0x%08x)\n", what, st);
    return 1;
  }
  return 0;
}

/* Refuse to start while the other mode's path is still running, e.g. after a
 * cs-trace killed by a timeout. Both paths live in the same bitstream, so this
 * is a plain register read. */
static int check_other_mode_idle(void)
{
  if (run_mode == RUN_MODE_CAPTURE)
    return s2mm_busy(DMA_BASE, "The bitmap DMA") ? -1 : 0;

  axi_regs_t cap;
  if (axi_regs_open(&cap, TRACE_CAPTURE_BASE, TRACE_CAPTURE_MAP_SIZE) < 0) {
    perror("[!] axi_regs_open trace capture");
    return -1;
  }
  uint32_t st = axi_regs_read(&cap, TRACE_CAPTURE_STATUS);
  axi_regs_close(&cap);
  if (st & (TRACE_CAPTURE_ST_ENABLED | TRACE_CAPTURE_ST_FLUSH_BUSY)) {
    fprintf(stderr, "[!] Trace capture still active (status 0x%08x)\n", st);
    return -1;
  }
  return s2mm_busy(TRACE_DMA_BASE, "The trace DMA") ? -1 : 0;
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
  trace_capture_t cap_handle;
  bool cap_open = false, cap_armed = false;

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

      if (run_mode == RUN_MODE_FUZZ) {
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
      } else {
        ret = trace_capture_open(&cap_handle);
        if (ret < 0) perror("[!] Trace capture setup issue");
        cap_open = (ret == 0);
      }

      ret = decoder_axi_open(&dec_axi_handle);
      if (ret < 0) perror("[!] Decoder AXI errors setup issue");

      /* The decoder range and the edge filter enable outlive a run, so write
       * both every time. The decoder applies the range to its exceptions, the
       * edge_extractor drops out-of-range atoms with it (PL filter only) */
      printf("[~] Address filter: %s\n", addr_filter_name(addr_filter));
      if (addr_filter == ADDR_FILTER_NONE || range_count == 0) {
        if (addr_filter != ADDR_FILTER_NONE)
          fprintf(stderr, "[!] No traced range, the decoder range stays open\n");
        decoder_axi_set_range(&dec_axi_handle, 0, UINT64_MAX);
      } else {
        decoder_axi_set_range(&dec_axi_handle, map_info[0].start, map_info[0].end);
      }
      decoder_axi_print_range(&dec_axi_handle);
      if (run_mode == RUN_MODE_FUZZ) {
        int want = addr_filter == ADDR_FILTER_PL;
        edge_extractor_set_range_filter(&edge_handle, want);
        /* Confirm the write landed */
        if ((int)(axi_regs_read(&edge_handle, EDGE_EXTRACTOR_RANGE_EN) & 1) != want)
          fprintf(stderr,
                  "[!] Edge range filter not applied: the loaded bitstream likely "
                  "predates the range-enable register\n");
      }

      // decoder_axi_soft_reset(&dec_axi_handle);
      decoder_stats_enable(&dec_stats_etm_handle);
      decoder_axi_stats_reset(&dec_axi_handle);
      if (run_mode == RUN_MODE_FUZZ) {
        edge_extractor_reset(&edge_handle);
        bitmap_dma_transfer(&dma_handle); // Clearing DMA
      } else if (cap_open) {
        /* Armed before the ETM is enabled, the TPIU does not wait for anyone */
        cap_armed = (trace_capture_arm(&cap_handle) == 0);
        if (!cap_armed) fprintf(stderr, "[!] Trace capture could not be armed\n");
      }

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
    /* Wait for the child process to stop, bail out on error (e.g. ECHILD)
       instead of busy-looping on an immediate -1 */
    if (waitpid(pid, &wstatus, WUNTRACED | WCONTINUED) < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "[!] waitpid(%d) failed: %s\n", pid, strerror(errno));
      break;
    }

    /** If the child process exited normally, stop and finalize the trace before
     * breaking from the loop else, if it was stopped using SIGSTOP, the
     * function triggers the callback function.
     */
    if (WIFEXITED(wstatus)) {
      /* Child timer ends the moment the child exits */
      clock_gettime(CLOCK_MONOTONIC, &child_end);

      /* Finalize on any exit status: a non-zero exit (e.g. a rejected fuzz
         input) still has a trace worth dumping */
      printf("[+] Child exited with status %d\n", WEXITSTATUS(wstatus));

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
        if (run_mode == RUN_MODE_FUZZ) {
          printf("============= EDGES =============\n");
          edge_extractor_print(&edge_handle);
        }
        printf("============ DEC ERR ============\n");
        decoder_axi_print(&dec_axi_handle);

        if (stats_csv_path && run_mode == RUN_MODE_FUZZ) {
          struct timespec csv_now;
          clock_gettime(CLOCK_MONOTONIC, &csv_now);
          double csv_child_s = (child_end.tv_sec - child_start.tv_sec) +
                                (child_end.tv_nsec - child_start.tv_nsec) / 1e9;
          double csv_instr_s = (csv_now.tv_sec - instr_start.tv_sec) +
                                (csv_now.tv_nsec - instr_start.tv_nsec) / 1e9;
          double csv_global_s = (csv_now.tv_sec - global_start.tv_sec) +
                                 (csv_now.tv_nsec - global_start.tv_nsec) / 1e9;
          write_stats_csv(stats_csv_path, binary_name, &dec_stats_etm_handle,
                           &dec_axi_handle, &edge_handle,
                           csv_child_s, csv_instr_s, csv_global_s);
        }

        if (run_mode == RUN_MODE_CAPTURE) {
          printf("============ CAPTURE ============\n");
          /* fini_trace() above wrote cstrace.bin from the ETR, which is off in
           * this mode: replace it with the frames captured in the PL, or remove
           * it rather than leave stale ETR data behind */
          if (cap_armed && trace_capture_finish(&cap_handle) == 0 &&
              trace_capture_export(&cap_handle, DEFAULT_CAPTURE_NAME) == 0) {
            printf("[+] Captured %" PRIu64 " frames (%zu bytes) into %s, %u dropped%s\n",
                   cap_handle.frames_captured, cap_handle.trace_bytes,
                   DEFAULT_CAPTURE_NAME, cap_handle.frames_dropped,
                   cap_handle.truncated ? ", TRUNCATED" : "");
          } else {
            fprintf(stderr, "[!] Trace capture failed, removing %s\n", DEFAULT_CAPTURE_NAME);
            unlink(DEFAULT_CAPTURE_NAME);
          }
          if (cap_open) trace_capture_close(&cap_handle);
          goto teardown_done;
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

        edge_extractor_close(&edge_handle);

teardown_done:
        decoder_stats_close(&dec_stats_etm_handle);
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
          "  -u, --udmabuf=NAME\t\tu-dma-buf device the ETR drains into "
          "(default: %s, or $%s)\n",
          UDMABUF_ETR_NAME, ETR_UDMABUF_ENV);
  fprintf(stderr, "  -k, --ksight\t\tenable ksight kernel tag events tracing (default: %d)\n",
          ksight_on);
  fprintf(stderr, "  -r, --useetr\t\tuse the ETR sink (in SDRAM), (default %d)\n",
          use_etr);
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
  fprintf(stderr, "  -M, --mode=MODE\t\tfuzz (edge bitmap over the bitmap DMA) or "
                  "capture (raw TPIU frames into cstrace.bin, forces the ETR off) "
                  "(default %s)\n", run_mode_name(run_mode));
  fprintf(stderr, "  -F, --addrfilter=MODE\t\twhere the tracee text range is enforced: "
                  "pl (decoder + edge_extractor, the ETM traces all of EL0), etm "
                  "(ETM address comparators) or none (default %s)\n",
                  addr_filter_name(addr_filter));
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
      {"mode", required_argument, NULL, 'M'},
      {"addrfilter", required_argument, NULL, 'F'},
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
  /* The PL filter replaces the ETM's, which overflows the ETM FIFO */
  addr_filter = ADDR_FILTER_PL;

  /* Check argument count */
  if (argc < 3) {
    usage(argv[0]);
    exit(EXIT_SUCCESS);
  }
  /* Parse CLI elements */
  while ((opt = getopt_long(argc, argv, "b:c:e:f:u:k:r:s:t:m:a:v:n::o:g:M:F:h", long_options,
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
      /* udmabuf device name */
      case 'u':
        udmabuf_name = optarg;
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
      case 'M':
        if (!strcmp(optarg, "fuzz"))
          run_mode = RUN_MODE_FUZZ;
        else if (!strcmp(optarg, "capture"))
          run_mode = RUN_MODE_CAPTURE;
        else {
          fprintf(stderr, "[!] Unknown mode '%s' (expected fuzz or capture)\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;
      case 'F':
        if (!strcmp(optarg, "pl"))
          addr_filter = ADDR_FILTER_PL;
        else if (!strcmp(optarg, "etm"))
          addr_filter = ADDR_FILTER_ETM;
        else if (!strcmp(optarg, "none"))
          addr_filter = ADDR_FILTER_NONE;
        else {
          fprintf(stderr, "[!] Unknown address filter '%s' "
                          "(expected pl, etm or none)\n", optarg);
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

  if (run_mode == RUN_MODE_CAPTURE) {
    if (no_trace) {
      fprintf(stderr, "[!] --mode=capture and --notrace are exclusive\n");
      exit(EXIT_FAILURE);
    }
    if (use_etr) {
      /* The trace DMA writes into the ETR's u-dma-buf by default, and
       * fini_trace() dumps the ETR over cstrace.bin anyway */
      printf("[~] Capture mode: ETR sink disabled, the trace leaves through the TPIU\n");
      use_etr = false;
    }
    if (stats_csv_path)
      fprintf(stderr, "[~] Capture mode: --csv is fuzz-mode only, ignored\n");
  }

  if (!no_trace && check_other_mode_idle() < 0) {
    fprintf(stderr, "[!] Refusing to start in %s mode while the %s path is active: "
                    "let that run finish, or reload the bitstream\n",
            run_mode_name(run_mode),
            run_mode == RUN_MODE_FUZZ ? "capture" : "fuzz");
    exit(EXIT_FAILURE);
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
