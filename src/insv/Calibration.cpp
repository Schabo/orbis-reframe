// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Calibration.h"

#include <charconv>
#include <string>
#include <vector>

namespace insv {

namespace {

// Tokenise a run of numbers separated by '_' or whitespace, starting at `from`.
// Stops at the first byte that can't extend a number or separator.
// from_chars, not strtod: strtod follows the host's LC_NUMERIC, and with a
// decimal comma it stops at the file's '.'.
std::vector<double> tokenizeNumbers(const std::string& s, size_t from)
{
    std::vector<double> t;
    const char* p = s.data() + from;
    const char* const last = s.data() + s.size();
    while (p < last) {
        while (p < last && (*p == '_' || *p == ' ' || *p == '\t'))
            ++p;
        if (p < last && *p == '+')
            ++p;
        double v = 0;
        const auto r = std::from_chars(p, last, v);
        if (r.ec != std::errc())
            break;
        t.push_back(v);
        p = r.ptr;
    }
    return t;
}

// A "2_<digit>" / "2 <digit>" start, so a caller can hand over a longer
// buffer with the string embedded; npos if there's none.
size_t findCalibrationStart(const std::string& s)
{
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != '2')
            continue;
        if ((s[i + 1] == '_' || s[i + 1] == ' ') && (s[i + 2] >= '0' && s[i + 2] <= '9')) {
            if (i == 0 || !((s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '.'))
                return i;
        }
    }
    return std::string::npos;
}

// Both mirror params in a believable range (X5 ~2.0, ONE X2 ~1.7), and the
// combined frame the principal points live in given.
bool sane(const LensCalibExt (&lens)[2])
{
    for (int i = 0; i < 2; ++i)
        if (!(lens[i].xi > 0.5 && lens[i].xi < 4.0) || !(lens[i].fx > 1.0) ||
            !(lens[i].fy > 1.0) || !(lens[i].w > 1.0))
            return false;
    return true;
}

} // namespace

bool ParseExtendedCalibrationString(const std::string& s, InsvCalibration& out)
{
    const size_t start = findCalibrationStart(s);
    if (start == std::string::npos)
        return false;

    const std::vector<double> t = tokenizeNumbers(s, start);
    constexpr int kPerLens = 27;
    // 1 leading count + 2 lenses * 27; a trailing token may or may not be present.
    if (static_cast<int>(t.size()) < 1 + 2 * kPerLens)
        return false;
    if (static_cast<int>(t[0]) != 2)
        return false;

    LensCalibExt lens[2];
    for (int i = 0; i < 2; ++i) {
        const double* b = &t[1 + i * kPerLens];
        LensCalibExt& L = lens[i];
        L.xi = b[0];
        L.fx = b[1];
        L.fy = b[2];
        L.cx = b[3];
        L.cy = b[4];
        L.yaw = b[5];
        L.pitch = b[6];
        L.roll = b[7];
        L.tx = b[8];
        L.ty = b[9];
        L.tz = b[10];
        L.k[0] = b[11];
        L.k[1] = b[12];
        L.k[2] = b[13];
        L.k[3] = b[14];
        // b[15] reserved
        L.p[0] = b[16];
        L.p[1] = b[17];
        L.s[0] = b[18];
        L.s[1] = b[19];
        L.s[2] = b[20];
        L.s[3] = b[21];
        // b[22], b[23] reserved
        L.w = b[24];
        L.h = b[25];
        L.type = static_cast<int>(b[26]);
    }
    if (!sane(lens))
        return false;
    out.lensesExt.assign(lens, lens + 2);
    return true;
}

bool ParseCalibrationV3String(const std::string& s, InsvCalibration& out)
{
    const size_t start = findCalibrationStart(s);
    if (start == std::string::npos)
        return false;

    const std::vector<double> t = tokenizeNumbers(s, start);
    constexpr int kPerLens = 19;
    if (static_cast<int>(t.size()) < 1 + 2 * kPerLens)
        return false;
    if (static_cast<int>(t[0]) != 2)
        return false;

    LensCalibExt lens[2];
    for (int i = 0; i < 2; ++i) {
        const double* b = &t[1 + i * kPerLens];
        LensCalibExt& L = lens[i];
        L.xi = b[0];
        L.fx = b[1];
        L.fy = b[2];
        L.cx = b[3];
        L.cy = b[4];
        L.yaw = b[5];
        L.pitch = b[6];
        L.roll = b[7];
        L.tx = b[8];
        L.ty = b[9];
        L.tz = b[10];
        L.k[0] = b[11];
        L.k[1] = b[12];
        L.k[2] = b[13];
        L.p[0] = b[14];
        L.p[1] = b[15];
        L.w = b[16];
        L.h = b[17];
        L.type = static_cast<int>(b[18]);
    }
    if (!sane(lens))
        return false;
    out.lensesExt.assign(lens, lens + 2);
    return true;
}

} // namespace insv
