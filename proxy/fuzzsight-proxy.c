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
   binary that must be traced by the CoreSight ETM hardware. The while
   loop boils down to:

     1. fork() the target binary
     2. child: PTRACE_TRACEME + execvpe (AFL has already set up stdin / @@)
     3. parent: catch SIGTRAP, call start_trace() with the new child PID
     4. waitpid until the child exits
     5. disable_trace() — flushes ETB, stops all sources/sinks
     6. bitmap_dma_transfer() — AXI DMA reads BRAM into udmabuf
     7. __afl_area_ptr is directly filled through DMA

   One-time setup (before __afl_start_forkserver):
     - dec_stats_open()   AXI-Lite ETM statistics handle  - TODO: add an option to disable
     - edge_stats_open()  AXI-Lite edge statistics handle - TODO: add an option to disable
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

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/shm.h>
#include <fcntl.h>

/* FuzzSight headers */
#include "bitmap_dma.h"
#include "decoder_stats.h"
#include "edge_stats.h"
#include "common.h"

/* --------------------------------------------------------------------------
 * AFL++ shared map
 * -------------------------------------------------------------------------- */

u8  *__afl_area_ptr;
u32  __afl_map_size = MAP_SIZE;   /* 65536 — matches BRAM Write_Depth_A */

/* --------------------------------------------------------------------------
 * Globals required by the coresight library
 * -------------------------------------------------------------------------- */

extern int            registration_verbose;
extern char          *board_name;
extern unsigned char *trace_bitmap;
extern int            trace_bitmap_size;

/* --------------------------------------------------------------------------
 * AFL++ boilerplate
 * -------------------------------------------------------------------------- */

static void __afl_start_forkserver(void) {

  u32 status = 0;

  if (__afl_map_size <= FS_OPT_MAX_MAPSIZE)
    status |= (FS_OPT_SET_MAPSIZE(__afl_map_size) | FS_OPT_MAPSIZE);
  if (status) status |= FS_OPT_ENABLED;

  if (write(FORKSRV_FD + 1, &status, 4) != 4) return;

}

static pid_t __afl_next_testcase(char **target_argv) {

  u32 was_killed;

  /* Wait for AFL's go signal */
  if (read(FORKSRV_FD, &was_killed, 4) != 4) return 0;

  /* Fork target  */
  pid_t child = fork();
  if (child < 0) {
    perror("[!] fuzzsight-proxy: fork");
    exit(EXIT_FAILURE);
  }

  if (child == 0) {
    close(FORKSRV_FD);
    close(FORKSRV_FD + 1);

    /* CHILD: request tracing then become the target */
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
      perror("[!] fuzzsight-proxy child: PTRACE_TRACEME");
      exit(EXIT_FAILURE);
    }
    execvpe(target_argv[0], target_argv, environ);
    perror("[!] fuzzsight-proxy child: execvpe");
    exit(EXIT_FAILURE);
  }

  if (write(FORKSRV_FD + 1, &child, 4) != 4) {
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return 0;
  }

  return child;

}

static void __afl_end_testcase(int wstatus) {
  fprintf(stderr, "[.] writing end testcase wstatus=0x%x\n", wstatus);
  if (write(FORKSRV_FD + 1, &wstatus, 4) != 4) {
    fprintf(stderr, "[!] end_testcase write failed: %s\n", strerror(errno));
    exit(1);
  }
  fprintf(stderr, "[.] end testcase write done\n");
}


/* --------------------------------------------------------------------------
 * One fuzzing iteration
 * -------------------------------------------------------------------------- */

/*
 * init_trace() reads /proc/<child>/maps to build the ETM address filter.
 */
static u8 first_run = 1;

