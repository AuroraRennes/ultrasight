#!/usr/bin/env bash
# Copy what targets_corpus.sh needs onto the board's disk, inputs named by queue id.
#
# Usage: stage_corpus.sh [-o STORAGE] CORPUS [stage dir]   (default: STORAGE/stage)
#   CORPUS holds one <target>-<yyyy-mm-dd>/minset/ directory per target.
#
set -euo pipefail
. "$(dirname "$0")/lib.sh"
parse_options "$@"
SRC=${ARGS[0]:?usage: stage_corpus.sh [-o STORAGE] CORPUS [stage dir]}
STAGE=${ARGS[1]:-$STORAGE/stage}
mkdir -p "$STAGE/scripts" "$STAGE/lib"
cp "$CS_TRACE" "$STAGE/"
cp "$SCRIPT_DIR/targets_corpus.sh" "$SCRIPT_DIR/check_clock.sh" "$SCRIPT_DIR/lib.sh" "$STAGE/scripts/"
rm -rf "$STAGE/corpus"
for minset in "$SRC"/*/minset; do
    campaign=$(basename "$(dirname "$minset")")
    name=${campaign%-[0-9]*-[0-9]*-[0-9]*}
    mkdir -p "$STAGE/corpus/$name"
    for input in "$minset"/*; do
        # AFL++ names carry commas and colons: keep the id:NNNNNN digits only
        id=$(basename "$input"); id=${id#id:}; id=${id%%,*}
        cp "$input" "$STAGE/corpus/$name/$id"
    done
    echo "$name: $(find "$STAGE/corpus/$name" -type f | wc -l) inputs" >&2
done
ln -sf /lib/libbz2.so.1 "$STAGE/lib/libbz2.so.1.0"
echo "$STAGE/scripts/targets_corpus.sh"
