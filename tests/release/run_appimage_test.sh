#!/bin/bash
# Inside an AppImage GUI test container (tests/release/Containerfile or
# Containerfile.<distro>), started by scripts/release_test.sh: the AppImage
# at /dist/, nothing installed for it. Writes to /work: install.txt (the
# distribution, its glibc, the libraries ldd can't find for the program,
# --version), then the smoke test's results (gpu_smoke.sh --sound, judged on
# the host as for the packages). A container has no FUSE, so the AppImage
# runs with APPIMAGE_EXTRACT_AND_RUN; the same AppRun and libraries start.
set -uo pipefail

W=/work
I=$W/install.txt
: > "$I"
result() { echo "$1=$2" | tee -a "$I"; }

app=$(echo /dist/orbis-reframe-*-x86_64.AppImage)
export APPIMAGE_EXTRACT_AND_RUN=1
# shellcheck source=/dev/null
result distribution "$(. /etc/os-release && echo "$PRETTY_NAME")"
result glibc "$(ldd --version | head -1 | grep -o '[0-9.]*$')"
version=$("$app" --version 2>&1)
result version "${version%%$'\n'*}"

/opt/smoke/gpu_smoke.sh --sound --no-check "$app" "$W"
