// Graphics Hub — a combined host for the screen-space infrastructure that other graphics mods
// build on. It bundles two independently-toggleable sub-features that share the goal of making
// added effects layer CORRECTLY over the game's original rendering rather than over-applying on
// top of it:
//
//   * Depth to Normal — reconstructs a per-pixel world-space surface normal (+ raw depth) from the
//     scene depth buffer once per frame and PUBLISHES it as a mod service (id
//     "dev.automata.depth_to_normal") that AO / GI / shadow mods consume. It is passive
//     infrastructure: it only does work when a consumer asks, so it has no on/off, just a debug
//     view. (SSILVB and Realtime Sun Shadows require this service — keep Graphics Hub enabled.)
//
//   * Deferred Fog — moves the game's fog to AFTER the opaque scene so screen-space effects darken
//     the world UNDER the fog instead of the fog itself. Independently toggleable.
//
// This file merges the two former standalone mods (depth_to_normal + deferred_fog) verbatim, each
// inside its own namespace (hub_dtn / hub_fog) so they keep separate state; the service imports and
// the mod entry points are shared. To change a default or drop a control, edit the sub-namespace's
// init()/build_section() — see docs/self_editing_guide.md.
//
// Game-linked (Deferred Fog hooks game functions) + webgpu.

#include "global.h"

#include "fog_math.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "dolphin/gf/GFPixel.h"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXLighting.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTev.h"

#include "depth_to_normal_service.h"
#include "water_plane_service.h"

#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
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
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
// Union of both sub-features' service needs, imported once and shared.
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

// ===========================================================================================
// SUB-FEATURE: Depth to Normal   (world-space normal provider service)
// ===========================================================================================
namespace hub_dtn {

ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_layout = nullptr;
GfxComputeTypeHandle g_computeType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;

ResourceBuffer g_debugShaderSource = RESOURCE_BUFFER_INIT;
WGPURenderPipeline g_debugPipeline = nullptr;
WGPUBindGroupLayout g_debugLayout = nullptr;
GfxDrawTypeHandle g_debugDrawType = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
ConfigVarHandle g_cvarNormalsDebug = 0;  // DEFAULT below in init()

struct NormalTarget {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
};
NormalTarget g_target;
struct RetiredTarget {
    NormalTarget target;
    int framesLeft = 0;
};
std::vector<RetiredTarget> g_retired;

bool g_cameraValid = false;
float g_viewFromProj[16] = {};
float g_worldFromView[16] = {};
bool g_frameComputed = false;
WGPUTextureView g_frameView = nullptr;
uint32_t g_frameWidth = 0;
uint32_t g_frameHeight = 0;

// Mirror of the WGSL Uniforms struct (keep in sync with res/reconstruct.wgsl).
struct ReconstructUniforms {
    float view_from_proj[16];
    float world_from_view[16];
    float inv_size[2];
    float _pad0[2];
};
static_assert(sizeof(ReconstructUniforms) == 144);
static_assert(sizeof(ReconstructUniforms) % 16 == 0);

struct ComputePayload {
    WGPUTextureView sceneDepth;
    WGPUTextureView normalOut;
    uint32_t uniformOffset;
    uint32_t uniformSize;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<ComputePayload>);

struct DebugDrawPayload {
    WGPUTextureView normal;
};
static_assert(sizeof(DebugDrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DebugDrawPayload>);

constexpr uint32_t div_ceil(uint32_t n, uint32_t d) { return (n + d - 1) / d; }

void release_target(NormalTarget& t) {
    if (t.view != nullptr) {
        wgpuTextureViewRelease(t.view);
        t.view = nullptr;
    }
    if (t.texture != nullptr) {
        wgpuTextureRelease(t.texture);
        t.texture = nullptr;
    }
    t.width = t.height = 0;
}

void tick_retired() {
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (--it->framesLeft <= 0) {
            release_target(it->target);
            it = g_retired.erase(it);
        } else {
            ++it;
        }
    }
}

bool ensure_target(uint32_t width, uint32_t height) {
    if (g_target.width == width && g_target.height == height && g_target.view != nullptr) {
        return true;
    }
    if (g_target.width != 0) {
        g_retired.push_back(RetiredTarget{std::exchange(g_target, NormalTarget{}), 4});
    }
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = {"depth-to-normal world normal", WGPU_STRLEN};
    desc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    desc.size = {width, height, 1};
    desc.format = WGPUTextureFormat_RGBA32Float;
    g_target.texture = wgpuDeviceCreateTexture(g_deviceInfo.device, &desc);
    if (g_target.texture != nullptr) {
        g_target.view = wgpuTextureCreateView(g_target.texture, nullptr);
    }
    if (g_target.view == nullptr) {
        release_target(g_target);
        return false;
    }
    g_target.width = width;
    g_target.height = height;
    return true;
}

bool build_pipeline() {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"depth-to-normal reconstruct", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {"depth-to-normal reconstruct", WGPU_STRLEN};
    desc.compute.module = module;
    desc.compute.entryPoint = {"reconstruct", WGPU_STRLEN};
    g_pipeline = wgpuDeviceCreateComputePipeline(g_deviceInfo.device, &desc);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr) {
        return false;
    }
    g_layout = wgpuComputePipelineGetBindGroupLayout(g_pipeline, 0);
    return g_layout != nullptr;
}

