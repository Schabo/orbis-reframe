#include "LensCalibrationJson.h"

namespace lenscal {

using json = nlohmann::json;

namespace {

constexpr int kVersion = 1;

json LensToJson(const insv::LensCalibExt& e)
{
    return json{
        {"xi", e.xi},
        {"fx", e.fx},
        {"fy", e.fy},
        {"cx", e.cx},
        {"cy", e.cy},
        {"yaw", e.yaw},
        {"pitch", e.pitch},
        {"roll", e.roll},
        {"t", {e.tx, e.ty, e.tz}},
        {"k", {e.k[0], e.k[1], e.k[2], e.k[3]}},
        {"p", {e.p[0], e.p[1]}},
        {"s", {e.s[0], e.s[1], e.s[2], e.s[3]}},
        {"frame", {e.w, e.h}},
        {"type", e.type},
        {"focalRefW", e.focalRefW},
    };
}

template <size_t N> bool Array(const json& j, const char* key, double (&out)[N])
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != N)
        return false;
    for (size_t i = 0; i < N; ++i) {
        if (!(*it)[i].is_number())
            return false;
        out[i] = (*it)[i].get<double>();
    }
    return true;
}

bool Number(const json& j, const char* key, double& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return false;
    out = it->get<double>();
    return true;
}

bool LensFromJson(const json& j, insv::LensCalibExt& e)
{
    if (!j.is_object())
        return false;
    double t[3], frame[2], type = 0;
    if (!Number(j, "xi", e.xi) || !Number(j, "fx", e.fx) || !Number(j, "fy", e.fy) ||
        !Number(j, "cx", e.cx) || !Number(j, "cy", e.cy) || !Number(j, "yaw", e.yaw) ||
        !Number(j, "pitch", e.pitch) || !Number(j, "roll", e.roll) || !Array(j, "t", t) ||
        !Array(j, "k", e.k) || !Array(j, "p", e.p) || !Array(j, "s", e.s) ||
        !Array(j, "frame", frame) || !Number(j, "type", type) ||
        !Number(j, "focalRefW", e.focalRefW))
        return false;
    e.tx = t[0];
    e.ty = t[1];
    e.tz = t[2];
    e.w = frame[0];
    e.h = frame[1];
    e.type = static_cast<int>(type);
    return e.fx > 0 && e.fy > 0 && e.w > 0 && e.focalRefW > 0;
}

} // namespace

const char* GuardName(int lensGuard)
{
    switch (lensGuard) {
    case 0: return "none";
    case 1: return "standard";
    case 2: return "premium";
    }
    return "unknown";
}

int GuardFromName(const std::string& name)
{
    if (name == "none")
        return 0;
    if (name == "standard")
        return 1;
    if (name == "premium")
        return 2;
    return -1;
}

json ToJson(const stitch::LensCalibration& c)
{
    return json{
        {"version", kVersion},
        {"serial", c.serial},
        {"lensGuard", GuardName(c.lensGuard)},
        {"created", c.created},
        {"squareMm", c.squareMm},
        {"fit",
         {{"rmsPx", {c.rmsPx[0], c.rmsPx[1]}},
          {"transferDeg", c.transferDeg},
          {"coverageDeg", {c.coverageDeg[0], c.coverageDeg[1]}}}},
        {"lenses", {LensToJson(c.lenses[0]), LensToJson(c.lenses[1])}},
        {"fileLenses", {LensToJson(c.fileLenses[0]), LensToJson(c.fileLenses[1])}},
    };
}

std::optional<stitch::LensCalibration> FromJson(const json& j)
{
    if (!j.is_object() || j.value("version", 0) != kVersion)
        return std::nullopt;
    stitch::LensCalibration c;
    c.serial = j.value("serial", std::string());
    c.lensGuard = GuardFromName(j.value("lensGuard", std::string()));
    c.created = j.value("created", std::string());
    if (c.serial.empty() || c.lensGuard < 0 || !Number(j, "squareMm", c.squareMm))
        return std::nullopt;
    const auto fit = j.find("fit");
    if (fit == j.end() || !fit->is_object() || !Array(*fit, "rmsPx", c.rmsPx) ||
        !Number(*fit, "transferDeg", c.transferDeg) || !Array(*fit, "coverageDeg", c.coverageDeg))
        return std::nullopt;
    for (const char* key : {"lenses", "fileLenses"}) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array() || it->size() != 2)
            return std::nullopt;
        auto& dst = std::string(key) == "lenses" ? c.lenses : c.fileLenses;
        for (int i = 0; i < 2; ++i)
            if (!LensFromJson((*it)[i], dst[i]))
                return std::nullopt;
    }
    return c;
}

} // namespace lenscal
