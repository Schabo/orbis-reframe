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
# --sound starts PipeWire with a null sink as the default output (listed
# after another null sink, so following the default is checked) and records
# it: the smoke project plays with "Voice only", so sound reaching the sink
# and ONNX Runtime loaded are checked too (needs pipewire, pipewire-pulse,
# wireplumber, pulseaudio-utils, dbus, python3). Without it there's no sound
# server and playback runs on the wall clock. --no-check only runs the app;
# --check judges an OUT_DIR such a run left (the release test runs the app
# in its container, without ffmpeg, and checks on the host).
#
# OUT_DIR gets app.log, sway.log, results.txt ("name=value" lines: what the
# run measured, then "check_<name>=ok|FAIL (what it saw)"), the viewport
# grabs, a fail-<line>.png for a failed step, the export and the saved
# project. Exits non-zero if a check failed. The judge only compares
# results.txt's values, never runs them: in the release test they come from
# inside a container, and are judged on the host.
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
    # Whether sound is checked, before anything that could fail to measure it.
    echo "sound=$sound" > "$R"

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

    # Sound: PipeWire with a null sink as the default output, behind a decoy
    # sink listed first -- the app must follow the default, not take the
    # first output.
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
        pactl load-module module-null-sink sink_name=decoysink > /dev/null
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
        # Whole samples only: parec may be cut off mid-sample.
        local peak
        peak=$(python3 -c '
import array, sys
b = open(sys.argv[1], "rb").read()
a = array.array("h", b[:len(b) // 2 * 2])
print(max((abs(x) for x in a), default=0))' "$rec") || peak=unreadable
        echo "sound_peak=$peak" >> "$R"
        rm -f "$rec"
    fi
    cleanup
    trap - EXIT
}

# --- checking what a run left -------------------------------------------
# shellcheck disable=SC2319 # each verdict's $? is the [[ ]] test before it
check_run() {
    # Again from what the run measured, if checked before.
    sed -i -E '/^(check_|export_(video|audio|duration_ms|video_ms|audio_ms|frame_luma_spread)=)/d' "$R"
    local failed=0
    get() { sed -nE "s/^$1=//p" "$R" | tail -1; }
    num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]] && echo "$1" || echo -1; }
    int() { printf '%.0f' "$(num "$1")"; }
    # Each check is a fixed [[ ]] test, run right before `verdict`, over
    # values made safe for it: numbers through int (in [[ ]], -ge and the
    # like would evaluate any other text as arithmetic, $(...) included),
    # text only as a quoted operand or the left side of ==.
    # verdict NAME STATUS SEEN: records the test's result (its STATUS) with
    # what it saw.
    verdict() {
        if (($2 == 0)); then
            echo "check_$1=ok ($3)" >> "$R"
        else
            echo "check_$1=FAIL ($3)" >> "$R"
            failed=1
        fi
    }
    # at_least NAME KEY MIN: the measured KEY is a number, MIN or more.
    at_least() {
        local v
        v=$(int "$(get "$2")")
        [[ $v -ge $3 ]]
        verdict "$1" $? "$2=$v, at least $3"
    }

    # Every step ran: one that measures nothing (a key, a click) fails here.
    local exit_status script steps_failed last_failed
    exit_status=$(get exit) script=$(get script) steps_failed=$(int "$(get steps_failed)")
    last_failed=$(get step_failed)
    [[ $exit_status == "0" && $script == "done" && $steps_failed == 0 ]]
    verdict quit $? "exit=$exit_status, script=$script, steps_failed=$steps_failed${last_failed:+, last: $last_failed}"
    at_least first_frame first_frame 0
    # 5 s of 29.97 fps playback: most frames reach the viewport and the
    # playhead keeps up with the clock.
    at_least play_decoded play_decoded 120
    at_least play_advance play_advance_ms 4500
    if [[ -z $(get viewport) ]]; then
        at_least first_drawn first_drawn 0
        at_least play_drawn play_drawn 120
        # A picture, not a flat colour: the road scene has plenty of contrast.
        local g sd mean
        for g in paused played; do
            sd=$(int "$(get "${g}_luma_stddev")") mean=$(int "$(get "${g}_luma_mean")")
            [[ $sd -ge 20 && $mean -ge 20 && $mean -le 235 ]]
            verdict "${g}_picture" $? "luma stddev $sd, at least 20; mean $mean, 20 to 235"
        done
    fi
    if [[ $(get sound) == "1" ]]; then
        # "Voice only": ONNX Runtime runs the voice model while playing.
        at_least voice_model play_onnxruntime 1
        at_least sound sound_peak 1
    fi
    local samples keys error
    samples=$(int "$(get track_samples)") keys=$(int "$(get track_keys)") error=$(get track_error)
    [[ $samples -ge 30 && $keys -ge 2 && -z $error ]]
    verdict track $? "samples=$samples, at least 30; keys=$keys, at least 2; error=$error"
    # The whole 10 s trimmed part scanned.
    samples=$(int "$(get scan_frames)") error=$(get scan_error)
    [[ $samples -ge 280 && -z $error ]]
    verdict scan $? "frames=$samples, at least 280; error=$error"
    local message size
    message=$(get export) size=$(int "$(stat -c %s "$out/smoke.mp4" 2> /dev/null)")
    [[ $message == "Export complete" && $size -gt 0 ]]
    verdict export $? "export=$message, smoke.mp4 $size bytes"

    # The exported file: one 1920x1080 HEVC video (the dialog's default) and
    # one audio stream, each as long as the timeline, and a picture in its
    # middle frame.
    if [[ -s $out/smoke.mp4 ]]; then
        probe() { ffprobe -v error -of default=nw=1:nk=1 "$@" "$out/smoke.mp4"; }
        # span STREAM: where the stream's first one starts and ends, in ms.
        span() {
            probe -select_streams "$1" -show_entries stream=start_time,duration | paste -sd' ' |
                awk '$1 ~ /^-?[0-9.]+$/ && $2 ~ /^[0-9.]+$/ { printf "%d %d", $1 * 1000, ($1 + $2) * 1000; ok = 1 }
                    END { if (!ok) print "? ?" }'
        }
        local video audio timeline_ms v0 v1 a0 a1 stats ylow yhigh spread
        video=$(probe -select_streams v -show_entries stream=codec_name,width,height | paste -sd,)
        audio=$(probe -select_streams a -show_entries stream=codec_name | paste -sd,)
        read -r v0 v1 <<< "$(span v:0)"
        read -r a0 a1 <<< "$(span a:0)"
        v0=$(int "$v0") v1=$(int "$v1") a0=$(int "$a0") a1=$(int "$a1")
        {
            echo "export_video=$video"
            echo "export_audio=$audio"
            echo "export_video_ms=$v0-$v1"
            echo "export_audio_ms=$a0-$a1"
        } >> "$R"
        [[ $video == "hevc,1920,1080" ]]
        verdict export_video $? "$video, want hevc,1920,1080"
        [[ -n $audio && $audio != *,* ]]
        verdict export_audio $? "$audio, want one stream"
        # Both streams, not only the container (which is as long as the
        # longer): within 100 ms of the timeline at both ends, for the frame
        # boundaries and the audio encoder's padding.
        timeline_ms=$(int "$(get timeline_ms)")
        [[ $timeline_ms -gt 0 ]] && ((v0 >= -100 && v0 <= 100 && a0 >= -100 && a0 <= 100 &&
            v1 - timeline_ms <= 100 && timeline_ms - v1 <= 100 &&
            a1 - timeline_ms <= 100 && timeline_ms - a1 <= 100))
        verdict export_length $? "video $v0-$v1 ms, audio $a0-$a1 ms, timeline $timeline_ms ms"
        # The middle frame's luma: its spread (YHIGH - YLOW: the 90th - 10th
        # percentile) shows a picture, not a flat colour.
        stats=$(ffmpeg -v error -ss "$((timeline_ms / 2000))" -i "$out/smoke.mp4" -frames:v 1 \
            -vf signalstats,metadata=print:file=- -f null - 2> /dev/null)
        # (the filter prints the frame's metadata twice)
        ylow=$(sed -nE 's/.*signalstats\.YLOW=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
        yhigh=$(sed -nE 's/.*signalstats\.YHIGH=([0-9.]+).*/\1/p' <<< "$stats" | head -1)
        spread=$(($(int "$yhigh") - $(int "$ylow")))
        echo "export_frame_luma_spread=$spread" >> "$R"
        [[ $spread -ge 40 ]]
        verdict export_picture $? "luma spread $spread, at least 40"
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
