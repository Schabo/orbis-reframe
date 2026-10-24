# Release targets (docs/RELEASE.md), sourced by scripts/package.sh and
# scripts/release_test.sh. A target is the distribution release the packages are
# built for and run on; each has its own build, tarball and .deb.
#
#   debian-testing   Debian Testing (no VERSION_ID in /etc/os-release)
#   debian-13        Debian 13 "trixie"
#   ubuntu-26.04     Ubuntu 26.04 LTS "Resolute Raccoon"
#
# cmake/Packaging.cmake works the target out the same way when configured
# without -DORBIS_PACKAGE_TARGET.
#
# The AppImage (scripts/package_appimage.sh) is a target of its own,
# `appimage`, outside ORBIS_TARGETS: one file for every distribution, built
# in Debian 13 with the libraries that differ between distributions bundled,
# and no .deb or tarball.

ORBIS_TARGETS=(debian-testing debian-13 ubuntu-26.04)

# The target of the system this runs on.
host_target() {
    (
        . /etc/os-release
        if [[ $ID == debian && ${VERSION_ID:-} =~ ^[0-9]+$ ]]; then
            echo "debian-$VERSION_ID"
        elif [[ $ID == ubuntu ]]; then
            echo "ubuntu-$VERSION_ID"
        else
            echo debian-testing
        fi
    )
}

# The container image a target builds and is tested in.
target_image() {
    case "$1" in
        debian-testing) echo docker.io/library/debian:testing ;;
        debian-13) echo docker.io/library/debian:trixie ;;
        ubuntu-26.04) echo docker.io/library/ubuntu:26.04 ;;
        appimage) echo docker.io/library/debian:trixie ;;
        *) return 1 ;;
    esac
}

# How docs/manual/getting-started.md names it (the tarball's apt line).
target_label() {
    case "$1" in
        debian-testing) echo "Debian Testing" ;;
        debian-13) echo "Debian 13" ;;
        ubuntu-26.04) echo "Ubuntu 26.04" ;;
        *) return 1 ;;
    esac
}
