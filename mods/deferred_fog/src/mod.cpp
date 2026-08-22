// Deferred Fog — moves the game's fog to AFTER the opaque scene, so screen-space effects darken
// the world UNDER the fog instead of darkening the fog itself.
//
// The game applies fog per draw, inside the opaque lists. Anything a mod composites afterwards
// (ambient occlusion, GI, shadows) therefore multiplies over pixels that are ALREADY fogged, and
// distant geometry gets its AO applied to the fog colour — the effect reads as grime on the haze
// rather than shading on the world. This mod suppresses the per-draw fog during the opaque world
// lists and re-applies it as a single fullscreen pass after every mod's SCENE_AFTER_OPAQUE
// composite, using aurora's own fog math bit-exactly (see src/fog_math.h), so the result is the
// game's fog over an already-shaded world.
//
// It was previously a sub-feature of the combined "Graphics Hub" mod. Graphics Hub is retired: its
// other half (Depth to Normal) reconstructed a surface normal from depth and published it as a mod
// service, which GfxService 1.3's get_scene_normals supersedes entirely — the game now hands mods
// the artist's authored normal directly, so there is nothing left for a provider mod to do. What
// remains is this, which is a genuine game-behaviour change and can only live in a game-linked mod.
//
// No mod imports this one. Ordering is guaranteed by STAGE SEPARATION, not by load order: a
// composite at SCENE_AFTER_OPAQUE is already ahead of a fog quad pushed at FRAME_BEFORE_HUD, and
// anything that must land ON TOP of the fog draws at FRAME_AFTER_HUD (which is what VBAO's debug
// views do). The exported service is still here so a consumer can read whether this frame actually
// deferred; see include/deferred_fog_service.h.
//
// Game-linked (hooks game fog functions) + webgpu.

#include "global.h"

#include "fog_math.h"

#include "deferred_fog_service.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_flower.h"
#include "d/actor/d_grass.h"
#include "d/d_bg_parts.h"
#include "d/d_com_inf_game.h"
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

// Hook targets (each emits a modmeta hook record the host resolves at load).
DEFINE_HOOK(GXSetFog, SetFog);
DEFINE_HOOK(GFSetFog, SetGfFog);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);
// The shared-display-list draw path, which bypasses drawFast entirely — see
// on_material_shared_dl_post. drawSimple brackets the window; one loadSharedDL hook per material
// class the loader can produce.
DEFINE_HOOK(&dBgp_c::modelMaterial_c::drawSimple, BgpDrawSimple);
// The self-drawing opaque packets — see g_selfDrawnIndex.
DEFINE_HOOK(&dGrass_packet_c::draw, GrassPacketDraw);
DEFINE_HOOK(&dFlower_packet_c::draw, FlowerPacketDraw);
DEFINE_HOOK(&J3DMaterial::loadSharedDL, MaterialSharedDL);
DEFINE_HOOK(&J3DPatchedMaterial::loadSharedDL, PatchedMaterialSharedDL);
DEFINE_HOOK(&J3DLockedMaterial::loadSharedDL, LockedMaterialSharedDL);

ConfigVarHandle g_cvarFogEnabled = 0;    // DEFAULT below in init()
ConfigVarHandle g_cvarFogMixed = 0;      // DEFAULT below in init()
ConfigVarHandle g_cvarFogDebug = 0;      // DEFAULT below in init()
ConfigVarHandle g_cvarFogBlended = 0;    // DEFAULT below in init()

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

