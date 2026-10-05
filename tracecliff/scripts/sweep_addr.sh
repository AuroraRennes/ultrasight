#!/usr/bin/env bash
# Every bench_addr binary, chain and loop, at bb=1: stall off and 3, both sinks.
#
# Usage: sweep_addr.sh [-o STORAGE] [reps]   (default: 3 reps)
#
set -uo pipefail
. "$(dirname "$0")/lib.sh"
ensure_root "$@"
parse_options "$@"
setup sweep_addr
REPS=${ARGS[0]:-3}

smoke_test bin/addr_chain/bench_chain_n1_s0_i10000000

FACTORS="--factor stall=off,3 --factor etr=0,1 --factor bb=1 --factor addrfilter=etm"
for kind in chain loop; do
    echo "--- $kind ---"
    python3 -u -m tracecliff sweep-addr --kind "$kind" --cs-flags "-b ZCU-104 -c 0" \
      --runs "$REPS" $FACTORS
    eval "${kind}_rc=$?"
done

collect addr
echo "=== SWEEP_ADDR_DONE chain=$chain_rc loop=$loop_rc $(date -Is) ==="
