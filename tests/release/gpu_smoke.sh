#!/bin/bash
# The end-to-end smoke test: the real app (Vulkan viewport, decoder, Track
# subject, plate scan, NVENC export) on a private headless Sway, driven by
# gpu_smoke.steps through the app's ORBIS_SMOKE_TEST hook (src/ui/
# SmokeTest.h) -- by shortcut, action and button names, not by screen
# position. Then checks the results: a picture in the viewport, frames while
# playing, the tracked path, the scan, and the exported file's streams,
# length and picture. Used by the GPU workflow (.github/workflows/gpu.yml)
# on the dev build and by the release test (scripts/release_test.sh, through
# run_gui_test.sh) on the installed packages. Needs the NVIDIA GPU, sway and
# the clip under /footage/ (or in sample_footage/); the checks need ffmpeg.
# Takes about a minute.
#
#   gpu_smoke.sh [--sound] [--no-check] APP OUT_DIR [SMOKE_ORP]
#   gpu_smoke.sh --check OUT_DIR
#
# APP is a build folder (with orbis-reframe in it) or the program to start.
# --sound starts PipeWire with a null sink as the output and records it: the
# smoke project plays with "Voice only", so sound reaching the sink and
# ONNX Runtime loaded are checked too (needs pipewire, pipewire-pulse,
# wireplumber, pulseaudio-utils, dbus, python3). Without it there's no sound
# server and playback runs on the wall clock. --no-check only runs the app;
# --check judges an OUT_DIR such a run left (the release test runs the app
# in its container, without ffmpeg, and checks on the host).
#
# OUT_DIR gets app.log, sway.log, results.txt ("name=value" lines: what the
# run measured, then "check_<name>=ok|FAIL ..."), the viewport grabs, a
# fail-<line>.png for a failed step, the export and the saved project.
# Exits non-zero if a check failed.
#
# Sway composites on the GPU only with access to its render node
# (/dev/dri/renderD*), and NVIDIA's Vulkan presents to a Wayland window only
# then. The GPU runner's jobs don't have it (only the host's login session
# does), so there the viewport never draws: the steps tagged "# viewport" and
# their checks are left out, and everything else -- decode, stitch, frames
# handed to the viewport, tracking, scan, export -- is still checked.
set -uo pipefail

here=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
sound=0 run=1 judge=1
while [[ ${1:-} == --* ]]; do
    case $1 in
        --sound) sound=1 ;;
        --no-check) judge=0 ;;
        --check) run=0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

