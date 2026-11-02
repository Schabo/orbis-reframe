#pragma once

// A camera's own lens calibration, measured with the chart (src/calib/) in
// place of the one its files carry: the same MEI model, in the file's units,
// for one camera (serial) and one lens-guard setting. The stitch takes its
// lens parameters instead of the file's (stitch::ApplyLensCalibration); the
// guard is in them, so no guard profile goes on top.
//
// Header-only and plain data: the project (src/core) keeps a copy per clip.

#include <Calibration.h> // insv::LensCalibExt

#include <array>
#include <cmath>
#include <string>

namespace stitch {

struct LensCalibration {
    // Per lens, the fitted model: xi, fx, fy, cx, cy, k, p, s and the
    // rotation (yaw, pitch, roll), in the file's units. The rest of each
    // LensCalibExt (frame size, focal window, lens offset) is the file's.
    std::array<insv::LensCalibExt, 2> lenses;
    // The file calibration it was measured against. It applies to a clip
    // whose file carries the same one (the same camera, the same
    // calibration string), whatever the capture mode.
    std::array<insv::LensCalibExt, 2> fileLenses;

    std::string serial;    // the camera's
    int lensGuard = 0;     // stitch::PreviewStitchConfig::LensGuard as int
    std::string created;   // ISO 8601, UTC: what the user sees it by
    double squareMm = 0;   // the chart's square, as measured
    // The fit's figures, for the result page and the clip's Stitching panel.
    double rmsPx[2] = {0, 0};
    double transferDeg = 0; // lens 0 -> lens 1 agreement on the chart, median
    double coverageDeg[2] = {0, 0}; // furthest off-axis angle with corners

    // The model's parameters equal (what the stitch uses).
    static bool sameModel(const insv::LensCalibExt& a, const insv::LensCalibExt& b)
    {
        if (a.xi != b.xi || a.fx != b.fx || a.fy != b.fy || a.cx != b.cx || a.cy != b.cy ||
            a.yaw != b.yaw || a.pitch != b.pitch || a.roll != b.roll || a.tx != b.tx ||
            a.ty != b.ty || a.tz != b.tz || a.w != b.w || a.h != b.h || a.focalRefW != b.focalRefW)
            return false;
        for (int i = 0; i < 4; ++i)
            if (a.k[i] != b.k[i] || a.s[i] != b.s[i])
                return false;
        return a.p[0] == b.p[0] && a.p[1] == b.p[1];
    }
    // Whether it was measured against `file` (a clip's lens calibration).
    bool matchesFile(const insv::LensCalibExt& lens0, const insv::LensCalibExt& lens1) const
    {
        return sameModel(fileLenses[0], lens0) && sameModel(fileLenses[1], lens1);
    }

    bool operator==(const LensCalibration& o) const
    {
        return sameModel(lenses[0], o.lenses[0]) && sameModel(lenses[1], o.lenses[1]) &&
               sameModel(fileLenses[0], o.fileLenses[0]) &&
               sameModel(fileLenses[1], o.fileLenses[1]) && serial == o.serial &&
               lensGuard == o.lensGuard && created == o.created && squareMm == o.squareMm &&
               rmsPx[0] == o.rmsPx[0] && rmsPx[1] == o.rmsPx[1] &&
               transferDeg == o.transferDeg && coverageDeg[0] == o.coverageDeg[0] &&
               coverageDeg[1] == o.coverageDeg[1];
    }
    bool operator!=(const LensCalibration& o) const { return !(*this == o); }
};

// `file` (a clip's two lenses as its file has them) with `cal`'s model in
// place: the lens parameters and rotations; frame size, focal window and
// lens offsets stay the clip's.
inline void ApplyLensCalibration(const LensCalibration& cal, insv::LensCalibExt& lens0,
                                 insv::LensCalibExt& lens1)
{
    insv::LensCalibExt* out[2] = {&lens0, &lens1};
    for (int i = 0; i < 2; ++i) {
        const insv::LensCalibExt& c = cal.lenses[i];
        insv::LensCalibExt& e = *out[i];
        e.xi = c.xi;
        e.fx = c.fx;
        e.fy = c.fy;
        e.cx = c.cx;
        e.cy = c.cy;
        e.yaw = c.yaw;
        e.pitch = c.pitch;
        e.roll = c.roll;
        for (int k = 0; k < 4; ++k) {
            e.k[k] = c.k[k];
            e.s[k] = c.s[k];
        }
        e.p[0] = c.p[0];
        e.p[1] = c.p[1];
    }
}

} // namespace stitch
