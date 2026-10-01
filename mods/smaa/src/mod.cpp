// SMAA: subpixel morphological antialiasing (SMAA 1x, orthogonal patterns only).
//
// Runs once per frame at GFX_STAGE_SCENE_AFTER_OPAQUE, before the game's translucency and bloom,
// so those effects work on antialiased geometry. Three passes:
//   1. edge detection (compute)   luma edges (reference SMAA) unioned with geometric edges from
//                                 the authored view-space normals and depth. Also zeroes BlendTex.
//   2. blend weights (compute)    edge pixels in each 16x16 workgroup are compacted into
//                                 contiguous threads (the CMAA2 idea), then each one searches
//                                 its edge run and gets an analytic coverage weight.
//   3. neighborhood blend (draw)  rewrites edge pixels in the live scene target. It reads colour
//                                 from the frame's scene snapshot (a copy), so there is no
//                                 read/write hazard.
//
// Threads: on_scene_after_opaque runs on the game thread (resolve, uniforms, queue the work);
// on_compute and on_draw run later on the render worker and must not touch game state or config.
//
// Service-only: no game headers and no hooks (uses gfx, config, ui, resource, log).
// Design notes: docs/smaa.md. Provenance: res/licenses/ATTRIBUTION.txt.

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"

#include "gfx_normal_compat.h"
#include "gfx_scene_pass.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
// No other mod is imported: the normals come from the same resolve_pass as colour and depth. When
// they are unavailable (compatibility renderers, MSAA on), geometric edges turn off and SMAA runs
// on its luma detector alone, as reference SMAA does.

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarEdgeThreshold = 0;
ConfigVarHandle g_cvarUseNormalEdges = 0;
ConfigVarHandle g_cvarNormalThreshold = 0;
ConfigVarHandle g_cvarDepthThreshold = 0;
ConfigVarHandle g_cvarMaxSearchSteps = 0;
ConfigVarHandle g_cvarBlendStrength = 0;
ConfigVarHandle g_cvarLocalContrast = 0;
ConfigVarHandle g_cvarDebugView = 0;

GfxComputeTypeHandle g_computeType = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_afterOpaqueHook = 0;
UiWindowHandle g_controlsWindow = 0;

ResourceBuffer g_edgeSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_blendSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_neighborhoodSource = RESOURCE_BUFFER_INIT;

GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_edgePipeline = nullptr;
WGPUComputePipeline g_blendPipeline = nullptr;
// Layout key of the scene pass the neighborhood pipeline was built for; see
// ensure_neighborhood_pipeline.
uint64_t g_sceneLayoutKey = 0;
bool g_sceneLayoutValid = false;
WGPUBindGroupLayout g_edgeLayout = nullptr;
WGPUBindGroupLayout g_blendLayout = nullptr;
WGPURenderPipeline g_neighborhoodPipeline = nullptr;
WGPUBindGroupLayout g_neighborhoodLayout = nullptr;
WGPUSampler g_linearSampler = nullptr;

// Set by the render worker once the compute chain has run; mod_update logs that once.
std::atomic<bool> g_chainExecuted{false};
bool g_loggedChain = false;
bool g_warnedNoColor = false;

// When geometric edges are unavailable SMAA logs one INFO line saying why; luma-only SMAA otherwise
// just looks weaker than expected. Possible causes: MSAA is on (aurora creates the normal buffer
// only when msaaSamples == 1; this game build never enables MSAA, the check is for builds that do),
// a compatibility renderer (D3D11 / OpenGL ES, no WebGPU core features), or no depth snapshot.
//
// The normal snapshot latches: the first resolve that asks for it returns null and enables normals
// from the next frame. So the log waits for this many consecutive frames without them. The counter
// only delays the log; until normals arrive SMAA simply runs luma-only.
constexpr uint32_t kNormalLatchGraceFrames = 8;
uint32_t g_normalWaitFrames = 0;
bool g_loggedNoNormals = false;

