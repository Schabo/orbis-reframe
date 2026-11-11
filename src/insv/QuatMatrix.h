// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

namespace insv {

// Row-major rotation matrix of the unit quaternion (w, x, y, z): v' = M v.
// A body -> world quaternion gives world = M body.
inline void QuatToMatrix(double w, double x, double y, double z, double m[3][3])
{
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - w * z);
    m[0][2] = 2 * (x * z + w * y);
    m[1][0] = 2 * (x * y + w * z);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - w * x);
    m[2][0] = 2 * (x * z - w * y);
    m[2][1] = 2 * (y * z + w * x);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

// The same for {w, x, y, z} in an array.
inline void QuatToMatrix(const double q[4], double m[3][3])
{
    QuatToMatrix(q[0], q[1], q[2], q[3], m);
}

} // namespace insv
