// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Calibration.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace insv {

namespace {

// Tokenise a run of numbers separated by '_' or whitespace, starting at `from`.
// Stops at the first byte that can't extend a number or separator.
std::vector<double> tokenizeNumbers(const std::string& s, size_t from)
{
    std::vector<double> t;
    const char* base = s.c_str();
    const char* p = base + from;
    while (*p) {
        while (*p == '_' || *p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p)
            break;
        t.push_back(v);
        p = end;
    }
    return t;
}

} // namespace

bool ParseExtendedCalibrationString(const std::string& s, InsvCalibration& out)
{
    // Find a "2_<digit>" / "2 <digit>" start so a caller can hand us a longer
    // buffer with the string embedded.
    size_t start = std::string::npos;
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != '2')
            continue;
        if ((s[i + 1] == '_' || s[i + 1] == ' ') && (s[i + 2] >= '0' && s[i + 2] <= '9')) {
            if (i == 0 || !((s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '.')) {
                start = i;
                break;
            }
        }
    }
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
    // Sanity: both mirror params must be in a believable range (X5 ~2.0), and
    // the combined frame the principal points live in must be given.
    for (int i = 0; i < 2; ++i)
        if (!(lens[i].xi > 0.5 && lens[i].xi < 4.0) || !(lens[i].fx > 1.0) ||
            !(lens[i].fy > 1.0) || !(lens[i].w > 1.0))
            return false;

    out.lensesExt.assign(lens, lens + 2);
    return true;
}

} // namespace insv