// EdgesTex / BlendTex (both RGBA8Unorm, storage + sampled), recreated when the render size changes.
// An old set is kept for a few frames before release, because a payload holding its views may
// still be in flight on the render worker.
struct Targets {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTexture edges = nullptr;
    WGPUTextureView edgesView = nullptr;
    WGPUTexture blend = nullptr;
    WGPUTextureView blendView = nullptr;
};
Targets g_targets;
struct RetiredTargets {
    Targets targets;
    int framesLeft = 0;
};
std::vector<RetiredTargets> g_retiredTargets;

// Mirror of the WGSL `Uniforms` struct declared in all three res/*.wgsl files. Keep them byte for
// byte identical, total size a multiple of 16; scalars only, to avoid vec3 alignment padding.
struct SmaaUniforms {
    float screen_size[2];        // pixels
    float inv_screen_size[2];
    float threshold;             // luma step, 0.05..0.20
    float normal_threshold;      // 1 - cos(angle) between neighbouring normals, 0.02..0.50
    float depth_threshold;       // relative raw-depth step, 0.001..0.2
    float max_search_steps;      // pixels per direction, 4..32
    float local_contrast_factor; // 1.0..4.0
    float blend_strength;        // weight multiplier, 0..1.5
    float corner_rounding;       // reserved: always 0, read by no shader
    uint32_t flags;              // bit 0 = geometric (normal/depth) edges enabled
    uint32_t debug_view;         // 0 off, 1 edges, 2 blend weights
    float _pad0;
    float _pad1;
    float _pad2;
};
static_assert(sizeof(SmaaUniforms) == 64);
static_assert(sizeof(SmaaUniforms) % 16 == 0);

struct ComputePayload {
    WGPUTextureView sceneColor;
    // When geometric edges are off, the next two hold the colour view as a stand-in.
    WGPUTextureView normal; // authored view-space normal snapshot, alpha = validity
    WGPUTextureView depth;  // raw reversed-Z depth snapshot
    WGPUTextureView edges;
    WGPUTextureView blend;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<ComputePayload>);

struct DrawPayload {
    WGPUTextureView sceneColor;
    WGPUTextureView blend;
    WGPUTextureView edges; // debug views only
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_view; // not read: the shader takes the mode from uniforms.debug_view
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

WGPUShaderModule create_shader_module(const char* label, const ResourceBuffer& source) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {label, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
}

bool build_compute_pipeline(const char* label, const ResourceBuffer& source, const char* entry,
    WGPUComputePipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderModule module = create_shader_module(label, source);
    if (module == nullptr) {
        return false;
    }
    WGPUComputePipelineDescriptor pipelineDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {label, WGPU_STRLEN};
    pipelineDesc.compute.module = module;
    pipelineDesc.compute.entryPoint = {entry, WGPU_STRLEN};
    outPipeline = wgpuDeviceCreateComputePipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (outPipeline == nullptr) {
        return false;
    }
    outLayout = wgpuComputePipelineGetBindGroupLayout(outPipeline, 0);
    return outLayout != nullptr;
}

bool build_neighborhood_pipeline(const gfx_compat::ScenePassLayout& sceneLayout) {
    WGPUShaderModule module = create_shader_module("SMAA neighborhood blend", g_neighborhoodSource);
    if (module == nullptr) {
        return false;
    }
    // The pipeline must match the scene pass's attachments exactly, so the layout comes from the
    // draw context (see ensure_neighborhood_pipeline). When the pass carries the normal attachment,
    // that second target comes back write-masked off: this draw replaces colour on target 0 and
    // leaves the authored normals untouched for the other mods that read them.
    const gfx_compat::ScenePassLayout& layout = sceneLayout;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = layout.color_target_count;
    fragment.targets = layout.color_targets;
    // The depth format must match the scene pass even though this draw never tests or writes depth.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = layout.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {"SMAA neighborhood blend", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = &depthStencil;
    pipelineDesc.multisample.count = layout.sample_count;
    pipelineDesc.fragment = &fragment;
    g_neighborhoodPipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_neighborhoodPipeline == nullptr) {
        return false;
    }
    g_neighborhoodLayout = wgpuRenderPipelineGetBindGroupLayout(g_neighborhoodPipeline, 0);
    return g_neighborhoodLayout != nullptr;
}

