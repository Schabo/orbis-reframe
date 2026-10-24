#!/usr/bin/env bash
# Build the AppImage's runtime (the static type-2 runtime, the program at the
# front of every AppImage that mounts the image and starts AppRun) from
# AppImage/type2-runtime at a pinned commit, the way its release workflow
# builds it, and check that the result is byte-identical to that release's
# binary (RUNTIME_SHA256). The runtime links libfuse (LGPL-2.1-only)
# statically, so its source and the runtime's must be offered next to every
# AppImage; building it ourselves makes the source we host exactly what is
# in the file.
#
# Output, under third_party/appimage-runtime/<commit>/ (gitignored):
#   runtime-x86_64                         the runtime, for appimagetool
#   type2-runtime-<commit>-source.tar.xz   to host on the release: the
#                                          runtime's tree at that commit,
#                                          libfuse's and squashfuse's
#                                          tarballs, the build's Alpine
#                                          packages, how to rebuild
# A finished build is reused. Runs upstream's build in an Alpine container
# (scripts/docker/Dockerfile in their tree), so it needs podman, network
# access and the host's /dev/fuse (see below). Not run inside another
# container: the AppImage's build calls it on the host.
#
# A new runtime: set RUNTIME_COMMIT and RUNTIME_SHA256 (the release's
# runtime-x86_64 for that commit, from
# https://github.com/AppImage/type2-runtime/releases), review what it links
# (packaging/appimage/licenses.tsv) and run this. The check also fails when
# Alpine has updated a package the runtime links since that release (the
# error lists the build's versions); upstream's next release has them.
set -euo pipefail

RUNTIME_COMMIT=fedfad51acc2860b2916a1eef32ba4a500ff31ef
RUNTIME_SHA256=617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46
RUNTIME_REPO=https://github.com/AppImage/type2-runtime

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHORT=${RUNTIME_COMMIT:0:7}
OUT="$REPO_ROOT/third_party/appimage-runtime/$SHORT"
SOURCE_ARCHIVE="$OUT/type2-runtime-$SHORT-source.tar.xz"
IMAGE="orbis-type2-runtime-build:$SHORT"

case "${1:-}" in
    --print-dir) echo "$OUT"; exit 0 ;;
    "") ;;
    -h|--help)
        echo "Usage: $(basename "$0") [--print-dir]"
        echo "Builds the AppImage runtime $SHORT into $OUT/ (reused when done)."
        echo "  --print-dir  print that folder and exit"
        exit 0
        ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
esac

if [[ -f "$SOURCE_ARCHIVE" && -f "$OUT/runtime-x86_64" ]] \
        && echo "$RUNTIME_SHA256  $OUT/runtime-x86_64" | sha256sum -c --status -; then
    echo "AppImage runtime $SHORT already built in $OUT"
    exit 0
fi
command -v podman > /dev/null || { echo "podman isn't installed" >&2; exit 1; }
# libfuse's install step creates /dev/fuse with mknod unless it exists, which
# a rootless build can't do; the host's device, passed in, lets it skip that.
[[ -c /dev/fuse ]] || { echo "/dev/fuse is missing (the runtime's build needs it)" >&2; exit 1; }

