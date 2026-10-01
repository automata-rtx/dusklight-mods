// Deferred Fog: moves the game's fog out of the opaque draws and into one fullscreen pass after
// them, so screen-space effects composited at SCENE_AFTER_OPAQUE (AO, shadows) darken the surface
// under the fog instead of the fog itself.
//
// Per frame, on the game thread unless noted:
//   SCENE_BEGIN         open the suppression scope (not while disabled or during Wolf Senses).
//   inside the scope    hooks on GXSetFog/GFSetFog, J3DShape::drawFast and dBgp_c's shared
//                       display lists record each draw's fog config and switch its fog off.
//   SCENE_AFTER_OPAQUE  close the scope and arm the quad. In Exact mode, when the frame used
//                       several configs (or Skip Unfogged has geometry to mark), replay the opaque
//                       lists into a per-pixel config-ID buffer.
//   quad anchor         push the quad at the first J3DShape::drawFast after that stage, else before
//                       the game's bloom, else at FRAME_BEFORE_HUD (see QuadAnchor).
//   render worker       on_draw records the quad; res/fog.wgsl applies aurora's fog math, with the
//                       coefficients from src/fog_math.h.
//
// Other mods need no import to composite under the fog: their SCENE_AFTER_OPAQUE hooks always run
// before the quad. Game-linked (hooks game functions) and webgpu. Design: docs/deferred_fog.md.

#include "global.h"

#include "fog_math.h"

#include "deferred_fog_service.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_flower.h"
#include "d/actor/d_grass.h"
#include "d/d_bg_parts.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_graphic.h"
#include "dolphin/gf/GFPixel.h"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXLighting.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTev.h"

#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"

#include "gfx_scene_pass.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

namespace {

// Hook targets (each emits a modmeta hook record the host resolves by symbol at load). Only
// GXSetFog is required (init fails without it); without drawFast the mod stays inert; the others
// log a warning and degrade.
DEFINE_HOOK(GXSetFog, SetFog);
DEFINE_HOOK(GFSetFog, SetGfFog);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);
// dBgp_c's shared-display-list path, which bypasses drawFast (see on_material_shared_dl_post).
// drawSimple brackets it; there is one loadSharedDL hook per material class.
DEFINE_HOOK(&dBgp_c::modelMaterial_c::drawSimple, BgpDrawSimple);
// The self-drawing grass and flower packets (see g_selfDrawnIndex).
DEFINE_HOOK(&dGrass_packet_c::draw, GrassPacketDraw);
DEFINE_HOOK(&dFlower_packet_c::draw, FlowerPacketDraw);
DEFINE_HOOK(&J3DMaterial::loadSharedDL, MaterialSharedDL);
// Fallback quad anchor before the game's bloom (see QuadAnchor).
DEFINE_HOOK(&mDoGph_gInf_c::bloom_c::draw, BloomDraw);
DEFINE_HOOK(&J3DPatchedMaterial::loadSharedDL, PatchedMaterialSharedDL);
DEFINE_HOOK(&J3DLockedMaterial::loadSharedDL, LockedMaterialSharedDL);

ConfigVarHandle g_cvarFogEnabled = 0;    // DEFAULT below in init()
ConfigVarHandle g_cvarFogMixed = 0;      // DEFAULT below in init()
ConfigVarHandle g_cvarFogDebug = 0;      // DEFAULT below in init()
ConfigVarHandle g_cvarFogSkipUnfogged = 0;  // DEFAULT below in init()
ConfigVarHandle g_cvarFogDeferInSenses = 0;  // DEFAULT below in init()

UiWindowHandle g_controlsWindow = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_fogPipeline = nullptr;
WGPURenderPipeline g_fogDebugPipeline = nullptr;
WGPURenderPipeline g_mixedPipeline = nullptr;
WGPURenderPipeline g_mixedDebugPipeline = nullptr;
WGPUBindGroupLayout g_fogLayout = nullptr;
WGPUBindGroupLayout g_fogDebugLayout = nullptr;
WGPUBindGroupLayout g_mixedLayout = nullptr;
WGPUBindGroupLayout g_mixedDebugLayout = nullptr;

// The four fog pipelines are built lazily from the live GfxDrawContext::layout and rebuilt whenever
// layout.key changes. The scene pass gains the authored-normal attachment at runtime, from the
// frame after any mod first asks for normals (VBAO and SMAA do; this mod does not), and WebGPU
// rejects a pipeline built for the old shape with no visible error: the fog quad just stops
// drawing.
uint64_t g_sceneLayoutKey = 0;
bool g_sceneLayoutValid = false;
bool ensure_fog_pipelines(const GfxDrawContext& ctx);
void release_fog_pipelines();

// GX fog range adjustment, which the game calls XFog (GxXFog_set, mXFogTbl). Fog is driven by Z,
// the distance along the view axis, but a pixel near the left or right edge is further from the eye
// than a centre pixel with the same Z. GXSetFogRangeAdj corrects for that with a per-column
// multiplier on the fog term, built from a 10-entry table and a centre column and applied before
// the start-Z bias c is subtracted.
//
// TP enables it in envcolor_init and never turns it off. The three direct fog setters
// (dKy_GxFog_set, dKy_GxFog_tevstr_set, dKy_GfFog_tevstr_set) call GxXFog_set() right after setting
// fog, and setLightTevColorType_MAJI_sub stamps the same globals into each material fog block it
// processes. Aurora implements it (build_fog_range_lut), so leaving it out would change the fog
// term by (lut - 1) * (fogF + c): about 3% at the screen edges with TP's default table, which is
// most visible in far-starting bands where c = startZ / (endZ - startZ) is large. fog.wgsl
// evaluates the multiplier per pixel (fog_range_factor).
struct FogRangeAdj {
    bool enable = false;
    uint16_t center = 0x140;
    uint16_t table[10] = {};
};

struct FogConfig {
    bool valid = false;
    uint8_t type = 0;
    float startZ = 0.0f;
    float endZ = 0.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    GXColor color{0, 0, 0, 0};
    FogRangeAdj adj;
};

// True from SCENE_BEGIN to SCENE_AFTER_OPAQUE while fog is being captured and suppressed.
bool g_scopeActive = false;
// Set at SCENE_AFTER_OPAQUE; the first anchor that sees it pushes the quad and clears it.
bool g_quadArmed = false;

// Where this frame's fog quad was pushed, shown on the Status line. The quad belongs right after
// the SCENE_AFTER_OPAQUE stage and before the translucent lists. There is no stage hook there, so
// the mod pushes it from the first J3DShape::drawFast after the stage, which is the first
// translucent J3D draw. (dComIfGd_drawXluListBG is DUSK_NOINLINE on this pin, so hooking it
// directly may now be possible; that is untested.)
//
// A frame with no translucent J3D draw falls back to a pre-hook on mDoGph_gInf_c::bloom_c::draw,
// and failing that to FRAME_BEFORE_HUD, which runs after bloom (on by default: game.bloomMode
// defaults to BloomMode::Dusk). Fog applied after bloom is fog the bloom never saw, so brightly
// bloomed distant subjects come out dimmer. Both fallbacks also fog the translucent geometry drawn
// before them.
enum class QuadAnchor : uint8_t { None, Translucent, BeforeBloom, FrameEnd };
QuadAnchor g_quadAnchor = QuadAnchor::None;

// The anchor the Status line reports. The line is built in on_scene_after_opaque, before this
// frame's anchor fires, so it shows the previous frame's.
QuadAnchor g_lastQuadAnchor = QuadAnchor::None;

const char* quad_anchor_name() {
    switch (g_lastQuadAnchor) {
    case QuadAnchor::Translucent: return "at translucents";
    case QuadAnchor::BeforeBloom: return "before bloom";
    case QuadAnchor::FrameEnd: return "AFTER BLOOM";
    default: return "not pushed";
    }
}
// Published through the service (deferred_fog_service.h): whether this frame's quad was armed.
// Written once per frame at SCENE_AFTER_OPAQUE; game thread only.
bool g_lastFrameDeferred = false;
// Vanilla mode: whether this frame may suppress configs that match the reference. It is the
// previous frame's verdict (no deviant configs), because a frame's configs are known only after
// its opaque lists have drawn. In the first frame of a mixed scene the matching draws are still
// deferred, and the deviant draws get both their forward fog and the quad.
bool g_suppressAllowed = false;
bool g_shapeHookOk = false;
bool g_warnedPushFailure = false;
// The frame's first captured config. The single-config quad uses it.
FogConfig g_reference;
uint32_t g_suppressedCount = 0;
uint32_t g_deviantCount = 0;

