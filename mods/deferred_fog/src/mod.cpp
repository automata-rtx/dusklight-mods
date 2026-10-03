// Deferred Fog
//
// The game fogs each opaque draw while drawing it, so anything composited over the finished opaque
// image (ambient occlusion, for one) lands on top of the fog. This mod switches the game's fog off
// for the opaque world and applies the same fog afterwards in one fullscreen pass, after every
// mod's GFX_STAGE_SCENE_AFTER_OPAQUE work and before anything the game draws or copies later.
//
// One frame (game thread, except on_draw):
//   GFX_STAGE_SCENE_BEGIN         on_scene_begin snapshots the depth the sky lists left and opens
//                                 the capture scope, unless the mod is off, Wolf Senses is active
//                                 or a hook is missing.
//   opaque world lists            the capture hooks record each draw's fog configuration and
//                                 switch its fog off.
//   GFX_STAGE_SCENE_AFTER_OPAQUE  on_scene_after_opaque closes the scope and arms the fog pass.
//                                 When the frame used several configurations (or Skip Unfogged has
//                                 fog-off draws to mark), it replays the opaque lists into a
//                                 per-pixel configuration-ID buffer.
//   dComIfGd_drawXluListBG        on_xlu_list_bg_pre pushes the fog pass. mDoGph_Painter calls this
//                                 function every frame directly after the SCENE_AFTER_OPAQUE stage,
//                                 before the translucent lists, every framebuffer copy and bloom.
//   render worker                 on_draw records the pass. res/fog.wgsl evaluates aurora's fog
//                                 formula with coefficients from src/fog_math.h, and leaves alone
//                                 every pixel whose depth is still the sky lists'.
//
// Every game hook is required: if any fails to attach, the mod stays inactive and the game draws
// its own fog. Reference: docs/deferred_fog.md.

#include "global.h"

#include "deferred_fog_service.h"
#include "fog_math.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_flower.h"
#include "d/actor/d_grass.h"
#include "d/d_bg_parts.h"
#include "d/d_com_inf_game.h"
#include "dolphin/gf/GFPixel.h"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXBump.h"
#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXLighting.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTev.h"

#include "gfx_scene_pass.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

namespace {

// ---------------------------------------------------------------------------------------------
// Game hooks. Each is resolved by symbol name when the mod loads; all of them are required
// (install_hooks).
// ---------------------------------------------------------------------------------------------

// Fog capture: the four ways the game sets fog on opaque geometry.
DEFINE_HOOK(GXSetFog, SetFog);
DEFINE_HOOK(GFSetFog, SetGfFog);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);
DEFINE_HOOK(&dBgp_c::modelMaterial_c::drawSimple, BgpDrawSimple);
DEFINE_HOOK(&J3DMaterial::loadSharedDL, MaterialSharedDL);
DEFINE_HOOK(&J3DPatchedMaterial::loadSharedDL, PatchedMaterialSharedDL);
DEFINE_HOOK(&J3DLockedMaterial::loadSharedDL, LockedMaterialSharedDL);
// The grass and flower packets, whose pixels the configuration-ID replay cannot label.
DEFINE_HOOK(&dGrass_packet_c::draw, GrassPacketDraw);
DEFINE_HOOK(&dFlower_packet_c::draw, FlowerPacketDraw);
// Where the fog pass is drawn.
DEFINE_HOOK(dComIfGd_drawXluListBG, XluListBGDraw);

// ---------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------

ConfigVarHandle g_cvarEnabled = 0;        // fogEnabled, default on
ConfigVarHandle g_cvarSkipUnfogged = 0;   // fogSkipUnfogged, default on
ConfigVarHandle g_cvarDeferInSenses = 0;  // fogDeferInSenses, default off
ConfigVarHandle g_cvarDebugView = 0;      // fogDebug, default 0
ConfigVarHandle g_cvarLogConfigs = 0;     // fogLogConfigs, default off

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

// ---------------------------------------------------------------------------------------------
// Fog configurations
// ---------------------------------------------------------------------------------------------

// GX fog range adjustment, which the game calls XFog (GxXFog_set, mXFogTbl). A pixel near the left
// or right screen edge is further from the eye than a centre pixel at the same Z, so GX multiplies
// the fog term by a per-column factor built from a 10-entry table and a centre column. The game
// enables it in envcolor_init. Each direct setter is followed by GxXFog_set(), and
// setLightTevColorType_MAJI_sub copies the same globals into the fog blocks of the materials it
// processes, so normally every configuration in a frame carries the same table; a material the
// game never re-stamps keeps the values authored in its model.
struct FogRangeAdj {
    bool enable = false;
    uint16_t center = 0x140;
    uint16_t table[10] = {};
};

// One fog configuration: what GXSetFog / J3DGDSetFog programs, plus its range adjustment.
struct FogConfig {
    uint8_t type = 0;
    float startZ = 0.0f;
    float endZ = 0.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    GXColor color{0, 0, 0, 0};
    FogRangeAdj adj;
};

// A material's fog block is issued with J3DGDSetFogRangeAdj from the same block, so the block is
// authoritative for the shapes the material draws.
void read_range_adj(const J3DFog& fog, FogRangeAdj& out) {
    out.enable = fog.mAdjEnable != 0;
    out.center = fog.mCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.table[i] = fog.mFogAdjTable.r[i];
    }
}

// The direct setters are followed by GxXFog_set(), which issues GXSetFogRangeAdj from these
// globals.
void read_range_adj_from_env(FogRangeAdj& out) {
    out.enable = g_env_light.mFogAdjEnable != 0;
    out.center = g_env_light.mFogAdjCenter;
    for (uint32_t i = 0; i < 10; ++i) {
        out.table[i] = g_env_light.mXFogTbl.r[i];
    }
}

// Whether two configurations render the same fog. Small tolerances let configurations that differ
// only by a palette blend in progress share one table slot.
bool config_matches(const FogConfig& a, const FogConfig& b) {
    if (a.type != b.type || a.adj.enable != b.adj.enable) {
        return false;
    }
    if (a.adj.enable &&
        (a.adj.center != b.adj.center ||
            std::memcmp(a.adj.table, b.adj.table, sizeof(a.adj.table)) != 0))
    {
        return false;
    }
    const auto close = [](uint8_t lhs, uint8_t rhs) {
        return std::abs(static_cast<int>(lhs) - static_cast<int>(rhs)) <= 6;
    };
    if (!close(a.color.r, b.color.r) || !close(a.color.g, b.color.g) ||
        !close(a.color.b, b.color.b))
    {
        return false;
    }
    const float span = std::max(std::fabs(a.endZ - a.startZ), 1.0f);
    return std::fabs(b.startZ - a.startZ) <= span * 0.02f &&
           std::fabs(b.endZ - a.endZ) <= span * 0.02f && std::fabs(b.nearZ - a.nearZ) <= 1.0f &&
           std::fabs(b.farZ - a.farZ) <= a.farZ * 0.01f + 1.0f;
}

