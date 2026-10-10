#pragma once

#include <string>

// Where the app's data files are at run time. Installed --
// by the .deb (/usr/bin -> /usr/share/orbis-reframe) or an unpacked tarball
// (<dir>/bin -> <dir>/share/orbis-reframe) -- they sit at <exe
// dir>/../share/orbis-reframe, found from /proc/self/exe so the install can be
// anywhere and the binary can be reached through a symlink. Development builds,
// which run from build/, fall back to the source tree
// (ORBIS_SOURCE_TREE_FALLBACK, off in packaged builds). No Qt, no other deps.
namespace core::paths {

// <exe dir>/../share/orbis-reframe; empty if /proc/self/exe can't be read.
std::string installedDataDir();

// The folder of the bundled model `name` (resources/models/<name> in the
// source tree): the installed one if it exists, else the source tree's (dev
// builds), else the installed path anyway, so a "missing file" message names
// where it should be.
std::string modelDir(const std::string& name);

} // namespace core::paths
