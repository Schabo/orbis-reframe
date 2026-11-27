// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/Image.h> // orbiscore::Image

#include <cstdint>
#include <vector>

// The GoPro MAX .360 picture: two HEVC streams, each a strip of three cube
// faces laid out SIDE | CENTER | SIDE, the faces stored equi-angularly -- the
// pixel is linear in angle, not in tan(angle) (docs/formats/gopro-max.md,
// *Geometry*, says how each fact was measured):
//   - stream 0 carries LEFT | FRONT | RIGHT, stream 1 DOWN | BACK | TOP, the
//     second turned so the strip runs on across the cube (BACK's right edge
//     meets TOP, its left edge DOWN);
//   - the centre face is EacLayout::centre px for 90 deg;
//   - a side region (EacLayout::side px) is a side face cut in half where the
//     two lenses meet, with EacLayout::overlap px between the halves: the
//     face spans 90 deg over side - overlap px, its inner half first. Both
//     halves run on into the overlap -- each lens's view past the boundary --
//     and the stitch cross-fades them across it.
// The numbers are the clip's (camera::ClipMetadata::cubeLayout: the file's
// own, or the reader's table), not derived here.
//
// Dependency-free (orbiscore lib).

namespace orbiscore {

// Where a strip's faces are, in pixels along it: SIDE | CENTRE | SIDE, so
// 2 * side + centre is the stream's width. 5.6K on the MAX: 1376 | 1344 |
// 1376 with a 64 px overlap.
struct EacLayout {
    int centre = 0;
    int side = 0;
    int overlap = 0;
    // A layout a `streamW` wide strip can hold, with an overlap narrower
    // than a side face's span (side - overlap).
    bool fits(int streamW) const
    {
        return centre > 0 && overlap > 0 && side - overlap > overlap &&
               2 * side + centre == streamW;
    }
    bool operator==(const EacLayout&) const = default;
};

class EacCubeStitcher {
public:
    // Map the faces of `streamW` x `streamH` strips laid out as `layout`
    // onto an outW x outH equirect. False on a layout the strip can't hold
    // (EacLayout::fits).
    bool configure(int streamW, int streamH, const EacLayout& layout, int outW, int outH);

    // Stitch the two streams (each configure()'s streamW x streamH; `a` is
    // stream 0) into `out` (allocated outW x outH). `owner` (may be null)
    // gets each pixel's face id (Face); `blend` false = each lens's half of
    // a side face cut hard at the lens boundary, no cross-fade.
    void renderEquirect(const Image& a, const Image& b, Image& out, int16_t* owner = nullptr,
                        bool blend = true) const;

    // The face ids of the owner map.
    enum Face { kLeft = 0, kRight = 1, kTop = 2, kFront = 3, kBack = 4, kDown = 5 };

    // How well the faces join: the median luminance jump across face
    // boundaries of the hard-cut stitch over the median of the busiest
    // internal gradients, at 512x256. About 1 where neighbouring faces
    // continue each other, several times that where the layout is wrong;
    // NaN for a layout the strips can't hold or a frame with too little
    // content.
    static double SeamRatio(const Image& a, const Image& b, const EacLayout& layout);

    int outWidth() const { return out_w_; }
    int outHeight() const { return out_h_; }
    // The layout configure() took, and a side face's span (side - overlap).
    int centreWidth() const { return center_; }
    int sideSpan() const { return span_; }
    int overlap() const { return overlap_; }

private:
    // One equirect pixel: the strip x of its sample and, inside a side
    // face's overlap, of the other lens's sample with the first's weight
    // (alpha 1 = no blend); the row; which stream; which face.
    struct Pixel {
        float x1, x2, y, alpha;
        uint8_t stream, face;
    };

    int stream_w_ = 0, stream_h_ = 0, out_w_ = 0, out_h_ = 0;
    int center_ = 0, side_ = 0, overlap_ = 0, span_ = 0;
    std::vector<Pixel> px_;
};

} // namespace orbiscore