// The Hyrule Castle barrier (d_a_obj_ganonwall, d_a_obj_ganonwall2) is a translucent dome drawn in
// the opaque lists. Every Draw() it sets its material fog to black over 1000..250000. Deferring
// that fog would put the dome's black fog on the castle behind it, so the barrier keeps its own
// fog: its draws are neither suppressed nor registered, and the replay writes nothing for it, so
// its pixels take the configuration of whatever is behind the dome.
//
// The test must be this exact triple. The materials the game fogs black by polygon code (the water
// codes MA03, MA17 and MA19, and MA20), whose fog setLightTevColorType_MAJI_sub turns black while
// keeping the room's range, are also black with a far end, and must be deferred like everything
// else.
bool is_barrier_fog(const FogConfig& c) {
    return c.color.r == 0 && c.color.g == 0 && c.color.b == 0 && c.startZ == 1000.0f &&
           c.endZ == 250000.0f;
}

// The frame's distinct configurations, in the order first seen. Slot 0 is the frame's main
// configuration: the single-configuration pass uses it, and configurations that do not fit in the
// table are drawn with it.
constexpr uint32_t kMaxFogConfigs = 8;
FogConfig g_frameConfigs[kMaxFogConfigs];
uint32_t g_frameConfigCount = 0;
uint32_t g_mergedDrawCount = 0;  // draws whose configuration did not fit and use slot 0

uint32_t find_frame_config(const FogConfig& config) {
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        if (config_matches(g_frameConfigs[i], config)) {
            return i;
        }
    }
    return kMaxFogConfigs;
}

uint32_t register_frame_config(const FogConfig& config) {
    const uint32_t found = find_frame_config(config);
    if (found != kMaxFogConfigs) {
        return found;
    }
    if (g_frameConfigCount < kMaxFogConfigs) {
        g_frameConfigs[g_frameConfigCount] = config;
        return g_frameConfigCount++;
    }
    ++g_mergedDrawCount;
    return 0;
}

uint32_t lookup_frame_config(const FogConfig& config) {
    const uint32_t found = find_frame_config(config);
    return found == kMaxFogConfigs ? 0 : found;
}

// ---------------------------------------------------------------------------------------------
// Material fog
// ---------------------------------------------------------------------------------------------

// What a material's fog block does when its display list runs:
//   Live     programs real fog.
//   Off      mType 0: J3DFog::load() programs GX_FOG_NONE, so the game draws the material with no
//            fog at any distance.
//   NoBlock  the material has no fog block and inherits the last GXSetFog.
enum class MaterialFog : uint8_t { NoBlock, Off, Live };

MaterialFog material_fog(J3DMaterial* material, FogConfig& out) {
    J3DPEBlock* pe = material != nullptr ? material->getPEBlock() : nullptr;
    J3DFog* fog = pe != nullptr ? pe->getFog() : nullptr;
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
    read_range_adj(*fog, out.adj);
    return MaterialFog::Live;
}

// Blends whose result differs between fogging each layer before blending (the game) and fogging
// the blended pixel once (this mod): additive (destination factor one) and subtract. Counted for
// the Status line; see docs/deferred_fog.md "Limitations".
bool material_over_unity_blend(J3DMaterial* material) {
    J3DPEBlock* pe = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DBlend* blend = pe != nullptr ? pe->getBlend() : nullptr;
    if (blend == nullptr) {
        return false;
    }
    const GXBlendMode mode = blend->getBlendMode();
    return mode == GX_BM_SUBTRACT || (mode == GX_BM_BLEND && blend->getDstFactor() == GX_BL_ONE);
}

// The fog pass reads one depth per pixel, so only a draw that writes its own depth decides a
// pixel's fog.
bool material_owns_depth(J3DMaterial* material) {
    J3DPEBlock* pe = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DZMode* z = pe != nullptr ? pe->getZMode() : nullptr;
    return z != nullptr && z->getCompareEnable() != 0 && z->getUpdateEnable() != 0;
}

// Whether the alpha test passes everything. The replay's flat stamp draws without textures and with
// the alpha test off, so it would stamp an alpha-tested material as its whole primitive rather than
// its cutout; a fog-off one is marked through its own alpha instead (plan_alpha_mark).
// calcAlphaCmpID packs (comp0 << 5) + (op << 3) + comp1; GX_ALWAYS is 7.
bool material_alpha_test_passes_all(J3DMaterial* material) {
    J3DPEBlock* pe = material != nullptr ? material->getPEBlock() : nullptr;
    const J3DAlphaComp* comp = pe != nullptr ? pe->getAlphaComp() : nullptr;
    if (comp == nullptr) {
        return false;
    }
    constexpr uint16_t kAlways = 7;
    return ((comp->mID >> 5) & 7) == kAlways && (comp->mID & 7) == kAlways;
}

// How the replay marks a fog-off, alpha-tested material through its own alpha
// (stamp_no_fog_through_alpha). Read from the material's blocks, which its display list was built
// from.
struct AlphaMark {
    uint8_t stageCount = 0;                // the material's TEV stages; the mark stage follows them
    GXTevAlphaArg alphaSource = GX_CA_APREV;  // the register its last stage writes alpha to
    GXBool alphaClamp = GX_TRUE;           // that stage's clamp setting
    uint8_t colorChanCount = 0;            // its colour channels
    uint8_t matAlpha = 255;                // its material colour's alpha, which ALPHA0 may read
};

