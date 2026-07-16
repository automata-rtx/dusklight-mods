// SSILVB — Screen Space Indirect Lighting with Visibility Bitmask (Therrien et al. 2023).
//
// One bounce of indirect diffuse light gathered through the same 32-sector visibility-bitmask
// slice walk our VBAO mod uses: each marched sample's NEWLY occluded sectors are exactly the
// solid angle through which its surface is visible from the receiver, so its radiance (scene
// color, linearized and MIP-prefiltered) is accumulated with that weight before the mask update.
// With the bounce toggled off the mod degenerates to a standalone directional AO (the sampling
// loop skips every color/normal fetch and costs the same as VBAO).
//
// Chain (per frame, GFX_STAGE_SCENE_AFTER_OPAQUE): depth MIP prefilter -> color MIP prefilter
// (gamma decode + box chain) -> SSILVB sampling -> edge-aware spatial denoise (rgba) ->
// temporal accumulation (rgba history + split depth plane) -> composite. The composite outputs
// (GI_add.rgb, AO_mul) in one draw and the blend state combines both with the scene:
// out = GI + dst * AO (Add mode) or out = GI * (1 - dst) + dst * AO (Screen mode, default).
//
// Service-only: gfx/camera/config/ui/resource/log, plus a HARD dependency on the Depth to Normal
// provider mod (receiver normal AND every march sample's emitter normal come from it).
//
// The framework WGSL in res/ derives from Bevy Engine's SSAO (MIT OR Apache-2.0) and Intel
// XeGTAO (MIT); see res/licenses/ and the headers of each shader.
//
// Design + rationale: docs/ssilvb_plan.md (read §0 first).

#include "mods/service.hpp"
#include "depth_to_normal_service.h"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <atomic>
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
// HARD dependency (unlike VBAO's optional import): the GI accumulate needs a surface normal at
// every marched sample; reconstructing those per sample would quintuple the depth fetches.
IMPORT_SERVICE(DepthToNormalService, svc_n2d);

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarGiEnabled = 0;
ConfigVarHandle g_cvarGiIntensity = 0;
ConfigVarHandle g_cvarGiBlendMode = 0;
ConfigVarHandle g_cvarChromaLift = 0;
ConfigVarHandle g_cvarBounceWhite = 0;
ConfigVarHandle g_cvarAoApply = 0;
ConfigVarHandle g_cvarAoIntensity = 0;
ConfigVarHandle g_cvarContrast = 0;
ConfigVarHandle g_cvarBlackPoint = 0;
ConfigVarHandle g_cvarQuality = 0;
ConfigVarHandle g_cvarCustomSlices = 0;
ConfigVarHandle g_cvarCustomSteps = 0;
ConfigVarHandle g_cvarRadius = 0;
ConfigVarHandle g_cvarRadiusFar = 0;
ConfigVarHandle g_cvarRadiusRampStart = 0;
ConfigVarHandle g_cvarRadiusRampEnd = 0;
ConfigVarHandle g_cvarRadiusMax = 0;
ConfigVarHandle g_cvarThickness = 0;
ConfigVarHandle g_cvarThickFade = 0;
ConfigVarHandle g_cvarThickDist = 0;
ConfigVarHandle g_cvarDepthBias = 0;
ConfigVarHandle g_cvarTemporal = 0;
ConfigVarHandle g_cvarTemporalFrames = 0;
ConfigVarHandle g_cvarTemporalClamp = 0;
ConfigVarHandle g_cvarMotionResponse = 0;
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
GfxStageHookHandle g_beforeHudHook = 0;
UiWindowHandle g_controlsWindow = 0;

ResourceBuffer g_preprocessSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_colorSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_ssilvbSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_denoiseSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_temporalSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_compositeSource = RESOURCE_BUFFER_INIT;

GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_preprocessPipeline = nullptr;
WGPUComputePipeline g_mip4Pipeline = nullptr;
WGPUComputePipeline g_colorMip0Pipeline = nullptr;
WGPUComputePipeline g_colorReducePipeline = nullptr;
WGPUComputePipeline g_ssilvbPipeline = nullptr;
WGPUComputePipeline g_denoisePipeline = nullptr;
WGPUComputePipeline g_temporalPipeline = nullptr;
WGPUBindGroupLayout g_preprocessLayout = nullptr;
WGPUBindGroupLayout g_mip4Layout = nullptr;
WGPUBindGroupLayout g_colorMip0Layout = nullptr;
WGPUBindGroupLayout g_colorReduceLayout = nullptr;
WGPUBindGroupLayout g_ssilvbLayout = nullptr;
WGPUBindGroupLayout g_denoiseLayout = nullptr;
WGPUBindGroupLayout g_temporalLayout = nullptr;
WGPURenderPipeline g_compositeScreenPipeline = nullptr;
WGPURenderPipeline g_compositeAddPipeline = nullptr;
WGPURenderPipeline g_compositeDebugPipeline = nullptr;
WGPUBindGroupLayout g_compositeScreenLayout = nullptr;
WGPUBindGroupLayout g_compositeAddLayout = nullptr;
WGPUBindGroupLayout g_compositeDebugLayout = nullptr;
WGPUTexture g_hilbertLut = nullptr;
WGPUTextureView g_hilbertLutView = nullptr;

// The chain needs more texture views than fit the 128-byte inline payload, so the payload carries
// a pointer to this table instead. It is heap-allocated per Targets generation and retired with
// its Targets (kept alive for a few frames after replacement) so a payload already in flight on
// the render worker never sees a dangling pointer. Single-mip views serve both as storage (write)
// and sampled (read) bindings, like VBAO's depth MIP views.
struct TargetViews {
    WGPUTextureView preprocessedDepthMips[5] = {};
    WGPUTextureView preprocessedDepthAll = nullptr;
    WGPUTextureView colorMips[5] = {};
    WGPUTextureView colorChainAll = nullptr;
    WGPUTextureView giNoisy = nullptr;
    WGPUTextureView depthDifferences = nullptr;
    WGPUTextureView giFinal = nullptr;
    WGPUTextureView historyColor[2] = {};
    WGPUTextureView historyDepth[2] = {};
};

