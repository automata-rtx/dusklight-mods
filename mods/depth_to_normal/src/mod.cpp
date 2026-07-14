// Depth to Normal - a single-purpose provider mod.
//
// Reconstructs a per-pixel WORLD-SPACE geometric surface normal from the game's depth snapshot
// once per frame and publishes it as a mod-exported service (DepthToNormalService) that any other
// mod can import. It has no user settings; its panel only shows a credit summary.
//
// Service-only (gfx/camera/resource/ui/log) - no game headers, so it survives game updates.
//
// The depth->normal reconstruction (atyuwen's 5-tap method, res/reconstruct.wgsl) is adapted from
// Encounter's ao_mod demo (which ports it from Bevy Engine's SSAO / Intel XeGTAO; see
// res/licenses/). This mod repackages that one function as a shared input for other mods.

#include "depth_to_normal_service.h"

#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(LogService, svc_log);

namespace {

ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_layout = nullptr;
GfxComputeTypeHandle g_computeType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;

// Output normal buffer (world-space normal + raw depth), screen-sized, recreated on resize. Old
// targets are retired a few frames rather than released immediately: a consumer's payload may
// still embed the view on the render worker.
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

// Per-frame state. The camera is captured at SCENE_BEGIN; the reconstruction is computed lazily
// on the first get_frame of the frame (compute-on-first-request), so it only runs if a consumer
// actually asks and it decouples from stage-hook ordering.
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

// Render worker thread: reconstruct the world-space normal buffer from the depth snapshot.
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

// Game thread: refresh per-frame state. The camera matrices captured here feed the lazy
// reconstruction; the depth snapshot is resolved later, in get_frame, when a consumer asks.
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

// The exported service entry point. Called by consumer mods on the game thread, before their own
// draw/compute that samples the normal. Compute-on-first-request per frame.
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

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "About");
    svc_ui->pane_add_text(mod_ctx, panel,
        "Reconstructs a world-space surface normal from the depth buffer once per frame and "
        "provides it to other mods (ambient occlusion, shadows, reflections, outlines). It has no "
        "settings of its own.",
        nullptr);
    svc_ui->pane_add_section(mod_ctx, panel, "Credit");
    svc_ui->pane_add_text(mod_ctx, panel,
        "The depth-to-normal reconstruction is a port of the depth-to-normal function from "
        "Encounter's ao_mod demo, itself based on atyuwen's 5-tap method and Bevy Engine SSAO / "
        "Intel XeGTAO. See res/licenses.",
        nullptr);
    return MOD_OK;
}

constexpr DepthToNormalService g_service{
    .header = SERVICE_HEADER(DepthToNormalService, DEPTH_TO_NORMAL_SERVICE_MAJOR,
        DEPTH_TO_NORMAL_SERVICE_MINOR),
    .get_frame = get_frame,
};

}  // namespace

EXPORT_SERVICE(g_service);

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "reconstruct.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return dusk::mods::set_error(error, result, "failed to load reconstruct.wgsl");
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_pipeline()) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to create reconstruct pipeline");
    }
    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "depth-to-normal reconstruct";
    computeDesc.callback = on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register stage hook");
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
    g_computeType = 0;
    g_sceneBeginHook = 0;
    g_cameraValid = false;
    g_frameComputed = false;
    g_frameView = nullptr;
    return MOD_OK;
}
}
