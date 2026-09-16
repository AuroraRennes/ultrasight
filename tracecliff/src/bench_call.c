/*
 * bench_call.c — CoreSight ETM address-range-crossing characterization
 *
 * The loop and chain families are hermetic: their branch targets all live in
 * the same segment as the loop, so the ETM never sees execution leave
 * map_info[0].  This benchmark defines a hot loop carrying one period-64
 * conditional branch per iteration, with one PLT call into libc every
 * CALL_EVERY iterations, effectively going in and out of map_info[0]
 *
 * One knob controls program generation:
 *   CALL_EVERY : iterations between successive library calls. 0 = never call
 *                (the control point: same loop, no range crossing at all),
 *                1 = a crossing every iteration. NOTE this is the INVERSE of
 *                crossing density: larger = crossing-SPARSER.
 *
 * The callee must land in a mapping other than map_info[0], or the ETM traces
 * it anyway and the crossing is not a crossing. Two things enforce that:
 *   - the binary is linked dynamically (no -static in the build rule), so
 *     libc is a separate mapping reached through the PLT;
 *   - -fno-builtin keeps the compiler from expanding strlen inline
 * The callee is deliberately short (strlen on a 1-byte string), so the cost
 * measured is the crossing itself ( TRACE_ON plus a Long address packet on
 * the way in and out) rather than whatever the library does once there.
 *
 * Usage:
 *   ./bench_call_cC_iI      (built with CALL_EVERY=C, ITERS=I)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef CALL_EVERY
#define CALL_EVERY 0
#endif

#ifndef ITERS
#define ITERS 100000
#endif

static volatile uint64_t sink = 0;

#if CALL_EVERY > 0
/* One byte plus its terminator. Non-const so the length cannot be
 * constant-folded even if -fno-builtin were dropped. */
static char one_byte[2] = "x";
#endif

__attribute__((noinline)) static void take_branch(void) { asm volatile("" ::: "memory"); }
__attribute__((noinline)) static void skip_branch(void) { asm volatile("" ::: "memory"); }

__attribute__((optimize("O1")))
static void bench(void)
{
#if CALL_EVERY > 0
    /* Counts down to the next crossing; a compare against 0 rather than a
     * modulo, so the dial does not also change the loop's arithmetic cost. */
    unsigned long countdown = 1;
#endif

    for (unsigned long i = 0; i < (unsigned long)ITERS; i++) {

#if CALL_EVERY > 0
        if (--countdown == 0) {
            countdown = (unsigned long)CALL_EVERY;
            /* The range crossing: out through the PLT into libc and back */
            sink += strlen(one_byte);
        }
#endif

        /* One period-64 conditional branch per iteration, so the direct-branch
         * baseline is fixed and every difference between CALL_EVERY values is
         * the range crossings alone */
        int cond = (int)((i >> 6) & 1);

        if (cond) {
            take_branch();
        } else {
            skip_branch();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    /* Warmup: cold-cache effects skew the first run, and the first PLT call
     * additionally pays lazy-binding resolution */
    bench();

    uint64_t ts0 = now_ns();
    bench();
    uint64_t ts1 = now_ns();

    uint64_t elapsed_ns = ts1 - ts0;
    double   elapsed_s  = (double)elapsed_ns * 1e-9;
    double   br_per_s   = (double)ITERS / elapsed_s;
#if CALL_EVERY > 0
    double   calls      = (double)ITERS / (double)CALL_EVERY;
#else
    double   calls      = 0.0;
#endif

    printf("call_every=%-3d  iters=%d\n", CALL_EVERY, ITERS);
    printf("elapsed=%.6f s  branches/s=%.0f\n", elapsed_s, br_per_s);
    printf("crossings=%.0f  crossings/s=%.0f\n", calls, calls / elapsed_s);

    return 0;
}
