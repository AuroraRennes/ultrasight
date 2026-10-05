# Shared by the capture scripts: source it, ensure_root "$@", parse_options "$@", setup NAME.
# Storage (-o DIR, $TRACECLIFF_STORAGE, else ROOT/storage) holds logs/, results/, tmp/.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
STORAGE=${TRACECLIFF_STORAGE:-$ROOT/storage}
TARGET_BIN=${TARGET_BIN:-}
CS_TRACE=$ROOT/../cs-trace
[ -x "$ROOT/cs-trace" ] && CS_TRACE=$ROOT/cs-trace

# cs-trace and the NFS results need root: re-run under sudo, which asks on the terminal.
ensure_root() {
    [ "$(id -u)" -eq 0 ] && return
    echo "root needed: re-running under sudo" >&2
    exec sudo -E "$0" "$@"
}

# Sets STORAGE (-o) and TARGET_BIN (-t), the operands are left in ARGS.
parse_options() {
    local opt
    OPTIND=1
    while getopts o:t: opt; do
        case $opt in
            o) STORAGE=$OPTARG ;;
            t) TARGET_BIN=$OPTARG ;;
            *) exit 2 ;;
        esac
    done
    shift $((OPTIND - 1))
    ARGS=("$@")
}

# Creates the storage tree, sends all output to its log and checks the clock.
setup() {
    mkdir -p "$STORAGE"/{logs,results,tmp} || exit 1
    STORAGE=$(cd "$STORAGE" && pwd)
    export TMPDIR=$STORAGE/tmp
    STAMP=$(date -u +%Y%m%d_%H%M%S)
    MARK=$TMPDIR/start_$STAMP
    touch "$MARK"
    exec > "$STORAGE/logs/${1}_$STAMP.log" 2>&1
    echo "=== start $(date -Is) on $(hostname), storage $STORAGE ==="
    "$SCRIPT_DIR"/check_clock.sh || exit 1
    cd "$ROOT" || exit 1
}

# One capture per sink of BINARY must complete, or a hung board costs the whole run.
smoke_test() {
    local etr smoke
    echo "--- smoke test: one capture per sink must complete ---"
    smoke=$(mktemp)
    for etr in 0 1; do
        if ! timeout 30 "$CS_TRACE" -b ZCU-104 -c 0 \
                --stall off --useetr "$etr" --branchbroadcast 1 --addrfilter etm \
                -o "$smoke" -- "$1"; then
            echo "ABORT: capture at etr=$etr did not complete (124 = hung)" >&2
            rm -f "$smoke"; exit 1
        fi
    done
    rm -f "$smoke"
    echo "smoke ok"
}

# Copies this run's results/PREFIX_*.csv into storage.
collect() {
    find "$ROOT/results" -name "${1}_*.csv" -newer "$MARK" -exec cp -v {} "$STORAGE/results/" \;
}
