/*
   FuzzSight — AFL++ proxy for hardware-accelerated CoreSight coverage
   -------------------------------------------------------------------

   Based on afl-proxy.c by Marc Heuse <mh@mh-sec.de>
   Copyright 2019-2024 AFLplusplus Project. All rights reserved.

   Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

   http://www.apache.org/licenses/LICENSE-2.0


   HOW IT WORKS
   ============

   The standard afl-proxy skeleton reads a testcase from stdin into buf[]
   and then runs the target inline.  FuzzSight's target is an external
   binary that must be traced by the CoreSight ETM hardware.  Uses
   libforksrv.so (LD_PRELOAD) to handle fork/freeze/unfreeze inside. The
   while loop boils down to:

     1. Read go signal from AFL
     2. Forward go signal to libforksrv (inner pipe)
     3. libforksrv forks the target, child raises(SIGSTOP) before main()
     4. libforksrv sends child PID back on inner pipe
     5. init_trace() on first run (child is frozen, safe to read /proc/maps)
     6. start_trace()
     7. Forward child PID to AFL (AFL unblocks)
     8. kill(child, SIGCONT) — child runs
     9. Wait for exit status from libforksrv on inner pipe
    10. stop_trace(), bitmap_dma_transfer(), memcpy to AFL shm
    11. Forward wstatus to AFL

   One-time setup (before __afl_start_forkserver):
     - decoder_stats_open() AXI-Lite ETM statistics handle  - TODO: add an option to disable
     - edge_stats_open()    AXI-Lite edge statistics handle - TODO: add an option to disable
     - bitmap_dma_open()  udmabuf DMA handle

   First iteration only (deferred until child PID is known):
     - init_trace()       board registration + ETM address filter from
                          /proc/<child>/maps; safe to do once because
                          ASLR is disabled and layout is stable across forks
   Invocation
   ----------
     afl-fuzz -i in/ -o out/ -- ./fuzzsight-proxy TARGET [ARGS]
*/

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

/* AFL++ headers — must come before our own so their config.h wins */
#include "config.h"   /* FORKSRV_FD, SHM_ENV_VAR, MAP_SIZE, FS_OPT_* */
#include "types.h"    /* u8, u32, s32 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <fcntl.h>

/* FuzzSight headers */
#include "bitmap_dma.h"
#include "decoder_stats.h"
#include "decoder_axi.h"
#include "edge_stats.h"
#include "common.h"
#include "timing.h"

/* Inner pipe fds used to talk to libforksrv inside the target.
   Must match AFL_FUZZSIGHT_FORKSRV_FD in libforksrv.so. */
#define AFL_FUZZSIGHT_FORKSRV_FD (FORKSRV_FD - 3)   /* 195 */

#define AFL_FUZZSIGHT_PROXY_NAME "afl-fuzzsight-proxy"


/* --------------------------------------------------------------------------
 * AFL++ globals
 * -------------------------------------------------------------------------- */

 /* Proxy name */
char *__afl_proxy_name = AFL_FUZZSIGHT_PROXY_NAME;

/* Shared map */
u8  *__afl_area_ptr;
u32  __afl_map_size = MAP_SIZE;   /* 65536 — matches BRAM Write_Depth_A */

/* libforksrv related */
s32 fsrv_pid = -1;
s32 proxy_ctl_fd = -1;
s32 proxy_st_fd = -1;

u8 first_run = 1;
u8 first_dump = 1;


static decoder_stats_t g_stats = {0};
static edge_stats_t g_edge     = {0};
static bitmap_dma_t g_dma      = {0};
static decoder_axi_t g_dec     = {0};

static int count = 0;

/* --------------------------------------------------------------------------
 * Globals required by the coresight library
 * -------------------------------------------------------------------------- */

extern int            registration_verbose;
extern bool           use_etr;
extern char          *board_name;
extern unsigned char *trace_bitmap;
extern int            trace_bitmap_size;
extern int            trace_cpu;

/* --------------------------------------------------------------------------
 * AFL++ boilerplate
 * -------------------------------------------------------------------------- */

void send_forkserver_error(int error)
{
    u32 status;
    if (!error || error > 0xffff) return;
    status = (FS_OPT_ERROR | FS_OPT_SET_ERROR(error));
    if (write(FORKSRV_FD + 1, (char *)&status, 4) != 4) return;
}