// GX FOG RANGE ADJUSTMENT — the game's own name for it is XFog.
//
// GX fog is computed from the fragment's Z, but Z is the distance along the view AXIS, not to the
// eye: a pixel at the left or right edge of the screen is genuinely further away than a pixel of
// the same Z at the centre. The hardware corrects for that with a per-column multiplier applied to
// the fog term BEFORE the start-Z bias is subtracted, driven by a 10-entry table plus a centre
// column (GXSetFogRangeAdj).
//
// TP has it ON EVERYWHERE. envcolor_init turns it on at environment init and nothing ever clears it
// (d_kankyo.cpp:1257-1260: mFogAdjEnable = true, mFogAdjTableType = 0, mFogAdjCenter = 0x140), and
// every path that sets fog re-arms it: the three direct setters (dKy_GxFog_set,
// dKy_GxFog_tevstr_set, dKy_GfFog_tevstr_set) each call GxXFog_set() immediately afterwards
// (:9416/:9438/:9460), and every BG material's J3DFog block is stamped with the same globals
// (:4481-4484) so J3DFog::load() re-issues it per material. "XFog" is the authored abbreviation —
// x for the screen x axis the table is indexed by.
//
// Aurora implements it: build_fog_range_lut bakes one multiplier per target column and the
// generated fragment shader applies `fogBase *= lut[u32(in.pos.x)]` right before `- c`
// (command_processor.cpp:201-220, shader.cpp:1550-1553). So it is part of what vanilla renders,
// and a deferred pass that omits it flattens a horizontal gradient the game has. The error is
// (lut - 1) * (c + fogF): ~3% of the fog term at the screen edges, which is a fraction of a
// percentage point for near fog but reaches double digits for the narrow, far-STARTING bands that
// distant haze uses (c = startZ/(endZ - startZ) is large there) — i.e. exactly the wide vistas
// where the difference was noticed. The mod reproduces the multiplier analytically in fog.wgsl
// rather than baking a LUT; see fog_range_factor there.
//
// (docs/deferred_fog.md used to state that aurora ignores range adjustment and that the mod
// therefore correctly ignored it too. That was false on this pin and is why nobody looked.)
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

bool g_scopeActive = false;
bool g_quadArmed = false;
// Published through the service: did this frame actually defer, or did it fall back to the game's
// own forward fog? Written on the game thread once per frame, read by consumers on the same thread.
bool g_lastFrameDeferred = false;
bool g_suppressAllowed = false;
bool g_shapeHookOk = false;
bool g_warnedPushFailure = false;
FogConfig g_reference;
uint32_t g_suppressedCount = 0;
uint32_t g_deviantCount = 0;

bool g_wasSuppressing = false;
FogConfig g_firstDeviant;
char g_statusText[160] = "Waiting for first fogged frame";

constexpr uint32_t kMaxFogConfigs = 8;
FogConfig g_frameConfigs[kMaxFogConfigs];
uint32_t g_frameConfigCount = 0;
bool g_fogReplayActive = false;
WGPUTextureView g_configIdView = nullptr;
bool g_wasMixed = false;
bool g_warnedReplayFailure = false;

// Bit set in the shader-side fog_type field when the range-adjust multiplier applies to that
// config. GXFogType's own bit 3 (0x08) means ORTHOGRAPHIC, and the shader masks the type to three
// bits, so this uses the next free bit up rather than colliding with a real GX meaning.
constexpr uint32_t kFogTypeRangeAdjBit = 0x10u;

// Mirror of the WGSL FogRange struct. One per frame, shared by every config: the game drives the
// table, the centre and the enable from the same g_env_light globals, so the configs in a frame
// cannot disagree about them (only about whether they are enabled at all, which rides in fog_type).
struct FogRangeUniform {
    float center;   // fog-range centre column, in NDC x
    float _pad0;
    float _pad1;
    float _pad2;
    float k[12];    // aurora's 10 range constants (pair-swapped, /64); 10 and 11 replicate 9
};
static_assert(sizeof(FogRangeUniform) == 64);

// Mirror of the WGSL FogUniforms struct (keep in sync with res/fog.wgsl).
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

// See material_blends(). Exposed as a toggle because it changes which draws the mod touches at all;
// turning it off restores the previous behaviour for A/B comparison. Sampled once per frame in
// on_scene_begin — keeps_forward_fog() runs per draw and must not hit the config service there.
bool g_blendedStayVanilla = true;

