/*
 * bench_addr.c — CoreSight ETM overflow characterization
 *
 * Generates a controlled stream of indirect branches at varying address
 * diversity and distance to stress the ETM to ETR/TPIU path.
 *
 * Two independent knobs:
 *   N_TARGETS   : unique branch targets. N_TARGETS > 3 defeats the ETM's
 *                 exact-match address cache (3 history slots), forcing a
 *                 real address packet on every call instead of a free hit.
 *   STUB_STRIDE : byte alignment between stub targets (power of two, or 0
 *                 for the compiler's default packing), tests whether
 *                 address distance forces a Long (9-byte) address packet
 *                 instead of a cheaper Short one.
 *
 * The overflow count is read from the FPGA side (fuzzsight extractor).
 * This binary only needs to run the pattern and report timing.
 *
 * Usage:
 *   ./bench_nN_sS          (built with N_TARGETS=N, STUB_STRIDE=S)
 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifndef N_TARGETS
#define N_TARGETS 64
#endif

#ifndef STUB_STRIDE
#define STUB_STRIDE 4096
#endif

#ifndef ITERS
#define ITERS 100000
#endif

#ifdef CHAIN
/* ------------------------------------------------------------------ */
/* Chain mode: t0..t(N_TARGETS-1) are hand-written in chain_nN_sS.S   */
/* (see gen_chain.py). One call to t0 unconditionally BRs through all */
/* N_TARGETS stubs ("a lap") and returns via a single RET at the far  */
/* end                                                                */
/* ------------------------------------------------------------------ */
extern void t0(void);

static void bench(void)
{
    unsigned long laps = ((unsigned long)ITERS + N_TARGETS - 1) / N_TARGETS;
    for (unsigned long i = 0; i < laps; i++) {
        t0();
    }
}

#else
/* ------------------------------------------------------------------ */
/* Loop mode: 64 unique noinline stubs Each has a distinct body so    */
/* the linker places them at separate addresses, STUB_STRIDE bytes    */
/* apart (0 = no forced alignment).                                   */
/* ------------------------------------------------------------------ */
#if STUB_STRIDE == 0
#define STUB(n) \
    __attribute__((noinline, optimize("O0"))) \
    static void t##n(void) { asm volatile("" ::: "memory"); }
#else
#define STUB(n) \
    __attribute__((noinline, optimize("O0"), aligned(STUB_STRIDE))) \
    static void t##n(void) { asm volatile("" ::: "memory"); }
#endif

STUB( 0) STUB( 1) STUB( 2) STUB( 3) STUB( 4) STUB( 5) STUB( 6) STUB( 7)
STUB( 8) STUB( 9) STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15)
STUB(16) STUB(17) STUB(18) STUB(19) STUB(20) STUB(21) STUB(22) STUB(23)
STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29) STUB(30) STUB(31)
STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39)
STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47)
STUB(48) STUB(49) STUB(50) STUB(51) STUB(52) STUB(53) STUB(54) STUB(55)
STUB(56) STUB(57) STUB(58) STUB(59) STUB(60) STUB(61) STUB(62) STUB(63)

typedef void (*fp_t)(void);

/* All 64 stubs in order */
static fp_t all_targets[64] = {
     t0,  t1,  t2,  t3,  t4,  t5,  t6,  t7,
     t8,  t9, t10, t11, t12, t13, t14, t15,
    t16, t17, t18, t19, t20, t21, t22, t23,
    t24, t25, t26, t27, t28, t29, t30, t31,
    t32, t33, t34, t35, t36, t37, t38, t39,
    t40, t41, t42, t43, t44, t45, t46, t47,
    t48, t49, t50, t51, t52, t53, t54, t55,
    t56, t57, t58, t59, t60, t61, t62, t63,
};

/* Working table, filled from all_targets[] before each run */
static fp_t table[N_TARGETS];

#ifndef UNROLL
#define UNROLL 8
#endif

/* Routed through a volatile so gcc can't prove (i+k) % N_TARGETS is a
 * loop-invariant constant per unrolled call site whenever N_TARGETS
 * divides UNROLL (true for every N_TARGETS in {1,2,4,8} */
static volatile unsigned idx_sink;

__attribute__((optimize("O1")))
static void bench(void)
{
    unsigned i = 0;
    unsigned main_iters = ((unsigned)ITERS / UNROLL) * UNROLL;

    for (; i < main_iters; i += UNROLL) {
#define CALL(k) do { idx_sink = (i + (k)) % N_TARGETS; table[idx_sink](); } while (0)
        CALL(0); CALL(1); CALL(2); CALL(3);
        CALL(4); CALL(5); CALL(6); CALL(7);
#undef CALL
    }
    for (; i < (unsigned)ITERS; i++) {
        idx_sink = i % N_TARGETS;
        table[idx_sink]();
    }
}
#endif /* CHAIN */

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void)
{
#ifndef CHAIN
    for (int i = 0; i < N_TARGETS; i++)
        table[i] = all_targets[i];
#endif

    /* Warmup: cold-cache effects skew the first run */
    bench();

    uint64_t ts0 = now_ns();
    bench();
    uint64_t ts1 = now_ns();

    uint64_t elapsed_ns = ts1 - ts0;
    double   elapsed_s  = (double)elapsed_ns * 1e-9;
    double   br_per_s   = (double)ITERS / elapsed_s;
#ifdef CHAIN
    /* Each hop is a single indirect BR = 1 address packet at up to 9 bytes */
    double   est_bw_mbs = br_per_s * 1.0 * 9.0 / 1e6;
#else
    /* Each indirect call + return = 2 address packets at up to 9 bytes each */
    double   est_bw_mbs = br_per_s * 2.0 * 9.0 / 1e6;
#endif

    printf("n_targets=%-3d  stub_stride=%-5d  iters=%d  mode=%s\n",
           N_TARGETS, STUB_STRIDE, ITERS,
#ifdef CHAIN
           "chain"
#else
           "loop"
#endif
           );
    printf("elapsed=%.6f s  branches/s=%.0f\n", elapsed_s, br_per_s);
    printf("est_trace_bw=%.1f MB/s  (%s addr pkts x 9 B worst-case)\n",
           est_bw_mbs,
#ifdef CHAIN
           "1"
#else
           "2"
#endif
           );

    return 0;
}
