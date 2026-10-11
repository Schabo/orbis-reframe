#!/usr/bin/env bash
# Download a GitHub release's files -- usually the draft the release workflow
# (.github/workflows/release.yml) made -- into dist/<target>/, laid out as
# scripts/package.sh leaves them, so scripts/release_test.sh tests exactly
# what will be published (docs/RELEASE.md). Needs the GitHub CLI, logged in
# (drafts are only visible to the repository's maintainers).
#
# Usage: scripts/fetch_release.sh vX.Y.Z
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=targets.sh
. "$REPO_ROOT/scripts/targets.sh"

tag=${1:-}
[[ $tag == v* ]] || { echo "Usage: $(basename "$0") vX.Y.Z" >&2; exit 1; }
version=${tag#v}

download=$(mktemp -d)
trap 'rm -rf "$download"' EXIT
gh release download "$tag" --dir "$download"

for target in "${ORBIS_TARGETS[@]}"; do
    # The .deb's file name per target (package.sh: its Version's '~' as '.').
    case $target in
        debian-testing) deb="orbis-reframe_${version}_amd64.deb" ;;
        debian-13) deb="orbis-reframe_${version}.deb13_amd64.deb" ;;
        ubuntu-26.04) deb="orbis-reframe_${version}.ubuntu26.04_amd64.deb" ;;
    esac
    dir="$REPO_ROOT/dist/$target"
    rm -rf "$dir"
    mkdir -p "$dir"
    for f in "$deb" "orbis-reframe-$version-$target-x86_64.tar.xz" "orbis-reframe-$version-source.tar.xz"; do
        cp "$download/$f" "$download/$f.sha256" "$dir/"
    done
    echo "== $target: $(ls "$dir" | tr '\n' ' ')"
done
