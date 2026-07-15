// Deferred Fog.
//
// Standalone helper mod for screen-space visual enhancements: the game applies fog per
// fragment while drawing (each J3D material bakes fog BP state into its display list; a few
// direct drawers call GXSetFog), which means any mod compositing over the opaque scene - AO,
// deferred shadows - darkens the *fogged* color, visibly dimming the fog/aerial perspective
// instead of the surfaces under it. This mod suppresses fog while the opaque world lists
// draw, captures the live fog parameters, and re-applies fog as a fullscreen pass pushed
// right before the first J3D translucent geometry draws (with a FRAME_BEFORE_HUD fallback) -
// after every mod's SCENE_AFTER_OPAQUE composites regardless of mod load order, and before
// water, particles, and bloom, which keep their native forward fog. Other mods need no
// changes and no awareness of this mod to benefit.
//
// The re-apply trigger deliberately avoids hooking the painter's own list functions: those
// are inlinable into their callsites, where a detour never fires. J3DShape::drawFast (already
// hooked for suppression) is the anchor instead - the first shape drawn after the opaque
// stage IS the first translucent geometry.
//
// The re-applied fog is an exact reproduction, not an approximation: aurora's per-fragment
// fog input is the raw depth value (the same value in the depth snapshot), and mod.cpp
// mirrors the exact J3DGDSetFog BP encode -> aurora command-processor decode round trip for
// the (a, b, c) coefficients, quantization included. See res/fog.wgsl.
//
// Special fog handling: materials are suppressed only when their fog matches the frame's
// reference configuration (the first fogged draw, normally stage geometry). Frames that mix
// configurations - twilight black fog (authored type 7, converted by d_kankyo to linear
// black), wolf-senses white fog (type 6), room-blend transitions - disable suppression from
// the next frame on, reverting to exact vanilla forward fog until the scene is uniform
// again. Mixed content is never flattened onto one config for more than the one transition
// frame.

#include "global.h"

#include "fog_math.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
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
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

namespace {

// Hook targets declared at namespace scope (each emits a modmeta hook record the host resolves at
// load); the generated aliases are passed to hook_add_pre in mod_initialize. GXSetFog / GFSetFog
// share a signature but are distinct functions, so each gets its own alias.
DEFINE_HOOK(GXSetFog, SetFog);
DEFINE_HOOK(GFSetFog, SetGfFog);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarMixedMode = 0;
ConfigVarHandle g_cvarDebugView = 0;

UiWindowHandle g_controlsWindow = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_fogPipeline = nullptr;       // (srcAlpha, 1-srcAlpha) blend
WGPURenderPipeline g_fogDebugPipeline = nullptr;  // no blend (fog factor view)
WGPURenderPipeline g_mixedPipeline = nullptr;      // per-pixel config selection, blended
WGPURenderPipeline g_mixedDebugPipeline = nullptr; // per-pixel config selection, unblended
WGPUBindGroupLayout g_fogLayout = nullptr;
WGPUBindGroupLayout g_fogDebugLayout = nullptr;
WGPUBindGroupLayout g_mixedLayout = nullptr;
WGPUBindGroupLayout g_mixedDebugLayout = nullptr;

// One fog configuration as the game specifies it (J3DFogInfo fields / GXSetFog arguments).
struct FogConfig {
    bool valid = false;
    uint8_t type = 0;  // GXFogType; only the low 3 bits reach aurora's fog state
    float startZ = 0.0f;
    float endZ = 0.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    GXColor color{0, 0, 0, 0};
};

// Per-frame suppression state (game thread only).
bool g_scopeActive = false;      // between SCENE_BEGIN and SCENE_AFTER_OPAQUE
bool g_quadArmed = false;        // push the fog quad at the next J3D shape draw (first xlu)
bool g_suppressAllowed = false;  // last completed frame saw exactly one fog configuration
bool g_shapeHookOk = false;      // J3DShape::drawFast hook installed (needs the vtable symbol)
bool g_warnedPushFailure = false;
FogConfig g_reference;           // first fogged configuration seen this frame
uint32_t g_suppressedCount = 0;  // draws whose fog was deferred this frame
uint32_t g_deviantCount = 0;     // draws whose fog did not match the reference

// Revert diagnostics: the silent fall-back to vanilla fog on mixed configurations is invisible
// in-game (other mods' effects then darken the fog itself), so surface it - a log line per state
// transition and a live status string for the panel.
bool g_wasSuppressing = false;   // previous frame's suppression verdict (transition detection)
FogConfig g_firstDeviant;        // first non-matching configuration this frame (for the log)
char g_statusText[160] = "Waiting for first fogged frame";

// Exact mixed-config mode: every fogged draw is suppressed and its configuration is captured
// into this per-frame table (matched with the config_matches tolerances). When the frame holds
// more than one config, the opaque lists are replayed once with each shape's output forced to a
// flat ID color, and the fog quad selects each pixel's exact config from the resulting buffer.
constexpr uint32_t kMaxFogConfigs = 8;
FogConfig g_frameConfigs[kMaxFogConfigs];
uint32_t g_frameConfigCount = 0;
bool g_fogReplayActive = false;          // the opaque lists are replaying for the ID buffer
WGPUTextureView g_configIdView = nullptr;  // frame-pooled ID buffer (mixed frames only)
bool g_wasMixed = false;                 // last frame held >1 config (transition logging)
bool g_warnedReplayFailure = false;

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
};
static_assert(sizeof(FogUniforms) % 16 == 0);

