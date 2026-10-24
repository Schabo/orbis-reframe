# The release packages' checks on an installed tree, sourced by
# scripts/package.sh (the unpacked tarball) and scripts/package_appimage.sh
# (the unpacked AppImage's usr/). Both hold the install layout of
# docs/dev-environment.md under the folder given.

# check_install_tree DIR: no build-machine paths, the models, the LUT, the
# licence texts, the desktop entry and the file types. Prints a FAIL line
# for each problem; returns 1 if there was one. Needs REPO_ROOT and HOME.
check_install_tree() {
    local top=$1 bad=0 needle hits m f glob mime_db

    for needle in "$REPO_ROOT" "$HOME"; do
        hits=$(grep -rlF "$needle" "$top" || true)
        [[ -z "$hits" ]] || { echo "FAIL: '$needle' found in:" >&2; echo "$hits" >&2; bad=1; }
    done

    for m in deepfilternet3/enc.onnx vittrack/vittrack.onnx \
             plates/yolo_v9_s_608_license_plates_heads.ncnn.param; do
        [[ -f "$top/share/orbis-reframe/models/$m" ]] || { echo "FAIL: model $m missing" >&2; bad=1; }
    done
    [[ -f "$top/share/orbis-reframe/luts/HLG to Rec.709.cube" ]] \
        || { echo "FAIL: the HLG to Rec.709 LUT is missing" >&2; bad=1; }

    # License texts (THIRD_PARTY.md).
    for f in doc/orbis-reframe/LICENSE doc/orbis-reframe/THIRD_PARTY.md \
             doc/orbis-reframe/LICENSE.lucide doc/orbis-reframe/licenses/ncnn-LICENSE.txt \
             doc/orbis-reframe/licenses/nlohmann-json-LICENSE.MIT doc/orbis-reframe/licenses/miniaudio-LICENSE \
             doc/orbis-reframe/licenses/insv-LICENSE-MIT doc/orbis-reframe/licenses/insv-LICENSE-APACHE \
             orbis-reframe/models/deepfilternet3/LICENSE-MIT orbis-reframe/models/deepfilternet3/LICENSE-APACHE \
             orbis-reframe/models/vittrack/LICENSE-APACHE orbis-reframe/models/plates/LICENSE-MIT; do
        [[ -f "$top/share/$f" ]] || { echo "FAIL: license file share/$f missing" >&2; bad=1; }
    done

    desktop-file-validate "$top/share/applications/orbis-reframe.desktop" || bad=1
    # The file types compile, and give .orp and .insv the types the desktop
    # entry names.
    mime_db=$(mktemp -d)
    mkdir "$mime_db/packages"
    cp "$top/share/mime/packages/orbis-reframe.xml" "$mime_db/packages/" 2>/dev/null \
        || { echo "FAIL: share/mime/packages/orbis-reframe.xml missing" >&2; bad=1; }
    if update-mime-database "$mime_db" 2>/dev/null; then
        for glob in application/x-orbis-reframe-project:\*.orp video/x-insta360-insv:\*.insv; do
            cut -d: -f2- "$mime_db/globs2" | grep -qxF "$glob" \
                || { echo "FAIL: no $glob in the file types" >&2; bad=1; }
            grep -q "^MimeType=.*${glob%%:*};" "$top/share/applications/orbis-reframe.desktop" \
                || { echo "FAIL: ${glob%%:*} not in the desktop entry's MimeType" >&2; bad=1; }
        done
    else
        echo "FAIL: the file types don't compile" >&2; bad=1
    fi
    rm -rf "$mime_db"

    return "$bad"
}
