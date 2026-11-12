#!/usr/bin/env bash
# Get the AppImage's runtime (the static type-2 runtime, the program at the
# front of every AppImage that mounts the image and starts AppRun):
# AppImage/type2-runtime's release binary for a pinned commit, checked by its
# SHA-256 (RUNTIME_SHA256), and the source to host next to every AppImage --
# the runtime links libfuse (LGPL-2.1-only) statically.
#
# The binary is upstream's, not our build: upstream builds it in an Alpine
# container whose packages (zlib, zstd, musl, mimalloc, linked in) Alpine
# keeps updating, and it keeps no older ones, so a build from the same commit
# weeks later is a different file. When a commit is pinned, --verify builds it
# the way upstream's release workflow does and checks that the result is
# byte-identical to upstream's binary; that build's Alpine package list is
# kept in packaging/appimage/type2-runtime-<commit>-packages.txt and goes into
# the source archive, so the source we host is what's in the file.
#
# The binary comes from upstream's "continuous" release while that is still
# this commit; after upstream moves on, from the front of the first AppImage
# of ours that shipped it (RUNTIME_SHIPPED_IN), with the two sections
# appimagetool fills in (.digest_md5, .upd_info) blanked again.
#
# Output, under third_party/appimage-runtime/<commit>/ (gitignored):
#   runtime-x86_64                         the runtime, for appimagetool
#   type2-runtime-<commit>-source.tar.xz   to host on the release: the
#                                          runtime's tree at that commit,
#                                          libfuse's and squashfuse's
#                                          tarballs, the verified build's
#                                          Alpine packages, how to rebuild
#   licenses/                              the licence texts of the runtime
#                                          and what it links, for the
#                                          AppImage's licence folder
# A finished one is reused. Needs network access; --verify also needs podman
# and the host's /dev/fuse (see below), and is not run inside another
# container.
#
# A new runtime: set RUNTIME_COMMIT and RUNTIME_SHA256 (upstream's release
# runtime-x86_64 for that commit, from
# https://github.com/AppImage/type2-runtime/releases), empty RUNTIME_SHIPPED_IN,
# review what it links (packaging/appimage/licenses.tsv) and run --verify soon
# after upstream's release, while Alpine still has the packages it was built
# with (the error lists the build's versions); commit the package list it
# writes. Set RUNTIME_SHIPPED_IN to the first released AppImage with it.
set -euo pipefail

RUNTIME_COMMIT=fedfad51acc2860b2916a1eef32ba4a500ff31ef
RUNTIME_SHA256=617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46
RUNTIME_REPO=https://github.com/AppImage/type2-runtime
RUNTIME_SHIPPED_IN=https://github.com/Schabo/orbis-reframe/releases/download/v0.8.0/orbis-reframe-0.8.0-x86_64.AppImage

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHORT=${RUNTIME_COMMIT:0:7}
OUT="$REPO_ROOT/third_party/appimage-runtime/$SHORT"
SOURCE_ARCHIVE="$OUT/type2-runtime-$SHORT-source.tar.xz"
IMAGE="orbis-type2-runtime-build:$SHORT"
PACKAGES="$REPO_ROOT/packaging/appimage/type2-runtime-$SHORT-packages.txt"
VERIFY=0

case "${1:-}" in
    --print-dir) echo "$OUT"; exit 0 ;;
    --verify) VERIFY=1 ;;
    "") ;;
    -h|--help)
        echo "Usage: $(basename "$0") [--print-dir | --verify]"
        echo "Gets the AppImage runtime $SHORT and its source into $OUT/ (reused when done)."
        echo "  --print-dir  print that folder and exit"
        echo "  --verify     also build it from source in upstream's Alpine container, check"
        echo "               it is byte-identical and write $(basename "$PACKAGES")"
        exit 0
        ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
esac