// Mirror of the WGSL MixedFogEntry / MixedFogUniforms (keep in sync with res/fog.wgsl).
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
    float _pad0;
    float _pad1;
};
static_assert(sizeof(MixedFogUniforms) % 16 == 0);

struct DrawPayload {
    WGPUTextureView sceneDepth;  // frame-pooled
    WGPUTextureView configIds;   // frame-pooled; non-null selects the mixed pipeline
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
    return get_bool_option(g_cvarEnabled, true) && g_shapeHookOk;
}

// Configurations within these tolerances are treated as the same fog: palette interpolation
// can leave actors a blend step apart from the stage without a visible difference. Anything
// beyond them is a deviant (special fog) and turns suppression off for following frames.
bool config_matches(const FogConfig& reference, const FogConfig& candidate) {
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

bool exact_mode() {
    return get_int_option(g_cvarMixedMode, 1) == 1;
}

// Find (or register) a configuration in the per-frame table. Returns its index; a table
// overflow (more than kMaxFogConfigs distinct configs in one frame - not seen in practice)
// falls back to index 0, the frame's reference config.
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

// Read-only lookup for the replay's ID override (the table is complete by replay time).
uint32_t lookup_frame_config(const FogConfig& config) {
    for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
        if (config_matches(g_frameConfigs[i], config)) {
            return i;
        }
    }
    return 0;
}

// Vote a fogged draw's configuration against the frame reference. Returns true when the
// draw's fog should be suppressed (deferred).
bool vote_config(const FogConfig& config) {
    if (!g_reference.valid) {
        g_reference = config;
        g_reference.valid = true;
    }
    if (exact_mode()) {
        // Exact mixed mode: every fogged draw is deferred; the config table + ID replay put
        // the right configuration back per pixel. Deviant counting stays for the status line.
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

// Game thread, per J3D shape draw.
//
// While the opaque world lists draw (the suppression scope): J3D materials never call
// GXSetFog - their fog is baked into the material display list's BP writes (which aurora's
// command processor replays into per-draw fog state). The material display list has already
// executed when the shape draws, so an immediate GXSetFog(GX_FOG_NONE) here overrides it
// for this shape's geometry - and the material's true parameters are readable off its PE
// block for capture. Same interception pattern as Realtime Sun Shadows' two-sided casters.
//
// After the opaque stage (quad armed): the first shape drawn is the first translucent
// geometry - water included - so the deferred fog pushes here, under it.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        // Config-ID replay: force this shape's output to a flat sparse-encoded index color
        // (see res/fog.wgsl). The material display list has already executed, so these GX
        // writes override its TEV/channel/fog state for this shape only; j3dSys.reinitGX()
        // after the replay clears any leaked state. Alpha-tested cutouts replay solid (the
        // constant alpha always passes) - a leaf hole gets its tree's config, which is
        // visually negligible since fog varies smoothly.
        const J3DShape* shape = dusk::mods::arg<const J3DShape*>(args, 0);
        J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
        J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
        J3DFog* fog = peBlock != nullptr ? peBlock->getFog() : nullptr;
        uint32_t index = 0;  // unfogged / unknown -> reference config at the quad
        if (fog != nullptr && fog->mType != 0) {
            FogConfig config;
            config.type = fog->mType;
            config.startZ = fog->mStartZ;
            config.endZ = fog->mEndZ;
            config.nearZ = fog->mNearZ;
            config.farZ = fog->mFarZ;
            config.color = fog->mColor;
            index = lookup_frame_config(config);
        }
        const auto idByte = static_cast<u8>((index + 1) * 24);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL,
            GX_DF_NONE, GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0, GXColor{idByte, 0, 0, 255});
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
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
    const J3DShape* shape = dusk::mods::arg<const J3DShape*>(args, 0);
    J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
    J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
    J3DFog* fog = peBlock != nullptr ? peBlock->getFog() : nullptr;
    if (fog == nullptr || fog->mType == 0) {
        return HOOK_CONTINUE;
    }
    FogConfig config;
    config.type = fog->mType;
    config.startZ = fog->mStartZ;
    config.endZ = fog->mEndZ;
    config.nearZ = fog->mNearZ;
    config.farZ = fog->mFarZ;
    config.color = fog->mColor;
    if (vote_config(config)) {
        GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, GXColor{0, 0, 0, 0});
    }
    return HOOK_CONTINUE;
}

