#!/usr/bin/env bash
# Build (and optionally run/clean) orbis-reframe. Keep in sync with
# CMakeLists.txt and docs/dev-environment.md whenever the build changes
# (new build types, targets, generator flags, etc).
#
# orbis-reframe / stitch gate on FFmpeg (+ Vulkan) only; CMake
# prints a STATUS line and skips them if the FFmpeg dev packages are absent.
# libavfilter-dev is among them (atempo time-stretch for clip speed, 2026-09-27).
# OpenCV (libopencv-dev) -> insv_optical + tools/residual_stab, also
# optional. Subject tracking (src/track/ -> "Track subject" in the Reframe
# panel, tools/track_subject) additionally needs the contrib tracking + dnn
# modules (libopencv-contrib-dev); without them the button is hidden.
# ncnn -> the privacy blur's plate scan ("Scan clip" in the Privacy panel,
# src/privacy/PlateScanner, tools/plate_scan; ncnn isn't in Debian):
# scripts/fetch_ncnn.sh unpacks the pinned release into third_party/ncnn/,
# found automatically, or NCNN_ROOT=<unpacked ncnn release> scripts/build.sh.
# Optional: without it the app applies existing scans but can't make one.
# Shaders are compiled into the binary; models are found next to it once
# installed (cmake --install) or in the source tree here. Release
# packages (tarball + .deb) come from scripts/package.sh, not this script.
# tools/audio_filter_bench (audio noise-filter harness) needs only FFmpeg; its
# run_all.sh optionally uses the prebuilt DeepFilterNet CLI (not built here).
# ONNX Runtime (libonnxruntime-dev, headers only at build time; the library is
# dlopen()ed) -> the "Voice only" / "Voice + ambience" noise modes, optional.
# An unpacked copy instead: ONNXRUNTIME_ROOT=<dir>/usr scripts/build.sh.
# Qt6 Multimedia (qt6-multimedia-dev) -> audio playback,
# also optional -- orbis-reframe still builds without it, just silently (no
# sound). libswresample-dev (usually pulled in with the other FFmpeg dev
# packages) is needed for it too.
#
# Stitching is stitch::NativeStitchSource: trailer/calibration parse -> the
# src/insv/ cleanroom core (insv + insv_demux, the latter carrying the
# Vulkan-video decoder) + render_gpu_stitch (headless Vulkan compute port of
# the fisheye->equirect stitch: src/render/VulkanFisheyeStitcher +
# shaders/fisheye_stitch{,_nv12}.comp). The all-GPU path (Vulkan video decode +
# compute stitch) runs real-time; INSV_NO_GPU_DECODE=1 forces CPU decode,
# INSV_NO_GPU_STITCH=1 forces the CPU stitch, INSV_FLAT_VIA_EQUIRECT=1 sends
# flat export back through an equirect, INSV_PLAYBACK_EQUIRECT=1 keeps the
# viewport on the stitched equirect instead of the lenses. `stitch_source_smoke` exercises
# the source directly; `playback_bench` times it per capture mode and
# `view_compare` checks the fused fisheye -> view pass against it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

RUN_AFTER=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        -r|--run) RUN_AFTER=1 ;;
        -c|--clean) CLEAN=1 ;;
        -h|--help)
            echo "Usage: $(basename "$0") [-c|--clean] [-r|--run]"
            echo "  -c, --clean   remove the build directory first"
            echo "  -r, --run     launch orbis-reframe after a successful build"
            echo "Env: BUILD_TYPE=Debug|Release (default: Debug)"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

if [[ "$CLEAN" -eq 1 ]]; then
    rm -rf "$BUILD_DIR"
fi

cmake -B "$BUILD_DIR" -S "$REPO_ROOT" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    ${ONNXRUNTIME_ROOT:+-DONNXRUNTIME_ROOT="$ONNXRUNTIME_ROOT"} \
    ${NCNN_ROOT:+-DNCNN_ROOT="$NCNN_ROOT"}
cmake --build "$BUILD_DIR" -j"$(nproc)"

# Fast, no-hardware unit tests (src/core edit model; TimelineWidget mouse
# handling on the offscreen platform, if qt6-base-dev's Qt6Test is present;
# the audio mixer and the M7b overlay reader / compositor on generated files).
# ctest exits non-zero on failure, so `set -e` fails the build.
ctest --test-dir "$BUILD_DIR" --output-on-failure

if [[ "$RUN_AFTER" -eq 1 ]]; then
    "$BUILD_DIR/orbis-reframe"
fi