bool g_wasSuppressing = false;
FogConfig g_firstDeviant;
char g_statusText[224] = "Waiting for first fogged frame";

constexpr uint32_t kMaxFogConfigs = 8;

// Config-ID index for "vanilla drew this pixel with no fog; leave it alone". stamp_replay_id writes
// (index + 1) * 24 into the red channel, so real configs 0..7 use 24..192 and this index writes
// 216, which fog.wgsl decodes as slot 9 and tests before its config-range check. The byte survives
// exactly: the offscreen ID target is single-sample, takes aurora's linear (non-sRGB) surface
// format, and the shader reads it with textureLoad.
constexpr uint32_t kNoFogSlot = 8;
static_assert(kNoFogSlot >= kMaxFogConfigs, "the sentinel must not collide with a real config");
static_assert((kNoFogSlot + 1) * 24 <= 255, "the sentinel must fit in the red channel");
// Exact mode: the distinct configs captured this frame, indexed as in the config-ID buffer.
FogConfig g_frameConfigs[kMaxFogConfigs];
uint32_t g_frameConfigCount = 0;
// True while replay_config_ids re-draws the opaque lists.
bool g_fogReplayActive = false;
// This frame's resolved config-ID buffer. Borrowed from the gfx service; valid this frame only.
WGPUTextureView g_configIdView = nullptr;
bool g_wasMixed = false;
bool g_warnedReplayFailure = false;

// Set in the shader-side fog_type when range adjustment applies to that config. GXFogType's own
// 0x08 bit means orthographic and the shader masks the type to three bits, so this uses 0x10.
constexpr uint32_t kFogTypeRangeAdjBit = 0x10u;

// The uniform structs below mirror res/fog.wgsl byte for byte; keep the static_asserts true.
//
// FogRange: one per frame, shared by every config. The game stamps the table and centre from the
// same g_env_light globals, so normally only the enable differs between configs (it rides in
// fog_type); the mixed pass uses the first enabled config's table.
struct FogRangeUniform {
    float center;   // fog-range centre column, in NDC x
    float _pad0;
    float _pad1;
    float _pad2;
    float k[12];    // aurora's 10 range constants (pair-swapped, /64); 10 and 11 replicate 9
};
static_assert(sizeof(FogRangeUniform) == 64);

// FogUniforms: the single-config pass (fs_main).
struct FogUniforms {
    float color[4];
    float a;
    float b;
    float c;
    uint32_t fog_type;
    uint32_t debug_mode;
    float _pad0;
    float _pad1;
    float _pad2;
    FogRangeUniform range;
};
static_assert(sizeof(FogUniforms) % 16 == 0);
static_assert(sizeof(FogUniforms) == 112);
static_assert(offsetof(FogUniforms, range) == 48);

// MixedFogEntry / MixedFogUniforms: the per-pixel-config pass (fs_mixed).
struct MixedFogEntry {
    float color[4];
    float a;
    float b;
    float c;
    uint32_t fog_type;
};
static_assert(sizeof(MixedFogEntry) == 32);
struct MixedFogUniforms {
    MixedFogEntry configs[8];
    uint32_t count;
    uint32_t debug_mode;
    uint32_t fallback_index;  // config for pixels the ID replay didn't cover (see push_fog_quad)
    float _pad1;
    FogRangeUniform range;
};
static_assert(sizeof(MixedFogUniforms) % 16 == 0);
static_assert(sizeof(MixedFogUniforms) == 336);
static_assert(offsetof(MixedFogUniforms, range) == 272);

// Inline payload handed to on_draw on the render worker. configIds is null for the single-config
// pass. The views are borrowed for this frame only.
struct DrawPayload {
    WGPUTextureView sceneDepth;
    WGPUTextureView configIds;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_mode;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

int64_t get_int_option(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool effect_enabled() {
    return get_bool_option(g_cvarFogEnabled, true) && g_shapeHookOk;
}

// Wolf Senses: while the senses are up, the environment fog setters replace the palette fog with
// black fog over a short range (dKy_WolfPowerup_FogNearFar; 750..1750 outdoors and 1000..1800
// indoors by default, other stages differ). Black fog commutes with multiplicative composites,
// m * (1 - f) * x == (1 - f) * (m * x), so deferring it gains nothing, while the quad's single
// depth per pixel can only lose exactness (it visibly revealed far more of the scene in some camera
// directions). So no scope opens while checkNowWolfPowerUp() is true, the same test those setters
// use. An additive composite (indirect light) would differ; none is currently built.
// fogDeferInSenses disables this exemption for diagnosis. The player check is needed because
// checkNowWolfEyeUp dereferences the player actor.
bool wolf_senses_active() {
    return dComIfGp_getLinkPlayer() != nullptr && daPy_py_c::checkNowWolfPowerUp();
}

bool g_sensesExempt = false;
bool g_wasSensesExempt = false;

// The world viewport width in the game's logical coordinates, sampled at SCENE_AFTER_OPAQUE while
// the world viewport is still current. Aurora's range centre is
// ((center - vp.left) / vp.width) * 2 - 1 + (renderVp.left / renderVp.width) * 2, and the render
// viewport is the logical one scaled along x (map_logical_viewport), so the left terms cancel and
// only the width is needed: 2 * center / vp.width - 1.
float g_frameViewportWidth = 640.0f;

// Range adjustment for a material's own fog: J3DFog::load() issues J3DGDSetFogRangeAdj from the
// same block as the fog, so the block is authoritative for the shapes the material draws.
void capture_adj_from_material(const J3DFog& fog, FogConfig& out) {
    out.adj.enable = fog.mAdjEnable != 0;
    out.adj.center = fog.mCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.adj.table[i] = fog.mFogAdjTable.r[i];
    }
}

// Range adjustment for a config captured at GXSetFog/GFSetFog: the direct setters call GxXFog_set()
// right afterwards, which issues GXSetFogRangeAdj from these globals.
void capture_adj_from_env(FogConfig& out) {
    out.adj.enable = g_env_light.mFogAdjEnable != 0;
    out.adj.center = g_env_light.mFogAdjCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.adj.table[i] = g_env_light.mXFogTbl.r[i];
    }
}

// Packs the range constants as aurora's build_fog_range_lut prepares them (pair-swapped, scaled by
// 1/64) plus the centre column in NDC. fog.wgsl evaluates the LUT's formula per pixel instead of
// reading a baked per-column table.
FogRangeUniform build_fog_range(const FogRangeAdj& adj) {
    FogRangeUniform out{};
    out.center =
        2.0f * static_cast<float>(adj.center) / std::max(g_frameViewportWidth, 1.0f) - 1.0f;
    for (uint32_t i = 0; i < 10; ++i) {
        const uint32_t source = (i & ~1u) | (1u - (i & 1u));
        out.k[i] = static_cast<float>(adj.table[source]) / 64.0f;
    }
    out.k[10] = out.k[9];
    out.k[11] = out.k[9];
    return out;
}

// Whether two configs render the same fog, within small tolerances so near-identical configs share
// one table slot.
bool config_matches(const FogConfig& reference, const FogConfig& candidate) {
    // Range adjustment is part of the config. Normally every config carries the same globals; a
    // material whose fog block the game never re-stamps keeps the values authored in its model.
    if (reference.adj.enable != candidate.adj.enable) {
        return false;
    }
    if (reference.adj.enable &&
        (reference.adj.center != candidate.adj.center ||
            std::memcmp(reference.adj.table, candidate.adj.table, sizeof(reference.adj.table)) !=
                0))
    {
        return false;
    }
    if (reference.type != candidate.type) {
        return false;
    }
    const auto colorClose = [](uint8_t lhs, uint8_t rhs) {
        return std::abs(static_cast<int>(lhs) - static_cast<int>(rhs)) <= 6;
    };
    if (!colorClose(reference.color.r, candidate.color.r) ||
        !colorClose(reference.color.g, candidate.color.g) ||
        !colorClose(reference.color.b, candidate.color.b))
    {
        return false;
    }
    const float span = std::max(std::fabs(reference.endZ - reference.startZ), 1.0f);
    return std::fabs(candidate.startZ - reference.startZ) <= span * 0.02f &&
           std::fabs(candidate.endZ - reference.endZ) <= span * 0.02f &&
           std::fabs(candidate.nearZ - reference.nearZ) <= 1.0f &&
           std::fabs(candidate.farZ - reference.farZ) <= reference.farZ * 0.01f + 1.0f;
}