bool read_blended_draws_stay_vanilla() {
    return get_bool_option(g_cvarFogBlended, true);
}

// The frame's world viewport width in the game's logical (640-wide) space, sampled once per frame
// while the world viewport is still current. Only the width is needed: aurora's centre term is
// ((center - vp.left) / vp.width) * 2 - 1 + (renderVp.left / renderVp.width) * 2, and the render
// viewport is just the logical one uniformly scaled (gx.cpp map_logical_viewport), so the two
// vp.left terms cancel exactly and leave 2 * center / vp.width - 1.
float g_frameViewportWidth = 640.0f;

// Per-material range adjustment: dKy stamps every BG material's J3DFog block with the environment
// globals (d_kankyo.cpp:4481-4484) and J3DFog::load() re-issues GXSetFogRangeAdj from it
// (J3DMatBlock.h:1526), so the material's own block is authoritative for the shapes it draws.
void capture_adj_from_material(const J3DFog& fog, FogConfig& out) {
    out.adj.enable = fog.mAdjEnable != 0;
    out.adj.center = fog.mCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.adj.table[i] = fog.mFogAdjTable.r[i];
    }
}

// Direct-setter range adjustment: dKy_GxFog_set, dKy_GxFog_tevstr_set and dKy_GfFog_tevstr_set each
// call GxXFog_set() immediately after their GXSetFog/GFSetFog, and that re-issues
// GXSetFogRangeAdj straight from these globals — so for a config captured at the setter, the live
// environment state IS the range adjustment that will be in force for it.
void capture_adj_from_env(FogConfig& out) {
    out.adj.enable = g_env_light.mFogAdjEnable != 0;
    out.adj.center = g_env_light.mFogAdjCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.adj.table[i] = g_env_light.mXFogTbl.r[i];
    }
}

// Reproduces aurora's build_fog_range_lut (command_processor.cpp:201-220) as shader uniforms: the
// same constants, in the same pair-swapped order, at the same 1/64 scale, with the centre column
// mapped to NDC. fog.wgsl then evaluates sqrt(offset^2 + k^2) / k per pixel instead of reading a
// baked per-column table, which is the same function of the same inputs.
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

