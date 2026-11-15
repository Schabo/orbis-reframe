// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace insv {

// `x` on an even grid of `stepSec`, through a Gaussian of `sigmaSec`
// centred on each sample (zero phase: a change shows when it happens, not
// later). NaN samples are left out of their neighbours and stay NaN.
template <typename T>
std::vector<T> GaussianSmooth(const std::vector<T>& x, double stepSec, double sigmaSec)
{
    const long r = static_cast<long>(3.0 * sigmaSec / stepSec);
    if (r < 1)
        return x;
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    for (long i = -r; i <= r; ++i) {
        const double u = static_cast<double>(i) * stepSec / sigmaSec;
        k[static_cast<size_t>(i + r)] = std::exp(-0.5 * u * u);
    }
    const long n = static_cast<long>(x.size());
    std::vector<T> y(x.size());
    for (long j = 0; j < n; ++j) {
        if (std::isnan(x[static_cast<size_t>(j)])) {
            y[static_cast<size_t>(j)] = x[static_cast<size_t>(j)];
            continue;
        }
        double sum = 0.0, weight = 0.0;
        for (long i = std::max(-r, -j); i <= r && j + i < n; ++i) {
            const T v = x[static_cast<size_t>(j + i)];
            if (std::isnan(v))
                continue;
            sum += k[static_cast<size_t>(i + r)] * v;
            weight += k[static_cast<size_t>(i + r)];
        }
        y[static_cast<size_t>(j)] = static_cast<T>(sum / weight);
    }
    return y;
}

} // namespace insv
