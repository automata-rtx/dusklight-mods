// Realtime Sun Shadows.
//
// Scene-wide dynamic sun/moon shadows, built on the dynamic shadows demo mod: the game's populated
// opaque scene draw lists are replayed into an offscreen pass with a light-space projection to
// produce a shadow map of the live scene, then deferred shadows are composited over the world
// (scene depth + CameraService unproject + PCF against the map).
//
// On top of the demo: slope-scaled bias + normal-offset receivers (kills shadow acne on sloped
// surfaces without the peter-panning a large constant bias causes), automatic indoor disable
// (interiors fall back to the game's own shadows), coverage up to 30000 units with the light
// volume scaled to match, an 8192 map option, and the screen-space shadow parameters exposed.
//
// The optional screen-space shadow term is Bend Studio's Days Gone technique (Apache-2.0):
// a wavefront-cooperative screen-space trace computed by a compute pass into a visibility
// texture the composite combines in. CPU dispatch construction is src/bend_sss_cpu.h
// (verbatim from Bend); the GPU half is ported to WGSL in res/bend_sss.wgsl. Because its
// thickness threshold is depth-relative it resolves fine detail at any distance, letting a
// tighter shadow-map radius pair with screen-space detail everywhere.

#include "global.h"

#include "bend_sss_cpu.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DU/J3DUClipper.h"
#include "JSystem/JMath/JMath.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_rain.h"
#include "dolphin/gx/GXAurora.h"
#include "dolphin/gx/GXBump.h"
#include "dolphin/gx/GXCull.h"
#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXLighting.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTev.h"
#include "dolphin/gx/GXTransform.h"
#include "m_Do/m_Do_mtx.h"
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/game.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GameService, svc_game);
IMPORT_SERVICE(LogService, svc_log);

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarShadowMap = 0;
ConfigVarHandle g_cvarMapSize = 0;
ConfigVarHandle g_cvarNormalSmooth = 0;
ConfigVarHandle g_cvarNoFrustumClipping = 0;
ConfigVarHandle g_cvarStrength = 0;
ConfigVarHandle g_cvarPcf = 0;
ConfigVarHandle g_cvarBias = 0;
ConfigVarHandle g_cvarBoxRadius = 0;
ConfigVarHandle g_cvarContactShadows = 0;
ConfigVarHandle g_cvarDebugView = 0;
ConfigVarHandle g_cvarSlopeBias = 0;
ConfigVarHandle g_cvarNormalOffset = 0;
ConfigVarHandle g_cvarSssThickness = 0;
ConfigVarHandle g_cvarSssEdgeThreshold = 0;
ConfigVarHandle g_cvarSssContrast = 0;
ConfigVarHandle g_cvarSssBias = 0;
ConfigVarHandle g_cvarSssLength = 0;
ConfigVarHandle g_cvarSssIgnoreEdges = 0;
ConfigVarHandle g_cvarSssFade = 0;
ConfigVarHandle g_cvarSssFadeStart = 0;
ConfigVarHandle g_cvarSssFadeEnd = 0;
ConfigVarHandle g_cvarIndoorDisable = 0;
ConfigVarHandle g_cvarTwoSidedCasters = 0;

GfxDrawTypeHandle g_drawType = 0;
GfxComputeTypeHandle g_sssComputeType = 0;
GfxComputeTypeHandle g_normalComputeType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterTerrainHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
UiWindowHandle g_controlsWindow = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_sssShaderSource = RESOURCE_BUFFER_INIT;
ResourceBuffer g_normalShaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_compositePipeline = nullptr;       // multiply blend
WGPURenderPipeline g_compositeDebugPipeline = nullptr;  // no blend (debug views)
WGPUBindGroupLayout g_compositeLayout = nullptr;
WGPUBindGroupLayout g_compositeDebugLayout = nullptr;
WGPUComputePipeline g_sssPipeline = nullptr;  // Bend screen-space shadow trace
WGPUBindGroupLayout g_sssLayout = nullptr;
WGPUComputePipeline g_normalGenPipeline = nullptr;  // smoothed-normal buffer (normal_smooth.wgsl)
WGPUComputePipeline g_normalBlurHPipeline = nullptr;
WGPUComputePipeline g_normalBlurVPipeline = nullptr;
WGPUBindGroupLayout g_normalGenLayout = nullptr;
WGPUBindGroupLayout g_normalBlurHLayout = nullptr;
WGPUBindGroupLayout g_normalBlurVLayout = nullptr;

// Bend SSS output target (screen-sized visibility, 1 = lit), recreated when the render size
// changes. Old targets are retired for a few frames instead of released immediately: payloads
// embedding their views may still be in flight on the render worker.
struct SssTarget {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
};
SssTarget g_sssTarget;
struct RetiredSssTarget {
    SssTarget target;
    int framesLeft = 0;
};
std::vector<RetiredSssTarget> g_retiredSssTargets;

// Smoothed-normal ping-pong buffers (rgba32float: normal.xyz + raw depth) at full render
// resolution. The blur radius scales with render height (kNormalReferenceHeight) so a given
// Normal Smoothing setting looks identical at any internal resolution. Same retire scheme.
constexpr float kNormalReferenceHeight = 1080.0f;
struct NormalTargets {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTexture textures[2] = {};
    WGPUTextureView views[2] = {};
};
NormalTargets g_normalTargets;
struct RetiredNormalTargets {
    NormalTargets targets;
    int framesLeft = 0;
};
std::vector<RetiredNormalTargets> g_retiredNormalTargets;

struct MapPassOutput {
    bool ready = false;
    WGPUTextureView shadowMap = nullptr;   // frame-pooled
    WGPUTextureView lightColor = nullptr;  // frame-pooled
    uint32_t mapSize = 0;
    Mtx44 lightVp;             // world -> light receiver projection, row-major game convention
    float dirToLightWorld[3];  // toward the light, normalized
    float fade = 0.0f;
    float lightNear = 100.0f;  // ortho depth range actually used this frame (bias normalization)
    float lightFar = 60000.0f;
    float texelWorld = 1.0f;   // world units per shadow-map texel (normal-offset scale)
};

MapPassOutput g_mapPass;
bool g_replayingSceneLists = false;
bool g_replayTwoSided = false;  // twoSidedCasters, latched for the current replay

constexpr float kLightDistance = 30000.0f;
constexpr float kLightNear = 100.0f;
constexpr float kLightFar = 60000.0f;
constexpr float kMaxLightLookahead = 10000.0f;
constexpr float kSunMoonDistance = 80000.0f;
constexpr float kSunMoonZDistance = -48000.0f;

using ClipperSphereClip = int (J3DUClipper::*)(f32 const (*)[4], Vec, f32) const;
using ClipperBoxClip = int (J3DUClipper::*)(f32 const (*)[4], Vec*, Vec*) const;
constexpr ClipperSphereClip kClipperSphereClip = static_cast<ClipperSphereClip>(&J3DUClipper::clip);
constexpr ClipperBoxClip kClipperBoxClip = static_cast<ClipperBoxClip>(&J3DUClipper::clip);

// Mirror of the WGSL Uniforms struct (keep in sync with res/shadow.wgsl).
struct ShadowUniforms {
    float world_from_proj[16];
    float light_vp[16];
    float size[2];
    float inv_size[2];
    float bias;
    float strength;
    float pcf_taps;
    float contact_enabled;
    uint32_t debug_mode;
    float slope_bias;          // extra bias per unit of surface slope (normalized depth units)
    float normal_offset;       // receiver offset along the surface normal, in shadow texels
    float texel_world;         // world units per shadow-map texel
    float light_dir_world[3];  // toward the light, world space (slope/offset receivers)
    float map_enabled;         // 0 = screen-space-only mode (map bindings are stand-ins)
    float smoothed_normals;    // 1 = the smoothed-normal buffer is bound
    float camera_eye[3];       // camera world position (screen-space shadow distance fade)
    float sss_fade_start;      // world units; screen-space shadow full below this distance
    float sss_fade_end;        // world units; screen-space shadow gone beyond this distance
    float _pad0;
    float _pad1;
};
static_assert(sizeof(ShadowUniforms) % 16 == 0);

// Mirror of the WGSL GenUniforms struct (keep in sync with res/normal_smooth.wgsl).
struct NormalGenUniforms {
    float world_from_proj[16];
};
static_assert(sizeof(NormalGenUniforms) % 16 == 0);

// Mirror of the WGSL BlurUniforms struct (keep in sync with res/normal_smooth.wgsl).
struct NormalBlurUniforms {
    float sigma;
    float radius;
    float _pad0;
    float _pad1;
};
static_assert(sizeof(NormalBlurUniforms) % 16 == 0);

