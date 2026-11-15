#include "ReaderUtil.h"

#include <algorithm>

namespace camera {

Thumbnail ThumbnailFromRgb(const orbiscore::Image& rgb) {
    Thumbnail t{rgb.w & ~1, rgb.h & ~1, false, {orbiscore::YcbcrMatrix::Bt601, false}, {}};
    t.nv12.resize(static_cast<size_t>(t.width) * t.height * 3 / 2);
    uint8_t* y = t.nv12.data();
    uint8_t* uv = y + static_cast<size_t>(t.width) * t.height;
    const auto px = [&](int x, int yy) { return &rgb.rgb[(static_cast<size_t>(yy) * rgb.w + x) * 3]; };
    for (int yy = 0; yy < t.height; ++yy)
        for (int x = 0; x < t.width; ++x) {
            const uint8_t* p = px(x, yy);
            y[static_cast<size_t>(yy) * t.width + x] =
                static_cast<uint8_t>(((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16);
        }
    for (int yy = 0; yy < t.height; yy += 2)
        for (int x = 0; x < t.width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = px(x + dx, yy + dy);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            r = (r + 2) / 4;
            g = (g + 2) / 4;
            b = (b + 2) / 4;
            uint8_t* o = uv + static_cast<size_t>(yy / 2) * t.width + x;
            o[0] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            o[1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    return t;
}

std::string LocalTimeFromDigits(const std::string& digits) {
    if (digits.size() != 14 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    const auto num = [&](int pos, int n) { return std::stoi(digits.substr(pos, n)); };
    const int month = num(4, 2), day = num(6, 2), hour = num(8, 2), minute = num(10, 2),
              second = num(12, 2);
    if (num(0, 4) < 2000 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 59)
        return {};
    return digits.substr(0, 4) + '-' + digits.substr(4, 2) + '-' + digits.substr(6, 2) + 'T' +
           digits.substr(8, 2) + ':' + digits.substr(10, 2) + ':' + digits.substr(12, 2);
}

} // namespace camera
