#!/bin/bash
# The end-to-end smoke test: the real app (Vulkan viewport, decoder, Track
# subject, plate scan, NVENC export) on a private headless Sway, driven by
# gpu_smoke.steps through the app's ORBIS_SMOKE_TEST hook (src/ui/
# SmokeTest.h) -- by shortcut, action and button names, not by screen
# position. Then checks the results: a picture in the viewport, frames while
# playing, the tracked path, the scan, and the exported file's streams,
# length and picture. Used by the GPU workflow (.github/workflows/gpu.yml);
# needs the NVIDIA GPU, sway, ffmpeg and the clip under /footage/ (or in
# sample_footage/). Takes about a minute.
#
# Sway composites on the GPU only with access to its render node
# (/dev/dri/renderD*), and NVIDIA's Vulkan presents to a Wayland window only
# then. The GPU runner's jobs don't have it (only the host's login session
# does), so there the viewport never draws: the steps tagged "# viewport" and
# their checks are left out, and everything else -- decode, stitch, frames
# handed to the viewport, tracking, scan, export -- is still checked.
#
# Usage: gpu_smoke.sh BUILD_DIR OUT_DIR [SMOKE_ORP]
# OUT_DIR gets app.log, sway.log, results.txt ("name=value" lines: what the
# app measured, then "check_<name>=ok|FAIL ..."), the viewport grabs, the
# export and the saved project. Exits non-zero if a check failed.
set -uo pipefail

here=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
build=$(realpath "$1")
out=$(realpath -m "$2")
orp=$(realpath "${3:-$here/smoke.orp}")
mkdir -p "$out"
R=$out/results.txt
: > "$R"

