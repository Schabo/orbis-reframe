// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Ycbcr.h"

namespace insv {

void YcbcrToRgbRows(const YcbcrCoding& coding, int bitDepth, float rows[3][4])
{
    // Luma weights (ITU-R BT.601, BT.709, BT.2020).
    double kr = 0.2126, kb = 0.0722;
    if (coding.matrix == YcbcrMatrix::Bt601)
        kr = 0.299, kb = 0.114;
    else if (coding.matrix == YcbcrMatrix::Bt2020)
        kr = 0.2627, kb = 0.0593;
    const double kg = 1.0 - kr - kb;

    // Texel value -> code value at the stream's depth.
    const int n = bitDepth > 8 ? 10 : 8;
    const double toCode = n == 8 ? 255.0 : 65535.0 / 64.0;
    // Code value -> Y' 0..1 and Cb / Cr -0.5..0.5.
    const double maxCode = (1 << n) - 1;
    const double step = 1 << (n - 8);
    const double ySpan = coding.fullRange ? maxCode : 219.0 * step;
    const double yZero = coding.fullRange ? 0.0 : 16.0 * step;
    const double cSpan = coding.fullRange ? maxCode : 224.0 * step;
    const double cZero = 128.0 * step;
    const double ys = toCode / ySpan, yo = -yZero / ySpan;
    const double cs = toCode / cSpan, co = -cZero / cSpan;

    // R' = Y' + 2(1 - Kr) Cr, B' = Y' + 2(1 - Kb) Cb, G' from Y' = Kr R' + Kg G' + Kb B'.
    const double rCr = 2.0 * (1.0 - kr);
    const double bCb = 2.0 * (1.0 - kb);
    const double gCb = -bCb * kb / kg;
    const double gCr = -rCr * kr / kg;
    const double m[3][3] = {{1.0, 0.0, rCr}, {1.0, gCb, gCr}, {1.0, bCb, 0.0}};
    for (int i = 0; i < 3; ++i) {
        rows[i][0] = static_cast<float>(m[i][0] * ys);
        rows[i][1] = static_cast<float>(m[i][1] * cs);
        rows[i][2] = static_cast<float>(m[i][2] * cs);
        rows[i][3] = static_cast<float>(m[i][0] * yo + (m[i][1] + m[i][2]) * co);
    }
}

} // namespace insv
