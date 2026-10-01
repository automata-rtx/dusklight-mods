// VBAO — Visibility Bitmask Ambient Occlusion. Service-only: no game headers, no hooks.
//
// Built on Encounter's ao_mod demo and its gfx-service compute chain, with these changes:
//  - Occlusion: a 32-sector visibility bitmask per slice (Therrien, Levesque and Gilet, 2023,
//    arXiv:2301.11376) instead of a horizon tracker, so gaps, separated occluders and thin geometry
//    such as grass do not overdarken.
//  - Temporal accumulation: the sampling noise changes every frame and a camera-reprojected history
//    averages the estimates. See res/temporal.wgsl and docs/vbao.md "Temporal accumulation".
//  - Shading normal: the game's authored view-space normal, resolved alongside depth
//    (GfxResolveDesc::normal -> GfxResolvedTargets::normal). There is no depth-reconstructed
//    fallback. Without normals (the D3D11 / OpenGL ES compatibility renderers) VBAO disables itself
//    and logs once.
//  - Depth-aware upscale in the composite, black point / contrast / distance fade, and a radius
//    that ramps with view depth.
//
// Per frame, at GFX_STAGE_SCENE_AFTER_OPAQUE (game thread): resolve depth + normals, push the
// uniforms, push one compute task (depth MIP prefilter -> occlusion -> 0-3 denoise passes ->
// temporal accumulation), then push the multiply composite into the scene pass. A selected debug
// view replaces the composite and is drawn at GFX_STAGE_FRAME_AFTER_HUD. The compute and draw
// callbacks run later on the render worker: they use their payload, the mod's own pipelines and
// wgpu calls, and must never touch game state.
//
// "Enhanced AO" is the mod's former name; it survives in pipeline labels and log lines.
//
// The framework WGSL in res/ derives from Bevy Engine's SSAO (MIT OR Apache-2.0) and Intel
// XeGTAO (MIT); see res/licenses/ and the headers of each shader.

#include "mods/service.hpp"

#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"

#include "gfx_normal_compat.h"
#include "gfx_scene_pass.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
IMPORT_SERVICE(CameraService, svc_camera);

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarQuality = 0;
ConfigVarHandle g_cvarCustomSlices = 0;
ConfigVarHandle g_cvarCustomSteps = 0;
ConfigVarHandle g_cvarRadius = 0;
ConfigVarHandle g_cvarRadiusFar = 0;
ConfigVarHandle g_cvarRadiusRampStart = 0;
ConfigVarHandle g_cvarRadiusRampEnd = 0;
ConfigVarHandle g_cvarRadiusMax = 0;
ConfigVarHandle g_cvarIntensity = 0;
ConfigVarHandle g_cvarContrast = 0;
ConfigVarHandle g_cvarBlackPoint = 0;
ConfigVarHandle g_cvarThickness = 0;
ConfigVarHandle g_cvarThickFade = 0;
ConfigVarHandle g_cvarThickDist = 0;
ConfigVarHandle g_cvarDebugDepthRange = 0;
ConfigVarHandle g_cvarDepthBias = 0;
ConfigVarHandle g_cvarTemporal = 0;
ConfigVarHandle g_cvarTemporalFrames = 0;
ConfigVarHandle g_cvarTemporalClamp = 0;
ConfigVarHandle g_cvarMotionResponse = 0;
ConfigVarHandle g_cvarMotionRange = 0;
ConfigVarHandle g_cvarContentThresh = 0;
ConfigVarHandle g_cvarDisoccTol = 0;
ConfigVarHandle g_cvarDenoisePasses = 0;
ConfigVarHandle g_cvarDenoiseStrength = 0;
ConfigVarHandle g_cvarDistanceFade = 0;
ConfigVarHandle g_cvarFadeStart = 0;
ConfigVarHandle g_cvarFadeEnd = 0;
ConfigVarHandle g_cvarHalfRes = 0;
ConfigVarHandle g_cvarDebugView = 0;

GfxComputeTypeHandle g_computeType = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_afterOpaqueHook = 0;
GfxStageHookHandle g_afterHudHook = 0;
UiWindowHandle g_controlsWindow = 0;

ResourceBuffer g_preprocessSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_vbaoSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_denoiseSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_temporalSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_compositeSource = RESOURCE_BUFFER_INIT;

GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_preprocessPipeline = nullptr;
WGPUComputePipeline g_mip4Pipeline = nullptr;
WGPUComputePipeline g_vbaoPipeline = nullptr;
WGPUComputePipeline g_denoisePipeline = nullptr;
WGPUComputePipeline g_temporalPipeline = nullptr;
WGPUBindGroupLayout g_preprocessLayout = nullptr;
WGPUBindGroupLayout g_mip4Layout = nullptr;
WGPUBindGroupLayout g_vbaoLayout = nullptr;
WGPUBindGroupLayout g_denoiseLayout = nullptr;
WGPUBindGroupLayout g_temporalLayout = nullptr;
WGPURenderPipeline g_compositePipeline = nullptr;
// Identity of the scene pass the composite pipelines were built for; see ensure_composite_pipelines.
uint64_t g_sceneLayoutKey = 0;
bool g_sceneLayoutValid = false;
WGPURenderPipeline g_compositeDebugPipeline = nullptr;
WGPUBindGroupLayout g_compositeLayout = nullptr;
WGPUBindGroupLayout g_compositeDebugLayout = nullptr;

// AO chain targets, recreated when the chain size changes (render size or Half Res). Old sets are
// kept for 4 frames before release, because payloads holding their views may still be in flight
// on the render worker.
struct AoTargets {
    uint32_t width = 0;   // AO chain resolution (half the render size in Half Res)
    uint32_t height = 0;
    uint32_t fullWidth = 0;   // full render resolution (temporal history / raw snapshot)
    uint32_t fullHeight = 0;
    WGPUTexture preprocessedDepth = nullptr;
    WGPUTextureView preprocessedDepthMips[5] = {};
    WGPUTextureView preprocessedDepthAll = nullptr;
    WGPUTexture aoNoisy = nullptr;
    WGPUTextureView aoNoisyView = nullptr;
    WGPUTexture depthDifferences = nullptr;
    WGPUTextureView depthDifferencesView = nullptr;
    WGPUTexture aoFinal = nullptr;
    WGPUTextureView aoFinalView = nullptr;
    // Temporal accumulation ping-pong, full render resolution: rgba16float (accumulated AO,
    // view depth / far plane, octahedral view-space normal).
    WGPUTexture history[2] = {};
    WGPUTextureView historyViews[2] = {};
};
AoTargets g_targets;
struct RetiredTargets {
    AoTargets targets;
    int framesLeft = 0;
};
std::vector<RetiredTargets> g_retiredTargets;

// Temporal state (game thread only).
uint32_t g_frameIndex = 0;
uint32_t g_historyWriteIndex = 0;
bool g_historyValid = false;   // the read history holds a valid previous accumulation
bool g_prevCameraValid = false;
float g_prevProjFromWorld[16] = {};

bool g_warnedNoInputs = false;

// The normal snapshot latches: the first resolve_pass that asks for normals returns a null view and
// enables the normal attachment from the next frame on (upstream mods/ao_mod says the same). A null
// view in the first frames therefore means "not yet". Only a run longer than
// kNormalLatchGraceFrames is reported, once.
//
// Two conditions keep the view null for good:
//  - no WebGPU core features (the D3D11 / OpenGL ES compatibility renderers);
//  - MSAA: aurora only creates the normal buffer at 1 sample (enable_normal_buffer() in
//    lib/webgpu/gpu.cpp; resolve_pass in lib/gfx/recording.cpp does not record the request).
//    This game build never enables MSAA (aurora treats an unset sample count as 1); the MSAA
//    branch of the warning is kept for builds that do. It reads the sample count from a fresh
//    get_device_info call.
constexpr uint32_t kNormalLatchGraceFrames = 8;
uint32_t g_normalWaitFrames = 0;
bool g_loggedChain = false;
float g_loggedFarPlane = 1.0f;  // last far plane reported to the log (world-unit calibration)

