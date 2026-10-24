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
#      from tests/release/Containerfile): the smoke test (gpu_smoke.sh
#      --sound) on the installed app -- headless Sway + PipeWire, open
#      tests/release/smoke.orp (10 s of a clip from sample_footage/, mounted
#      read-only), play with "Voice only", Track subject, Scan clip, export,
#      save, quit, driven by gpu_smoke.steps. Judged here (gpu_smoke.sh
#      --check: what the app measured, ffprobe on the export).
#   4. The dev test (tests/release/dev_test.sh, bare image of the target): build
#      the source archive with docs/dev-environment.md's packages, ctest, and
#      the headless GPU tools (stitch_source_smoke, playback_bench,
#      view_compare, reframe_export, plate_scan) on 2 s of the same clip.
#
# --target appimage tests dist/appimage/ instead, in five distributions
# (APPIMAGE_DISTROS below):
#   1. sha256 files match.
#   2. Bare images of Fedora, Arch and Tumbleweed with only the libraries
#      the AppImage takes from the system (tests/release/appimage_bare.sh;
#      the release workflow does this for Debian 13 and Ubuntu 26.04):
#      every bundled library resolves, --version; which glvnd libraries
#      came from the bundled fallback.
#   3. The GUI test in each (tests/release/run_appimage_test.sh in the image
#      from tests/release/Containerfile or Containerfile.<distro>): the same
#      smoke test as for the packages.
#   4. Each export's video and audio streams are the same as the Debian
#      Testing .deb's (its release test of the same version, run first).
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
ONLY=
BASE_OVERRIDE=
# Where the AppImage is tested, and from which image; Debian's and Ubuntu's
# are the targets' own (scripts/targets.sh).
APPIMAGE_DISTROS=(debian-13 ubuntu-26.04 fedora arch tumbleweed)
declare -A APPIMAGE_BASE=(
    [fedora]=registry.fedoraproject.org/fedora:latest
    [arch]=docker.io/library/archlinux:latest
    [tumbleweed]=registry.opensuse.org/opensuse/tumbleweed:latest
)
DEV=1

usage() {
    cat << EOF
Usage: $(basename "$0") [--target TARGET] [--image IMAGE] [--footage DIR] [--only deb|tarball|DISTRO] [--no-dev] [DIST_DIR]
  --target TARGET test dist/TARGET/ (${ORBIS_TARGETS[*]}, or appimage)
  DIST_DIR        the packages to test (default: dist/$(host_target)/);
                  the target is the one in the tarball's name
  --image IMAGE   run in IMAGE instead of the target's own image: a
                  distribution built on the target, e.g.
                  docker.io/linuxmintd/mint23-amd64 for ubuntu-26.04
  --footage DIR   folder holding the test clip (default: sample_footage/);
                  tests/release/smoke.orp names it under /footage/
  --only KIND     test only the .deb or only the tarball; for the
                  AppImage, only one distribution (${APPIMAGE_DISTROS[*]})
  --no-dev        skip the dev test (build from source, ctest, GPU tools)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) DIST_DIR="$REPO_ROOT/dist/$2"; shift 2 ;;
        --image) BASE_OVERRIDE=$2; shift 2 ;;
        --footage) FOOTAGE_DIR=$(realpath "$2"); shift 2 ;;
        --only) KINDS=("$2"); ONLY=$2; shift 2 ;;
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

[[ -d "$DIST_DIR" ]] || die "no $DIST_DIR (scripts/package.sh)"