// False when the material cannot be marked: it sets no alpha test of its own, has no TEV stages
// to read, or already uses all 16.
bool plan_alpha_mark(J3DMaterial* material, AlphaMark& out) {
    J3DPEBlock* pe = material != nullptr ? material->getPEBlock() : nullptr;
    J3DTevBlock* tev = material != nullptr ? material->getTevBlock() : nullptr;
    if (pe == nullptr || pe->getAlphaComp() == nullptr || tev == nullptr) {
        return false;
    }
    const uint8_t stages = tev->getTevStageNum();
    const J3DTevStage* last = stages > 0 ? tev->getTevStage(stages - 1) : nullptr;
    if (last == nullptr || stages >= GX_MAX_TEVSTAGE) {
        return false;
    }
    // J3DTevStage keeps the stage's alpha combiner as the GX register does: mTevAlphaOp holds the
    // output register in bits 6-7 and the clamp in bit 3.
    static constexpr GXTevAlphaArg kOutputRegisterAlpha[4] = {
        GX_CA_APREV, GX_CA_A0, GX_CA_A1, GX_CA_A2};
    out.stageCount = stages;
    out.alphaSource = kOutputRegisterAlpha[(last->mTevAlphaOp >> 6) & 3];
    out.alphaClamp = ((last->mTevAlphaOp >> 3) & 1) != 0 ? GX_TRUE : GX_FALSE;
    J3DColorBlock* color = material->getColorBlock();
    const J3DGXColor* matColor = color != nullptr ? color->getMatColor(0) : nullptr;
    out.colorChanCount = color != nullptr ? color->getColorChanNum() : 0;
    out.matAlpha = matColor != nullptr ? matColor->a : 255;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Per-frame state (game thread)
// ---------------------------------------------------------------------------------------------

bool g_hooksOk = false;          // every game hook attached (install_hooks)
const char* g_missingHook = nullptr;

bool g_scopeActive = false;      // SCENE_BEGIN .. SCENE_AFTER_OPAQUE: capturing and suppressing
bool g_quadArmed = false;        // SCENE_AFTER_OPAQUE .. dComIfGd_drawXluListBG
bool g_replayActive = false;     // replay_config_ids is re-drawing the opaque lists
bool g_skipUnfogged = false;     // fogSkipUnfogged, read once per frame
uint32_t g_debugView = 0;        // fogDebug, read once per frame: 0 off, 1..3 the debug views
bool g_sensesExempt = false;
bool g_wasSensesExempt = false;
bool g_wasReplaying = false;
bool g_lastFrameDeferred = false;  // published through the service
bool g_warnedReplayFailure = false;
bool g_warnedDepthFailure = false;

// This frame's configuration-ID buffer, borrowed from the gfx service; valid this frame only.
WGPUTextureView g_configIdView = nullptr;

// The depth buffer as the sky lists left it, snapshotted at GFX_STAGE_SCENE_BEGIN; valid this frame
// only. The sky lists draw before the scope opens and keep their own fog, and some of them write
// depth: while the sun is on screen, drawVrkumo first draws the drifting clouds (kumo: cloud) as
// depth-only shapes, and dKyr_sun_move reads depth back around the sun (dComIfGd_peekZ) to judge
// how much of it is hidden. The fog pass leaves every pixel whose depth still equals this snapshot
// to the sky's own fog.
WGPUTextureView g_skyDepthView = nullptr;
bool g_warnedSkyDepthFailure = false;

// The world viewport width in the game's logical coordinates, for the range-adjustment centre.
float g_viewportWidth = 640.0f;

// Status line counts, per draw.
uint32_t g_capturedDrawCount = 0;   // draws whose fog was captured and suppressed
uint32_t g_sharedDlFogCount = 0;    // ... of those, map-unit (dBgp_c) material draws
uint32_t g_fogOffCount = 0;         // draws whose material has fog switched off (MaterialFog::Off)
uint32_t g_fogOffNoDepth = 0;       // ... of those, draws that write no depth
uint32_t g_fogOffAlphaTested = 0;   // ... draws that write depth, marked through their alpha
uint32_t g_fogOffUnmarkable = 0;    // ... draws that write depth but cannot be marked
uint32_t g_overUnityCount = 0;      // draws with an additive or subtractive blend
uint32_t g_overUnityNoDepth = 0;    // ... of those, draws that write no depth

// Fog-off draws Skip Unfogged can mark: those that write depth, with or without an alpha test.
uint32_t fog_off_markable() {
    return g_fogOffCount - g_fogOffNoDepth - g_fogOffUnmarkable;
}

// The configuration the grass and flower packets drew with this frame. They load their material
// display list, set the room fog (grass: dKy_GfFog_tevstr_set -> GFSetFog; flowers:
// dKy_GxFog_tevstr_set -> GXSetFog) and send raw geometry, so the replay cannot recolour them.
// Their pixels, and those of any other self-drawing packet, take this configuration.
bool g_inSelfDrawnPacket = false;
bool g_selfDrawnIndexValid = false;
uint32_t g_selfDrawnIndex = 0;

// True inside dBgp_c::modelMaterial_c::drawSimple (see on_material_shared_dl_post).
bool g_inBgpMaterial = false;

char g_statusText[224] = "Waiting for the first frame";

// The configuration-ID buffer is used when the frame has more than one configuration, when Skip
// Unfogged has fog-off draws to mark, or when the Replay Coverage debug view shows the buffer. The
// replay and the fog pass both ask this one function.
constexpr uint32_t kDebugReplayCoverage = 3;

bool needs_id_buffer() {
    return g_frameConfigCount > 1 || (g_skipUnfogged && fog_off_markable() > 0) ||
           g_debugView == kDebugReplayCoverage;
}

// Wolf Senses replaces every environment fog with black fog over a short range
// (dKy_WolfPowerup_FogNearFar). Black fog only scales colour, (1 - f) * x, so a multiplicative
// composite such as AO gives the same image under the game's fog as under this mod's:
// m * (1 - f) * x == (1 - f) * (m * x). The mod therefore leaves senses fog to the game, which also
// avoids the fog pass's one-depth-per-pixel limits where the fog reaches black within a short
// range. checkNowWolfPowerUp() is the game's own test for the senses fog; it reads the player, so
// the player is checked first.
bool wolf_senses_active() {
    return dComIfGp_getLinkPlayer() != nullptr && daPy_py_c::checkNowWolfPowerUp();
}

// ---------------------------------------------------------------------------------------------
// Capture (inside the scope)
// ---------------------------------------------------------------------------------------------

// Registers a captured configuration and counts the draw. Returns its table slot.
uint32_t capture_config(const FogConfig& config) {
    ++g_capturedDrawCount;
    return register_frame_config(config);
}

// A J3D material's display list has just set its fog: count it, register it and switch it off.
// Returns whether the material carried live fog.
bool capture_material_fog(J3DMaterial* material) {
    FogConfig config;
    const MaterialFog state = material_fog(material, config);
    const bool ownsDepth = material_owns_depth(material);
    if (material_over_unity_blend(material)) {
        ++g_overUnityCount;
        g_overUnityNoDepth += ownsDepth ? 0 : 1;
    }
    if (state == MaterialFog::Off) {
        ++g_fogOffCount;
        AlphaMark mark;
        if (!ownsDepth) {
            ++g_fogOffNoDepth;
        } else if (material_alpha_test_passes_all(material)) {
            // marked with the flat stamp
        } else if (plan_alpha_mark(material, mark)) {
            ++g_fogOffAlphaTested;
        } else {
            ++g_fogOffUnmarkable;
        }
    }
    if (state != MaterialFog::Live) {
        return false;
    }
    if (!is_barrier_fog(config)) {
        capture_config(config);
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Configuration-ID replay
// ---------------------------------------------------------------------------------------------

// The replay draws every opaque shape in a flat colour that encodes its configuration: red =
// (slot + 1) * 24, green and blue 0, so slots 0..7 write 24..192. kNoFogSlot writes 216: "the game
// draws this pixel with no fog" (Skip Unfogged). res/fog.wgsl decodes red to a slot within +-4 and
// sends any pixel with green or blue above 0.03 (lit geometry the replay could not recolour) to the
// grass and flower configuration.
constexpr uint32_t kNoFogSlot = 8;
static_assert(kNoFogSlot >= kMaxFogConfigs, "the no-fog slot must not collide with a real one");
static_assert((kNoFogSlot + 1) * 24 <= 255, "the no-fog slot must fit in the red channel");

constexpr u8 slot_red(uint32_t slot) {
    return static_cast<u8>((slot + 1) * 24);
}

void stamp_replay_id(uint32_t slot) {
    const u8 red = slot_red(slot);
    GXSetColorUpdate(GX_TRUE);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetNumChans(1);
    GXSetChanCtrl(
        GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, GXColor{red, 0, 0, 255});
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
}

// Marks a fog-off, alpha-tested material with the no-fog slot where its alpha test passes, so only
// its visible cutout is marked and the replay's depth matches the frame's. The material's display
// list has already set its textures, TEV stages, alpha test and Z mode; this keeps all of them and
// appends one stage that outputs the no-fog colour with the alpha the material's last stage wrote,
// unchanged. The colour comes from channel COLOR0, switched to its material colour register. That
// register is shared with ALPHA0, whose own setting is left as the material set it, so its alpha
// keeps the material's value.
void stamp_no_fog_through_alpha(const AlphaMark& mark) {
    const auto stage = static_cast<GXTevStageID>(GX_TEVSTAGE0 + mark.stageCount);
    if (mark.colorChanCount == 0) {
        GXSetNumChans(1);
    }
    GXSetChanCtrl(
        GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0, GXColor{slot_red(kNoFogSlot), 0, 0, mark.matAlpha});
    GXSetNumTevStages(static_cast<u8>(mark.stageCount + 1));
    GXSetTevOrder(stage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevDirect(stage);
    GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, mark.alphaSource);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, mark.alphaClamp, GX_TEVPREV);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
}

// Writes nothing for this draw, so its pixels keep the ID of the draw that owns their depth. Done
// with a blend that keeps the destination rather than by switching colour writes off: every J3D
// material reloads its blend mode but not the colour-update switch (J3DGDSetBlendMode masks it
// out), so the state cannot outlive the draw into a following draw the hooks do not restamp.
void stamp_nothing() {
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_ONE, GX_LO_COPY);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
}

// Replay, one material draw: stamp its slot, the no-fog slot, or nothing.
void replay_stamp_material(J3DMaterial* material) {
    FogConfig config;
    const MaterialFog state = material_fog(material, config);
    if (state == MaterialFog::Live && is_barrier_fog(config)) {
        stamp_nothing();  // the dome's pixels take the configuration of whatever is behind it
        return;
    }
    const bool ownsDepth = material_owns_depth(material);
    if (state == MaterialFog::Off && !ownsDepth) {
        // A fog-off draw that writes no depth (a glow or swirl layered over other geometry) has no
        // configuration of its own to stamp, and stamping one would replace the ID of the surface
        // whose depth the fog pass uses there, including that surface's no-fog mark.
        stamp_nothing();
        return;
    }
    if (state == MaterialFog::Off && g_skipUnfogged && ownsDepth) {
        if (material_alpha_test_passes_all(material)) {
            stamp_replay_id(kNoFogSlot);
            return;
        }
        AlphaMark mark;
        if (plan_alpha_mark(material, mark)) {
            stamp_no_fog_through_alpha(mark);
            return;
        }
    }
    stamp_replay_id(state == MaterialFog::Live ? lookup_frame_config(config) : 0u);
}

// The opaque lists the scope covers, without the Pri0_B particles and the game's shadows that are
// drawn between them.
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

// Re-draws the opaque lists into an offscreen pass with the game's camera, every shape forced to
// its slot colour by the capture hooks (g_replayActive), and keeps the colour as this frame's
// configuration-ID buffer. Restores the viewport, scissor and J3D state it changes.
bool replay_config_ids(uint32_t width, uint32_t height) {
    f32 viewport[6];
    GXGetViewportv(viewport);
    u32 scissor[4];
    GXGetScissor(&scissor[0], &scissor[1], &scissor[2], &scissor[3]);

    if (svc_gfx->create_pass(mod_ctx, width, height) != MOD_OK) {
        return false;
    }
    J3DShape::resetVcdVatCache();
    GXSetViewport(0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f);
    GXSetViewportRender(0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, width, height);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

    J3DModel* savedModel = j3dSys.getModel();
    j3dSys.setModel(nullptr);
    g_replayActive = true;
    draw_opaque_scene_lists();
    g_replayActive = false;
    j3dSys.setModel(savedModel);
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetViewport(viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]);
    GXSetScissor(scissor[0], scissor[1], scissor[2], scissor[3]);

    GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
    desc.color = true;
    desc.depth = false;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &desc, &resolved) != MOD_OK) {
        return false;
    }
    g_configIdView = resolved.color;
    return g_configIdView != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Capture hooks
