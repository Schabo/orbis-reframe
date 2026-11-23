#!/bin/bash
# Tests gpu_smoke.sh's judge (--check) on what a passing run left: copies of
# its results.txt and export, each changed the way a broken run or a hostile
# app would leave them, must get the verdicts below -- and the judge must
# never run a value as code. Used by the GPU workflow after the smoke test;
# needs ffmpeg. Takes a few seconds.
#
#   gpu_smoke_judge_test.sh OUT_DIR
#
# OUT_DIR is a smoke test's output folder whose checks all passed. The cases
# go into OUT_DIR/judge-test/ (removed again when every case passed).
# Exits non-zero if a case failed.
set -uo pipefail

here=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
[[ $# -eq 1 ]] || { echo "usage: gpu_smoke_judge_test.sh OUT_DIR" >&2; exit 2; }
base=$(realpath "$1")
[[ -s $base/results.txt && -s $base/smoke.mp4 ]] || { echo "no run in $base" >&2; exit 2; }
work=$base/judge-test
rm -rf "$work"
mkdir -p "$work"
mark=$work/ran-a-value
failures=0

# A case's folder: the run's results without the judge's lines (and without
# the KEYs given, set again with `set_value`), its export linked in.
new_case() {
    local dir=$work/$1 key
    shift
    mkdir -p "$dir"
    grep -v '^check_' "$base/results.txt" > "$dir/results.txt"
    for key; do
        sed -i "/^$key=/d" "$dir/results.txt"
    done
    ln -f "$base/smoke.mp4" "$dir/smoke.mp4" 2> /dev/null || cp "$base/smoke.mp4" "$dir/smoke.mp4"
    echo "$dir"
}
set_value() { printf '%s=%s\n' "$2" "$3" >> "$1/results.txt"; }
judge() { "$here/gpu_smoke.sh" --check "$1" > /dev/null 2>&1; }
verdict_of() { sed -nE "s/^check_$2=(ok|FAIL).*/\1/p" "$1/results.txt"; }

# expect CASE DIR STATUS [CHECK=ok|FAIL|none]...: the judge's exit status
# (0 or 1) and the named checks' verdicts.
expect() {
    local name=$1 dir=$2 want=$3 got item check v
    shift 3
    judge "$dir"
    got=$?
    local problems=()
    [[ $got == "$want" ]] || problems+=("exit $got, want $want")
    for item; do
        check=${item%%=*} v=$(verdict_of "$dir" "$check")
        [[ ${v:-none} == "${item#*=}" ]] || problems+=("check_$check ${v:-missing}, want ${item#*=}")
    done
    [[ -e $mark ]] && problems+=("a value ran as a command")
    rm -f "$mark"
    if ((${#problems[@]})); then
        printf 'FAIL  %s: %s\n' "$name" "$(IFS=';'; echo "${problems[*]}")"
        failures=$((failures + 1))
    else
        printf 'ok    %s\n' "$name"
    fi
}

dir=$(new_case control)
expect "the run as it is passes" "$dir" 0 quit=ok export_length=ok

# Values that would end the [[ ]] expression and run a command, were the
# judge to evaluate text: the check fails on them, and they're kept as text.
dir=$(new_case quoted-error track_error)
set_value "$dir" track_error "x' ]] || touch '$mark'; [[ 'y"
expect "a quote in a job error is text" "$dir" 1 track=FAIL
grep -qF "error=x' ]] || touch '$mark'; [[ 'y)" "$dir/results.txt" ||
    { echo "FAIL  the job error isn't recorded as it was"; failures=$((failures + 1)); }
dir=$(new_case quoted-exit exit export)
set_value "$dir" exit "0' ]] || touch '$mark'; [[ '"
set_value "$dir" export "Export complete' ]] || touch '$mark'; [[ '"
expect "a quote in the exit status or the export's message is text" "$dir" 1 quit=FAIL export=FAIL
dir=$(new_case arithmetic play_decoded)
set_value "$dir" play_decoded "a[\$(touch '$mark')]"
expect "a number that's an expression is no number" "$dir" 1 play_decoded=FAIL

# A failed step that measures nothing of its own (a key, a click).
dir=$(new_case failed-step steps_failed script)
set_value "$dir" step_failed "19: key Home: no shortcut Home"
set_value "$dir" steps_failed 1
set_value "$dir" script "done"
expect "a failed step fails the run" "$dir" 1 quit=FAIL
dir=$(new_case no-step-count steps_failed)
expect "a run that didn't count its steps fails" "$dir" 1 quit=FAIL

# Sound: checked whenever the run recorded it, whatever the peak says.
for peak in "" none 0 unreadable; do
    dir=$(new_case "sound-${peak:-empty}" sound sound_peak)
    set_value "$dir" sound 1
    [[ $peak == none ]] || set_value "$dir" sound_peak "$peak"
    expect "sound on, peak '$peak' fails" "$dir" 1 sound=FAIL
done
dir=$(new_case sound-heard sound sound_peak play_onnxruntime)
set_value "$dir" sound 1
set_value "$dir" sound_peak 5360
set_value "$dir" play_onnxruntime 5
expect "sound on and heard passes" "$dir" 0 sound=ok voice_model=ok
dir=$(new_case sound-off sound sound_peak)
set_value "$dir" sound 0
expect "sound off checks no sound" "$dir" 0 sound=none voice_model=none

# Each stream as long as the timeline, not only the container.
for short in video audio; do
    dir=$(new_case "short-$short")
    rm "$dir/smoke.mp4"
    if [[ $short == video ]]; then
        ffmpeg -v error -t 1 -i "$base/smoke.mp4" -i "$base/smoke.mp4" -map 0:v -map 1:a -c copy \
            "$dir/smoke.mp4"
    else
        ffmpeg -v error -i "$base/smoke.mp4" -t 1 -i "$base/smoke.mp4" -map 0:v -map 1:a -c copy \
            "$dir/smoke.mp4"
    fi
    expect "the $short cut to 1 s fails the length" "$dir" 1 export_length=FAIL
done

if ((failures)); then
    echo "$failures case(s) failed; their folders are in $work"
    exit 1
fi
rm -rf "$work"
echo "all cases passed"
