# Downloads from snapshot.debian.org for the AppImage scripts
# (package_appimage.sh, archive_appimage_sources.sh): sourced, not run.
#
# snapshot.debian.org is often overloaded and answers 503 / 504 for minutes
# at a time, so a failed download isn't evidence that a package version is
# missing. snapshot_fetch tells the two apart and retries the transient
# failures for a few minutes before giving up.

# snapshot_fetch URL OUT: download URL into the file OUT. Returns 0 on
# success, 1 when the server answers 404 (not there), 2 when it can't be
# reached or keeps failing (5xx, timeouts, a stalled transfer); says which
# on stderr for 2.
snapshot_fetch() {
    local url=$1 out=$2 code rc=0
    # No overall time limit (source tarballs can be large); a transfer that
    # stalls below 1 kB/s for a minute counts as a timeout and is retried.
    # curl retries timeouts and 408 / 429 / 500 / 502 / 503 / 504 itself.
    code=$(curl -sSL --retry 8 --retry-delay 15 --retry-connrefused \
        --connect-timeout 30 --speed-limit 1000 --speed-time 60 \
        -o "$out" -w '%{http_code}' "$url") || rc=$?
    if (( rc == 0 )) && [[ "$code" == 2* ]]; then
        return 0
    fi
    [[ "$code" == 404 ]] && return 1
    echo "snapshot.debian.org can't be reached for $url (HTTP ${code:-000}, curl exit $rc," \
         "after retrying for ~2 minutes)" >&2
    return 2
}
