// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// How a lens stream's Y'CbCr samples turn into R'G'B': the matrix and range
// the stream is coded with (its VUI; LensDemuxer reads them). Shared by the
// CPU decode's swscale set-up, the GPU stitch's ycbcr conversion and the
// viewport's own conversion, so all three agree. Dependency-free.

namespace insv {

// The Y'CbCr -> R'G'B' matrix.
enum class YcbcrMatrix { Bt601, Bt709, Bt2020 };

struct YcbcrCoding {
    YcbcrMatrix matrix = YcbcrMatrix::Bt709;
    bool fullRange = false; // "pc" / JPEG range; false = limited ("tv")
};

// The affine map from a 2-plane picture's sampled values to R'G'B' (0..1,
// unclamped): rgb[i] = rows[i][0] * y + rows[i][1] * cb + rows[i][2] * cr +
// rows[i][3], where y, cb, cr are the planes' UNORM texel values -- 8-bit
// planes as stored, 10-bit ones P010-style in the top bits of 16. The same
// arithmetic as a Vulkan ycbcr conversion (range expansion, then the model).
void YcbcrToRgbRows(const YcbcrCoding& coding, int bitDepth, float rows[3][4]);

} // namespace insv
