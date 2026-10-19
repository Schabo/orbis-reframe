#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

// Project-level output format (Project Settings panel): the flat frame the
// reframe path is authored for and exported at. Aspect ratio is first-class
// -- in a 360 reframe editor each aspect is a different crop of the same
// sphere, not letterboxing -- so the viewport pins its rectilinear aspect to
// width/height and export defaults to this size. Reframe
// keys stay in yaw/pitch/roll/FOV terms, so changing the aspect later only
// changes the crop window they're seen through.
//
// Plain value type, no Qt: shared by Project (persisted as the .orp's
// "output" block), File ▸ Project settings… (ProjectSettingsDialog), and ExportOptionsDialog.
struct OutputSettings {
    enum class Aspect { Landscape16x9, Portrait9x16, Square1x1, Classic4x3 };

    Aspect aspect = Aspect::Landscape16x9;
    int width = 1920;
    int height = 1080;
    // Output frame rate. 0 = match the first clip's native rate (the pre-panel
    // behaviour, and what an .orp without an "output" block loads as).
    // Clips at another rate are conformed to it (ReframeExportController).
    double fps = 0.0;
    // Export 10 bits per sample (HEVC Main 10, still BT.709 SDR) instead of
    // 8: keeps the gradation of 10-bit clips (log footage to grade in
    // another program, smooth skies). H.265 only; off by default -- 8-bit
    // plays everywhere.
    bool tenBit = false;

    double aspectRatio() const { return height > 0 ? double(width) / height : 16.0 / 9.0; }

    // The rate to actually export at, given the first clip's rate.
    double effectiveFps(double firstClipFps) const
    {
        return fps > 0.0 ? fps : firstClipFps;
    }

    bool operator==(const OutputSettings &o) const
    {
        return aspect == o.aspect && width == o.width && height == o.height && fps == o.fps &&
               tenBit == o.tenBit;
    }
    bool operator!=(const OutputSettings &o) const { return !(*this == o); }

    struct Size {
        int w;
        int h;
    };

    // Resolution presets per aspect, smallest first; the default is marked by
    // defaultPresetIndex(). All even and within NVENC's H.264 limit (4096).
    static std::span<const Size> presets(Aspect a)
    {
        static constexpr Size k16x9[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        static constexpr Size k9x16[] = {{720, 1280}, {1080, 1920}, {1440, 2560}, {2160, 3840}};
        static constexpr Size k1x1[] = {{720, 720}, {1080, 1080}, {1440, 1440}, {2160, 2160}};
        static constexpr Size k4x3[] = {{960, 720}, {1440, 1080}, {1920, 1440}, {2880, 2160}};
        switch (a) {
        case Aspect::Portrait9x16: return k9x16;
        case Aspect::Square1x1: return k1x1;
        case Aspect::Classic4x3: return k4x3;
        case Aspect::Landscape16x9: break;
        }
        return k16x9;
    }
    // The "1080p-class" entry of each preset list.
    static constexpr int defaultPresetIndex() { return 1; }

    // Common delivery rates, offered after "Match first clip" (fps = 0).
    static std::span<const double> fpsPresets()
    {
        static constexpr double k[] = {23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0};
        return k;
    }

    // Export quality levels (Export dialog). Custom = a bitrate typed in.
    enum class Quality { Smaller, Standard, High, Custom };

    // Video bitrate range the export accepts: both NVENC encoders open at
    // 300 Mbit/s with the 1.5x VBR peak (ReframeFlatten), at every preset size.
    static constexpr int64_t kMinBitrateKbps = 1000;
    static constexpr int64_t kMaxBitrateKbps = 300000;

    // The video bitrate for a quality level at w x h, fps, codec. Standard is
    // a generous delivery rate: 40 Mbit/s for 1080p30 H.264, sub-linear in
    // pixel count (~21 at 720p, ~121 at 2160p) and frame rate (x1.5 at 60),
    // and 30 % less for H.265. Smaller halves it, High doubles it. Whole
    // Mbit/s, clamped to the accepted range. Custom => Standard's rate.
    static int64_t bitrateKbpsFor(int w, int h, double fps, bool h265,
                                  Quality q = Quality::Standard)
    {
        const double px = double(w) * h / (1920.0 * 1080.0);
        const double rate = fps > 0.0 ? fps / 30.0 : 1.0;
        double mbps = 40.0 * std::pow(px, 0.8) * std::pow(rate, 0.6) * (h265 ? 0.7 : 1.0);
        if (q == Quality::Smaller)
            mbps *= 0.5;
        else if (q == Quality::High)
            mbps *= 2.0;
        const int64_t kbps = std::llround(mbps) * 1000;
        return std::clamp(kbps, kMinBitrateKbps, kMaxBitrateKbps);
    }
};