static int run_target(pid_t child,
                      dec_stats_t  *etm,
                      edge_stats_t *edge,
                      bitmap_dma_t *dma)
{
  int wstatus = 0;
  int ret;

  waitpid(child, &wstatus, 0);

  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != SIGTRAP) {
    fprintf(stderr, "[!] fuzzsight-proxy: unexpected child state 0x%x "
            "(WIFSTOPPED=%d WSTOPSIG=%d WIFEXITED=%d WEXITSTATUS=%d)\n",
            wstatus, WIFSTOPPED(wstatus), WSTOPSIG(wstatus),
            WIFEXITED(wstatus), WEXITSTATUS(wstatus));
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    /* Still return a valid-looking exit status so AFL++ doesn't lose the pipe */
    return (1 << 8); /* fake WEXITSTATUS=1 */
  }

  if (first_run) {
    ret = init_trace(getpid(), child);
    if (ret < 0) {
      fprintf(stderr, "[!] fuzzsight-proxy: init_trace failed (%d)\n", ret);
      kill(child, SIGKILL);
      waitpid(child, NULL, 0);
      return (1 << 8);
    }
    first_run = 0;
  }

  ret = start_trace(child, true);
  if (ret < 0) {
    fprintf(stderr, "[!] fuzzsight-proxy: start_trace failed (%d)\n", ret);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return (1 << 8);
  }

  dec_stats_enable(etm);
  edge_stats_reset(edge);

  fprintf(stderr, "[.] detaching child %d\n", child);
  int detach_ret = ptrace(PTRACE_DETACH, child, NULL, NULL);
  fprintf(stderr, "[.] PTRACE_DETACH returned %d errno=%s\n", detach_ret, strerror(errno));

  /* Wait for child to finish */
  fprintf(stderr, "[.] entering wait loop\n");
  do {
    ret = waitpid(child, &wstatus, WUNTRACED | WCONTINUED);
    fprintf(stderr, "[.] waitpid returned %d wstatus=0x%x WIFEXITED=%d WIFSIGNALED=%d WIFSTOPPED=%d\n",
            ret, wstatus, WIFEXITED(wstatus), WIFSIGNALED(wstatus), WIFSTOPPED(wstatus));
    if (ret < 0) {
      perror("[!] fuzzsight-proxy: waitpid");
      break;
    }
  } while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));
  fprintf(stderr, "[.] wait loop done\n");
  ret = stop_trace(true);
  if (ret < 0)
    fprintf(stderr, "[!] fuzzsight-proxy: stop_trace failed (%d)\n", ret);

  dec_stats_disable(etm);

  ret = bitmap_dma_transfer(dma);
  if (ret < 0)
    fprintf(stderr, "[!] fuzzsight-proxy: bitmap_dma_transfer failed\n");

  return wstatus;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {

  // FIXME: Putting errors somewhere
  int logfd = open("/tmp/fuzzsight.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }


  if (argc < 2) {
    fprintf(stderr, "Usage: %s TARGET [ARGS]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  char **target_argv = &argv[1];

  if (access(target_argv[0], F_OK | X_OK) != 0) {
    perror("[!] fuzzsight-proxy: target not found or not executable");
    exit(EXIT_FAILURE);
  }

  dec_stats_t  etm  = {0};
  edge_stats_t edge = {0};
  bitmap_dma_t dma  = {0};

  if (dec_stats_open(&etm) < 0)
    perror("[!] fuzzsight-proxy: dec_stats_open");
  if (edge_stats_open(&edge) < 0)
    perror("[!] fuzzsight-proxy: edge_stats_open");
  if (bitmap_dma_open(&dma, MAP_SIZE) < 0) {
    perror("[!] fuzzsight-proxy: bitmap_dma_open");
    exit(EXIT_FAILURE);
  }

  /*
   * Point __afl_area_ptr directly at the udmabuf mmap'd VA.
   * bitmap_dma_open() has already mmap'd the udmabuf region at dma.buf
   * and configured the AXI DMA S2MM destination to its physical address.
   */
  __afl_area_ptr = dma.buf;
  trace_bitmap   = dma.buf;
  /* Tell AFL++ the map is live */
  __afl_area_ptr[0] = 1;

  /* AFL++ protocol init */
  __afl_start_forkserver();

  /* Main fuzzing loop */
  pid_t child;
  while ((child = __afl_next_testcase(target_argv)) > 0) {

    int wstatus = run_target(child, &etm, &edge, &dma);
    __afl_end_testcase(wstatus);

  }

  /* Teardown */
  fini_trace();
  dec_stats_close(&etm);
  edge_stats_close(&edge);

  return 0;

}