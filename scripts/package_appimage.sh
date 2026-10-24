#!/usr/bin/env bash
# Build the AppImage (docs/dev-environment.md, "The AppImage"): one file that
# runs on every distribution with glibc 2.41 or newer, built on Debian 13 with
# the libraries whose versions differ between distributions bundled. Only
# glibc, libstdc++, the Vulkan loader, the graphics and VA-API drivers,
# GL / EGL, fontconfig, freetype, X11 / xcb, libwayland-client and the
# sound server's client library come from the system it runs on. ->
#   dist/appimage/orbis-reframe-<version>-x86_64.AppImage   (+ .zsync)
#   dist/appimage/orbis-reframe-<version>-appimage-sources.txt
#   dist/appimage/ffmpeg-<version>.tar.xz (+ .asc), type2-runtime-<commit>-source.tar.xz
# each with a .sha256 file, followed by the cheap checks.
#
# Run by `scripts/package.sh --target appimage`, which builds the runtime
# (scripts/build_appimage_runtime.sh) on the host and then runs this in a
# debian:trixie container with the build packages; runs directly on a
# Debian 13 system that has those and the runtime. Like package.sh: a
# clean Release build with every feature (scripts/fetch_ncnn.sh run once),
# against our own FFmpeg (scripts/build_ffmpeg.sh), ctest, then
#   - cmake --install into an AppDir (usr/), the Qt plugins in
#     packaging/appimage/qt-plugins.txt and packaging/appimage/qt.conf;
#   - linuxdeploy collects the libraries the program, its plugins and ONNX
#     Runtime (dlopen()ed, so not linked) need into usr/lib/ and points the
#     RUNPATHs there; packaging/appimage/AppRun is the entry point;
#   - glvnd's libraries in usr/lib/glvnd-fallback/, for AppRun to add when
#     the system has none;
#   - every bundled file mapped to the package and version it comes from
#     (usr/share/doc/orbis-reframe/bundled.txt) and checked against the
#     licence review, packaging/appimage/licenses.tsv; their licence texts in
#     usr/share/doc/orbis-reframe/licenses/<package>/; the list of where the
#     copyleft parts' sources are (Debian's on snapshot.debian.org, links
#     checked; ours on the release), in the AppImage and next to it;
#   - appimagetool packs it behind our runtime, with the update information
#     AppImageUpdate and its kind read (a .zsync per release on GitHub).
# The tools are pinned in packaging/appimage/tools.sha256.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=package_checks.sh
. "$REPO_ROOT/scripts/package_checks.sh"
BUILD_DIR="$REPO_ROOT/build-package/appimage"
DIST_DIR="$REPO_ROOT/dist/appimage"
TOOLS_DIR="$REPO_ROOT/third_party/appimage-tools"
APPDIR="$BUILD_DIR/AppDir"
# Where AppImageUpdate, Gear Lever and AppImageLauncher look for a newer
# version: the .zsync of the latest release (not drafts or pre-releases) on
# GitHub. The app itself never looks.
UPDATE_INFO="gh-releases-zsync|Schabo|orbis-reframe|latest|orbis-reframe-*-x86_64.AppImage.zsync"
# glvnd's libraries Debian's Qt Gui links (libGLdispatch is what the other
# two need).
GLVND_FALLBACK=(libOpenGL.so.0 libGLX.so.0 libGLdispatch.so.0)

die() { echo "$*" >&2; exit 1; }

case "${1:-}" in
    "") ;;
    -h|--help)
        echo "Usage: $(basename "$0")"
        echo "Builds the AppImage into dist/appimage/ (replaced), on Debian 13."
        echo "Build dir: build-package/appimage/ (rebuilt from scratch)."
        echo "Usually run as: scripts/package.sh --target appimage"
        exit 0
        ;;
    *) die "Unknown option: $1" ;;
esac

# shellcheck source=/dev/null
[[ "$(. /etc/os-release && echo "$ID-${VERSION_ID:-}")" == debian-13 ]] \
    || die "The AppImage is built on Debian 13 (scripts/package.sh --target appimage builds it in a container)"