# --- running the app ------------------------------------------------------
run_app() {
    local app=$1 orp
    orp=$(realpath "${3:-$here/smoke.orp}")
    [[ -d $app ]] && app=$app/orbis-reframe
    : > "$R"

    # The app saves the project: work on a copy. Its own settings, caches
    # and project library stay in OUT_DIR too. Outside the GPU runner's
    # container (no /footage), the clip is looked for in the source tree's
    # sample_footage/.
    # A run before this one's output would change this one: its caches
    # (a scan served from the cache), its export (the save dialog would ask
    # to replace it).
    rm -rf "$out/home" "$out/smoke.mp4" "$out"/fail-*.png "$out/paused.png" "$out/played.png"
    [[ $orp -ef $out/smoke.orp ]] || cp "$orp" "$out/smoke.orp"
    local clip local_footage
    clip=$(sed -nE 's/.*"source": "([^"]+)".*/\1/p' "$orp" | head -1)
    if [[ ! -f $clip && $clip == /footage/* ]]; then
        local_footage=$(realpath -m "$here/../../sample_footage")
        sed -i "s|\"/footage/|\"$local_footage/|" "$out/smoke.orp"
        clip=$local_footage/${clip#/footage/}
    fi
    [[ -f $clip ]] || { echo "test clip missing: $clip" >&2; return 1; }
    export HOME=$out/home XDG_CONFIG_HOME=$out/home/config XDG_DATA_HOME=$out/home/data \
        XDG_CACHE_HOME=$out/home/cache
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

    local viewport=0 node
    for node in /dev/dri/renderD*; do
        [[ -r $node && -w $node ]] && viewport=1
    done
    if ((viewport)); then
        sed "s|@OUT@|$out|g" "$here/gpu_smoke.steps" > "$out/steps.txt"
    else
        echo "viewport=skipped (no access to the GPU's render node)" >> "$R"
        sed "s|@OUT@|$out|g; /# viewport\s*$/d" "$here/gpu_smoke.steps" > "$out/steps.txt"
    fi

    XDG_RUNTIME_DIR=$(mktemp -d)
    export XDG_RUNTIME_DIR
    chmod 700 "$XDG_RUNTIME_DIR"
    local pids=()
    # shellcheck disable=SC2317,SC2329 # called by the trap
    cleanup() {
        kill "${pids[@]}" 2> /dev/null
        wait 2> /dev/null
        rm -rf "$XDG_RUNTIME_DIR"
    }
    trap cleanup EXIT

    # A session bus of its own that starts no services (a desktop's bus
    # would start portals and gvfs for the app, with FUSE mounts in the
    # runtime folder); none at all where there's no dbus-daemon.
    unset DBUS_SESSION_BUS_ADDRESS
    if command -v dbus-daemon > /dev/null; then
        cat > "$XDG_RUNTIME_DIR/bus.conf" << EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$XDG_RUNTIME_DIR/bus</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
        pids+=("$(dbus-daemon --config-file="$XDG_RUNTIME_DIR/bus.conf" --fork --print-pid=1)")
        export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
    fi

    # Sound: PipeWire with a null sink as the default output.
    local pulse=(PULSE_SERVER=unix:/nonexistent)
    if ((sound)); then
        pipewire > "$out/pipewire.log" 2>&1 &
        pids+=($!)
        sleep 1
        wireplumber > /dev/null 2>&1 &
        pids+=($!)
        pipewire-pulse > /dev/null 2>&1 &
        pids+=($!)
        sleep 2
        pactl load-module module-null-sink sink_name=smokesink > /dev/null
        pactl set-default-sink smokesink
        pulse=()
    fi

    # A headless Sway, one 1920x1080 output.
    printf 'output HEADLESS-1 mode 1920x1080@60Hz\ndefault_border none\nxwayland disable\n' \
        > "$XDG_RUNTIME_DIR/sway.conf"
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        sway -c "$XDG_RUNTIME_DIR/sway.conf" > "$out/sway.log" 2>&1 &
    pids+=($!)
    local display=
    for _ in $(seq 50); do
        display=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-*' -type s -printf '%f\n' | head -1)
        [[ -n $display ]] && break
        sleep 0.2
    done
    [[ -n $display ]] || { echo "sway didn't start (sway.log)" >&2; return 1; }
    export WAYLAND_DISPLAY=$display

    # Everything the sink plays, for the whole run.
    local rec=$out/sound.raw
    if ((sound)); then
        parec -d smokesink.monitor --format=s16le --rate=48000 --channels=2 --raw > "$rec" &
        pids+=($!)
    fi

    env ORBIS_SMOKE_TEST="$out/steps.txt" ORBIS_VERBOSE=1 "${pulse[@]}" \
        timeout 1800 "$app" "$out/smoke.orp" > "$out/app.log" 2>&1
    local status=$?
    sed -nE 's/^\[smoke\] //p' "$out/app.log" >> "$R"
    echo "exit=$status" >> "$R"
    if ((sound)); then
        sleep 0.5
        echo "sound_peak=$(python3 -c '
import array, sys
a = array.array("h", open(sys.argv[1], "rb").read())
print(max((abs(x) for x in a), default=0))' "$rec")" >> "$R"
        rm -f "$rec"
    fi
    cleanup
    trap - EXIT
}

# --- checking what a run left -------------------------------------------
check_run() {
    # Again from what the run measured, if checked before.
    sed -i -E '/^(check_|export_(video|audio|duration_ms|frame_luma_spread)=)/d' "$R"
    local failed=0
    get() { sed -nE "s/^$1=//p" "$R" | tail -1; }
    # check NAME CONDITION...: CONDITION is a [[ ]] expression with the
    # measured values filled in, evaluated as is and recorded with the verdict.
    check() {
        local name=$1 cond -
        shift
        set -f # the conditions' patterns (*,*) aren't file names
        # shellcheck disable=SC2048,SC2086 # split on purpose: one line
        cond=$(printf '%s ' $*)
        cond=${cond% }
        if eval "[[ $cond ]]" 2> /dev/null; then
            echo "check_$name=ok ($cond)" >> "$R"
        else
            echo "check_$name=FAIL ($cond)" >> "$R"
            failed=1
        fi
    }
    num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]] && echo "$1" || echo -1; }
    int() { printf '%.0f' "$(num "$1")"; }

    check quit "'$(get exit)' == 0 && '$(get script)' == done"
    check first_frame "$(int "$(get first_frame)") -ge 0"
    # 5 s of 29.97 fps playback: most frames reach the viewport and the
    # playhead keeps up with the clock.
    check play_decoded "$(int "$(get play_decoded)") -ge 120"
    check play_advance "$(int "$(get play_advance_ms)") -ge 4500"
    if [[ -z $(get viewport) ]]; then
        check first_drawn "$(int "$(get first_drawn)") -ge 0"
        check play_drawn "$(int "$(get play_drawn)") -ge 120"
        # A picture, not a flat colour: the road scene has plenty of contrast.
        for g in paused played; do
            check "${g}_picture" "$(int "$(get "${g}_luma_stddev")") -ge 20 &&
                $(int "$(get "${g}_luma_mean")") -ge 20 && $(int "$(get "${g}_luma_mean")") -le 235"
        done
    fi
    if [[ -n $(get sound_peak) ]]; then
        # "Voice only": ONNX Runtime runs the voice model while playing.
        check voice_model "$(int "$(get play_onnxruntime)") -gt 0"
        check sound "$(int "$(get sound_peak)") -gt 0"
    fi
    check track "$(int "$(get track_samples)") -ge 30 && $(int "$(get track_keys)") -ge 2 &&
        -z '$(get track_error)'"
    # The whole 10 s trimmed part scanned.
    check scan "$(int "$(get scan_frames)") -ge 280 && -z '$(get scan_error)'"
    check export "'$(get export)' == 'Export complete' &&
        $(stat -c %s "$out/smoke.mp4" 2> /dev/null || echo 0) -gt 0"

    # The exported file: one 1920x1080 HEVC video (the dialog's default) and
    # one audio stream, as long as the timeline, and a picture in its middle
    # frame.
    if [[ -s $out/smoke.mp4 ]]; then
        probe() { ffprobe -v error -of default=nw=1:nk=1 "$@" "$out/smoke.mp4"; }
        local video audio duration_ms timeline_ms stats ylow yhigh
        video=$(probe -select_streams v -show_entries stream=codec_name,width,height | paste -sd,)
        audio=$(probe -select_streams a -show_entries stream=codec_name | paste -sd,)
        duration_ms=$(probe -show_entries format=duration | awk '{printf "%d", $1 * 1000}')
        {
            echo "export_video=$video"
            echo "export_audio=$audio"
            echo "export_duration_ms=$duration_ms"
        } >> "$R"
        check export_video "'$video' == hevc,1920,1080"
        check export_audio "-n '$audio' && '$audio' != *,*"
        timeline_ms=$(int "$(get timeline_ms)")
        check export_length "$timeline_ms -gt 0 && $((duration_ms - timeline_ms)) -le 100 &&
            $((timeline_ms - duration_ms)) -le 100"
        # The middle frame's luma: its spread (YHIGH - YLOW: the 90th - 10th
        # percentile) shows a picture, not a flat colour.
        stats=$(ffmpeg -v error -ss "$((timeline_ms / 2000))" -i "$out/smoke.mp4" -frames:v 1 \
            -vf signalstats,metadata=print:file=- -f null - 2> /dev/null)
        # (the filter prints the frame's metadata twice)
        ylow=$(sed -nE 's/.*signalstats\.YLOW=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
        yhigh=$(sed -nE 's/.*signalstats\.YHIGH=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
        echo "export_frame_luma_spread=$(($(int "$yhigh") - $(int "$ylow")))" >> "$R"
        check export_picture "$(($(int "$yhigh") - $(int "$ylow"))) -ge 40"
    fi
    return $failed
}

if ((run)); then
    [[ $# -ge 2 ]] || { echo "usage: gpu_smoke.sh [--sound] [--no-check] APP OUT_DIR [SMOKE_ORP]" >&2; exit 2; }
    out=$(realpath -m "$2")
    mkdir -p "$out"
    R=$out/results.txt
    run_app "$@" || exit 1
else
    [[ $# -eq 1 ]] || { echo "usage: gpu_smoke.sh --check OUT_DIR" >&2; exit 2; }
    out=$(realpath "$1")
    R=$out/results.txt
fi
((judge)) || exit 0
check_run
failed=$?
cat "$R"
exit $failed
