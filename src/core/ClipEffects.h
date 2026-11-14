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
#include <cstdint>
#include <optional>
#include <string_view>

namespace fx {

// An effect dragged out of the Effects library: its name (kVignette, ...).
constexpr const char *kMimeType = "application/x-orbis-reframe-effect";
// The effects' names, as the library and the project file call them.
constexpr const char *kVignette = "vignette";
constexpr const char *kGrain = "grain";

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

// Film grain: noise over the picture, strongest in the midtones (black and
// white stay as they are), new on every frame. Its size is against the
// output frame's height, so it looks the same at every export size.
struct Grain {
    static constexpr float kMinSize = 50.0f;
    static constexpr float kMaxSize = 300.0f;

    bool on = true;
    // Percent: how strong; 100 = a spread (one standard deviation) of 15 %
    // of white in the midtones.
    float amount = 25.0f;
    // Percent: a grain's size, 100 = one pixel of a 1080-line frame.
    float size = 100.0f;
    // Percent: 0 = the same grain in every channel (grey), 100 = each
    // channel its own.
    float colour = 0.0f;

    void clamp()
    {
        amount = std::clamp(amount, 0.0f, 100.0f);
        size = std::clamp(size, kMinSize, kMaxSize);
        colour = std::clamp(colour, 0.0f, 100.0f);
    }
    bool active() const { return on && amount > 0.0f; }
    bool operator==(const Grain &o) const
    {
        return on == o.on && amount == o.amount && size == o.size && colour == o.colour;
    }
    bool operator!=(const Grain &o) const { return !(*this == o); }
};

// The grain's frame number (fx::Render::setFrame) for timeline time
// `timelineMs` at `fps` frames a second: the output frame it falls in.
inline uint32_t frameAt(int64_t timelineMs, double fps)
{
    return fps > 0.0 ? uint32_t(std::max<int64_t>(0, std::llround(double(timelineMs) * fps / 1000.0)))
                     : 0u;
}

// What the renderers apply, in the shaders' layout (clip_effects.glsl's
// uniform block): vec4s, all zero = nothing.
struct Render {
    // {amount -1..1, inner radius, outer radius, unused}; amount 0 = off.
    std::array<float, 4> vignette{};
    // {spread (fraction of white), grains per frame height, colour 0..1,
    // frame number}; spread 0 = off. The frame number seeds the noise: the
    // renderer sets it for the frame it draws (setFrame).
    std::array<float, 4> grain{};

    bool any() const { return vignette[0] != 0.0f || grain[0] != 0.0f; }
    // The frame the grain is drawn for -- the output frame on the timeline
    // (frameAt), or any number that changes with the picture. Kept to 24
    // bits, which a float holds exactly (77 hours at 60 fps).
    void setFrame(uint32_t n) { grain[3] = float(n & 0xFFFFFFu); }
    bool operator==(const Render &o) const { return vignette == o.vignette && grain == o.grain; }
    bool operator!=(const Render &o) const { return !(*this == o); }
};

// The effects on one clip, as the Effects section edits them and the project
// stores them. Absent = not on the clip.
struct ClipEffects {
    std::optional<Vignette> vignette;
    std::optional<Grain> grain;

    bool isEmpty() const { return !vignette && !grain; }
    // Whether it has the effect named `name`; add() puts it on with its
    // defaults (true if it wasn't there), remove() takes it off.
    bool has(const char *name) const
    {
        const std::string_view n(name);
        return (n == kVignette && vignette) || (n == kGrain && grain);
    }
    bool add(const char *name)
    {
        const std::string_view n(name);
        if (n == kVignette && !vignette) {
            vignette = Vignette{};
            return true;
        }
        if (n == kGrain && !grain) {
            grain = Grain{};
            return true;
        }
        return false;
    }
    void remove(const char *name)
    {
        const std::string_view n(name);
        if (n == kVignette)
            vignette.reset();
        else if (n == kGrain)
            grain.reset();
    }
    void clamp()
    {
        if (vignette)
            vignette->clamp();
        if (grain)
            grain->clamp();
    }
    bool operator==(const ClipEffects &o) const
    {
        return vignette == o.vignette && grain == o.grain;
    }
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
        if (grain && grain->active())
            r.grain = {0.15f * grain->amount / 100.0f, 1080.0f * 100.0f / grain->size,
                       grain->colour / 100.0f, 0.0f};
        return r;
    }
};

// The grain's noise (clip_effects.glsl has the same, bit for bit in the
// integer part): a hash per grain cell and channel, smoothly interpolated
// between cell centres and scaled back to the same spread everywhere.
inline uint32_t grainHash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A cell's value: two 16-bit uniforms summed, a triangle in [-1, 1] with
// variance 1/6.
inline float grainCell(int32_t cx, int32_t cy, uint32_t seed)
{
    const uint32_t hv = grainHash(uint32_t(cx) * 0x9E3779B1u ^ grainHash(uint32_t(cy) ^ seed));
    return (float(hv & 0xFFFFu) + float(hv >> 16)) / 65535.0f - 1.0f;
}

// Unit-variance noise at (qx, qy) in cells; `seed` picks the frame and
// channel.
inline float grainNoise(float qx, float qy, uint32_t seed)
{
    const float fx = qx - 0.5f, fy = qy - 0.5f;
    const float ix = std::floor(fx), iy = std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const int32_t cx = int32_t(ix), cy = int32_t(iy);
    const float a = grainCell(cx, cy, seed), b = grainCell(cx + 1, cy, seed);
    const float c = grainCell(cx, cy + 1, seed), d = grainCell(cx + 1, cy + 1, seed);
    const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    const float v = top + (bottom - top) * ty;
    // Blended values have less spread than the cells'; divide that out.
    const float nx = std::sqrt((1.0f - tx) * (1.0f - tx) + tx * tx);
    const float ny = std::sqrt((1.0f - ty) * (1.0f - ty) + ty * ty);
    return v / (nx * ny) * 2.4494897f; // sqrt(6): unit variance
}

// The seed of a frame's channel: 0 the grey grain, 1-3 red, green, blue.
inline uint32_t grainSeed(float frame, uint32_t channel)
{
    return grainHash(uint32_t(frame) * 4u + channel + 1u);
}

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
    if (e.grain[0] != 0.0f) {
        // Square cells against the frame's height.
        const float scale = e.grain[1] / h;
        const float qx = x * scale, qy = y * scale;
        const float grey = grainNoise(qx, qy, grainSeed(e.grain[3], 0));
        float n[3] = {grey, grey, grey};
        if (e.grain[2] > 0.0f) {
            const float own = std::sqrt(e.grain[2]), shared = std::sqrt(1.0f - e.grain[2]);
            for (uint32_t k = 0; k < 3; ++k)
                n[k] = shared * grey + own * grainNoise(qx, qy, grainSeed(e.grain[3], k + 1));
        }
        // Strongest in the midtones, none at black or white.
        const float l = std::clamp(
            (0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]) / white, 0.0f, 1.0f);
        const float s = e.grain[0] * white * 4.0f * l * (1.0f - l);
        for (int k = 0; k < 3; ++k)
            rgb[k] = std::clamp(rgb[k] + s * n[k], 0.0f, white);
    }
}

} // namespace fx