// Frame-time-aware ceiling on the temporal velocity term (temporal.wgsl).
//
// That term is screen motion in pixels per frame * motionResponse, so one camera pan drives it
// harder the lower the frame rate. A blend weight near 1 displays the raw single-frame estimate,
// whose sampling pattern changes every frame: the eye fuses that at high refresh rates and sees
// flicker at 30-60 fps. The ceiling is kVelocityFusionFrameTime / frame time: a full reset above
// 250 fps, ~0.58 at 144 fps, ~0.24 at 60 fps, ~0.12 at 30 fps (below the default base weight of
// 1/8, so the term has no effect there). Only the velocity term is capped; the disocclusion and
// content rejects are not. See docs/vbao.md "Temporal accumulation".
constexpr float kVelocityFusionFrameTime = 0.004f; // seconds: 250 fps and above allow a full reset
constexpr float kFrameDtMin = 0.001f;              // clamp for the raw interval (spikes, hitches)
constexpr float kFrameDtMax = 0.100f;
constexpr float kFrameDtResetGap = 0.5f;           // a pause/menu gap restarts the smoothing
constexpr float kFrameDtLogInterval = 2.0f;        // seconds between log lines, at most
bool g_frameTimeValid = false;                     // g_lastStageTime holds a usable previous sample
bool g_frameTimeSeeded = false;                    // the EMA has seen a real interval since (re)start
std::chrono::steady_clock::time_point g_lastStageTime{};
std::chrono::steady_clock::time_point g_lastFrameTimeLog{};
float g_smoothedFrameDt = 1.0f / 60.0f;            // EMA of the stage-hook interval (seconds)
float g_loggedFrameDt = 0.0f;                      // last interval reported to the log

// Game thread, once per rendered frame: advance the frame-interval estimate and return this frame's
// velocity ceiling. Measured between calls of the stage hook rather than from a game clock, so it
// stays service-only and follows the rate frames are actually rendered at.
float update_velocity_cap() {
    const auto now = std::chrono::steady_clock::now();
    bool measured = false;
    if (g_frameTimeValid) {
        const float rawDt = std::chrono::duration<float>(now - g_lastStageTime).count();
        if (rawDt > kFrameDtResetGap) {
            g_frameTimeValid = false; // restart below rather than smear a pause into the average
        } else {
            const float dt = std::clamp(rawDt, kFrameDtMin, kFrameDtMax);
            // The first real interval seeds the average outright so the estimate (and the log line
            // below) does not spend its first seconds blending out of the 60 fps initial guess.
            g_smoothedFrameDt = g_frameTimeSeeded ? g_smoothedFrameDt + (dt - g_smoothedFrameDt) * 0.1f : dt;
            g_frameTimeSeeded = true;
            measured = true;
        }
    }
    // No interval yet (first frame, or after a gap): keep the previous estimate (initially 60 fps).
    g_frameTimeValid = true;
    g_lastStageTime = now;
    const float cap = std::clamp(kVelocityFusionFrameTime / g_smoothedFrameDt, 0.0f, 1.0f);
    // Log when the smoothed interval moves by more than 25% (at most every 2 s), so a report of
    // flicker in motion can be read against the frame rate it happened at.
    if (measured && std::fabs(g_smoothedFrameDt - g_loggedFrameDt) > g_loggedFrameDt * 0.25f &&
        std::chrono::duration<float>(now - g_lastFrameTimeLog).count() > kFrameDtLogInterval)
    {
        g_loggedFrameDt = g_smoothedFrameDt;
        g_lastFrameTimeLog = now;
        char msg[128];
        std::snprintf(msg, sizeof(msg),
            "frame time %.1f ms (%.0f fps): motion response ceiling %.2f", g_smoothedFrameDt * 1000.0f,
            1.0f / g_smoothedFrameDt, cap);
        svc_log->info(mod_ctx, msg);
    }
    return cap;
}
// Set by the render worker after the first complete chain; mod_update logs it once.
std::atomic g_chainExecuted{false};

// One line at init naming the GPU and backend, so a report can be read against the hardware it came
// from. The two entry points are looked up through get_proc_address rather than linked; if the host
// does not export them the line is skipped.
void log_adapter_info() {
    if (g_deviceInfo.adapter == nullptr) {
        return;
    }
    using GetInfoFn = WGPUStatus (*)(WGPUAdapter, WGPUAdapterInfo*);
    using FreeMembersFn = void (*)(WGPUAdapterInfo);
    const auto getInfo =
        reinterpret_cast<GetInfoFn>(svc_gfx->get_proc_address(mod_ctx, "wgpuAdapterGetInfo"));
    const auto freeMembers = reinterpret_cast<FreeMembersFn>(
        svc_gfx->get_proc_address(mod_ctx, "wgpuAdapterInfoFreeMembers"));
    if (getInfo == nullptr) {
        return;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (getInfo(g_deviceInfo.adapter, &info) != WGPUStatus_Success) {
        return;
    }
    const auto view = [](WGPUStringView sv) {
        struct Piece { int len; const char* data; };
        if (sv.data == nullptr) {
            return Piece{0, ""};
        }
        const size_t len = sv.length == WGPU_STRLEN ? std::strlen(sv.data) : sv.length;
        return Piece{static_cast<int>(std::min<size_t>(len, 96)), sv.data};
    };
    const char* backend = "other";
    switch (info.backendType) {
    case WGPUBackendType_D3D11: backend = "D3D11"; break;
    case WGPUBackendType_D3D12: backend = "D3D12"; break;
    case WGPUBackendType_Metal: backend = "Metal"; break;
    case WGPUBackendType_Vulkan: backend = "Vulkan"; break;
    case WGPUBackendType_OpenGL: backend = "OpenGL"; break;
    case WGPUBackendType_OpenGLES: backend = "OpenGLES"; break;
    default: break;
    }
    const auto device = view(info.device);
    const auto vendor = view(info.vendor);
    const auto arch = view(info.architecture);
    char msg[320];
    std::snprintf(msg, sizeof(msg),
        "adapter: %.*s (%.*s, %.*s) backend %s vendorID 0x%04x deviceID 0x%04x subgroup %u-%u",
        device.len, device.data, vendor.len, vendor.data, arch.len, arch.data, backend,
        info.vendorID, info.deviceID, info.subgroupMinSize, info.subgroupMaxSize);
    svc_log->info(mod_ctx, msg);
    if (freeMembers != nullptr) {
        freeMembers(info);
    }
}

// Important: mirrors the `Uniforms` struct in all five res/*.wgsl files byte for byte. Change every
// copy together and keep the size a multiple of 16.
struct AoUniforms {
    float projection[16];          // proj_from_view (column-major)
    float inverse_projection[16];  // view_from_proj
    float reproject[16];           // current view -> previous frame's clip space
    float size[2];                 // AO chain size in pixels (half the render size in Half Res)
    float inv_size[2];
    float depth_scale[2];          // render (snapshot) pixels per chain pixel: 1 or 2
    float effect_radius;           // near radius, fraction of view depth
    float intensity;               // composite strength, 1 = 100%
    float slice_count;
    float steps_per_side;
    float thickness;               // base occluder thickness multiplier
    float contrast;                // exponent applied to visibility in the composite
    float temporal_alpha;          // base history blend weight, 1 / Temporal Frames
    float temporal_clamp_k;        // history clamp half-width, in sigmas of the 3x3 neighbourhood
    float inv_far;                 // 1 / far plane; normalises the depth stored in the history
    float radius_max;    // screen-space radius cap, fraction of viewport height
    float depth_bias;    // self-occlusion bias: view position scaled by (1 - depth_bias)
    float thick_fade;    // occluder-thickness fade range, multiple of the view radius
    float velocity_scale;  // velocity blend weight per pixel/frame of screen motion
    float content_thresh;  // outlier-test threshold scale (1 = 1..2.5 sigma)
    float disocc_tol;      // disocclusion depth tolerance, fraction of depth (shader floor 0.015)
    float black_point;     // occlusion floor removed in the composite
    float fade_start;      // distance fade start, world units of view depth
    float fade_end;        // distance fade end, world units of view depth
    uint32_t debug_view;
    uint32_t frame_index;  // advances per frame while accumulating, else 0
    uint32_t flags; // bit 0 = temporal enabled, bit 1 = history valid, bit 2 = distance fade
    float thick_dist_scale;  // extra occluder thickness, fraction of the view-space radius
    float inv_debug_depth;   // debug depth view gradient scale (1 / world units)
    float radius_far;        // far effect radius (fraction of view depth); 0 disables the ramp
    float radius_ramp_start; // radius ramp band start, world units of view depth
    float radius_ramp_end;   // radius ramp band end, world units of view depth
    float denoise_strength;  // spatial denoise blend, 0 raw .. 1 fully blurred
    float velocity_cap;      // ceiling on the velocity blend weight (frame-time aware, host-set)
    float velocity_range;    // velocity term fades over [this, 2x] view depth, world units; 0 = off
    float _pad2;
};
static_assert(sizeof(AoUniforms) % 16 == 0);

struct ComputePayload {
    WGPUTextureView depth;  // frame-pooled scene depth snapshot
    WGPUTextureView preprocessedDepthMips[5];
    WGPUTextureView preprocessedDepthAll;
    WGPUTextureView aoNoisy;
    WGPUTextureView depthDifferences;
    WGPUTextureView aoFinal;
    WGPUTextureView historyIn;
    WGPUTextureView historyOut;
    WGPUTextureView sceneNormal;  // GfxService scene normal snapshot (view space)
    uint32_t uniform_offset;
    uint32_t uniform_size;
    // Resolutions are packed (hi 16 = width, lo 16 = height) so the payload fits the 128-byte
    // GFX_INLINE_DRAW_PAYLOAD_SIZE exactly. Render resolutions are well under 65535.
    uint32_t chainSize;      // AO chain (half) resolution, packed
    uint32_t fullSize;       // full render resolution, packed
    uint32_t run_temporal;
    uint32_t denoise_passes; // 0-3; ping-pongs aoNoisy <-> aoFinal
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<ComputePayload>);

struct CompositePayload {
    WGPUTextureView aoSource;           // history, last denoise output, or aoNoisy (view 7)
    WGPUTextureView preprocessedDepth;  // all MIPs: upscale weights and the depth debug views
    WGPUTextureView sceneDepth;         // raw snapshot: reference depth, staircase view
    WGPUTextureView sceneNormal;        // scene normal snapshot, for debug views 2 and 6
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_view;
};
static_assert(sizeof(CompositePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<CompositePayload>);

// A debug view is staged by the SCENE_AFTER_OPAQUE hook and drawn at FRAME_AFTER_HUD, the last
// stage of the frame, so translucency, bloom, other mods' passes (Deferred Fog's quad) and the HUD
// cannot cover it, with no dependency on another mod's hook order. Game thread only; the payload's
// views stay valid for the rest of the frame.
CompositePayload g_pendingDebugDraw{};
bool g_debugDrawPending = false;

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

// Slices per pixel and marched steps per slice side for each Quality preset.
// Quality 4 = Custom: the Custom Slices / Custom Steps settings are used directly.
void quality_counts(int64_t quality, float& sliceCount, float& stepsPerSide) {
    switch (std::clamp<int64_t>(quality, 0, 4)) {
    case 0:
        sliceCount = 3.0f;
        stepsPerSide = 2.0f;
        break;
    case 1:
        sliceCount = 5.0f;
        stepsPerSide = 2.0f;
        break;
    default:
    case 2:
        sliceCount = 7.0f;
        stepsPerSide = 3.0f;
        break;
    case 3:
        sliceCount = 9.0f;
        stepsPerSide = 3.0f;
        break;
    case 4:
        sliceCount =
            static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarCustomSlices, 7), 1, 16));
        stepsPerSide =
            static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarCustomSteps, 3), 1, 8));
        break;
    }
}

