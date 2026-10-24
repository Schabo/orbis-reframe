#!/usr/bin/env bash
# Is an AppImage's bundle still current? Reads its bundled.txt
# (usr/share/doc/orbis-reframe/bundled.txt inside the AppImage, written by
# scripts/package_appimage.sh) and fails when
#   - Debian 13's security archive has a newer version of a bundled binary
#     package, or
#   - FFmpeg has a newer point release of the bundled series (9.0.x).
# Newer versions that aren't security updates (Debian's point releases, a
# new FFmpeg series) are listed but don't fail it. A failure means a patch
# release (docs/RELEASE.md). The weekly workflow
# .github/workflows/appimage-updates.yml runs it on the latest release.
#
#   scripts/check_appimage_updates.sh BUNDLED_TXT
#
# Runs on Debian 13 after `apt-get update` (debian:trixie has the security
# archive in its sources); needs curl for FFmpeg's release list.
set -euo pipefail

die() { echo "$*" >&2; exit 1; }

case "${1:-}" in
    -h|--help) sed -n '2,16s/^# \{0,1\}//p' "$0"; exit 0 ;;
esac
[[ $# -eq 1 && -f "$1" ]] || die "Usage: $(basename "$0") BUNDLED_TXT"
bundled=$1
# shellcheck source=/dev/null
[[ "$(. /etc/os-release && echo "$ID-${VERSION_ID:-}")" == debian-13 ]] \
    || die "Runs on Debian 13, which the AppImage is built from (e.g. in a debian:trixie container)"

fail=0
report() { echo "$*"; [[ -z "${GITHUB_STEP_SUMMARY:-}" ]] || echo "- $*" >> "$GITHUB_STEP_SUMMARY"; }

# Debian: each binary package once, with the version bundled.
while IFS=$'\t' read -r bin have; do
    newer=$(apt-cache madison "$bin" | awk -F' *[|] *' '{print $2 "\t" $3}' \
        | while IFS=$'\t' read -r v repo; do
            if dpkg --compare-versions "$v" gt "$have"; then printf '%s\t%s\n' "$v" "$repo"; fi
        done)
    [[ -n "$(apt-cache madison "$bin")" ]] || { report "UNKNOWN: $bin isn't in Debian 13 any more"; fail=1; continue; }
    [[ -n "$newer" ]] || continue
    security=$(grep -- '-security' <<< "$newer" | cut -f1 | sort -V | tail -1 || true)
    if [[ -n "$security" ]]; then
        report "SECURITY: $bin $have -> $security"
        fail=1
    else
        report "update (not a security one): $bin $have -> $(cut -f1 <<< "$newer" | sort -V | tail -1)"
    fi
done < <(grep -v '^#' "$bundled" | awk -F'\t' '$2 != "-" {print $2 "\t" $5}' | sort -u)

# FFmpeg: the newest release of the bundled major.minor series.
ffmpeg=$(grep -v '^#' "$bundled" | awk -F'\t' '$3 == "ffmpeg" {print $4; exit}')
if [[ -n "$ffmpeg" ]]; then
    releases=$(curl -fsSL --retry 3 https://ffmpeg.org/releases/ | grep -o 'ffmpeg-[0-9][0-9.]*[0-9]\.tar\.xz' \
        | sed 's/^ffmpeg-//; s/\.tar\.xz$//' | sort -uV) || die "can't read https://ffmpeg.org/releases/"
    series=$(cut -d. -f1-2 <<< "$ffmpeg")
    latest=$(grep -E "^${series//./\\.}(\.|$)" <<< "$releases" | tail -1 || true)
    if [[ -n "$latest" && "$latest" != "$ffmpeg" ]] && [[ "$(sort -V <<< "$latest"$'\n'"$ffmpeg" | tail -1)" == "$latest" ]]; then
        report "FFMPEG: $ffmpeg -> $latest"
        fail=1
    fi
    newest=$(tail -1 <<< "$releases")
    [[ "$(sort -V <<< "$newest"$'\n'"$latest" | tail -1)" == "$latest" ]] \
        || report "newer FFmpeg series (not a fix release): $newest"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "== the AppImage needs a new release" >&2
    exit 1
fi
echo "== the AppImage's bundle is current"
