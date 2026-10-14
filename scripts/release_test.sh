#!/usr/bin/env bash
# The GPU half of the release check (CI has no GPU): test the packages scripts/package.sh made for one
# target (scripts/targets.sh), in podman containers of that Debian release
# that use this machine's NVIDIA GPU (docs/RELEASE.md, "Test the packages").
#
#   1. sha256 files match.
#   2. Clean install, per package, in a bare image of the target (e.g.
#      debian:testing): the .deb with apt, the tarball with the target's apt
#      line from docs/manual/getting-started.md (option B). Every library
#      found (ldd), --version prints the version.
#   3. The GUI test, per package (tests/release/run_gui_test.sh in the image
#      from tests/release/Containerfile): headless Sway + PipeWire, open
#      tests/release/smoke.orp (10 s of a clip from sample_footage/, mounted
#      read-only), play with "Voice only", Track subject, Scan clip, export,
#      save, quit. Judged here from what it measured, the saved project and
#      ffprobe on the export.
#   4. The dev test (tests/release/dev_test.sh, bare image of the target): build
#      the source archive with docs/dev-environment.md's packages, ctest, and
#      the headless GPU tools (stitch_source_smoke, playback_bench,
#      view_compare, reframe_export, plate_scan) on 2 s of the same clip.
#
# Needs podman and the NVIDIA Container Toolkit's CDI spec
# (nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml). Results, logs and
# screenshots go to tmp/release-test/<version>/<target>/. Takes ~10 minutes;
# the first run per target also builds its test image and fills an apt cache.
# The containers use this machine's NVIDIA driver, whatever the target's own
# driver packages are.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=targets.sh
. "$REPO_ROOT/scripts/targets.sh"
DIST_DIR="$REPO_ROOT/dist/$(host_target)"
FOOTAGE_DIR="$REPO_ROOT/sample_footage"
KINDS=(deb tarball)
BASE_OVERRIDE=
DEV=1

usage() {
    cat << EOF
Usage: $(basename "$0") [--target TARGET] [--image IMAGE] [--footage DIR] [--only deb|tarball] [--no-dev] [DIST_DIR]
  --target TARGET test dist/TARGET/ (${ORBIS_TARGETS[*]})
  DIST_DIR        the packages to test (default: dist/$(host_target)/);
                  the target is the one in the tarball's name
  --image IMAGE   run in IMAGE instead of the target's own image: a
                  distribution built on the target, e.g.
                  docker.io/linuxmintd/mint23-amd64 for ubuntu-26.04
  --footage DIR   folder holding the test clip (default: sample_footage/);
                  tests/release/smoke.orp names it under /footage/
  --only KIND     test only the .deb or only the tarball
  --no-dev        skip the dev test (build from source, ctest, GPU tools)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) DIST_DIR="$REPO_ROOT/dist/$2"; shift 2 ;;
        --image) BASE_OVERRIDE=$2; shift 2 ;;
        --footage) FOOTAGE_DIR=$(realpath "$2"); shift 2 ;;
        --only) KINDS=("$2"); shift 2 ;;
        --no-dev) DEV=0; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
        *) DIST_DIR=$(realpath "$1"); shift ;;
    esac
done

die() { echo "release_test: $*" >&2; exit 1; }

command -v podman > /dev/null || die "podman isn't installed"
grep -qs 'nvidia.com' /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml \
    || die "no NVIDIA CDI spec -- sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml"