bool config_matches(const FogConfig& reference, const FogConfig& candidate) {
    // Two draws that agree on colour and range but disagree on range adjustment do NOT render the
    // same, so they are distinct configs. In practice every config in a frame carries the same
    // globals and this never splits the table; it matters only for a material whose fog block dKy
    // never touched, which keeps whatever the .bmd authored.
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

// The Hyrule Castle "Ganon barrier" (game actor d_a_obj_ganonwall2) is a translucent dome, but it
// draws in the OPAQUE BG list (its Draw() calls dComIfGd_setListBG()), so it lands inside the
// suppression scope. Every frame that actor rewrites its material fog to pure BLACK with a huge
// range (startZ 1000, endZ 250000) so the dome fades to black at distance. If we defer that config,
// two things break: the config-ID replay rasterizes the (really translucent) dome SOLID and stamps
// its black fog onto the castle and trees INSIDE it (they turn dark), and the barrier's own
// fog-then-blend compositing is lost. So we recognise this one distinctive signature and leave the
// barrier entirely on its vanilla forward fog: its shapes are never suppressed and never registered
// as a frame config. The frame then stays uniform (the field fog), the geometry inside the barrier
// keeps the correct field fog, and the dome keeps its own black forward fog. (Residual: the
// fullscreen quad still adds the field fog over the dome pixels — minor, and far better than the
// black-stamped geometry it replaces. A perfect result is impossible here: a single fullscreen fog
// pass cannot reproduce per-fragment fog through a translucent surface.)
//
// The Ganon barrier dome writes these three literals every frame, in both barrier actors:
// d_a_obj_ganonwall2.cpp:112-116 and d_a_obj_ganonwall.cpp:131-135 set colour 0,0,0 with
// mStartZ = 1000.0f and mEndZ = 250000.0f. Matching the exact triple is free and is the only
// safe test.
//
// THIS USED TO BE `black && endZ > 100000`, WHICH MATCHED A WHOLE MATERIAL CLASS. `mType = 7` is
// the GAME'S OWN black-fog sentinel, not a barrier marker: dKy_bg_MAxx_proc stamps it on the
// terrain water family MA03/MA17/MA19 (d_kankyo.cpp:11390) and on MA20 (:11588), and
// setLightTevColorType_MAJI_sub reads it and forces the fog COLOUR to pure black (:4466-4470)
// while the start/end Z stay the room palette's. So in any room whose palette fog_end_z exceeds
// 100000, every water surface and every MA20 material looked like the barrier: the mod left them
// on vanilla forward fog AND painted the quad over them, i.e. double fog. Palette fog is
// interpolated by float_kankyo_color_ratio_set and cannot land on both literals, so the exact test
// cannot collide with it.
bool is_barrier_fog(const FogConfig& c) {
    return c.color.r == 0 && c.color.g == 0 && c.color.b == 0 && c.startZ == 1000.0f &&
        c.endZ == 250000.0f;
}

// (A "widest fog" helper used to live here, ranking the frame's configs by reach so uncovered
// pixels and the barrier dome could be given "the distant fog". It is gone. There is no distant
// fog to find: every config in a frame carries the same near/far from the one live view
// (d_kankyo.cpp:4461-4463 for BG materials, :9394/:9429/:9451 for the direct setters), the world
// lists draw under one perspective projection (m_Do_graphic.cpp:2338), and what TP widens for
// distant scenery is the CPU clipper (d_a_bg.cpp:298, d_bg_parts.cpp:681), which never touches
// fog. Both of its callers now have exact answers instead of a ranking: uncovered pixels take
// g_selfDrawnIndex, and blended draws take the config of whatever is behind them.)

// The fallback here MUST be the value fogMixedMode is registered with (1 = Exact). The two
// disagreed once already, which meant a failed config read silently ran the mode the UI was not
// showing; keep them in step.
bool exact_mode() {
    return get_int_option(g_cvarFogMixed, 1) == 1;
}

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

uint32_t lookup_frame_config(const FogConfig& config) {
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        if (config_matches(g_frameConfigs[i], config)) {
            return i;
        }
    }
    return 0;
}

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