// ---------------------------------------------------------------------------------------------

// GXSetFog and GFSetFog (same signature): during the replay, force the fog off; inside the scope,
// capture the configuration and switch it off.
HookAction on_set_fog_pre(ModContext*, void* args, void*, void*) {
    if (g_replayActive) {
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
    read_range_adj_from_env(config.adj);
    if (is_barrier_fog(config)) {
        return HOOK_CONTINUE;
    }
    const uint32_t slot = capture_config(config);
    if (g_inSelfDrawnPacket && !g_selfDrawnIndexValid) {
        g_selfDrawnIndex = slot;
        g_selfDrawnIndexValid = true;
    }
    mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
    return HOOK_CONTINUE;
}

// J3DShape::drawFast runs after the material's display list (and its fog) has been sent, so a
// GXSetFog here overrides the material's fog for the shape.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (!g_replayActive && !g_scopeActive) {
        return HOOK_CONTINUE;
    }
    const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
    J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
    if (g_replayActive) {
        replay_stamp_material(material);
    } else {
        capture_material_fog(material);
    }
    return HOOK_CONTINUE;
}

// Map units (dBgp_c, the shared pieces a stage is assembled from) do not use drawFast:
// dBgp_c::modelMaterial_c::drawSimple loads the material's shared display list, which sets the
// material's fog, then draws the shapes through J3DShapeDraw::draw. The post-hook on loadSharedDL
// (all three material classes) does what the drawFast hook does. It acts only inside drawSimple:
// the other callers of loadSharedDL (dMdl_c::draw, dPa_modelEcallBack::model_c::draw, the chain
// actors and daAlink_c::hsChainShape_c::draw) set the room fog after the display list, so the
// material's own fog never reaches their geometry, and the GXSetFog hook captures what does.
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
    if (g_replayActive) {
        replay_stamp_material(material);
    } else if (g_scopeActive && capture_material_fog(material)) {
        ++g_sharedDlFogCount;
    }
}

HookAction on_self_drawn_packet_pre(ModContext*, void*, void*, void*) {
    g_inSelfDrawnPacket = true;
    return HOOK_CONTINUE;
}

void on_self_drawn_packet_post(ModContext*, void*, void*, void*) {
    g_inSelfDrawnPacket = false;
}

// ---------------------------------------------------------------------------------------------
// The fog pass
// ---------------------------------------------------------------------------------------------

// These structs mirror res/fog.wgsl byte for byte.

// Range adjustment, one per frame: the table pair-swapped and scaled by 1/64 as aurora's
// build_fog_range_lut prepares it, entries 10 and 11 repeating 9, and the centre column in NDC x.
struct FogRangeUniform {
    float center;
    float _pad0;
    float _pad1;
    float _pad2;
    float k[12];
};
static_assert(sizeof(FogRangeUniform) == 64);

// fs_main: one configuration for the whole frame.
struct FogUniforms {
    float color[4];
    float a;
    float b;
    float c;
    uint32_t fog_type;  // GXFogType & 7, plus kFogTypeRangeAdj
    uint32_t debug_mode;
    float _pad0;
    float _pad1;
    float _pad2;
    FogRangeUniform range;
};
static_assert(sizeof(FogUniforms) == 112);
static_assert(offsetof(FogUniforms, range) == 48);