shopt -s nullglob
[[ -d "$DIST_DIR" ]] || die "no $DIST_DIR (scripts/package.sh)"
debs=("$DIST_DIR"/orbis-reframe_*_amd64.deb)
tarballs=("$DIST_DIR"/orbis-reframe-*-x86_64.tar.xz)
sources=("$DIST_DIR"/orbis-reframe-*-source.tar.xz)
shopt -u nullglob
[[ ${#debs[@]} -eq 1 && ${#tarballs[@]} -eq 1 && ${#sources[@]} -eq 1 ]] \
    || die "$DIST_DIR needs exactly one .deb, tarball and source archive (scripts/package.sh)"
# The .deb's version without a numbered release's .deb13 / .ubuntu26.04 is the binary's.
version=$(basename "${debs[0]}" | sed -E 's/^orbis-reframe_(.*)_amd64\.deb$/\1/; s/\.(deb[0-9]+|ubuntu[0-9.]+)$//')
[[ $(basename "${tarballs[0]}") == orbis-reframe-"$version"-* ]] \
    || die "the .deb and the tarball have different versions"
target=
for t in "${ORBIS_TARGETS[@]}"; do
    [[ $(basename "${tarballs[0]}") == *-"$t"-x86_64.tar.xz ]] && target=$t
done
[[ -n $target ]] || die "no known target in $(basename "${tarballs[0]}") (${ORBIS_TARGETS[*]})"
BASE=$(target_image "$target")
# Results, apt cache and test image are per target, or per image with --image.
run=$target
if [[ -n $BASE_OVERRIDE ]]; then
    BASE=$BASE_OVERRIDE
    run=$target-on-$(basename "${BASE%%:*}")
fi
IMAGE=orbis-release-test:$run

clip=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["timeline"]["clips"][0]["source"])' \
    "$REPO_ROOT/tests/release/smoke.orp")
[[ -f "$FOOTAGE_DIR/${clip#/footage/}" ]] || die "test clip missing: $FOOTAGE_DIR/${clip#/footage/}"

OUT="$REPO_ROOT/tmp/release-test/$version/$run"
APT_CACHE="$REPO_ROOT/tmp/release-test/apt-cache/$run"
rm -rf "$OUT"
mkdir -p "$OUT" "$APT_CACHE"
echo "== orbis-reframe $version, $target in ${BASE##*/} -> ${OUT#"$REPO_ROOT"/}"

fails=()
check() { # check NAME OK DETAIL
    if [[ "$2" == 1 ]]; then
        printf '  ok    %-28s %s\n' "$1" "$3"
    else
        printf '  FAIL  %-28s %s\n' "$1" "$3"
        fails+=("$1")
    fi
}

echo "== checksums"
(cd "$DIST_DIR" && sha256sum -c --quiet ./*.sha256) && check checksums 1 "" || check checksums 0 ""

# The guide's apt line for the tarball (option B) on this target -- the
# first after a line naming it ("On Debian 13 ..."), as plain package names.
awk -v label="$(target_label "$target")" '/^### Option B/ {b=1} b && index($0, "On " label) == 1 {t=1}
    t && /^sudo apt install/ {p=1; next} p && /^```/ {exit} p' \
    "$REPO_ROOT/docs/manual/getting-started.md" | tr -d '\\' | xargs -n1 > "$OUT/apt-packages.txt"
[[ -s "$OUT/apt-packages.txt" ]] \
    || die "no apt line for $(target_label "$target") in docs/manual/getting-started.md (option B)"

GPU=(--device nvidia.com/gpu=all -e NVIDIA_DRIVER_CAPABILITIES=graphics,video,compute,utility)
# Downloaded .debs are kept between runs (Debian's images delete them).
CACHE=(-v "$APT_CACHE:/var/cache/apt/archives")
NOCLEAN='rm -f /etc/apt/apt.conf.d/docker-clean'

echo "== clean install (bare ${BASE##*/})"
podman pull -q "$BASE" > /dev/null
for kind in "${KINDS[@]}"; do
    if [[ $kind == deb ]]; then
        install="apt-get install -y -qq /dist/$(basename "${debs[0]}") && bin=/usr/bin/orbis-reframe"
    else
        # xz-utils only to unpack: a bare image lacks it, a desktop has it.
        install="apt-get install -y -qq xz-utils \$(cat /out/apt-packages.txt) && mkdir /opt/t \
            && tar -xJf /dist/$(basename "${tarballs[0]}") -C /opt/t && bin=\$(echo /opt/t/*/bin/orbis-reframe)"
    fi
    log="$OUT/clean-$kind.log"
    podman run --rm "${CACHE[@]}" -v "$DIST_DIR:/dist:ro" -v "$OUT:/out:ro" -e DEBIAN_FRONTEND=noninteractive \
        "$BASE" bash -c "$NOCLEAN; apt-get update -qq && $install \
            && echo \"ldd_missing=\$(ldd \$bin | grep -c 'not found')\" \
            && echo \"version=\$(\$bin --version)\"" > "$log" 2>&1 || true
    missing=$(sed -n 's/^ldd_missing=//p' "$log")
    reported=$(sed -n 's/^version=//p' "$log")
    check "$kind: installs, libraries" "$([[ $missing == 0 ]] && echo 1 || echo 0)" "ldd: ${missing:-?} missing"
    check "$kind: --version" "$([[ $reported == "orbis-reframe $version" ]] && echo 1 || echo 0)" "$reported"
done

echo "== GUI test image"
podman build -q -t "$IMAGE" --build-arg BASE="$BASE" "$REPO_ROOT/tests/release" > "$OUT/image-build.log" 2>&1 \
    || { cat "$OUT/image-build.log" >&2; die "building the test image failed"; }

for kind in "${KINDS[@]}"; do
    echo "== GUI test: $kind"
    work="$OUT/$kind"
    mkdir -p "$work"
    cp "$REPO_ROOT/tests/release/smoke.orp" "$OUT/apt-packages.txt" "$work/"
    podman run --rm "${GPU[@]}" "${CACHE[@]}" -v "$DIST_DIR:/dist:ro" -v "$FOOTAGE_DIR:/footage:ro" \
        -v "$work:/work" "$IMAGE" bash -c "$NOCLEAN; run_gui_test.sh $kind" > "$work/run.log" 2>&1 || true
    r() { sed -n "s/^$1=//p" "$work/results.txt" 2> /dev/null | tail -1; }

    check "$kind: install" "$([[ $(r install) == 0 && $(r ldd_missing) == 0 ]] && echo 1 || echo 0)" \
        "apt $(r install), ldd $(r ldd_missing) missing"
    check "$kind: input" "$([[ -z $(r input_error) ]] && echo 1 || echo 0)" "$(r input_error)"
    check "$kind: first frame" "$([[ $(r first_frame) == 0 ]] && echo 1 || echo 0)" ""
    frames=$(r play_new_frames_max)
    check "$kind: playback" "$([[ ${frames:-0} -ge 25 ]] && echo 1 || echo 0)" "${frames:-0} new frames/s at best"
    check "$kind: voice mode" "$([[ $(r onnxruntime_loaded) -gt 0 ]] && echo 1 || echo 0)" \
        "ONNX Runtime mapped: $(r onnxruntime_loaded)"
    peak=$(r audio_peak)
    check "$kind: sound" "$([[ ${peak:-0} -gt 0 ]] && echo 1 || echo 0)" "peak ${peak:-0} / 32767"
    keys=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["timeline"]["clips"][0]["reframe"]["keys"]))' \
        "$work/smoke.orp" 2> /dev/null || echo 0)
    check "$kind: track subject" "$([[ $(r track_done) == 0 && $keys -gt 1 ]] && echo 1 || echo 0)" \
        "$keys reframe keys saved"
    scan=$(r scan_line)
    scan_frames=$(echo "$scan" | sed -nE 's/.*: ([0-9]+) frames.*/\1/p')
    check "$kind: scan clip" "$([[ $(r scan_done) == 0 && ${scan_frames:-0} -gt 0 ]] && echo 1 || echo 0)" \
        "$(echo "$scan" | sed -E 's/^\[plates\] [^:]*: //')"
    # A loose project file exports next to itself, named after it.
    probe=$(ffprobe -v error -show_entries stream=codec_type,codec_name,width,height:format=duration \
        -of compact=p=0:nk=1 "$work/smoke.mp4" 2> /dev/null | tr '\n' ' ') || probe="no $kind/smoke.mp4"
    export_ok=$(python3 - "$work/smoke.mp4" << 'EOF'
import json, subprocess, sys
try:
    j = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_entries",
        "stream=codec_type,codec_name,width,height:format=duration", "-of", "json", sys.argv[1]]))
