#!/bin/bash
# Inside a bare container of the target release (debian:testing,
# debian:trixie) with the GPU, started by
# scripts/release_test.sh: build the release's source archive the way
# docs/dev-environment.md says (Debian's -dev packages, scripts/fetch_ncnn.sh,
# every feature on), run ctest, then the headless GPU tools on 2 s of the
# test clip. Writes the facts to /work/results.txt; release_test.sh judges.
set -uo pipefail

W=/work
R=$W/results.txt
: > "$R"
result() { echo "$1=$2" | tee -a "$R"; }
# step NAME CMD...: run CMD with its output in /work/NAME.log, record its status.
step() {
    local name=$1
    shift
    "$@" > "$W/$name.log" 2>&1
    result "$name" $?
}

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq > /dev/null
# docs/dev-environment.md's build dependencies, all of them (package.sh's
# ORBIS_REQUIRE_ALL_FEATURES), plus what fetch_ncnn.sh and unpacking need.
step deps apt-get install -y -qq --no-install-recommends \
    g++ cmake ninja-build pkgconf \
    qt6-base-dev qt6-base-dev-tools qt6-wayland qt6-svg-plugins \
    libvulkan-dev glslang-tools nlohmann-json3-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
    libopencv-dev libopencv-contrib-dev libonnxruntime-dev \
    curl unzip ca-certificates xz-utils

mkdir -p /build
tar -xJf /dist/orbis-reframe-*-source.tar.xz -C /build
src=$(echo /build/orbis-reframe-*)
cd "$src" || exit 1
step fetch_ncnn scripts/fetch_ncnn.sh
step configure cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DORBIS_REQUIRE_ALL_FEATURES=ON
step build cmake --build build -j"$(nproc)"
QT_QPA_PLATFORM=offscreen step ctest ctest --test-dir build --output-on-failure

# The headless GPU tools on the test clip (they write their own results).
"$src/tests/release/gpu_tools.sh" "$src/build" $W /work/smoke.orp
result tests_run "$(sed -nE 's/.*tests passed, ([0-9]+) tests failed out of ([0-9]+).*/\2/p' $W/ctest.log)"
exit 0