// Mirror of the WGSL SssUniforms struct (keep in sync with res/bend_sss.wgsl). One slot is
// pushed per Bend dispatch: the light coordinate and tuning are shared, the wave offset is
// per-dispatch.
struct SssUniforms {
    float light_coordinate[4];
    int32_t wave_offset[2];
    float surface_thickness;
    float bilinear_threshold;
    float shadow_contrast;
    uint32_t ignore_edge_pixels;
    uint32_t debug_mode;
    float receiver_bias;
    float range_falloff;
    float _pad0;
    float _pad1;
    float _pad2;
};
static_assert(sizeof(SssUniforms) % 16 == 0);

struct DrawPayload {
    WGPUTextureView sceneDepth;    // frame-pooled
    WGPUTextureView shadowMap;     // frame-pooled
    WGPUTextureView lightColor;    // frame-pooled
    WGPUTextureView screenShadow;  // Bend SSS output (or the depth view when disabled)
    WGPUTextureView smoothNormal;  // smoothed-normal buffer (or the depth view when disabled)
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_mode;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

struct NormalComputePayload {
    WGPUTextureView sceneDepth;  // frame-pooled
    WGPUTextureView normalA;     // mod-owned ping-pong (gen writes A, blur H A->B, blur V B->A)
    WGPUTextureView normalB;
    uint32_t genUniformOffset;
    uint32_t genUniformSize;
    uint32_t blurUniformOffset;
    uint32_t blurUniformSize;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(NormalComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<NormalComputePayload>);

struct SssComputePayload {
    WGPUTextureView sceneDepth;  // frame-pooled
    WGPUTextureView shadowOut;   // mod-owned SSS target
    uint32_t uniformSize;
    uint32_t dispatchCount;         // <= 8 (Bend::DispatchList capacity)
    uint32_t uniformOffsets[8];     // one uniform slot per dispatch
    uint32_t workgroupsYZ[8][2];    // dispatch X is always the 64-group wavefront dimension
};
static_assert(sizeof(SssComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<SssComputePayload>);

struct LightCamera {
    Mtx view;
    Mtx44 ortho;
    Mtx44 vp;
    float dirToLight[3];
    float fade = 0.0f;
    float lightNear = 100.0f;
    float lightFar = 60000.0f;
};

struct SceneCamera {
    bool valid = false;
    bool raw_valid = false;
    CameraInfo info = CAMERA_INFO_INIT;
    Mtx raw_view;
    f32 raw_projection[7]{};
    Mtx44 raw_projection_mtx;
};

SceneCamera g_sceneCamera;

struct ActualLightDebugState {
    bool active = false;
    Mtx savedView;
    f32 savedProjection[7];
    f32 savedViewport[6];
    u32 savedScissor[4];
};

ActualLightDebugState g_actualLightDebug;

struct replay_scope {
    replay_scope() { g_replayingSceneLists = true; }
    ~replay_scope() { g_replayingSceneLists = false; }
};

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

int64_t get_debug_mode() {
    return std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 13);
}

bool matrix_ready(const Mtx m) {
    float basis = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(m[r][c])) {
                return false;
            }
            if (c < 3) {
                basis += std::fabs(m[r][c]);
            }
        }
    }
    return basis > 0.001f;
}

bool projection_vector_ready(const f32 projection[7]) {
    if (projection[0] != 0.0f) {
        return false;
    }
    for (int i = 1; i < 7; ++i) {
        if (!std::isfinite(projection[i])) {
            return false;
        }
    }
    return std::fabs(projection[1]) > 0.001f && std::fabs(projection[3]) > 0.001f &&
           std::fabs(projection[6]) > 0.001f;
}

// Row-major game matrix -> column-major WGSL layout (matching CameraService).
void store_column_major(const Mtx44 in, float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] = in[r][c];
        }
    }
}

void copy_projection(const Mtx44 in, Mtx44 out) {
    std::memcpy(out, in, sizeof(Mtx44));
    // TODO: check GfxDeviceInfo.uses_reversed_z
    for (int c = 0; c < 4; ++c) {
        out[2][c] = -out[2][c];
    }
}

void projection_vector_from_perspective(const Mtx44 projection, f32 out[7]) {
    out[0] = 0.0f;
    out[1] = projection[0][0];
    out[2] = projection[0][2];
    out[3] = projection[1][1];
    out[4] = projection[1][2];
    out[5] = projection[2][2];
    out[6] = projection[2][3];
}

const view_class* stage_game_view(const GfxStageContext* stageCtx) {
    if (stageCtx == nullptr || stageCtx->struct_size < sizeof(GfxStageContext) ||
        stageCtx->game_view == nullptr)
    {
        return nullptr;
    }
    return static_cast<const view_class*>(stageCtx->game_view);
}

bool capture_raw_camera(
    const view_class* gameView, Mtx outView, Mtx44 outProjectionMtx, f32 outProjection[7]) {
    if (gameView == nullptr || !matrix_ready(gameView->viewMtx)) {
        return false;
    }
    std::memcpy(outProjectionMtx, gameView->projMtx, sizeof(Mtx44));
    projection_vector_from_perspective(outProjectionMtx, outProjection);
    if (!projection_vector_ready(outProjection)) {
        return false;
    }
    cMtx_copy(gameView->viewMtx, outView);
    return true;
}

bool capture_scene_camera(const GfxStageContext* stageCtx) {
    g_sceneCamera.valid = false;
    g_sceneCamera.raw_valid = false;
    const view_class* gameView = stage_game_view(stageCtx);
    if (gameView == nullptr) {
        return false;
    }
    CameraInfo info = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &info) != MOD_OK) {
        return false;
    }
    g_sceneCamera.info = info;
    g_sceneCamera.valid = true;
    g_sceneCamera.raw_valid = capture_raw_camera(gameView, g_sceneCamera.raw_view,
        g_sceneCamera.raw_projection_mtx, g_sceneCamera.raw_projection);
    return true;
}

bool get_replay_camera(Mtx outView, Mtx44 outProjectionMtx, f32 outProjection[7]) {
    if (g_sceneCamera.raw_valid && matrix_ready(g_sceneCamera.raw_view)) {
        cMtx_copy(g_sceneCamera.raw_view, outView);
        std::memcpy(outProjectionMtx, g_sceneCamera.raw_projection_mtx,
            sizeof(g_sceneCamera.raw_projection_mtx));
        std::memcpy(
            outProjection, g_sceneCamera.raw_projection, sizeof(g_sceneCamera.raw_projection));
        return projection_vector_ready(outProjection);
    }

    return false;
}