// Game thread: direct (non-J3D) drawers set fog through this call; rewrite the type to
// GX_FOG_NONE when the configuration matches the frame reference.
HookAction on_set_fog_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        // Direct (non-J3D) drawers during the ID replay: kill their fog; their color output is
        // not a valid ID (decodes as invalid -> reference config at the quad).
        dusk::mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
        return HOOK_CONTINUE;
    }
    if (!g_scopeActive) {
        return HOOK_CONTINUE;
    }
    const auto type = dusk::mods::arg<GXFogType>(args, 0);
    if (type == GX_FOG_NONE) {
        return HOOK_CONTINUE;
    }
    FogConfig config;
    config.type = static_cast<uint8_t>(type);
    config.startZ = dusk::mods::arg<float>(args, 1);
    config.endZ = dusk::mods::arg<float>(args, 2);
    config.nearZ = dusk::mods::arg<float>(args, 3);
    config.farZ = dusk::mods::arg<float>(args, 4);
    config.color = dusk::mods::arg<GXColor>(args, 5);
    if (vote_config(config)) {
        dusk::mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
    }
    return HOOK_CONTINUE;
}

// Render worker thread: fullscreen fog blend over the scene.
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
    // The single-config path binds FogUniforms at 1; the mixed path binds the config-ID
    // texture at 2 and MixedFogUniforms at 3 (matching each entry point's bindings).
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

// Game thread: resolve the opaque scene depth and push the fog quad at the current point in
// the command stream (the top of the translucent phase).
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
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 2));

    // Mixed frame with a valid config-ID buffer: per-pixel config selection.
    if (exact_mode() && g_frameConfigCount > 1 && g_configIdView != nullptr) {
        MixedFogUniforms uniforms{};
        for (uint32_t i = 0; i < g_frameConfigCount; ++i) {
            const FogConfig& config = g_frameConfigs[i];
            MixedFogEntry& entry = uniforms.configs[i];
            dusk_fog::compute_fog_coefficients(
                config.startZ, config.endZ, config.nearZ, config.farZ, entry.a, entry.b, entry.c);
            if (entry.a == 0.0f && entry.c == 0.0f) {
                // Degenerate range: force the zero-fog linear curve (a=0,c=0 with LIN yields 0).
                entry.fog_type = 2u;
            } else {
                entry.fog_type = config.type & 7u;
            }
            entry.color[0] = static_cast<float>(config.color.r) / 255.0f;
            entry.color[1] = static_cast<float>(config.color.g) / 255.0f;
            entry.color[2] = static_cast<float>(config.color.b) / 255.0f;
            entry.color[3] = 1.0f;
        }
        uniforms.count = g_frameConfigCount;
        uniforms.debug_mode = debugMode;
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
        // Degenerate range (start == end or near == far): the vanilla term is zero
        // everywhere except a 0/0 singularity; there is no fog to re-apply.
        return;
    }
    uniforms.color[0] = static_cast<float>(g_reference.color.r) / 255.0f;
    uniforms.color[1] = static_cast<float>(g_reference.color.g) / 255.0f;
    uniforms.color[2] = static_cast<float>(g_reference.color.b) / 255.0f;
    uniforms.color[3] = 1.0f;
    // Aurora's command processor keeps only 3 bits of the fog type (ortho variants collapse
    // onto their perspective curve); match that.
    uniforms.fog_type = g_reference.type & 7u;
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