// TP DRAWS TRANSLUCENT SURFACES INSIDE THE OPAQUE LISTS, AND THEIR FOG CANNOT BE DEFERRED AT ALL.
//
// GX fog is per fragment and runs BEFORE the blend: the hardware computes mix(src, fogColour, f)
// and blends THAT over the framebuffer. A fullscreen pass can only do the outer operation,
// mix(blend(src, dst), fogColour, f). The two agree only when the draw REPLACES what is under it.
// For a blended draw they are different operations, and no amount of tuning makes them equal:
//
//   * an ADDITIVE pass fogged toward a bright fog colour gets BRIGHTER with distance, because the
//     fog colour is what gets added. Deferring can only pull the finished pixel toward the fog
//     colour, so the same surface comes out dimmer — the terrain vocabulary even names this class
//     of material, `_Kasan` (加算, "addition"); see docs/japanese-naming.md;
//   * a surface fogged toward BLACK fades OUT with distance. Deferring instead paints the room's
//     pale fog over the surface AND over everything visible through it.
//
// Two places in TP do exactly this, both verified in the decompilation:
//
//   * the Hyrule Castle barrier. d_a_obj_ganonwall2::Draw (and d_a_obj_ganonwall) rewrites every
//     material's fog to pure black over startZ 1000 / endZ 250000 every frame and then enters the
//     model with dComIfGd_setListBG() — a translucent dome in the OPAQUE BG list, deliberately
//     fading to black with distance.
//   * water. dKy_bg_MAxx_proc stamps mType = 7 on the terrain water family MA03/MA17/MA19 and on
//     MA20 (d_kankyo.cpp:11390/:11588) and mType = 6 on MA09 (:11381), which
//     setLightTevColorType_MAJI_sub turns into pure BLACK and pure WHITE fog respectively — and
//     the same pass moves them into the DarkBG opaque list (:11371). So the game fogs the water's
//     own colour to black before blending it over the riverbed. Deferring instead fogged the water
//     AND the riverbed under it toward black, which is why deferred water read so differently.
//
// So: a blended draw keeps its vanilla forward fog, is never registered as a frame config, and in
// the replay writes no colour — leaving the config of whatever is behind it, which is the config
// the deferred quad should use for those pixels. Where the blended surface does not write depth
// (the usual case) that is exactly right: the geometry behind gets its own deferred fog and the
// surface keeps its own forward fog on top, same as vanilla. Where it does write depth the quad
// still fogs at the surface's depth, which is the one residual a single fullscreen pass cannot
// avoid.
//
// This GENERALISES the old is_barrier_fog special case, which is kept as a second trigger for the
// same treatment so the barrier is handled even if a material's blend state cannot be read.
bool material_blends(J3DMaterial* material) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    if (peBlock == nullptr) {
        return false;
    }
    const J3DBlend* blend = peBlock->getBlend();
    if (blend == nullptr) {
        // J3DPEBlockOpa / TexEdge / Xlu carry no blend state to read; only the Xlu variant is a
        // blended material, and its PE block type says so. (TexEdge is alpha-TESTED, not blended:
        // it replaces what is under it, so its fog defers correctly.)
        return peBlock->getType() == 'PEXL';
    }
    const GXBlendMode mode = blend->getBlendMode();
    if (mode == GX_BM_NONE) {
        return false;
    }
    // GX_BM_BLEND with ONE/ZERO is a replace, whatever it calls itself.
    if (mode == GX_BM_BLEND && blend->getSrcFactor() == GX_BL_ONE &&
        blend->getDstFactor() == GX_BL_ZERO)
    {
        return false;
    }
    return true;
}

// The fog a material's own PE block carries, if it has one. Only J3DPEBlockFull carries fog at all
// — J3DPEBlockOpa / TexEdge / Xlu / FogOff all return NULL from getFog() and their display lists
// leave the fog registers alone, so those materials simply inherit whatever the last GXSetFog put
// there (which the GXSetFog hook has already dealt with).
bool material_fog_config(J3DMaterial* material, FogConfig& out) {
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    J3DFog* fog = peBlock != nullptr ? peBlock->getFog() : nullptr;
    if (fog == nullptr || fog->mType == 0) {
        return false;
    }
    out.type = fog->mType;
    out.startZ = fog->mStartZ;
    out.endZ = fog->mEndZ;
    out.nearZ = fog->mNearZ;
    out.farZ = fog->mFarZ;
    out.color = fog->mColor;
    capture_adj_from_material(*fog, out);
    return true;
}

// Replay mode: force everything this draw emits to a flat config-ID colour, and no fog.
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

// Count of draws this frame left on vanilla forward fog because they blend (diagnostic).
uint32_t g_blendedDrawCount = 0;

bool keeps_forward_fog(J3DMaterial* material, const FogConfig& config) {
    return (g_blendedStayVanilla && material_blends(material)) || is_barrier_fog(config);
}

void replay_stamp_material(J3DMaterial* material) {
    FogConfig config;
    const bool hasFog = material_fog_config(material, config);
    if (keeps_forward_fog(material, hasFog ? config : FogConfig{})) {
        // Write no colour, so the config of whatever is behind this surface survives in the ID
        // buffer — that is the config the deferred quad should use for these pixels. stamp_replay_id
        // turns colour writes back on for the next ordinary draw, and replay_config_ids restores
        // them at the end of the pass.
        GXSetColorUpdate(GX_FALSE);
        return;
    }
    stamp_replay_id(hasFog ? lookup_frame_config(config) : 0u);
}