void release_targets(Targets& targets) {
    if (targets.edgesView != nullptr) {
        wgpuTextureViewRelease(targets.edgesView);
        targets.edgesView = nullptr;
    }
    if (targets.blendView != nullptr) {
        wgpuTextureViewRelease(targets.blendView);
        targets.blendView = nullptr;
    }
    if (targets.edges != nullptr) {
        wgpuTextureRelease(targets.edges);
        targets.edges = nullptr;
    }
    if (targets.blend != nullptr) {
        wgpuTextureRelease(targets.blend);
        targets.blend = nullptr;
    }
    targets.width = targets.height = 0;
}

void tick_retired_targets() {
    for (auto it = g_retiredTargets.begin(); it != g_retiredTargets.end();) {
        if (--it->framesLeft <= 0) {
            release_targets(it->targets);
            it = g_retiredTargets.erase(it);
        } else {
            ++it;
        }
    }
}

bool ensure_targets(uint32_t width, uint32_t height) {
    if (g_targets.width == width && g_targets.height == height) {
        return true;
    }
    if (g_targets.width != 0) {
        g_retiredTargets.push_back(RetiredTargets{std::exchange(g_targets, Targets{}), 4});
    }

    const auto createTexture = [&](const char* label, WGPUTexture& outTexture,
                                   WGPUTextureView& outView) {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.label = {label, WGPU_STRLEN};
        texDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        texDesc.size = {width, height, 1};
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        outTexture = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
        if (outTexture == nullptr) {
            return false;
        }
        outView = wgpuTextureCreateView(outTexture, nullptr);
        return outView != nullptr;
    };

    if (!createTexture("SMAA edges", g_targets.edges, g_targets.edgesView) ||
        !createTexture("SMAA blend", g_targets.blend, g_targets.blendView)) {
        release_targets(g_targets);
        return false;
    }
    g_targets.width = width;
    g_targets.height = height;
    return true;
}

constexpr uint32_t div_ceil(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

// Render worker: edge detection (8x8 workgroups), then compacted blend weights (16x16). WebGPU
// orders the two dispatches regardless (each dispatch is its own usage scope, so pass 2 sees pass
// 1's writes); they are separate compute passes only so each gets its own debug label.
void on_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(ComputePayload)) {
        return;
    }
    ComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.sceneColor == nullptr || data.edges == nullptr || data.blend == nullptr ||
        g_edgePipeline == nullptr || g_blendPipeline == nullptr) {
        return;
    }

    const auto makeBindGroup = [&](WGPUBindGroupLayout layout,
                                   std::initializer_list<WGPUBindGroupEntry> entries) {
        WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindGroupDesc.layout = layout;
        bindGroupDesc.entryCount = entries.size();
        bindGroupDesc.entries = entries.begin();
        return wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    };
    const auto textureEntry = [](uint32_t binding, WGPUTextureView view) {
        WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = binding;
        entry.textureView = view;
        return entry;
    };
    const auto uniformEntry = [&](uint32_t binding) {
        WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = binding;
        entry.buffer = ctx->uniform_buffer;
        entry.offset = data.uniform_offset;
        entry.size = data.uniform_size;
        return entry;
    };

    WGPUBindGroup edgeGroup = makeBindGroup(g_edgeLayout,
        {textureEntry(0, data.sceneColor), textureEntry(1, data.normal), textureEntry(2, data.edges),
            textureEntry(3, data.blend), uniformEntry(4), textureEntry(5, data.depth)});
    WGPUBindGroup blendGroup = makeBindGroup(g_blendLayout,
        {textureEntry(0, data.edges), textureEntry(1, data.blend), uniformEntry(2)});
    if (edgeGroup == nullptr || blendGroup == nullptr) {
        if (edgeGroup != nullptr) {
            wgpuBindGroupRelease(edgeGroup);
        }
        if (blendGroup != nullptr) {
            wgpuBindGroupRelease(blendGroup);
        }
        return;
    }

    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"SMAA edge detection", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    wgpuComputePassEncoderSetPipeline(pass, g_edgePipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, edgeGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(data.width, 8), div_ceil(data.height, 8), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    passDesc.label = {"SMAA blend weights", WGPU_STRLEN};
    pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    wgpuComputePassEncoderSetPipeline(pass, g_blendPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, blendGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(data.width, 16), div_ceil(data.height, 16), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    wgpuBindGroupRelease(edgeGroup);
    wgpuBindGroupRelease(blendGroup);
    g_chainExecuted.store(true, std::memory_order_release);
}

void release_neighborhood_pipeline() {
    if (g_neighborhoodPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_neighborhoodPipeline);
        g_neighborhoodPipeline = nullptr;
    }
    if (g_neighborhoodLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_neighborhoodLayout);
        g_neighborhoodLayout = nullptr;
    }
    g_sceneLayoutKey = 0;
    g_sceneLayoutValid = false;
}

