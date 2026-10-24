#!/usr/bin/env bash
# Build our own FFmpeg: the pinned release, LGPL only, shared libraries, with
# only what the program uses (Vulkan, VAAPI, NVENC; no network, no capture
# devices). The AppImage bundles it instead of a distribution's FFmpeg
# (docs/dev-environment.md, "Our own FFmpeg"); the per-distribution .debs
# keep the system's.
#
# The tarball is checked against its pinned SHA-256 and FFmpeg's release
# signing key (scripts/ffmpeg-release-key.asc, fingerprint below, as listed on
# https://ffmpeg.org/download.html), and kept: it is the source we host next
# to a release that bundles this build. Output, under third_party/ffmpeg/
# (gitignored):
#   ffmpeg-<version>.tar.xz (+ .asc)      the verified source
#   <version>-<os>/usr/{lib,include}      the build, per build system (<os> is
#                                         /etc/os-release's ID-VERSION_CODENAME)
#   <version>-<os>/usr/lib/pkgconfig      .pc files pointing at that folder
#   <version>-<os>/usr/share/doc/ffmpeg/  licence texts, configure line
# Configured with --prefix=/usr so no build-machine path ends up in the
# libraries (avcodec_configuration() embeds the configure line).
#
#   PKG_CONFIG_PATH=$(scripts/build_ffmpeg.sh --print-dir)/usr/lib/pkgconfig
#
# A finished build with the same configure line is reused. Needs a C
# compiler, make, nasm, pkgconf, curl, gpg and the headers of what it enables:
# libvulkan-dev libva-dev libdrm-dev libffmpeg-nvenc-dev zlib1g-dev.
set -euo pipefail

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
FFMPEG_KEY_FPR=FCF986EA15E6E293A5644F10B4322F04D67658D8
URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"

# LGPL only: never --enable-gpl (we encode on the GPU, so no x264 / x265) or
# --enable-nonfree (makes the build unredistributable; NVENC doesn't need it).
# --disable-autodetect: nothing is linked that isn't named here.
# --disable-network: the program only reads and writes local files, so no
# network protocol is built in (and none of their security fixes are ours to
# ship); --disable-avdevice: capture devices aren't used.
CONFIGURE_FLAGS=(
    --prefix=/usr
    --enable-shared --disable-static --disable-programs --disable-doc
    --disable-debug --disable-autodetect --disable-network --disable-avdevice
    --enable-pthreads --enable-zlib --enable-iconv
    --enable-vulkan --enable-vaapi --enable-libdrm --enable-ffnvcodec --enable-nvenc
)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="$REPO_ROOT/third_party/ffmpeg"
# shellcheck source=/dev/null
OS_TAG=$(. /etc/os-release && echo "${ID}-${VERSION_CODENAME:-${VERSION_ID:-unknown}}")
OUT="$CACHE/$FFMPEG_VERSION-$OS_TAG"

case "${1:-}" in
    --print-dir) echo "$OUT"; exit 0 ;;
    --print-version) echo "$FFMPEG_VERSION"; exit 0 ;;
    "") ;;
    -h|--help)
        echo "Usage: $(basename "$0") [--print-dir | --print-version]"
        echo "Builds FFmpeg $FFMPEG_VERSION into $CACHE/$FFMPEG_VERSION-<os>/ (reused when done)."
        echo "  --print-dir      print that folder for this system and exit"
        echo "  --print-version  print the FFmpeg version and exit"
        exit 0
        ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
esac

STAMP="$OUT/usr/share/doc/ffmpeg/configure.txt"
if [[ -f "$STAMP" && "$(cat "$STAMP")" == "${CONFIGURE_FLAGS[*]}" ]]; then
    echo "FFmpeg $FFMPEG_VERSION already built in $OUT"
    exit 0
fi

mkdir -p "$CACHE"
TARBALL="$CACHE/ffmpeg-$FFMPEG_VERSION.tar.xz"
for f in "$TARBALL" "$TARBALL.asc"; do
    if [[ ! -f "$f" ]]; then
        echo "Downloading $URL${f#"$TARBALL"}"
        curl -fsSL --retry 3 -o "$f.part" "$URL${f#"$TARBALL"}"
        mv "$f.part" "$f"
    fi
done
echo "$FFMPEG_SHA256  $TARBALL" | sha256sum -c -

# The signature, by the pinned key only: a throwaway keyring holding just
# that key, and gpg's VALIDSIG line must name its fingerprint. --no-autostart:
# a public key needs no gpg-agent, which a build container may not have.
GNUPGHOME=$(mktemp -d)
export GNUPGHOME
WORK=""
trap 'rm -rf "$GNUPGHOME" ${WORK:+"$WORK"}' EXIT
gpg --batch --quiet --no-autostart --import "$REPO_ROOT/scripts/ffmpeg-release-key.asc" 2> /dev/null \
    || { echo "gpg couldn't import scripts/ffmpeg-release-key.asc" >&2; exit 1; }
status=$(gpg --batch --no-autostart --status-fd 1 --verify "$TARBALL.asc" "$TARBALL" 2> /dev/null || true)
if ! grep -q "^\[GNUPG:\] VALIDSIG $FFMPEG_KEY_FPR " <<< "$status"; then
    echo "$TARBALL: no valid signature by FFmpeg's release key $FFMPEG_KEY_FPR" >&2
    exit 1
fi
echo "$TARBALL: signed by FFmpeg's release key"

WORK=$(mktemp -d "$CACHE/build.XXXXXX")
tar -xJf "$TARBALL" -C "$WORK"
SRC="$WORK/ffmpeg-$FFMPEG_VERSION"
(
    cd "$SRC"
    ./configure "${CONFIGURE_FLAGS[@]}" > "$WORK/configure.log" 2>&1 \
        || { tail -20 "$WORK/configure.log"; tail -40 ffbuild/config.log; exit 1; }
    # configure's own summary says which licence the result is under.
    grep -q '^License: LGPL version 2.1 or later$' "$WORK/configure.log" \
        || { echo "configure didn't report an LGPL build:" >&2; grep '^License' "$WORK/configure.log" >&2; exit 1; }
    make -j"$(nproc)" > "$WORK/make.log" 2>&1 || { tail -40 "$WORK/make.log"; exit 1; }
    rm -rf "$OUT"
    make install DESTDIR="$OUT" > /dev/null
)
# The .pc files say prefix=/usr; point them at this folder so the app's
# pkg-config finds these headers and libraries.
sed -i "s|^prefix=/usr\$|prefix=$OUT/usr|; s|=/usr/|=$OUT/usr/|" "$OUT"/usr/lib/pkgconfig/*.pc
mkdir -p "$OUT/usr/share/doc/ffmpeg"
cp "$SRC"/{LICENSE.md,COPYING.LGPLv2.1} "$OUT/usr/share/doc/ffmpeg/"
echo "${CONFIGURE_FLAGS[*]}" > "$STAMP"
echo "FFmpeg $FFMPEG_VERSION ready in $OUT"
