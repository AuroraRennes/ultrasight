#ifndef TIMING_H
#define TIMING_H

#include <time.h>
#include <stdio.h>

#ifdef TIMING

#include <string.h>

/* Declaration of the clock */
#define TS_DECL(name) \
    struct timespec _ts_##name##_start, _ts_##name##_end

/* Start monitoring */
#define TS_START(name) \
    clock_gettime(CLOCK_MONOTONIC, &_ts_##name##_start)

/* Stop monitoring */
#define TS_STOP(name) \
    clock_gettime(CLOCK_MONOTONIC, &_ts_##name##_end)

/* Total column width of the label field at depth 0; each nesting level eats
   into it so printed timings stay aligned regardless of depth. */
#define TS_LABEL_WIDTH 48

/* Indentation string for a given nesting depth: "  " per level, plus a
   trailing "|-- " marker for any depth > 0. Depth is an explicit integer
   argument at the call site, never inferred from the label text. */
static inline const char *ts_indent(int depth)
{
    static char buf[32];
    int n = 0;
    for (int i = 0; i < depth && n < (int)sizeof(buf) - 5; i++) {
        buf[n++] = ' ';
        buf[n++] = ' ';
    }
    if (depth > 0 && n < (int)sizeof(buf) - 5) {
        buf[n++] = '|';
        buf[n++] = '-';
        buf[n++] = '-';
        buf[n++] = ' ';
    }
    buf[n] = '\0';
    return buf;
}

/* Print a duration (in us) as underscore-grouped thousands, indented and
   column-aligned according to depth. */
static inline void ts_print_fmt(FILE *f, int depth, const char *label, double us)
{
    long long iv = (long long)us;
    int frac = (int)((us - iv) * 1000 + 0.5);
    if (frac >= 1000) { ++iv; frac = 0; }

    char buf[32];
    if      (iv < 1000LL)        snprintf(buf, sizeof(buf), "%lld", iv);
    else if (iv < 1000000LL)     snprintf(buf, sizeof(buf), "%lld_%03lld", iv/1000, iv%1000);
    else if (iv < 1000000000LL)  snprintf(buf, sizeof(buf), "%lld_%03lld_%03lld",
                                           iv/1000000, (iv/1000)%1000, iv%1000);
    else                          snprintf(buf, sizeof(buf), "%lld_%03lld_%03lld_%03lld",
                                           iv/1000000000LL, (iv/1000000)%1000, (iv/1000)%1000, iv%1000);

    const char *indent = ts_indent(depth);
    int lw = TS_LABEL_WIDTH - (int)strlen(indent);
    if (lw < 8) lw = 8;

    fprintf(f, "[.] %s%-*s %6s.%03d us\n", indent, lw, label, buf, frac);
}

/* Print timestamp. depth: 0 = top level, 1+ = nested under the previous
   depth-0 entry (drives both indentation and column width). */
#define TS_PRINT(name, depth, label) \
    ts_print_fmt(stderr, (depth), (label), \
        (_ts_##name##_end.tv_sec  - _ts_##name##_start.tv_sec)  * 1e6 + \
        (_ts_##name##_end.tv_nsec - _ts_##name##_start.tv_nsec) / 1e3)

#define TS_MEASURE(name, depth, label, code) \
    do { TS_DECL(name); TS_START(name); code; TS_STOP(name); TS_PRINT(name, depth, label); } while (0)

/* Split start/print pair for wrapping a block that isn't a single expression. */
#define TS_OPEN(name, depth, label) \
    TS_DECL(name); \
    fprintf(stderr, "[.] %s%s\n", ts_indent(depth), (label)); \
    TS_START(name)

#define TS_CLOSE(name, depth, label) \
    TS_STOP(name); \
    TS_PRINT(name, depth, label)

#else  /* !FUZZSIGHT_TIMING */

#define TS_DECL(name)
#define TS_START(name)
#define TS_STOP(name)
#define TS_PRINT(name, depth, label)
#define TS_MEASURE(name, depth, label, code)  do { code; } while (0)
#define TS_OPEN(name, depth, label)
#define TS_CLOSE(name, depth, label)

#endif /* FUZZSIGHT_TIMING */

#endif /* TIMING_H */