// fs_mixed: one entry per slot, chosen per pixel from the configuration-ID buffer.
struct MixedFogEntry {
    float color[4];
    float a;
    float b;
    float c;
    uint32_t fog_type;
};
static_assert(sizeof(MixedFogEntry) == 32);

struct MixedFogUniforms {
    MixedFogEntry configs[kMaxFogConfigs];
    uint32_t count;
    uint32_t debug_mode;
    uint32_t fallback_index;  // the grass and flower slot
    float _pad1;
    FogRangeUniform range;
};
static_assert(sizeof(MixedFogUniforms) == 336);
static_assert(offsetof(MixedFogUniforms, range) == 272);

// Set in fog_type when range adjustment applies. GXFogType's own 0x08 bit (orthographic) is masked
// off; the pass treats every type as perspective.
constexpr uint32_t kFogTypeRangeAdj = 0x10u;

// Handed to on_draw on the render worker. configIds is null for fs_main. Views are borrowed for
// this frame.
struct DrawPayload {
    WGPUTextureView sceneDepth;
    WGPUTextureView skyDepth;
    WGPUTextureView configIds;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_mode;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
GfxDrawTypeHandle g_drawType = 0;

// The four pipelines (fs_main and fs_mixed, each blended and debug) are built on the render worker
// from the live GfxDrawContext::layout and rebuilt whenever layout.key changes: the scene pass
// gains the authored-normal attachment the frame after any mod requests normals, and a pipeline
// built for the old shape is rejected without a visible error.
struct FogPipeline {
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroupLayout layout = nullptr;
};
FogPipeline g_mainPipeline;
FogPipeline g_mainDebugPipeline;
FogPipeline g_mixedPipeline;
FogPipeline g_mixedDebugPipeline;
uint64_t g_layoutKey = 0;
bool g_layoutValid = false;

void release_pipeline(FogPipeline& p) {
    if (p.layout != nullptr) {
        wgpuBindGroupLayoutRelease(p.layout);
    }
    if (p.pipeline != nullptr) {
        wgpuRenderPipelineRelease(p.pipeline);
    }
    p = FogPipeline{};
}

void release_pipelines() {
    release_pipeline(g_mainPipeline);
    release_pipeline(g_mainDebugPipeline);
    release_pipeline(g_mixedPipeline);
    release_pipeline(g_mixedDebugPipeline);
    g_layoutValid = false;
    g_layoutKey = 0;
}

// blend = true mixes the fog colour in by the fog factor in alpha (SrcAlpha, OneMinusSrcAlpha),
// which is aurora's mix(); destination alpha is kept, as the game's fog does not touch alpha.
// blend = false is the debug variant, which writes the shader output as is.
bool build_pipeline(const gfx_compat::ScenePassLayout& sceneLayout, bool blend,
    const char* entryPoint, FogPipeline& out) {
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
    // Attachments this mod does not own (the normal attachment) come back write-masked off.
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

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {blend ? "deferred fog" : "deferred fog (debug)", WGPU_STRLEN};
    desc.vertex.module = module;
    desc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.depthStencil = &depthStencil;
    desc.multisample.count = layout.sample_count;
    desc.fragment = &fragment;
    out.pipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &desc);
    wgpuShaderModuleRelease(module);
    if (out.pipeline == nullptr) {
        return false;
    }
    out.layout = wgpuRenderPipelineGetBindGroupLayout(out.pipeline, 0);
    return out.layout != nullptr;
}

// Render worker. Either all four pipelines exist for the current layout key or none do.
bool ensure_fog_pipelines(const GfxDrawContext& ctx) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (g_layoutValid && g_layoutKey == key) {
        return true;
    }
    release_pipelines();
    gfx_compat::ScenePassLayout layout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, layout)) {
        return false;
    }
    if (!build_pipeline(layout, true, "fs_main", g_mainPipeline) ||
        !build_pipeline(layout, false, "fs_main", g_mainDebugPipeline) ||
        !build_pipeline(layout, true, "fs_mixed", g_mixedPipeline) ||
        !build_pipeline(layout, false, "fs_mixed", g_mixedDebugPipeline))
    {
        release_pipelines();
        return false;
    }
    g_layoutKey = key;
    g_layoutValid = true;
    return true;
}

// Render worker: records the fullscreen fog triangle. Uses only the payload, the draw context and
// wgpu calls.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload) || ctx == nullptr || !ensure_fog_pipelines(*ctx)) {
        return;
    }
    DrawPayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.sceneDepth == nullptr || data.skyDepth == nullptr) {
        return;
    }

    const bool mixed = data.configIds != nullptr;
    const bool debug = data.debug_mode != 0;
    const FogPipeline& p = mixed ? (debug ? g_mixedDebugPipeline : g_mixedPipeline)
                                 : (debug ? g_mainDebugPipeline : g_mainPipeline);

    // Bindings as declared in res/fog.wgsl: 0 scene depth and 4 sky depth (both); 1 FogUniforms
    // (fs_main); 2 configuration IDs and 3 MixedFogUniforms (fs_mixed).
    WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = 4;
    entries[1].textureView = data.skyDepth;
    entries[2].binding = mixed ? 3 : 1;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniform_offset;
    entries[2].size = data.uniform_size;
    entries[3].binding = 2;
    entries[3].textureView = data.configIds;
    WGPUBindGroupDescriptor bindDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDesc.layout = p.layout;
    bindDesc.entryCount = mixed ? 4 : 3;
    bindDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindDesc);
    if (bindGroup == nullptr) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, p.pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

FogRangeUniform build_fog_range(const FogRangeAdj& adj) {
    FogRangeUniform out{};
    out.center = 2.0f * static_cast<float>(adj.center) / std::max(g_viewportWidth, 1.0f) - 1.0f;
    for (uint32_t i = 0; i < 10; ++i) {
        const uint32_t source = (i & ~1u) | (1u - (i & 1u));
        out.k[i] = static_cast<float>(adj.table[source]) / 64.0f;
    }
    out.k[10] = out.k[9];
    out.k[11] = out.k[9];
    return out;
}

// The shader-side fields of one configuration. A degenerate range (near == far or start == end)
// gives a = c = 0, which is no fog, as the game's registers give.
void fill_entry(const FogConfig& config, float color[4], float& a, float& b, float& c,
    uint32_t& fogType) {
    dusk_fog::compute_fog_coefficients(
        config.startZ, config.endZ, config.nearZ, config.farZ, a, b, c);
    fogType = (a == 0.0f && c == 0.0f) ? 2u : (config.type & 7u);
    if (config.adj.enable) {
        fogType |= kFogTypeRangeAdj;
    }
    color[0] = static_cast<float>(config.color.r) / 255.0f;
    color[1] = static_cast<float>(config.color.g) / 255.0f;
    color[2] = static_cast<float>(config.color.b) / 255.0f;
    color[3] = 1.0f;
}