float wrap_daytime(float daytime) {
    if (!std::isfinite(daytime)) {
        return 180.0f;
    }
    float wrapped = std::fmod(daytime, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped;
}

float daytime_percent(float max, float min, float value) {
    const float range = max - min;
    if (range == 0.0f) {
        return 1.0f;
    }
    const float percent = 1.0f - ((max - value) / range);
    return percent < 1.0f ? percent : 1.0f;
}

float sun_moon_angle(float daytime) {
    daytime = wrap_daytime(daytime);
    if (daytime >= 90.0f && daytime <= 270.0f) {
        return daytime_percent(270.0f, 90.0f, daytime) * 150.0f + 105.0f;
    }

    float angle = daytime;
    if (angle < 90.0f) {
        angle += 360.0f;
    }

    angle = daytime_percent(450.0f, 270.0f, angle) * 210.0f + 255.0f;
    if (angle > 360.0f) {
        angle -= 360.0f;
    }
    return angle;
}

cXyz sun_moon_offset(float daytime) {
    const float angle = DEG_TO_RAD(sun_moon_angle(daytime));
    const float angleSin = sinf(angle);
    const float angleCos = cosf(angle);
    return cXyz{
        angleSin * kSunMoonDistance, -angleCos * kSunMoonDistance, angleCos * kSunMoonZDistance};
}

bool build_composite_pipeline(
    bool blend, WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"sun shadow composite", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }

    // Multiply blend: fragment output is the darkening multiplier (result = dst * src).
    WGPUBlendState blendState{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Dst,
            .dstFactor = WGPUBlendFactor_Zero},
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
    pipelineDesc.label = {blend ? "shadow composite" : "shadow composite (debug)", WGPU_STRLEN};
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

bool build_compute_pipeline(const char* label, const ResourceBuffer& source, const char* entry,
    WGPUComputePipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {label, WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
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

bool build_sss_pipeline() {
    return build_compute_pipeline(
        "bend screen-space shadows", g_sssShaderSource, "cs_main", g_sssPipeline, g_sssLayout);
}

bool build_normal_pipelines() {
    return build_compute_pipeline("smoothed normals gen", g_normalShaderSource, "normal_gen",
               g_normalGenPipeline, g_normalGenLayout) &&
           build_compute_pipeline("smoothed normals blur h", g_normalShaderSource,
               "normal_blur_h", g_normalBlurHPipeline, g_normalBlurHLayout) &&
           build_compute_pipeline("smoothed normals blur v", g_normalShaderSource,
               "normal_blur_v", g_normalBlurVPipeline, g_normalBlurVLayout);
}

void release_sss_target(SssTarget& target) {
    if (target.view != nullptr) {
        wgpuTextureViewRelease(target.view);
        target.view = nullptr;
    }
    if (target.texture != nullptr) {
        wgpuTextureRelease(target.texture);
        target.texture = nullptr;
    }
    target.width = target.height = 0;
}

void tick_retired_sss_targets() {
    for (auto it = g_retiredSssTargets.begin(); it != g_retiredSssTargets.end();) {
        if (--it->framesLeft <= 0) {
            release_sss_target(it->target);
            it = g_retiredSssTargets.erase(it);
        } else {
            ++it;
        }
    }
}

bool ensure_sss_target(uint32_t width, uint32_t height) {
    if (g_sssTarget.width == width && g_sssTarget.height == height) {
        return true;
    }
    if (g_sssTarget.width != 0) {
        g_retiredSssTargets.push_back(RetiredSssTarget{std::exchange(g_sssTarget, SssTarget{}), 4});
    }
    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.label = {"bend screen-space shadow", WGPU_STRLEN};
    texDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    texDesc.size = {width, height, 1};
    texDesc.format = WGPUTextureFormat_R32Float;
    g_sssTarget.texture = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
    if (g_sssTarget.texture != nullptr) {
        g_sssTarget.view = wgpuTextureCreateView(g_sssTarget.texture, nullptr);
    }
    if (g_sssTarget.view == nullptr) {
        release_sss_target(g_sssTarget);
        return false;
    }
    g_sssTarget.width = width;
    g_sssTarget.height = height;
    return true;
}

void release_normal_targets(NormalTargets& targets) {
    for (auto*& view : targets.views) {
        if (view != nullptr) {
            wgpuTextureViewRelease(view);
            view = nullptr;
        }
    }
    for (auto*& texture : targets.textures) {
        if (texture != nullptr) {
            wgpuTextureRelease(texture);
            texture = nullptr;
        }
    }
    targets.width = targets.height = 0;
}

void tick_retired_normal_targets() {
    for (auto it = g_retiredNormalTargets.begin(); it != g_retiredNormalTargets.end();) {
        if (--it->framesLeft <= 0) {
            release_normal_targets(it->targets);
            it = g_retiredNormalTargets.erase(it);
        } else {
            ++it;
        }
    }
}

bool ensure_normal_targets(uint32_t width, uint32_t height) {
    if (g_normalTargets.width == width && g_normalTargets.height == height) {
        return true;
    }
    if (g_normalTargets.width != 0) {
        g_retiredNormalTargets.push_back(
            RetiredNormalTargets{std::exchange(g_normalTargets, NormalTargets{}), 4});
    }
    for (int i = 0; i < 2; ++i) {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.label = {"smoothed normals", WGPU_STRLEN};
        texDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        texDesc.size = {width, height, 1};
        texDesc.format = WGPUTextureFormat_RGBA32Float;
        g_normalTargets.textures[i] = wgpuDeviceCreateTexture(g_deviceInfo.device, &texDesc);
        if (g_normalTargets.textures[i] != nullptr) {
            g_normalTargets.views[i] = wgpuTextureCreateView(g_normalTargets.textures[i], nullptr);
        }
        if (g_normalTargets.views[i] == nullptr) {
            release_normal_targets(g_normalTargets);
            return false;
        }
    }
    g_normalTargets.width = width;
    g_normalTargets.height = height;
    return true;
}

constexpr uint32_t div_ceil(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

// Render worker thread: build the smoothed-normal buffer - gen reconstructs per-pixel normals
// into A, then one separable depth-aware Gaussian (H: A->B, V: B->A) whose radius came from
// the host, so A holds the final smoothed normals.
void on_normal_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(NormalComputePayload)) {
        return;
    }
    NormalComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.sceneDepth == nullptr || data.normalA == nullptr || data.normalB == nullptr ||
        g_normalGenPipeline == nullptr)
    {
        return;
    }

    const auto makeGroup = [&](WGPUBindGroupLayout layout, WGPUTextureView in, WGPUTextureView out,
                               uint32_t uniformOffset, uint32_t uniformSize) {
        WGPUBindGroupEntry entries[3] = {
            WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].textureView = in;
        entries[1].binding = 1;
        entries[1].textureView = out;
        entries[2].binding = 2;
        entries[2].buffer = ctx->uniform_buffer;
        entries[2].offset = uniformOffset;
        entries[2].size = uniformSize;
        WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindGroupDesc.layout = layout;
        bindGroupDesc.entryCount = 3;
        bindGroupDesc.entries = entries;
        return wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    };

    WGPUBindGroup genGroup = makeGroup(g_normalGenLayout, data.sceneDepth, data.normalA,
        data.genUniformOffset, data.genUniformSize);
    WGPUBindGroup blurH = makeGroup(g_normalBlurHLayout, data.normalA, data.normalB,
        data.blurUniformOffset, data.blurUniformSize);
    WGPUBindGroup blurV = makeGroup(g_normalBlurVLayout, data.normalB, data.normalA,
        data.blurUniformOffset, data.blurUniformSize);
    if (genGroup != nullptr && blurH != nullptr && blurV != nullptr) {
        WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        passDesc.label = {"smoothed normals", WGPU_STRLEN};
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
        const uint32_t groupsX = div_ceil(data.width, 8);
        const uint32_t groupsY = div_ceil(data.height, 8);
        wgpuComputePassEncoderSetPipeline(pass, g_normalGenPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, genGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groupsX, groupsY, 1);
        wgpuComputePassEncoderSetPipeline(pass, g_normalBlurHPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, blurH, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groupsX, groupsY, 1);
        wgpuComputePassEncoderSetPipeline(pass, g_normalBlurVPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, blurV, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groupsX, groupsY, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    }
    for (WGPUBindGroup group : {genGroup, blurH, blurV}) {
        if (group != nullptr) {
            wgpuBindGroupRelease(group);
        }
    }
}

// Render worker thread: the Bend screen-space shadow trace, one dispatch per quadrant
// rectangle from BuildDispatchList (all sharing the depth snapshot and output).
void on_sss_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(SssComputePayload)) {
        return;
    }
    SssComputePayload data;
    std::memcpy(&data, payload, sizeof(data));
    const uint32_t count = std::min<uint32_t>(data.dispatchCount, 8u);
    if (data.sceneDepth == nullptr || data.shadowOut == nullptr || g_sssPipeline == nullptr ||
        count == 0)
    {
        return;
    }

    WGPUBindGroup groups[8] = {};
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i) {
        WGPUBindGroupEntry entries[3] = {
            WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].textureView = data.sceneDepth;
        entries[1].binding = 1;
        entries[1].textureView = data.shadowOut;
        entries[2].binding = 2;
        entries[2].buffer = ctx->uniform_buffer;
        entries[2].offset = data.uniformOffsets[i];
        entries[2].size = data.uniformSize;
        WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindGroupDesc.layout = g_sssLayout;
        bindGroupDesc.entryCount = 3;
        bindGroupDesc.entries = entries;
        groups[i] = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
        ok = ok && groups[i] != nullptr;
    }
    if (ok) {
        WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        passDesc.label = {"bend screen-space shadows", WGPU_STRLEN};
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &passDesc);
        wgpuComputePassEncoderSetPipeline(pass, g_sssPipeline);
        for (uint32_t i = 0; i < count; ++i) {
            wgpuComputePassEncoderSetBindGroup(pass, 0, groups[i], 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, 64, data.workgroupsYZ[i][0], data.workgroupsYZ[i][1]);
        }
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (groups[i] != nullptr) {
            wgpuBindGroupRelease(groups[i]);
        }
    }
}

// Render worker thread: fullscreen deferred-shadow composite.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload)) {
        return;
    }
    DrawPayload data;
    std::memcpy(&data, payload, sizeof(data));

    WGPURenderPipeline pipeline =
        data.debug_mode != 0 ? g_compositeDebugPipeline : g_compositePipeline;
    WGPUBindGroupLayout layout = data.debug_mode != 0 ? g_compositeDebugLayout : g_compositeLayout;
    if (data.sceneDepth == nullptr || data.shadowMap == nullptr || data.lightColor == nullptr ||
        data.screenShadow == nullptr || data.smoothNormal == nullptr || pipeline == nullptr)
    {
        return;
    }

    WGPUBindGroupEntry entries[6] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = 1;
    entries[1].textureView = data.shadowMap;
    entries[2].binding = 2;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniform_offset;
    entries[2].size = data.uniform_size;
    entries[3].binding = 3;
    entries[3].textureView = data.lightColor;
    entries[4].binding = 4;
    entries[4].textureView = data.screenShadow;
    entries[5].binding = 5;
    entries[5].textureView = data.smoothNormal;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = 6;
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

