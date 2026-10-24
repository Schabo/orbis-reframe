#!/usr/bin/env bash
# Build the release packages (docs/RELEASE.md) for one target
# (scripts/targets.sh): a clean Release build with every feature, then CPack
# (cmake/Packaging.cmake) ->
#   dist/<target>/orbis-reframe-<version>-<target>-x86_64.tar.xz
#   dist/<target>/orbis-reframe_<version>[.deb13|.ubuntu26.04]_amd64.deb
#   dist/<target>/orbis-reframe-<version>-source.tar.xz   (git archive of HEAD, GPL)
# each with a .sha256 file, followed by the cheap checks (no build-machine
# paths in the binaries, every library found, --version). The GPU half of the
# release check is scripts/release_test.sh.
#
# <version> is git describe's (cmake/GitVersion.cmake): 0.1.0 on a release
# tag, 0.1.0+12.gabc1234 after it. Needs scripts/fetch_ncnn.sh run once and
# the build dependencies in docs/dev-environment.md, all of them -- a missing
# one fails the configure (ORBIS_REQUIRE_ALL_FEATURES).
#
# The target is this machine's (Debian Testing: debian-testing). Another one
# (--target debian-13, ubuntu-26.04) is built in a podman container of that release, which
# installs the build dependencies itself; the source tree is mounted into it.
#
# The .deb's Maintainer is CPACK_PACKAGE_CONTACT in cmake/Packaging.cmake;
# ORBIS_MAINTAINER="Name <email>" overrides it for one run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=targets.sh
. "$REPO_ROOT/scripts/targets.sh"
HOST_TARGET=$(host_target)
TARGET=$HOST_TARGET
ALLOW_DIRTY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --target) TARGET=$2; shift 2 ;;
        -h|--help)
            echo "Usage: $(basename "$0") [--target TARGET] [--allow-dirty]"
            echo "  --target TARGET  ${ORBIS_TARGETS[*]} (default: this machine's, $HOST_TARGET);"
            echo "                   another one is built in a podman container"
            echo "  --allow-dirty    package uncommitted changes (version gets .dirty;"
            echo "                   the source archive is still HEAD)"
            echo "Output: dist/<target>/ (replaced). Build dir: build-package/<target>/ (rebuilt from scratch)."
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done
target_image "$TARGET" > /dev/null || { echo "Unknown target: $TARGET (${ORBIS_TARGETS[*]})" >&2; exit 1; }
BUILD_DIR="$REPO_ROOT/build-package/$TARGET"
DIST_DIR="$REPO_ROOT/dist/$TARGET"