// The Hyrule Castle barrier dome (actors d_a_obj_ganonwall and d_a_obj_ganonwall2) is translucent,
// but its materials draw inside the suppression scope (seen in-game: stamping its config in the
// replay darkened the castle behind it). Each Draw() rewrites their fog to black over 1000..250000.
// Deferring that config fails twice: the replay stamps the dome as a solid surface, so its black
// fog lands on the castle and trees behind it, and the dome loses its own fog-before-blend. So the
// barrier keeps its forward fog: its draws are neither suppressed nor registered, and in the replay
// it writes no colour, so its pixels take the config of whatever is behind. (The quad still adds
// that fog over the dome; one fullscreen pass cannot fog through a translucent surface.)
//
// Match the exact literal triple the actors write. A looser test (black && endZ > 100000) also
// caught the game's own mType 7 materials (water MA03/MA17/MA19, and MA20), which
// setLightTevColorType_MAJI_sub turns black while keeping the room palette's range, and fogged
// them twice.
bool is_barrier_fog(const FogConfig& c) {
    return c.color.r == 0 && c.color.g == 0 && c.color.b == 0 && c.startZ == 1000.0f &&
        c.endZ == 250000.0f;
}

// fogSkipUnfogged, read once per frame in on_scene_begin. Off by default because it forces the
// config-ID replay (an extra pass over the opaque geometry) in frames that would otherwise be
// uniform; the Status line's fog-off counts show whether it can have any effect in a view.
bool g_skipUnfogged = false;

bool read_skip_unfogged() {
    return get_bool_option(g_cvarFogSkipUnfogged, false);
}

bool skip_unfogged_geometry() {
    return g_skipUnfogged;
}

// The fallback must equal fogMixedMode's registered default (1 = Exact); otherwise a failed config
// read silently runs a mode the UI is not showing.
bool exact_mode() {
    return get_int_option(g_cvarFogMixed, 1) == 1;
}

// The one test for "this frame uses the config-ID buffer", shared by on_scene_after_opaque (which
// builds the buffer) and push_fog_quad (which reads it). Keep it in one function so the two cannot
// drift apart.
bool needs_id_buffer();

// Returns the config's slot in this frame's table, adding it if new. When the table is full, an
// unmatched config is merged into slot 0.
uint32_t register_frame_config(const FogConfig& config) {
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        if (config_matches(g_frameConfigs[i], config)) {
            return i;
        }
    }
    if (g_frameConfigCount < kMaxFogConfigs) {
        g_frameConfigs[g_frameConfigCount] = config;
        g_frameConfigs[g_frameConfigCount].valid = true;
        return g_frameConfigCount++;
    }
    return 0;
}

// Like register_frame_config but never adds; an unknown config maps to slot 0.
uint32_t lookup_frame_config(const FogConfig& config) {
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        if (config_matches(g_frameConfigs[i], config)) {
            return i;
        }
    }
    return 0;
}

// Records a captured config and returns whether to suppress its forward fog. Exact mode registers
// it in the frame table and always suppresses ("deviant" there just means "not slot 0"). Vanilla
// mode suppresses only configs matching the reference, and only if g_suppressAllowed.
bool vote_config(const FogConfig& config) {
    if (!g_reference.valid) {
        g_reference = config;
        g_reference.valid = true;
    }
    if (exact_mode()) {
        const uint32_t index = register_frame_config(config);
        if (index != 0) {
            if (g_deviantCount == 0) {
                g_firstDeviant = config;
                g_firstDeviant.valid = true;
            }
            ++g_deviantCount;
        } else {
            ++g_suppressedCount;
        }
        return true;
    }
    if (!config_matches(g_reference, config)) {
        if (g_deviantCount == 0) {
            g_firstDeviant = config;
            g_firstDeviant.valid = true;
        }
        ++g_deviantCount;
        return false;
    }
    if (!g_suppressAllowed) {
        return false;
    }
    ++g_suppressedCount;
    return true;
}

void push_fog_quad();

// A material's fog as the deferred pass sees it:
//   Live     its J3DFog block programs real fog; capture and suppress it.
//   Off      its block has mType 0. J3DFog::load() then programs GX_FOG_NONE, and
//            setLightTevColorType_MAJI_sub leaves such blocks alone, so vanilla draws this geometry
//            with no fog at any distance. J3DGDSetFog writes raw BP commands, so the GXSetFog hook
//            never sees it.
//   NoBlock  no fog block; the draw inherits the last GXSetFog, which that hook already handled.
//            Retail TP should not produce this (the game's model loaders request J3DPEBlockFull,
//            PE flag 0x10000000). It is kept apart from Off so the fog-off mark never fires on
//            geometry that does have fog.
enum class MaterialFog : uint8_t { NoBlock, Off, Live };

MaterialFog material_fog_state(J3DMaterial* material, FogConfig& out) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    J3DFog* fog = peBlock != nullptr ? peBlock->getFog() : nullptr;
    if (fog == nullptr) {
        return MaterialFog::NoBlock;
    }
    if (fog->mType == 0) {
        return MaterialFog::Off;
    }
    out.type = fog->mType;
    out.startZ = fog->mStartZ;
    out.endZ = fog->mEndZ;
    out.nearZ = fog->mNearZ;
    out.farZ = fog->mFarZ;
    out.color = fog->mColor;
    capture_adj_from_material(*fog, out);
    return MaterialFog::Live;
}

// Whether the material's blend makes forward and deferred fog differ (counted for the Status line).
// Aurora fogs each fragment's source colour in the shader, before the pipeline blend; the quad fogs
// the blended result. With one fog factor f and colour F for the pixel, layers drawn with GX
// factors (s_i, d_i) differ by f * F * (K - 1), where K = sum_i(s_i * prod_{j>i} d_j). An alpha
// blend over an opaque base gives K = 1, no difference; additive blends (dst factor ONE) and
// GX_BM_SUBTRACT (ReverseSubtract One/One in aurora) do not. This is also why blended draws must
// not simply be exempted from suppression: the quad still fogs their pixels, so they get fogged
// twice. See docs/deferred_fog.md "Known issues".
bool material_over_unity_blend(J3DMaterial* material) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DBlend* blend = peBlock != nullptr ? peBlock->getBlend() : nullptr;
    if (blend == nullptr) {
        return false;
    }
    const GXBlendMode mode = blend->getBlendMode();
    if (mode == GX_BM_SUBTRACT) {
        return true;
    }
    return mode == GX_BM_BLEND && blend->getDstFactor() == GX_BL_ONE;
}

// The fog-off mark may only go on materials whose alpha test passes everything. stamp_replay_id
// forces GX_ALWAYS and binds no texture, so an alpha-tested material stamps its whole primitive
// rather than its cutout, and a mark that differs from its surroundings would punch a hole in the
// fog. calcAlphaCmpID packs (comp0 << 5) + (op << 3) + comp1 and GX_ALWAYS is 7, so the default
// 0x00E7 is "always AND always".
bool material_alpha_test_trivial(J3DMaterial* material) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DAlphaComp* comp = peBlock != nullptr ? peBlock->getAlphaComp() : nullptr;
    if (comp == nullptr) {
        return false;
    }
    constexpr uint16_t kAlways = 7;
    return ((comp->mID >> 5) & 7) == kAlways && (comp->mID & 7) == kAlways;
}

// The quad takes its fog factor from the depth buffer, so only a draw that owns the depth at a
// pixel may decide that pixel's fog. Marking a fog-off overlay that does not write depth would
// remove the fog from whatever is behind it.
bool material_owns_depth(J3DMaterial* material) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DZMode* zMode = peBlock != nullptr ? peBlock->getZMode() : nullptr;
    return zMode != nullptr && zMode->getCompareEnable() != 0 && zMode->getUpdateEnable() != 0;
}

