#!/bin/bash
# Inside the release-test container (tests/release/Containerfile), started by
# scripts/release_test.sh: install the package under test, start a headless
# Sway + PipeWire (a null sink as the default output), open /work/smoke.orp
# and drive the app like a user -- play with a voice noise mode, Track
# subject, Scan clip, export -- then save and quit. Writes everything to
# /work (log, screenshots, the saved project, the export) and the facts it
# measured to /work/results.txt; release_test.sh judges them.
#
#   run_gui_test.sh deb      install /dist/orbis-reframe_*_amd64.deb
#   run_gui_test.sh tarball  install the tarball + the install guide's apt line
#
# Coordinates are for the 1920x1080 headless output and the app's default
# layout (both sidebars open at their default widths, the Privacy tab's
# button); the screenshots show where they hit.
set -uo pipefail

kind=${1:?deb or tarball}
W=/work
R=$W/results.txt
: > "$R"
result() { echo "$1=$2" | tee -a "$R"; }
shot() { grim "$W/$1.png"; }

# --- install
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq > /dev/null
if [[ $kind == deb ]]; then
    apt-get install -y -qq /dist/orbis-reframe_*_amd64.deb > "$W/apt.log" 2>&1
    result install $?
    app=orbis-reframe
    bin=/usr/bin/orbis-reframe
else
    # shellcheck disable=SC2046 # the guide's package list, one word each
    apt-get install -y -qq $(cat /work/apt-packages.txt) > "$W/apt.log" 2>&1
    result install $?
    tar -xJf /dist/orbis-reframe-*-x86_64.tar.xz -C /opt
    app=$(echo /opt/orbis-reframe-*/orbis-reframe) # the top-level launcher
    bin=$(dirname "$app")/bin/orbis-reframe
fi
missing=$(ldd "$bin" | grep -c 'not found')
result ldd_missing "$missing"
result version "$("$app" --version 2>&1 | head -1)"

# --- desktop: PipeWire with a null sink, headless Sway
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p -m 700 $XDG_RUNTIME_DIR
DBUS_SESSION_BUS_ADDRESS=$(dbus-daemon --session --fork --print-address=1)
export DBUS_SESSION_BUS_ADDRESS
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1
wireplumber > /dev/null 2>&1 &
pipewire-pulse > /dev/null 2>&1 &
sleep 2
pactl load-module module-null-sink sink_name=testsink > /dev/null
pactl set-default-sink testsink

printf 'output HEADLESS-1 mode 1920x1080@60Hz\ndefault_border none\nxwayland disable\n' > /tmp/sway.conf
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c /tmp/sway.conf > "$W/sway.log" 2>&1 &
for _ in $(seq 50); do [[ -S $XDG_RUNTIME_DIR/wayland-1 ]] && break; sleep 0.2; done
export WAYLAND_DISPLAY=wayland-1
sock() { echo $XDG_RUNTIME_DIR/sway-ipc.*.sock; }

# Window titles Sway knows, one per line.
windows() {
    swaymsg -s "$(sock)" -t get_tree | python3 -c '
import json, sys
def walk(n):
    if n.get("pid"): print(n.get("name") or "")
    for c in n.get("nodes", []) + n.get("floating_nodes", []): walk(c)
walk(json.load(sys.stdin))'
}
# wait_log PATTERN SECONDS: until the app log has a line matching PATTERN.
wait_log() {
    local i
    for ((i = 0; i < $2 * 2; ++i)); do
        grep -qE "$1" "$W/app.log" && return 0
        sleep 0.5
    done
    return 1
}
# wait_window PATTERN SECONDS: until a window title matches PATTERN.
wait_window() {
    local i
    for ((i = 0; i < $2 * 2; ++i)); do
        windows | grep -qE "$1" && return 0
        sleep 0.5
    done
    return 1
}

# enter_until_gone PATTERN: press Enter until no window title matches PATTERN
# (a freshly opened file dialog sometimes drops the first key).
enter_until_gone() {
    local i
    for ((i = 0; i < 5; ++i)); do
        vk key Return
        sleep 1.5
        windows | grep -qE "$1" || return 0
    done
    return 1
}

# --- the app
export QT_QPA_PLATFORM=wayland ORBIS_VERBOSE=1 ORBIS_FRAME_STATS=1 HOME=$W
"$app" "$W/smoke.orp" > "$W/app.log" 2>&1 &
pid=$!
wait_log '^\[viewport\] drawing' 60
result first_frame $?
sleep 4 # the sync fine-tune runs while paused; let the window settle
shot 1-open

# Play 6 s with "Voice only" on (set in smoke.orp): frames must arrive, the
# voice model (ONNX Runtime) must load, and sound must reach the sink.
mark=$(wc -l < "$W/app.log")
vk key space
sleep 2
timeout 3 parec -d testsink.monitor --format=s16le --rate=48000 --channels=2 --raw > /tmp/rec.raw
sleep 1
vk key space
shot 2-played
result onnxruntime_loaded "$(grep -c libonnxruntime /proc/$pid/maps)"
result audio_peak "$(python3 -c '
import array, sys
a = array.array("h", open("/tmp/rec.raw", "rb").read())
print(max((abs(x) for x in a), default=0))')"
# Frames per second while playing: the busiest "[stats] viewport" second.
result play_new_frames_max "$(tail -n +"$mark" "$W/app.log" | grep -oE 'new frames [0-9]+' \
    | awk '{print $3}' | sort -n | tail -1)"

# Track subject: back to the start, T, drag a box around the motorbike ahead.
vk key Home sleep 500 key t sleep 500
vp move 632 452 down move 650 480 move 676 520 up
shot 3-track-box
wait_log '^\[track\]' 120
result track_done $?
sleep 1
shot 4-tracked

# Scan clip: the settings sidebar's Privacy tab (Alt+4), then its "Scan
# clip" button.
vk key alt+4 sleep 800
vp move 1740 206 click
sleep 2
shot 5-scanning
wait_log '^\[plates\]' 300
result scan_done $?
result scan_line "$(grep -m1 '^\[plates\]' "$W/app.log")"
shot 6-scanned

# Export: Ctrl+E, accept the options, accept ~/timeline.mp4 (HOME=/work),
# then close the "Export complete" message.
vk key ctrl+e
wait_window '^Export Flat Video$' 10
shot 7-export-options
enter_until_gone '^Export Flat Video$'
result export_options $?
wait_window '^Export flat video$' 10
shot 8-export-file
enter_until_gone '^Export flat video$'
result export_file_dialog $?
wait_window '^Export (complete|failed)' 300
result export_dialog "$(windows | grep -E '^Export (complete|failed)' | head -1)"
shot 9-exported
enter_until_gone '^Export (complete|failed)'

# Save, quit.
vk key ctrl+s sleep 1000 key ctrl+q
for _ in $(seq 40); do kill -0 $pid 2> /dev/null || break; sleep 0.5; done
if kill -0 $pid 2> /dev/null; then
    shot 10-not-quit
    result quit 1
    kill $pid
else
    wait $pid
    result quit $?
fi
grep -E 'error|warning|fail' -i "$W/app.log" | grep -v '^\[stats\]' > "$W/app-warnings.log"
exit 0