# The licence texts of the runtime and of everything linked into it, from
# the source archive's tree and tarballs and, for the Alpine packages, from
# upstream at the version packages.txt lists. Into $OUT/licenses/.
collect_licenses() {
    local src=$1 lic="$OUT/licenses" tmp v
    tmp="$lic.tmp"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    cp "$src/type2-runtime-$SHORT/LICENSE" "$tmp/type2-runtime-$SHORT-LICENSE"
    # libfuse (LICENSE says which files are LGPL, LGPL2.txt is that
    # licence) and squashfuse, named by their tarballs' top folder.
    local t top f
    for t in "$src"/*.tar.*; do
        top=$(tar -tf "$t" | sed -n '1s,/.*,,p')
        for f in LICENSE LGPL2.txt; do
            tar -xf "$t" -O "$top/$f" > "$tmp/$top-$f" 2> /dev/null || rm -f "$tmp/$top-$f"
        done
        [[ -f "$tmp/$top-LICENSE" ]] || { echo "no LICENSE in $t" >&2; return 1; }
    done
    pkg_version() {
        sed -n "s/^$1-\([0-9][0-9.]*\)-r[0-9]*\$/\1/p" "$src/packages.txt"
    }
    local -A urls=(
        [zstd]="https://raw.githubusercontent.com/facebook/zstd/v%s/LICENSE"
        [zlib]="https://raw.githubusercontent.com/madler/zlib/v%s/LICENSE"
        [mimalloc2]="https://raw.githubusercontent.com/microsoft/mimalloc/v%s/LICENSE"
        [musl]="https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v%s"
    )
    for p in "${!urls[@]}"; do
        v=$(pkg_version "$p")
        [[ -n "$v" ]] || { echo "no $p in $src/packages.txt" >&2; return 1; }
        # shellcheck disable=SC2059
        curl -fsSL --retry 3 -o "$tmp/${p%2}-$v-LICENSE" "$(printf "${urls[$p]}" "$v")"
    done
    rm -rf "$lic"
    mv "$tmp" "$lic"
}

# upstream's runtime-x86_64 for $RUNTIME_COMMIT, into $1.
fetch_runtime() {
    local dst=$1 shoff entsize num size name off len
    if curl -fsSL --retry 3 -o "$dst" "$RUNTIME_REPO/releases/download/continuous/runtime-x86_64" \
            && echo "$RUNTIME_SHA256  $dst" | sha256sum -c --status -; then
        echo "runtime-x86_64: upstream's continuous release"
        return 0
    fi
    [[ -n "$RUNTIME_SHIPPED_IN" ]] || {
        echo "upstream's continuous release is no longer $SHORT and no AppImage of ours has it yet" >&2
        return 1
    }
    # The runtime is an ELF file whose section headers come last; the image
    # follows them.
    curl -fsSL --retry 3 -r 0-63 -o "$dst" "$RUNTIME_SHIPPED_IN"
    shoff=$(od -An -t u8 -j 40 -N 8 "$dst" | tr -d ' ')
    read -r entsize num < <(od -An -t u2 -j 58 -N 4 "$dst")
    size=$((shoff + entsize * num))
    curl -fsSL --retry 3 -r "0-$((size - 1))" -o "$dst" "$RUNTIME_SHIPPED_IN"
    while read -r name off len; do
        dd if=/dev/zero of="$dst" bs=1 seek=$((16#$off)) count=$((16#$len)) conv=notrunc status=none
    done < <(readelf -SW "$dst" | sed -n 's/.*\] \(\.digest_md5\|\.upd_info\) *[A-Z]* *[0-9a-f]* \([0-9a-f]*\) \([0-9a-f]*\).*/\1 \2 \3/p')
    echo "$RUNTIME_SHA256  $dst" | sha256sum -c --status - || {
        echo "the runtime in $RUNTIME_SHIPPED_IN isn't upstream's for $SHORT" >&2
        return 1
    }
    echo "runtime-x86_64: upstream's, from $(basename "$RUNTIME_SHIPPED_IN")"
}

if (( ! VERIFY )) && [[ -f "$SOURCE_ARCHIVE" && -f "$OUT/runtime-x86_64" ]] \
        && echo "$RUNTIME_SHA256  $OUT/runtime-x86_64" | sha256sum -c --status -; then
    if [[ ! -d "$OUT/licenses" ]]; then
        mkdir -p "$REPO_ROOT/tmp"
        WORK=$(mktemp -d "$REPO_ROOT/tmp/appimage-runtime.XXXXXX")
        trap 'rm -rf "$WORK"' EXIT
        tar -xJf "$SOURCE_ARCHIVE" -C "$WORK"
        collect_licenses "$WORK/type2-runtime-$SHORT-source"
    fi
    echo "AppImage runtime $SHORT already in $OUT"
    exit 0
fi
if (( VERIFY )); then
    command -v podman > /dev/null || { echo "podman isn't installed" >&2; exit 1; }
    # libfuse's install step creates /dev/fuse with mknod unless it exists, which
    # a rootless build can't do; the host's device, passed in, lets it skip that.
    [[ -c /dev/fuse ]] || { echo "/dev/fuse is missing (the runtime's build needs it)" >&2; exit 1; }