// Per-frame Status line counters (docs/deferred_fog.md "Status line"). They count draws in the
// scope (each drawFast or shared-DL load), not distinct materials.
uint32_t g_fogOffCount = 0;            // draws vanilla renders with no fog (MaterialFog::Off)
uint32_t g_fogOffNoDepth = 0;          // ... of those, draws that do not own their depth
uint32_t g_fogOffAlphaTested = 0;      // ... draws that own depth but are alpha-tested
uint32_t g_overUnityCount = 0;         // draws whose blend gives K != 1 (additive / subtract)
uint32_t g_noDepthOverUnityCount = 0;  // ... of those, draws that do not own their depth

// Fog-off draws that pass both marking restrictions (own their depth, trivial alpha test).
uint32_t fog_off_markable() {
    return g_fogOffCount - g_fogOffNoDepth - g_fogOffAlphaTested;
}

bool needs_id_buffer() {
    if (!exact_mode()) {
        return false;
    }
    return g_frameConfigCount > 1 || (g_skipUnfogged && g_fogOffCount > 0);
}

// Replay mode: force everything this draw emits to a flat config-ID colour (index + 1) * 24 in red,
// with no fog, no blending and no alpha test.
void stamp_replay_id(uint32_t index) {
    const auto idByte = static_cast<u8>((index + 1) * 24);
    GXSetColorUpdate(GX_TRUE);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetNumChans(1);
    GXSetChanCtrl(
        GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, GXColor{idByte, 0, 0, 255});
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
}

// Replay mode, one material draw: stamp its config ID, the no-fog sentinel, or (for the barrier)
// nothing.
void replay_stamp_material(J3DMaterial* material) {
    FogConfig config;
    const MaterialFog state = material_fog_state(material, config);
    if (state == MaterialFog::Live && is_barrier_fog(config)) {
        // Write no colour, so the config of whatever is behind the dome survives in the ID buffer.
        // stamp_replay_id turns colour writes back on for the next draw, and replay_config_ids
        // restores them at the end of the pass. This test must stay ahead of the fog-off mark.
        GXSetColorUpdate(GX_FALSE);
        return;
    }
    if (state == MaterialFog::Off && skip_unfogged_geometry() && material_owns_depth(material) &&
        material_alpha_test_trivial(material))
    {
        stamp_replay_id(kNoFogSlot);
        return;
    }
    stamp_replay_id(state == MaterialFog::Live ? lookup_frame_config(config) : 0u);
}

// Capture mode: update the Status counters, register the material's own fog and suppress it if it
// is being deferred. Returns whether the material carried live fog (for the shared-DL counter).
bool suppress_material_fog(J3DMaterial* material) {
    FogConfig config;
    const MaterialFog state = material_fog_state(material, config);
    if (material_over_unity_blend(material)) {
        ++g_overUnityCount;
        if (!material_owns_depth(material)) {
            ++g_noDepthOverUnityCount;
        }
    }
    if (state == MaterialFog::Off) {
        ++g_fogOffCount;
        if (!material_owns_depth(material)) {
            ++g_fogOffNoDepth;
        } else if (!material_alpha_test_trivial(material)) {
            ++g_fogOffAlphaTested;
        }
    }
    if (state != MaterialFog::Live) {
        return false;
    }
    if (is_barrier_fog(config)) {
        return true;  // leave the Ganon barrier on its own forward fog (see is_barrier_fog)
    }
    if (vote_config(config)) {
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
    }
    return true;
}

// Pre-hook on J3DShape::drawFast. The material's display list (fog included) has already been sent
// when it runs, so a GXSetFog here overrides the material's fog. Three roles: stamp the config ID
// during the replay; push the armed quad at the first draw after SCENE_AFTER_OPAQUE; and inside the
// scope, capture and suppress the material's fog.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
        replay_stamp_material(shape != nullptr ? shape->getMaterial() : nullptr);
        return HOOK_CONTINUE;
    }
    if (g_quadArmed) {
        g_quadArmed = false;
        g_quadAnchor = QuadAnchor::Translucent;
        push_fog_quad();
        return HOOK_CONTINUE;
    }
    if (!g_scopeActive) {
        return HOOK_CONTINUE;
    }
    const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
    suppress_material_fog(shape != nullptr ? shape->getMaterial() : nullptr);
    return HOOK_CONTINUE;
}

// dBgp_c map units (the shared, instanced pieces a stage is assembled from) bypass
// J3DShape::drawFast: dBgp_c::modelMaterial_c::drawSimple calls loadSharedDL() and then
// J3DShapeDraw::draw() directly. Their packet sets the room fog with dKy_GxFog_tevstr_set (caught
// by the GXSetFog hook) before its material loop, but each material's display list then re-issues
// J3DGDSetFog from the material's own fog block. Without these hooks that geometry would keep its
// forward fog under the quad (double fog) and would never be stamped in the replay.
//
// The post-hook on loadSharedDL lands between the display list and the shapes. All three material
// classes' overrides are hooked, since which class a model gets is J3DMaterialFactory's choice.
// Important: the hook must stay bracketed to drawSimple. Every other loadSharedDL caller (dMdl_c,
// dPa_modelEcallBack::model_c, the chain and hookshot shapes) sets its fog after the display list,
// so the material's own fog never renders there, and registering it would add a config vanilla
// never draws with (enough to make a uniform scene "mixed").
bool g_inBgpMaterial = false;
uint32_t g_sharedDlFogCount = 0;

// Fallback for pixels the replay could not stamp: the config the grass (dGrass_packet_c) and flower
// (dFlower_packet_c) packets used this frame. Those packets call their material display list, then
// their fog setter (grass: dKy_GfFog_tevstr_set, flowers: dKy_GxFog_tevstr_set), then emit raw GX
// batches, so the flat-ID override never reaches them; they rasterize lit colours, which fog.wgsl
// rejects as unstamped. Their pre/post hooks bracket the draw and on_set_fog_pre records the slot
// their setter resolved to. Other self-drawing packets (dMdl_c models, 3D lines) land on the same
// fallback. If the hooks fail it stays slot 0, normally the same room fog. (Picking the config with
// the widest endZ instead would give grass the weakest fog in the frame.)
bool g_inSelfDrawnPacket = false;
bool g_selfDrawnIndexValid = false;
uint32_t g_selfDrawnIndex = 0;

HookAction on_self_drawn_packet_pre(ModContext*, void*, void*, void*) {
    g_inSelfDrawnPacket = true;
    return HOOK_CONTINUE;
}

void on_self_drawn_packet_post(ModContext*, void*, void*, void*) {
    g_inSelfDrawnPacket = false;
}

HookAction on_bgp_draw_simple_pre(ModContext*, void*, void*, void*) {
    g_inBgpMaterial = true;
    return HOOK_CONTINUE;
}

void on_bgp_draw_simple_post(ModContext*, void*, void*, void*) {
    g_inBgpMaterial = false;
}

void on_material_shared_dl_post(ModContext*, void* args, void*, void*) {
    if (!g_inBgpMaterial) {
        return;
    }
    J3DMaterial* material = mods::arg<J3DMaterial*>(args, 0);
    if (g_fogReplayActive) {
        replay_stamp_material(material);
        return;
    }
    if (!g_scopeActive) {
        return;
    }
    if (suppress_material_fog(material)) {
        ++g_sharedDlFogCount;
    }
}

