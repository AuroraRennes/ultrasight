#!/bin/bash
N=10
TRACEE=tests/base_basicmath
TRACEE_ARGS=100

sum_trace=0
sum_notrace=0

echo "======= FULL TRACE RUNS ======="
for i in $(seq 1 $N); do
    t=$(TRACEE=$TRACEE TRACEE_ARGS=$TRACEE_ARGS make trace 2>&1 \
        | grep "Child execution time" \
        | awk '{print $(NF-1)}')
    sum_trace=$(echo "$sum_trace + $t" | bc)
    echo "Run $i: $t"
done

echo ""
echo "======= NO TRACE RUNS ======="
for i in $(seq 1 $N); do
    t=$(TRACEE=$TRACEE TRACEE_ARGS=$TRACEE_ARGS CS_TRACE_FLAGS="--no-trace" make trace 2>&1 \
        | grep "Child execution time" \
        | awk '{print $(NF-1)}')
    sum_notrace=$(echo "$sum_notrace + $t" | bc)
    echo "Run $i: $t"
done

avg_trace=$(echo "scale=6; $sum_trace / $N" | bc)
avg_notrace=$(echo "scale=6; $sum_notrace / $N" | bc)
overhead=$(echo "scale=6; $avg_trace - $avg_notrace" | bc)
overhead_pct=$(echo "scale=4; ($avg_trace - $avg_notrace) / $avg_notrace * 100" | bc)

echo ""
echo "======= RESULTS ======="
echo "Average child time (trace):    $avg_trace seconds"
echo "Average child time (no-trace): $avg_notrace seconds"
echo "Overhead:                      $overhead seconds"
echo "Overhead (%):                  $overhead_pct %"