# Reused when built (and its hash checked); otherwise this needs podman,
# which package.sh's container doesn't have, so package.sh builds it first.
"$REPO_ROOT/scripts/build_appimage_runtime.sh"
RUNTIME="$("$REPO_ROOT/scripts/build_appimage_runtime.sh" --print-dir)/runtime-x86_64"

# The pinned tools, downloaded once and checked before every use.
mkdir -p "$TOOLS_DIR"
declare -A TOOL
while read -r sum url; do
    [[ -z "$sum" || "$sum" == \#* ]] && continue
    f="$TOOLS_DIR/$(basename "$url")"
    if ! echo "$sum  $f" | sha256sum -c --status - 2> /dev/null; then
        echo "Downloading $url"
        curl -fsSL --retry 3 -o "$f.part" "$url"
        mv "$f.part" "$f"
        echo "$sum  $f" | sha256sum -c --quiet - || die "$f: not the pinned file (packaging/appimage/tools.sha256)"
    fi
    chmod +x "$f"
    TOOL[$(basename "$url" | sed 's/-x86_64\.AppImage$//')]=$f
done < "$REPO_ROOT/packaging/appimage/tools.sha256"
[[ -n "${TOOL[linuxdeploy]:-}" && -n "${TOOL[appimagetool]:-}" ]] \
    || die "packaging/appimage/tools.sha256 must name linuxdeploy and appimagetool"
# Both are AppImages themselves; a container has no FUSE to mount them.
export APPIMAGE_EXTRACT_AND_RUN=1

"$REPO_ROOT/scripts/build_ffmpeg.sh"
FFMPEG="$("$REPO_ROOT/scripts/build_ffmpeg.sh" --print-dir)/usr"

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$DIST_DIR"

# As package.sh, plus: our FFmpeg (no distribution FFmpeg is installed, so
# nothing else can be found), and ONNX Runtime loaded by its soname only, so
# the bundled copy is the one found, not a build-machine path.
PKG_CONFIG_PATH="$FFMPEG/lib/pkgconfig" cmake -B "$BUILD_DIR" -S "$REPO_ROOT" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DORBIS_SOURCE_TREE_FALLBACK=OFF \
    -DORBIS_REQUIRE_ALL_FEATURES=ON \
    -DORBIS_ORT_BUILD_PATH=OFF \
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$REPO_ROOT/=" \
    ${NCNN_ROOT:+-DNCNN_ROOT="$NCNN_ROOT"}
cmake --build "$BUILD_DIR" -j"$(nproc)"
# Our FFmpeg's libraries have no RUNPATH, and the build's RPATH only names
# the folders of what a program links directly.
LD_LIBRARY_PATH="$FFMPEG/lib" QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD_DIR" --output-on-failure
version=$(sed -nE 's/.*ORBIS_VERSION_STRING "([^"]+)".*/\1/p' "$BUILD_DIR/generated/OrbisVersion.h")
[[ -n "$version" ]] || die "no version in $BUILD_DIR/generated/OrbisVersion.h"
echo "== orbis-reframe $version (appimage)"

# --- the AppDir
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --strip > /dev/null
qt_plugins=$(qtpaths6 --query QT_INSTALL_PLUGINS 2> /dev/null || /usr/lib/qt6/bin/qtpaths --query QT_INSTALL_PLUGINS)
deps=()
while read -r p; do
    [[ -z "$p" || "$p" == \#* ]] && continue
    [[ -f "$qt_plugins/$p" ]] || die "Qt plugin $qt_plugins/$p missing (packaging/appimage/qt-plugins.txt)"
    mkdir -p "$APPDIR/usr/plugins/$(dirname "$p")"
    cp "$qt_plugins/$p" "$APPDIR/usr/plugins/$p"
    deps+=(--deploy-deps-only "$APPDIR/usr/plugins/$p")
done < "$REPO_ROOT/packaging/appimage/qt-plugins.txt"
cp "$REPO_ROOT/packaging/appimage/qt.conf" "$APPDIR/usr/bin/qt.conf"

# The ONNX Runtime the program was built against (the -dev package's link
# names it), by its soname, the name VoiceIsolator dlopen()s; linuxdeploy
# keeps the file name it is given.
ort=$(readlink -f /usr/lib/x86_64-linux-gnu/libonnxruntime.so)
[[ -f "$ort" ]] || die "no ONNX Runtime library (libonnxruntime-dev)"
ort="$(dirname "$ort")/$(objdump -p "$ort" | awk '$1 == "SONAME" {print $2}')"
[[ -f "$ort" ]] || die "no $ort (ONNX Runtime's soname)"

# linuxdeploy finds our FFmpeg and the installed ncnn through
# LD_LIBRARY_PATH; it leaves out what its exclude list says comes from the
# system (glibc, libstdc++, GL, fontconfig, X11, ...).
(cd "$BUILD_DIR" && LD_LIBRARY_PATH="$FFMPEG/lib:$APPDIR/usr/lib/orbis-reframe" "${TOOL[linuxdeploy]}" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/orbis-reframe" \
    --library "$ort" \
    "${deps[@]}" \
    --desktop-file "$APPDIR/usr/share/applications/orbis-reframe.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/orbis-reframe.svg" \
    --custom-apprun "$REPO_ROOT/packaging/appimage/AppRun" > "$BUILD_DIR/linuxdeploy.log" 2>&1) \
    || { tail -30 "$BUILD_DIR/linuxdeploy.log"; die "linuxdeploy failed ($BUILD_DIR/linuxdeploy.log)"; }
# The Vulkan loader is the system's (it finds the system's drivers);
# linuxdeploy's list doesn't exclude it. ncnn now sits in usr/lib/ with
# everything else, where the program's new RUNPATH points.
rm -f "$APPDIR/usr/lib/libvulkan.so.1"
rm -rf "$APPDIR/usr/lib/orbis-reframe"
# linuxdeploy copies each library's copyright file into usr/share/doc/<binary
# package>/; the licence texts go into our own folder below instead, one per
# source package and only for what stays bundled.
find "$APPDIR/usr/share/doc" -mindepth 1 -maxdepth 1 ! -name orbis-reframe -exec rm -rf {} +
for lib in "${GLVND_FALLBACK[@]}"; do
    mkdir -p "$APPDIR/usr/lib/glvnd-fallback/$lib"
    cp -L "/usr/lib/x86_64-linux-gnu/$lib" "$APPDIR/usr/lib/glvnd-fallback/$lib/"
done

# --- what is bundled: every file linuxdeploy and the plugins brought, mapped
# to the package and version it comes from, checked against the licence
# review, with the licence texts and the list of where the sources are.
DOC_DIR="$APPDIR/usr/share/doc/orbis-reframe"
LICENSES_TSV="$REPO_ROOT/packaging/appimage/licenses.tsv"
ffmpeg_version=$("$REPO_ROOT/scripts/build_ffmpeg.sh" --print-version)
ffmpeg_tarball="$(dirname "$("$REPO_ROOT/scripts/build_ffmpeg.sh" --print-dir)")/ffmpeg-$ffmpeg_version.tar.xz"
runtime_dir=$("$REPO_ROOT/scripts/build_appimage_runtime.sh" --print-dir)
runtime_version=$(basename "$runtime_dir")
ncnn_dir=$(sed -n 's/^ncnn_DIR:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
ncnn_version=$(sed -n 's/^set(PACKAGE_VERSION "\(.*\)")$/\1/p' "$ncnn_dir/ncnnConfigVersion.cmake")
[[ -n "$ncnn_version" ]] || die "no ncnn version in $ncnn_dir/ncnnConfigVersion.cmake"
# Debian's upstream version: what licenses.tsv's review holds to.
upstream() { if [[ "$1" == *-* ]]; then echo "${1%-*}"; else echo "$1"; fi; }

# path <TAB> binary package <TAB> source package <TAB> source version <TAB>
# binary package version ("-": not from a Debian package). Debian's updates
# come per binary package, whose version can differ from its source's
# (rebuilds), so the weekly check (scripts/check_appimage_updates.sh) reads
# the last column.
manifest="$BUILD_DIR/bundled.tsv"
: > "$manifest"
while IFS= read -r rel; do
    name=$(basename "$rel")
    if [[ "$rel" == usr/plugins/* ]]; then
        sys="$qt_plugins/${rel#usr/plugins/}"
    elif [[ -e "$FFMPEG/lib/$name" ]]; then
        printf '%s\t-\tffmpeg\t%s\t-\n' "$rel" "$ffmpeg_version" >> "$manifest"; continue
    elif [[ -e "$ncnn_dir/../../$name" ]]; then
        printf '%s\t-\tncnn\t%s\t-\n' "$rel" "$ncnn_version" >> "$manifest"; continue
    else
        sys="/usr/lib/x86_64-linux-gnu/$name"
    fi
    # The file itself, or the soname link pointing at it (libblas and
    # liblapack go through /etc/alternatives).
    owner=$(dpkg -S "$(readlink -f "$sys")" 2> /dev/null || dpkg -S "$sys" 2> /dev/null || true)
    owner=$(grep -v '^diversion ' <<< "$owner" | sed -n '1s/:.*//p' || true)
    [[ -n "$owner" ]] || die "$rel: can't tell which package it comes from (looked for $sys)"
    printf '%s\t%s\t%s\n' "$rel" "$owner" \
        "$(dpkg-query -W -f '${source:Package}\t${source:Version}\t${Version}' "$owner")" >> "$manifest"
done < <(cd "$APPDIR" && find usr/lib usr/plugins -type f | sort)
printf 'runtime\t-\ttype2-runtime\t%s\t-\n' "$runtime_version" >> "$manifest"

# Nothing else in the AppDir that isn't ours: the install's files and what
# this script puts there.
unplaced=$(cd "$APPDIR" && find . \( -type f -o -type l \) -printf '%P\n' | sort \
    | grep -v -e '^usr/lib/' -e '^usr/plugins/' \
        -e '^AppRun$' -e '^\.DirIcon$' -e '^orbis-reframe\.desktop$' -e '^orbis-reframe\.svg$' -e '^usr/bin/qt\.conf$' \
    | comm -23 - <(sed 's,^/,,' "$BUILD_DIR/install_manifest.txt" | sort) || true)
[[ -z "$unplaced" ]] || die "files in the AppDir that are neither installed by us nor mapped to a package:
$unplaced"

# The review: a row for every source package, at the reviewed upstream
# version, naming every binary package; no row for anything not bundled.
declare -A REVIEW_VERSION REVIEW_SHIPS REVIEW_OFFER
while IFS=$'\t' read -r pkg ver ships _ offer _; do
    [[ -z "$pkg" || "$pkg" == \#* || "$pkg" == package ]] && continue
    REVIEW_VERSION[$pkg]=$ver
    REVIEW_SHIPS[$pkg]=", $ships,"
    REVIEW_OFFER[$pkg]=$offer
done < "$LICENSES_TSV"
lic_fail=0
declare -A USED
while IFS=$'\t' read -r rel bin src ver _; do
    if [[ -z "${REVIEW_VERSION[$src]:-}" ]]; then
        echo "FAIL: $rel: $src $ver isn't in packaging/appimage/licenses.tsv (review it)" >&2; lic_fail=1; continue
    fi
    [[ "$(upstream "$ver")" == "$(upstream "${REVIEW_VERSION[$src]}")" ]] \
        || { echo "FAIL: $rel: $src is $ver, the review is of ${REVIEW_VERSION[$src]} (packaging/appimage/licenses.tsv)" >&2; lic_fail=1; }
    [[ "$bin" == - || "${REVIEW_SHIPS[$src]}" == *", $bin,"* ]] \
        || { echo "FAIL: $rel: binary package $bin isn't in $src's ships column (packaging/appimage/licenses.tsv)" >&2; lic_fail=1; }
    USED[$src]=$ver
done < "$manifest"
for pkg in "${!REVIEW_VERSION[@]}"; do
    [[ -n "${USED[$pkg]:-}" ]] || { echo "FAIL: $pkg is in packaging/appimage/licenses.tsv but nothing bundled comes from it" >&2; lic_fail=1; }
done
[[ "$lic_fail" -eq 0 ]] || die "the bundle and its licence review (packaging/appimage/licenses.tsv) differ"

{
    echo "# The files orbis-reframe $version's AppImage bundles and where each comes"
    echo "# from: path, Debian 13 binary package, source package, source version,"
    echo "# binary package version (\"-\": our own build). \"runtime\" is the"
    echo "# AppImage's runtime, in front of the image."
    echo "# Licence texts: licenses/<source package>/; where the sources are:"
    echo "# licenses/appimage-sources.txt."
    cat "$manifest"
} > "$DOC_DIR/bundled.txt"

# Licence texts, one folder per source package.
while IFS=$'\t' read -r _ bin src _; do
    dest="$DOC_DIR/licenses/$src"
    mkdir -p "$dest"
    case "$src" in
        ffmpeg) cp "$FFMPEG/share/doc/ffmpeg/"* "$dest/" ;;
        ncnn) cp "$REPO_ROOT/packaging/licenses/ncnn-LICENSE.txt" "$dest/" ;;
        type2-runtime) cp "$runtime_dir/licenses/"* "$dest/" ;;
        *)
            # One copyright file per source package; Debian installs the same
            # one with each binary package.
            f="/usr/share/doc/$bin/copyright"
            [[ -f "$f" ]] || die "no $f"
            if [[ -f "$dest/copyright" ]]; then
                cmp -s "$f" "$dest/copyright" || die "$f differs from the other copyright files of $src"
            else
                cp -L "$f" "$dest/copyright"
            fi
            ;;
    esac
done < "$manifest"

# Where the source of each copyleft component is: Debian's by exact version on
# snapshot.debian.org (checked here), ours on the release. In the AppImage and
# on the release.
sources_txt="$BUILD_DIR/appimage-sources.txt"
snapshot_url() { echo "https://snapshot.debian.org/package/$1/${2//:/%3A}/"; }
{
    echo "Source code for orbis-reframe $version's AppImage"
    echo "(orbis-reframe-$version-x86_64.AppImage)"
    echo
    echo "orbis-reframe itself: orbis-reframe-$version-source.tar.xz, on the same"
    echo "release at https://github.com/Schabo/orbis-reframe/releases."
    echo
    echo "The libraries it bundles under a copyleft licence (GPL, LGPL), with the"
    echo "source of the exact versions bundled. Debian 13's packages, on"
    echo "snapshot.debian.org (each page links the .dsc and the tarballs):"
    echo
    for src in $(printf '%s\n' "${!USED[@]}" | sort); do
        [[ "${REVIEW_OFFER[$src]}" == snapshot ]] || continue
        printf '  %-14s %-26s %s\n' "$src" "${USED[$src]}" "$(snapshot_url "$src" "${USED[$src]}")"
    done
    echo
    echo "Not in Debian, so on the same release as the AppImage:"
    echo
    printf '  %-14s %-26s %s\n' ffmpeg "$ffmpeg_version" "$(basename "$ffmpeg_tarball") (+ .asc, FFmpeg's signature)"
    printf '  %-14s %-26s %s\n' "" "" "unchanged, configured with (licenses/ffmpeg/configure.txt):"
    fold -s -w 56 "$FFMPEG/share/doc/ffmpeg/configure.txt" | sed -e 's/ *$//' -e 's/^/                                            /'
    printf '  %-14s %-26s %s\n' type2-runtime "$runtime_version" "type2-runtime-$runtime_version-source.tar.xz"
    printf '  %-14s %-26s %s\n' "" "" "(the runtime, with libfuse and squashfuse; its README"
    printf '  %-14s %-26s %s\n' "" "" "says how to rebuild it)"
    echo
    echo "Every other bundled library is under a permissive licence. The licence"
    echo "texts of all of them are in the AppImage, in"
    echo "  usr/share/doc/orbis-reframe/licenses/<package>/"
    echo "and"
    echo "  usr/share/doc/orbis-reframe/bundled.txt"
    echo "lists which file comes from which package."
} > "$sources_txt"
cp "$sources_txt" "$DOC_DIR/licenses/appimage-sources.txt"

echo "== checking the snapshot.debian.org links"
for src in $(printf '%s\n' "${!USED[@]}" | sort); do
    [[ "${REVIEW_OFFER[$src]}" == snapshot ]] || continue
    ver=${USED[$src]}
    # The machine-readable list of the version's source files, and the page
    # the list links.
    files=$(curl -fsS --retry 3 --max-time 120 "https://snapshot.debian.org/mr/package/$src/${ver//:/%3A}/srcfiles?fileinfo=1" || true)
    grep -qF "\"name\":\"${src}_${ver#*:}.dsc\"" <<< "$files" \
        && curl -fsS --retry 3 --max-time 120 -o /dev/null "$(snapshot_url "$src" "$ver")" \
        || die "snapshot.debian.org has no $src $ver (a security update not imported yet? it imports every few hours)"
done

appimage="$DIST_DIR/orbis-reframe-$version-x86_64.AppImage"
# zsyncmake (zsync) writes the .zsync next to the AppImage, into the
# working folder.
(cd "$DIST_DIR" && ARCH=x86_64 "${TOOL[appimagetool]}" --no-appstream \
    --runtime-file "$RUNTIME" -u "$UPDATE_INFO" \
    "$APPDIR" "$(basename "$appimage")" > "$BUILD_DIR/appimagetool.log" 2>&1) \
    || { tail -30 "$BUILD_DIR/appimagetool.log"; die "appimagetool failed ($BUILD_DIR/appimagetool.log)"; }
# The sources this release hosts, and the list of all of them.
cp "$ffmpeg_tarball" "$ffmpeg_tarball.asc" "$runtime_dir/type2-runtime-$runtime_version-source.tar.xz" "$DIST_DIR/"
cp "$sources_txt" "$DIST_DIR/orbis-reframe-$version-appimage-sources.txt"
(cd "$DIST_DIR" && for f in *; do sha256sum "$f" > "$f.sha256"; done)

# --- cheap checks, on the unpacked AppImage
echo "== checks"
fail=0
check_dir="$BUILD_DIR/check"
mkdir -p "$check_dir"
(cd "$check_dir" && "$appimage" --appimage-extract > /dev/null)
root="$check_dir/squashfs-root"

[[ -f "$appimage.zsync" ]] || { echo "FAIL: no .zsync next to the AppImage" >&2; fail=1; }
# The runtime answers this itself, but not when told to extract and run.
info=$(env -u APPIMAGE_EXTRACT_AND_RUN "$appimage" --appimage-updateinformation 2> /dev/null || true)
[[ "$info" == "$UPDATE_INFO" ]] \
    || { echo "FAIL: update information is '$info', expected $UPDATE_INFO" >&2; fail=1; }
cmp -s "$root/AppRun" "$REPO_ROOT/packaging/appimage/AppRun" && [[ -x "$root/AppRun" ]] \
    || { echo "FAIL: AppRun isn't packaging/appimage/AppRun" >&2; fail=1; }

reported=$("$appimage" --version)
[[ "$reported" == "orbis-reframe $version" ]] \
    || { echo "FAIL: --version says '$reported', expected $version" >&2; fail=1; }

# Every ELF file's libraries found, with nothing from the environment; the
# bundled ones (FFmpeg, ncnn, ONNX Runtime) from usr/lib/.
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q '^ELF' || continue
    missing=$(LD_LIBRARY_PATH= ldd "$f" 2>&1 | grep "not found" || true)
    [[ -z "$missing" ]] || { echo "FAIL: ${f#"$root/"}: libraries not found:" >&2; echo "$missing" >&2; fail=1; }
done < <(find "$root/usr" -type f \( -name '*.so*' -o -path '*/bin/*' \) -print0)
# ldd names them through the RUNPATH ($ORIGIN/../lib): resolve the paths.
program_libs=$(LD_LIBRARY_PATH= ldd "$root/usr/bin/orbis-reframe" | awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}' \
    | while read -r so path; do echo "$so $(readlink -f "$path")"; done)
for lib in libavcodec libavformat libavutil libavfilter libswscale libswresample libncnn; do
    grep -q "^$lib\.so[.0-9]* $root/usr/lib/[^/]*$" <<< "$program_libs" \
        || { echo "FAIL: $lib isn't taken from the AppImage's usr/lib/" >&2; fail=1; }
done
[[ -f "$root/usr/lib/$(basename "$ort")" ]] \
    || { echo "FAIL: ONNX Runtime isn't bundled as $(basename "$ort")" >&2; fail=1; }
# Our FFmpeg, not another: avcodec_configuration() is the configure line.
grep -qF -- "$(cat "$FFMPEG/share/doc/ffmpeg/configure.txt")" "$root"/usr/lib/libavcodec.so.* \
    || { echo "FAIL: the bundled libavcodec isn't our build" >&2; fail=1; }
for f in libvulkan.so.1 orbis-reframe; do
    [[ ! -e "$root/usr/lib/$f" ]] || { echo "FAIL: usr/lib/$f shouldn't be bundled" >&2; fail=1; }
done
for lib in "${GLVND_FALLBACK[@]}"; do
    [[ -f "$root/usr/lib/glvnd-fallback/$lib/$lib" ]] \
        || { echo "FAIL: glvnd fallback $lib missing" >&2; fail=1; }
done
for p in $(grep -v -e '^#' -e '^$' "$REPO_ROOT/packaging/appimage/qt-plugins.txt"); do
    [[ -f "$root/usr/plugins/$p" ]] || { echo "FAIL: Qt plugin $p missing" >&2; fail=1; }
done

# The manifest names every bundled library, each package it names has its
# licence texts, and the sources list is in the AppImage and next to it with
# the sources it says the release hosts.
doc="$root/usr/share/doc/orbis-reframe"
if [[ -f "$doc/bundled.txt" ]]; then
    while IFS= read -r rel; do
        grep -qF "$rel"$'\t' "$doc/bundled.txt" || { echo "FAIL: $rel isn't in bundled.txt" >&2; fail=1; }
    done < <(cd "$root" && find usr/lib usr/plugins -type f)
    for src in $(grep -v '^#' "$doc/bundled.txt" | cut -f3 | sort -u); do
        [[ -n "$(ls -A "$doc/licenses/$src" 2> /dev/null)" ]] \
            || { echo "FAIL: no licence texts in licenses/$src/" >&2; fail=1; }
    done
else
    echo "FAIL: no usr/share/doc/orbis-reframe/bundled.txt" >&2; fail=1
fi
cmp -s "$doc/licenses/appimage-sources.txt" "$DIST_DIR/orbis-reframe-$version-appimage-sources.txt" \
    || { echo "FAIL: the sources list isn't in the AppImage and next to it" >&2; fail=1; }
for f in "$(basename "$ffmpeg_tarball")" "$(basename "$ffmpeg_tarball").asc" "type2-runtime-$runtime_version-source.tar.xz"; do
    [[ -f "$DIST_DIR/$f" ]] || { echo "FAIL: $f (named in the sources list) isn't in $DIST_DIR" >&2; fail=1; }
done
"$REPO_ROOT/scripts/appimage_third_party.sh" --check || fail=1

check_install_tree "$root/usr" || fail=1

if [[ "$fail" -ne 0 ]]; then
    echo "== AppImage checks failed" >&2
    exit 1
fi
echo "== done ($(du -sh "$root/usr/lib" | cut -f1) of libraries, $(find "$root/usr/lib" -type f | wc -l) files)"
ls -l "$DIST_DIR"
