#define _GNU_SOURCE /* for RTLD_NEXT */
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <signal.h>
#include <asm/unistd.h>
#include <sys/prctl.h>

#define FORKSRV_FD 198
#define AFLCS_FORKSRV_FD (FORKSRV_FD - 3)

static void __cs_start_forkserver(void) {
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

        child_pid = fork();
        if (child_pid < 0) {
            _exit(3); // fork failed
        }

        if (!child_pid) {
            /* Child process */
            prctl(PR_SET_PDEATHSIG, SIGCONT);

            /* Child process. Wait for parent start tracing */
            raise(SIGSTOP);

            /* Close descriptors and run free. */
            close(AFLCS_FORKSRV_FD);
            close(AFLCS_FORKSRV_FD + 1);

            return;
        }

        /* Parent — wait for child to be confirmed stopped before telling proxy */
        int stop_status;
        if (waitpid(child_pid, &stop_status, WUNTRACED) < 0) {
            _exit(4); // waitpid failed writing for SIGSTOP
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

        // /* Wait for second SIGSTOP — post main, pre teardown */
        // if (waitpid(child_pid, &stop_status, WUNTRACED) < 0) {
        //     _exit(7);
        // }
        // if (!WIFSTOPPED(stop_status) || WSTOPSIG(stop_status) != SIGSTOP) {
        //     /* main exited without second stop — relay and continue */
        //     if (write(AFLCS_FORKSRV_FD + 1, &stop_status, 4) != 4) {
        //         _exit(8);
        //     }
        //     continue;
        // }

        // /* Relay second SIGSTOP to proxy as wstatus — proxy calls stop_trace */
        // if (write(AFLCS_FORKSRV_FD + 1, &stop_status, 4) != 4){
        //     _exit(9);
        // }

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

int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end) {

    int (*orig)(int (*main)(int, char **, char **), int argc, char **argv,
                void (*init)(void), void (*fini)(void), void (*rtld_fini)(void),
                void *stack_end);

    orig = dlsym(RTLD_NEXT, __func__);
    if (!orig) {
        fprintf(stderr, "Did not find original %s: %s\n", __func__, dlerror());
        exit(EXIT_FAILURE);
    }

    if(getenv("CS_FORKSERVER") != NULL){
        /* AFL-CS-START */
        do { __cs_start_forkserver(); } while(0);
    } else {
        /* CS-TRACE */
        raise(SIGSTOP);
    }

  return orig(main, argc, argv, init, fini, rtld_fini, stack_end);
}