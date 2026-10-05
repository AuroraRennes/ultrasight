#!/usr/bin/env bash
# Crossing rate against the ceiling at bb=1: addrfilter etm against none.
#
# Usage: call_fill.sh [-o STORAGE] [reps]   (default: 5 reps)
#
set -uo pipefail
. "$(dirname "$0")/lib.sh"
ensure_root "$@"
parse_options "$@"
setup call_fill
REPS=${ARGS[0]:-5}

smoke_test bin/call_fill/bench_call_fill_k0_i10000000

# etm ignores bbfilter (both comparators are the tracee text), so its in arm
# only repeats allm the none arms are the comparison.
echo "--- etm ---"
python3 -u -m tracecliff sweep-call --fill --cs-flags "-b ZCU-104 -c 0" \
  --runs "$REPS" --factor etr=0,1 --factor stall=off,3 --factor bb=1 \
  --factor addrfilter=etm --factor bbfilter=all
etm_rc=$?
echo "--- none, all and in ---"
python3 -u -m tracecliff sweep-call --fill --cs-flags "-b ZCU-104 -c 0" \
  --runs "$REPS" --factor etr=0,1 --factor stall=off,3 --factor bb=1 \
  --factor addrfilter=none --factor bbfilter=all
none_rc=$?

collect call_fill
echo "=== CALL_FILL_DONE etm=$etm_rc none=$none_rc $(date -Is) ==="