mkdir -p "$REPO_ROOT/tmp"
WORK=$(mktemp -d "$REPO_ROOT/tmp/appimage-runtime.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
TREE="$WORK/type2-runtime-$SHORT"
git init -q "$TREE"
git -C "$TREE" fetch -q --depth 1 "$RUNTIME_REPO" "$RUNTIME_COMMIT"
git -C "$TREE" checkout -q FETCH_HEAD
[[ "$(git -C "$TREE" rev-parse HEAD)" == "$RUNTIME_COMMIT" ]] \
    || { echo "fetched the wrong commit of $RUNTIME_REPO" >&2; exit 1; }
# What the runtime prints as its version; the release workflow writes the same.
echo -n "$RUNTIME_REPO/commit/$SHORT" > "$TREE/src/runtime/version"

# The tarballs upstream's install-dependencies.sh downloads and checks
# (libfuse, squashfuse), fetched here too, for the source archive.
DEPS="$TREE/scripts/common/install-dependencies.sh"
mapfile -t urls < <(sed -n 's/^wget "\{0,1\}\([^" ]*\)"\{0,1\}$/\1/p' "$DEPS")
mapfile -t sums < <(sed -n 's/^echo "\([0-9a-f]\{64\}\)  \([^"]*\)" | sha256sum -c -$/\1  \2/p' "$DEPS")
(( ${#urls[@]} > 0 && ${#urls[@]} == ${#sums[@]} )) \
    || { echo "can't read the downloads from $DEPS -- check it by hand" >&2; exit 1; }
mkdir "$WORK/deps"
for i in "${!urls[@]}"; do
    curl -fsSL --retry 3 -o "$WORK/deps/${sums[$i]#*  }" "${urls[$i]}"
done
(cd "$WORK/deps" && printf '%s\n' "${sums[@]}" | sha256sum -c --quiet -)

echo "== building the runtime in Alpine ($IMAGE)"
podman build --device /dev/fuse -t "$IMAGE" -f "$TREE/scripts/docker/Dockerfile" "$TREE" > "$WORK/image.log" 2>&1 \
    || { tail -30 "$WORK/image.log"; exit 1; }
mkdir "$WORK/out"
podman run --rm -w /ws -v "$TREE:/ws" -v "$WORK/out:/ws/out" "$IMAGE" \
    bash scripts/build-runtime.sh > "$WORK/build.log" 2>&1 \
    || { tail -30 "$WORK/build.log"; exit 1; }
podman run --rm "$IMAGE" sh -c 'echo "Alpine $(cat /etc/alpine-release)"; apk info -v 2> /dev/null | sort' \
    > "$WORK/packages.txt"

if ! echo "$RUNTIME_SHA256  $WORK/out/runtime-x86_64" | sha256sum -c --status -; then
    echo "The runtime differs from upstream's release binary for $SHORT (expected $RUNTIME_SHA256," >&2
    echo "got $(sha256sum < "$WORK/out/runtime-x86_64" | cut -d' ' -f1)). The build's Alpine packages:" >&2
    grep -E '^(Alpine|zstd|zlib|mimalloc|musl|clang)' "$WORK/packages.txt" >&2
    exit 1
fi
echo "runtime-x86_64: identical to upstream's release for $SHORT"

# The source archive: everything needed to rebuild this exact runtime.
SRC="$WORK/type2-runtime-$SHORT-source"
mkdir "$SRC"
git -C "$TREE" archive --prefix="type2-runtime-$SHORT/" HEAD | tar -x -C "$SRC"
cp "$TREE/src/runtime/version" "$SRC/type2-runtime-$SHORT/src/runtime/version"
cp "$WORK/deps"/* "$WORK/packages.txt" "$SRC/"
cat > "$SRC/README" << EOF
The AppImage runtime bundled in orbis-reframe's AppImage: AppImage/type2-runtime
at commit $RUNTIME_COMMIT
($RUNTIME_REPO), built unchanged. runtime-x86_64,
SHA-256 $RUNTIME_SHA256,
is the same file as that project's release binary for this commit.

$(printf '%-24s%s\n' "type2-runtime-$SHORT/" "the runtime's source (MIT), src/runtime/version as built")
$(cd "$WORK/deps" && for f in *; do printf '%-24s%s\n' "$f" "downloaded and built by install-dependencies.sh"; done)
$(printf '%-24s%s\n' packages.txt "the build container's Alpine packages; zstd, zlib,")
$(printf '%-24s%s\n' "" "mimalloc and musl are linked in from these")

It links libfuse (LGPL-2.1-only) statically; to rebuild it with a changed
libfuse, edit install-dependencies.sh or the patch in patches/libfuse/, then
in type2-runtime-$SHORT/:

  podman build --device /dev/fuse -t type2-runtime-build -f scripts/docker/Dockerfile .
  mkdir -p out && podman run --rm -w /ws -v "\$PWD:/ws" type2-runtime-build bash scripts/build-runtime.sh

To put out/runtime-x86_64 in front of the AppImage instead of this one, unpack
it (orbis-reframe-*.AppImage --appimage-extract) and pack squashfs-root again
with appimagetool --runtime-file out/runtime-x86_64.
EOF
mkdir -p "$OUT"
cp "$WORK/out/runtime-x86_64" "$OUT/"
tar -C "$WORK" --sort=name --owner=0 --group=0 --numeric-owner -cJf "$SOURCE_ARCHIVE" "$(basename "$SRC")"
echo "AppImage runtime $SHORT ready in $OUT"
