/*
 * bench_call.c — CoreSight ETM address-range-crossing characterization
 *
 * The loop and chain families are hermetic: their branch targets all live in
 * the same segment as the loop, so the ETM never sees execution leave
 * map_info[0].  This benchmark defines a hot loop carrying one period-64
 * conditional branch per iteration, with a PLT call into libc on CALL_K of
 * every CALL_P iterations, effectively going in and out of map_info[0]
 *
 * Four knobs control program generation:
 *   CALL_K : iterations out of every CALL_P that call into libc, spread as
 *            evenly as the period allows. 0 = never call (the control point:
 *            same loop, no range crossing at all), CALL_P = a crossing every
 *            iteration. The crossing density is CALL_K / CALL_P, linear in the
 *            knob, in steps of 1/CALL_P.
 *   CALL_P : the period, a power of two no larger than 32 (default 16).
 *   CALL_LEN : length of the string strlen scans, i.e. how long each call
 *            stays in libc. strlen's loop takes a direct backward branch per
 *            chunk it scans, the branches broadcast pays for outside the text;
 *            1 has none, so the crossing is all there is.
 *   CALL_FILL : dependent adds per iteration, straight-line code with no
 *            branch, so it costs cycles and no trace. It lowers the loop's
 *            own trace rate without changing its packets, placing the
 *            no-call control under the sink's ceiling. 0 = none.
 *
 * The code is the same for every CALL_K: the pattern is a mask read from a
 * volatile at run time and tested on every iteration, so the compiler cannot
 * fold the test away at 0 or CALL_P, and only the data differs between
 * binaries.
 *
 * The callee must land in a mapping other than map_info[0], or the ETM traces
 * it anyway and the crossing is not a crossing. Two things enforce that:
 *   - the binary is linked dynamically (no -static in the build rule), so
 *     libc is a separate mapping reached through the PLT;
 *   - -fno-builtin keeps the compiler from expanding strlen inline
 * At CALL_LEN=1 the callee is as short as it gets, so the cost measured is the
 * crossing itself (TRACE_ON plus a Long address packet on the way in and out)
 * rather than whatever the library does once there.
 *
 * Usage:
 *   ./bench_call_kK_lL_iI   (built with CALL_K=K, CALL_LEN=L, ITERS=I)
 *   ./bench_call_fill_kK_iI (built with CALL_FILL, CALL_K=K, CALL_LEN=1, ITERS=I)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef CALL_K
#define CALL_K 0
#endif

#ifndef CALL_P
#define CALL_P 16
#endif

#ifndef CALL_LEN
#define CALL_LEN 1
#endif

#ifndef ITERS
#define ITERS 100000
#endif

#ifndef CALL_FILL
#define CALL_FILL 0
#endif

#define STR_(x) #x
#define STR(x) STR_(x)

#if CALL_P > 32 || (CALL_P & (CALL_P - 1)) != 0
#error "CALL_P must be a power of two no larger than 32"
#endif
#if CALL_K < 0 || CALL_K > CALL_P
#error "CALL_K must lie in [0, CALL_P]"
#endif

/* Slot j of the period calls when floor((j+1)K/P) steps past floor(jK/P):
 * exactly K of the P slots, spaced as evenly as integers allow. */
#define SLOT(j) \
    ((j) < CALL_P && ((j) + 1) * CALL_K / CALL_P != (j) * CALL_K / CALL_P \
         ? 1ul << (j) : 0ul)
#define SLOTS4(j) (SLOT(j) | SLOT((j) + 1) | SLOT((j) + 2) | SLOT((j) + 3))
#define CALL_MASK \
    (SLOTS4(0) | SLOTS4(4) | SLOTS4(8) | SLOTS4(12) | \
     SLOTS4(16) | SLOTS4(20) | SLOTS4(24) | SLOTS4(28))

static volatile uint64_t sink = 0;

/* Volatile so the pattern is data, not code: see the header */
static volatile uint32_t call_mask = CALL_MASK;

/* CALL_LEN bytes plus the terminator, filled in main. Non-const so the length
 * cannot be constant-folded even if -fno-builtin were dropped. */
static char string[CALL_LEN + 1];

__attribute__((noinline)) static void take_branch(void) { asm volatile("" ::: "memory"); }
__attribute__((noinline)) static void skip_branch(void) { asm volatile("" ::: "memory"); }

__attribute__((optimize("O1")))
static void bench(void)
{
    uint32_t mask = call_mask;
#if CALL_FILL > 0
    uint64_t fill = 0;
#endif

    for (unsigned long i = 0; i < (unsigned long)ITERS; i++) {

#if CALL_FILL > 0
        /* A dependency chain the core cannot overlap: cycles, no branch */
        asm volatile(".rept " STR(CALL_FILL) "\n\tadd %0, %0, #1\n\t.endr"
                     : "+r"(fill));
#endif

        if ((mask >> (i & (CALL_P - 1))) & 1) {
            /* The range crossing: out through the PLT into libc and back */
            sink += strlen(string);
        }

        /* One period-64 conditional branch per iteration, so the direct-branch
         * baseline is fixed and every difference between CALL_K values is
         * the range crossings alone */
        int cond = (int)((i >> 6) & 1);

        if (cond) {
            take_branch();
        } else {
            skip_branch();
        }
    }
#if CALL_FILL > 0
    sink += fill;
#endif
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
    /* One libc call whatever CALL_LEN, so the text's own trace does not
     * depend on it */
    memset(string, 'x', CALL_LEN);

    /* Warmup: cold-cache effects skew the first run, and the first PLT call
     * additionally pays lazy-binding resolution */
    bench();

    uint64_t ts0 = now_ns();
    bench();
    uint64_t ts1 = now_ns();

    uint64_t elapsed_ns = ts1 - ts0;
    double   elapsed_s  = (double)elapsed_ns * 1e-9;
    double   br_per_s   = (double)ITERS / elapsed_s;
    double   calls      = (double)ITERS * CALL_K / CALL_P;

    printf("call_k=%-3d  call_p=%-3d  call_len=%-5d  call_fill=%-3d  iters=%d\n",
           CALL_K, CALL_P, CALL_LEN, CALL_FILL, ITERS);
    printf("elapsed=%.6f s  branches/s=%.0f\n", elapsed_s, br_per_s);
    printf("crossings=%.0f  crossings/s=%.0f\n", calls, calls / elapsed_s);

    return 0;
}