// Pre-hook on GXSetFog and GFSetFog: turns fog off during the replay, and inside the scope captures
// the config and suppresses it.
HookAction on_set_fog_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
        return HOOK_CONTINUE;
    }
    if (!g_scopeActive) {
        return HOOK_CONTINUE;
    }
    const auto type = mods::arg<GXFogType>(args, 0);
    if (type == GX_FOG_NONE) {
        return HOOK_CONTINUE;
    }
    FogConfig config;
    config.type = static_cast<uint8_t>(type);
    config.startZ = mods::arg<float>(args, 1);
    config.endZ = mods::arg<float>(args, 2);
    config.nearZ = mods::arg<float>(args, 3);
    config.farZ = mods::arg<float>(args, 4);
    config.color = mods::arg<GXColor>(args, 5);
    capture_adj_from_env(config);
    if (is_barrier_fog(config)) {
        return HOOK_CONTINUE;  // leave the Ganon barrier on its own forward fog (see is_barrier_fog)
    }
    const bool suppress = vote_config(config);
    // Inside a grass or flower packet: remember the slot this config resolved to, as the fallback
    // for unstamped pixels (see g_selfDrawnIndex). The first one in the frame wins.
    if (g_inSelfDrawnPacket && !g_selfDrawnIndexValid) {
        g_selfDrawnIndex = lookup_frame_config(config);
        g_selfDrawnIndexValid = true;
    }
    if (suppress) {
        mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
    }
    return HOOK_CONTINUE;
}

// Render worker: records the fullscreen fog triangle into the scene pass. Uses only the payload,
// the draw context and wgpu calls.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    // The pipelines do not exist until the first draw and are rebuilt when the pass changes shape,
    // so this must run before anything reads g_fogPipeline and the others.
    if (payloadSize != sizeof(DrawPayload) || ctx == nullptr || !ensure_fog_pipelines(*ctx)) {
        return;
    }
    DrawPayload data;
    std::memcpy(&data, payload, sizeof(data));

    const bool mixed = data.configIds != nullptr;
    WGPURenderPipeline pipeline = mixed
        ? (data.debug_mode != 0 ? g_mixedDebugPipeline : g_mixedPipeline)
        : (data.debug_mode != 0 ? g_fogDebugPipeline : g_fogPipeline);
    WGPUBindGroupLayout layout = mixed
        ? (data.debug_mode != 0 ? g_mixedDebugLayout : g_mixedLayout)
        : (data.debug_mode != 0 ? g_fogDebugLayout : g_fogLayout);
    if (data.sceneDepth == nullptr || pipeline == nullptr) {
        return;
    }

    WGPUBindGroupEntry entries[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = mixed ? 3 : 1;
    entries[1].buffer = ctx->uniform_buffer;
    entries[1].offset = data.uniform_offset;
    entries[1].size = data.uniform_size;
    if (mixed) {
        entries[2].binding = 2;
        entries[2].textureView = data.configIds;
    }
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = mixed ? 3 : 2;
    bindGroupDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    if (bindGroup == nullptr) {
        return;
    }

    wgpuRenderPassEncoderSetPipeline(ctx->pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

// Game thread, at the quad anchor: snapshots the scene depth and pushes the fog draw. Uses fs_mixed
// with the config-ID buffer when needs_id_buffer() and the replay succeeded, otherwise fs_main with
// the reference config.
void push_fog_quad() {
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = false;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr)
    {
        if (!g_warnedPushFailure) {
            g_warnedPushFailure = true;
            svc_log->warn(mod_ctx, "deferred fog: depth resolve failed; fog lost this frame");
        }
        return;
    }

    const auto debugMode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarFogDebug, 0), 0, 2));

    if (needs_id_buffer() && g_configIdView != nullptr) {
        MixedFogUniforms uniforms{};
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            const FogConfig& config = g_frameConfigs[i];
            MixedFogEntry& entry = uniforms.configs[i];
            dusk_fog::compute_fog_coefficients(
                config.startZ, config.endZ, config.nearZ, config.farZ, entry.a, entry.b, entry.c);
            if (entry.a == 0.0f && entry.c == 0.0f) {
                entry.fog_type = 2u;
            } else {
                entry.fog_type = config.type & 7u;
            }
            if (config.adj.enable) {
                entry.fog_type |= kFogTypeRangeAdjBit;
            }
            entry.color[0] = static_cast<float>(config.color.r) / 255.0f;
            entry.color[1] = static_cast<float>(config.color.g) / 255.0f;
            entry.color[2] = static_cast<float>(config.color.b) / 255.0f;
            entry.color[3] = 1.0f;
        }
        uniforms.count = g_frameConfigCount;
        uniforms.debug_mode = debugMode;
        // One range block for the whole frame, from the first config that has it enabled (see
        // FogRangeUniform). Configs with it off do not set kFogTypeRangeAdjBit.
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            if (g_frameConfigs[i].adj.enable) {
                uniforms.range = build_fog_range(g_frameConfigs[i].adj);
                break;
            }
        }
        // Unstamped pixels (grass, flowers and other self-drawing packets) take this config; see
        // g_selfDrawnIndex.
        uniforms.fallback_index = g_selfDrawnIndexValid ? g_selfDrawnIndex : 0;
        GfxRange uniformRange{0, 0};
        if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) !=
            MOD_OK)
        {
            return;
        }
        const DrawPayload payload{
            resolved.depth, g_configIdView, uniformRange.offset, uniformRange.size, debugMode};
        svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
        return;
    }

    FogUniforms uniforms{};
    dusk_fog::compute_fog_coefficients(g_reference.startZ, g_reference.endZ, g_reference.nearZ,
        g_reference.farZ, uniforms.a, uniforms.b, uniforms.c);
    if (uniforms.a == 0.0f && uniforms.c == 0.0f) {
        return;
    }
    uniforms.color[0] = static_cast<float>(g_reference.color.r) / 255.0f;
    uniforms.color[1] = static_cast<float>(g_reference.color.g) / 255.0f;
    uniforms.color[2] = static_cast<float>(g_reference.color.b) / 255.0f;
    uniforms.color[3] = 1.0f;
    uniforms.fog_type =
        (g_reference.type & 7u) | (g_reference.adj.enable ? kFogTypeRangeAdjBit : 0u);
    uniforms.range = build_fog_range(g_reference.adj);
    uniforms.debug_mode = debugMode > 1u ? 1u : debugMode;

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.depth, nullptr, uniformRange.offset, uniformRange.size,
        uniforms.debug_mode};
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
}

// The lists the replay re-draws: the opaque lists of the scope, without the Pri0_B particle passes
// and dComIfGd_drawShadow, which the game draws between them.
void draw_opaque_scene_lists() {
    dComIfGd_drawOpaListBG();
    dComIfGd_drawOpaListDarkBG();
    dComIfGd_drawOpaListMiddle();
    dComIfGd_drawOpaList();
    dComIfGd_drawOpaListDark();
    dComIfGd_drawOpaListPacket();
}

bool draw_lists_ready() {
    return dComIfGd_getOpaListBG() != nullptr && dComIfGd_getOpaList() != nullptr &&
           dComIfGd_getOpaListDark() != nullptr && dComIfGd_getXluListBG() != nullptr &&
           dComIfGd_getListPacket() != nullptr;
}

// Exact mode: re-draws the opaque lists into an offscreen pass with every draw forced to its flat
// config-ID colour (the drawFast and shared-DL hooks do the stamping while g_fogReplayActive), then
// resolves the colour as this frame's ID buffer. Restores the viewport, scissor and J3D state.
bool replay_config_ids(uint32_t width, uint32_t height) {
    f32 savedViewport[6];
    GXGetViewportv(savedViewport);
    u32 savedScissor[4];
    GXGetScissor(&savedScissor[0], &savedScissor[1], &savedScissor[2], &savedScissor[3]);
    const auto restore = [&]() {
        GXSetViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3],
            savedViewport[4], savedViewport[5]);
        GXSetScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
    };

    if (svc_gfx->create_pass(mod_ctx, width, height) != MOD_OK) {
        return false;
    }
    J3DShape::resetVcdVatCache();
    GXSetViewport(
        0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    GXSetViewportRender(
        0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, width, height);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

    J3DModel* savedModel = j3dSys.getModel();
    j3dSys.setModel(nullptr);
    g_fogReplayActive = true;
    draw_opaque_scene_lists();
    g_fogReplayActive = false;
    j3dSys.setModel(savedModel);
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    // The barrier switches colour writes off for its own geometry (see replay_stamp_material);
    // make sure the pass never ends with them off.
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    restore();

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = false;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr)
    {
        return false;
    }
    g_configIdView = resolved.color;
    return true;
}

// Diagnostic (fogLogConfigs, off by default): logs the frame's captured fog-config table whenever
// it changes.
ConfigVarHandle g_cvarFogLog = 0;
char g_lastFogLogSig[128] = "";