// Capture mode: register the material's own fog and suppress it if we are deferring it. Returns
// whether the material carried live fog at all (for the shared-DL diagnostic).
bool suppress_material_fog(J3DMaterial* material) {
    FogConfig config;
    const bool hasFog = material_fog_config(material, config);
    if (keeps_forward_fog(material, hasFog ? config : FogConfig{})) {
        ++g_blendedDrawCount;
        return hasFog;
    }
    if (!hasFog) {
        return false;
    }
    if (vote_config(config)) {
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
    }
    return true;
}

HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
        replay_stamp_material(shape != nullptr ? shape->getMaterial() : nullptr);
        return HOOK_CONTINUE;
    }
    if (g_quadArmed) {
        g_quadArmed = false;
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

// THE SHARED-DISPLAY-LIST PATH DOES NOT GO THROUGH J3DShape::drawFast.
//
// dBgp_c ("bg parts" — the shared, instanced MAP UNITS a stage is assembled from, which in the
// field is most of the distant scenery) draws its geometry itself:
// dBgp_c::modelMaterial_c::drawSimple (d_bg_parts.cpp:20) calls mpMaterial->loadSharedDL() and then
// walks the shape's matrix groups calling J3DShapeDraw::draw() directly — never
// J3DShape::drawFast. d_model.cpp, d_particle.cpp and the chain actors use the same shape. The
// packet-level fog those paths set through dKy_GxFog_tevstr_set IS caught (that is a real GXSetFog
// call), but the material display list replayed by loadSharedDL re-issues J3DGDSetFog from the
// material's OWN fog block afterwards, and nothing we hooked runs in between. Two consequences,
// both worst exactly where the fog term is largest:
//
//   * that geometry kept its forward fog AND got the deferred quad on top — double fog;
//   * in exact mode the replay never stamped it, so it rasterized real lit colours, decoded as
//     "uncovered", and fell through to the fallback config instead of its own.
//
// Post-hooking loadSharedDL lands exactly between the display list and the shapes. All three
// overrides are hooked because the material class a model loads with (plain / patched / locked) is
// a J3DMaterialFactory decision we do not control; an override that fails to resolve is warned
// about rather than being fatal, and only costs the coverage it would have added.
//
// IT IS SCOPED TO dBgp_c, and that is not caution — it is required. Every OTHER loadSharedDL caller
// (d_model.cpp:22, d_particle.cpp:593, the fchain/wchain/hookshot chain shapes) calls
// dKy_GxFog_tevstr_set IMMEDIATELY AFTER loading the display list, so the material's own fog is
// overwritten before a single triangle rasterizes and never renders at all. Registering it would
// put a config in the frame table that vanilla never draws with — enough to make a uniform scene
// read as "mixed", which in Vanilla mode reverts the whole scene to forward fog. dBgp_c is the one
// caller that sets its fog BEFORE the material loop (d_bg_parts.cpp:157), so there, and only there,
// the display list has the last word.
bool g_inBgpMaterial = false;
uint32_t g_sharedDlFogCount = 0;

// THE UNCOVERED-PIXEL FALLBACK: the config the SELF-DRAWING opaque packets used this frame.
//
// Field/tall grass (dGrass_packet_c) and flowers (dFlower_packet_c) do not draw through J3D at all.
// They call the room's fog setter, replay their own static material display lists, and then emit
// raw GX batches — so the replay's per-draw flat-ID override cannot reach them: the material list
// re-programs TEV after anything we could set, and the geometry is not a J3DShape. They therefore
// rasterize REAL LIT COLOURS into the ID buffer, the shader's green/blue guard correctly rejects
// those, and every grass and flower pixel lands on the fallback. That makes the fallback's value
// the fog those two packets get, which is worth measuring rather than assuming.
//
// So bracket their draws and record the config index their own fog setter resolved to. It is the
// room's environment fog — the same one the terrain under them uses. If the hooks do not resolve,
// the value stays 0, the frame's reference config, which is that same room fog in an ordinary
// frame; the degradation is invisible.
//
// (This also catches MSAA silhouette fringes and any other stray unstamped pixel. Giving those the
// room fog is right for the same reason.)
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
    // Grass and flowers set their fog from inside their own packet draw, and this is the only place
    // we ever see it. Remember the config it resolved to — those packets' pixels all land on the
    // uncovered-pixel fallback. First one in the frame wins; per-room palette differences within the
    // config-match tolerance collapse to the same index anyway.
    if (g_inSelfDrawnPacket && !g_selfDrawnIndexValid) {
        g_selfDrawnIndex = lookup_frame_config(config);
        g_selfDrawnIndexValid = true;
    }
    if (suppress) {
        mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
    }
    return HOOK_CONTINUE;
}

