#!/usr/bin/env bash
# Download the pinned ncnn release into third_party/ncnn/ (gitignored), where
# CMakeLists.txt finds it without NCNN_ROOT. ncnn runs the privacy blur's plate
# scan; it isn't packaged in Debian, so packaged builds ship libncnn.so.1 in
# lib/orbis-reframe/. Keep NCNN_VERSION in sync
# with CMakeLists.txt and docs/dev-environment.md.
set -euo pipefail

NCNN_VERSION=20260526
NCNN_ASSET="ncnn-${NCNN_VERSION}-ubuntu-2404-shared"
NCNN_SHA256=6f003ab9394921fe7090932151b144ad53b1855971429a78b8cf4a5dbc6ba23e
URL="https://github.com/Tencent/ncnn/releases/download/${NCNN_VERSION}/${NCNN_ASSET}.zip"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/third_party/ncnn"

if [[ -f "$DEST/$NCNN_ASSET/lib/cmake/ncnn/ncnnConfig.cmake" ]]; then
    echo "ncnn $NCNN_VERSION already in $DEST/$NCNN_ASSET"
    exit 0
fi

mkdir -p "$DEST"
ZIP="$DEST/$NCNN_ASSET.zip"
if [[ ! -f "$ZIP" ]]; then
    echo "Downloading $URL"
    curl -fsSL --retry 3 -o "$ZIP.part" "$URL"
    mv "$ZIP.part" "$ZIP"
fi
echo "$NCNN_SHA256  $ZIP" | sha256sum -c -
rm -rf "${DEST:?}/$NCNN_ASSET"
unzip -q "$ZIP" -d "$DEST"
# The release zip stores the .so symlinks as plain files on some unzip
# versions; recreate them so the soname resolves.
LIB="$DEST/$NCNN_ASSET/lib"
REAL="$(cd "$LIB" && ls libncnn.so.1.* | head -1)"
ln -sf "$REAL" "$LIB/libncnn.so.1"
ln -sf libncnn.so.1 "$LIB/libncnn.so"
echo "ncnn $NCNN_VERSION ready in $DEST/$NCNN_ASSET"
