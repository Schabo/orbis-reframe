#pragma once

#include <cstddef>
#include <cstdint>

// The SPIR-V of every shader in src/render/shaders/, compiled into the binary
// at build time (glslangValidator --vn, see CMakeLists.txt) so nothing is read
// from disk and the binary can be installed anywhere.
namespace render::shaders {

struct Spirv {
    const uint32_t* code = nullptr;
    size_t bytes = 0; // VkShaderModuleCreateInfo::codeSize
};

Spirv equirectVert();
Spirv equirectFrag();
Spirv fisheyeViewFrag();
Spirv insetFrag();
Spirv markerFrag();
Spirv regionFrag();
Spirv fisheyeStitchComp();
Spirv fisheyeStitchNv12Comp();
Spirv reframeReprojectComp();
Spirv fisheyeViewNv12Comp();

} // namespace render::shaders
