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
Spirv seamFlowComp();
// The view shaders' view-shape build (VIEW_SHAPE): for a view whose Natural
// or Fisheye is above 0 (view::Lens::shaped); the ones above leave them out.
Spirv equirectFragShape();
Spirv fisheyeViewFragShape();
Spirv reframeReprojectCompShape();
Spirv fisheyeViewNv12CompShape();

} // namespace render::shaders