else
    [[ -f "$PACKAGES" ]] || { echo "$PACKAGES is missing: run $(basename "$0") --verify" >&2; exit 1; }
    command -v readelf > /dev/null || { echo "readelf (binutils) isn't installed" >&2; exit 1; }
fi

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

mkdir "$WORK/out"
fetch_runtime "$WORK/out/runtime-x86_64"

if (( VERIFY )); then
    echo "== building the runtime in Alpine ($IMAGE)"
    mkdir "$WORK/built"
    podman build --pull=always --no-cache --device /dev/fuse -t "$IMAGE" -f "$TREE/scripts/docker/Dockerfile" "$TREE" \
            > "$WORK/image.log" 2>&1 \
        || { tail -30 "$WORK/image.log"; exit 1; }
    podman run --rm -w /ws -v "$TREE:/ws" -v "$WORK/built:/ws/out" "$IMAGE" \
        bash scripts/build-runtime.sh > "$WORK/build.log" 2>&1 \
        || { tail -30 "$WORK/build.log"; exit 1; }
    podman run --rm "$IMAGE" sh -c 'echo "Alpine $(cat /etc/alpine-release)"; apk info -v 2> /dev/null | sort' \
        > "$WORK/packages.txt"
    if ! cmp -s "$WORK/built/runtime-x86_64" "$WORK/out/runtime-x86_64"; then
        echo "The build differs from upstream's release binary for $SHORT (expected $RUNTIME_SHA256," >&2
        echo "got $(sha256sum < "$WORK/built/runtime-x86_64" | cut -d' ' -f1)). The build's Alpine packages:" >&2
        grep -E '^(Alpine|zstd|zlib|mimalloc|musl|clang)' "$WORK/packages.txt" >&2
        exit 1
    fi
    cp "$WORK/packages.txt" "$PACKAGES"
    echo "runtime-x86_64: our build is identical to upstream's release for $SHORT;"
    echo "  its Alpine packages are in $PACKAGES"
fi

# The source archive: everything needed to rebuild this exact runtime.
SRC="$WORK/type2-runtime-$SHORT-source"
mkdir "$SRC"
git -C "$TREE" archive --prefix="type2-runtime-$SHORT/" HEAD | tar -x -C "$SRC"
cp "$TREE/src/runtime/version" "$SRC/type2-runtime-$SHORT/src/runtime/version"
cp "$WORK/deps"/* "$SRC/"
cp "$PACKAGES" "$SRC/packages.txt"
cat > "$SRC/README" << EOF
The AppImage runtime bundled in orbis-reframe's AppImage: AppImage/type2-runtime
at commit $RUNTIME_COMMIT
($RUNTIME_REPO), unchanged: runtime-x86_64,
SHA-256 $RUNTIME_SHA256,
is that project's release binary for this commit.
Built from this source in its Alpine container, with the Alpine packages in
packages.txt, it comes out byte-identical (checked when the commit was picked).

$(printf '%-24s%s\n' "type2-runtime-$SHORT/" "the runtime's source (MIT), src/runtime/version as built")
$(cd "$WORK/deps" && for f in *; do printf '%-24s%s\n' "$f" "downloaded and built by install-dependencies.sh"; done)
$(printf '%-24s%s\n' packages.txt "that build container's Alpine packages; zstd, zlib,")
$(printf '%-24s%s\n' "" "mimalloc and musl are linked in from these")

It links libfuse (LGPL-2.1-only) statically; to rebuild it with a changed
libfuse, edit install-dependencies.sh or the patch in patches/libfuse/, then
in type2-runtime-$SHORT/:

Alpine updates its packages and keeps no older ones, so a build today links
newer zlib, zstd, musl or mimalloc than packages.txt lists.

  podman build --device /dev/fuse -t type2-runtime-build -f scripts/docker/Dockerfile .
  mkdir -p out && podman run --rm -w /ws -v "\$PWD:/ws" type2-runtime-build bash scripts/build-runtime.sh

To put out/runtime-x86_64 in front of the AppImage instead of this one, unpack
it (orbis-reframe-*.AppImage --appimage-extract) and pack squashfs-root again
with appimagetool --runtime-file out/runtime-x86_64.
EOF
mkdir -p "$OUT"
cp "$WORK/out/runtime-x86_64" "$OUT/"
tar -C "$WORK" --sort=name --owner=0 --group=0 --numeric-owner -cJf "$SOURCE_ARCHIVE" "$(basename "$SRC")"
collect_licenses "$SRC"
echo "AppImage runtime $SHORT ready in $OUT"