// Game thread: replay the opaque draw lists once into a mod-owned offscreen pass, with every
// shape's output forced (by on_shape_draw_pre) to a flat config-index color. The resolved color
// is the frame's per-pixel config-ID buffer. Same save-replay-resolve bracket as the shadow
// mod's cascades, minus the light-space camera override - the game's own view/projection stay
// in place, so the replay rasterizes the same visibility the main pass produced.
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

    // Stale-model guard (see the shadow mod): shapes drawn outside a live packet would leave
    // j3dSys's model pointer dangling across scene teardowns.
    J3DModel* savedModel = j3dSys.getModel();
    j3dSys.setModel(nullptr);
    g_fogReplayActive = true;
    draw_opaque_scene_lists();
    g_fogReplayActive = false;
    j3dSys.setModel(savedModel);
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
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

void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = 0;
    g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_configIdView = nullptr;
    g_quadArmed = false;
    // The sky lists draw before this stage and keep their own fog untouched.
    g_scopeActive = effect_enabled();
    if (!g_scopeActive) {
        g_suppressAllowed = false;
    }
}

// Game thread, right after the last opaque list: end the suppression scope (the next J3D
// shape drawn is translucent geometry) and arm the deferred fog push. The quad itself pushes
// later - at the first translucent shape draw, or the FRAME_BEFORE_HUD fallback - so it
// lands after every mod's SCENE_AFTER_OPAQUE composites regardless of stage-callback order.
void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (!g_scopeActive) {
        return;
    }
    g_scopeActive = false;
    const bool exact = exact_mode();
    g_quadArmed = (g_suppressedCount > 0 || (exact && g_frameConfigCount > 0)) &&
                  g_reference.valid;
    // Vanilla mode: one configuration this frame -> keep (or start) deferring next frame; mixed
    // -> exact vanilla fog from the next frame until the scene is uniform. Exact mode: always
    // defer; the ID replay below reconstructs per-pixel configs for mixed frames.
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
            g_configIdView = nullptr;  // quad falls back to the reference config everywhere
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

    // Diagnostics: log the revert/re-engage transitions (invisible in-game otherwise) and keep
    // the panel status line current. Log volume is bounded by transitions, not frames.
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
            "Deferring fog (exact: %u draws, %u config%s%s)", g_suppressedCount + g_deviantCount,
            g_frameConfigCount, g_frameConfigCount == 1 ? "" : "s",
            g_frameConfigCount > 1 && g_configIdView == nullptr ? ", replay failed" : "");
    } else if (g_deviantCount > 0) {
        std::snprintf(g_statusText, sizeof(g_statusText),
            "REVERTED: mixed fog configs (%u matching / %u deviant)", g_suppressedCount,
            g_deviantCount);
    } else {
        std::snprintf(g_statusText, sizeof(g_statusText), "Deferring fog (%u draws this frame)",
            g_suppressedCount);
    }
}

// Game thread, after the full 3D scene: fallback for frames without any translucent J3D
// geometry - late fog (over translucency/bloom, vanilla-incorrect for one frame) beats
// suppressed fog never coming back. Also guarantees the armed flag never crosses frames.
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

// Live status readout (game thread; polled per frame while visible).
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
    control.config_var = g_cvarEnabled;
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

