#ifndef TIMING_H
#define TIMING_H

#include <time.h>
#include <stdio.h>

#ifdef TIMING

/* Declaration of the clock */
#define TS_DECL(name) \
    struct timespec _ts_##name##_start, _ts_##name##_end

/* Start monitoring */
#define TS_START(name) \
    clock_gettime(CLOCK_MONOTONIC, &_ts_##name##_start)

/* Stop monitoring */
#define TS_STOP(name) \
    clock_gettime(CLOCK_MONOTONIC, &_ts_##name##_end)

/* Print timestamp */
#define TS_PRINT(name, label) \
    fprintf(stderr, "[.] %-32s %.3f us\n", (label), \
        (_ts_##name##_end.tv_sec  - _ts_##name##_start.tv_sec)  * 1e6 + \
        (_ts_##name##_end.tv_nsec - _ts_##name##_start.tv_nsec) / 1e3)

#define TS_MEASURE(name, label, code) \
    do { TS_DECL(name); TS_START(name); code; TS_STOP(name); TS_PRINT(name, label); } while (0)

#else  /* !FUZZSIGHT_TIMING */

#define TS_DECL(name)
#define TS_START(name)
#define TS_STOP(name)
#define TS_PRINT(name, label)
#define TS_MEASURE(name, label, code)  do { code; } while (0)

#endif /* FUZZSIGHT_TIMING */

#endif /* TIMING_H */