bool build_debug_pipeline() {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_debugShaderSource.data), g_debugShaderSource.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"depth-to-normal debug", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = g_deviceInfo.color_format;
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
    pipelineDesc.label = {"depth-to-normal debug", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = &depthStencil;
    pipelineDesc.multisample.count = g_deviceInfo.sample_count;
    pipelineDesc.fragment = &fragment;
    g_debugPipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_debugPipeline == nullptr) {
        return false;
    }
    g_debugLayout = wgpuRenderPipelineGetBindGroupLayout(g_debugPipeline, 0);
    return g_debugLayout != nullptr;
}

void on_debug_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DebugDrawPayload)) {
        return;
    }
    DebugDrawPayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.normal == nullptr || g_debugPipeline == nullptr) {
        return;
    }
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = 0;
    entry.textureView = data.normal;
    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = g_debugLayout;
    bgDesc.entryCount = 1;
    bgDesc.entries = &entry;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(ctx->device, &bgDesc);
    if (group == nullptr) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, g_debugPipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(group);
}

void on_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(ComputePayload)) {
        return;
    }
    ComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.sceneDepth == nullptr || data.normalOut == nullptr || g_pipeline == nullptr) {
        return;
    }
    WGPUBindGroupEntry entries[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = 1;
    entries[1].textureView = data.normalOut;
    entries[2].binding = 2;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniformOffset;
    entries[2].size = data.uniformSize;
    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = g_layout;
    bgDesc.entryCount = 3;
    bgDesc.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(ctx->device, &bgDesc);
    if (group == nullptr) {
        return;
    }
    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"depth-to-normal", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    wgpuComputePassEncoderSetPipeline(pass, g_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, div_ceil(data.width, 8), div_ceil(data.height, 8), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

void on_scene_begin(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired();
    g_frameComputed = false;
    g_frameView = nullptr;
    g_cameraValid = false;
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext) ||
        stageCtx->game_view == nullptr)
    {
        return;
    }
    CameraInfo info = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &info) != MOD_OK) {
        return;
    }
    std::memcpy(g_viewFromProj, info.view_from_proj, sizeof(g_viewFromProj));
    std::memcpy(g_worldFromView, info.world_from_view, sizeof(g_worldFromView));
    g_cameraValid = true;
}

