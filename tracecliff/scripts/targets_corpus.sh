#!/usr/bin/env bash
# Every staged corpus input of every target under the three bb=1 configurations
# (etm all, none all, none in), stall off and 3, both sinks; one CSV row per run.
#
# Usage: targets_corpus.sh -t TARGET_BIN [-o STORAGE] [reps] [target...]   (3 reps, all)
#
set -uo pipefail
. "$(dirname "$0")/lib.sh"
ensure_root "$@"
parse_options "$@"
if [ -z "$TARGET_BIN" ]; then
    echo "usage: targets_corpus.sh -t TARGET_BIN [-o STORAGE] [reps] [target...]" >&2
    exit 2
fi
setup targets_corpus
REPS=${ARGS[0]:-3}
ONLY=("${ARGS[@]:1}")
CORPUS=$ROOT/corpus
TRASH_OUT=$TMPDIR/targets_corpus_trash_out.$$
# bsdtar asks for libbz2.so.1.0, the board ships the same library as .so.1
export LD_LIBRARY_PATH=$ROOT/lib

# binary|arguments, @@ the input
TARGETS=(
    "objdump|--dwarf-check -C -g -f -dwarf -x @@"
    "readelf|-a @@"
    "nm-new|-C @@"
    "bsdtar|-xOf @@"
    "nasm|-f elf -o $TRASH_OUT @@"
    "bison|@@"
    "tiff2bw|@@ $TRASH_OUT"
    "tiffinfo|@@"
    "xmllint|@@"
    "tic|@@"
)
# addrfilter|bbfilter
ARMS=("etm|all" "none|all" "none|in")

out=$STORAGE/results/targets_corpus_bb1_$STAMP.csv
row=$(mktemp)
header=""
for target in "${TARGETS[@]}"; do
    name=${target%%|*}; args=${target#*|}
    bin=$TARGET_BIN/$name
    if [ ${#ONLY[@]} -gt 0 ] && [[ " ${ONLY[*]} " != *" $name "* ]]; then
        continue
    fi
    if [ ! -x "$bin" ] || [ ! -d "$CORPUS/$name" ]; then
        echo "SKIP $name: no binary at $bin or no corpus in $CORPUS/$name"
        continue
    fi
    # libtool wrappers are shell scripts: tracing one traces the shell
    if [ "$(head -c4 "$bin" | tail -c3)" != "ELF" ]; then
        echo "SKIP $name: $bin is not an ELF, a libtool wrapper?"
        continue
    fi
    for input in "$CORPUS/$name"/*; do
        # stage_corpus.sh names each input by its queue id
        id=$(basename "$input")
        cmd="$bin ${args//@@/$input}"
        for arm in "${ARMS[@]}"; do
            af=${arm%%|*}; bf=${arm#*|}
            for etr in 0 1; do
                for stall in off 3; do
                    for run in $(seq 1 "$REPS"); do
                        rm -f "$row"
                        # shellcheck disable=SC2086  # cmd is split into argv on purpose
                        if ! timeout 60 "$CS_TRACE" -b ZCU-104 -c 0 \
                                --stall "$stall" --useetr "$etr" --branchbroadcast 1 \
                                --addrfilter "$af" --bbfilter "$bf" \
                                -o "$row" -- $cmd >/dev/null 2>&1; then
                            echo "FAIL $name $id $af/$bf etr=$etr stall=$stall run=$run"
                            continue
                        fi
                        if [ -z "$header" ]; then
                            header=$(head -1 "$row")
                            echo "target,input,addrfilter,bbfilter,etr,stall,bb,run,$header" > "$out"
                        fi
                        echo "$name,$id,$af,$bf,$etr,$stall,1,$run,$(tail -1 "$row")" >> "$out"
                    done
                done
            done
        done
        echo "$name $id done"
    done
    echo "$name done $(date -Is)"
done
rm -f "$row" "$TRASH_OUT"

echo "$out"
echo "=== TARGETS_CORPUS_DONE $(date -Is) ==="