// Chain targets, recreated when the render size (or halfRes) changes. Old sets are retired for a
// few frames instead of released immediately: payloads embedding the views table pointer may
// still be in flight on the render worker.
struct Targets {
    uint32_t width = 0;   // chain resolution (half the render size in Half Res)
    uint32_t height = 0;
    uint32_t fullWidth = 0;   // full render resolution (temporal history / raw snapshot)
    uint32_t fullHeight = 0;
    WGPUTexture preprocessedDepth = nullptr;
    WGPUTexture colorChain = nullptr;
    WGPUTexture giNoisy = nullptr;
    WGPUTexture depthDifferences = nullptr;
    WGPUTexture giFinal = nullptr;
    // Temporal ping-pong: rgba16float (GI.rgb, AO) + r32float (normalized view depth).
    WGPUTexture historyColor[2] = {};
    WGPUTexture historyDepth[2] = {};
    TargetViews* views = nullptr;
};
Targets g_targets;
struct RetiredTargets {
    Targets targets;
    int framesLeft = 0;
};
std::vector<RetiredTargets> g_retiredTargets;

// Temporal state (game thread only).
uint32_t g_frameIndex = 0;
uint32_t g_historyWriteIndex = 0;
bool g_historyValid = false;   // the read history holds a valid previous accumulation
bool g_prevCameraValid = false;
float g_prevProjFromWorld[16] = {};

bool g_warnedNoSnapshots = false;
bool g_loggedNoProvider = false;
bool g_loggedChain = false;
float g_loggedFarPlane = 1.0f;  // last far plane reported to the log (world-unit calibration)
std::atomic g_chainExecuted{false};

// Mirror of the WGSL Uniforms struct (keep in sync with EVERY res/*.wgsl).
struct SsilvbUniforms {
    float projection[16];
    float inverse_projection[16];
    float reproject[16];
    float view_from_world[16];  // rotates the Depth to Normal provider's world normal into view
    float size[2];
    float inv_size[2];
    float depth_scale[2];
    float effect_radius;
    float intensity;            // AO strength (composite)
    float slice_count;
    float steps_per_side;
    float thickness;
    float contrast;
    float temporal_alpha;
    float temporal_clamp_k;
    float inv_far;
    float radius_max;
    float depth_bias;
    float thick_fade;
    float velocity_scale;
    float content_thresh;
    float disocc_tol;
    float black_point;
    float fade_start;
    float fade_end;
    uint32_t debug_view;
    uint32_t frame_index;
    uint32_t flags; // bit 0 temporal, bit 1 history valid, bit 2 distance fade,
                    // bit 3 GI enabled, bit 4 AO apply, bit 5 white bounce proxy
    float thick_dist_scale;  // extra occluder thickness, fraction of the view-space radius
    float radius_far;        // far effect radius (fraction of view depth); 0 disables the ramp
    float radius_ramp_start; // radius ramp band start, world units of view depth
    float radius_ramp_end;   // radius ramp band end, world units of view depth
    float denoise_strength;  // spatial denoise blend, 0 raw .. 1 fully blurred
    float gi_intensity;      // indirect bounce strength (composite)
    float chroma_lift;       // receiver albedo proxy: 0 raw scene color .. 1 full chroma norm
    float _pad0;
    float _pad1;
};
static_assert(sizeof(SsilvbUniforms) % 16 == 0);

struct ComputePayload {
    WGPUTextureView depth;       // frame-pooled scene depth snapshot
    WGPUTextureView sceneColor;  // frame-pooled scene color snapshot (light input)
    WGPUTextureView d2nNormal;   // Depth to Normal provider output (frame-valid)
    const TargetViews* views;    // mod-owned view table; retire keeps it alive across frames
    uint32_t uniform_offset;
    uint32_t uniform_size;
    // Resolutions packed (hi 16 = width, lo 16 = height); render sizes are well under 65535.
    uint32_t chainSize;      // chain (half) resolution, packed
    uint32_t fullSize;       // full render resolution, packed
    uint32_t run_temporal;
    uint32_t run_gi;         // skip the color-chain dispatches entirely when the bounce is off
    uint32_t denoise_passes; // 0-3; ping-pongs giNoisy <-> giFinal
    uint32_t history_write;  // temporal ping-pong write index (read = 1 - write)
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<ComputePayload>);

struct CompositePayload {
    WGPUTextureView giSource;           // accumulated (temporal) or denoised (fallback) GI+AO
    WGPUTextureView preprocessedDepth;  // chain MIP0 (depth-aware upscale weights)
    WGPUTextureView sceneDepth;         // raw snapshot
    WGPUTextureView sceneColor;         // raw snapshot (receiver albedo proxy + debug view)
    WGPUTextureView d2nNormal;          // provider normals (debug view)
    WGPUTextureView colorChain;         // linear light MIPs (debug view)
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_view;
    uint32_t blend_mode;  // 0 = Screen, 1 = Add
};
static_assert(sizeof(CompositePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<CompositePayload>);

// Debug views draw at FRAME_BEFORE_HUD instead of SCENE_AFTER_OPAQUE so nothing layered on
// after the opaque scene (deferred fog, bloom, translucency) obscures them; the payload is
// staged here between the two stages (game thread only; its views live for the frame).
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

// Slices x marched steps per slice side. GI wants MORE steps per slice than AO (every step is a
// potential light source; a miss reads as missing light, not just missing shadow) and tolerates
// FEWER slices (radiance is lower-frequency than visibility) — hence the different shape from
// VBAO's presets. Quality 4 = Custom: the raw slice/step settings are used directly.
void quality_counts(int64_t quality, float& sliceCount, float& stepsPerSide) {
    switch (std::clamp<int64_t>(quality, 0, 4)) {
    case 0:
        sliceCount = 2.0f;
        stepsPerSide = 3.0f;
        break;
    case 1:
        sliceCount = 3.0f;
        stepsPerSide = 4.0f;
        break;
    default:
    case 2:
        sliceCount = 4.0f;
        stepsPerSide = 6.0f;
        break;
    case 3:
        sliceCount = 6.0f;
        stepsPerSide = 8.0f;
        break;
    case 4:
        sliceCount =
            static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarCustomSlices, 4), 1, 16));
        stepsPerSide =
            static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarCustomSteps, 6), 1, 8));
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

enum class CompositeMode { Screen, Add, Debug };