void log_fog_configs() {
    if (!get_bool_option(g_cvarFogLog, false)) {
        g_lastFogLogSig[0] = '\0';
        return;
    }
    const bool exact = exact_mode();
    // Signature over the table so we only log when it changes.
    char sig[128];
    int n = std::snprintf(sig, sizeof(sig), "%d|%u|%u|%.0f|%.0f|%d,%d,%d", exact ? 1 : 0,
        g_frameConfigCount, static_cast<unsigned>(g_reference.type), g_reference.startZ,
        g_reference.endZ, g_reference.color.r, g_reference.color.g, g_reference.color.b);
    for (uint32_t i = 0; i < g_frameConfigCount && n < static_cast<int>(sizeof(sig)); ++i) {
        n += std::snprintf(sig + n, sizeof(sig) - n, ";%.0f/%.0f", g_frameConfigs[i].startZ,
            g_frameConfigs[i].endZ);
    }
    if (std::strcmp(sig, g_lastFogLogSig) == 0) {
        return;
    }
    std::snprintf(g_lastFogLogSig, sizeof(g_lastFogLogSig), "%s", sig);

    char msg[200];
    std::snprintf(msg, sizeof(msg),
        "fog: mode=%s configs=%u  REF type=%u rgb(%u,%u,%u) start=%.0f end=%.0f near=%.1f far=%.0f",
        exact ? "exact" : "vanilla", g_frameConfigCount, static_cast<unsigned>(g_reference.type),
        static_cast<unsigned>(g_reference.color.r), static_cast<unsigned>(g_reference.color.g),
        static_cast<unsigned>(g_reference.color.b), g_reference.startZ, g_reference.endZ,
        g_reference.nearZ, g_reference.farZ);
    svc_log->info(mod_ctx, msg);
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        const FogConfig& c = g_frameConfigs[i];
        std::snprintf(msg, sizeof(msg),
            "  cfg %u: type=%u rgb(%u,%u,%u) start=%.0f end=%.0f near=%.1f far=%.0f", i,
            static_cast<unsigned>(c.type), static_cast<unsigned>(c.color.r),
            static_cast<unsigned>(c.color.g), static_cast<unsigned>(c.color.b), c.startZ, c.endZ,
            c.nearZ, c.farZ);
        svc_log->info(mod_ctx, msg);
    }
}

// SCENE_BEGIN (after the sky lists): resets per-frame state and opens the suppression scope unless
// the mod is disabled, the drawFast hook failed, or Wolf Senses is active.
void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = 0;
    g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_sharedDlFogCount = 0;
    g_fogOffCount = g_fogOffNoDepth = g_fogOffAlphaTested = 0;
    g_overUnityCount = g_noDepthOverUnityCount = 0;
    g_skipUnfogged = read_skip_unfogged();
    g_lastQuadAnchor = g_quadAnchor;
    g_quadAnchor = QuadAnchor::None;
    g_configIdView = nullptr;
    g_quadArmed = false;
    g_inBgpMaterial = false;
    g_inSelfDrawnPacket = false;
    g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_scopeActive = effect_enabled();
    // During Wolf Senses no scope opens at all; see wolf_senses_active().
    g_sensesExempt = g_scopeActive && wolf_senses_active() &&
                     !get_bool_option(g_cvarFogDeferInSenses, false);
    if (g_sensesExempt) {
        g_scopeActive = false;
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Wolf Senses: fog left to the game (black fog has nothing to defer)");
    }
    if (g_sensesExempt != g_wasSensesExempt) {
        svc_log->info(mod_ctx, g_sensesExempt
                                   ? "deferred fog: Wolf Senses active; the game's own fog is used"
                                   : "deferred fog: Wolf Senses over; deferring again");
        g_wasSensesExempt = g_sensesExempt;
    }
    if (!g_scopeActive) {
        g_suppressAllowed = false;
        g_lastFrameDeferred = false;
    }
}

// SCENE_AFTER_OPAQUE: closes the scope, arms the quad, runs the config-ID replay when this frame
// needs it, decides g_suppressAllowed for the next frame, logs mode transitions and builds the
// Status line.
void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (!g_scopeActive) {
        return;
    }
    g_scopeActive = false;
    // Sample the world viewport while it is still current; by the time a fallback anchor fires the
    // game has moved on to other viewports. See g_frameViewportWidth.
    f32 viewport[6];
    GXGetViewportv(viewport);
    if (viewport[2] > 1.0f) {
        g_frameViewportWidth = viewport[2];
    }
    const bool exact = exact_mode();
    g_quadArmed = (g_suppressedCount > 0 || (exact && g_frameConfigCount > 0)) &&
                  g_reference.valid;
    g_lastFrameDeferred = g_quadArmed;
    g_suppressAllowed = exact ? effect_enabled() : (g_deviantCount == 0 && effect_enabled());

    if (needs_id_buffer() && g_quadArmed) {
        bool ok = false;
        if (draw_lists_ready()) {
            GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
            resolveDesc.color = false;
            resolveDesc.depth = true;
            GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
            if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) == MOD_OK &&
                resolved.depth != nullptr && resolved.width > 0 && resolved.height > 0)
            {
                ok = replay_config_ids(resolved.width, resolved.height);
            }
        }
        if (!ok) {
            g_configIdView = nullptr;
            if (!g_warnedReplayFailure) {
                g_warnedReplayFailure = true;
                svc_log->warn(mod_ctx,
                    "deferred fog: config-ID replay failed; mixed frames fall back to the "
                    "reference config");
            }
        }
    }
    if (exact) {
        const bool mixed = g_frameConfigCount > 1;
        if (mixed != g_wasMixed) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                mixed ? "deferred fog: scene went mixed (%u configs); per-pixel ID replay active"
                      : "deferred fog: scene uniform again (%u config)",
                g_frameConfigCount);
            svc_log->info(mod_ctx, msg);
            g_wasMixed = mixed;
        }
    }

    if (!exact && g_wasSuppressing && !g_suppressAllowed && effect_enabled()) {
        char msg[240];
        std::snprintf(msg, sizeof(msg),
            "deferred fog REVERTED to vanilla: mixed fog configs (%u matching, %u deviant); "
            "reference type %u range %.0f..%.0f rgb(%u,%u,%u) vs deviant type %u range "
            "%.0f..%.0f rgb(%u,%u,%u). Screen-space AO/shadows will darken the fog until the "
            "scene is uniform again.",
            g_suppressedCount, g_deviantCount, static_cast<unsigned>(g_reference.type),
            g_reference.startZ, g_reference.endZ, static_cast<unsigned>(g_reference.color.r),
            static_cast<unsigned>(g_reference.color.g), static_cast<unsigned>(g_reference.color.b),
            static_cast<unsigned>(g_firstDeviant.type), g_firstDeviant.startZ,
            g_firstDeviant.endZ, static_cast<unsigned>(g_firstDeviant.color.r),
            static_cast<unsigned>(g_firstDeviant.color.g),
            static_cast<unsigned>(g_firstDeviant.color.b));
        svc_log->warn(mod_ctx, msg);
    } else if (!g_wasSuppressing && g_suppressAllowed) {
        svc_log->info(mod_ctx, "deferred fog engaged (uniform fog configuration)");
    }
    g_wasSuppressing = g_suppressAllowed;

    if (!effect_enabled()) {
        std::snprintf(g_statusText, sizeof(g_statusText), "Disabled");
    } else if (!g_reference.valid) {
        std::snprintf(g_statusText, sizeof(g_statusText), "No fogged draws this frame");
    } else if (exact) {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Deferring fog (exact: %u draws, %u config%s%s; %u shared-DL, %u fog-off "
            "(%u markable/%u no-Z/%u alpha), %u additive/%u no-Z) [%s]",
            g_suppressedCount + g_deviantCount, g_frameConfigCount,
            g_frameConfigCount == 1 ? "" : "s",
            needs_id_buffer() && g_configIdView == nullptr ? ", replay failed" : "",
            g_sharedDlFogCount, g_fogOffCount, fog_off_markable(), g_fogOffNoDepth,
            g_fogOffAlphaTested, g_overUnityCount, g_noDepthOverUnityCount, quad_anchor_name());
    } else if (g_deviantCount > 0) {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "REVERTED: mixed fog configs (%u matching / %u deviant)", g_suppressedCount,
            g_deviantCount);
    } else {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Deferring fog (%u draws; %u shared-DL, %u fog-off (%u markable/%u no-Z/%u alpha), "
            "%u additive/%u no-Z) [%s]",
            g_suppressedCount, g_sharedDlFogCount, g_fogOffCount, fog_off_markable(),
            g_fogOffNoDepth, g_fogOffAlphaTested, g_overUnityCount, g_noDepthOverUnityCount,
            quad_anchor_name());
    }

    log_fog_configs();
}