// The tab's left pane holds the SELECT controls: SELECT is only supported where a help pane
// exists (window tabs), never in the flat mods panel, so these must live here to render at all.
ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;
    add_enabled_toggle(left);

    static const char* kMixedOptions[] = {"Vanilla", "Exact (replay)"};
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Mixed Scenes";
    control.help_rml =
        "How scenes that draw with several fog configurations are handled.<br/>Vanilla: fall "
        "back to the game's forward fog (exact, but AO/shadows then darken the fog itself at "
        "range).<br/>Exact (replay): defer every configuration - the opaque geometry is "
        "replayed once into a per-pixel config-ID buffer and each pixel gets its own exact fog. "
        "Costs one extra opaque scene of per-frame geometry streaming on mixed frames; with "
        "heavy shadow-cascade settings this can crowd the engine's fixed streaming buffers, so "
        "prefer Vanilla there until the adaptive-buffer engine update lands.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarMixedMode;
    control.options = kMixedOptions;
    control.option_count = 2;
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
    control.config_var = g_cvarDebugView;
    control.options = kDebugOptions;
    control.option_count = 3;
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
    tabs[0].title = "Controls";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open Deferred Fog controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    add_enabled_toggle(panel);
    add_status_line(panel);

    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
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

    // mix(dst, fogColor, fogZ): fog color in rgb, fog factor in alpha, standard alpha
    // blending; the target's alpha channel stays untouched (forward fog never wrote alpha).
    WGPUBlendState blendState{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_SrcAlpha,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = g_deviceInfo.color_format;
    if (blend) {
        colorTarget.blend = &blendState;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {entryPoint, WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = g_deviceInfo.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {blend ? "deferred fog" : "deferred fog (debug)", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = &depthStencil;
    pipelineDesc.multisample.count = g_deviceInfo.sample_count;
    pipelineDesc.fragment = &fragment;
    outPipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (outPipeline == nullptr) {
        return false;
    }
    outLayout = wgpuRenderPipelineGetBindGroupLayout(outPipeline, 0);
    return outLayout != nullptr;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "fog.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return dusk::mods::set_error(error, result, "failed to load fog.wgsl");
    }

    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "effectEnabled";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarEnabled) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "mixedMode";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 1;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarMixedMode) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "debugView";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 0;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarDebugView) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_fog_pipeline(true, "fs_main", g_fogPipeline, g_fogLayout) ||
        !build_fog_pipeline(false, "fs_main", g_fogDebugPipeline, g_fogDebugLayout) ||
        !build_fog_pipeline(true, "fs_mixed", g_mixedPipeline, g_mixedLayout) ||
        !build_fog_pipeline(false, "fs_mixed", g_mixedDebugPipeline, g_mixedDebugLayout))
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to create fog pipeline");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "deferred fog";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register draw type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_sceneAfterOpaqueHook) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    if (dusk::mods::hook_add_pre<SetFog>(svc_hook, on_set_fog_pre) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook GXSetFog");
    }
    // Environment/packet fog (grass, flowers, the dKy tevstr path) is programmed through
    // GFSetFog - a direct BP-register write, not GXSetFog - and the geometry that uses it draws
    // outside J3DShape::drawFast, so it evades both other interception points. GFSetFog has the
    // identical signature to GXSetFog, so the same callback captures and suppresses it; without
    // this hook that geometry keeps its forward fog and the deferred quad double-fogs it. Only
    // one call site in the game (the grass/flower fog helper), so normal terrain is untouched.
    if (dusk::mods::hook_add_pre<SetGfFog>(svc_hook, on_set_fog_pre) != MOD_OK) {
        svc_log->warn(mod_ctx,
            "failed to hook GFSetFog; grass/flower fog will not be deferred (double-fogged)");
    }
    // Virtual: resolves through the symbol manifest. Without it, J3D fog can't be deferred,
    // so the mod stays loaded but inert (with vanilla fog) rather than failing.
    g_shapeHookOk =
        dusk::mods::hook_add_pre<ShapeDrawFast>(svc_hook, on_shape_draw_pre) == MOD_OK;
    if (!g_shapeHookOk) {
        svc_log->warn(mod_ctx,
            "failed to hook J3DShape::drawFast (missing dusklight.symdb?); deferred fog is "
            "disabled");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
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
    g_cvarEnabled = g_cvarMixedMode = g_cvarDebugView = 0;
    g_controlsWindow = 0;
    g_drawType = g_sceneBeginHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_scopeActive = g_quadArmed = g_suppressAllowed = g_shapeHookOk = g_wasSuppressing = false;
    g_fogReplayActive = g_wasMixed = g_warnedReplayFailure = false;
    g_reference = FogConfig{};
    g_firstDeviant = FogConfig{};
    g_suppressedCount = g_deviantCount = 0;
    g_frameConfigCount = 0;
    g_configIdView = nullptr;
    std::snprintf(g_statusText, sizeof(g_statusText), "Waiting for first fogged frame");
    return MOD_OK;
}
}