# The app saves the project: work on a copy. Its own settings, caches and
# project library stay in OUT_DIR too. Outside the GPU runner's container
# (no /footage), the clip is looked for in the source tree's sample_footage/.
cp "$orp" "$out/smoke.orp"
clip=$(sed -nE 's/.*"source": "([^"]+)".*/\1/p' "$orp" | head -1)
if [[ ! -f $clip && $clip == /footage/* ]]; then
    local_footage=$(realpath "$here/../../sample_footage")
    sed -i "s|\"/footage/|\"$local_footage/|" "$out/smoke.orp"
    clip=$local_footage/${clip#/footage/}
fi
[[ -f $clip ]] || { echo "test clip missing: $clip" >&2; exit 1; }
export HOME=$out/home XDG_CONFIG_HOME=$out/home/config XDG_DATA_HOME=$out/home/data \
    XDG_CACHE_HOME=$out/home/cache
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
viewport=0
for node in /dev/dri/renderD*; do
    [[ -r $node && -w $node ]] && viewport=1
done
if ((viewport)); then
    sed "s|@OUT@|$out|g" "$here/gpu_smoke.steps" > "$out/steps.txt"
else
    echo "viewport=skipped (no access to the GPU's render node)" >> "$R"
    sed "s|@OUT@|$out|g; /# viewport\s*$/d" "$here/gpu_smoke.steps" > "$out/steps.txt"
fi

# --- a private headless Sway, one 1920x1080 output
XDG_RUNTIME_DIR=$(mktemp -d)
export XDG_RUNTIME_DIR
chmod 700 "$XDG_RUNTIME_DIR"
printf 'output HEADLESS-1 mode 1920x1080@60Hz\ndefault_border none\nxwayland disable\n' \
    > "$XDG_RUNTIME_DIR/sway.conf"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$XDG_RUNTIME_DIR/sway.conf" > "$out/sway.log" 2>&1 &
sway_pid=$!
cleanup() {
    kill "$sway_pid" 2> /dev/null
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT
for _ in $(seq 50); do
    display=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-*' -type s -printf '%f\n' | head -1)
    [[ -n $display ]] && break
    sleep 0.2
done
[[ -n $display ]] || { echo "sway didn't start (sway.log)" >&2; exit 1; }
export WAYLAND_DISPLAY=$display

# --- the app. No sound server: playback runs on the wall clock.
ORBIS_SMOKE_TEST=$out/steps.txt ORBIS_VERBOSE=1 PULSE_SERVER=unix:/nonexistent \
    timeout 1800 "$build/orbis-reframe" "$out/smoke.orp" > "$out/app.log" 2>&1
status=$?
sed -nE 's/^\[smoke\] //p' "$out/app.log" >> "$R"
echo "exit=$status" >> "$R"

# --- checks
failed=0
get() { sed -nE "s/^$1=//p" "$R" | tail -1; }
# check NAME CONDITION...: CONDITION is a [[ ]] expression, evaluated as is.
check() {
    local name=$1
    shift
    if eval "[[ $* ]]" 2> /dev/null; then
        echo "check_$name=ok" >> "$R"
    else
        echo "check_$name=FAIL ($*)" >> "$R"
        failed=1
    fi
}
num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]] && echo "$1" || echo -1; }
int() { printf '%.0f' "$(num "$1")"; }

check quit "'$status' == 0 && '$(get script)' == done"
check first_frame "$(int "$(get first_frame)") -ge 0"
# 5 s of 29.97 fps playback: most frames reach the viewport and the
# playhead keeps up with the clock.
check play_decoded "$(int "$(get play_decoded)") -ge 120"
check play_advance "$(int "$(get play_advance_ms)") -ge 4500"
if ((viewport)); then
    check first_drawn "$(int "$(get first_drawn)") -ge 0"
    check play_drawn "$(int "$(get play_drawn)") -ge 120"
    # A picture, not a flat colour: the road scene has plenty of contrast.
    for g in paused played; do
        check "${g}_picture" "$(int "$(get ${g}_luma_stddev)") -ge 20 &&
            $(int "$(get ${g}_luma_mean)") -ge 20 && $(int "$(get ${g}_luma_mean)") -le 235"
    done
fi
check track "$(int "$(get track_samples)") -ge 30 && $(int "$(get track_keys)") -ge 2 &&
    -z '$(get track_error)'"
# The whole 10 s trimmed part scanned.
check scan "$(int "$(get scan_frames)") -ge 280 && -z '$(get scan_error)'"
check export "'$(get export)' == 'Export complete' && -s '$out/smoke.mp4'"

# The exported file: one 1920x1080 video and one audio stream, as long as
# the timeline, and a picture in its middle frame.
if [[ -s $out/smoke.mp4 ]]; then
    probe() { ffprobe -v error -of default=nw=1:nk=1 "$@" "$out/smoke.mp4"; }
    video=$(probe -select_streams v -show_entries stream=codec_name,width,height | paste -sd,)
    audio=$(probe -select_streams a -show_entries stream=codec_name | paste -sd,)
    duration_ms=$(probe -show_entries format=duration | awk '{printf "%d", $1 * 1000}')
    {
        echo "export_video=$video"
        echo "export_audio=$audio"
        echo "export_duration_ms=$duration_ms"
    } >> "$R"
    check export_video "'$video' =~ ^(h264|hevc),1920,1080$"
    check export_audio "-n '$audio' && '$audio' != *,*"
    timeline_ms=$(int "$(get timeline_ms)")
    check export_length "$timeline_ms -gt 0 && $(( duration_ms - timeline_ms )) -le 100 &&
        $(( timeline_ms - duration_ms )) -le 100"
    # The middle frame's luma: its spread (YHIGH - YLOW: the 90th - 10th
    # percentile) shows a picture, not a flat colour.
    stats=$(ffmpeg -v error -ss "$(( timeline_ms / 2000 ))" -i "$out/smoke.mp4" -frames:v 1 \
        -vf signalstats,metadata=print:file=- -f null - 2> /dev/null)
    # (the filter prints the frame's metadata twice)
    ylow=$(sed -nE 's/.*signalstats\.YLOW=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
    yhigh=$(sed -nE 's/.*signalstats\.YHIGH=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
    echo "export_frame_luma_spread=$(( $(int "$yhigh") - $(int "$ylow") ))" >> "$R"
    check export_picture "$(( $(int "$yhigh") - $(int "$ylow") )) -ge 40"
fi

cat "$R"
exit $failed
