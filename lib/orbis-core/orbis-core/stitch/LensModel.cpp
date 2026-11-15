// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/stitch/LensModel.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbiscore {

double LensMask::clearRadAt(double azRad) const
{
    const int n = static_cast<int>(clearDeg.size());
    double a = azRad / (2.0 * std::numbers::pi);
    a -= std::floor(a);
    const int k0 = static_cast<int>(a * n) % n, k1 = (k0 + 1) % n;
    double r = 1e9;
    for (const int k : {k0, k1})
        if (clearDeg[k] > 0.0)
            r = std::min(r, clearDeg[k] * (std::numbers::pi / 180.0));
    return r;
}

} // namespace orbiscore