// Pre-hook on mDoGph_gInf_c::bloom_c::draw, before the game's bloom reads the frame. Pushes the
// quad only if no translucent draw has done so this frame; see QuadAnchor.
HookAction on_bloom_draw_pre(ModContext*, void*, void*, void*) {
    if (g_quadArmed) {
        g_quadArmed = false;
        g_quadAnchor = QuadAnchor::BeforeBloom;
        push_fog_quad();
    }
    return HOOK_CONTINUE;
}

// Last-resort anchor, after bloom; see QuadAnchor.
void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    if (!g_quadArmed) {
        return;
    }
    g_quadArmed = false;
    g_quadAnchor = QuadAnchor::FrameEnd;
    push_fog_quad();
}

void add_control(UiElementHandle panel, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, panel, &desc, nullptr);
}

void status_get(ModContext*, void*, UiControlValue* outValue) {
    outValue->string_value = g_statusText;
}
void status_set(ModContext*, void*, const UiControlValue*) {}
bool status_disabled(ModContext*, void*) {
    return true;
}

void add_enabled_toggle(UiElementHandle pane) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.help_rml =
        "Applies the game's fog after other mods' screen-space effects (AO, shadows) instead "
        "of during world drawing, so those effects darken the surfaces under the fog rather "
        "than the fog itself.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogEnabled;
    add_control(pane, control);
}

void add_status_line(UiElementHandle pane) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = "Status";
    control.help_rml =
        "Live suppression state. \"Deferring fog\" is the working state (exact mode also shows "
        "the frame's config count). \"REVERTED: mixed fog configs\" (Vanilla mode) means this "
        "scene draws with several fog configurations and fell back to forward fog - AO/shadow "
        "darkening will then show on top of the fog at range. Transitions are logged with the "
        "config details.";
    control.binding = UI_BINDING_CALLBACKS;
    control.get = status_get;
    control.set = status_set;
    control.is_disabled = status_disabled;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;
    add_enabled_toggle(left);

    static const char* kMixedOptions[] = {"Vanilla", "Exact (replay)"};
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Mixed Scenes";
    control.help_rml =
        "How scenes that draw with several fog configurations are handled.<br/><b>Vanilla</b>: in a "
        "multi-config scene, revert that scene to the game's own forward fog - exactly vanilla - "
        "while still deferring in the common single-config scenes. Safe, but gives up the "
        "AO-under-fog benefit in most outdoor scenes, which mix configs.<br/>"
        "<b>Exact (replay)</b> (default): always defer, replaying the opaque geometry into a "
        "per-pixel config-ID buffer so each pixel gets the fog its own draw used. Costs one extra "
        "opaque geometry pass on mixed frames. Pixels the replay cannot label, such as grass and "
        "flowers (which draw their own geometry), take the fog the grass and flower packets set.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogMixed;
    control.options = kMixedOptions;
    control.option_count = 2;
    add_control(left, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Skip Unfogged Geometry (experimental)";
    // The help text says plainly that this did not fix the distant-landmark case it was written
    // for, so players with that symptom are not sent to it (docs/deferred_fog.md, Known issues).
    control.help_rml =
        "Some surfaces are drawn by the game with fog switched off entirely, so they stay at full "
        "brightness however far away they are. The deferred pass has no way to know that on its "
        "own and fogs them like everything else. With this on, those surfaces are marked in the "
        "per-pixel buffer and the fog pass leaves them alone."
        "<br/><b>This did not fix the known case.</b> It was written for the distant-landmark "
        "problem (Death Mountain looking washed out compared to the mod being off) and tested "
        "there, and it made no difference - so if that is your symptom, this is probably not it. "
        "Left in because the effect it describes is real and it doubles as a diagnostic."
        "<br/><b>Check the Status line first.</b> If its <i>fog-off</i> count is 0 in the view you "
        "care about, this will do nothing at all. It also forces the per-pixel replay - one extra "
        "pass over the world's geometry - in scenes that did not need it, so there is a framerate "
        "cost. Requires Mixed Scenes = Exact.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogSkipUnfogged;
    add_control(left, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Defer Fog During Wolf Senses (diagnostic)";
    control.help_rml =
        "Off by default, and should stay off for play. While Wolf Link's senses are active the game "
        "switches its fog to a short black fog, and black fog gains nothing from being deferred - "
        "so the mod normally steps aside and lets the game draw it. Turning this on makes the mod "
        "defer it anyway, which brings back the known senses bug (some camera directions show far "
        "more of the world than the senses view allows). It exists only so that bug can be "
        "examined with the Debug View.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogDeferInSenses;
    add_control(left, control);

    static const char* kDebugOptions[] = {"Off", "Fog Factor", "Config IDs"};
    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Debug View";
    control.help_rml =
        "Fog Factor: the deferred fog term as grayscale (white = full fog).<br/>Config IDs: "
        "on mixed frames in exact mode, which captured fog configuration each pixel resolved "
        "to (one gray band per config); falls back to Fog Factor on uniform frames.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogDebug;
    control.options = kDebugOptions;
    control.option_count = 3;
    add_control(left, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Log Fog Configs";
    control.help_rml =
        "Diagnostic: prints the frame's captured fog configuration table to the log whenever it "
        "changes - the number of distinct fog configs and each one's type, color, and start/end/"
        "near/far range. Use it to see what fog a spot actually uses (e.g. is a distant subject one "
        "config or several).";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogLog;
    add_control(left, control);
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Deferred Fog";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open Deferred Fog controls window");
    }
}

// The mod's section in the shared mods panel: Enabled, Status, and a button for the full controls
// window.
void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Deferred Fog");
    add_enabled_toggle(panel);
    add_status_line(panel);

    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Fog Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
}

// Builds one fullscreen fog pipeline for `entryPoint`. blend = true mixes the fog colour in by
// fogZ; blend = false is the debug variant, which writes the shader output directly.
bool build_fog_pipeline(const gfx_compat::ScenePassLayout& sceneLayout, bool blend,
    const char* entryPoint, WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"deferred fog", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }

    WGPUBlendState blendState{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_SrcAlpha,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    // `sceneLayout` is the pass this draw is recorded into, taken from the live GfxDrawContext by
    // ensure_fog_pipelines (never derived from GfxDeviceInfo). Attachments the mod does not own,
    // such as the normal buffer, come back write-masked off, so the fog leaves normals untouched.
    gfx_compat::ScenePassLayout layout = sceneLayout;
    if (blend) {
        layout.color_targets[0].blend = &blendState;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {entryPoint, WGPU_STRLEN};
    fragment.targetCount = layout.color_target_count;
    fragment.targets = layout.color_targets;
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = layout.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {blend ? "deferred fog" : "deferred fog (debug)", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = &depthStencil;
    pipelineDesc.multisample.count = layout.sample_count;
    pipelineDesc.fragment = &fragment;
    outPipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (outPipeline == nullptr) {
        return false;
    }
    outLayout = wgpuRenderPipelineGetBindGroupLayout(outPipeline, 0);
    return outLayout != nullptr;
}

void release_fog_pipelines() {
    const auto releasePipeline = [](WGPURenderPipeline& pipeline) {
        if (pipeline != nullptr) {
            wgpuRenderPipelineRelease(pipeline);
            pipeline = nullptr;
        }
    };
    const auto releaseLayout = [](WGPUBindGroupLayout& layout) {
        if (layout != nullptr) {
            wgpuBindGroupLayoutRelease(layout);
            layout = nullptr;
        }
    };
    releasePipeline(g_fogPipeline);
    releasePipeline(g_fogDebugPipeline);
    releasePipeline(g_mixedPipeline);
    releasePipeline(g_mixedDebugPipeline);
    releaseLayout(g_fogLayout);
    releaseLayout(g_fogDebugLayout);
    releaseLayout(g_mixedLayout);
    releaseLayout(g_mixedDebugLayout);
    g_sceneLayoutValid = false;
    g_sceneLayoutKey = 0;
}

/// Builds all four pipelines for the pass this draw is recorded into, rebuilding them when its
/// layout key changes. Runs on the render worker from on_draw. Either all four exist for the
/// current key or none do, so the caller only has to check the one it wants.
bool ensure_fog_pipelines(const GfxDrawContext& ctx) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (g_sceneLayoutValid && g_sceneLayoutKey == key && g_fogPipeline != nullptr &&
        g_fogDebugPipeline != nullptr && g_mixedPipeline != nullptr &&
        g_mixedDebugPipeline != nullptr)
    {
        return true;
    }
    release_fog_pipelines();
    gfx_compat::ScenePassLayout layout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, layout)) {
        return false;
    }
    if (!build_fog_pipeline(layout, true, "fs_main", g_fogPipeline, g_fogLayout) ||
        !build_fog_pipeline(layout, false, "fs_main", g_fogDebugPipeline, g_fogDebugLayout) ||
        !build_fog_pipeline(layout, true, "fs_mixed", g_mixedPipeline, g_mixedLayout) ||
        !build_fog_pipeline(layout, false, "fs_mixed", g_mixedDebugPipeline, g_mixedDebugLayout))
    {
        release_fog_pipelines();
        return false;
    }
    g_sceneLayoutKey = key;
    g_sceneLayoutValid = true;
    return true;
}