// Picks the sun or moon (whichever is above the horizon) and returns the normalized
// world-space direction *toward* the light plus a horizon fade factor. False = no light.
bool compute_light(float outDirToLight[3], float& outFade) {
    dScnKy_env_light_c* envLight = dKy_getEnvlight();
    if (envLight == nullptr) {
        return false;
    }

    // The packet positions can be stale when this runs before the world lists are consumed.
    // Mirror dScnKy_env_light_c::setSunpos() so --time-of-day directly moves the debug light.
    const float daytime = wrap_daytime(dComIfGs_getTime());
    cXyz offset = sun_moon_offset(daytime);
    if (offset.y <= 0.0f) {
        offset = sun_moon_offset(daytime + 180.0f);
    }
    const float length = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    if (length < 1.0f) {
        return false;
    }
    outDirToLight[0] = offset.x / length;
    outDirToLight[1] = offset.y / length;
    outDirToLight[2] = offset.z / length;
    // Fade shadows out as the light approaches the horizon (elevation below ~11 degrees).
    outFade = std::clamp((outDirToLight[1] - 0.05f) / 0.15f, 0.0f, 1.0f);
    return outFade > 0.0f;
}

bool build_light_camera(const Mtx cameraView, uint32_t mapSize, float radius, LightCamera& out) {
    Mtx cameraInvView;
    cMtx_inverse(cameraView, cameraInvView);
    if (!matrix_ready(cameraInvView)) {
        return false;
    }
    if (!compute_light(out.dirToLight, out.fade)) {
        return false;
    }

    // Fit a fixed-radius ortho box around the visible play space. The camera target alone can sit
    // behind the receiver field, while a far-horizon center drops foreground receivers.
    const cXyz eye{cameraInvView[0][3], cameraInvView[1][3], cameraInvView[2][3]};
    cXyz forward{-cameraInvView[0][2], -cameraInvView[1][2], -cameraInvView[2][2]};
    const float forwardLength =
        std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    if (forwardLength > 0.001f) {
        forward = forward / forwardLength;
    } else {
        forward = cXyz{0.0f, 0.0f, -1.0f};
    }
    const float lookahead = std::min(radius * 0.75f, kMaxLightLookahead);
    const cXyz center = eye + forward * lookahead;
    // Scale the light volume with the coverage radius so large radii stay inside the ortho depth
    // range (terrain headroom included); a fixed distance/far pair clips casters at wide coverage.
    const float lightDistance = std::max(kLightDistance, radius * 1.5f + 10000.0f);
    out.lightNear = kLightNear;
    out.lightFar = lightDistance + radius + 20000.0f;
    const cXyz lightEye{center.x + out.dirToLight[0] * lightDistance,
        center.y + out.dirToLight[1] * lightDistance,
        center.z + out.dirToLight[2] * lightDistance};
    const bool nearlyVertical = std::fabs(out.dirToLight[1]) > 0.99f;
    cXyz up = nearlyVertical ? cXyz{0.0f, 0.0f, 1.0f} : cXyz{0.0f, 1.0f, 0.0f};

    cMtx_lookAt(out.view, &lightEye, &center, &up, 0);
    const float unitsPerTexel = (2.0f * radius) / static_cast<float>(mapSize);
    out.view[0][3] = std::round(out.view[0][3] / unitsPerTexel) * unitsPerTexel;
    out.view[1][3] = std::round(out.view[1][3] / unitsPerTexel) * unitsPerTexel;

    C_MTXOrtho(out.ortho, radius, -radius, -radius, radius, out.lightNear, out.lightFar);
    cMtx_concatProjView(out.ortho, out.view, out.vp);
    return true;
}

bool build_light_replay_projection(
    const LightCamera& lightCamera, const Mtx cameraView, Mtx44 out) {
    Mtx cameraInvView;
    cMtx_inverse(cameraView, cameraInvView);
    if (!matrix_ready(cameraInvView)) {
        return false;
    }

    Mtx lightFromCamera;
    cMtx_concat(lightCamera.view, cameraInvView, lightFromCamera);
    cMtx_concatProjView(lightCamera.ortho, lightFromCamera, out);
    return true;
}

// True when the dynamic shadow pass will run this frame: enabled, a camera exists, and the
// sun or moon is above the horizon. Also gates the game-shadow skip hooks, which fire earlier
// in the painter than our SCENE_AFTER_TERRAIN hook.
// Interiors read as fully shadowed under a scene-wide sun map (no sky visibility), so the
// effect auto-disables there; the hooks then also let the game's own shadows come back.
bool indoor_blocked() {
    return get_bool_option(g_cvarIndoorDisable, true) && dKy_Indoor_check() != 0;
}

bool dynamic_shadows_wanted() {
    // Both gates matter: with the shadow map disabled (screen-space-only mode) the game's
    // own real/blob shadows must come back, so the skip hooks go inactive.
    if (!get_bool_option(g_cvarEnabled, true) || !get_bool_option(g_cvarShadowMap, true) ||
        indoor_blocked())
    {
        return false;
    }
    if (!g_sceneCamera.raw_valid) {
        return false;
    }
    float dirToLight[3];
    float fade = 0.0f;
    return compute_light(dirToLight, fade);
}