static void __afl_map_shm(void)
{
    char *id_str = getenv(SHM_ENV_VAR);
    char *ptr;

    if ((ptr = getenv("AFL_MAP_SIZE")) != NULL) {
        u32 val = atoi(ptr);
        if (val > 0) trace_bitmap_size = val;
    }

    if (trace_bitmap_size > MAP_SIZE) {
        if (trace_bitmap_size > FS_OPT_MAX_MAPSIZE) {
            fprintf(stderr,
                    "Error: %s *require* to set AFL_MAP_SIZE to %u to "
                    "be able to run this instrumented program!\n",
                    __afl_proxy_name, trace_bitmap_size);
            if (id_str) {
                send_forkserver_error(FS_ERROR_MAP_SIZE);
                exit(-1);
            }
        } else {
            fprintf(stderr,
                    "Warning: %s will need to set AFL_MAP_SIZE to %u to "
                    "be able to run this instrumented program!\n",
                    __afl_proxy_name, trace_bitmap_size);
        }
    }

    if (id_str) {
#ifdef USEMMAP
        const char *shm_file_path = id_str;
        int shm_fd = shm_open(shm_file_path, O_RDWR, 0600);
        if (shm_fd == -1) {
            fprintf(stderr, "shm_open() failed\n");
            send_forkserver_error(FS_ERROR_SHM_OPEN);
            exit(1);
        }
        unsigned char *shm_base = mmap(0, trace_bitmap_size,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, shm_fd, 0);
        if (shm_base == MAP_FAILED) {
            close(shm_fd);
            fprintf(stderr, "mmap() failed\n");
            send_forkserver_error(FS_ERROR_MMAP);
            exit(2);
        }
        trace_bitmap = shm_base;
#else
        u32 shm_id = atoi(id_str);
        trace_bitmap = shmat(shm_id, 0, 0);
#endif
        if (trace_bitmap == (void *)-1) {
            send_forkserver_error(FS_ERROR_SHMAT);
            exit(1);
        }
        trace_bitmap[0] = 1;
    }
}


static void __afl_start_forkserver(char **target_argv) {

  u8  tmp[4] = {0, 0, 0, 0};
  u32 status = 0;
  int st_pipe[2], ctl_pipe[2];

  /* Pipes between proxy and libforksrv */
  if (pipe(st_pipe) || pipe(ctl_pipe)) {
    perror("[!] fuzzsight-proxy: pipe() failed");
    exit(EXIT_FAILURE);
  }

  fsrv_pid = fork();
  if (fsrv_pid < 0) {
    perror("[!] fuzzsight-proxy: fork() failed");
    exit(EXIT_FAILURE);
  }

  if (fsrv_pid == 0) {
    /* ---- child: become the target with libforksrv preloaded ---- */

    /* Wire inner pipes to the fds libforksrv expects */
    if (dup2(ctl_pipe[0], AFL_FUZZSIGHT_FORKSRV_FD) < 0 ||
        dup2(st_pipe[1],  AFL_FUZZSIGHT_FORKSRV_FD + 1) < 0) {
      perror("[!] fuzzsight-proxy child: dup2() failed");
      exit(EXIT_FAILURE);
    }

    close(ctl_pipe[0]); close(ctl_pipe[1]);
    close(st_pipe[0]);  close(st_pipe[1]);

    /* AFL's outer fds must not leak into the target */
    close(FORKSRV_FD);
    close(FORKSRV_FD + 1);

    /* Build LD_PRELOAD: optional CS_LD_PRELOAD + libforksrv.so */
    char *libforksrv_path = getenv("FUZZSIGHT_LIBFORKSRV");
    if (!libforksrv_path) {
      perror("[!] fuzzsight-proxy child: could not find libforksrv.so");
      exit(EXIT_FAILURE);
    }

    char ld_preload[4096] = "LD_PRELOAD=";
    char *cs_ld_preload = getenv("CS_LD_PRELOAD");
    if (cs_ld_preload) {
      strncat(ld_preload, cs_ld_preload, sizeof(ld_preload) - strlen(ld_preload) - 2);
      strncat(ld_preload, ":", sizeof(ld_preload) - strlen(ld_preload) - 2);
    }
    strncat(ld_preload, libforksrv_path, sizeof(ld_preload) - strlen(ld_preload) - 1);

    char ld_lib[4096] = "LD_LIBRARY_PATH=";
    char *cs_ld_lib = getenv("CS_LD_LIBRARY_PATH");
    if (cs_ld_lib)
      strncat(ld_lib, cs_ld_lib, sizeof(ld_lib) - strlen(ld_lib) - 1);

    char *envp[] = { "CS_FORKSERVER=1", ld_preload, ld_lib, NULL };

    execve(target_argv[0], target_argv, envp);
    perror("[!] fuzzsight-proxy child: execve failed");
    exit(EXIT_FAILURE);
  }

  /* ---- parent: finish pipe setup ---- */
  close(ctl_pipe[0]);
  close(st_pipe[1]);
  proxy_ctl_fd = ctl_pipe[1];
  proxy_st_fd  = st_pipe[0];

  /* Wait for libforksrv's hello (4 bytes) */
  if (read(proxy_st_fd, tmp, 4) != 4) {
    perror("[!] fuzzsight-proxy: read() failed - libforksrv did not start");
    exit(EXIT_FAILURE);
  }
  memcpy(&status, tmp, 4);

  /* Build our own status word for AFL */
  if (!status) {
    if (__afl_map_size <= FS_OPT_MAX_MAPSIZE)
      status |= (FS_OPT_SET_MAPSIZE(__afl_map_size) | FS_OPT_MAPSIZE);
    if (status) status |= FS_OPT_ENABLED;
    memcpy(tmp, &status, 4);
  }

  /* Tell AFL++ "forkserver is ready" */
  if (write(FORKSRV_FD + 1, tmp, 4) != 4) {
    perror("[!] fuzzsight-proxy: write() status to AFL failed");
    exit(EXIT_FAILURE);
  }
}