// One fragment output drives both operations through the blend state:
//   Add:    out = src.rgb + dst * src.a          (GI added, AO multiplied)
//   Screen: out = src.rgb * (1 - dst) + dst * src.a  (GI screened — never clips on LDR)
//   Debug:  no blend (replace), for the debug views.
bool build_composite_pipeline(
    CompositeMode mode, WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderModule module = create_shader_module("SSILVB composite", g_compositeSource);
    if (module == nullptr) {
        return false;
    }

    WGPUBlendState blendState{
        .color =
            {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = mode == CompositeMode::Screen ? WGPUBlendFactor_OneMinusDst
                                                           : WGPUBlendFactor_One,
                .dstFactor = WGPUBlendFactor_SrcAlpha,
            },
        .alpha =
            {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_Zero,
                .dstFactor = WGPUBlendFactor_One,
            },
    };
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = g_deviceInfo.color_format;
    if (mode != CompositeMode::Debug) {
        colorTarget.blend = &blendState;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;
    // Depth state must match the EFB pass despite never touching depth.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = g_deviceInfo.depth_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {mode == CompositeMode::Screen ? "SSILVB composite (screen)"
                          : mode == CompositeMode::Add  ? "SSILVB composite (add)"
                                                        : "SSILVB composite (debug)",
        WGPU_STRLEN};
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

// Hilbert curve index LUT for the R2 noise sequence, generated once at init.
uint16_t hilbert_index(uint16_t x, uint16_t y) {
    uint16_t index = 0;
    for (uint16_t level = 32; level > 0; level /= 2) {
        const uint16_t regionX = (x & level) > 0 ? 1 : 0;
        const uint16_t regionY = (y & level) > 0 ? 1 : 0;
        index += level * level * ((3 * regionX) ^ regionY);
        if (regionY == 0) {
            if (regionX == 1) {
                x = 63 - x;
                y = 63 - y;
            }
            std::swap(x, y);
        }
    }
    return index;
}

bool build_hilbert_lut() {
    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.label = {"SSILVB hilbert LUT", WGPU_STRLEN};
    texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texDesc.size = {64, 64, 1};
    texDesc.format = WGPUTextureFormat_R16Uint;
    g_hilbertLut = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
    if (g_hilbertLut == nullptr) {
        return false;
    }
    g_hilbertLutView = wgpuTextureCreateView(g_hilbertLut, nullptr);
    if (g_hilbertLutView == nullptr) {
        return false;
    }

    uint16_t lut[64 * 64];
    for (uint16_t y = 0; y < 64; ++y) {
        for (uint16_t x = 0; x < 64; ++x) {
            lut[y * 64 + x] = hilbert_index(x, y);
        }
    }
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = g_hilbertLut;
    WGPUTexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = 64 * 2, .rowsPerImage = 64};
    WGPUExtent3D extent{64, 64, 1};
    wgpuQueueWriteTexture(g_deviceInfo.queue, &dst, lut, sizeof(lut), &layout, &extent);
    return true;
}

