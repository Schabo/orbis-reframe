#!/bin/bash
# Inside the release-test container (tests/release/Containerfile), started by
# scripts/release_test.sh: install the package under test, then run the
# smoke test on it (gpu_smoke.sh --sound: headless Sway, PipeWire with a null
# sink, the app driven by gpu_smoke.steps -- play with a voice noise mode,
# Track subject, Scan clip, export, save, quit). Writes to /work: install.txt
# (install, ldd, --version), and the smoke test's results.txt, logs, viewport
# grabs, export and saved project; release_test.sh judges them on the host
# (gpu_smoke.sh --check, which needs ffmpeg -- not installed here, so it
# can't stand in for a library a package forgot).
#
#   run_gui_test.sh deb      install /dist/orbis-reframe_*_amd64.deb
#   run_gui_test.sh tarball  install the tarball + the install guide's apt line
set -uo pipefail

kind=${1:?deb or tarball}
W=/work
I=$W/install.txt
: > "$I"
result() { echo "$1=$2" | tee -a "$I"; }

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
result ldd_missing "$(ldd "$bin" | grep -c 'not found')"
result version "$("$app" --version 2>&1 | head -1)"

/opt/smoke/gpu_smoke.sh --sound --no-check "$(command -v "$app")" "$W"