// Column-major 4x4 multiply: out = a * b (matching the CameraService/WGSL convention).
void mat4_mul_col(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
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

bool build_composite_pipeline(const gfx_compat::ScenePassLayout& sceneLayout, bool blend,
    WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderModule module = create_shader_module("Enhanced AO composite", g_compositeSource);
    if (module == nullptr) {
        return false;
    }

    // Multiply blend: scene colour *= AO; scene alpha untouched.
    WGPUBlendState blendState{
        .color =
            {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_Dst,
                .dstFactor = WGPUBlendFactor_Zero,
            },
        .alpha =
            {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_Zero,
                .dstFactor = WGPUBlendFactor_One,
            },
    };
    // The pipeline must match the scene pass's attachments as they are now. Once the normal buffer
    // is on, the pass has a second, renderer-owned colour target and a one-target pipeline is
    // rejected. The layout comes from the draw context (see ensure_composite_pipelines). Targets
    // the mod does not own come back write-masked off, so only scene colour is written.
    gfx_compat::ScenePassLayout layout = sceneLayout;
    if (blend) {
        layout.color_targets[0].blend = &blendState;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = layout.color_target_count;
    fragment.targets = layout.color_targets;
    // The depth format must match the scene pass even though the composite neither tests nor
    // writes depth.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = layout.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {
        blend ? "Enhanced AO composite" : "Enhanced AO composite (debug)", WGPU_STRLEN};
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

void release_targets(AoTargets& targets) {
    const auto releaseView = [](WGPUTextureView& view) {
        if (view != nullptr) {
            wgpuTextureViewRelease(view);
            view = nullptr;
        }
    };
    const auto releaseTexture = [](WGPUTexture& texture) {
        if (texture != nullptr) {
            wgpuTextureRelease(texture);
            texture = nullptr;
        }
    };
    for (auto*& view : targets.preprocessedDepthMips) {
        releaseView(view);
    }
    releaseView(targets.preprocessedDepthAll);
    releaseView(targets.aoNoisyView);
    releaseView(targets.depthDifferencesView);
    releaseView(targets.aoFinalView);
    releaseView(targets.historyViews[0]);
    releaseView(targets.historyViews[1]);
    releaseTexture(targets.preprocessedDepth);
    releaseTexture(targets.aoNoisy);
    releaseTexture(targets.depthDifferences);
    releaseTexture(targets.aoFinal);
    releaseTexture(targets.history[0]);
    releaseTexture(targets.history[1]);
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

bool ensure_targets(uint32_t width, uint32_t height, uint32_t fullWidth, uint32_t fullHeight) {
    if (g_targets.width == width && g_targets.height == height) {
        return true;
    }
    if (g_targets.width != 0) {
        g_retiredTargets.push_back(RetiredTargets{std::exchange(g_targets, AoTargets{}), 4});
    }
    g_historyValid = false; // the history lives in the retired set; restart accumulation

    const auto createStorageTexture = [&](const char* label, WGPUTextureFormat format,
                                          uint32_t mipCount, uint32_t w, uint32_t h,
                                          WGPUTexture& outTexture) {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.label = {label, WGPU_STRLEN};
        texDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        texDesc.size = {w, h, 1};
        texDesc.format = format;
        texDesc.mipLevelCount = mipCount;
        outTexture = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
        return outTexture != nullptr;
    };

    // The AO chain runs at chain resolution; the temporal history is always full render
    // resolution so a half-res estimate can be reconstructed into it (temporal upsampling).
    bool ok = createStorageTexture("Enhanced AO preprocessed depth", WGPUTextureFormat_R32Float, 5,
                  width, height, g_targets.preprocessedDepth) &&
              createStorageTexture(
                  "Enhanced AO noisy", WGPUTextureFormat_R32Float, 1, width, height, g_targets.aoNoisy) &&
              createStorageTexture("Enhanced AO depth differences", WGPUTextureFormat_R32Uint, 1,
                  width, height, g_targets.depthDifferences) &&
              createStorageTexture(
                  "Enhanced AO final", WGPUTextureFormat_R32Float, 1, width, height, g_targets.aoFinal) &&
              // (ao, depth / far, octahedral normal .xy), 8 bytes per pixel; the normal is the
              // temporal pass's second surface-identity test.
              createStorageTexture("Enhanced AO history 0", WGPUTextureFormat_RGBA16Float, 1,
                  fullWidth, fullHeight, g_targets.history[0]) &&
              createStorageTexture("Enhanced AO history 1", WGPUTextureFormat_RGBA16Float, 1,
                  fullWidth, fullHeight, g_targets.history[1]);
    if (ok) {
        for (uint32_t mip = 0; mip < 5 && ok; ++mip) {
            WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            viewDesc.baseMipLevel = mip;
            viewDesc.mipLevelCount = 1;
            g_targets.preprocessedDepthMips[mip] =
                wgpuTextureCreateView(g_targets.preprocessedDepth, &viewDesc);
            ok = g_targets.preprocessedDepthMips[mip] != nullptr;
        }
    }
    if (ok) {
        g_targets.preprocessedDepthAll =
            wgpuTextureCreateView(g_targets.preprocessedDepth, nullptr);
        g_targets.aoNoisyView = wgpuTextureCreateView(g_targets.aoNoisy, nullptr);
        g_targets.depthDifferencesView = wgpuTextureCreateView(g_targets.depthDifferences, nullptr);
        g_targets.aoFinalView = wgpuTextureCreateView(g_targets.aoFinal, nullptr);
        g_targets.historyViews[0] = wgpuTextureCreateView(g_targets.history[0], nullptr);
        g_targets.historyViews[1] = wgpuTextureCreateView(g_targets.history[1], nullptr);
        ok = g_targets.preprocessedDepthAll != nullptr && g_targets.aoNoisyView != nullptr &&
             g_targets.depthDifferencesView != nullptr && g_targets.aoFinalView != nullptr &&
             g_targets.historyViews[0] != nullptr && g_targets.historyViews[1] != nullptr;
    }
    if (!ok) {
        release_targets(g_targets);
        return false;
    }
    g_targets.width = width;
    g_targets.height = height;
    g_targets.fullWidth = fullWidth;
    g_targets.fullHeight = fullHeight;
    return true;
}

constexpr uint32_t div_ceil(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

// Render worker thread: the AO chain as one compute pass (depth prefilter, occlusion, denoise, and
// optionally temporal accumulation).
void on_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(ComputePayload)) {
        return;
    }
    ComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.depth == nullptr || g_preprocessPipeline == nullptr) {
        return;
    }
    const uint32_t width = data.chainSize >> 16;
    const uint32_t height = data.chainSize & 0xFFFFu;
    const uint32_t fullWidth = data.fullSize >> 16;
    const uint32_t fullHeight = data.fullSize & 0xFFFFu;

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
    const auto release = [](WGPUBindGroup group) {
        if (group != nullptr) {
            wgpuBindGroupRelease(group);
        }
    };

    WGPUBindGroup preprocessGroup = makeBindGroup(g_preprocessLayout,
        {textureEntry(0, data.depth), textureEntry(1, data.preprocessedDepthMips[0]),
            textureEntry(2, data.preprocessedDepthMips[1]),
            textureEntry(3, data.preprocessedDepthMips[2]),
            textureEntry(4, data.preprocessedDepthMips[3]), uniformEntry(5)});
    WGPUBindGroup mip4Group =
        makeBindGroup(g_mip4Layout, {textureEntry(6, data.preprocessedDepthMips[3]),
                                        textureEntry(7, data.preprocessedDepthMips[4])});
    WGPUBindGroup vbaoGroup = makeBindGroup(
        g_vbaoLayout, {textureEntry(0, data.preprocessedDepthAll),
                          textureEntry(2, data.aoNoisy),
                          textureEntry(3, data.depthDifferences), uniformEntry(4),
                          textureEntry(5, data.sceneNormal)});
    // Denoise ping-pongs aoNoisy -> aoFinal -> aoNoisy -> aoFinal; the last-written buffer feeds
    // temporal/composite (the game thread computes the same parity for the composite payload).
    // From the second pass on, aoNoisy no longer holds the raw estimate.
    const uint32_t denoisePasses = std::min(data.denoise_passes, 3u);
    WGPUBindGroup denoiseGroups[3] = {};
    bool denoiseOk = true;
    for (uint32_t i = 0; i < denoisePasses; ++i) {
        const bool even = (i % 2u) == 0u;
        denoiseGroups[i] = makeBindGroup(g_denoiseLayout,
            {textureEntry(0, even ? data.aoNoisy : data.aoFinal),
                textureEntry(1, data.depthDifferences),
                textureEntry(2, even ? data.aoFinal : data.aoNoisy), uniformEntry(3)});
        denoiseOk = denoiseOk && denoiseGroups[i] != nullptr;
    }
    const WGPUTextureView denoisedView =
        denoisePasses == 0 ? data.aoNoisy : ((denoisePasses % 2u) != 0u ? data.aoFinal : data.aoNoisy);
    WGPUBindGroup temporalGroup = nullptr;
    if (data.run_temporal != 0) {
        // Binding 3 (raw_depth) is the full-res scene depth snapshot, the same texture the
        // prefilter reads.
        temporalGroup = makeBindGroup(g_temporalLayout,
            {textureEntry(0, denoisedView), textureEntry(1, data.historyIn),
                textureEntry(2, data.preprocessedDepthMips[0]), textureEntry(3, data.depth),
                textureEntry(4, data.historyOut), uniformEntry(5),
                textureEntry(6, data.sceneNormal)});
    }
    if (preprocessGroup == nullptr || mip4Group == nullptr || vbaoGroup == nullptr || !denoiseOk ||
        (data.run_temporal != 0 && temporalGroup == nullptr))
    {
        release(preprocessGroup);
        release(mip4Group);
        release(vbaoGroup);
        for (auto* group : denoiseGroups) {
            release(group);
        }
        release(temporalGroup);
        return;
    }

    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"Enhanced AO chain", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
    // Each preprocess workgroup covers 16x16 MIP-0 texels (8x8 invocations, 2x2 texels each).
    wgpuComputePassEncoderSetPipeline(pass, g_preprocessPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, preprocessGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(width, 16), div_ceil(height, 16), 1);
    wgpuComputePassEncoderSetPipeline(pass, g_mip4Pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, mip4Group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, div_ceil(std::max(width >> 4, 1u), 8),
        div_ceil(std::max(height >> 4, 1u), 8), 1);
    wgpuComputePassEncoderSetPipeline(pass, g_vbaoPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, vbaoGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(width, 8), div_ceil(height, 8), 1);
    if (denoisePasses > 0) {
        wgpuComputePassEncoderSetPipeline(pass, g_denoisePipeline);
        for (uint32_t i = 0; i < denoisePasses; ++i) {
            wgpuComputePassEncoderSetBindGroup(pass, 0, denoiseGroups[i], 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, div_ceil(width, 8), div_ceil(height, 8), 1);
        }
    }
    if (temporalGroup != nullptr) {
        // The temporal pass runs at full render resolution.
        wgpuComputePassEncoderSetPipeline(pass, g_temporalPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, temporalGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(
            pass, div_ceil(fullWidth, 8), div_ceil(fullHeight, 8), 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    release(preprocessGroup);
    release(mip4Group);
    release(vbaoGroup);
    for (auto* group : denoiseGroups) {
        release(group);
    }
    release(temporalGroup);
    g_chainExecuted.store(true, std::memory_order_release);
}

void release_composite_pipelines() {
    for (auto* pipeline : {&g_compositePipeline, &g_compositeDebugPipeline}) {
        if (*pipeline != nullptr) {
            wgpuRenderPipelineRelease(*pipeline);
            *pipeline = nullptr;
        }
    }
    for (auto* layout : {&g_compositeLayout, &g_compositeDebugLayout}) {
        if (*layout != nullptr) {
            wgpuBindGroupLayoutRelease(*layout);
            *layout = nullptr;
        }
    }
    g_sceneLayoutKey = 0;
    g_sceneLayoutValid = false;
}

// Builds the composite pipelines for the pass this draw is recorded into, and rebuilds them when
// GfxDrawContext::layout.key changes (as the SDK header asks).
//
// Important: the scene pass changes shape at runtime. The first normal request makes aurora add a
// normal attachment from the next frame on (lib/gfx/recording.cpp), and a pipeline built for the
// old one-target layout is rejected without any log, so the composite silently disappears. That is
// why nothing here is built at init; upstream mods/ao_mod does the same. Both the composite and
// the debug-view draw come through here; the steady state is one key comparison per draw.
bool ensure_composite_pipelines(const GfxDrawContext& ctx) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (g_sceneLayoutValid && key == g_sceneLayoutKey && g_compositePipeline != nullptr &&
        g_compositeDebugPipeline != nullptr)
    {
        return true;
    }
    release_composite_pipelines();
    gfx_compat::ScenePassLayout sceneLayout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, sceneLayout)) {
        return false;
    }
    if (!build_composite_pipeline(sceneLayout, true, g_compositePipeline, g_compositeLayout) ||
        !build_composite_pipeline(
            sceneLayout, false, g_compositeDebugPipeline, g_compositeDebugLayout))
    {
        release_composite_pipelines();
        return false;
    }
    g_sceneLayoutKey = key;
    g_sceneLayoutValid = true;
    return true;
}

// Render worker thread: multiply the AO over the scene, or draw a debug view opaquely.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(CompositePayload) || ctx == nullptr ||
        !ensure_composite_pipelines(*ctx))
    {
        return;
    }
    CompositePayload data;
    std::memcpy(&data, payload, sizeof(data));
    WGPURenderPipeline pipeline =
        data.debug_view != 0 ? g_compositeDebugPipeline : g_compositePipeline;
    WGPUBindGroupLayout layout = data.debug_view != 0 ? g_compositeDebugLayout : g_compositeLayout;
    if (data.aoSource == nullptr || data.preprocessedDepth == nullptr ||
        data.sceneDepth == nullptr || data.sceneNormal == nullptr || pipeline == nullptr)
    {
        return;
    }

    WGPUBindGroupEntry entries[5] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.aoSource;
    entries[1].binding = 1;
    entries[1].textureView = data.preprocessedDepth;
    entries[2].binding = 2;
    entries[2].textureView = data.sceneDepth;
    entries[3].binding = 3;
    entries[3].buffer = ctx->uniform_buffer;
    entries[3].offset = data.uniform_offset;
    entries[3].size = data.uniform_size;
    entries[4].binding = 4;
    entries[4].textureView = data.sceneNormal;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = 5;
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