/* --------------------------------------------------------------------------
 * One fuzzing iteration
 * -------------------------------------------------------------------------- */

static pid_t __afl_next_testcase(void) {

  u32 was_killed;
  s32 child_pid;

  /* Read go signal from AFL, forward to libforksrv */
  TS_DECL(pipe);
  TS_START(pipe);
  if (read(FORKSRV_FD, &was_killed, 4) != 4) return -1;
  if (write(proxy_ctl_fd, &was_killed, 4) != 4) return -1;

  /* libforksrv forks + child raises SIGSTOP before main(), sends us PID */
  if (read(proxy_st_fd, &child_pid, 4) != 4) return -1;

  TS_STOP(pipe);
  TS_PRINT(pipe, "initial pipe");

  /* One-time board registration + ETM address filter */
  if (first_run) {
    trace_cpu = 0;
    TS_DECL(init);
    TS_START(init);
    if (init_trace(fsrv_pid, child_pid) < 0) {
      fprintf(stderr, "[!] fuzzsight-proxy: init_trace failed\n");
      kill(child_pid, SIGKILL);
      return -1;
    }
    TS_STOP(init);
    TS_PRINT(init, "init_trace");
    first_run = 0;

    /* Set address range filter in PL decoder from /proc/<child>/maps */
    struct map_info range[1];
    int n = setup_map_info(child_pid, range, 1);
    if (n > 0) {
        decoder_axi_set_range(&g_dec, range[0].start, range[0].end);
    } else {
        fprintf(stderr, "[!] fuzzsight-proxy: could not read map info for range filter\n");
    }

    /* Print range, if stderr is accessible */
    decoder_axi_print_range(&g_dec);

    /* Initial decoder soft reset */
    decoder_axi_soft_reset(&g_dec);

    if (bitmap_dma_transfer(&g_dma) < 0)
      fprintf(stderr, "[!] fuzzsight-proxy: clear bitmap_dma_transfer failed\n");
  }

  /* Start CoreSight — child still frozen, safe */
  TS_DECL(start);
  TS_START(start);
  if (start_trace(child_pid, true) < 0) {
    fprintf(stderr, "[!] fuzzsight-proxy: start_trace failed\n");
    kill(child_pid, SIGKILL);
    return -1;
  }
  TS_STOP(start);
  TS_PRINT(start, "start_trace");

#ifdef STATS
  edge_stats_reset(&g_edge);
  decoder_axi_stats_reset(&g_dec);
  decoder_stats_enable(&g_stats);
#endif

  /* Tell AFL the PID — AFL unblocks */
  TS_DECL(next_pid);
  TS_START(next_pid);
  if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) return -1;
  TS_STOP(next_pid);
  TS_PRINT(next_pid, "next_pid");

  /* Release child into main */
  kill(child_pid, SIGCONT);

  return child_pid;
}


