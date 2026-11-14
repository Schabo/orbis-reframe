#pragma once

// A clip's video effects (the Effects library) and what the renderers apply.
// No Qt, no Vulkan, so the model, the GPU paths and the CPU export can all
// use it.
//
// Effects run in a fixed order -- the pipeline's -- and a clip has at most one
// of each, so a clip's effects are a set of optional members, not a list.
// Per-pixel effects run in the shaders that grade, right after the grade:
// src/render/shaders/clip_effects.glsl is the GLSL twin of applyEffects()
// below; change both together.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace fx {

// Darker (or lighter) toward the edges of the output frame -- the whole
// frame, also when a clip is split into panes. The falloff is an ellipse
// with the frame's shape, reaching 1 in the corners.
struct Vignette {
    static constexpr float kMinAmount = -100.0f;
    static constexpr float kMaxAmount = 100.0f;

    bool on = true;
    // Percent: negative darkens the edges (-100 = black), positive lightens
    // them (100 = white).
    float amount = -30.0f;
    // Percent of the way from the centre (0) to the corners (100) where the
    // falloff starts.
    float size = 50.0f;
    // Percent: 0 = a hard edge at `size`, 100 = the falloff spread from
    // there to well past the corners.
    float softness = 50.0f;

    void clamp()
    {
        amount = std::clamp(amount, kMinAmount, kMaxAmount);
        size = std::clamp(size, 0.0f, 100.0f);
        softness = std::clamp(softness, 0.0f, 100.0f);
    }
    // Changes the picture: on, with an amount.
    bool active() const { return on && amount != 0.0f; }
    bool operator==(const Vignette &o) const
    {
        return on == o.on && amount == o.amount && size == o.size && softness == o.softness;
    }
    bool operator!=(const Vignette &o) const { return !(*this == o); }
};

// What the renderers apply, in the shaders' layout (clip_effects.glsl's
// uniform block): vec4s, all zero = nothing.
struct Render {
    // {amount -1..1, inner radius, outer radius, unused}; amount 0 = off.
    std::array<float, 4> vignette{};

    bool any() const { return vignette[0] != 0.0f; }
    bool operator==(const Render &o) const { return vignette == o.vignette; }
    bool operator!=(const Render &o) const { return !(*this == o); }
};

// The effects on one clip, as the Effects section edits them and the project
// stores them. Absent = not on the clip.
struct ClipEffects {
    std::optional<Vignette> vignette;

    bool isEmpty() const { return !vignette; }
    void clamp()
    {
        if (vignette)
            vignette->clamp();
    }
    bool operator==(const ClipEffects &o) const { return vignette == o.vignette; }
    bool operator!=(const ClipEffects &o) const { return !(*this == o); }

    Render render() const
    {
        Render r;
        if (vignette && vignette->active()) {
            const float inner = vignette->size / 100.0f;
            // The outer edge a little past the inner one at softness 0, so
            // the hard edge is still a smoothstep, not a division by zero.
            const float outer =
                std::max(inner + 1e-3f, inner + (1.5f - inner) * vignette->softness / 100.0f);
            r.vignette = {vignette->amount / 100.0f, inner, outer, 0.0f};
        }
        return r;
    }
};

// The effects at one output pixel: `rgb` in [0, white] (white = 1 or 255,
// as the grade's), the pixel's centre (x, y) in a frame of w x h pixels.
// The C++ twin of clip_effects.glsl's applyEffects().
inline void applyEffects(float rgb[3], float x, float y, float w, float h, const Render &e,
                         float white)
{
    if (e.vignette[0] != 0.0f) {
        const float u = 2.0f * x / w - 1.0f;
        const float v = 2.0f * y / h - 1.0f;
        const float r = std::sqrt((u * u + v * v) * 0.5f);
        const float t0 = std::clamp((r - e.vignette[1]) / (e.vignette[2] - e.vignette[1]), 0.0f,
                                    1.0f);
        const float t = t0 * t0 * (3.0f - 2.0f * t0); // GLSL smoothstep
        const float a = e.vignette[0] * t;
        for (int k = 0; k < 3; ++k)
            rgb[k] = a < 0.0f ? rgb[k] * (1.0f + a) : rgb[k] + (white - rgb[k]) * a;
    }
}

} // namespace fx
