#!/bin/bash
# The headless GPU tools on tests/release/smoke.orp's clip, 2 s from its
# trim-in: stitch, playback, view, export and plate scan each have to run
# through. Used by the release dev test (dev_test.sh) and the GPU workflow
# (.github/workflows/gpu.yml); needs the NVIDIA GPU and the clip under
# /footage/.
#
# Usage: gpu_tools.sh BUILD_DIR OUT_DIR [SMOKE_ORP]
# Each tool's output goes to OUT_DIR/<tool>.log, what it writes to
# OUT_DIR/tools/, and "<tool>=<exit status>" is appended to
# OUT_DIR/results.txt. Exits non-zero if any tool failed.
set -uo pipefail

build=$(realpath "$1")
out=$(realpath -m "$2")
orp=${3:-$(dirname "${BASH_SOURCE[0]}")/smoke.orp}
mkdir -p "$out/tools"

clip=$(sed -nE 's/.*"source": "([^"]+)".*/\1/p' "$orp" | head -1)
t0=$(sed -nE 's/.*"trimInMs": ([0-9]+).*/\1/p' "$orp" | head -1)
t1=$((t0 + 2000))
[[ -f $clip ]] || { echo "test clip missing: $clip" >&2; exit 1; }

failed=0
# step NAME CMD...: run CMD in OUT_DIR/tools with its output in NAME.log.
step() {
    local name=$1 status
    shift
    (cd "$out/tools" && "$@") > "$out/$name.log" 2>&1
    status=$?
    echo "$name=$status" | tee -a "$out/results.txt"
    [[ $status -eq 0 ]] || failed=1
}

step stitch_source_smoke "$build/stitch_source_smoke" "$clip" "$t0" "$t1"
step playback_bench "$build/playback_bench" --frames 30 "$clip"
step view_compare "$build/view_compare" --at "$t0" --out "$out/tools/view" "$clip"
step reframe_export "$build/reframe_export" "$clip" "$out/tools/reframe_export.mp4" \
    --trim-in "$t0" --trim-out "$t1"
step plate_scan "$build/plate_scan" --t0 "$t0" --dur 2 "$clip"
exit $failed
