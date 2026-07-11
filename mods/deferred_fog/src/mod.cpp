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
#include "dolphin/gx/GXPixel.h"
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

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarDebugView = 0;

GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_fogPipeline = nullptr;       // (srcAlpha, 1-srcAlpha) blend
WGPURenderPipeline g_fogDebugPipeline = nullptr;  // no blend (fog factor view)
WGPUBindGroupLayout g_fogLayout = nullptr;
WGPUBindGroupLayout g_fogDebugLayout = nullptr;

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

struct DrawPayload {
    WGPUTextureView sceneDepth;  // frame-pooled
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

// Vote a fogged draw's configuration against the frame reference. Returns true when the
// draw's fog should be suppressed (deferred).
bool vote_config(const FogConfig& config) {
    if (!g_reference.valid) {
        g_reference = config;
        g_reference.valid = true;
    }
    if (!config_matches(g_reference, config)) {
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

    WGPURenderPipeline pipeline = data.debug_mode != 0 ? g_fogDebugPipeline : g_fogPipeline;
    WGPUBindGroupLayout layout = data.debug_mode != 0 ? g_fogDebugLayout : g_fogLayout;
    if (data.sceneDepth == nullptr || pipeline == nullptr) {
        return;
    }

    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = 1;
    entries[1].buffer = ctx->uniform_buffer;
    entries[1].offset = data.uniform_offset;
    entries[1].size = data.uniform_size;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = 2;
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
    const auto debugMode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 1));
    uniforms.debug_mode = debugMode;

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }
    const DrawPayload payload{resolved.depth, uniformRange.offset, uniformRange.size, debugMode};
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
}

void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_reference = FogConfig{};
    g_suppressedCount = 0;
    g_deviantCount = 0;
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
    g_quadArmed = g_suppressedCount > 0 && g_reference.valid;
    // One configuration this frame -> keep (or start) deferring next frame. Mixed
    // configurations -> exact vanilla fog from the next frame until the scene is uniform.
    g_suppressAllowed = g_deviantCount == 0 && effect_enabled();
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

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.help_rml =
        "Applies the game's fog after other mods' screen-space effects (AO, shadows) instead "
        "of during world drawing, so those effects darken the surfaces under the fog rather "
        "than the fog itself. Scenes mixing several fog configurations fall back to the "
        "vanilla fog path automatically.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarEnabled;
    add_control(panel, control);

    static const char* kDebugOptions[] = {"Off", "Fog Factor"};
    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Debug View";
    control.help_rml = "Fog Factor: the deferred fog term as grayscale (white = full fog).";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarDebugView;
    control.options = kDebugOptions;
    control.option_count = 2;
    add_control(panel, control);
    return MOD_OK;
}

bool build_fog_pipeline(
    bool blend, WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
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
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
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
    cvarDesc.name = "debugView";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 0;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarDebugView) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register fog option");
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_fog_pipeline(true, g_fogPipeline, g_fogLayout) ||
        !build_fog_pipeline(false, g_fogDebugPipeline, g_fogDebugLayout))
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

    if (dusk::mods::hook_add_pre<GXSetFog>(svc_hook, on_set_fog_pre) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook GXSetFog");
    }
    // Virtual: resolves through the symbol manifest. Without it, J3D fog can't be deferred,
    // so the mod stays loaded but inert (with vanilla fog) rather than failing.
    g_shapeHookOk =
        dusk::mods::hook_add_pre<&J3DShape::drawFast>(svc_hook, on_shape_draw_pre) == MOD_OK;
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
    if (g_fogPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_fogPipeline);
        g_fogPipeline = nullptr;
    }
    if (g_fogDebugPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_fogDebugPipeline);
        g_fogDebugPipeline = nullptr;
    }
    if (g_fogLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_fogLayout);
        g_fogLayout = nullptr;
    }
    if (g_fogDebugLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_fogDebugLayout);
        g_fogDebugLayout = nullptr;
    }
    g_cvarEnabled = g_cvarDebugView = 0;
    g_drawType = g_sceneBeginHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_scopeActive = g_quadArmed = g_suppressAllowed = g_shapeHookOk = false;
    g_reference = FogConfig{};
    g_suppressedCount = g_deviantCount = 0;
    return MOD_OK;
}
}