void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload)) {
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

    if (exact_mode() && g_frameConfigCount > 1 && g_configIdView != nullptr) {
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
        // One range block for the whole frame: every config's table and centre come from the same
        // environment globals, so the first enabled one speaks for all of them. Configs with the
        // adjustment off simply don't set kFogTypeRangeAdjBit and ignore it.
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            if (g_frameConfigs[i].adj.enable) {
                uniforms.range = build_fog_range(g_frameConfigs[i].adj);
                break;
            }
        }
        // Pixels the config-ID replay could not stamp fall back to this config — in practice grass
        // and flowers, which cannot be stamped at all. See g_selfDrawnIndex.
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
    // Blended draws switch colour writes off for their own geometry (see replay_stamp_material);
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

// Diagnostic: dump the frame's captured fog-config table on change, so the exact configs at a
// spot (uniform vs multiple, their ranges/colors) can be read off in-game. Off by default.
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

void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = 0;
    g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_sharedDlFogCount = 0;
    g_blendedDrawCount = 0;
    g_blendedStayVanilla = read_blended_draws_stay_vanilla();
    g_configIdView = nullptr;
    g_quadArmed = false;
    g_inBgpMaterial = false;
    g_inSelfDrawnPacket = false;
    g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_scopeActive = effect_enabled();
    if (!g_scopeActive) {
        g_suppressAllowed = false;
    }
}

