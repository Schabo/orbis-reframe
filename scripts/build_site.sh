#!/usr/bin/env bash
# Builds the project site (GitHub Pages, .github/workflows/pages.yml) into
# one folder: the project page in site/, the user manual and the camera
# format documents rendered from their Markdown (scripts/site/), a sitemap,
# then checks every internal link.
#
#   scripts/build_site.sh [TAG] [OUT]
#
# TAG: the release whose manual goes up (v0.9.1); the published site
# always shows the newest release's, so it matches what people download.
# Without it, the manual in the working tree (a preview). The formats come
# from the working tree either way: they describe the cameras' files, not
# the app. OUT: default tmp/site. Look at it with
#   python3 -m http.server -d tmp/site
#
# Needs pandoc and python3.
set -euo pipefail

cd "$(dirname "$0")/.."
tag="${1:-}"
out="${2:-tmp/site}"

command -v pandoc >/dev/null || { echo "build_site.sh: pandoc is needed (apt install pandoc)" >&2; exit 1; }

manual=docs/manual
ref=main
if [[ -n "$tag" ]]; then
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    git archive "$tag" docs/manual | tar -x -C "$work"
    manual="$work/docs/manual"
    ref="$tag"
fi

python3 scripts/site/build.py --site site --manual "$manual" --manual-ref "$ref" \
    --formats docs/formats --out "$out"
python3 scripts/site/check_links.py "$out"
