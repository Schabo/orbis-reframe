#include "DataPaths.h"

#include <filesystem>
#include <system_error>

namespace core::paths {

namespace fs = std::filesystem;

std::string installedDataDir()
{
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty())
        return {};
    return (exe.parent_path().parent_path() / "share" / "orbis-reframe").string();
}

std::string modelDir(const std::string& name)
{
    std::error_code ec;
    const std::string data = installedDataDir();
    const fs::path installed = data.empty() ? fs::path() : fs::path(data) / "models" / name;
    if (!installed.empty() && fs::is_directory(installed, ec))
        return installed.string();
#ifdef ORBIS_SOURCE_MODELS_DIR
    const fs::path source = fs::path(ORBIS_SOURCE_MODELS_DIR) / name;
    if (fs::is_directory(source, ec))
        return source.string();
#endif
    return installed.string();
}

std::string lutsDir()
{
    std::error_code ec;
    const std::string data = installedDataDir();
    const fs::path installed = data.empty() ? fs::path() : fs::path(data) / "luts";
    if (!installed.empty() && fs::is_directory(installed, ec))
        return installed.string();
#ifdef ORBIS_SOURCE_LUTS_DIR
    if (fs::is_directory(ORBIS_SOURCE_LUTS_DIR, ec))
        return ORBIS_SOURCE_LUTS_DIR;
#endif
    return installed.string();
}

} // namespace core::paths