// Render worker. Builds the neighborhood pipeline for the pass this draw is recorded into, and
// rebuilds it when the pass's layout key changes. Requesting normals adds a second colour attachment
// to the scene pass from the next frame on, and a pipeline built for the old shape is rejected with
// nothing logged. See CONTRIBUTING.md "Rules that have bitten before".
bool ensure_neighborhood_pipeline(const GfxDrawContext& ctx) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (g_sceneLayoutValid && key == g_sceneLayoutKey && g_neighborhoodPipeline != nullptr) {
        return true;
    }
    release_neighborhood_pipeline();
    gfx_compat::ScenePassLayout sceneLayout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, sceneLayout) ||
        !build_neighborhood_pipeline(sceneLayout))
    {
        release_neighborhood_pipeline();
        return false;
    }
    g_sceneLayoutKey = key;
    g_sceneLayoutValid = true;
    return true;
}

// Render worker: composite the antialiased edges (or a debug view) into the live scene target.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload) || ctx == nullptr ||
        !ensure_neighborhood_pipeline(*ctx))
    {
        return;
    }
    DrawPayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.sceneColor == nullptr || data.blend == nullptr || data.edges == nullptr ||
        g_neighborhoodPipeline == nullptr || g_linearSampler == nullptr) {
        return;
    }

    WGPUBindGroupEntry entries[5] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneColor;
    entries[1].binding = 1;
    entries[1].textureView = data.blend;
    entries[2].binding = 2;
    entries[2].sampler = g_linearSampler;
    entries[3].binding = 3;
    entries[3].buffer = ctx->uniform_buffer;
    entries[3].offset = data.uniform_offset;
    entries[3].size = data.uniform_size;
    entries[4].binding = 4;
    entries[4].textureView = data.edges;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = g_neighborhoodLayout;
    bindGroupDesc.entryCount = 5;
    bindGroupDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    if (bindGroup == nullptr) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, g_neighborhoodPipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