// Game thread, after the opaque lists and before the translucent lists.
void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired_targets();
    // Sampled before the early-outs so the interval estimate follows every rendered frame.
    const float velocityCap = update_velocity_cap();
    if (!get_bool_option(g_cvarEnabled, true)) {
        g_historyValid = false;
        g_prevCameraValid = false;
        return;
    }
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext) ||
        stageCtx->game_view == nullptr)
    {
        return;
    }

    CameraInfo camera = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &camera) != MOD_OK) {
        return;
    }

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = false;
    resolveDesc.depth = true;
    // resolve_pass snapshots depth and the authored view-space normals as the scene pass stands at
    // this call, then ends that pass and continues on a new one that loads its contents; the
    // composite pushed below lands in the continuation. The normal fields go through
    // common/gfx_normal_compat.h, which detects them by member name, so an SDK without them builds
    // as "no normal buffer" instead of failing to compile.
    gfx_compat::request_normal(resolveDesc, true);
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    const bool resolveOk = svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) == MOD_OK;
    const WGPUTextureView sceneNormalView =
        resolveOk ? gfx_compat::resolved_normal(resolved) : nullptr;
    if (!resolveOk || resolved.depth == nullptr || sceneNormalView == nullptr) {
        // See kNormalLatchGraceFrames: the first frames after asking have no normal view by
        // design, so only a longer run means the device cannot supply one.
        if (resolveOk && resolved.depth != nullptr) {
            ++g_normalWaitFrames;
        }
        if (!g_warnedNoInputs &&
            (g_normalWaitFrames > kNormalLatchGraceFrames || !resolveOk ||
                resolved.depth == nullptr))
        {
            g_warnedNoInputs = true;
            GfxDeviceInfo live = GFX_DEVICE_INFO_INIT;
            const uint32_t samples =
                svc_gfx->get_device_info(mod_ctx, &live) == MOD_OK ? live.sample_count : 1u;
            if (sceneNormalView == nullptr && resolveOk && resolved.depth != nullptr &&
                samples > 1u)
            {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                    "scene normals unavailable; AO disabled. MSAA is on (%ux) and the scene normal "
                    "buffer requires MSAA off - set antialiasing to none in the game's video "
                    "settings.",
                    samples);
                svc_log->warn(mod_ctx, msg);
            } else {
                svc_log->warn(mod_ctx,
                    "scene depth or normals unavailable; AO disabled (the D3D11 and OpenGL ES "
                    "compatibility renderers cannot provide scene normals)");
            }
        }
        // Invalidate the temporal state as the disabled path does, so the first frame after the
        // inputs return does not reproject from a stale history and camera.
        g_historyValid = false;
        g_prevCameraValid = false;
        return;
    }
    g_normalWaitFrames = 0;

    const bool halfRes = get_bool_option(g_cvarHalfRes, false);
    const uint32_t divisor = halfRes ? 2 : 1;
    const uint32_t width = resolved.width / divisor;
    const uint32_t height = resolved.height / divisor;
    if (width < 32 || height < 32 ||
        !ensure_targets(width, height, resolved.width, resolved.height)) {
        return;
    }

    const bool temporal = get_bool_option(g_cvarTemporal, true);
    if (!temporal) {
        g_historyValid = false;
    }
    g_frameIndex++;

    AoUniforms uniforms{};
    std::memcpy(uniforms.projection, camera.proj_from_view, sizeof(uniforms.projection));
    std::memcpy(
        uniforms.inverse_projection, camera.view_from_proj, sizeof(uniforms.inverse_projection));
    // Reprojection: current view-space position -> previous frame's clip space. Without a previous
    // camera it is the current projection (the history is invalid then anyway).
    if (g_prevCameraValid) {
        mat4_mul_col(g_prevProjFromWorld, camera.world_from_view, uniforms.reproject);
    } else {
        std::memcpy(uniforms.reproject, camera.proj_from_view, sizeof(uniforms.reproject));
    }
    uniforms.size[0] = static_cast<float>(width);
    uniforms.size[1] = static_cast<float>(height);
    uniforms.inv_size[0] = 1.0f / uniforms.size[0];
    uniforms.inv_size[1] = 1.0f / uniforms.size[1];
    uniforms.depth_scale[0] = static_cast<float>(resolved.width) / uniforms.size[0];
    uniforms.depth_scale[1] = static_cast<float>(resolved.height) / uniforms.size[1];
    // Settings -> shader values. All of them go through the per-frame uniform block, so changing
    // them live costs no pipeline rebuild. Most are percent (/100); radius, radiusFar, thickDist
    // and depthBias are per-mille (/1000); distances are world units.
    const auto percent = [](ConfigVarHandle cvar, int64_t fallback, int64_t lo, int64_t hi) {
        return static_cast<float>(std::clamp<int64_t>(get_int_option(cvar, fallback), lo, hi)) /
               100.0f;
    };
    // Radius as a fraction of view depth; the setting is per-mille (100 = 10%).
    uniforms.effect_radius =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarRadius, 200), 25, 800)) /
        1000.0f;
    // Far radius (same scale, 0 = off) ramps in across [rampStart, rampEnd] world units of view
    // depth. World units rather than far-plane fractions, because the far plane varies per stage.
    uniforms.radius_far =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarRadiusFar, 800), 0, 800)) /
        1000.0f;
    uniforms.radius_ramp_start = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarRadiusRampStart, 0), 0, 200000));
    uniforms.radius_ramp_end = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarRadiusRampEnd, 10000), 500, 200000));
    uniforms.intensity = percent(g_cvarIntensity, 150, 0, 500);
    uniforms.contrast = percent(g_cvarContrast, 150, 50, 300);
    uniforms.thickness = percent(g_cvarThickness, 150, 25, 400);
    uniforms.black_point = percent(g_cvarBlackPoint, 3, 0, 30);
    uniforms.radius_max = percent(g_cvarRadiusMax, 40, 10, 100);
    uniforms.thick_fade = percent(g_cvarThickFade, 150, 50, 400);
    // Extra occluder thickness proportional to the view-space radius (per-mille). The base
    // thickness grows only logarithmically with the radius, which thins out mid/far occlusion.
    uniforms.thick_dist_scale =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarThickDist, 60), 0, 100)) /
        1000.0f;
    uniforms.inv_debug_depth =
        1.0f / static_cast<float>(
                   std::clamp<int64_t>(get_int_option(g_cvarDebugDepthRange, 3300), 500, 100000));
    // Self-occlusion bias: the view position is pulled toward the camera by this per-mille of its
    // depth (1 = x0.999).
    uniforms.depth_bias =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarDepthBias, 4), 0, 20)) /
        1000.0f;
    quality_counts(
        get_int_option(g_cvarQuality, 2), uniforms.slice_count, uniforms.steps_per_side);
    uniforms.denoise_strength = percent(g_cvarDenoiseStrength, 60, 0, 100);
    const int64_t temporalFrames = std::clamp<int64_t>(get_int_option(g_cvarTemporalFrames, 5), 2, 12);
    uniforms.temporal_alpha = 1.0f / static_cast<float>(temporalFrames);
    uniforms.temporal_clamp_k = percent(g_cvarTemporalClamp, 200, 100, 300);
    uniforms.velocity_scale = percent(g_cvarMotionResponse, 100, 0, 100);
    // World units of view depth: the motion response is full up to this depth and gone at twice
    // it (0 = no fade). Close up the screen-space search spans a small world radius and the
    // single-frame estimate is already clean; far away it spans a large one, the estimate is
    // sparse, and it needs the accumulation even in motion.
    uniforms.velocity_range = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarMotionRange, 5000), 0, 200000));
    uniforms.velocity_cap = velocityCap;
    uniforms.content_thresh = percent(g_cvarContentThresh, 100, 25, 300);
    uniforms.disocc_tol = percent(g_cvarDisoccTol, 0, 0, 20);
    const bool distanceFade = get_bool_option(g_cvarDistanceFade, false);
    uniforms.fade_start = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarFadeStart, 15000), 0, 200000));
    uniforms.fade_end = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarFadeEnd, 40000), 500, 200000));
    uniforms.inv_far = camera.far_plane > 1.0f ? 1.0f / camera.far_plane : 1.0f / 200000.0f;
    // Reference for the world-unit distance settings: log the far plane when it changes by >1%.
    if (camera.far_plane > 1.0f &&
        std::fabs(camera.far_plane - g_loggedFarPlane) > g_loggedFarPlane * 0.01f)
    {
        g_loggedFarPlane = camera.far_plane;
        char msg[96];
        std::snprintf(
            msg, sizeof(msg), "camera far plane: %.0f world units", camera.far_plane);
        svc_log->info(mod_ctx, msg);
    }
    const uint32_t debugMode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 8));
    uniforms.debug_view = debugMode;
    // The noise (and the half-res jitter) advances per frame only while accumulating; without
    // accumulation it is pinned, so the spatial denoiser sees a stable pattern.
    uniforms.frame_index = temporal ? g_frameIndex : 0u;
    uniforms.flags =
        (temporal ? 1u : 0u) | (g_historyValid ? 2u : 0u) | (distanceFade ? 4u : 0u);

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }

    const uint32_t writeIdx = g_historyWriteIndex;
    const uint32_t readIdx = 1u - writeIdx;

    ComputePayload computePayload{};
    computePayload.depth = resolved.depth;
    for (int mip = 0; mip < 5; ++mip) {
        computePayload.preprocessedDepthMips[mip] = g_targets.preprocessedDepthMips[mip];
    }
    computePayload.preprocessedDepthAll = g_targets.preprocessedDepthAll;
    computePayload.aoNoisy = g_targets.aoNoisyView;
    computePayload.depthDifferences = g_targets.depthDifferencesView;
    computePayload.aoFinal = g_targets.aoFinalView;
    computePayload.historyIn = g_targets.historyViews[readIdx];
    computePayload.historyOut = g_targets.historyViews[writeIdx];
    const uint32_t denoisePasses =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDenoisePasses, 1), 0, 3));
    computePayload.uniform_offset = uniformRange.offset;
    computePayload.uniform_size = uniformRange.size;
    computePayload.chainSize = (width << 16) | height;
    computePayload.fullSize = (resolved.width << 16) | resolved.height;
    computePayload.run_temporal = temporal ? 1u : 0u;
    computePayload.denoise_passes = denoisePasses;
    computePayload.sceneNormal = sceneNormalView;
    if (svc_gfx->push_compute(mod_ctx, g_computeType, &computePayload, sizeof(computePayload)) !=
        MOD_OK)
    {
        return;
    }

    // Mirror of on_compute's ping-pong parity: where the last denoise pass wrote.
    const WGPUTextureView denoisedView = denoisePasses == 0
        ? g_targets.aoNoisyView
        : ((denoisePasses % 2u) != 0u ? g_targets.aoFinalView : g_targets.aoNoisyView);
    // Debug view 7 reads aoNoisy, which holds the raw single-frame estimate only with 0 or 1
    // denoise passes; from 2 passes on the ping-pong has overwritten it with a denoised result.
    // Every other view and the real composite read the chain's final output.
    const WGPUTextureView aoSourceView = debugMode == 7u
        ? g_targets.aoNoisyView
        : (temporal ? g_targets.historyViews[writeIdx] : denoisedView);
    const CompositePayload drawPayload{
        aoSourceView, g_targets.preprocessedDepthAll,
        resolved.depth, computePayload.sceneNormal, uniformRange.offset, uniformRange.size,
        debugMode};
    if (debugMode != 0) {
        // Staged for on_frame_after_hud (see g_pendingDebugDraw); the debug view replaces the
        // composite for this frame.
        g_pendingDebugDraw = drawPayload;
        g_debugDrawPending = true;
    } else {
        svc_gfx->push_draw(mod_ctx, g_drawType, &drawPayload, sizeof(drawPayload));
    }

    // Advance the temporal state for the next frame.
    if (temporal) {
        g_historyWriteIndex = readIdx;
        g_historyValid = true;
    }
    std::memcpy(g_prevProjFromWorld, camera.proj_from_world, sizeof(g_prevProjFromWorld));
    g_prevCameraValid = true;
}