ModResult init(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "fog.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return mods::set_error(error, result, "failed to load fog.wgsl");
    }

    // DEFAULT: deferred fog enabled.
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogEnabled";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogEnabled) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: mixed-scene mode = Exact replay (1); 0 = Vanilla revert. Exact keeps deferring in
    // scenes that mix configs, which most outdoor scenes do. Keep exact_mode()'s fallback equal to
    // this value.
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogMixedMode";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 1;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogMixed) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: off (it can force the config-ID replay; see g_skipUnfogged).
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogSkipUnfogged";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = false;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogSkipUnfogged) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: off. Diagnostic that disables the Wolf Senses exemption; see wolf_senses_active().
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogDeferInSenses";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = false;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogDeferInSenses) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: fog debug view off (0).
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogDebug";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 0;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogDebug) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: fog-config diagnostic logging off.
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogLogConfigs";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = false;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogLog) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    // The pipelines are not built here: ensure_fog_pipelines() builds them on the first draw and
    // rebuilds them when the scene pass changes shape (see g_sceneLayoutKey).

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "deferred fog";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register draw type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_sceneAfterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    if (mods::hook_add_pre<SetFog>(svc_hook, on_set_fog_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook GXSetFog");
    }
    if (mods::hook_add_pre<SetGfFog>(svc_hook, on_set_fog_pre) != MOD_OK) {
        svc_log->warn(mod_ctx,
            "failed to hook GFSetFog; grass/flower fog will not be deferred (double-fogged)");
    }
    g_shapeHookOk =
        mods::hook_add_pre<ShapeDrawFast>(svc_hook, on_shape_draw_pre) == MOD_OK;
    if (!g_shapeHookOk) {
        svc_log->warn(mod_ctx,
            "failed to hook J3DShape::drawFast (missing dusklight.symdb?); deferred fog is "
            "disabled");
    }
    const bool sharedDlOk =
        (mods::hook_add_pre<BgpDrawSimple>(svc_hook, on_bgp_draw_simple_pre) == MOD_OK) &
        (mods::hook_add_post<BgpDrawSimple>(svc_hook, on_bgp_draw_simple_post) == MOD_OK) &
        (mods::hook_add_post<MaterialSharedDL>(svc_hook, on_material_shared_dl_post) == MOD_OK) &
        (mods::hook_add_post<PatchedMaterialSharedDL>(svc_hook, on_material_shared_dl_post) ==
            MOD_OK) &
        (mods::hook_add_post<LockedMaterialSharedDL>(svc_hook, on_material_shared_dl_post) ==
            MOD_OK);
    // Bracket the grass and flower packets for the unstamped-pixel fallback (see g_selfDrawnIndex).
    // Without them the fallback is the reference config, so failure only warns.
    const bool selfDrawnOk =
        (mods::hook_add_pre<GrassPacketDraw>(svc_hook, on_self_drawn_packet_pre) == MOD_OK) &
        (mods::hook_add_post<GrassPacketDraw>(svc_hook, on_self_drawn_packet_post) == MOD_OK) &
        (mods::hook_add_pre<FlowerPacketDraw>(svc_hook, on_self_drawn_packet_pre) == MOD_OK) &
        (mods::hook_add_post<FlowerPacketDraw>(svc_hook, on_self_drawn_packet_post) == MOD_OK);
    if (!selfDrawnOk) {
        svc_log->warn(mod_ctx,
            "failed to hook the grass/flower packet draws; in exact mode their pixels fall back "
            "to the frame's reference fog config instead of the one they actually drew with");
    }
    if (mods::hook_add_pre<BloomDraw>(svc_hook, on_bloom_draw_pre) != MOD_OK) {
        svc_log->warn(mod_ctx,
            "failed to hook the bloom draw; frames with no translucent J3D geometry will apply "
            "their fog after the game's bloom and post-processing instead of before it");
    }
    if (!sharedDlOk) {
        svc_log->warn(mod_ctx,
            "failed to hook the dBgp_c shared-display-list path (drawSimple / loadSharedDL); "
            "map-unit scenery whose material carries its own fog may keep that forward fog "
            "under the deferred quad (double fog at range)");
    }
    return MOD_OK;
}

void shutdown() {
    svc_resource->free(mod_ctx, &g_shaderSource);
    release_fog_pipelines();
    g_cvarFogEnabled = g_cvarFogMixed = g_cvarFogDebug = g_cvarFogLog = 0;
    g_cvarFogSkipUnfogged = g_cvarFogDeferInSenses = 0;
    g_sensesExempt = g_wasSensesExempt = false;
    g_lastFogLogSig[0] = '\0';
    g_controlsWindow = 0;
    g_drawType = g_sceneBeginHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_scopeActive = g_quadArmed = g_suppressAllowed = g_shapeHookOk = g_wasSuppressing = false;
    g_quadAnchor = g_lastQuadAnchor = QuadAnchor::None;
    g_lastFrameDeferred = false;
    g_fogReplayActive = g_wasMixed = g_warnedReplayFailure = false;
    g_inBgpMaterial = g_inSelfDrawnPacket = g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_sharedDlFogCount = 0;
    g_fogOffCount = g_fogOffNoDepth = g_fogOffAlphaTested = 0;
    g_overUnityCount = g_noDepthOverUnityCount = 0;
    g_skipUnfogged = false;
    g_configIdView = nullptr;
    std::snprintf(g_statusText, sizeof(g_statusText), "Waiting for first fogged frame");
}

}  // namespace

namespace {

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    build_section(panel);
    return MOD_OK;
}

}  // namespace

// The exported service: whether this frame's fog is being deferred. See
// include/deferred_fog_service.h.
namespace {
ModResult service_get_state(ModContext*, DeferredFogState* outState) {
    if (outState == nullptr || outState->struct_size < sizeof(DeferredFogState)) {
        return MOD_INVALID_ARGUMENT;
    }
    const uint32_t structSize = outState->struct_size;
    *outState = DeferredFogState DEFERRED_FOG_STATE_INIT;
    outState->struct_size = structSize;
    outState->deferring = g_lastFrameDeferred;
    return MOD_OK;
}
}  // namespace

constexpr DeferredFogService g_fogService{
    .header = SERVICE_HEADER(
        DeferredFogService, DEFERRED_FOG_SERVICE_MAJOR, DEFERRED_FOG_SERVICE_MINOR),
    .get_state = service_get_state,
};
EXPORT_SERVICE(g_fogService);

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    const ModResult result = init(error);
    if (result != MOD_OK) {
        return result;
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "deferred_fog ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) { return MOD_OK; }

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    shutdown();
    return MOD_OK;
}
}