HookAction on_game_shadow_pre(ModContext*, void*, void*, void*) {
    if (!dynamic_shadows_wanted()) {
        return HOOK_CONTINUE;
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction on_frustum_clip_pre(ModContext*, void*, void* retval, void*) {
    if (!get_bool_option(g_cvarNoFrustumClipping, false) || !dynamic_shadows_wanted()) {
        return HOOK_CONTINUE;
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 0;
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction on_copy_tex_pre(ModContext*, void*, void*, void*) {
    return g_replayingSceneLists ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// Two-sided casters: single-sided geometry (level-edge walls, roofs, terrain skirts) faces the
// player, so from the light's viewpoint it is back-facing and gets culled out of the shadow map,
// punching light-leak holes wherever no second surface stands behind it. During the replay every
// caster renders with culling off; depth is still the nearest-to-light surface, so closed meshes
// are unaffected.
//
// Direct GX drawers pick their cull mode up from this call, so rewriting the argument covers them.
HookAction on_cull_mode_pre(ModContext*, void* args, void*, void*) {
    if (g_replayingSceneLists && g_replayTwoSided) {
        dusk::mods::arg_ref<GXCullMode>(args, 0) = GX_CULL_NONE;
    }
    return HOOK_CONTINUE;
}

// J3D materials don't call GXSetCullMode: their cull mode is baked into the genMode BP write of
// the material display list (J3DGDSetGenMode), replayed through the command processor each draw.
// Re-issue genMode through the GX shim between the material load and the shape's geometry lists
// (the shim's deferred write flushes at the shape's first GXCallDisplayList): same stage counts
// as the material — the whole register is rewritten, and stale counts would break alpha-tested
// casters like foliage — but with culling forced off.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (!g_replayingSceneLists || !g_replayTwoSided) {
        return HOOK_CONTINUE;
    }
    const J3DShape* shape = dusk::mods::arg<const J3DShape*>(args, 0);
    J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
    if (material == nullptr || material->getColorBlock() == nullptr ||
        material->getIndBlock() == nullptr)
    {
        return HOOK_CONTINUE;
    }
    GXSetNumTexGens(static_cast<u8>(material->getTexGenNum()));
    GXSetNumChans(material->getColorBlock()->getColorChanNum());
    GXSetNumTevStages(material->getTevStageNum());
    GXSetNumIndStages(material->getIndBlock()->getIndTexStageNum());
    GXSetCullMode(GX_CULL_NONE);
    return HOOK_CONTINUE;
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

void render_shadow_map(
    const Mtx replayView, const Mtx44 replayProjectionMtx, const f32 replayProjection[7]);

void restore_actual_light_debug() {
    if (!g_actualLightDebug.active) {
        return;
    }

    j3dSys.setViewMtx(g_actualLightDebug.savedView);
    GXSetProjectionv(g_actualLightDebug.savedProjection);
    GXSetViewport(g_actualLightDebug.savedViewport[0], g_actualLightDebug.savedViewport[1],
        g_actualLightDebug.savedViewport[2], g_actualLightDebug.savedViewport[3],
        g_actualLightDebug.savedViewport[4], g_actualLightDebug.savedViewport[5]);
    GXSetScissor(g_actualLightDebug.savedScissor[0], g_actualLightDebug.savedScissor[1],
        g_actualLightDebug.savedScissor[2], g_actualLightDebug.savedScissor[3]);
    dKy_setLight();
    J3DShape::resetVcdVatCache();

    g_actualLightDebug.active = false;
}

void on_scene_begin(ModContext*, const GfxStageContext* stageCtx, void*) {
    tick_retired_sss_targets();
    tick_retired_normal_targets();
    restore_actual_light_debug();
    capture_scene_camera(stageCtx);
    if (!get_bool_option(g_cvarEnabled, true) || get_debug_mode() != 9) {
        return;
    }

    Mtx cameraView;
    if (!g_sceneCamera.raw_valid || !matrix_ready(g_sceneCamera.raw_view)) {
        return;
    }
    cMtx_copy(g_sceneCamera.raw_view, cameraView);

    const uint32_t mapSize = 1024u << std::clamp<int64_t>(get_int_option(g_cvarMapSize, 2), 0, 3);
    const float radius =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarBoxRadius, 8000), 1000, 30000));
    LightCamera lightCamera{};
    if (!build_light_camera(cameraView, mapSize, radius, lightCamera)) {
        return;
    }
    Mtx44 lightProjection;
    if (!build_light_replay_projection(lightCamera, cameraView, lightProjection)) {
        return;
    }

    cMtx_copy(cameraView, g_actualLightDebug.savedView);
    GXGetProjectionv(g_actualLightDebug.savedProjection);
    GXGetViewportv(g_actualLightDebug.savedViewport);
    GXGetScissor(&g_actualLightDebug.savedScissor[0], &g_actualLightDebug.savedScissor[1],
        &g_actualLightDebug.savedScissor[2], &g_actualLightDebug.savedScissor[3]);
    g_actualLightDebug.active = true;

    j3dSys.setViewMtx(g_actualLightDebug.savedView);
    GXSetProjectionFull(lightProjection);
    dKy_setLight();
    J3DShape::resetVcdVatCache();
}

void on_scene_after_terrain(ModContext*, const GfxStageContext* stageCtx, void*) {
    if (g_mapPass.ready) {
        return;
    }

    const view_class* gameView = stage_game_view(stageCtx);
    Mtx replayView;
    Mtx44 replayProjectionMtx;
    f32 replayProjection[7];
    if (!capture_raw_camera(gameView, replayView, replayProjectionMtx, replayProjection)) {
        return;
    }
    render_shadow_map(replayView, replayProjectionMtx, replayProjection);
}

// Game thread, after the draw handlers have populated next frame's scene lists: replay opaque scene
// geometry from the light's point of view.
void render_shadow_map(
    const Mtx replayView, const Mtx44 replayProjectionMtx, const f32 replayProjection[7]) {
    if (g_mapPass.ready || !get_bool_option(g_cvarEnabled, true) ||
        !get_bool_option(g_cvarShadowMap, true) || indoor_blocked())
    {
        return;
    }
    const int64_t debugMode = get_debug_mode();
    if (debugMode == 9) {
        return;
    }
    if (!matrix_ready(replayView)) {
        return;
    }
    Mtx replayViewMtx;
    cMtx_copy(replayView, replayViewMtx);

    const uint32_t mapSize = 1024u << std::clamp<int64_t>(get_int_option(g_cvarMapSize, 2), 0, 3);
    const bool cameraReplayDebug = debugMode == 10;
    const float radius =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarBoxRadius, 8000), 1000, 30000));
    LightCamera lightCamera{};
    if (!build_light_camera(replayViewMtx, mapSize, radius, lightCamera)) {
        return;
    }
    Mtx44 lightReplayProjection;
    if (!build_light_replay_projection(lightCamera, replayViewMtx, lightReplayProjection)) {
        return;
    }
    f32 savedProjection[7];
    GXGetProjectionv(savedProjection);
    f32 savedViewport[6];
    GXGetViewportv(savedViewport);
    u32 savedScissor[4];
    GXGetScissor(&savedScissor[0], &savedScissor[1], &savedScissor[2], &savedScissor[3]);
    Mtx savedView;
    cMtx_copy(j3dSys.getViewMtx(), savedView);

    auto restore_game_camera = [&]() {
        j3dSys.setViewMtx(savedView);
        GXSetProjectionv(savedProjection);
        GXSetViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3],
            savedViewport[4], savedViewport[5]);
        GXSetScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
        dKy_setLight();
    };
    auto set_replay_camera = [&]() {
        j3dSys.setViewMtx(replayViewMtx);
        if (cameraReplayDebug) {
            GXSetProjectionv(replayProjection);
        } else {
            GXSetProjectionFull(lightReplayProjection);
        }
        dKy_setLight();
    };
    if (!draw_lists_ready()) {
        return;
    }
    if (svc_gfx->create_pass(mod_ctx, mapSize, mapSize) != MOD_OK) {
        return;
    }
    J3DShape::resetVcdVatCache();

    set_replay_camera();
    GXSetViewport(0.0f, 0.0f, static_cast<float>(mapSize), static_cast<float>(mapSize), 0.0f, 1.0f);
    GXSetViewportRender(
        0.0f, 0.0f, static_cast<float>(mapSize), static_cast<float>(mapSize), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, mapSize, mapSize);
    dKy_setLight();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    g_replayTwoSided = get_bool_option(g_cvarTwoSidedCasters, true);
    {
        replay_scope replay;
        draw_opaque_scene_lists();
    }
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restore_game_camera();

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr || resolved.depth == nullptr)
    {
        return;
    }

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restore_game_camera();

    g_mapPass.lightColor = resolved.color;
    g_mapPass.shadowMap = resolved.depth;
    g_mapPass.mapSize = mapSize;
    g_mapPass.lightNear = lightCamera.lightNear;
    g_mapPass.lightFar = lightCamera.lightFar;
    g_mapPass.texelWorld = (2.0f * radius) / static_cast<float>(mapSize);
    copy_projection(lightCamera.vp, g_mapPass.lightVp);
    std::memcpy(
        g_mapPass.dirToLightWorld, lightCamera.dirToLight, sizeof(g_mapPass.dirToLightWorld));
    g_mapPass.fade = lightCamera.fade;
    g_mapPass.ready = true;
}

// Game thread: build the Bend dispatch list for this frame's light and queue the SSS compute
// pass over the scene depth snapshot. Returns false when nothing was dispatched.
bool push_sss_dispatches(
    const GfxResolvedTargets& resolved, const float dirToLightWorld[3], int64_t debugMode) {
    if (g_sssPipeline == nullptr || !ensure_sss_target(resolved.width, resolved.height)) {
        return false;
    }

    // Homogeneous light coordinate: proj_from_world x (direction toward the light, 0) - a
    // directional light is a point at infinity in that direction (column-major, column-vector
    // convention, so w = 0 drops the fourth column).
    const CameraInfo& camera = g_sceneCamera.info;
    float lightProjection[4];
    for (int r = 0; r < 4; ++r) {
        lightProjection[r] = camera.proj_from_world[0 * 4 + r] * dirToLightWorld[0] +
                             camera.proj_from_world[1 * 4 + r] * dirToLightWorld[1] +
                             camera.proj_from_world[2 * 4 + r] * dirToLightWorld[2];
    }
    int viewport[2] = {static_cast<int>(resolved.width), static_cast<int>(resolved.height)};
    int minBounds[2] = {0, 0};
    int maxBounds[2] = {viewport[0], viewport[1]};
    Bend::DispatchList dispatchList =
        Bend::BuildDispatchList(lightProjection, viewport, minBounds, maxBounds);
    if (dispatchList.DispatchCount <= 0) {
        return false;
    }

    SssUniforms sssUniforms{};
    std::memcpy(sssUniforms.light_coordinate, dispatchList.LightCoordinate_Shader,
        sizeof(sssUniforms.light_coordinate));
    // Thickness/threshold settings are in hundredths of a percent of the remaining depth
    // range (Bend's recommended 0.5% / 2% = 50 / 200).
    sssUniforms.surface_thickness =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSssThickness, 50), 5, 500)) /
        10000.0f;
    sssUniforms.bilinear_threshold =
        static_cast<float>(
            std::clamp<int64_t>(get_int_option(g_cvarSssEdgeThreshold, 200), 25, 1000)) /
        10000.0f;
    sssUniforms.shadow_contrast =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSssContrast, 4), 1, 8));
    sssUniforms.ignore_edge_pixels = get_bool_option(g_cvarSssIgnoreEdges, false) ? 1u : 0u;
    sssUniforms.debug_mode = debugMode == 12 ? 1u : 0u;
    // Receiver bias in shadow-window units (slider 0-100 -> 0.0-1.0): blunt fallback that
    // lightens everything. Shadow length (slider 4-60 px) is the targeted facet-banding fix:
    // the falloff (1/length) forgives depth deltas in proportion to the caster's ray distance,
    // so near-contact micro-shadows keep full strength and facet-scale banding fades out. At
    // the max (60 = the full trace) the falloff matches Bend's own tail fade and adds nothing.
    sssUniforms.receiver_bias =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSssBias, 0), 0, 100)) /
        100.0f;
    const int64_t sssLength = std::clamp<int64_t>(get_int_option(g_cvarSssLength, 20), 4, 60);
    sssUniforms.range_falloff =
        sssLength >= 60 ? 0.0f : 1.0f / static_cast<float>(sssLength);

    SssComputePayload payload{};
    payload.sceneDepth = resolved.depth;
    payload.shadowOut = g_sssTarget.view;
    for (int i = 0; i < dispatchList.DispatchCount; ++i) {
        const Bend::DispatchData& dispatch = dispatchList.Dispatch[i];
        sssUniforms.wave_offset[0] = dispatch.WaveOffset_Shader[0];
        sssUniforms.wave_offset[1] = dispatch.WaveOffset_Shader[1];
        GfxRange range{0, 0};
        if (svc_gfx->push_uniform(mod_ctx, &sssUniforms, sizeof(sssUniforms), &range) != MOD_OK) {
            return false;
        }
        payload.uniformOffsets[i] = range.offset;
        payload.uniformSize = range.size;
        payload.workgroupsYZ[i][0] = static_cast<uint32_t>(dispatch.WaveCount[1]);
        payload.workgroupsYZ[i][1] = static_cast<uint32_t>(dispatch.WaveCount[2]);
    }
    payload.dispatchCount = static_cast<uint32_t>(dispatchList.DispatchCount);
    return svc_gfx->push_compute(mod_ctx, g_sssComputeType, &payload, sizeof(payload)) == MOD_OK;
}