// The exported service entry point (see g_dtnService below).
ModResult get_frame(ModContext*, DepthToNormalFrame* out) {
    if (out == nullptr || out->struct_size < sizeof(DepthToNormalFrame)) {
        return MOD_INVALID_ARGUMENT;
    }
    out->normal = nullptr;
    out->width = 0;
    out->height = 0;
    if (!g_cameraValid || g_pipeline == nullptr) {
        return MOD_UNAVAILABLE;
    }
    if (!g_frameComputed) {
        GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
        resolveDesc.color = false;
        resolveDesc.depth = true;
        GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
            resolved.depth == nullptr || resolved.width == 0 || resolved.height == 0)
        {
            return MOD_UNAVAILABLE;
        }
        if (!ensure_target(resolved.width, resolved.height)) {
            return MOD_UNAVAILABLE;
        }
        ReconstructUniforms uniforms{};
        std::memcpy(uniforms.view_from_proj, g_viewFromProj, sizeof(uniforms.view_from_proj));
        std::memcpy(uniforms.world_from_view, g_worldFromView, sizeof(uniforms.world_from_view));
        uniforms.inv_size[0] = 1.0f / static_cast<float>(resolved.width);
        uniforms.inv_size[1] = 1.0f / static_cast<float>(resolved.height);
        GfxRange range{0, 0};
        if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &range) != MOD_OK) {
            return MOD_UNAVAILABLE;
        }
        ComputePayload payload{};
        payload.sceneDepth = resolved.depth;
        payload.normalOut = g_target.view;
        payload.uniformOffset = range.offset;
        payload.uniformSize = range.size;
        payload.width = resolved.width;
        payload.height = resolved.height;
        if (svc_gfx->push_compute(mod_ctx, g_computeType, &payload, sizeof(payload)) != MOD_OK) {
            return MOD_UNAVAILABLE;
        }
        g_frameView = g_target.view;
        g_frameWidth = resolved.width;
        g_frameHeight = resolved.height;
        g_frameComputed = true;
    }
    out->normal = g_frameView;
    out->width = g_frameWidth;
    out->height = g_frameHeight;
    return MOD_OK;
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    bool debugOn = false;
    if (g_cvarNormalsDebug == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarNormalsDebug, &debugOn) != MOD_OK || !debugOn)
    {
        return;
    }
    DepthToNormalFrame frame = DEPTH_TO_NORMAL_FRAME_INIT;
    if (get_frame(mod_ctx, &frame) != MOD_OK || frame.normal == nullptr) {
        return;
    }
    DebugDrawPayload payload{};
    payload.normal = frame.normal;
    svc_gfx->push_draw(mod_ctx, g_debugDrawType, &payload, sizeof(payload));
}

// Adds this sub-feature's section to the shared mods panel.
void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Depth to Normal");
    svc_ui->pane_add_text(mod_ctx, panel,
        "Reconstructs a world-space surface normal from the depth buffer each frame and provides "
        "it to other mods (AO, GI, shadows). Passive: no on/off, just the debug view below.",
        nullptr);
    UiControlDesc debugToggle = UI_CONTROL_DESC_INIT;
    debugToggle.kind = UI_CONTROL_TOGGLE;
    debugToggle.label = "Show Normals";
    debugToggle.help_rml =
        "Draws the reconstructed world-space normal buffer over the whole screen at the very end "
        "of the frame (after every other effect), as a diagnostic. World normal XYZ maps to RGB; "
        "sky / invalid pixels are black.";
    debugToggle.binding = UI_BINDING_CONFIG_VAR;
    debugToggle.config_var = g_cvarNormalsDebug;
    svc_ui->pane_add_control(mod_ctx, panel, &debugToggle, nullptr);
}

ModResult init(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "reconstruct.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return mods::set_error(error, result, "failed to load reconstruct.wgsl");
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_pipeline()) {
        return mods::set_error(error, MOD_ERROR, "failed to create reconstruct pipeline");
    }
    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "depth-to-normal reconstruct";
    computeDesc.callback = on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    // DEFAULT: normals debug view off.
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "normalsDebug";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = false;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarNormalsDebug) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register normalsDebug");
    }
    result = svc_resource->load(mod_ctx, "debug.wgsl", &g_debugShaderSource);
    if (result != MOD_OK || g_debugShaderSource.data == nullptr) {
        return mods::set_error(error, result, "failed to load debug.wgsl");
    }
    if (!build_debug_pipeline()) {
        return mods::set_error(error, MOD_ERROR, "failed to create debug pipeline");
    }
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "depth-to-normal debug";
    drawDesc.draw = on_debug_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_debugDrawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register debug draw type");
    }
    stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register frame hook");
    }
    return MOD_OK;
}