// Snapshots the scene depth and pushes the fog draw: fs_mixed with the configuration-ID buffer when
// the frame needs it and the replay produced it, otherwise fs_main with slot 0.
void push_fog_quad() {
    if (g_skyDepthView == nullptr) {
        return;  // the scope does not open without it (on_scene_begin)
    }
    GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
    desc.color = false;
    desc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &desc, &resolved) != MOD_OK || resolved.depth == nullptr) {
        if (!g_warnedDepthFailure) {
            g_warnedDepthFailure = true;
            svc_log->warn(mod_ctx, "depth snapshot failed; no fog pass this frame");
        }
        return;
    }
    const uint32_t debugMode = g_debugView;
    GfxRange range{0, 0};

    if (needs_id_buffer() && g_configIdView != nullptr) {
        MixedFogUniforms uniforms{};
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            MixedFogEntry& e = uniforms.configs[i];
            fill_entry(g_frameConfigs[i], e.color, e.a, e.b, e.c, e.fog_type);
        }
        uniforms.count = g_frameConfigCount;
        uniforms.debug_mode = debugMode;
        uniforms.fallback_index = g_selfDrawnIndexValid ? g_selfDrawnIndex : 0;
        // One range table serves the frame: normally every configuration carries the same one
        // (see FogRangeAdj), so the first enabled configuration's is used. Configurations with
        // range adjustment off leave kFogTypeRangeAdj clear.
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            if (g_frameConfigs[i].adj.enable) {
                uniforms.range = build_fog_range(g_frameConfigs[i].adj);
                break;
            }
        }
        if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &range) != MOD_OK) {
            return;
        }
        const DrawPayload payload{
            resolved.depth, g_skyDepthView, g_configIdView, range.offset, range.size, debugMode};
        svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
        return;
    }

    FogUniforms uniforms{};
    fill_entry(g_frameConfigs[0], uniforms.color, uniforms.a, uniforms.b, uniforms.c,
        uniforms.fog_type);
    if (uniforms.a == 0.0f && uniforms.c == 0.0f) {
        return;
    }
    uniforms.range = build_fog_range(g_frameConfigs[0].adj);
    uniforms.debug_mode = std::min(debugMode, 1u);  // fs_main has no configuration-ID buffer
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &range) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.depth, g_skyDepthView, nullptr, range.offset, range.size,
        uniforms.debug_mode};
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
}

// ---------------------------------------------------------------------------------------------
// Frame stages
// ---------------------------------------------------------------------------------------------

// Writes the frame's configuration table to the log whenever it changes (fogLogConfigs).
char g_lastLogSignature[256] = "";

void log_fog_configs() {
    if (!get_bool_option(g_cvarLogConfigs, false)) {
        g_lastLogSignature[0] = '\0';
        return;
    }
    // A change worth logging: the number of configurations, any configuration's start or end, or
    // slot 0's type or colour.
    char signature[256];
    int n = std::snprintf(signature, sizeof(signature), "%u", g_frameConfigCount);
    if (g_frameConfigCount > 0) {
        const FogConfig& main = g_frameConfigs[0];
        n += std::snprintf(signature + n, sizeof(signature) - n, "|%u|%u,%u,%u",
            static_cast<unsigned>(main.type), static_cast<unsigned>(main.color.r),
            static_cast<unsigned>(main.color.g), static_cast<unsigned>(main.color.b));
    }
    for (uint32_t i = 0; i < g_frameConfigCount && n < static_cast<int>(sizeof(signature)); ++i) {
        n += std::snprintf(signature + n, sizeof(signature) - n, ";%.0f/%.0f",
            g_frameConfigs[i].startZ, g_frameConfigs[i].endZ);
    }
    if (std::strcmp(signature, g_lastLogSignature) == 0) {
        return;
    }
    std::snprintf(g_lastLogSignature, sizeof(g_lastLogSignature), "%s", signature);

    char line[200];
    std::snprintf(line, sizeof(line), "fog configurations this frame: %u", g_frameConfigCount);
    svc_log->info(mod_ctx, line);
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        const FogConfig& c = g_frameConfigs[i];
        std::snprintf(line, sizeof(line),
            "  slot %u: type %u, rgb(%u,%u,%u), start %.0f, end %.0f, near %.1f, far %.0f%s", i,
            static_cast<unsigned>(c.type), static_cast<unsigned>(c.color.r),
            static_cast<unsigned>(c.color.g), static_cast<unsigned>(c.color.b), c.startZ, c.endZ,
            c.nearZ, c.farZ, c.adj.enable ? ", range adjusted" : "");
        svc_log->info(mod_ctx, line);
    }
}

void update_status_line() {
    if (g_frameConfigCount == 0) {
        std::snprintf(g_statusText, sizeof(g_statusText), "No fogged draws in view");
        return;
    }
    char merged[32] = "";
    if (g_mergedDrawCount > 0) {
        std::snprintf(merged, sizeof(merged), ", %u merged", g_mergedDrawCount);
    }
    std::snprintf(g_statusText, sizeof(g_statusText),
        "Deferring fog (%u draws, %u config%s%s%s; %u shared-DL, %u fog-off "
        "(%u markable, %u by alpha/%u no-Z/%u unmarkable), %u additive/%u no-Z)",
        g_capturedDrawCount, g_frameConfigCount, g_frameConfigCount == 1 ? "" : "s", merged,
        needs_id_buffer() && g_configIdView == nullptr ? ", replay failed" : "",
        g_sharedDlFogCount, g_fogOffCount, fog_off_markable(), g_fogOffAlphaTested,
        g_fogOffNoDepth, g_fogOffUnmarkable, g_overUnityCount, g_overUnityNoDepth);
}

// GFX_STAGE_SCENE_BEGIN, after the sky lists: resets the frame, snapshots the sky's depth and opens
// the capture scope.
void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_frameConfigCount = 0;
    g_mergedDrawCount = 0;
    g_capturedDrawCount = 0;
    g_sharedDlFogCount = 0;
    g_fogOffCount = g_fogOffNoDepth = g_fogOffAlphaTested = g_fogOffUnmarkable = 0;
    g_overUnityCount = g_overUnityNoDepth = 0;
    g_configIdView = nullptr;
    g_skyDepthView = nullptr;
    g_quadArmed = false;
    g_inBgpMaterial = false;
    g_inSelfDrawnPacket = false;
    g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_scopeActive = false;
    // g_lastFrameDeferred keeps the previous frame's value until SCENE_AFTER_OPAQUE when the scope
    // opens, and is cleared here when it does not.
    if (!g_hooksOk) {
        g_lastFrameDeferred = false;
        return;  // the Status line keeps the missing hook's name
    }
    if (!get_bool_option(g_cvarEnabled, true)) {
        g_lastFrameDeferred = false;
        g_sensesExempt = g_wasSensesExempt = false;
        std::snprintf(g_statusText, sizeof(g_statusText), "Off: the game's own fog is used");
        return;
    }
    g_skipUnfogged = get_bool_option(g_cvarSkipUnfogged, true);
    g_debugView = static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0,
        static_cast<int64_t>(kDebugReplayCoverage)));
    g_sensesExempt = wolf_senses_active() && !get_bool_option(g_cvarDeferInSenses, false);
    if (g_sensesExempt != g_wasSensesExempt) {
        svc_log->info(mod_ctx, g_sensesExempt ? "Wolf Senses: the game's own fog is used"
                                              : "Wolf Senses over: deferring fog");
        g_wasSensesExempt = g_sensesExempt;
    }
    if (g_sensesExempt) {
        g_lastFrameDeferred = false;
        std::snprintf(
            g_statusText, sizeof(g_statusText), "Wolf Senses: the game's own fog is used");
        return;
    }
    // Without the sky's depth the fog pass would fog whatever the sky lists drew depth for, so the
    // scope stays closed and the game draws its own fog this frame.
    GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
    desc.color = false;
    desc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &desc, &resolved) != MOD_OK || resolved.depth == nullptr) {
        g_lastFrameDeferred = false;
        if (!g_warnedSkyDepthFailure) {
            g_warnedSkyDepthFailure = true;
            svc_log->warn(mod_ctx, "sky depth snapshot failed; such frames use the game's own fog");
        }
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Sky depth snapshot failed: the game's own fog is used");
        return;
    }
    g_skyDepthView = resolved.depth;
    g_scopeActive = true;
}

