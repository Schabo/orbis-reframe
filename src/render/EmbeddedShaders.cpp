#include "EmbeddedShaders.h"

#include <cstdint>

namespace {
// Generated into build/shaders/ (one `const uint32_t kSpv_<name>[]` each).
#include "equirect.vert.h"
#include "equirect.frag.h"
#include "fisheye_view.frag.h"
#include "inset.frag.h"
#include "marker.frag.h"
#include "region.frag.h"
#include "fisheye_stitch.comp.h"
#include "fisheye_stitch_nv12.comp.h"
#include "reframe_reproject.comp.h"
#include "fisheye_view_nv12.comp.h"
} // namespace

namespace render::shaders {

#define ORBIS_SPIRV(fn, array) \
    Spirv fn() { return {array, sizeof(array)}; }

ORBIS_SPIRV(equirectVert, kSpv_equirect_vert)
ORBIS_SPIRV(equirectFrag, kSpv_equirect_frag)
ORBIS_SPIRV(fisheyeViewFrag, kSpv_fisheye_view_frag)
ORBIS_SPIRV(insetFrag, kSpv_inset_frag)
ORBIS_SPIRV(markerFrag, kSpv_marker_frag)
ORBIS_SPIRV(regionFrag, kSpv_region_frag)
ORBIS_SPIRV(fisheyeStitchComp, kSpv_fisheye_stitch_comp)
ORBIS_SPIRV(fisheyeStitchNv12Comp, kSpv_fisheye_stitch_nv12_comp)
ORBIS_SPIRV(reframeReprojectComp, kSpv_reframe_reproject_comp)
ORBIS_SPIRV(fisheyeViewNv12Comp, kSpv_fisheye_view_nv12_comp)

#undef ORBIS_SPIRV

} // namespace render::shaders
