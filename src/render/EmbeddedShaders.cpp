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
#include "seam_flow.comp.h"
#include "sharpen.comp.h"
#include "reframe_reproject.comp.h"
#include "fisheye_view_nv12.comp.h"
#include "equirect.frag.shape.h"
#include "fisheye_view.frag.shape.h"
#include "reframe_reproject.comp.shape.h"
#include "fisheye_view_nv12.comp.shape.h"
#include "equirect.frag.fx.h"
#include "fisheye_view.frag.fx.h"
#include "reframe_reproject.comp.fx.h"
#include "fisheye_view_nv12.comp.fx.h"
#include "equirect.frag.shape_fx.h"
#include "fisheye_view.frag.shape_fx.h"
#include "reframe_reproject.comp.shape_fx.h"
#include "fisheye_view_nv12.comp.shape_fx.h"
#include "inset.frag.fx.h"
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
ORBIS_SPIRV(seamFlowComp, kSpv_seam_flow_comp)
ORBIS_SPIRV(sharpenComp, kSpv_sharpen_comp)
ORBIS_SPIRV(equirectFragShape, kSpv_equirect_frag_shape)
ORBIS_SPIRV(fisheyeViewFragShape, kSpv_fisheye_view_frag_shape)
ORBIS_SPIRV(reframeReprojectCompShape, kSpv_reframe_reproject_comp_shape)
ORBIS_SPIRV(fisheyeViewNv12CompShape, kSpv_fisheye_view_nv12_comp_shape)
ORBIS_SPIRV(equirectFragFx, kSpv_equirect_frag_fx)
ORBIS_SPIRV(fisheyeViewFragFx, kSpv_fisheye_view_frag_fx)
ORBIS_SPIRV(reframeReprojectCompFx, kSpv_reframe_reproject_comp_fx)
ORBIS_SPIRV(fisheyeViewNv12CompFx, kSpv_fisheye_view_nv12_comp_fx)
ORBIS_SPIRV(equirectFragShapeFx, kSpv_equirect_frag_shape_fx)
ORBIS_SPIRV(fisheyeViewFragShapeFx, kSpv_fisheye_view_frag_shape_fx)
ORBIS_SPIRV(reframeReprojectCompShapeFx, kSpv_reframe_reproject_comp_shape_fx)
ORBIS_SPIRV(fisheyeViewNv12CompShapeFx, kSpv_fisheye_view_nv12_comp_shape_fx)
ORBIS_SPIRV(insetFragFx, kSpv_inset_frag_fx)

#undef ORBIS_SPIRV

} // namespace render::shaders