// GFX_STAGE_SCENE_AFTER_OPAQUE: closes the scope, arms the fog pass and, when the frame needs it,
// builds the configuration-ID buffer.
void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (!g_scopeActive) {
        return;
    }
    g_scopeActive = false;

    // The world viewport is current here; the range-adjustment centre is relative to its width.
    f32 viewport[6];
    GXGetViewportv(viewport);
    if (viewport[2] > 1.0f) {
        g_viewportWidth = viewport[2];
    }

    g_quadArmed = g_frameConfigCount > 0;
    g_lastFrameDeferred = g_quadArmed;

    if (g_quadArmed && needs_id_buffer()) {
        bool ok = false;
        if (draw_lists_ready()) {
            GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
            desc.color = false;
            desc.depth = true;
            GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
            if (svc_gfx->resolve_pass(mod_ctx, &desc, &resolved) == MOD_OK &&
                resolved.depth != nullptr && resolved.width > 0 && resolved.height > 0)
            {
                ok = replay_config_ids(resolved.width, resolved.height);
            }
        }
        if (!ok) {
            // The game's fog is already switched off for this frame, so the fog pass still runs,
            // with slot 0 for every pixel.
            g_configIdView = nullptr;
            if (!g_warnedReplayFailure) {
                g_warnedReplayFailure = true;
                svc_log->warn(mod_ctx,
                    "configuration-ID replay failed; such frames use one fog configuration");
            }
        }
    }

    const bool replaying = g_quadArmed && needs_id_buffer();
    if (replaying != g_wasReplaying) {
        char line[128];
        if (replaying) {
            std::snprintf(line, sizeof(line),
                "per-pixel replay on: %u fog configurations, %u markable fog-off draws in view",
                g_frameConfigCount, g_skipUnfogged ? fog_off_markable() : 0u);
        } else {
            std::snprintf(line, sizeof(line), "per-pixel replay off");
        }
        svc_log->info(mod_ctx, line);
        g_wasReplaying = replaying;
    }

    update_status_line();
    log_fog_configs();
}

// Pre-hook on dComIfGd_drawXluListBG. mDoGph_Painter calls it every frame directly after
// GFX_STAGE_SCENE_AFTER_OPAQUE, so the fog pass lands after every mod's work at that stage and
// before the translucent lists, the depth-of-field and framebuffer copies, the 2D-screen filters
// (such as the underwater one) and bloom, all of which read or redraw the frame.
HookAction on_xlu_list_bg_pre(ModContext*, void*, void*, void*) {
    if (g_quadArmed) {
        g_quadArmed = false;
        push_fog_quad();
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

UiWindowHandle g_controlsWindow = 0;

void add_control(UiElementHandle parent, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, parent, &desc, nullptr);
}

void add_toggle(UiElementHandle parent, const char* label, const char* help, ConfigVarHandle var) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    add_control(parent, control);
}

constexpr const char* kEnabledHelp =
    "Draws the game's fog after the world instead of during it, so screen-space effects from "
    "other mods, such as ambient occlusion, sit under the fog like the rest of the scenery. Off: "
    "the game draws its own fog.";

void status_get(ModContext*, void*, UiControlValue* outValue) {
    outValue->string_value = g_statusText;
}
void status_set(ModContext*, void*, const UiControlValue*) {}
bool status_disabled(ModContext*, void*) {
    return true;
}

void add_status_line(UiElementHandle parent) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = "Status";
    control.help_rml =
        "What the fog pass did this frame.<br/><b>Deferring fog</b>: how many draws had their fog "
        "taken over and how many fog configurations they used, then counts used for diagnosis. "
        "More than one configuration, or a fog-off surface for Skip Unfogged to mark, runs a "
        "per-pixel replay of the world.<br/><b>Inactive</b>: a game function this mod needs could "
        "not be hooked in this game build, so the game's own fog is used.";
    control.binding = UI_BINDING_CALLBACKS;
    control.get = status_get;
    control.set = status_set;
    control.is_disabled = status_disabled;
    add_control(parent, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    add_toggle(left, "Enabled", kEnabledHelp, g_cvarEnabled);

    add_toggle(left, "Skip Unfogged Geometry",
        "The game draws some materials with fog switched off, so they keep their own colour at any "
        "distance. The fog pass cannot tell that from depth, so with this on those surfaces are "
        "marked in a per-pixel buffer and left unfogged, as the game draws them. Off: they are "
        "fogged like everything else.<br/>A surface can be marked if it writes its own depth: the "
        "Status line's <i>markable</i> count shows how many draws in view qualify. An alpha-tested "
        "one is marked through its own alpha, so only its visible part is marked.<br/>Runs the "
        "per-pixel replay, one extra pass over the world's geometry, in every frame with a "
        "markable draw.",
        g_cvarSkipUnfogged);

    static const char* kDebugViews[] = {"Off", "Fog Factor", "Config IDs", "Replay Coverage"};
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Debug View";
    control.help_rml =
        "Replaces the image with what the fog pass computes; anything the game draws later (such "
        "as translucent objects) still draws over it. Black: pixels left to the sky's own fog (the "
        "sky, and the clouds drawn with it).<br/><b>Fog Factor</b>: the amount of fog per pixel "
        "(white = full fog).<br/><b>Config IDs</b>: which fog configuration each pixel uses, one "
        "gray level each (white = the last), on frames that run the per-pixel replay; otherwise "
        "the same as Fog Factor.<br/><b>Replay Coverage</b>: what the per-pixel replay recorded, "
        "and runs it every frame: green = a fog configuration, blue = nothing the replay draws "
        "(grass, flowers, particles and other directly drawn geometry), which takes the grass and "
        "flower configuration.<br/>Red in every view: pixels Skip Unfogged leaves unfogged.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarDebugView;
    control.options = kDebugViews;
    control.option_count = 4;
    add_control(left, control);

    add_toggle(left, "Log Fog Configs",
        "Writes the fog configurations in view to the log (each one's type, colour, and start, "
        "end, near and far distances) when their number, a start or end distance, or the main "
        "configuration's type or colour changes.",
        g_cvarLogConfigs);

    add_toggle(left, "Defer Fog During Wolf Senses (diagnostic)",
        "Leave off for play. During Wolf Senses the game uses a short black fog. Taking it over "
        "gains nothing, because black fog darkens other mods' effects and the scenery alike, so "
        "the mod leaves it to the game. This makes the mod take it over anyway, for examination "
        "with the Debug View; the result can then differ from the game's own look wherever the fog "
        "pass's one depth per pixel does not match the surface.",
        g_cvarDeferInSenses);
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
        svc_log->error(mod_ctx, "could not open the Deferred Fog controls window");
    }
}

