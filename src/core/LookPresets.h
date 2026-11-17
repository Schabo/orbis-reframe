#pragma once

// The colour looks built into the app (the Effects library's Colour group).
// Each is a formula sampled into a 3D LUT, so a built-in look takes the same
// path through every renderer as a look loaded from a .cube file. A project
// stores a built-in look by its id; a look whose formula ever changes gets a
// new id, so a saved project keeps its picture.

#include <array>
#include <memory>
#include <string_view>

namespace color {

struct Lut3D;

namespace look {

// Black & White: monochrome with the channels weighted like a yellow filter
// on film (a blue sky a little darker, so clouds stand out) and a gentle
// filmic curve -- not the flat grey of the Colour panel's Saturation -100.
constexpr const char *kBlackWhite = "bw";

// The built-in looks' ids, in the library's order.
constexpr std::array<const char *, 1> kPresets{kBlackWhite};

constexpr bool isPreset(std::string_view id)
{
    for (const char *p : kPresets)
        if (id == p)
            return true;
    return false;
}

// The built-in look `id` as a LUT, built on first use and shared after;
// nullptr for an id this build doesn't know (a later version's look).
std::shared_ptr<const Lut3D> presetLut(std::string_view id);

// Black & White at one colour (Rec.709-encoded, each 0..1): the formula
// presetLut(kBlackWhite) samples.
void blackWhite(const float in[3], float out[3]);

} // namespace look
} // namespace color