// Game thread, GFX_STAGE_FRAME_AFTER_HUD (the last stage of the frame): push the staged debug-view
// draw over the finished frame, HUD included.
void on_frame_after_hud(ModContext*, const GfxStageContext*, void*) {
    if (!g_debugDrawPending) {
        return;
    }
    g_debugDrawPending = false;
    svc_gfx->push_draw(mod_ctx, g_drawType, &g_pendingDebugDraw, sizeof(g_pendingDebugDraw));
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
    add_toggle(left, "Enabled", g_cvarEnabled, "Enables the ambient occlusion pass.");
    add_number(left, "Intensity", g_cvarIntensity,
        "How strongly occlusion darkens the scene.", 0, 500, 5, "%");
    add_number(left, "Contrast", g_cvarContrast,
        "Contrast of the occlusion falloff. Lower softens the transition; higher sharpens it.",
        50, 300, 10, "%");
    add_number(left, "Black Point", g_cvarBlackPoint,
        "Removes a small uniform occlusion floor so flat, open surfaces read as fully bright "
        "while real crevices are kept and rescaled. 0 disables.",
        0, 30, 1, "%");

    svc_ui->pane_add_section(mod_ctx, left, "Occlusion");
    static const char* kQualityOptions[] = {"Low", "Medium", "High", "Ultra", "Custom"};
    add_select(left, "Quality", g_cvarQuality,
        "Horizon slices and marched samples per pixel. Custom uses the two settings below.",
        kQualityOptions, 5);
    add_number(left, "Custom Slices", g_cvarCustomSlices,
        "Custom quality only: horizon slice count per pixel. The dominant cost factor.",
        1, 16, 1, nullptr);
    add_number(left, "Custom Steps", g_cvarCustomSteps,
        "Custom quality only: marched samples per slice side.", 1, 8, 1, nullptr);
    add_number(left, "Radius", g_cvarRadius,
        "How far the occlusion reaches up close, as a fraction of view distance (100 = 10%). "
        "Raise to broaden coverage; lower for tighter contact shadows.",
        25, 800, 25, nullptr);
    add_number(left, "Far Radius", g_cvarRadiusFar,
        "Radius at long view distances (same scale as Radius). The radius ramps from Radius up "
        "to this across the band below, so close-up content keeps tight contact detail while "
        "distant landmarks gain broad occlusion depth. 0 disables (constant Radius).",
        0, 800, 25, nullptr);
    add_number(left, "Far Radius Start", g_cvarRadiusRampStart,
        "View distance in world units where the radius starts ramping toward Far Radius. The "
        "log prints the stage's camera far plane for reference.",
        0, 200000, 500, nullptr);
    add_number(left, "Far Radius End", g_cvarRadiusRampEnd,
        "View distance in world units where the ramp reaches Far Radius.",
        500, 200000, 500, nullptr);
    add_number(left, "Max Screen Radius", g_cvarRadiusMax,
        "Hard cap on the screen-space search radius, as a share of screen height. The on-screen "
        "radius grows as Far Radius ramps in, so at default settings this cap limits Far Radius "
        "at long range (beyond roughly 4,600 world units at a 60-degree field of view). Raise it "
        "to let Far Radius take full effect at distance.",
        10, 100, 5, "%");
    add_number(left, "Thickness", g_cvarThickness,
        "How thick occluders are treated. Higher darkens the deepest part of contacts and "
        "crevices and widens coverage; lower keeps the effect thin and local.",
        25, 400, 25, "%");
    add_number(left, "Thickness Fade Range", g_cvarThickFade,
        "How far (relative to the radius) an occluder can sit in front of a surface before its "
        "influence fades out. Lower stops halos around silhouettes sooner; higher lets deep "
        "crevices darken further.",
        50, 400, 25, "%");
    add_number(left, "Distance Thickness", g_cvarThickDist,
        "Extra occluder thickness that scales with distance (per-mille of the search radius). "
        "The base thickness grows only logarithmically, which starves mid/far occlusion; raise "
        "this to keep distant geometry darkening at full strength. 0 restores the old "
        "behavior.",
        0, 100, 5, nullptr);
    add_number(left, "Depth Bias", g_cvarDepthBias,
        "Small bias toward the camera that suppresses a surface shadowing itself (speckle/acne "
        "on flat surfaces). Raise if flat surfaces show noise; too high loses fine contact "
        "detail.",
        0, 20, 1, nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Temporal");
    add_toggle(left, "Temporal Accumulation", g_cvarTemporal,
        "Accumulates occlusion across frames for a cleaner, more stable result. When off, the "
        "spatial denoiser alone filters each frame (sharper in motion, noisier on detail).");
    add_number(left, "Temporal Frames", g_cvarTemporalFrames,
        "Effective accumulation length. Higher is smoother but responds slower to change.",
        2, 12, 1, nullptr);
    add_number(left, "Temporal Clamp", g_cvarTemporalClamp,
        "How far history may drift from the current frame before it is clamped (in sigmas of the "
        "local AO distribution). Tightens automatically under screen motion. Lower is more "
        "responsive (less ghosting, more shimmer); higher accumulates more (cleaner, can ghost).",
        100, 300, 10, "%");
    add_number(left, "Motion Response", g_cvarMotionResponse,
        "How much screen motion shortens the accumulation so AO tracks geometry instead of "
        "dragging behind it. Applies within Motion Response Range and fades out beyond it, and "
        "is ceilinged by frame time so a full reset is only reachable at very high frame rates. "
        "Higher keeps characters' AO full and responsive in motion; lower accumulates more.",
        0, 100, 5, "%");
    add_number(left, "Motion Response Range", g_cvarMotionRange,
        "View distance in world units up to which Motion Response applies in full; it fades to "
        "nothing at twice this distance. Close geometry is sampled densely and its single-frame "
        "estimate is clean, so a short accumulation costs nothing there; distant, broad AO is "
        "sampled sparsely and needs the accumulation to stay solid in motion. 0 applies Motion "
        "Response at every distance.",
        0, 200000, 500, nullptr);
    add_number(left, "Content Response", g_cvarContentThresh,
        "Threshold for treating history as stale, measured in sigmas of the current local AO "
        "distribution (100% = discard from about 1 to 2.5 sigma). This is what removes trails "
        "left by moving occluders - Link's contact shadow on the ground he just left. Lower "
        "reacts faster to moving objects; higher accumulates more on noisy detail like grass.",
        25, 300, 25, "%");
    add_number(left, "Disocclusion Tolerance", g_cvarDisoccTol,
        "Depth mismatch (as % of the pixel's own depth) before reprojected history is treated as "
        "a different surface and discarded. Lower rejects more aggressively at silhouettes; "
        "higher keeps more history. Values below 1.5% act as 1.5%, which separates a character "
        "from the ground behind it at any distance.",
        0, 20, 1, "%");

    svc_ui->pane_add_section(mod_ctx, left, "Filtering");
    add_number(left, "Denoise Passes", g_cvarDenoisePasses,
        "Edge-aware spatial blur passes over the raw occlusion. 0 shows the raw estimate "
        "(noisy; useful with temporal accumulation on, or for judging the raw kernel). More "
        "passes are smoother but softer.",
        0, 3, 1, nullptr);
    add_number(left, "Denoise Strength", g_cvarDenoiseStrength,
        "How strongly each denoise pass blurs (0 = raw estimate, 100 = full blur). Lower "
        "preserves fine detail - with temporal accumulation carrying the noise reduction, "
        "moderate values keep the sharper look without visible noise.",
        0, 100, 5, "%");
    add_toggle(left, "Half Resolution", g_cvarHalfRes,
        "Computes occlusion at half resolution (about a quarter of the cost). With Temporal "
        "Accumulation on, a jittered temporal upsampler reconstructs full-resolution detail across "
        "frames, so the result stays close to full-res; with it off, a depth-aware bilinear upscale "
        "is used instead (silhouettes stay crisp, but softer).");

    svc_ui->pane_add_section(mod_ctx, left, "Distance Fade");
    add_toggle(left, "Distance Fade", g_cvarDistanceFade,
        "Fades the AO out with distance so far terrain (already washed toward fog) is not "
        "darkened. Off applies AO at full strength to the horizon.");
    add_number(left, "Fade Start", g_cvarFadeStart,
        "View distance in world units where the fade begins.", 0, 200000, 500, nullptr);
    add_number(left, "Fade End", g_cvarFadeEnd,
        "View distance in world units where the AO is fully faded out.", 500, 200000, 500,
        nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugOptions[] = {"Off", "AO", "Normals", "Depth", "Staircase",
        "Geo Normal", "Normal Agreement", "Raw AO", "Depth MIP 3"};
    add_select(left, "Debug View", g_cvarDebugView,
        "AO: the final shaped occlusion term as grayscale (accumulated when temporal is "
        "on).<br/>Normals: the view-space scene normals the occlusion pass consumes, black "
        "where the scene has none (sky, billboards) - exactly the pixels the AO leaves "
        "fully lit.<br/>Depth: the preprocessed depth as a distance "
        "gradient.<br/>Staircase: detects quantized depth - smooth depth is "
        "near-black with thin triangle edges, quantized depth lights up across "
        "surfaces.<br/>Geo Normal: the face normal derived from depth that the occlusion pass "
        "rejects below-surface samples against (magenta = degenerate, black = sky).<br/>Normal "
        "Agreement: how well the scene normal agrees with that face normal - green good, yellow "
        "the tilt smoothed low-poly curvature is expected to have, red poor, WHITE pointing away "
        "from the surface (a wrong-space or wrong-frame normal), blue no scene normal. Flat ground "
        "should read green.<br/>Raw AO: the single-frame estimate, unshaped and before "
        "accumulation and denoise. With Denoise Passes at 2 or 3 it shows a denoised result "
        "instead.<br/>Depth MIP 3: the coarse prefiltered depth the march reads for samples far "
        "from the pixel.<br/>Debug views draw over the finished frame (after fog, bloom and the "
        "HUD), so other effects never obscure them. When reporting broken AO, screenshots of AO, "
        "Normals, Geo Normal, Normal Agreement and Raw AO from the same spot, standing still, pin "
        "down which stage is wrong.",
        kDebugOptions, 9);
    add_number(left, "Debug Depth Range", g_cvarDebugDepthRange,
        "Distance scale of the Depth debug view's gradient, in world units: the view fades "
        "toward black across roughly 3x this distance. Raise to inspect large scenes; the "
        "visualization has no effect on the AO itself.",
        500, 100000, 500, nullptr);
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
        svc_log->error(mod_ctx, "failed to open Enhanced AO controls window");
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
        return mods::set_error(error, MOD_ERROR, "failed to register AO option");
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
        return mods::set_error(error, MOD_ERROR, "failed to register AO option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "preprocess_depth.wgsl", &g_preprocessSource);
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "vbao.wgsl", &g_vbaoSource);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "denoise.wgsl", &g_denoiseSource);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "temporal.wgsl", &g_temporalSource);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "composite.wgsl", &g_compositeSource);
    }
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load AO shaders");
    }

    const struct {
        const char* name;
        bool defaultValue;
        ConfigVarHandle* handle;
    } boolOptions[] = {
        {"effectEnabled", true, &g_cvarEnabled},
        {"temporal", true, &g_cvarTemporal},
        {"distanceFade", false, &g_cvarDistanceFade},
        {"halfRes", true, &g_cvarHalfRes},
    };
    for (const auto& opt : boolOptions) {
        result = register_bool_option(opt.name, opt.defaultValue, *opt.handle, error);
        if (result != MOD_OK) {
            return result;
        }
    }
    const struct {
        const char* name;
        int64_t defaultValue;
        ConfigVarHandle* handle;
    } intOptions[] = {
        {"quality", 2, &g_cvarQuality},
        {"customSlices", 7, &g_cvarCustomSlices},
        {"customSteps", 3, &g_cvarCustomSteps},
        {"radius", 200, &g_cvarRadius},
        {"radiusFar", 800, &g_cvarRadiusFar},
        {"radiusRampStart", 0, &g_cvarRadiusRampStart},
        {"radiusRampEnd", 10000, &g_cvarRadiusRampEnd},
        {"radiusMax", 40, &g_cvarRadiusMax},
        {"intensity", 150, &g_cvarIntensity},
        {"contrast", 150, &g_cvarContrast},
        {"blackPoint", 1, &g_cvarBlackPoint},
        {"thickness", 150, &g_cvarThickness},
        {"thickFade", 150, &g_cvarThickFade},
        {"thickDist", 60, &g_cvarThickDist},
        {"depthBias", 1, &g_cvarDepthBias},
        {"debugDepthRange", 3300, &g_cvarDebugDepthRange},
        {"temporalFrames", 8, &g_cvarTemporalFrames},
        {"temporalClamp", 200, &g_cvarTemporalClamp},
        {"motionResponse", 100, &g_cvarMotionResponse},
        {"motionRange", 5000, &g_cvarMotionRange},
        {"contentThresh", 100, &g_cvarContentThresh},
        {"disoccTol", 0, &g_cvarDisoccTol},
        {"denoisePasses", 1, &g_cvarDenoisePasses},
        {"denoiseStrength", 60, &g_cvarDenoiseStrength},
        {"fadeStart", 15000, &g_cvarFadeStart},
        {"fadeEnd", 40000, &g_cvarFadeEnd},
        {"debugMode", 0, &g_cvarDebugView},
    };
    for (const auto& opt : intOptions) {
        result = register_int_option(opt.name, opt.defaultValue, *opt.handle, error);
        if (result != MOD_OK) {
            return result;
        }
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    log_adapter_info();
    if (!build_compute_pipeline("Enhanced AO preprocess depth", g_preprocessSource,
            "preprocess_depth", g_preprocessPipeline, g_preprocessLayout) ||
        !build_compute_pipeline("Enhanced AO downsample mip4", g_preprocessSource,
            "downsample_mip4", g_mip4Pipeline, g_mip4Layout) ||
        !build_compute_pipeline(
            "Enhanced AO VBAO", g_vbaoSource, "vbao", g_vbaoPipeline, g_vbaoLayout) ||
        !build_compute_pipeline("Enhanced AO denoise", g_denoiseSource, "spatial_denoise",
            g_denoisePipeline, g_denoiseLayout) ||
        !build_compute_pipeline("Enhanced AO temporal", g_temporalSource, "temporal_accumulate",
            g_temporalPipeline, g_temporalLayout))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create AO compute pipelines");
    }
    // The composite pipelines are not built here: they depend on the scene pass layout, which
    // changes at runtime once the normal request takes effect. See ensure_composite_pipelines.

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "Enhanced AO chain";
    computeDesc.callback = on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "Enhanced AO composite";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register draw type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc, &g_afterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_frame_after_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_AFTER_HUD, &stageDesc, &g_afterHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "vbao ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (!g_loggedChain && g_chainExecuted.load(std::memory_order_acquire)) {
        g_loggedChain = true;
        svc_log->info(mod_ctx, "Enhanced AO chain executed OK");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_resource->free(mod_ctx, &g_preprocessSource);
    svc_resource->free(mod_ctx, &g_vbaoSource);
    svc_resource->free(mod_ctx, &g_denoiseSource);
    svc_resource->free(mod_ctx, &g_temporalSource);
    svc_resource->free(mod_ctx, &g_compositeSource);

    release_targets(g_targets);
    for (auto& retired : g_retiredTargets) {
        release_targets(retired.targets);
    }
    g_retiredTargets.clear();

    const auto releasePipeline = [](WGPUComputePipeline& pipeline) {
        if (pipeline != nullptr) {
            wgpuComputePipelineRelease(pipeline);
            pipeline = nullptr;
        }
    };
    const auto releaseLayout = [](WGPUBindGroupLayout& layout) {
        if (layout != nullptr) {
            wgpuBindGroupLayoutRelease(layout);
            layout = nullptr;
        }
    };
    releasePipeline(g_preprocessPipeline);
    releasePipeline(g_mip4Pipeline);
    releasePipeline(g_vbaoPipeline);
    releasePipeline(g_denoisePipeline);
    releasePipeline(g_temporalPipeline);
    releaseLayout(g_preprocessLayout);
    releaseLayout(g_mip4Layout);
    releaseLayout(g_vbaoLayout);
    releaseLayout(g_denoiseLayout);
    releaseLayout(g_temporalLayout);
    // Also clears the cached layout key, so a reload rebuilds against the pass as it is then.
    release_composite_pipelines();
    g_cvarEnabled = g_cvarQuality = g_cvarCustomSlices = g_cvarCustomSteps = 0;
    g_cvarRadius = g_cvarRadiusFar = g_cvarRadiusRampStart = g_cvarRadiusRampEnd = 0;
    g_cvarRadiusMax = g_cvarIntensity = g_cvarContrast = 0;
    g_cvarBlackPoint = g_cvarThickness = g_cvarThickFade = g_cvarThickDist = g_cvarDepthBias = 0;
    g_cvarDebugDepthRange = 0;
    g_cvarTemporal = g_cvarTemporalFrames = g_cvarTemporalClamp = g_cvarMotionResponse = 0;
    g_cvarMotionRange = 0;
    g_cvarContentThresh = g_cvarDisoccTol = g_cvarDenoisePasses = g_cvarDenoiseStrength = 0;
    g_cvarDistanceFade = g_cvarFadeStart = g_cvarFadeEnd = 0;
    g_cvarHalfRes = g_cvarDebugView = 0;
    g_computeType = g_drawType = 0;
    g_afterOpaqueHook = g_afterHudHook = 0;
    g_debugDrawPending = false;
    g_controlsWindow = 0;
    g_frameIndex = 0;
    g_historyWriteIndex = 0;
    g_historyValid = false;
    g_prevCameraValid = false;
    g_normalWaitFrames = 0;
    g_warnedNoInputs = false;
    g_loggedFarPlane = 1.0f;
    g_frameTimeValid = false;
    g_frameTimeSeeded = false;
    g_smoothedFrameDt = 1.0f / 60.0f;
    g_loggedFrameDt = 0.0f;
    return MOD_OK;
}
}