void shutdown() {
    svc_resource->free(mod_ctx, &g_shaderSource);
    svc_resource->free(mod_ctx, &g_debugShaderSource);
    release_target(g_target);
    for (auto& retired : g_retired) {
        release_target(retired.target);
    }
    g_retired.clear();
    if (g_pipeline != nullptr) {
        wgpuComputePipelineRelease(g_pipeline);
        g_pipeline = nullptr;
    }
    if (g_layout != nullptr) {
        wgpuBindGroupLayoutRelease(g_layout);
        g_layout = nullptr;
    }
    if (g_debugPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_debugPipeline);
        g_debugPipeline = nullptr;
    }
    if (g_debugLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_debugLayout);
        g_debugLayout = nullptr;
    }
    g_computeType = 0;
    g_sceneBeginHook = 0;
    g_debugDrawType = 0;
    g_frameBeforeHudHook = 0;
    g_cvarNormalsDebug = 0;
    g_cameraValid = false;
    g_frameComputed = false;
    g_frameView = nullptr;
}

}  // namespace hub_dtn

// The exported world-normal service — same id ("dev.automata.depth_to_normal") the standalone
// provider used, so SSILVB / Realtime Sun Shadows resolve it unchanged. Kept at global scope so
// EXPORT_SERVICE's generated meta record is a simple token.
constexpr DepthToNormalService g_dtnService{
    .header = SERVICE_HEADER(DepthToNormalService, DEPTH_TO_NORMAL_SERVICE_MAJOR,
        DEPTH_TO_NORMAL_SERVICE_MINOR),
    .get_frame = hub_dtn::get_frame,
};
EXPORT_SERVICE(g_dtnService);

// ===========================================================================================
// SUB-FEATURE: Water Plane   (water-surface height provider service)
// ===========================================================================================
namespace hub_water {

// Cached once per frame at scene begin (game thread); get_frame just hands these back. A single
// horizontal plane for the frame (the water body at the player) - see water_plane_service.h and
// docs for the flat-plane limitation.
bool g_hasWater = false;
float g_waterY = 0.0f;
GfxStageHookHandle g_sceneBeginHook = 0;

// Probe the water surface at the player's XZ once per frame. This is a PURE QUERY: no draw, no
// offscreen pass, no GX state touched - so the provider is provably invisible unless a consumer
// reads it. Runs on the game thread (stage callback), the only place game state is safe to read.
void on_scene_begin(ModContext*, const GfxStageContext*, void*) {
    g_hasWater = false;
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    // Guard the position too: on the first frames of a scene the actor can exist before its
    // placement is meaningful (idiom from realtime_sun_shadows's link cascade).
    if (player != nullptr && std::isfinite(player->current.pos.x) &&
        std::isfinite(player->current.pos.y) && std::isfinite(player->current.pos.z))
    {
        f32 y = 0.0f;
        // fopAcM_getWaterY(const cXyz* pos, f32* o_waterY): returns nonzero and writes the
        // surface Y when there is water at pos.xz, else returns 0 (and writes -inf).
        if (fopAcM_getWaterY(&player->current.pos, &y) != 0) {
            g_waterY = y;
            g_hasWater = true;
        }
    }
}

// The exported service entry point (see g_waterService below). Idempotent per frame: returns the
// frame's cached probe. has_water == false is a valid "no water" result, not an error.
ModResult get_frame(ModContext*, WaterPlaneFrame* out) {
    if (out == nullptr || out->struct_size < sizeof(WaterPlaneFrame)) {
        return MOD_INVALID_ARGUMENT;
    }
    out->water_y = g_waterY;
    out->has_water = g_hasWater;
    return MOD_OK;
}

void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Water Plane");
    svc_ui->pane_add_text(mod_ctx, panel,
        "Probes the water-surface height at the player each frame and provides it to other mods "
        "(AO / GI) so they can fade their effect underwater. Passive: a pure query with no on/off "
        "and no cost unless another mod reads it.",
        nullptr);
}

ModResult init(ModError* error) {
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register water scene hook");
    }
    return MOD_OK;
}