void release_targets(Targets& targets) {
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
    if (targets.views != nullptr) {
        for (auto*& view : targets.views->preprocessedDepthMips) {
            releaseView(view);
        }
        releaseView(targets.views->preprocessedDepthAll);
        for (auto*& view : targets.views->colorMips) {
            releaseView(view);
        }
        releaseView(targets.views->colorChainAll);
        releaseView(targets.views->giNoisy);
        releaseView(targets.views->depthDifferences);
        releaseView(targets.views->giFinal);
        releaseView(targets.views->historyColor[0]);
        releaseView(targets.views->historyColor[1]);
        releaseView(targets.views->historyDepth[0]);
        releaseView(targets.views->historyDepth[1]);
        delete targets.views;
        targets.views = nullptr;
    }
    releaseTexture(targets.preprocessedDepth);
    releaseTexture(targets.colorChain);
    releaseTexture(targets.giNoisy);
    releaseTexture(targets.depthDifferences);
    releaseTexture(targets.giFinal);
    releaseTexture(targets.historyColor[0]);
    releaseTexture(targets.historyColor[1]);
    releaseTexture(targets.historyDepth[0]);
    releaseTexture(targets.historyDepth[1]);
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
        g_retiredTargets.push_back(RetiredTargets{std::exchange(g_targets, Targets{}), 4});
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

    // The chain runs at chain (half) res; the temporal history is full render res so the
    // half-res estimate can be reconstructed into it (temporal upsampling). At full res the two
    // sizes coincide.
    bool ok = createStorageTexture("SSILVB preprocessed depth", WGPUTextureFormat_R32Float, 5,
                  width, height, g_targets.preprocessedDepth) &&
              createStorageTexture("SSILVB color chain", WGPUTextureFormat_RGBA16Float, 5, width,
                  height, g_targets.colorChain) &&
              createStorageTexture("SSILVB noisy", WGPUTextureFormat_RGBA16Float, 1, width,
                  height, g_targets.giNoisy) &&
              createStorageTexture("SSILVB depth differences", WGPUTextureFormat_R32Uint, 1,
                  width, height, g_targets.depthDifferences) &&
              createStorageTexture("SSILVB final", WGPUTextureFormat_RGBA16Float, 1, width,
                  height, g_targets.giFinal) &&
              createStorageTexture("SSILVB history color 0", WGPUTextureFormat_RGBA16Float, 1,
                  fullWidth, fullHeight, g_targets.historyColor[0]) &&
              createStorageTexture("SSILVB history color 1", WGPUTextureFormat_RGBA16Float, 1,
                  fullWidth, fullHeight, g_targets.historyColor[1]) &&
              createStorageTexture("SSILVB history depth 0", WGPUTextureFormat_R32Float, 1,
                  fullWidth, fullHeight, g_targets.historyDepth[0]) &&
              createStorageTexture("SSILVB history depth 1", WGPUTextureFormat_R32Float, 1,
                  fullWidth, fullHeight, g_targets.historyDepth[1]);
    if (ok) {
        g_targets.views = new TargetViews{};
        for (uint32_t mip = 0; mip < 5 && ok; ++mip) {
            WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            viewDesc.baseMipLevel = mip;
            viewDesc.mipLevelCount = 1;
            g_targets.views->preprocessedDepthMips[mip] =
                wgpuTextureCreateView(g_targets.preprocessedDepth, &viewDesc);
            g_targets.views->colorMips[mip] =
                wgpuTextureCreateView(g_targets.colorChain, &viewDesc);
            ok = g_targets.views->preprocessedDepthMips[mip] != nullptr &&
                 g_targets.views->colorMips[mip] != nullptr;
        }
    }
    if (ok) {
        TargetViews& v = *g_targets.views;
        v.preprocessedDepthAll = wgpuTextureCreateView(g_targets.preprocessedDepth, nullptr);
        v.colorChainAll = wgpuTextureCreateView(g_targets.colorChain, nullptr);
        v.giNoisy = wgpuTextureCreateView(g_targets.giNoisy, nullptr);
        v.depthDifferences = wgpuTextureCreateView(g_targets.depthDifferences, nullptr);
        v.giFinal = wgpuTextureCreateView(g_targets.giFinal, nullptr);
        v.historyColor[0] = wgpuTextureCreateView(g_targets.historyColor[0], nullptr);
        v.historyColor[1] = wgpuTextureCreateView(g_targets.historyColor[1], nullptr);
        v.historyDepth[0] = wgpuTextureCreateView(g_targets.historyDepth[0], nullptr);
        v.historyDepth[1] = wgpuTextureCreateView(g_targets.historyDepth[1], nullptr);
        ok = v.preprocessedDepthAll != nullptr && v.colorChainAll != nullptr &&
             v.giNoisy != nullptr && v.depthDifferences != nullptr && v.giFinal != nullptr &&
             v.historyColor[0] != nullptr && v.historyColor[1] != nullptr &&
             v.historyDepth[0] != nullptr && v.historyDepth[1] != nullptr;
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

// Render worker thread: the SSILVB chain as one compute pass (depth prefilter, color prefilter,
// sampling, denoise, and optionally temporal accumulation).
void on_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(ComputePayload)) {
        return;
    }
    ComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.depth == nullptr || data.views == nullptr || g_preprocessPipeline == nullptr) {
        return;
    }
    const TargetViews& views = *data.views;
    const uint32_t width = data.chainSize >> 16;
    const uint32_t height = data.chainSize & 0xFFFFu;
    const uint32_t fullWidth = data.fullSize >> 16;
    const uint32_t fullHeight = data.fullSize & 0xFFFFu;
    const uint32_t writeIdx = data.history_write & 1u;
    const uint32_t readIdx = 1u - writeIdx;

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
        {textureEntry(0, data.depth), textureEntry(1, views.preprocessedDepthMips[0]),
            textureEntry(2, views.preprocessedDepthMips[1]),
            textureEntry(3, views.preprocessedDepthMips[2]),
            textureEntry(4, views.preprocessedDepthMips[3]), uniformEntry(5)});
    WGPUBindGroup mip4Group =
        makeBindGroup(g_mip4Layout, {textureEntry(6, views.preprocessedDepthMips[3]),
                                        textureEntry(7, views.preprocessedDepthMips[4])});
    // Color chain (only dispatched when the bounce is on).
    WGPUBindGroup colorMip0Group = nullptr;
    WGPUBindGroup colorReduceGroups[4] = {};
    bool colorOk = true;
    if (data.run_gi != 0) {
        colorMip0Group =
            makeBindGroup(g_colorMip0Layout, {textureEntry(0, data.sceneColor),
                                                 textureEntry(1, views.colorMips[0]),
                                                 uniformEntry(2)});
        colorOk = colorMip0Group != nullptr && data.sceneColor != nullptr;
        for (uint32_t mip = 1; mip < 5 && colorOk; ++mip) {
            colorReduceGroups[mip - 1] =
                makeBindGroup(g_colorReduceLayout, {textureEntry(3, views.colorMips[mip - 1]),
                                                       textureEntry(4, views.colorMips[mip])});
            colorOk = colorReduceGroups[mip - 1] != nullptr;
        }
    }
    WGPUBindGroup ssilvbGroup = makeBindGroup(
        g_ssilvbLayout, {textureEntry(0, views.preprocessedDepthAll),
                            textureEntry(1, g_hilbertLutView), textureEntry(2, views.giNoisy),
                            textureEntry(3, views.depthDifferences), uniformEntry(4),
                            textureEntry(5, data.d2nNormal), textureEntry(6, views.colorChainAll)});
    // Denoise ping-pongs giNoisy <-> giFinal; the last-written buffer feeds temporal/composite
    // (the game thread computes the same parity for the composite payload).
    const uint32_t denoisePasses = std::min(data.denoise_passes, 3u);
    WGPUBindGroup denoiseGroups[3] = {};
    bool denoiseOk = true;
    for (uint32_t i = 0; i < denoisePasses; ++i) {
        const bool even = (i % 2u) == 0u;
        denoiseGroups[i] = makeBindGroup(g_denoiseLayout,
            {textureEntry(0, even ? views.giNoisy : views.giFinal),
                textureEntry(1, views.depthDifferences),
                textureEntry(2, even ? views.giFinal : views.giNoisy), uniformEntry(3)});
        denoiseOk = denoiseOk && denoiseGroups[i] != nullptr;
    }
    const WGPUTextureView denoisedView =
        denoisePasses == 0 ? views.giNoisy
                           : ((denoisePasses % 2u) != 0u ? views.giFinal : views.giNoisy);
    WGPUBindGroup temporalGroup = nullptr;
    if (data.run_temporal != 0) {
        // binding 3 (raw_depth) reuses the full-res scene depth snapshot (data.depth), which the
        // preprocess pass also consumes as its input.
        temporalGroup = makeBindGroup(g_temporalLayout,
            {textureEntry(0, denoisedView), textureEntry(1, views.historyColor[readIdx]),
                textureEntry(2, views.preprocessedDepthMips[0]), textureEntry(3, data.depth),
                textureEntry(4, views.historyColor[writeIdx]), uniformEntry(5),
                textureEntry(6, views.historyDepth[readIdx]),
                textureEntry(7, views.historyDepth[writeIdx])});
    }
    if (preprocessGroup == nullptr || mip4Group == nullptr || ssilvbGroup == nullptr ||
        !colorOk || !denoiseOk || (data.run_temporal != 0 && temporalGroup == nullptr))
    {
        release(preprocessGroup);
        release(mip4Group);
        release(colorMip0Group);
        for (auto* group : colorReduceGroups) {
            release(group);
        }
        release(ssilvbGroup);
        for (auto* group : denoiseGroups) {
            release(group);
        }
        release(temporalGroup);
        return;
    }

    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"SSILVB chain", WGPU_STRLEN};
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
    if (data.run_gi != 0) {
        wgpuComputePassEncoderSetPipeline(pass, g_colorMip0Pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, colorMip0Group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(
            pass, div_ceil(width, 8), div_ceil(height, 8), 1);
        wgpuComputePassEncoderSetPipeline(pass, g_colorReducePipeline);
        for (uint32_t mip = 1; mip < 5; ++mip) {
            const uint32_t mipWidth = std::max(width >> mip, 1u);
            const uint32_t mipHeight = std::max(height >> mip, 1u);
            wgpuComputePassEncoderSetBindGroup(pass, 0, colorReduceGroups[mip - 1], 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, div_ceil(mipWidth, 8), div_ceil(mipHeight, 8), 1);
        }
    }
    wgpuComputePassEncoderSetPipeline(pass, g_ssilvbPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, ssilvbGroup, 0, nullptr);
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
        // The temporal pass runs at full render resolution (it reconstructs the half-res estimate
        // into the full-res history).
        wgpuComputePassEncoderSetPipeline(pass, g_temporalPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, temporalGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(
            pass, div_ceil(fullWidth, 8), div_ceil(fullHeight, 8), 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    release(preprocessGroup);
    release(mip4Group);
    release(colorMip0Group);
    for (auto* group : colorReduceGroups) {
        release(group);
    }
    release(ssilvbGroup);
    for (auto* group : denoiseGroups) {
        release(group);
    }
    release(temporalGroup);
    g_chainExecuted.store(true, std::memory_order_release);
}

// Render worker thread: composite GI + AO over the scene (or show a debug view).
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(CompositePayload)) {
        return;
    }
    CompositePayload data;
    std::memcpy(&data, payload, sizeof(data));
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout layout;
    if (data.debug_view != 0) {
        pipeline = g_compositeDebugPipeline;
        layout = g_compositeDebugLayout;
    } else if (data.blend_mode != 0) {
        pipeline = g_compositeAddPipeline;
        layout = g_compositeAddLayout;
    } else {
        pipeline = g_compositeScreenPipeline;
        layout = g_compositeScreenLayout;
    }
    if (data.giSource == nullptr || data.preprocessedDepth == nullptr ||
        data.sceneDepth == nullptr || data.sceneColor == nullptr || data.d2nNormal == nullptr ||
        data.colorChain == nullptr || pipeline == nullptr)
    {
        return;
    }

    WGPUBindGroupEntry entries[7] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.giSource;
    entries[1].binding = 1;
    entries[1].textureView = data.preprocessedDepth;
    entries[2].binding = 2;
    entries[2].textureView = data.sceneDepth;
    entries[3].binding = 3;
    entries[3].buffer = ctx->uniform_buffer;
    entries[3].offset = data.uniform_offset;
    entries[3].size = data.uniform_size;
    entries[4].binding = 4;
    entries[4].textureView = data.sceneColor;
    entries[5].binding = 5;
    entries[5].textureView = data.d2nNormal;
    entries[6].binding = 6;
    entries[6].textureView = data.colorChain;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = 7;
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

// Game thread, after opaque scene draws and before translucent/fog overlay lists.
void on_scene_after_opaque(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired_targets();
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

    // Color AND depth: the color snapshot is the bounce light input (and the albedo proxy).
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr || resolved.color == nullptr)
    {
        if (!g_warnedNoSnapshots) {
            g_warnedNoSnapshots = true;
            svc_log->warn(mod_ctx, "scene snapshots unavailable; SSILVB disabled");
        }
        return;
    }

    // Hard requirement: the provider's world-space normal buffer (receiver + emitter normals).
    // MOD_UNAVAILABLE just means no populated scene yet (loading, menus) - skip the frame.
    DepthToNormalFrame n2dFrame = DEPTH_TO_NORMAL_FRAME_INIT;
    if (svc_n2d->get_frame(mod_ctx, &n2dFrame) != MOD_OK || n2dFrame.normal == nullptr) {
        if (!g_loggedNoProvider) {
            g_loggedNoProvider = true;
            svc_log->info(mod_ctx, "waiting for the Depth to Normal provider's first frame");
        }
        return;
    }

    const bool halfRes = get_bool_option(g_cvarHalfRes, true);
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

    SsilvbUniforms uniforms{};
    std::memcpy(uniforms.projection, camera.proj_from_view, sizeof(uniforms.projection));
    std::memcpy(
        uniforms.inverse_projection, camera.view_from_proj, sizeof(uniforms.inverse_projection));
    // Reprojection: current view-space position -> previous frame's clip space.
    if (g_prevCameraValid) {
        mat4_mul_col(g_prevProjFromWorld, camera.world_from_view, uniforms.reproject);
    } else {
        std::memcpy(uniforms.reproject, camera.proj_from_view, sizeof(uniforms.reproject));
    }
    // For rotating the Depth to Normal provider's world-space normal into view space.
    std::memcpy(
        uniforms.view_from_world, camera.view_from_world, sizeof(uniforms.view_from_world));
    uniforms.size[0] = static_cast<float>(width);
    uniforms.size[1] = static_cast<float>(height);
    uniforms.inv_size[0] = 1.0f / uniforms.size[0];
    uniforms.inv_size[1] = 1.0f / uniforms.size[1];
    uniforms.depth_scale[0] = static_cast<float>(resolved.width) / uniforms.size[0];
    uniforms.depth_scale[1] = static_cast<float>(resolved.height) / uniforms.size[1];
    // Percent/permille settings -> shader values. Every one of these rides the per-frame uniform
    // block, so changing them live has no rebuild or pipeline cost.
    const auto percent = [](ConfigVarHandle cvar, int64_t fallback, int64_t lo, int64_t hi) {
        return static_cast<float>(std::clamp<int64_t>(get_int_option(cvar, fallback), lo, hi)) /
               100.0f;
    };
    // Depth-proportional radius: the setting is a permille of the view distance (100 = 10%).
    uniforms.effect_radius =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarRadius, 200), 25, 800)) /
        1000.0f;
    uniforms.radius_far =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarRadiusFar, 800), 0, 800)) /
        1000.0f;
    uniforms.radius_ramp_start = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarRadiusRampStart, 0), 0, 200000));
    uniforms.radius_ramp_end = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarRadiusRampEnd, 10000), 500, 200000));
    uniforms.intensity = percent(g_cvarAoIntensity, 150, 0, 500);
    uniforms.gi_intensity = percent(g_cvarGiIntensity, 200, 0, 800);
    uniforms.chroma_lift = percent(g_cvarChromaLift, 50, 0, 100);
    uniforms.contrast = percent(g_cvarContrast, 150, 50, 300);
    uniforms.thickness = percent(g_cvarThickness, 150, 25, 400);
    uniforms.black_point = percent(g_cvarBlackPoint, 3, 0, 30);
    uniforms.radius_max = percent(g_cvarRadiusMax, 40, 10, 100);
    uniforms.thick_fade = percent(g_cvarThickFade, 150, 50, 400);
    uniforms.thick_dist_scale =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarThickDist, 60), 0, 100)) /
        1000.0f;
    // Self-occlusion bias in permille toward the camera (4 = the 0.996 factor).
    uniforms.depth_bias =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarDepthBias, 4), 0, 20)) /
        1000.0f;
    quality_counts(
        get_int_option(g_cvarQuality, 2), uniforms.slice_count, uniforms.steps_per_side);
    uniforms.denoise_strength = percent(g_cvarDenoiseStrength, 60, 0, 100);
    const int64_t temporalFrames =
        std::clamp<int64_t>(get_int_option(g_cvarTemporalFrames, 8), 2, 16);
    uniforms.temporal_alpha = 1.0f / static_cast<float>(temporalFrames);
    uniforms.temporal_clamp_k = percent(g_cvarTemporalClamp, 250, 100, 400);
    uniforms.velocity_scale = percent(g_cvarMotionResponse, 10, 0, 100);
    uniforms.content_thresh = percent(g_cvarContentThresh, 100, 25, 300);
    uniforms.disocc_tol = percent(g_cvarDisoccTol, 0, 0, 20);
    const bool distanceFade = get_bool_option(g_cvarDistanceFade, false);
    uniforms.fade_start = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarFadeStart, 15000), 0, 200000));
    uniforms.fade_end = static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarFadeEnd, 40000), 500, 200000));
    uniforms.inv_far = camera.far_plane > 1.0f ? 1.0f / camera.far_plane : 1.0f / 200000.0f;
    // Calibration aid for the world-unit distance settings above: log the stage's far plane
    // whenever it changes materially (once per change, not per frame).
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
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 5));
    uniforms.debug_view = debugMode;
    // The noise advances per frame only while accumulating; pinned otherwise (the spatial
    // denoiser alone then sees a stable pattern, matching the single-frame fallback).
    uniforms.frame_index = temporal ? g_frameIndex : 0u;
    const bool giEnabled = get_bool_option(g_cvarGiEnabled, true);
    const bool aoApply = get_bool_option(g_cvarAoApply, true);
    const bool bounceWhite = get_bool_option(g_cvarBounceWhite, false);
    uniforms.flags = (temporal ? 1u : 0u) | (g_historyValid ? 2u : 0u) |
        (distanceFade ? 4u : 0u) | (giEnabled ? 8u : 0u) | (aoApply ? 16u : 0u) |
        (bounceWhite ? 32u : 0u);

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }

    const uint32_t writeIdx = g_historyWriteIndex;
    const uint32_t readIdx = 1u - writeIdx;

    ComputePayload computePayload{};
    computePayload.depth = resolved.depth;
    computePayload.sceneColor = resolved.color;
    computePayload.d2nNormal = n2dFrame.normal;
    computePayload.views = g_targets.views;
    const uint32_t denoisePasses =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int_option(g_cvarDenoisePasses, 2), 0, 3));
    computePayload.uniform_offset = uniformRange.offset;
    computePayload.uniform_size = uniformRange.size;
    computePayload.chainSize = (width << 16) | height;
    computePayload.fullSize = (resolved.width << 16) | resolved.height;
    computePayload.run_temporal = temporal ? 1u : 0u;
    computePayload.run_gi = giEnabled ? 1u : 0u;
    computePayload.denoise_passes = denoisePasses;
    computePayload.history_write = writeIdx;
    if (svc_gfx->push_compute(mod_ctx, g_computeType, &computePayload, sizeof(computePayload)) !=
        MOD_OK)
    {
        return;
    }

    // Mirror of on_compute's ping-pong parity: where the last denoise pass wrote.
    const WGPUTextureView denoisedView = denoisePasses == 0
        ? g_targets.views->giNoisy
        : ((denoisePasses % 2u) != 0u ? g_targets.views->giFinal : g_targets.views->giNoisy);
    const uint32_t blendMode = static_cast<uint32_t>(
        std::clamp<int64_t>(get_int_option(g_cvarGiBlendMode, 0), 0, 1));
    const CompositePayload drawPayload{
        temporal ? g_targets.views->historyColor[writeIdx] : denoisedView,
        g_targets.views->preprocessedDepthMips[0], resolved.depth, resolved.color,
        n2dFrame.normal, g_targets.views->colorChainAll, uniformRange.offset, uniformRange.size,
        debugMode, blendMode};
    if (debugMode != 0) {
        // Debug views draw at FRAME_BEFORE_HUD so deferred fog, translucency, and bloom
        // don't paint over them (all payload views stay valid for the rest of the frame).
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

// Game thread, after the full 3D scene: push the staged debug-view draw, unobscured by
// everything the scene layered on after the opaque pass.
void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
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
    add_toggle(left, "Enabled", g_cvarEnabled, "Enables the SSILVB pass.");
    add_toggle(left, "Indirect Bounce", g_cvarGiEnabled,
        "One bounce of colored light gathered from nearby surfaces. Off turns the mod into a "
        "standalone directional ambient-occlusion effect (all light sampling is skipped).");
    add_number(left, "Bounce Intensity", g_cvarGiIntensity,
        "How strongly the gathered bounce light brightens the scene.", 0, 800, 10, "%");
    static const char* kBlendOptions[] = {"Screen", "Add"};
    add_select(left, "Bounce Blend", g_cvarGiBlendMode,
        "How the bounce light combines with the scene. Screen never over-brightens (softer, "
        "recommended); Add is a plain sum (punchier, can clip on bright surfaces).",
        kBlendOptions, 2);
    add_number(left, "Chroma Lift", g_cvarChromaLift,
        "How the receiving surface tints its bounce light. 0 uses the surface's on-screen color "
        "directly (most faithful in vanilla's flat lighting); 100 keeps only its hue/saturation "
        "(better when shadow/AO mods darken surfaces, so shaded receivers still show bounce).",
        0, 100, 5, "%");
    add_toggle(left, "White Bounce", g_cvarBounceWhite,
        "Ignores the receiving surface's color and adds the raw gathered light (chalky; mainly "
        "for judging the light transport itself).");
    add_toggle(left, "Apply AO", g_cvarAoApply,
        "Also darken by the ambient occlusion this pass measures. Turn OFF when running the "
        "VBAO mod alongside, otherwise the scene is darkened twice.");
    add_number(left, "AO Intensity", g_cvarAoIntensity,
        "How strongly occlusion darkens the scene.", 0, 500, 5, "%");
    add_number(left, "Contrast", g_cvarContrast,
        "Contrast of the occlusion falloff. Lower softens the transition; higher sharpens it.",
        50, 300, 10, "%");
    add_number(left, "Black Point", g_cvarBlackPoint,
        "Removes a small uniform occlusion floor so flat, open surfaces read as fully bright "
        "while real crevices are kept and rescaled. 0 disables.",
        0, 30, 1, "%");

    svc_ui->pane_add_section(mod_ctx, left, "Sampling");
    static const char* kQualityOptions[] = {"Low", "Medium", "High", "Ultra", "Custom"};
    add_select(left, "Quality", g_cvarQuality,
        "Hemisphere slices and marched samples per pixel. Custom uses the two settings below.",
        kQualityOptions, 5);
    add_number(left, "Custom Slices", g_cvarCustomSlices,
        "Custom quality only: hemisphere slice count per pixel.", 1, 16, 1, nullptr);
    add_number(left, "Custom Steps", g_cvarCustomSteps,
        "Custom quality only: marched samples per slice side. Each step is a potential light "
        "source, so GI benefits from more steps than AO alone would need.",
        1, 8, 1, nullptr);
    add_number(left, "Radius", g_cvarRadius,
        "How far light and occlusion are gathered up close, as a fraction of view distance "
        "(100 = 10%). Wider radii pull bounce light from farther away.",
        25, 800, 25, nullptr);
    add_number(left, "Far Radius", g_cvarRadiusFar,
        "Radius at long view distances (same scale as Radius). The radius ramps from Radius up "
        "to this across the band below. 0 disables (constant Radius).",
        0, 800, 25, nullptr);
    add_number(left, "Far Radius Start", g_cvarRadiusRampStart,
        "View distance in world units where the radius starts ramping toward Far Radius. The "
        "log prints the stage's camera far plane for reference.",
        0, 200000, 500, nullptr);
    add_number(left, "Far Radius End", g_cvarRadiusRampEnd,
        "View distance in world units where the ramp reaches Far Radius.",
        500, 200000, 500, nullptr);
    add_number(left, "Max Screen Radius", g_cvarRadiusMax,
        "Hard cap on the screen-space search radius, as a share of screen height. Only engages "
        "to bound sampling cost when Radius is pushed very high.",
        10, 100, 5, "%");
    add_number(left, "Thickness", g_cvarThickness,
        "How thick occluders are treated. Higher darkens contacts and blocks more light behind "
        "surfaces; lower lets more light pass behind thin geometry.",
        25, 400, 25, "%");
    add_number(left, "Thickness Fade Range", g_cvarThickFade,
        "How far (relative to the radius) an occluder can sit in front of a surface before its "
        "influence fades out. Lower stops halos around silhouettes sooner.",
        50, 400, 25, "%");
    add_number(left, "Distance Thickness", g_cvarThickDist,
        "Extra occluder thickness that scales with distance (per-mille of the search radius); "
        "keeps mid/far geometry contributing at full strength. 0 disables.",
        0, 100, 5, nullptr);
    add_number(left, "Depth Bias", g_cvarDepthBias,
        "Small bias toward the camera that suppresses a surface shadowing (and lighting) "
        "itself. Raise if flat surfaces show noise.",
        0, 20, 1, nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Temporal");
    add_toggle(left, "Temporal Accumulation", g_cvarTemporal,
        "Accumulates light and occlusion across frames for a cleaner, more stable result. When "
        "off, the spatial denoiser alone filters each frame (noisier - GI needs this more than "
        "AO does).");
    add_number(left, "Temporal Frames", g_cvarTemporalFrames,
        "Effective accumulation length. Higher is smoother but responds slower to change; "
        "bounce light benefits from a longer tail than AO.",
        2, 16, 1, nullptr);
    add_number(left, "Temporal Clamp", g_cvarTemporalClamp,
        "How far history may drift from the current frame before it is clamped, per color "
        "channel. Lower is more responsive (less ghosting, more shimmer); higher accumulates "
        "more (cleaner, can ghost colored light).",
        100, 400, 10, "%");
    add_number(left, "Motion Response", g_cvarMotionResponse,
        "How much camera motion shortens the accumulation so light tracks geometry instead of "
        "dragging behind it.",
        0, 100, 5, "%");
    add_number(left, "Content Response", g_cvarContentThresh,
        "Threshold for treating a history/current mismatch as real change (animated objects, "
        "moving light). Lower reacts faster; higher accumulates more.",
        25, 300, 25, "%");
    add_number(left, "Disocclusion Tolerance", g_cvarDisoccTol,
        "Depth mismatch (as % of depth) before reprojected history is treated as a different "
        "surface and discarded. 0 rejects the most aggressively (minimizes ghosting).",
        0, 20, 1, "%");

    svc_ui->pane_add_section(mod_ctx, left, "Filtering");
    add_number(left, "Denoise Passes", g_cvarDenoisePasses,
        "Edge-aware spatial blur passes over the raw estimate. GI is noisier than AO, so it "
        "defaults higher; 0 shows the raw estimate.",
        0, 3, 1, nullptr);
    add_number(left, "Denoise Strength", g_cvarDenoiseStrength,
        "How strongly each denoise pass blurs (0 = raw estimate, 100 = full blur).",
        0, 100, 5, "%");
    add_toggle(left, "Half Resolution", g_cvarHalfRes,
        "Computes the effect at half resolution (about a quarter of the cost - the recommended "
        "default; the paper's own headline configuration). With Temporal Accumulation on, a "
        "jittered temporal upsampler reconstructs full-resolution detail across frames.");

    svc_ui->pane_add_section(mod_ctx, left, "Distance Fade");
    add_toggle(left, "Distance Fade", g_cvarDistanceFade,
        "Fades both the bounce light and the AO out with distance so far terrain (already "
        "washed toward fog) is neither darkened nor re-lit.");
    add_number(left, "Fade Start", g_cvarFadeStart,
        "View distance in world units where the fade begins.", 0, 200000, 500, nullptr);
    add_number(left, "Fade End", g_cvarFadeEnd,
        "View distance in world units where the effect is fully faded out.", 500, 200000, 500,
        nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugOptions[] = {
        "Off", "Bounce Light", "AO", "Albedo Proxy", "Light Input", "Normals"};
    add_select(left, "Debug View", g_cvarDebugView,
        "Bounce Light: the added indirect light over black.<br/>AO: the shaped occlusion term "
        "as grayscale.<br/>Albedo Proxy: the receiver tint derived from the scene (see Chroma "
        "Lift).<br/>Light Input: the prefiltered scene radiance the bounce actually samples."
        "<br/>Normals: the Depth to Normal provider's world-space normals.<br/>Debug views draw "
        "over the finished frame (after fog and bloom), so other effects never obscure them.",
        kDebugOptions, 6);
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
        svc_log->error(mod_ctx, "failed to open SSILVB controls window");
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
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Indirect Bounce";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarGiEnabled;
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
        return mods::set_error(error, MOD_ERROR, "failed to register SSILVB option");
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
        return mods::set_error(error, MOD_ERROR, "failed to register SSILVB option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "preprocess_depth.wgsl", &g_preprocessSource);
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "preprocess_color.wgsl", &g_colorSource);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "ssilvb.wgsl", &g_ssilvbSource);
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
        return mods::set_error(error, result, "failed to load SSILVB shaders");
    }

    const struct {
        const char* name;
        bool defaultValue;
        ConfigVarHandle* handle;
    } boolOptions[] = {
        {"effectEnabled", true, &g_cvarEnabled},
        {"giEnabled", true, &g_cvarGiEnabled},
        {"bounceWhite", false, &g_cvarBounceWhite},
        {"aoApply", true, &g_cvarAoApply},
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
        {"giIntensity", 200, &g_cvarGiIntensity},
        {"giBlendMode", 0, &g_cvarGiBlendMode},
        {"chromaLift", 50, &g_cvarChromaLift},
        {"aoIntensity", 150, &g_cvarAoIntensity},
        {"contrast", 150, &g_cvarContrast},
        {"blackPoint", 3, &g_cvarBlackPoint},
        {"quality", 2, &g_cvarQuality},
        {"customSlices", 4, &g_cvarCustomSlices},
        {"customSteps", 6, &g_cvarCustomSteps},
        {"radius", 200, &g_cvarRadius},
        {"radiusFar", 800, &g_cvarRadiusFar},
        {"radiusRampStart", 0, &g_cvarRadiusRampStart},
        {"radiusRampEnd", 10000, &g_cvarRadiusRampEnd},
        {"radiusMax", 40, &g_cvarRadiusMax},
        {"thickness", 150, &g_cvarThickness},
        {"thickFade", 150, &g_cvarThickFade},
        {"thickDist", 60, &g_cvarThickDist},
        {"depthBias", 4, &g_cvarDepthBias},
        {"temporalFrames", 8, &g_cvarTemporalFrames},
        {"temporalClamp", 250, &g_cvarTemporalClamp},
        {"motionResponse", 10, &g_cvarMotionResponse},
        {"contentThresh", 100, &g_cvarContentThresh},
        {"disoccTol", 0, &g_cvarDisoccTol},
        {"denoisePasses", 2, &g_cvarDenoisePasses},
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
    if (!build_compute_pipeline("SSILVB preprocess depth", g_preprocessSource,
            "preprocess_depth", g_preprocessPipeline, g_preprocessLayout) ||
        !build_compute_pipeline("SSILVB downsample mip4", g_preprocessSource,
            "downsample_mip4", g_mip4Pipeline, g_mip4Layout) ||
        !build_compute_pipeline("SSILVB color mip0", g_colorSource, "prefilter_color",
            g_colorMip0Pipeline, g_colorMip0Layout) ||
        !build_compute_pipeline("SSILVB color reduce", g_colorSource, "reduce_color",
            g_colorReducePipeline, g_colorReduceLayout) ||
        !build_compute_pipeline(
            "SSILVB sampling", g_ssilvbSource, "ssilvb", g_ssilvbPipeline, g_ssilvbLayout) ||
        !build_compute_pipeline("SSILVB denoise", g_denoiseSource, "spatial_denoise",
            g_denoisePipeline, g_denoiseLayout) ||
        !build_compute_pipeline("SSILVB temporal", g_temporalSource, "temporal_accumulate",
            g_temporalPipeline, g_temporalLayout))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create SSILVB compute pipelines");
    }
    if (!build_composite_pipeline(
            CompositeMode::Screen, g_compositeScreenPipeline, g_compositeScreenLayout) ||
        !build_composite_pipeline(
            CompositeMode::Add, g_compositeAddPipeline, g_compositeAddLayout) ||
        !build_composite_pipeline(
            CompositeMode::Debug, g_compositeDebugPipeline, g_compositeDebugLayout))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create SSILVB composite pipeline");
    }
    if (!build_hilbert_lut()) {
        return mods::set_error(error, MOD_ERROR, "failed to create SSILVB noise LUT");
    }

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "SSILVB chain";
    computeDesc.callback = on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "SSILVB composite";
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
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_beforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "ssilvb ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (!g_loggedChain && g_chainExecuted.load(std::memory_order_acquire)) {
        g_loggedChain = true;
        svc_log->info(mod_ctx, "SSILVB chain executed OK");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_resource->free(mod_ctx, &g_preprocessSource);
    svc_resource->free(mod_ctx, &g_colorSource);
    svc_resource->free(mod_ctx, &g_ssilvbSource);
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
    const auto releaseRenderPipeline = [](WGPURenderPipeline& pipeline) {
        if (pipeline != nullptr) {
            wgpuRenderPipelineRelease(pipeline);
            pipeline = nullptr;
        }
    };
    releasePipeline(g_preprocessPipeline);
    releasePipeline(g_mip4Pipeline);
    releasePipeline(g_colorMip0Pipeline);
    releasePipeline(g_colorReducePipeline);
    releasePipeline(g_ssilvbPipeline);
    releasePipeline(g_denoisePipeline);
    releasePipeline(g_temporalPipeline);
    releaseLayout(g_preprocessLayout);
    releaseLayout(g_mip4Layout);
    releaseLayout(g_colorMip0Layout);
    releaseLayout(g_colorReduceLayout);
    releaseLayout(g_ssilvbLayout);
    releaseLayout(g_denoiseLayout);
    releaseLayout(g_temporalLayout);
    releaseRenderPipeline(g_compositeScreenPipeline);
    releaseRenderPipeline(g_compositeAddPipeline);
    releaseRenderPipeline(g_compositeDebugPipeline);
    releaseLayout(g_compositeScreenLayout);
    releaseLayout(g_compositeAddLayout);
    releaseLayout(g_compositeDebugLayout);
    if (g_hilbertLutView != nullptr) {
        wgpuTextureViewRelease(g_hilbertLutView);
        g_hilbertLutView = nullptr;
    }
    if (g_hilbertLut != nullptr) {
        wgpuTextureRelease(g_hilbertLut);
        g_hilbertLut = nullptr;
    }
    g_cvarEnabled = g_cvarGiEnabled = g_cvarGiIntensity = g_cvarGiBlendMode = 0;
    g_cvarChromaLift = g_cvarBounceWhite = g_cvarAoApply = g_cvarAoIntensity = 0;
    g_cvarContrast = g_cvarBlackPoint = 0;
    g_cvarQuality = g_cvarCustomSlices = g_cvarCustomSteps = 0;
    g_cvarRadius = g_cvarRadiusFar = g_cvarRadiusRampStart = g_cvarRadiusRampEnd = 0;
    g_cvarRadiusMax = g_cvarThickness = g_cvarThickFade = g_cvarThickDist = g_cvarDepthBias = 0;
    g_cvarTemporal = g_cvarTemporalFrames = g_cvarTemporalClamp = g_cvarMotionResponse = 0;
    g_cvarContentThresh = g_cvarDisoccTol = g_cvarDenoisePasses = g_cvarDenoiseStrength = 0;
    g_cvarDistanceFade = g_cvarFadeStart = g_cvarFadeEnd = 0;
    g_cvarHalfRes = g_cvarDebugView = 0;
    g_computeType = g_drawType = 0;
    g_afterOpaqueHook = g_beforeHudHook = 0;
    g_debugDrawPending = false;
    g_controlsWindow = 0;
    g_frameIndex = 0;
    g_historyWriteIndex = 0;
    g_historyValid = false;
    g_prevCameraValid = false;
    g_loggedFarPlane = 1.0f;
    return MOD_OK;
}
}
