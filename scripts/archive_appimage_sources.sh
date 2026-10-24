#!/usr/bin/env bash
# Keep a copy of the Debian sources an AppImage release links to, in case
# snapshot.debian.org loses them while the release is still offered
# (docs/RELEASE.md). Reads a release's orbis-reframe-<version>-appimage-sources.txt
# and downloads each linked source package version from snapshot.debian.org
# into DIR/<package>/<version>/ (the .dsc and its tarballs), checked against
# the SHA-1 snapshot.debian.org files them under. A version already there is
# checked and kept, so each one is stored once however many releases link it.
#
#   scripts/archive_appimage_sources.sh SOURCES_TXT DIR
#
# Needs curl and jq. The sources this project hosts itself (FFmpeg, the
# AppImage runtime) are on the release and not fetched.
set -euo pipefail

die() { echo "$*" >&2; exit 1; }

case "${1:-}" in
    -h|--help) sed -n '2,13s/^# \{0,1\}//p' "$0"; exit 0 ;;
esac
[[ $# -eq 2 ]] || die "Usage: $(basename "$0") SOURCES_TXT DIR"
list=$1
dir=$2
[[ -f "$list" ]] || die "no $list"
command -v jq > /dev/null || die "jq isn't installed"

mapfile -t urls < <(grep -o 'https://snapshot\.debian\.org/package/[^ ]*/' "$list")
(( ${#urls[@]} > 0 )) || die "no snapshot.debian.org links in $list"
for url in "${urls[@]}"; do
    path=${url#https://snapshot.debian.org/package/}
    src=${path%%/*}
    ver=${path#*/}
    ver=${ver%/}
    dest="$dir/$src/${ver//%3A/:}"
    mkdir -p "$dest"
    # hash <TAB> file name, one per source file of that version.
    files=$(curl -fsS --retry 3 --max-time 120 "https://snapshot.debian.org/mr/package/$src/$ver/srcfiles?fileinfo=1" \
        | jq -r '.fileinfo | to_entries[] | "\(.key)\t\(.value[0].name)"') \
        || die "snapshot.debian.org has no $src $ver"
    [[ -n "$files" ]] || die "snapshot.debian.org lists no files for $src $ver"
    while IFS=$'\t' read -r sha1 name; do
        f="$dest/$name"
        if ! echo "$sha1  $f" | sha1sum -c --status - 2> /dev/null; then
            echo "Downloading $src $ver: $name"
            curl -fsSL --retry 3 -o "$f.part" "https://snapshot.debian.org/file/$sha1"
            mv "$f.part" "$f"
            echo "$sha1  $f" | sha1sum -c --quiet - || die "$f: not the file snapshot.debian.org lists"
        fi
    done <<< "$files"
    echo "$src $ver: $(wc -l <<< "$files") files in $dest"
done
echo "Archive: $(du -sh "$dir" | cut -f1) in $dir"