void shutdown() {
    g_sceneBeginHook = 0;
    g_hasWater = false;
    g_waterY = 0.0f;
}

}  // namespace hub_water

// The exported water-plane service. Kept at global scope so EXPORT_SERVICE's generated meta
// record is a simple token, mirroring g_dtnService above.
constexpr WaterPlaneService g_waterService{
    .header =
        SERVICE_HEADER(WaterPlaneService, WATER_PLANE_SERVICE_MAJOR, WATER_PLANE_SERVICE_MINOR),
    .get_frame = hub_water::get_frame,
};
EXPORT_SERVICE(g_waterService);

// ===========================================================================================
// SUB-FEATURE: Deferred Fog   (re-applies fog after screen-space effects)
// ===========================================================================================
namespace hub_fog {

// Hook targets (each emits a modmeta hook record the host resolves at load).
DEFINE_HOOK(GXSetFog, SetFog);
DEFINE_HOOK(GFSetFog, SetGfFog);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);

ConfigVarHandle g_cvarFogEnabled = 0;    // DEFAULT below in init()
ConfigVarHandle g_cvarFogMixed = 0;      // DEFAULT below in init()
ConfigVarHandle g_cvarFogDebug = 0;      // DEFAULT below in init()

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

struct FogConfig {
    bool valid = false;
    uint8_t type = 0;
    float startZ = 0.0f;
    float endZ = 0.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    GXColor color{0, 0, 0, 0};
};

bool g_scopeActive = false;
bool g_quadArmed = false;
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

HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (g_fogReplayActive) {
        const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
        J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
        J3DPEBlock* peBlock = material != nullptr ? material->getPEBlock() : nullptr;
        J3DFog* fog = peBlock != nullptr ? peBlock->getFog() : nullptr;
        uint32_t index = 0;
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
    const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
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
    if (vote_config(config)) {
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
        return;
    }
    uniforms.color[0] = static_cast<float>(g_reference.color.r) / 255.0f;
    uniforms.color[1] = static_cast<float>(g_reference.color.g) / 255.0f;
    uniforms.color[2] = static_cast<float>(g_reference.color.b) / 255.0f;
    uniforms.color[3] = 1.0f;
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
    const bool exact = exact_mode();
    g_quadArmed = (g_suppressedCount > 0 || (exact && g_frameConfigCount > 0)) &&
                  g_reference.valid;
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
        "How scenes that draw with several fog configurations are handled.<br/>Vanilla: fall "
        "back to the game's forward fog (exact, but AO/shadows then darken the fog itself at "
        "range).<br/>Exact (replay): defer every configuration - the opaque geometry is "
        "replayed once into a per-pixel config-ID buffer and each pixel gets its own exact fog. "
        "Costs one extra opaque scene of per-frame geometry streaming on mixed frames; with "
        "heavy shadow-cascade settings this can crowd the engine's fixed streaming buffers, so "
        "prefer Vanilla there until the adaptive-buffer engine update lands.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarFogMixed;
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
    control.config_var = g_cvarFogDebug;
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
    // DEFAULT: mixed-scene mode = Exact replay (1). 0 = Vanilla fallback.
    cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogMixedMode";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 1;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarFogMixed) != MOD_OK) {
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
    g_cvarFogEnabled = g_cvarFogMixed = g_cvarFogDebug = 0;
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
}

}  // namespace hub_fog

// ===========================================================================================
// Combined mod entry points + shared UI panel
// ===========================================================================================
namespace {

ModResult build_combined_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    hub_dtn::build_section(panel);
    hub_water::build_section(panel);
    hub_fog::build_section(panel);
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = hub_dtn::init(error);
    if (result != MOD_OK) {
        return result;
    }
    result = hub_water::init(error);
    if (result != MOD_OK) {
        return result;
    }
    result = hub_fog::init(error);
    if (result != MOD_OK) {
        return result;
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_combined_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "graphics_hub ready (depth-to-normal + deferred fog)");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    hub_fog::shutdown();
    hub_water::shutdown();
    hub_dtn::shutdown();
    return MOD_OK;
}
}