// The mod's section in the shared Mods panel: Enabled, Status and a button for the controls window.
ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Deferred Fog");
    add_toggle(panel, "Enabled", kEnabledHelp, g_cvarEnabled);
    add_status_line(panel);
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Fog Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

// ---------------------------------------------------------------------------------------------
// Initialisation and shutdown
// ---------------------------------------------------------------------------------------------

GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;

void note_missing_hook(const char* name) {
    char line[160];
    std::snprintf(line, sizeof(line), "could not hook %s in this game build", name);
    svc_log->error(mod_ctx, line);
    if (g_missingHook == nullptr) {
        g_missingHook = name;
    }
}

template <class Entry>
void require_pre(HookPreFn callback, const char* name) {
    if (mods::hook::add_pre<Entry>(svc_hook, callback) != MOD_OK) {
        note_missing_hook(name);
    }
}

template <class Entry>
void require_post(HookPostFn callback, const char* name) {
    if (mods::hook::add_post<Entry>(svc_hook, callback) != MOD_OK) {
        note_missing_hook(name);
    }
}

// All or nothing: the fog is only right when every path the game sets fog through is captured and
// the pass lands at its one place in the frame. With any hook missing, the scope never opens, so
// hooks that did attach have nothing to do and the game draws its own fog.
void install_hooks() {
    g_missingHook = nullptr;
    require_pre<SetFog>(on_set_fog_pre, "GXSetFog");
    require_pre<SetGfFog>(on_set_fog_pre, "GFSetFog");
    require_pre<ShapeDrawFast>(on_shape_draw_pre, "J3DShape::drawFast");
    require_pre<BgpDrawSimple>(on_bgp_draw_simple_pre, "dBgp_c::modelMaterial_c::drawSimple");
    require_post<BgpDrawSimple>(on_bgp_draw_simple_post, "dBgp_c::modelMaterial_c::drawSimple");
    require_post<MaterialSharedDL>(on_material_shared_dl_post, "J3DMaterial::loadSharedDL");
    require_post<PatchedMaterialSharedDL>(
        on_material_shared_dl_post, "J3DPatchedMaterial::loadSharedDL");
    require_post<LockedMaterialSharedDL>(
        on_material_shared_dl_post, "J3DLockedMaterial::loadSharedDL");
    require_pre<GrassPacketDraw>(on_self_drawn_packet_pre, "dGrass_packet_c::draw");
    require_post<GrassPacketDraw>(on_self_drawn_packet_post, "dGrass_packet_c::draw");
    require_pre<FlowerPacketDraw>(on_self_drawn_packet_pre, "dFlower_packet_c::draw");
    require_post<FlowerPacketDraw>(on_self_drawn_packet_post, "dFlower_packet_c::draw");
    require_pre<XluListBGDraw>(on_xlu_list_bg_pre, "dComIfGd_drawXluListBG");
    g_hooksOk = g_missingHook == nullptr;
    if (!g_hooksOk) {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "Inactive: %s could not be hooked in this game build; the game's own fog is used",
            g_missingHook);
    }
}

ModResult register_bool(const char* name, bool fallback, ConfigVarHandle& out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = fallback;
    return svc_config->register_var(mod_ctx, &desc, &out);
}

ModResult register_int(const char* name, int64_t fallback, ConfigVarHandle& out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = fallback;
    return svc_config->register_var(mod_ctx, &desc, &out);
}

ModResult init(ModError* error) {
    if (svc_resource->load(mod_ctx, "fog.wgsl", &g_shaderSource) != MOD_OK ||
        g_shaderSource.data == nullptr)
    {
        return mods::set_error(error, MOD_ERROR, "could not load fog.wgsl");
    }
    // Defaults are the second argument; see docs/editing-options.md.
    if (register_bool("fogEnabled", true, g_cvarEnabled) != MOD_OK ||
        register_bool("fogSkipUnfogged", true, g_cvarSkipUnfogged) != MOD_OK ||
        register_bool("fogDeferInSenses", false, g_cvarDeferInSenses) != MOD_OK ||
        register_int("fogDebug", 0, g_cvarDebugView) != MOD_OK ||
        register_bool("fogLogConfigs", false, g_cvarLogConfigs) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "could not register options");
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "could not query the graphics device");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "deferred fog";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "could not register the fog draw");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "could not register the scene-begin stage");
    }
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc,
            &g_sceneAfterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "could not register the after-opaque stage");
    }

    install_hooks();
    return MOD_OK;
}

void shutdown() {
    svc_resource->free(mod_ctx, &g_shaderSource);
    release_pipelines();
    g_cvarEnabled = g_cvarSkipUnfogged = g_cvarDeferInSenses = g_cvarDebugView = 0;
    g_cvarLogConfigs = 0;
    g_drawType = g_sceneBeginHook = g_sceneAfterOpaqueHook = 0;
    g_controlsWindow = 0;
    g_hooksOk = false;
    g_missingHook = nullptr;
    g_scopeActive = g_quadArmed = g_replayActive = g_skipUnfogged = false;
    g_debugView = 0;
    g_sensesExempt = g_wasSensesExempt = g_wasReplaying = false;
    g_lastFrameDeferred = g_warnedReplayFailure = g_warnedDepthFailure = false;
    g_warnedSkyDepthFailure = false;
    g_inBgpMaterial = g_inSelfDrawnPacket = g_selfDrawnIndexValid = false;
    g_selfDrawnIndex = 0;
    g_frameConfigCount = g_mergedDrawCount = g_capturedDrawCount = g_sharedDlFogCount = 0;
    g_fogOffCount = g_fogOffNoDepth = g_fogOffAlphaTested = g_fogOffUnmarkable = 0;
    g_overUnityCount = g_overUnityNoDepth = 0;
    g_configIdView = g_skyDepthView = nullptr;
    g_viewportWidth = 640.0f;
    g_lastLogSignature[0] = '\0';
    std::snprintf(g_statusText, sizeof(g_statusText), "Waiting for the first frame");
}

// The exported service (include/deferred_fog_service.h).
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
    .header =
        SERVICE_HEADER(DeferredFogService, DEFERRED_FOG_SERVICE_MAJOR, DEFERRED_FOG_SERVICE_MINOR),
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
    svc_log->info(mod_ctx, g_hooksOk ? "ready" : "inactive: a required game hook is missing");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    shutdown();
    return MOD_OK;
}
}
