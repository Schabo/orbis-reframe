#!/bin/bash
# In a bare image of a distribution, started by scripts/release_test.sh for
# the AppImage: install only the libraries the AppImage takes from the
# system (docs/dev-environment.md, "The AppImage"), leaving out glvnd's
# libOpenGL / libGLX where the distribution packages them apart, so the
# bundled fallback is used; then every bundled library resolves and
# --version runs. The AppImage is /dist/*.AppImage. Prints name=value lines:
#   install    the package manager's exit status
#   missing    libraries not found, as "file: soname" (empty when none)
#   fallback   the sonames AppRun takes from the bundled glvnd fallback
#   elf_files  how many ELF files were checked
#   version    what --version printed
set -uo pipefail

# shellcheck source=/dev/null
. /etc/os-release
case $ID in
    fedora)
        dnf -y -q install --setopt=install_weak_deps=False libglvnd-egl libX11 libdrm fontconfig \
            freetype harfbuzz vulkan-loader libwayland-client > /tmp/install.log 2>&1 ;;
    arch)
        # One package holds all of glvnd here (and on Tumbleweed).
        pacman -Sy --noconfirm --needed libglvnd libx11 libdrm fontconfig freetype2 harfbuzz \
            vulkan-icd-loader wayland > /tmp/install.log 2>&1 ;;
    opensuse-tumbleweed)
        zypper -n -q install --no-recommends libglvnd libX11-6 libdrm2 libfontconfig1 libfreetype6 \
            libharfbuzz0 libvulkan1 libwayland-client0 > /tmp/install.log 2>&1 ;;
    *) echo "install=unknown distribution $ID"; exit 1 ;;
esac
echo "install=$?"

app=$(echo /dist/orbis-reframe-*-x86_64.AppImage)
cd /tmp && "$app" --appimage-extract > /dev/null
root=/tmp/squashfs-root

# Only bash and ldd from here on: a bare image may lack find, awk, even tr.
# What AppRun would add: its own logic, with the program's start replaced by
# printing the library path.
while IFS= read -r line; do
    [[ $line == exec\ * ]] && line='echo "$LD_LIBRARY_PATH"; exit 0'
    printf '%s\n' "$line"
done < "$root/AppRun" > "$root/AppRun.paths"
chmod +x "$root/AppRun.paths"
paths=$(env -u LD_LIBRARY_PATH "$root/AppRun.paths")
fallback=()
IFS=: read -ra dirs <<< "$paths"
for d in "${dirs[@]}"; do [[ -n $d ]] && fallback+=("${d##*/}"); done
echo "fallback=${fallback[*]}"

shopt -s globstar
elves=0
missing=()
for f in "$root"/usr/**; do
    [[ -f "$f" && ! -L "$f" && "$f" != */glvnd-fallback/* ]] || continue
    [[ "$(head -c 4 "$f")" == $'\x7fELF' ]] || continue
    elves=$((elves + 1))
    while IFS= read -r line; do
        [[ $line == *"not found"* ]] || continue
        line=${line%% =>*}
        missing+=("${f#"$root"/}: ${line//[[:space:]]/}")
    done < <(LD_LIBRARY_PATH=$paths ldd "$f" 2>&1)
done
# How many files were looked at, so a check that saw none can't pass.
echo "elf_files=$elves"
echo "missing=${missing[*]}"
version=$(APPIMAGE_EXTRACT_AND_RUN=1 "$app" --version 2>&1)
echo "version=${version%%$'\n'*}"