cd "$REPO_ROOT"
if [[ "$ALLOW_DIRTY" -eq 0 && -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "Uncommitted changes -- commit them, or pass --allow-dirty" >&2
    exit 1
fi

if [[ "$TARGET" != "$HOST_TARGET" ]]; then
    # Another release: this script again, in a container of it, with
    # docs/dev-environment.md's build dependencies and what packaging needs.
    # The tree is mounted at the same path and HOME is passed on, so the
    # prefix map and the "no build-machine paths" check see this machine's. Downloaded .debs are kept between runs.
    command -v podman > /dev/null || { echo "podman isn't installed (needed for --target $TARGET)" >&2; exit 1; }
    apt_cache="$REPO_ROOT/tmp/package-apt-cache/$TARGET"
    mkdir -p "$apt_cache"
    echo "== $TARGET: building in $(target_image "$TARGET")"
    podman run --rm -v "$REPO_ROOT:$REPO_ROOT" -v "$apt_cache:/var/cache/apt/archives" \
        -w "$REPO_ROOT" -e DEBIAN_FRONTEND=noninteractive -e HOME="$HOME" \
        ${ORBIS_MAINTAINER:+-e ORBIS_MAINTAINER="$ORBIS_MAINTAINER"} \
        "$(target_image "$TARGET")" bash -c "
            set -e
            rm -f /etc/apt/apt.conf.d/docker-clean
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends \
                g++ cmake ninja-build pkgconf \
                qt6-base-dev qt6-base-dev-tools qt6-wayland qt6-svg-plugins \
                libvulkan-dev glslang-tools nlohmann-json3-dev zlib1g-dev \
                libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
                libopencv-dev libopencv-contrib-dev libonnxruntime-dev \
                git dpkg-dev file desktop-file-utils shared-mime-info xz-utils ca-certificates > /dev/null
            scripts/package.sh --target $TARGET $([[ $ALLOW_DIRTY -eq 1 ]] && echo --allow-dirty)"
    exit $?
fi

# The version the binary will report, worked out the same way as its build.
project_version=$(sed -nE 's/^project\(orbis-reframe VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)
version_header=$(mktemp)
trap 'rm -f "$version_header"' EXIT
cmake -DSOURCE_DIR="$REPO_ROOT" -DVERSION="$project_version" -DOUT="$version_header" \
    -P cmake/GitVersion.cmake
version=$(sed -nE 's/.*ORBIS_VERSION_STRING "([^"]+)".*/\1/p' "$version_header")
echo "== orbis-reframe $version ($TARGET)"

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$DIST_DIR"

# -ffile-prefix-map: __FILE__ (asserts, logs) and debug info name files
# relative to the source tree, not this machine's checkout.
cmake -B "$BUILD_DIR" -S "$REPO_ROOT" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DORBIS_PACKAGE_TARGET="$TARGET" \
    -DORBIS_SOURCE_TREE_FALLBACK=OFF \
    -DORBIS_REQUIRE_ALL_FEATURES=ON \
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$REPO_ROOT/=" \
    ${ONNXRUNTIME_ROOT:+-DONNXRUNTIME_ROOT="$ONNXRUNTIME_ROOT"} \
    ${NCNN_ROOT:+-DNCNN_ROOT="$NCNN_ROOT"}
cmake --build "$BUILD_DIR" -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD_DIR" --output-on-failure

(cd "$BUILD_DIR" && cpack -B "$DIST_DIR" \
    -D CPACK_PACKAGE_VERSION="$version" \
    ${ORBIS_MAINTAINER:+-D CPACK_PACKAGE_CONTACT="$ORBIS_MAINTAINER"})
rm -rf "$DIST_DIR/_CPack_Packages"

git archive --format=tar --prefix="orbis-reframe-$version/" HEAD \
    | xz -T0 > "$DIST_DIR/orbis-reframe-$version-source.tar.xz"

# GitHub turns a '~' in an uploaded file's name into '.', so the .deb gets
# that name here already (the package's Version: keeps the '~'); its .sha256
# then names the file people download.
for f in "$DIST_DIR"/orbis-reframe_*~*.deb; do
    if [[ -e $f ]]; then mv "$f" "${f//\~/.}"; fi
done
tarball="$DIST_DIR/orbis-reframe-$version-$TARGET-$(uname -m).tar.xz"
deb=$(ls "$DIST_DIR"/orbis-reframe_*.deb)
(cd "$DIST_DIR" && for f in *.tar.xz *.deb; do sha256sum "$f" > "$f.sha256"; done)

# --- cheap checks, on the unpacked tarball (the .deb holds the same tree)
echo "== checks"
check_dir="$BUILD_DIR/check"
mkdir -p "$check_dir"
tar -xJf "$tarball" -C "$check_dir"
top="$check_dir/$(basename "$tarball" .tar.xz)"
fail=0

[[ -x "$top/orbis-reframe" ]] || { echo "FAIL: no launcher at the tarball's top" >&2; fail=1; }

reported=$("$top/orbis-reframe" --version)
[[ "$reported" == "orbis-reframe $version" ]] \
    || { echo "FAIL: --version says '$reported', expected $version" >&2; fail=1; }

missing=$(ldd "$top/bin/orbis-reframe" | grep "not found" || true)
[[ -z "$missing" ]] || { echo "FAIL: libraries not found:" >&2; echo "$missing" >&2; fail=1; }
grep -q "lib/orbis-reframe/libncnn" <(ldd "$top/bin/orbis-reframe") \
    || { echo "FAIL: libncnn isn't taken from the package's lib/orbis-reframe/" >&2; fail=1; }

for needle in "$REPO_ROOT" "$HOME"; do
    hits=$(grep -rlF "$needle" "$top" || true)
    [[ -z "$hits" ]] || { echo "FAIL: '$needle' found in:" >&2; echo "$hits" >&2; fail=1; }
done

for m in deepfilternet3/enc.onnx vittrack/vittrack.onnx \
         plates/yolo_v9_s_608_license_plates_heads.ncnn.param; do
    [[ -f "$top/share/orbis-reframe/models/$m" ]] || { echo "FAIL: model $m missing" >&2; fail=1; }
done
[[ -f "$top/share/orbis-reframe/luts/HLG to Rec.709.cube" ]] \
    || { echo "FAIL: the HLG to Rec.709 LUT is missing" >&2; fail=1; }

# License texts (THIRD_PARTY.md).
for f in doc/orbis-reframe/LICENSE doc/orbis-reframe/THIRD_PARTY.md \
         doc/orbis-reframe/LICENSE.lucide doc/orbis-reframe/licenses/ncnn-LICENSE.txt \
         doc/orbis-reframe/licenses/nlohmann-json-LICENSE.MIT doc/orbis-reframe/licenses/miniaudio-LICENSE \
         doc/orbis-reframe/licenses/insv-LICENSE-MIT doc/orbis-reframe/licenses/insv-LICENSE-APACHE \
         orbis-reframe/models/deepfilternet3/LICENSE-MIT orbis-reframe/models/deepfilternet3/LICENSE-APACHE \
         orbis-reframe/models/vittrack/LICENSE-APACHE orbis-reframe/models/plates/LICENSE-MIT; do
    [[ -f "$top/share/$f" ]] || { echo "FAIL: license file share/$f missing" >&2; fail=1; }
done

desktop-file-validate "$top/share/applications/orbis-reframe.desktop" || fail=1
# The file types compile, and give .orp and .insv the types the desktop entry
# names.
mime_db=$(mktemp -d)
mkdir "$mime_db/packages"
cp "$top/share/mime/packages/orbis-reframe.xml" "$mime_db/packages/" 2>/dev/null \
    || { echo "FAIL: share/mime/packages/orbis-reframe.xml missing" >&2; fail=1; }
if update-mime-database "$mime_db" 2>/dev/null; then
    for glob in application/x-orbis-reframe-project:\*.orp video/x-insta360-insv:\*.insv; do
        cut -d: -f2- "$mime_db/globs2" | grep -qxF "$glob" \
            || { echo "FAIL: no $glob in the file types" >&2; fail=1; }
        grep -q "^MimeType=.*${glob%%:*};" "$top/share/applications/orbis-reframe.desktop" \
            || { echo "FAIL: ${glob%%:*} not in the desktop entry's MimeType" >&2; fail=1; }
    done
else
    echo "FAIL: the file types don't compile" >&2; fail=1
fi
rm -rf "$mime_db"

dpkg-deb --field "$deb" Package Version Depends Recommends Suggests
[[ $(dpkg-deb --contents "$deb" | grep -c "/usr/bin/orbis-reframe$") -eq 1 ]] \
    || { echo "FAIL: no /usr/bin/orbis-reframe in the .deb" >&2; fail=1; }

if [[ "$fail" -ne 0 ]]; then
    echo "== package checks failed" >&2
    exit 1
fi
echo "== done"
ls -l "$DIST_DIR"
