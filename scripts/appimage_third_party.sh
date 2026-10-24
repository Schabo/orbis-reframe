#!/usr/bin/env bash
# The table of THIRD_PARTY.md's "Bundled in the AppImage" section, made from
# the licence review (packaging/appimage/licenses.tsv), so the two can't
# drift: the AppImage's build fails unless every bundled package has a row
# there, and runs this with --check. Versions are the upstream ones (a new
# Debian revision of a reviewed version changes neither file).
#
#   scripts/appimage_third_party.sh           print the table
#   scripts/appimage_third_party.sh --check   compare it with THIRD_PARTY.md's
#   scripts/appimage_third_party.sh --update  write it into THIRD_PARTY.md
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TSV="$REPO_ROOT/packaging/appimage/licenses.tsv"
DOC="$REPO_ROOT/THIRD_PARTY.md"
BEGIN='<!-- Generated from packaging/appimage/licenses.tsv by scripts/appimage_third_party.sh --update; the AppImage build checks it. -->'
END='<!-- End of the generated table. -->'

table() {
    echo "$BEGIN"
    echo
    echo "| Component | From | Version | License | Source |"
    echo "|---|---|---|---|---|"
    awk -F'\t' '
        /^#/ || NF == 0 || $1 == "package" { next }
        {
            v = $2
            sub(/^[0-9]+:/, "", v)          # epoch
            if (v ~ /-/) sub(/-[^-]*$/, "", v)  # Debian revision
            if ($1 == "ffmpeg") from = "our build (`scripts/build_ffmpeg.sh`), configure line in `licenses/ffmpeg/configure.txt`"
            else if ($1 == "ncnn") from = "Tencent'"'"'s release"
            else if ($1 == "type2-runtime") from = "our build (`scripts/build_appimage_runtime.sh`), the AppImage'"'"'s runtime"
            else from = "Debian 13"
            if ($5 == "snapshot") src = "snapshot.debian.org"
            else if ($5 == "hosted") src = "on the release"
            else src = "--"
            printf "| %s | %s | %s | %s | %s |\n", $1, from, v, $4, src
        }' "$TSV"
    echo
    echo "$END"
}

# THIRD_PARTY.md with the lines from BEGIN to END replaced by the table.
updated() {
    local t
    t=$(table)
    grep -qxF "$BEGIN" "$DOC" && grep -qxF "$END" "$DOC" \
        || { echo "THIRD_PARTY.md has no generated table (the two marker comments)" >&2; return 1; }
    awk -v begin="$BEGIN" -v end="$END" -v table="$t" '
        $0 == begin { print table; skip = 1; next }
        $0 == end { skip = 0; next }
        !skip' "$DOC"
}

case "${1:-}" in
    "") table ;;
    --check)
        new=$(updated)
        if [[ "$new" != "$(cat "$DOC")" ]]; then
            echo "THIRD_PARTY.md's AppImage table doesn't match packaging/appimage/licenses.tsv:" >&2
            diff <(cat "$DOC") <(echo "$new") >&2 || true
            echo "(scripts/appimage_third_party.sh --update writes it)" >&2
            exit 1
        fi
        ;;
    --update)
        new=$(updated)
        echo "$new" > "$DOC"
        ;;
    -h|--help) sed -n '2,10s/^# \{0,1\}//p' "$0" ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
esac