except Exception:
    print(0); sys.exit()
v = [s for s in j["streams"] if s["codec_type"] == "video"]
a = [s for s in j["streams"] if s["codec_type"] == "audio"]
d = float(j["format"]["duration"])
print(int(len(v) == 1 and v[0]["codec_name"] == "hevc" and (v[0]["width"], v[0]["height"]) == (1920, 1080)
          and len(a) == 1 and 9.5 < d < 10.5))
EOF
)
    check "$kind: export" "$([[ $(r export_dialog) == 'Export complete' && $export_ok == 1 ]] && echo 1 || echo 0)" \
        "$probe"
    check "$kind: save + quit" "$([[ $(r quit) == 0 ]] && echo 1 || echo 0)" ""
done

if [[ $DEV == 1 ]]; then
    echo "== dev test: build from source, ctest, GPU tools"
    work="$OUT/dev"
    mkdir -p "$work"
    cp "$REPO_ROOT/tests/release/smoke.orp" "$work/"
    podman run --rm "${GPU[@]}" "${CACHE[@]}" -v "$DIST_DIR:/dist:ro" -v "$FOOTAGE_DIR:/footage:ro" \
        -v "$work:/work" -v "$REPO_ROOT/tests/release/dev_test.sh:/dev_test.sh:ro" \
        "$BASE" bash -c "$NOCLEAN; /dev_test.sh" > "$work/run.log" 2>&1 || true
    r() { sed -n "s/^$1=//p" "$work/results.txt" 2> /dev/null | tail -1; }
    for s in deps fetch_ncnn configure build; do
        check "dev: $s" "$([[ $(r $s) == 0 ]] && echo 1 || echo 0)" "dev/$s.log"
    done
    check "dev: ctest" "$([[ $(r ctest) == 0 ]] && echo 1 || echo 0)" \
        "$(tail -n 3 "$work/ctest.log" 2> /dev/null | grep -m1 'tests passed' || echo 'dev/ctest.log')"
    for s in stitch_source_smoke playback_bench view_compare reframe_export plate_scan; do
        check "dev: $s" "$([[ $(r $s) == 0 ]] && echo 1 || echo 0)" "dev/$s.log"
    done
fi

echo
if [[ ${#fails[@]} -eq 0 ]]; then
    echo "== all checks passed ($version)"
else
    echo "== ${#fails[@]} check(s) failed: logs and screenshots in ${OUT#"$REPO_ROOT"/}" >&2
    exit 1
fi
