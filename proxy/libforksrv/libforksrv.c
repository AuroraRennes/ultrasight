#define _GNU_SOURCE /* for RTLD_NEXT */
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <signal.h>
#include <asm/unistd.h>
#include <sys/prctl.h>

#define FORKSRV_FD 198
#define AFLCS_FORKSRV_FD (FORKSRV_FD - 3)

void __cs_start_forkserver(void) {
    if(getenv("__CS_PROXY") != NULL) {
        /* CS-PROXY */
        fprintf(stdout, "Start forksrv\n");
    } else {
        /* CS-TRACE */
        if(getenv("__CS_TRACE") != NULL) {
            raise(SIGSTOP);
            return;
        }else {
            return;
        }
    }
    int status;
    pid_t child_pid;

    static char tmp[4] = {0, 0, 0, 0};
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    if (write(AFLCS_FORKSRV_FD + 1, tmp, 4) != 4) {
        _exit(1); // failed to send hello
    }

    while (1) {
        /* Whoops, parent dead? */
        if (read(AFLCS_FORKSRV_FD, tmp, 4) != 4) {
            _exit(2); // parent dead/pipe closed
        }

        /* 10 retries checking for EAGAIN */
        for (int attempt = 0; attempt < 10; attempt++) {
            child_pid = fork();
            if (child_pid >= 0 || errno != EAGAIN) {
                break;
            }
            usleep(1000 << attempt);
        }
        if (child_pid < 0) {
            _exit(3); // fork failed
        }

        if (!child_pid) {
            /* Child process */
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            /* Child process. Wait for parent start tracing */
            raise(SIGSTOP);

            /* Close descriptors and run free. */
            close(AFLCS_FORKSRV_FD);
            close(AFLCS_FORKSRV_FD + 1);

            return;
        }

        /* Parent. Wait for the child to actually reach its raise(SIGSTOP). */
        int stop_status;
        if (waitpid(child_pid, &stop_status, WUNTRACED) < 0) {
            _exit(4); // waitpid failed waiting for SIGSTOP
        }
        if (!WIFSTOPPED(stop_status) || WSTOPSIG(stop_status) != SIGSTOP) {
            if (write(AFLCS_FORKSRV_FD + 1, &stop_status, 4) != 4) {
                _exit(5); // failed to relay early exit
            }
            continue;
        }
        /* Child confirmed stopped — send PID to proxy */
        if (write(AFLCS_FORKSRV_FD + 1, &child_pid, 4) != 4) {
            _exit(6); // failed to send PID
        }

        while (1) {
            /* Get status. */
            if (waitpid(child_pid, &status, WUNTRACED) < 0) {
                _exit(7); // waitpid failed waiting for exit
            }
            /* Relay status to proxy. */
            if (write(AFLCS_FORKSRV_FD + 1, &status, 4) != 4) {
                _exit(8); // failed to relay status
            }
            if (!(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP)) {
                /* The child process is exited. */
                break;
            }
        }
    }
}

#ifndef STATIC

/* Start the forkserver from a constructor rather than from a __libc_start_main
 * hook. Runs after the dynamic linker has already finished relocation, but
 * before the traced binary's own constructors and its main(). Raises a single
 * SIGSTOP under cs-trace, and is a no-op otherwise. */
__attribute__((constructor))
static void __cs_forkserver_ctor(void) {
    __cs_start_forkserver();
}
#endif