void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (!g_scopeActive) {
        return;
    }
    g_scopeActive = false;
    // Sample the world viewport while it is still current — by FRAME_BEFORE_HUD, where the vanilla
    // path pushes the quad, the game has moved on to its 2D viewport. Width only; see
    // g_frameViewportWidth.
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

    if (exact && g_frameConfigCount > 1 && g_quadArmed) {
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
            "Deferring fog (exact: %u draws, %u config%s%s; %u shared-DL, %u blended)",
            g_suppressedCount + g_deviantCount, g_frameConfigCount,
            g_frameConfigCount == 1 ? "" : "s",
            g_frameConfigCount > 1 && g_configIdView == nullptr ? ", replay failed" : "",
            g_sharedDlFogCount, g_blendedDrawCount);
    } else if (g_deviantCount > 0) {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "REVERTED: mixed fog configs (%u matching / %u deviant)", g_suppressedCount,
            g_deviantCount);
    } else {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Deferring fog (%u draws this frame; %u shared-DL, %u blended)", g_suppressedCount,
            g_sharedDlFogCount, g_blendedDrawCount);
    }

    log_fog_configs();
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    if (!g_quadArmed) {
        return;
    }
    g_quadArmed = false;
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
        "per-pixel "
        "config-ID buffer so each pixel gets the fog its own draw used. Costs one extra opaque "
        "geometry pass on mixed frames. Pixels the replay cannot rasterize faithfully - "
        "translucent surfaces drawn in the opaque lists, notably the Ganon barrier dome - fall "
        "back to the frame's widest fog rather than their own.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogMixed;
    control.options = kMixedOptions;
    control.option_count = 2;
    add_control(left, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Blended Draws Keep Vanilla Fog";
    control.help_rml =
        "The game's fog is applied to each surface <i>before</i> it is blended, so a single "
        "fullscreen pass cannot reproduce it for anything see-through. A few surfaces are drawn "
        "see-through inside the opaque world lists - water (which the game fogs to <b>black</b>, "
        "so it darkens with depth), the Hyrule Castle barrier dome (also fogged to black so it "
        "fades out at distance), and additively blended terrain passes (which the game's fog makes "
        "<i>brighter</i>). With this on, those draws are left on the game's own fog and only the "
        "surfaces behind them are deferred, which matches vanilla. Turn it off to compare.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogBlended;
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

// Adds this sub-feature's section to the shared mods panel.
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

bool build_fog_pipeline(bool blend, const char* entryPoint, WGPURenderPipeline& outPipeline,
    WGPUBindGroupLayout& outLayout) {
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
    // The pipeline has to describe the scene pass's attachments, whatever they currently are: with
    // a device that carries the scene normals, the pass has a second, renderer-owned colour target,
    // and a one-target pipeline is rejected outright. Asking the service beats rebuilding the
    // layout from GfxDeviceInfo, which is a copy of the renderer's logic that goes silently wrong
    // whenever the pass gains an attachment. The extra target comes back write-masked off, so the
    // fog leaves the game's authored normals untouched - correct in itself, since fog changes what
    // a surface looks like and not which way it faces.
    gfx_compat::ScenePassLayout layout;
    if (!gfx_compat::scene_pass_layout(mod_ctx, svc_gfx, g_deviceInfo, layout)) {
        wgpuShaderModuleRelease(module);
        return false;
    }
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
    // DEFAULT: mixed-scene mode = Exact replay (1). 0 = Vanilla revert. Exact keeps deferring in a
    // multi-config scene and reconstructs each pixel's own config from a replayed ID buffer, so the
    // AO-under-fog benefit survives scenes that mix configs — which is most outdoor scenes. Vanilla
    // gives those scenes back to the game's forward fog instead, exact but with AO on top of the
    // fog again. Keep exact_mode()'s fallback in step with this value.
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogMixedMode";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 1;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogMixed) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    // DEFAULT: blended draws keep vanilla forward fog (on). See material_blends() — GX fog runs
    // before the blend, so a fullscreen pass cannot reproduce it for anything that does not replace
    // what is under it. Off restores the pre-fix behaviour for A/B comparison.
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogBlendedVanilla";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogBlended) != MOD_OK) {
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
    if (!build_fog_pipeline(true, "fs_main", g_fogPipeline, g_fogLayout) ||
        !build_fog_pipeline(false, "fs_main", g_fogDebugPipeline, g_fogDebugLayout) ||
        !build_fog_pipeline(true, "fs_mixed", g_mixedPipeline, g_mixedLayout) ||
        !build_fog_pipeline(false, "fs_mixed", g_mixedDebugPipeline, g_mixedDebugLayout))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create fog pipeline");
    }

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
    // Bracket the self-drawing opaque packets so the uncovered-pixel fallback is measured rather
    // than assumed (see g_selfDrawnIndex). Failure degrades to the reference config, so it warns.
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
    g_cvarFogEnabled = g_cvarFogMixed = g_cvarFogDebug = g_cvarFogLog = g_cvarFogBlended = 0;
    g_lastFogLogSig[0] = '\0';
    g_controlsWindow = 0;
    g_drawType = g_sceneBeginHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_scopeActive = g_quadArmed = g_suppressAllowed = g_shapeHookOk = g_wasSuppressing = false;
    g_lastFrameDeferred = false;
    g_fogReplayActive = g_wasMixed = g_warnedReplayFailure = false;
    g_inBgpMaterial = g_inSelfDrawnPacket = g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_sharedDlFogCount = g_blendedDrawCount = 0;
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

// Exported so a consumer can read whether this frame's fog was actually deferred (a mod can then
// tell the user whether its composite is landing under the fog). Importing it also happens to force
// load order, which is the only ordering lever the mod API has - but no mod in this repo needs
// that: see the note at the top of this file. See include/deferred_fog_service.h.
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
