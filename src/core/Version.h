#pragma once

// The app's version: the project() version, which `cz bump` sets, plus git
// build metadata between releases -- "0.1.0" on a release tag,
// "0.1.0+12.gabc1234" twelve commits after it, ".dirty" with uncommitted
// changes (cmake/GitVersion.cmake).
namespace core::version {

const char* string();

} // namespace core::version