static int __afl_end_testcase(pid_t child_pid) {
  int wstatus;

  /* Wait for exit status from libforksrv */
  if (read(proxy_st_fd, &wstatus, 4) != 4) return -1;

  TS_MEASURE(stop, "stop_trace",
  stop_trace(true);
  );

#ifdef STATS
  /* Stop stats collection */
  decoder_stats_enable(&g_stats);

  /* Print edge, decoder errors and decoder stats */
  edge_stats_print(&g_edge);
  decoder_axi_print(&g_dec);
  decoder_stats_print(&g_stats);
#endif

  TS_MEASURE(dma, "dma_transfer",
  if (bitmap_dma_transfer(&g_dma) < 0)
      fprintf(stderr, "[!] fuzzsight-proxy: bitmap_dma_transfer failed\n");
  );

  TS_MEASURE(copy, "memcpy",
  memcpy(__afl_area_ptr, g_dma.buf, MAP_SIZE);
  );

#ifdef BITMAP_CMP
  /* Comparing bitmaps */
  static unsigned char ref_bitmap[MAP_SIZE] = {0};
  static int ref_bitmap_set = 0;
  static const unsigned char zero_bitmap[MAP_SIZE] = {0};

  unsigned char *bitmap = (unsigned char *)g_dma.buf;

  if (!ref_bitmap_set) {
      memcpy(ref_bitmap, bitmap, MAP_SIZE);
      ref_bitmap_set = 1;
      fprintf(stderr, "[.] reference bitmap stored\n");
  } else {
      if (memcmp(ref_bitmap, bitmap, MAP_SIZE) != 0) {
          if (first_dump) {
            int diff_count = 0;
            fprintf(stderr, "[.] bitmap differs from reference:\n");
            for (int i = 0; i < MAP_SIZE; i++) {
                if (ref_bitmap[i] != bitmap[i])
                    fprintf(stderr, "[.] diff [0x%x]: ref=%02x cur=%02x\n",
                            i, ref_bitmap[i], bitmap[i]);
                    diff_count = diff_count + abs(ref_bitmap[i] - bitmap[i]);
            }
            fprintf(stderr, "Total diff edges: %d\n", diff_count);

            export_trace_with_config(count);
            count++;
            first_dump = 0;
          }
      }
  }
#endif

  /* Reset the decoder */
  decoder_axi_soft_reset(&g_dec);

  /* Report exit status to AFL */
  if (write(FORKSRV_FD + 1, &wstatus, 4) != 4) return -1;

  return 0;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

 int main(int argc, char *argv[]) {

#if defined(STATS) || defined(BITMAP_CMP)
  int logfd = open("/tmp/fuzzsightq.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }
#endif

  setbuf(stderr, NULL);
  setbuf(stdout, NULL);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s -- TARGET [ARGS]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if (geteuid() != 0) {
    fprintf(stderr, "Error: root is required (CoreSight needs /dev/mem)\n");
    return -1;
  }

    /* ---- Initialize ---- */
  registration_verbose = 0;
  use_etr = false;

  /* Find -- separator */
  char **target_argv = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--") && i + 1 < argc) {
      target_argv = &argv[i + 1];
      break;
    }
  }
  if (!target_argv) {
    fprintf(stderr, "Usage: %s -- TARGET [ARGS]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if (access(target_argv[0], F_OK | X_OK) != 0) {
    perror("[!] fuzzsight-proxy: target not found or not executable");
    exit(EXIT_FAILURE);
  }

  if (decoder_stats_open(&g_stats) < 0)
    perror("[!] fuzzsight-proxy: decoder_stats_open");
  if (edge_stats_open(&g_edge) < 0)
    perror("[!] fuzzsight-proxy: edge_stats_open");
  if(decoder_axi_open(&g_dec))
    perror("[!] fuzzsight-proxy: decoder_axi_open");
  if (bitmap_dma_open(&g_dma, MAP_SIZE) < 0) {
    perror("[!] fuzzsight-proxy: bitmap_dma_open");
    exit(EXIT_FAILURE);
  }

  setenv("__AFLCS_ENABLE", "1", 0);

  /* Map shared memory */
  __afl_map_shm();
  /* Point __afl_area_ptr at the same SHM as trace_bitmap */
  if (trace_bitmap) {
      __afl_area_ptr = trace_bitmap;
  } else {
      /* No SHM (running outside AFL) — allocate a dummy buffer */
      __afl_area_ptr = calloc(MAP_SIZE, 1);
  }
  __afl_area_ptr[0] = 1;

  /* trace_bitmap = DMA buffer (what hardware writes to) */
  trace_bitmap = g_dma.buf;

  /* AFL++ protocol init */
  __afl_start_forkserver(target_argv);

  /* Main fuzzing loop */
  pid_t child;
  while ((child = __afl_next_testcase()) > 0) {
    if (__afl_end_testcase(child) < 0) break;
  }

  /* Teardown */
  fini_trace();
  decoder_stats_close(&g_stats);
  edge_stats_close(&g_edge);
  bitmap_dma_close(&g_dma);

  return 0;

}