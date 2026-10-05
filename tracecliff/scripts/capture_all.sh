#!/usr/bin/env bash
# The whole bb=1 capture: sweep_addr, call_fill, and targets_corpus when -t is given.
#
# Usage: capture_all.sh [-o STORAGE] [-t TARGET_BIN]   (each script keeps its default reps)
#
set -uo pipefail
. "$(dirname "$0")/lib.sh"
ensure_root "$@"
parse_options "$@"
mkdir -p "$STORAGE" || exit 1
STORAGE=$(cd "$STORAGE" && pwd)

rc=0
run() {
    echo "=== $* $(date -Is) ==="
    "$SCRIPT_DIR/$1" -o "$STORAGE" "${@:2}" || { rc=1; echo "FAILED: $1" >&2; }
}

run sweep_addr.sh
run call_fill.sh
[ -n "$TARGET_BIN" ] && run targets_corpus.sh -t "$TARGET_BIN"
echo "=== CAPTURE_ALL_DONE rc=$rc $(date -Is) ==="
exit $rc