fails=()
check() { # check NAME OK DETAIL
    if [[ "$2" == 1 ]]; then
        printf '  ok    %-28s %s\n' "$1" "$3"
    else
        printf '  FAIL  %-28s %s\n' "$1" "$3"
        fails+=("$1")
    fi
}
footage_check() {
    local clip
    clip=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["timeline"]["clips"][0]["source"])' \
        "$REPO_ROOT/tests/release/smoke.orp")
    [[ -f "$FOOTAGE_DIR/${clip#/footage/}" ]] || die "test clip missing: $FOOTAGE_DIR/${clip#/footage/}"
}
finish() {
    echo
    if [[ ${#fails[@]} -eq 0 ]]; then
        echo "== all checks passed ($version)"
    else
        echo "== ${#fails[@]} check(s) failed: logs and screenshots in ${OUT#"$REPO_ROOT"/}" >&2
        exit 1
    fi
}
GPU=(--device nvidia.com/gpu=all -e NVIDIA_DRIVER_CAPABILITIES=graphics,video,compute,utility)
NOCLEAN='rm -f /etc/apt/apt.conf.d/docker-clean'

# --- the AppImage
shopt -s nullglob
appimages=("$DIST_DIR"/orbis-reframe-*-x86_64.AppImage)
shopt -u nullglob
if [[ ${#appimages[@]} -gt 0 ]]; then
    [[ ${#appimages[@]} -eq 1 ]] || die "$DIST_DIR has more than one AppImage"
    [[ -x "${appimages[0]}" ]] || die "${appimages[0]} isn't executable (chmod +x, as a user would)"
    app=$(basename "${appimages[0]}")
    version=${app#orbis-reframe-}
    version=${version%-x86_64.AppImage}
    distros=("${APPIMAGE_DISTROS[@]}")
    if [[ -n $ONLY ]]; then
        [[ " ${APPIMAGE_DISTROS[*]} " == *" $ONLY "* ]] || die "--only: one of ${APPIMAGE_DISTROS[*]}"
        distros=("$ONLY")
    fi
    footage_check
    OUT="$REPO_ROOT/tmp/release-test/$version/appimage"
    rm -rf "$OUT"
    mkdir -p "$OUT"
    echo "== orbis-reframe $version, the AppImage -> ${OUT#"$REPO_ROOT"/}"

    echo "== checksums"
    (cd "$DIST_DIR" && sha256sum -c --quiet ./*.sha256) && check checksums 1 "" || check checksums 0 ""

    echo "== bare images"
    for d in "${distros[@]}"; do
        [[ -n ${APPIMAGE_BASE[$d]:-} ]] || continue
        log="$OUT/bare-$d.log"
        podman pull -q "${APPIMAGE_BASE[$d]}" > /dev/null
        podman run --rm -v "$DIST_DIR:/dist:ro" -v "$REPO_ROOT/tests/release/appimage_bare.sh:/bare.sh:ro" \
            "${APPIMAGE_BASE[$d]}" bash /bare.sh > "$log" 2>&1 || true
        r() { sed -n "s/^$1=//p" "$log" | tail -1; }
        check "$d bare: libraries" \
            "$([[ $(r install) == 0 && $(r elf_files) -gt 0 && -z $(r missing) ]] && grep -q '^missing=' "$log" && echo 1 || echo 0)" \
            "$(r elf_files) files; $([[ -n $(r missing) ]] && echo "missing: $(r missing)" || echo "fallback: $(r fallback)")"
        check "$d bare: --version" "$([[ $(r version) == "orbis-reframe $version" ]] && echo 1 || echo 0)" "$(r version)"
    done

    for d in "${distros[@]}"; do
        echo "== GUI test: $d"
        image=orbis-release-test:appimage-$d
        if [[ -n ${APPIMAGE_BASE[$d]:-} ]]; then
            build=(-f "$REPO_ROOT/tests/release/Containerfile.$d")
        else
            build=(--build-arg BASE="$(target_image "$d")" --build-arg EXTRA="libopengl0 libglx0")
        fi
        podman build -q -t "$image" "${build[@]}" "$REPO_ROOT/tests/release" > "$OUT/image-$d.log" 2>&1 \
            || { cat "$OUT/image-$d.log" >&2; die "building the test image for $d failed"; }
        work="$OUT/$d"
        mkdir -p "$work"
        # The CDI spec, generated on this Debian machine, mounts NVIDIA's GBM
        # backend at Debian's path, where the other distributions' Mesa
        # doesn't look; without it Sway doesn't composite.
        podman run --rm "${GPU[@]}" -e GBM_BACKENDS_PATH=/usr/lib/x86_64-linux-gnu/gbm \
            -v "$DIST_DIR:/dist:ro" -v "$FOOTAGE_DIR:/footage:ro" -v "$work:/work" \
            "$image" run_appimage_test.sh > "$work/run.log" 2>&1 || true
        r() { sed -n "s/^$1=//p" "$work/install.txt" 2> /dev/null | tail -1; }
        check "$d: --version" "$([[ $(r version) == "orbis-reframe $version" ]] && echo 1 || echo 0)" \
            "$(r distribution), glibc $(r glibc)"
        "$REPO_ROOT/tests/release/gpu_smoke.sh" --check "$work" > /dev/null 2>&1 || true
        if ! grep -q '^check_' "$work/results.txt" 2> /dev/null; then
            check "$d: smoke test" 0 "no results: appimage/$d/run.log"
        fi
        while IFS='=' read -r name verdict; do
            check "$d: ${name#check_}" "$([[ $verdict == ok* ]] && echo 1 || echo 0)" \
                "$(sed -E 's/^(ok|FAIL) \((.*)\)$/\2/' <<< "$verdict")"
        done < <(grep '^check_' "$work/results.txt" 2> /dev/null)
    done

    # Our FFmpeg and Debian's, the same encoder and the same frames: the
    # same streams (the container's metadata may differ).
    echo "== export: the same as the Debian Testing .deb's"
    ref="$REPO_ROOT/tmp/release-test/$version/debian-testing/deb/smoke.mp4"
    hashes() { ffmpeg -v error -i "$1" -map 0 -c copy -f streamhash -hash sha256 - 2> /dev/null | sort; }
    if [[ -s $ref ]]; then
        want=$(hashes "$ref")
        for d in "${distros[@]}"; do
            got=$(hashes "$OUT/$d/smoke.mp4")
            check "$d: export streams" "$([[ -n $got && $got == "$want" ]] && echo 1 || echo 0)" \
                "$([[ $got == "$want" ]] && echo "video and audio identical" || echo "differ from debian-testing/deb/smoke.mp4")"
        done
    else
        check "export comparison" 0 "no ${ref#"$REPO_ROOT"/}: run release_test.sh for debian-testing first"
    fi
    finish
    exit 0
fi

shopt -s nullglob
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

footage_check

OUT="$REPO_ROOT/tmp/release-test/$version/$run"
APT_CACHE="$REPO_ROOT/tmp/release-test/apt-cache/$run"
rm -rf "$OUT"
mkdir -p "$OUT" "$APT_CACHE"
echo "== orbis-reframe $version, $target in ${BASE##*/} -> ${OUT#"$REPO_ROOT"/}"

echo "== checksums"
(cd "$DIST_DIR" && sha256sum -c --quiet ./*.sha256) && check checksums 1 "" || check checksums 0 ""

# The guide's apt line for the tarball (option B) on this target -- the
# first after a line naming it ("On Debian 13 ..."), as plain package names.
awk -v label="$(target_label "$target")" '/^### Option B/ {b=1} b && index($0, "On " label) == 1 {t=1}
    t && /^sudo apt install/ {p=1; next} p && /^```/ {exit} p' \
    "$REPO_ROOT/docs/manual/getting-started.md" | tr -d '\\' | xargs -n1 > "$OUT/apt-packages.txt"
[[ -s "$OUT/apt-packages.txt" ]] \
    || die "no apt line for $(target_label "$target") in docs/manual/getting-started.md (option B)"

# Downloaded .debs are kept between runs (Debian's images delete them).
CACHE=(-v "$APT_CACHE:/var/cache/apt/archives")

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
    cp "$OUT/apt-packages.txt" "$work/"
    podman run --rm "${GPU[@]}" "${CACHE[@]}" -v "$DIST_DIR:/dist:ro" -v "$FOOTAGE_DIR:/footage:ro" \
        -v "$work:/work" "$IMAGE" bash -c "$NOCLEAN; run_gui_test.sh $kind" > "$work/run.log" 2>&1 || true
    r() { sed -n "s/^$1=//p" "$work/install.txt" 2> /dev/null | tail -1; }

    check "$kind: install" "$([[ $(r install) == 0 && $(r ldd_missing) == 0 ]] && echo 1 || echo 0)" \
        "apt $(r install), ldd $(r ldd_missing) missing"
    # The smoke test's own checks (tests/release/gpu_smoke.sh), one line each.
    "$REPO_ROOT/tests/release/gpu_smoke.sh" --check "$work" > /dev/null 2>&1 || true
    if ! grep -q '^check_' "$work/results.txt" 2> /dev/null; then
        check "$kind: smoke test" 0 "no results: $kind/run.log"
    fi
    while IFS='=' read -r name verdict; do
        check "$kind: ${name#check_}" "$([[ $verdict == ok* ]] && echo 1 || echo 0)" \
            "$(sed -E 's/^(ok|FAIL) \((.*)\)$/\2/' <<< "$verdict")"
    done < <(grep '^check_' "$work/results.txt" 2> /dev/null)
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
    for s in stitch_source_smoke playback_bench view_compare reframe_export reframe_export_10bit \
        plate_scan; do
        check "dev: $s" "$([[ $(r $s) == 0 ]] && echo 1 || echo 0)" "dev/$s.log"
    done
fi

finish
