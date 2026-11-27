// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <orbis-core/Image.h> // orbiscore::Image

#include <cstdint>
#include <vector>

// The GoPro MAX .360 picture: two HEVC streams, each a strip of three cube
// faces laid out SIDE | CENTER | SIDE, the faces stored equi-angularly -- the
// pixel is linear in angle, not in tan(angle) (docs/formats/gopro-max.md).
//
// The layout, measured on GS012267.360 against FFmpeg 9.1's own stitch
// (v360_vulkan input=gopromax), every face's picture correlating at
// NCC >= 0.9994 with it (go_pro_2 tools/eac_layout_fit.py):
//   - stream 0 carries LEFT | FRONT | RIGHT, stream 1 DOWN | BACK | TOP, the
//     second turned so the strip runs on across the cube (BACK's right edge
//     meets TOP, its left edge DOWN);
//   - the centre face is the stream's height square (1344);
//   - a side face spans 90 deg over side - overlap px (1312), cut in half
//     where the two lenses meet, with the overlap (64 px) between the halves:
//     inner half at 0..656, outer half at 720..1376 of the 1376 px region.
//     Both halves run on into the overlap -- each lens's view past the
//     boundary -- and the stitch cross-fades them across it.
// For a 4096x1344 strip: centre = H, side = (W - H) / 2, overlap =
// 2 * (side - centre), a side face's span = side - overlap. Other stream
// sizes take the same relations; only 5.6K has been measured.
//
// Dependency-free (orbiscore lib).

namespace orbiscore {

class EacCubeStitcher {
public:
    // Map the faces of a `streamW` x `streamH` strip onto an outW x outH
    // equirect. False on a strip that isn't a 3-face belt with an overlap
    // (needs a side region wider than the centre face, and the overlap
    // narrower than a side face).
    bool configure(int streamW, int streamH, int outW, int outH);

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
    // internal gradients, at 512x256 (tools/seam_report.py's measure). About
    // 1 where neighbouring faces continue each other, several times that
    // where the layout is wrong; NaN for a strip that isn't a belt or a
    // frame with too little content.
    static double SeamRatio(const Image& a, const Image& b);

    int outWidth() const { return out_w_; }
    int outHeight() const { return out_h_; }
    // The layout's numbers as configure() derived them (see the header).
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