// Game thread: build the full-resolution smoothed-normal buffer for this frame's composite.
// The blur radius scales with render height so `smoothing` covers the same fraction of the
// screen (and looks the same) at any internal resolution. Returns false when the buffer
// isn't available (composite falls back to the inline per-pixel cross).
bool push_normal_dispatches(const GfxResolvedTargets& resolved, int64_t smoothing) {
    if (g_normalGenPipeline == nullptr || resolved.height == 0 ||
        !ensure_normal_targets(resolved.width, resolved.height))
    {
        return false;
    }

    NormalGenUniforms genUniforms{};
    std::memcpy(genUniforms.world_from_proj, g_sceneCamera.info.world_from_proj,
        sizeof(genUniforms.world_from_proj));
    GfxRange genRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &genUniforms, sizeof(genUniforms), &genRange) != MOD_OK) {
        return false;
    }

    // Setting 1 ~ 1 texel of radius at the reference height; scaled to the real height and
    // capped to the shader's MAX_BLUR_RADIUS (32). sigma = radius/2 (a full Gaussian bell).
    const float radius = std::min(32.0f,
        static_cast<float>(smoothing) * static_cast<float>(resolved.height) / kNormalReferenceHeight);
    NormalBlurUniforms blurUniforms{};
    blurUniforms.radius = radius;
    blurUniforms.sigma = std::max(radius * 0.5f, 0.5f);
    GfxRange blurRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &blurUniforms, sizeof(blurUniforms), &blurRange) != MOD_OK) {
        return false;
    }

    NormalComputePayload payload{};
    payload.sceneDepth = resolved.depth;
    payload.normalA = g_normalTargets.views[0];
    payload.normalB = g_normalTargets.views[1];
    payload.genUniformOffset = genRange.offset;
    payload.genUniformSize = genRange.size;
    payload.blurUniformOffset = blurRange.offset;
    payload.blurUniformSize = blurRange.size;
    payload.width = resolved.width;
    payload.height = resolved.height;
    return svc_gfx->push_compute(mod_ctx, g_normalComputeType, &payload, sizeof(payload)) ==
           MOD_OK;
}

// Game thread: consume the frame's map pass and push the deferred composite draw at the
// current point in the command stream. With the shadow map disabled the composite still runs
// in screen-space-only mode: the Bend trace supplies the whole occlusion term and the game's
// own shadows return (dynamic_shadows_wanted is false, so the skip hooks pass through).
void composite_map_pass(int64_t debugMode) {
    const MapPassOutput mapPass = std::exchange(g_mapPass, {});
    // Indoors, the shadow map is suppressed (it reads as fully shadowed under a sky-light
    // map) but the screen-space shadows stay - so indoors is just screen-space-only mode.
    const bool mapWanted = get_bool_option(g_cvarShadowMap, true) && !indoor_blocked();
    const bool mapReady =
        mapWanted && mapPass.ready && mapPass.shadowMap != nullptr && mapPass.lightColor != nullptr;
    if (mapWanted && !mapReady) {
        // The map should have rendered but didn't (e.g. a lost frame): bail rather than
        // flash the screen-space-only look for one frame.
        return;
    }
    if (!g_sceneCamera.valid) {
        return;
    }
    const CameraInfo& camera = g_sceneCamera.info;

    float dirToLight[3];
    float fade = 0.0f;
    if (mapReady) {
        std::memcpy(dirToLight, mapPass.dirToLightWorld, sizeof(dirToLight));
        fade = mapPass.fade;
    } else {
        // Screen-space-only mode (map off, or auto-disabled indoors): the Bend trace supplies
        // the whole term and the game's own shadows return (dynamic_shadows_wanted is false).
        if (!get_bool_option(g_cvarEnabled, true) ||
            !get_bool_option(g_cvarContactShadows, true))
        {
            return;
        }
        if (!compute_light(dirToLight, fade)) {
            return;
        }
    }

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = false;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr)
    {
        return;
    }

    // The Bend screen-space shadow term runs as a compute pass ahead of this draw in the
    // same command stream; its output binds to the composite.
    const bool contactWanted = get_bool_option(g_cvarContactShadows, true) || debugMode == 11 ||
                               debugMode == 12;
    const bool sssReady = contactWanted && push_sss_dispatches(resolved, dirToLight, debugMode);
    if (!mapReady && !sssReady) {
        return;
    }

    // Smoothed receiver normals feed the slope-bias / normal-offset receivers (and the
    // Receiver Normal debug view); needed only when those are in play. 0 = off (the composite
    // then falls back to an inline per-pixel cross for whichever receiver is active).
    const int64_t smoothing = std::clamp<int64_t>(get_int_option(g_cvarNormalSmooth, 3), 0, 16);
    const bool normalsWanted =
        smoothing > 0 &&
        (debugMode == 13 ||
            (mapReady && (get_int_option(g_cvarSlopeBias, 30) > 0 ||
                             get_int_option(g_cvarNormalOffset, 100) > 0)));
    const bool normalsReady = normalsWanted && push_normal_dispatches(resolved, smoothing);

    ShadowUniforms uniforms{};
    std::memcpy(uniforms.world_from_proj, camera.world_from_proj, sizeof(uniforms.world_from_proj));
    if (mapReady) {
        store_column_major(mapPass.lightVp, uniforms.light_vp);
    }
    // Bias values are configured in world units along the light direction and normalized by the
    // ortho depth range actually used for this frame's map.
    const float lightRange = std::max(mapPass.lightFar - mapPass.lightNear, 1.0f);
    uniforms.bias =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarBias, 55), 0, 200)) /
        lightRange;
    uniforms.slope_bias =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSlopeBias, 30), 0, 200)) /
        lightRange;
    uniforms.normal_offset =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarNormalOffset, 100), 0, 300)) /
        100.0f;
    uniforms.texel_world = mapPass.texelWorld;
    std::memcpy(uniforms.light_dir_world, dirToLight, sizeof(uniforms.light_dir_world));
    uniforms.map_enabled = mapReady ? 1.0f : 0.0f;
    uniforms.smoothed_normals = normalsReady ? 1.0f : 0.0f;
    uniforms.size[0] = mapReady ? static_cast<float>(mapPass.mapSize) : 1.0f;
    uniforms.size[1] = uniforms.size[0];
    uniforms.inv_size[0] = 1.0f / uniforms.size[0];
    uniforms.inv_size[1] = 1.0f / uniforms.size[1];
    uniforms.strength =
        fade *
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarStrength, 45), 0, 100)) /
        100.0f;
    uniforms.pcf_taps = static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarPcf, 1), 0, 3));
    uniforms.contact_enabled = sssReady ? 1.0f : 0.0f;
    // Screen-space shadow distance fade: ease the SSS term out with world distance so distant,
    // fogged-out geometry isn't full-strength shadowed. Off -> push the band past the far
    // plane so the fade is always 1.
    std::memcpy(uniforms.camera_eye, camera.eye, sizeof(uniforms.camera_eye));
    if (get_bool_option(g_cvarSssFade, true)) {
        uniforms.sss_fade_start =
            static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSssFadeStart, 8000), 0, 200000));
        uniforms.sss_fade_end = static_cast<float>(
            std::clamp<int64_t>(get_int_option(g_cvarSssFadeEnd, 25000), 0, 300000));
    } else {
        uniforms.sss_fade_start = 1.0e12f;
        uniforms.sss_fade_end = 1.0e12f;
    }
    uniforms.debug_mode = static_cast<uint32_t>(debugMode);

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }
    // Every binding needs a texture view; the depth snapshot stands in for the map/light
    // views in screen-space-only mode and for the SSS view when the trace didn't run (the
    // shader never reads the stand-ins: map_enabled / contact_enabled gate them).
    const DrawPayload payload{resolved.depth, mapReady ? mapPass.shadowMap : resolved.depth,
        mapReady ? mapPass.lightColor : resolved.depth,
        sssReady ? g_sssTarget.view : resolved.depth,
        normalsReady ? g_normalTargets.views[0] : resolved.depth, uniformRange.offset,
        uniformRange.size, static_cast<uint32_t>(debugMode)};
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
}