// Game thread, once all opaque world geometry is drawn: snapshot the scene, fill the uniforms and
// queue the compute chain and the composite draw.
void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired_targets();
    if (!get_bool_option(g_cvarEnabled, true)) {
        return;
    }
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext)) {
        return;
    }

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    gfx_compat::request_normal(resolveDesc, true);
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr) {
        // Logged once, but the hook retries every frame and resumes if the snapshot comes back.
        if (!g_warnedNoColor) {
            g_warnedNoColor = true;
            svc_log->warn(mod_ctx, "scene colour snapshot unavailable; SMAA disabled");
        }
        return;
    }
    if (resolved.width < 16 || resolved.height < 16 ||
        !ensure_targets(resolved.width, resolved.height)) {
        return;
    }

    // Geometric edges need both the normal snapshot (creases) and the depth snapshot (silhouettes).
    // Either can be missing: normals during the latch frames or on a renderer that cannot provide
    // them, depth where aurora cannot snapshot it. Then geometric edges are off and SMAA runs on
    // luma edges only, picking the normals up by itself once they arrive. The grace counter below
    // only delays the one-time log (see kNormalLatchGraceFrames).
    const WGPUTextureView sceneNormalView = gfx_compat::resolved_normal(resolved);
    const bool haveGeometry = sceneNormalView != nullptr && resolved.depth != nullptr;
    const bool useNormalEdges = haveGeometry && get_bool_option(g_cvarUseNormalEdges, true);
    if (haveGeometry) {
        g_normalWaitFrames = 0;
    } else if (!g_loggedNoNormals && ++g_normalWaitFrames > kNormalLatchGraceFrames) {
        g_loggedNoNormals = true;
        GfxDeviceInfo live = GFX_DEVICE_INFO_INIT;
        const uint32_t samples =
            svc_gfx->get_device_info(mod_ctx, &live) == MOD_OK ? live.sample_count : 1u;
        if (sceneNormalView == nullptr && samples > 1u) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "geometric edge detection off: MSAA is on (%ux) and the scene normal buffer "
                "requires MSAA off. SMAA is running on luma edges only.",
                samples);
            svc_log->info(mod_ctx, msg);
        } else {
            // Also reached when only the depth snapshot is missing, although the text names normals.
            svc_log->info(mod_ctx,
                "geometric edge detection off: this renderer provides no scene normals. SMAA is "
                "running on luma edges only.");
        }
    }

    const auto scaled = [](ConfigVarHandle cvar, int64_t fallback, int64_t lo, int64_t hi,
                            float scale) {
        return static_cast<float>(std::clamp<int64_t>(get_int_option(cvar, fallback), lo, hi)) *
            scale;
    };

    SmaaUniforms uniforms{};
    uniforms.screen_size[0] = static_cast<float>(resolved.width);
    uniforms.screen_size[1] = static_cast<float>(resolved.height);
    uniforms.inv_screen_size[0] = 1.0f / uniforms.screen_size[0];
    uniforms.inv_screen_size[1] = 1.0f / uniforms.screen_size[1];
    uniforms.threshold = scaled(g_cvarEdgeThreshold, 20, 5, 20, 0.01f);
    uniforms.normal_threshold = scaled(g_cvarNormalThreshold, 5, 2, 50, 0.01f);
    uniforms.depth_threshold = scaled(g_cvarDepthThreshold, 20, 1, 200, 0.001f);
    uniforms.max_search_steps =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarMaxSearchSteps, 16), 4, 32));
    uniforms.local_contrast_factor = scaled(g_cvarLocalContrast, 200, 100, 400, 0.01f);
    uniforms.blend_strength = scaled(g_cvarBlendStrength, 100, 0, 150, 0.01f);
    uniforms.corner_rounding = 0.0f; // reserved; corner rounding is not implemented
    uniforms.flags = useNormalEdges ? 1u : 0u;
    const uint32_t debugMode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 2));
    uniforms.debug_view = debugMode;

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }

    ComputePayload computePayload{};
    computePayload.sceneColor = resolved.color;
    // The edge shader reads bindings 1 and 5 only when flags bit 0 is set. Otherwise bind the colour
    // snapshot (also a texture_2d<f32>) in their place so the bind group is always complete.
    computePayload.normal = useNormalEdges ? sceneNormalView : resolved.color;
    computePayload.depth = useNormalEdges ? resolved.depth : resolved.color;
    computePayload.edges = g_targets.edgesView;
    computePayload.blend = g_targets.blendView;
    computePayload.uniform_offset = uniformRange.offset;
    computePayload.uniform_size = uniformRange.size;
    computePayload.width = resolved.width;
    computePayload.height = resolved.height;
    if (svc_gfx->push_compute(mod_ctx, g_computeType, &computePayload, sizeof(computePayload)) !=
        MOD_OK) {
        return;
    }

    DrawPayload drawPayload{resolved.color, g_targets.blendView, g_targets.edgesView,
        uniformRange.offset, uniformRange.size, debugMode};
    svc_gfx->push_draw(mod_ctx, g_drawType, &drawPayload, sizeof(drawPayload));
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help,
    int64_t min, int64_t max, int64_t step, const char* suffix) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help,
    const char** options, size_t optionCount) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.options = options;
    control.option_count = optionCount;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "Effect");
    add_toggle(left, "Enabled", g_cvarEnabled, "Enables the SMAA antialiasing pass.");
    add_number(left, "Blend Strength", g_cvarBlendStrength,
        "Overall strength of the edge blending. 100% is the natural amount; lower keeps edges "
        "crisper, higher smooths harder (and softens slightly).",
        0, 150, 5, "%");

    svc_ui->pane_add_section(mod_ctx, left, "Edge Detection");
    add_number(left, "Luma Threshold", g_cvarEdgeThreshold,
        "Luma contrast an edge must reach to be antialiased. Lower catches more edges (softer "
        "overall, can blur texture detail). 20% is both the default and the maximum.<br/>"
        "Reference SMAA uses 10%, which is tuned for high-contrast modern rendering; on this "
        "game's flatter art it treats ordinary texture detail as edges. Geometric Edges below "
        "catches the silhouettes and creases a lower luma threshold would otherwise be needed for.",
        5, 20, 1, nullptr);
    add_number(left, "Local Contrast", g_cvarLocalContrast,
        "Suppresses an edge when a nearby edge is more than this many times stronger (stops "
        "doubled edges inside high-contrast texture). Higher suppresses less. 200% is the SMAA "
        "default.",
        100, 400, 10, "%");
    add_toggle(left, "Geometric Edges", g_cvarUseNormalEdges,
        "Also detect edges from the game's own surface normals and the depth buffer. Catches "
        "silhouettes and creases where two flat-shaded surfaces have similar brightness. No effect "
        "if the scene normals are unavailable (the D3D11 and OpenGL ES compatibility renderers "
        "cannot provide them).");
    add_number(left, "Normal Threshold", g_cvarNormalThreshold,
        "How sharp a normal difference counts as a geometric edge. The value is angular: 5% is "
        "about an 18 degree crease, 10% about 26 degrees. Lower catches subtler creases; higher "
        "only steep angles.<br/>The default was 10% when this read a normal reconstructed from "
        "depth - that normal is flat per triangle, so every facet boundary on a curved low-poly "
        "surface registered as a step and a low threshold lit up geometry that was not really an "
        "edge. It now reads the game's <b>authored</b> normals, which are smooth across a surface, "
        "so those interior steps do not exist and 5% is safe. Raise it if creases you consider "
        "part of the shading are being antialiased as edges.",
        2, 50, 1, "%");
    add_number(left, "Depth Threshold", g_cvarDepthThreshold,
        "Relative depth discontinuity that counts as a geometric edge (silhouettes). Lower catches "
        "more distant silhouettes; higher only sharp foreground steps.",
        1, 200, 1, nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Search");
    add_number(left, "Max Search Steps", g_cvarMaxSearchSteps,
        "How far along an edge the pattern search reaches. Higher smooths longer near-horizontal / "
        "near-vertical edges but costs more per edge pixel.",
        4, 32, 1, nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugOptions[] = {"Off", "Edges", "Weights"};
    add_select(left, "Debug View", g_cvarDebugView,
        "Edges: the detected edge mask (red = vertical, green = horizontal).<br/>Weights: the "
        "computed blend weights. Both draw at this stage, so later effects may paint over them.",
        kDebugOptions, 3);
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
        svc_log->error(mod_ctx, "failed to open SMAA controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarEnabled;
    add_control(panel, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool_option(
    const char* name, bool defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = name;
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register SMAA option");
    }
    return MOD_OK;
}

ModResult register_int_option(
    const char* name, int64_t defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = name;
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register SMAA option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "edge_detection.wgsl", &g_edgeSource);
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "blend_weights.wgsl", &g_blendSource);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "neighborhood_blend.wgsl", &g_neighborhoodSource);
    }
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load SMAA shaders");
    }

    if (register_bool_option("effectEnabled", true, g_cvarEnabled, error) != MOD_OK ||
        register_bool_option("useNormalEdges", true, g_cvarUseNormalEdges, error) != MOD_OK) {
        return MOD_ERROR;
    }
    const struct {
        const char* name;
        int64_t defaultValue;
        ConfigVarHandle* handle;
    } intOptions[] = {
        // DEFAULT 20 (= 0.20 luma contrast), the top of the range. Reference SMAA uses 0.10, which
        // on TP's flat, low-contrast art picks up and softens ordinary texture detail; geometric
        // edges cover the silhouettes and creases a lower threshold would be needed for. Keep in
        // step with the fallback in the uniform fill.
        {"edgeThreshold", 20, &g_cvarEdgeThreshold},
        {"normalThreshold", 5, &g_cvarNormalThreshold},
        {"depthThreshold", 20, &g_cvarDepthThreshold},
        {"maxSearchSteps", 16, &g_cvarMaxSearchSteps},
        {"blendStrength", 100, &g_cvarBlendStrength},
        {"localContrast", 200, &g_cvarLocalContrast},
        {"debugMode", 0, &g_cvarDebugView},
    };
    for (const auto& opt : intOptions) {
        if (register_int_option(opt.name, opt.defaultValue, *opt.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_compute_pipeline("SMAA edge detection", g_edgeSource, "edge_detection",
            g_edgePipeline, g_edgeLayout) ||
        !build_compute_pipeline(
            "SMAA blend weights", g_blendSource, "blend_weights", g_blendPipeline, g_blendLayout)) {
        return mods::set_error(error, MOD_ERROR, "failed to create SMAA compute pipelines");
    }
    // The neighborhood pipeline is not built here: it must match the scene pass, which gains a
    // normal attachment at runtime. ensure_neighborhood_pipeline builds it on the first draw and
    // rebuilds it when the layout key changes.

    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.label = {"SMAA linear clamp", WGPU_STRLEN};
    samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    samplerDesc.magFilter = WGPUFilterMode_Linear;
    samplerDesc.minFilter = WGPUFilterMode_Linear;
    samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    samplerDesc.maxAnisotropy = 1;
    g_linearSampler = wgpuDeviceCreateSampler(g_deviceInfo.device, &samplerDesc);
    if (g_linearSampler == nullptr) {
        return mods::set_error(error, MOD_ERROR, "failed to create SMAA sampler");
    }

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "SMAA edge + blend weights";
    computeDesc.callback = on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register SMAA compute type");
    }
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "SMAA neighborhood blend";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register SMAA draw type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_afterOpaqueHook) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register SMAA stage hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "smaa ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (!g_loggedChain && g_chainExecuted.load(std::memory_order_acquire)) {
        g_loggedChain = true;
        svc_log->info(mod_ctx, "SMAA chain executed OK");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_resource->free(mod_ctx, &g_edgeSource);
    svc_resource->free(mod_ctx, &g_blendSource);
    svc_resource->free(mod_ctx, &g_neighborhoodSource);

    release_targets(g_targets);
    for (auto& retired : g_retiredTargets) {
        release_targets(retired.targets);
    }
    g_retiredTargets.clear();

    if (g_edgePipeline != nullptr) {
        wgpuComputePipelineRelease(g_edgePipeline);
        g_edgePipeline = nullptr;
    }
    if (g_blendPipeline != nullptr) {
        wgpuComputePipelineRelease(g_blendPipeline);
        g_blendPipeline = nullptr;
    }
    if (g_edgeLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_edgeLayout);
        g_edgeLayout = nullptr;
    }
    if (g_blendLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_blendLayout);
        g_blendLayout = nullptr;
    }
    // Also forgets the cached layout key, so after a reload the first draw rebuilds the pipeline.
    release_neighborhood_pipeline();
    if (g_linearSampler != nullptr) {
        wgpuSamplerRelease(g_linearSampler);
        g_linearSampler = nullptr;
    }

    g_cvarEnabled = g_cvarEdgeThreshold = g_cvarUseNormalEdges = g_cvarNormalThreshold = 0;
    g_cvarDepthThreshold = g_cvarMaxSearchSteps = g_cvarBlendStrength = g_cvarLocalContrast = 0;
    g_cvarDebugView = 0;
    g_computeType = g_drawType = 0;
    g_afterOpaqueHook = 0;
    g_controlsWindow = 0;
    g_loggedChain = false;
    g_warnedNoColor = false;
    g_normalWaitFrames = 0;
    g_loggedNoNormals = false;
    g_chainExecuted.store(false, std::memory_order_release);
    return MOD_OK;
}
}