// Game thread, after the opaque scene but before translucency and the game's bloom filter: the
// normal composite runs here so shadows darken the world underneath water, particles, and bloom
// (compositing after bloom visibly dims the glow over shadowed areas).
void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (get_debug_mode() != 0) {
        return;
    }
    composite_map_pass(0);
}

// Game thread, after the full 3D scene: debug views draw here, unobscured by everything the
// scene layers on after the opaque pass. Also the safety net that clears a map pass the
// after-opaque composite didn't consume.
void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    const int64_t debugMode = get_debug_mode();
    restore_actual_light_debug();
    if (debugMode == 0 || debugMode == 9) {
        g_mapPass = {};
        return;
    }
    composite_map_pass(debugMode);
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

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char** options,
    uint32_t optionCount, const char* help) {
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

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
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

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    // General: settings that affect both shadowing methods.
    svc_ui->pane_add_section(mod_ctx, left, "General");
    add_toggle(left, "Enabled", g_cvarEnabled, "Master toggle for realtime sun/moon shadows.");
    add_number(left, "Strength", g_cvarStrength, 0, 100, 5, "%",
        "How dark shadowed areas become. Applies to both the shadow map and the screen-space "
        "shadows.");

    // Shadow Map: the light-space depth map and everything that only affects it.
    svc_ui->pane_add_section(mod_ctx, left, "Shadow Map");
    add_toggle(left, "Shadow Map", g_cvarShadowMap,
        "Renders the sun/moon shadow map. Off: only the screen-space shadows run, and the "
        "game's own character shadows come back.");
    static const char* kMapSizes[] = {"1024", "2048", "4096", "8192"};
    add_select(left, "Map Size", g_cvarMapSize, kMapSizes, 4,
        "Shadow map resolution. Larger is sharper and slower.");
    add_number(left, "Coverage", g_cvarBoxRadius, 1000, 30000, 500, nullptr,
        "Radius of the shadowed area around the camera, in world units. Smaller is sharper for "
        "the same map size; the light volume scales automatically. Screen-space shadows fill in "
        "detail beyond the coverage, so keep this small.");
    static const char* kPcfOptions[] = {"Off", "3x3", "5x5", "7x7"};
    add_select(left, "Soft Shadows", g_cvarPcf, kPcfOptions, 4,
        "Shadow-map edge softening (percentage-closer filtering). Softens edges and hides "
        "stair-steps on steep terrain.");
    add_number(left, "Bias", g_cvarBias, 0, 200, 5, nullptr,
        "Constant depth bias in world units. Raise to remove shadow-map acne; lower to reduce "
        "peter-panning. Prefer Slope Bias and Normal Offset for acne on sloped surfaces - they "
        "only apply where needed, so the constant bias can stay small.");
    add_number(left, "Slope Bias", g_cvarSlopeBias, 0, 200, 5, nullptr,
        "Extra bias that grows with surface slope relative to the light, in world units. Sloped "
        "surfaces alias the most; this targets them without detaching flat-ground shadows.");
    add_number(left, "Normal Offset", g_cvarNormalOffset, 0, 300, 10, "%",
        "Shifts the shadow-map lookup point along the surface normal, scaled to the size of one "
        "shadow-map texel. The most effective acne fix with the least peter-panning; 100% = one "
        "texel.");
    add_number(left, "Normal Smoothing", g_cvarNormalSmooth, 0, 16, 1, nullptr,
        "Rounds the surface direction Slope Bias and Normal Offset rely on (like smooth "
        "shading), removing the faceted bias bands low-poly models can show. The blur radius "
        "scales with your render resolution, so one value looks the same at any internal "
        "resolution / supersampling factor. Only affects the shadow-map bias. Higher = "
        "smoother; 0 = off.");
    add_toggle(left, "No Frustum Clipping", g_cvarNoFrustumClipping,
        "Keeps camera-frustum-culled objects in the draw lists so off-screen objects still cast "
        "map shadows. Fixes shadows popping in and out with camera direction (distant mountains, "
        "ceilings) at some GPU cost.");
    add_toggle(left, "Two-Sided Casters", g_cvarTwoSidedCasters,
        "Renders map casters with backface culling disabled. Fixes light leaking through "
        "single-sided geometry (level-edge walls, roofs) that faces away from the sun.");
    add_toggle(left, "Disable Map Indoors", g_cvarIndoorDisable,
        "Turns the shadow MAP off in interior spaces (which read as fully shadowed under a "
        "sky-light map) and restores the game's own shadows there. Screen-space shadows still "
        "run indoors.");

    // Screen Space Shadows: the Bend depth-trace term and everything that only affects it.
    svc_ui->pane_add_section(mod_ctx, left, "Screen Space Shadows");
    add_toggle(left, "Screen Space Shadows", g_cvarContactShadows,
        "Bend Studio's Days Gone screen-space shadow trace: per-pixel detail the shadow map "
        "misses (contact darkening, thin geometry, fine detail) at any range - also re-grounds "
        "objects when bias settings push the mapped shadow away from their base. Runs even when "
        "the shadow map is off or disabled indoors.");
    add_number(left, "SSS Thickness", g_cvarSssThickness, 5, 500, 5, nullptr,
        "Assumed surface thickness for the screen-space trace, in hundredths of a percent of "
        "the remaining depth range (50 = 0.5%). Tune in steps of 2x: higher grounds more, too "
        "high over-thickens shadows.");
    add_number(left, "SSS Edge Threshold", g_cvarSssEdgeThreshold, 25, 1000, 25, nullptr,
        "Depth difference treated as a geometric edge instead of a slope, in hundredths of a "
        "percent (200 = 2%). Tune with the SSS Edge Mask debug view if striated patterns "
        "appear on flat surfaces.");
    add_number(left, "SSS Contrast", g_cvarSssContrast, 1, 8, 1, nullptr,
        "Contrast boost on the screen-space shadow transition. Higher is harder-edged.");
    add_number(left, "SSS Shadow Length", g_cvarSssLength, 4, 60, 2, nullptr,
        "Maximum screen-space shadow length, in render pixels, with a smooth falloff. This is "
        "the facet-banding fix: the banding on low-poly surfaces (Link's cap, hair) is cast "
        "polygon-by-polygon from tens of pixels away, while genuine micro-detail (the Hylian "
        "shield insignia, hands, straps) shadows its receiver within a few pixels of contact. "
        "Shorten until the cap is clean - micro-detail keeps full strength. 60 = the full "
        "unrestricted trace. Scales with resolution: raise it when supersampling.");
    add_number(left, "SSS Bias", g_cvarSssBias, 0, 100, 5, "%",
        "Blunt fallback: uniformly pushes the screen-space trace off the receiving surface, "
        "lightening ALL near-surface shadow detail equally. Prefer SSS Shadow Length for "
        "facet banding; keep this at 0 unless acne survives that the length cannot catch.");
    add_toggle(left, "SSS Ignore Edges", g_cvarSssIgnoreEdges,
        "Pixels detected as geometric edges do not cast screen-space shadows. Helps aliasing "
        "on large flat surfaces lit at grazing angles; can thin out foliage shadows.");
    add_toggle(left, "SSS Distance Fade", g_cvarSssFade,
        "Fades the screen-space shadows out with distance so distant, fogged-out geometry isn't "
        "shadowed at full strength. Off applies them at full strength to the horizon.");
    add_number(left, "SSS Fade Start", g_cvarSssFadeStart, 0, 60000, 1000, nullptr,
        "Distance from the camera (world units) where the screen-space shadow fade begins. Set "
        "near where the scene starts washing into fog.");
    add_number(left, "SSS Fade End", g_cvarSssFadeEnd, 0, 100000, 1000, nullptr,
        "Distance from the camera (world units) where the screen-space shadows are fully faded "
        "out. Set around the fog's full-density distance.");

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugOptions[] = {"Off", "Shadow Map", "Shadow Factor", "Occlusion",
        "Light UV", "Compare Sign", "Depth Values", "Receiver Range", "Bounds", "Light View",
        "Camera Replay", "Screen Shadows", "SSS Edge Mask", "Receiver Normal"};
    add_select(left, "Debug View", g_cvarDebugView, kDebugOptions, 14,
        "Shadow Map: light-space depth buffer<br/>Shadow Factor: final "
        "darkening term<br/>Occlusion: map comparison result<br/>Light UV: receiver "
        "projection coverage<br/>Compare Sign: current comparison in red and opposite "
        "comparison in blue<br/>Depth Values: receiver depth in red and map depth in green<br/>"
        "Receiver Range: beyond-far in red, valid depth in green, and before-near in blue<br/>"
        "Bounds: valid X in red, valid Y in green, and valid depth in blue<br/>Light View: "
        "renders the game world directly from the light camera<br/>Camera Replay: "
        "captures the same draw-list replay from the gameplay camera<br/>Screen Shadows: "
        "the Bend SSS visibility buffer (white = lit)<br/>SSS Edge Mask: the SSS edge "
        "detector, for tuning SSS Edge Threshold<br/>Receiver Normal: the smoothed surface "
        "direction Slope Bias / Normal Offset act on - facets should melt together as "
        "Normal Smoothing rises");
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
        svc_log->error(mod_ctx, "failed to open Realtime Sun Shadows controls window");
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
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register shadow option");
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
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register shadow option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "shadow.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return dusk::mods::set_error(error, result, "failed to load shadow.wgsl");
    }
    result = svc_resource->load(mod_ctx, "bend_sss.wgsl", &g_sssShaderSource);
    if (result != MOD_OK || g_sssShaderSource.data == nullptr) {
        return dusk::mods::set_error(error, result, "failed to load bend_sss.wgsl");
    }
    result = svc_resource->load(mod_ctx, "normal_smooth.wgsl", &g_normalShaderSource);
    if (result != MOD_OK || g_normalShaderSource.data == nullptr) {
        return dusk::mods::set_error(error, result, "failed to load normal_smooth.wgsl");
    }

    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("shadowMapEnabled", true, g_cvarShadowMap, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("mapSize", 2, g_cvarMapSize, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("normalSmooth", 3, g_cvarNormalSmooth, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("noFrustumClipping", true, g_cvarNoFrustumClipping, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("strength", 45, g_cvarStrength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("pcf", 2, g_cvarPcf, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("bias", 55, g_cvarBias, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("boxRadius", 8000, g_cvarBoxRadius, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("contactShadows", true, g_cvarContactShadows, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("debugView", 0, g_cvarDebugView, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("slopeBias", 30, g_cvarSlopeBias, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("normalOffset", 100, g_cvarNormalOffset, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssThickness", 50, g_cvarSssThickness, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssEdgeThreshold", 200, g_cvarSssEdgeThreshold, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssContrast", 4, g_cvarSssContrast, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssBias", 0, g_cvarSssBias, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssLength", 20, g_cvarSssLength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("sssIgnoreEdges", false, g_cvarSssIgnoreEdges, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("sssFade", true, g_cvarSssFade, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssFadeStart", 8000, g_cvarSssFadeStart, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("sssFadeEnd", 25000, g_cvarSssFadeEnd, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("indoorDisable", true, g_cvarIndoorDisable, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("twoSidedCasters", true, g_cvarTwoSidedCasters, error);
    if (result != MOD_OK) {
        return result;
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_composite_pipeline(true, g_compositePipeline, g_compositeLayout) ||
        !build_composite_pipeline(false, g_compositeDebugPipeline, g_compositeDebugLayout))
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to create composite pipeline");
    }
    if (!build_sss_pipeline()) {
        return dusk::mods::set_error(
            error, MOD_ERROR, "failed to create screen-space shadow pipeline");
    }
    if (!build_normal_pipelines()) {
        return dusk::mods::set_error(
            error, MOD_ERROR, "failed to create normal smoothing pipelines");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "sun shadow composite";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register draw type");
    }
    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "bend screen-space shadows";
    computeDesc.callback = on_sss_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_sssComputeType) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "smoothed normals";
    computeDesc.callback = on_normal_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_normalComputeType) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register compute type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_scene_after_terrain;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_TERRAIN, &stageDesc, &g_sceneAfterTerrainHook) != MOD_OK)
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

    // Skip the game's own shadow rendering while the dynamic pass is active: the
    // shadowControl pair covers the actor real/blob shadows, drawCloudShadow the weather
    // cloud shadows.
    if (dusk::mods::hook_add_pre<&dDlst_shadowControl_c::imageDraw>(svc_hook, on_game_shadow_pre) !=
            MOD_OK ||
        dusk::mods::hook_add_pre<&dDlst_shadowControl_c::draw>(svc_hook, on_game_shadow_pre) !=
            MOD_OK ||
        dusk::mods::hook_add_pre<&drawCloudShadow>(svc_hook, on_game_shadow_pre) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook game shadow rendering");
    }
    if (dusk::mods::hook_add_pre<kClipperSphereClip>(svc_hook, on_frustum_clip_pre) != MOD_OK ||
        dusk::mods::hook_add_pre<kClipperBoxClip>(svc_hook, on_frustum_clip_pre) != MOD_OK)
    {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook frustum clipping");
    }
    if (dusk::mods::hook_add_pre<GXCopyTex>(svc_hook, on_copy_tex_pre) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook GXCopyTex");
    }
    // Two-sided casters (see on_shape_draw_pre / on_cull_mode_pre). The J3DShape::drawFast hook
    // is virtual, so it resolves through the symbol manifest; if that's missing, degrade to
    // leaky shadows instead of failing the whole mod.
    if (dusk::mods::hook_add_pre<GXSetCullMode>(svc_hook, on_cull_mode_pre) != MOD_OK) {
        return dusk::mods::set_error(error, MOD_ERROR, "failed to hook GXSetCullMode");
    }
    if (dusk::mods::hook_add_pre<&J3DShape::drawFast>(svc_hook, on_shape_draw_pre) != MOD_OK) {
        svc_log->warn(mod_ctx,
            "failed to hook J3DShape::drawFast (missing dusklight.symdb?); Two-Sided Casters "
            "will not affect J3D geometry");
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
    restore_actual_light_debug();
    svc_resource->free(mod_ctx, &g_shaderSource);
    svc_resource->free(mod_ctx, &g_sssShaderSource);
    svc_resource->free(mod_ctx, &g_normalShaderSource);
    release_sss_target(g_sssTarget);
    for (auto& retired : g_retiredSssTargets) {
        release_sss_target(retired.target);
    }
    g_retiredSssTargets.clear();
    release_normal_targets(g_normalTargets);
    for (auto& retired : g_retiredNormalTargets) {
        release_normal_targets(retired.targets);
    }
    g_retiredNormalTargets.clear();
    if (g_sssPipeline != nullptr) {
        wgpuComputePipelineRelease(g_sssPipeline);
        g_sssPipeline = nullptr;
    }
    if (g_sssLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_sssLayout);
        g_sssLayout = nullptr;
    }
    const auto releaseComputePipeline = [](WGPUComputePipeline& pipeline) {
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
    releaseComputePipeline(g_normalGenPipeline);
    releaseComputePipeline(g_normalBlurHPipeline);
    releaseComputePipeline(g_normalBlurVPipeline);
    releaseLayout(g_normalGenLayout);
    releaseLayout(g_normalBlurHLayout);
    releaseLayout(g_normalBlurVLayout);
    if (g_compositePipeline != nullptr) {
        wgpuRenderPipelineRelease(g_compositePipeline);
        g_compositePipeline = nullptr;
    }
    if (g_compositeDebugPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_compositeDebugPipeline);
        g_compositeDebugPipeline = nullptr;
    }
    if (g_compositeLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_compositeLayout);
        g_compositeLayout = nullptr;
    }
    if (g_compositeDebugLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_compositeDebugLayout);
        g_compositeDebugLayout = nullptr;
    }
    g_cvarEnabled = g_cvarShadowMap = 0;
    g_cvarMapSize = g_cvarNormalSmooth = 0;
    g_cvarNoFrustumClipping = 0;
    g_cvarStrength = 0;
    g_cvarPcf = g_cvarBias = g_cvarBoxRadius = g_cvarContactShadows = g_cvarDebugView = 0;
    g_cvarSlopeBias = g_cvarNormalOffset = 0;
    g_cvarSssThickness = g_cvarSssEdgeThreshold = g_cvarSssContrast = g_cvarSssBias =
        g_cvarSssLength = g_cvarSssIgnoreEdges = 0;
    g_cvarSssFade = g_cvarSssFadeStart = g_cvarSssFadeEnd = 0;
    g_cvarIndoorDisable = g_cvarTwoSidedCasters = 0;
    g_drawType = g_sssComputeType = g_normalComputeType = g_sceneBeginHook =
        g_sceneAfterTerrainHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_controlsWindow = 0;
    g_replayTwoSided = false;
    g_mapPass = {};
    g_sceneCamera.valid = false;
    g_sceneCamera.raw_valid = false;
    return MOD_OK;
}
}
