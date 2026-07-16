// Local Light Shadows - real-geometry shadows cast by the game's local light sources (torch
// fires, bonfires, lanterns). Game-linked: reads the point-light registry the game maintains
// (dKy_getEnvlight()->pointlight[]) to find the nearest active local light, then replays the
// game's own opaque draw lists from that light into an offscreen light-space depth map (the same
// draw-list-replay technique Realtime Sun Shadows uses for the sun) and composites the result over
// the scene, darkening surfaces the light cannot reach.
//
// First attempt (v0.1): ONE nearest light, a DIRECTIONAL (ortho) approximation of the point light
// so the reversed-Z depth path is the proven Realtime Sun Shadows one, on-screen casters only (no
// frustum-clip bypass), and the receiver normal reconstructed inline in the shader (no service
// dependency). The per-pixel reach term (distance attenuation x surface facing, in shadow.wgsl) is
// what makes the darkening read as a local light rather than a sun. A true point projection
// (perspective / dual-paraboloid), N lights, and the SSILVB light-input service export are
// follow-ups documented in docs/local_light_shadows_plan.md.

#include "global.h"

#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/JMath/JMath.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dolphin/gx/GXAurora.h"
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

#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);
// GameService is imported automatically by the SDK under `FEATURES game` (it enforces the game
// ABI epoch); a manual IMPORT_SERVICE(GameService, ...) here would be a duplicate.

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarStrength = 0;
ConfigVarHandle g_cvarMapSize = 0;
ConfigVarHandle g_cvarCoverage = 0;
ConfigVarHandle g_cvarBias = 0;
ConfigVarHandle g_cvarSlopeBias = 0;
ConfigVarHandle g_cvarNormalOffset = 0;
ConfigVarHandle g_cvarPcf = 0;
ConfigVarHandle g_cvarAttenPower = 0;
ConfigVarHandle g_cvarFov = 0;
ConfigVarHandle g_cvarHeightOffset = 0;
ConfigVarHandle g_cvarTwoSidedCasters = 0;
ConfigVarHandle g_cvarDebugView = 0;

GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_sceneAfterTerrainHook = 0;
GfxStageHookHandle g_sceneAfterOpaqueHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
UiWindowHandle g_controlsWindow = 0;
ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_compositePipeline = nullptr;       // multiply blend
WGPURenderPipeline g_compositeDebugPipeline = nullptr;  // no blend (debug views replace the scene)
WGPUBindGroupLayout g_compositeLayout = nullptr;
WGPUBindGroupLayout g_compositeDebugLayout = nullptr;
WGPUSampler g_shadowSampler = nullptr;  // non-filtering clamp sampler for the PCF textureGather

constexpr float kLightNear = 100.0f;

bool g_replayingSceneLists = false;
bool g_replayTwoSided = false;

// Mirror of the WGSL Uniforms struct (keep in sync with res/shadow.wgsl). Scalars are packed to
// avoid vec3 16-byte alignment traps; total size is a multiple of 16.
struct LocalShadowUniforms {
    float world_from_proj[16];
    float light_vp[16];
    float light_pos_x;
    float light_pos_y;
    float light_pos_z;
    float light_pow;
    float strength;
    float bias;
    float slope_bias;
    float normal_offset;
    float map_size;
    float inv_map_size;
    float pcf_taps;
    float texel_world;
    float map_enabled;
    uint32_t debug_mode;
    float atten_power;
    float _pad0;
};
static_assert(sizeof(LocalShadowUniforms) == 192);
static_assert(sizeof(LocalShadowUniforms) % 16 == 0);

struct DrawPayload {
    WGPUTextureView sceneDepth;  // frame-pooled
    WGPUTextureView shadowMap;   // frame-pooled offscreen depth
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t debug_mode;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

struct LightCamera {
    Mtx view;
    Mtx44 proj;
    Mtx44 vp;
    float lightNear = kLightNear;
    float lightFar = 60000.0f;
};

// The rendered local-light shadow map for this frame, plus the light it was rendered from.
struct LocalShadow {
    bool ready = false;
    WGPUTextureView shadowMap = nullptr;  // frame-pooled
    uint32_t mapSize = 0;
    Mtx44 lightVp;              // world -> light receiver clip, row-major, reversed-Z
    float texelWorld = 1.0f;
    float lightNear = kLightNear;
    float lightFar = 60000.0f;
    float lightPos[3] = {};
    float lightPow = 1.0f;
};
LocalShadow g_localShadow;

struct SceneCamera {
    bool valid = false;
    bool raw_valid = false;
    CameraInfo info = CAMERA_INFO_INIT;
    Mtx raw_view;
    f32 raw_projection[7]{};
    Mtx44 raw_projection_mtx;
};
SceneCamera g_sceneCamera;

struct replay_scope {
    replay_scope() { g_replayingSceneLists = true; }
    ~replay_scope() { g_replayingSceneLists = false; }
};

// Hook targets (each DEFINE_HOOK emits a modmeta record the host resolves at load).
DEFINE_HOOK(GXCopyTex, CopyTex);
DEFINE_HOOK(GXSetCullMode, CullMode);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);

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
    return std::clamp<int64_t>(get_int_option(g_cvarDebugView, 0), 0, 4);
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

// Copy a light view-projection and flip Z for the reversed-Z convention the resolved map stores
// (same as Realtime Sun Shadows). The result stays row-major; store_column_major runs at bind time.
void copy_projection(const Mtx44 in, Mtx44 out) {
    std::memcpy(out, in, sizeof(Mtx44));
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

void capture_scene_camera(const GfxStageContext* stageCtx) {
    g_sceneCamera.valid = false;
    g_sceneCamera.raw_valid = false;
    const view_class* gameView = stage_game_view(stageCtx);
    if (gameView == nullptr) {
        return;
    }
    CameraInfo info = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stageCtx->game_view, &info) != MOD_OK) {
        return;
    }
    g_sceneCamera.info = info;
    g_sceneCamera.valid = true;
    g_sceneCamera.raw_valid = capture_raw_camera(gameView, g_sceneCamera.raw_view,
        g_sceneCamera.raw_projection_mtx, g_sceneCamera.raw_projection);
}

// Nearest active point light whose influence sphere contains `ref` (the game's own rule in
// dKy_light_influence_id: a light matters when the receiver is within its mPow radius). Reads the
// registry directly through dKy_getEnvlight() (a proven symbol) + header struct offsets, so it
// links no new game symbols. Returns nullptr when the player stands in no local light.
LIGHT_INFLUENCE* select_local_light(const cXyz& ref) {
    dScnKy_env_light_c* env = dKy_getEnvlight();
    if (env == nullptr) {
        return nullptr;
    }
    LIGHT_INFLUENCE* best = nullptr;
    float bestDistSq = 1.0e30f;
    for (int i = 0; i < 100; ++i) {
        LIGHT_INFLUENCE* light = env->pointlight[i];
        if (light == nullptr || !(light->mPow > 1.0f)) {
            continue;
        }
        const float dx = light->mPosition.x - ref.x;
        const float dy = light->mPosition.y - ref.y;
        const float dz = light->mPosition.z - ref.z;
        if (!std::isfinite(dx + dy + dz)) {
            continue;
        }
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < light->mPow * light->mPow && distSq < bestDistSq) {
            bestDistSq = distSq;
            best = light;
        }
    }
    return best;
}

// Perspective light camera positioned AT the local light, looking toward the receiver region. A
// true point projection: shadows diverge from the light's position, so a flame on a totem casts
// its shadows radially outward rather than as one parallel direction (the v0.1 ortho stand-in put
// them in the wrong place for anything but the player's exact spot). The reversed-Z path is still
// the proven one: copy_projection negates only the clip z row (the w row is the perspective
// divide), so ndc.z reproduces aurora's stored reversed depth exactly as it does for ortho.
bool build_local_light_camera(const cXyz& eye, const cXyz& target, float fovYDeg, float nearZ,
    float farZ, LightCamera& out) {
    cXyz look{target.x - eye.x, target.y - eye.y, target.z - eye.z};
    const float len = std::sqrt(look.x * look.x + look.y * look.y + look.z * look.z);
    if (!(len > 1.0f) || !std::isfinite(len)) {
        return false;
    }
    out.lightNear = nearZ;
    out.lightFar = farZ;
    const bool nearlyVertical = std::fabs(look.y / len) > 0.99f;
    cXyz up = nearlyVertical ? cXyz{0.0f, 0.0f, 1.0f} : cXyz{0.0f, 1.0f, 0.0f};
    cMtx_lookAt(out.view, &eye, &target, &up, 0);
    C_MTXPerspective(out.proj, fovYDeg, 1.0f, nearZ, farZ);
    cMtx_concatProjView(out.proj, out.view, out.vp);
    return true;
}

// GX projection for the replay: fold the camera-inverse-view into the light projection so the
// replay can keep the game's own view matrix (the geometry then transforms world -> light -> clip).
bool build_light_replay_projection(
    const LightCamera& lightCamera, const Mtx cameraView, Mtx44 out) {
    Mtx cameraInvView;
    cMtx_inverse(cameraView, cameraInvView);
    if (!matrix_ready(cameraInvView)) {
        return false;
    }
    Mtx lightFromCamera;
    cMtx_concat(lightCamera.view, cameraInvView, lightFromCamera);
    cMtx_concatProjView(lightCamera.proj, lightFromCamera, out);
    return true;
}

HookAction on_copy_tex_pre(ModContext*, void*, void*, void*) {
    return g_replayingSceneLists ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// Two-sided casters: single-sided geometry facing the player is back-facing from the light and
// would be culled out of the map, leaking light. Rewrite direct GX cull-mode sets to none during
// the replay.
HookAction on_cull_mode_pre(ModContext*, void* args, void*, void*) {
    if (g_replayingSceneLists && g_replayTwoSided) {
        mods::arg_ref<GXCullMode>(args, 0) = GX_CULL_NONE;
    }
    return HOOK_CONTINUE;
}

// J3D materials bake their cull mode into the material display list's genMode BP write; re-issue
// genMode through the GX shim (same stage counts, cull forced off) between the material load and
// the shape's geometry so J3D casters are two-sided too.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (!g_replayingSceneLists || !g_replayTwoSided) {
        return HOOK_CONTINUE;
    }
    const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
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

bool build_pipeline(bool blend, WGPURenderPipeline& outPipeline, WGPUBindGroupLayout& outLayout) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(g_shaderSource.data), g_shaderSource.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"local light shadow composite", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }
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
    pipelineDesc.label = {
        blend ? "local shadow composite" : "local shadow composite (debug)", WGPU_STRLEN};
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
    if (data.sceneDepth == nullptr || data.shadowMap == nullptr || pipeline == nullptr ||
        g_shadowSampler == nullptr)
    {
        return;
    }

    WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = data.sceneDepth;
    entries[1].binding = 1;
    entries[1].textureView = data.shadowMap;
    entries[2].binding = 2;
    entries[2].buffer = ctx->uniform_buffer;
    entries[2].offset = data.uniform_offset;
    entries[2].size = data.uniform_size;
    entries[3].binding = 3;
    entries[3].sampler = g_shadowSampler;
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = layout;
    bindGroupDesc.entryCount = 4;
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

// Game thread, after the draw handlers populate this frame's opaque lists: select the nearest
// local light and replay the opaque geometry into an offscreen light-space depth map.
void render_local_shadow_map(const Mtx replayView) {
    if (g_localShadow.ready || !get_bool_option(g_cvarEnabled, true)) {
        return;
    }
    if (!matrix_ready(replayView) || !draw_lists_ready()) {
        return;
    }

    // Receiver anchor: the player (matches vanilla, which casts Link's shadow from the local
    // light), else the camera eye.
    cXyz ref;
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (player != nullptr && std::isfinite(player->current.pos.x) &&
        std::isfinite(player->current.pos.y) && std::isfinite(player->current.pos.z))
    {
        ref = player->current.pos;
    } else if (g_sceneCamera.valid) {
        ref = cXyz{g_sceneCamera.info.eye[0], g_sceneCamera.info.eye[1], g_sceneCamera.info.eye[2]};
    } else {
        return;
    }

    LIGHT_INFLUENCE* light = select_local_light(ref);
    if (light == nullptr) {
        return;
    }
    cXyz lightPos = light->mPosition;
    // The registered light position can sit below the visible flame particle; let the user raise it
    // to line up. Applied to both the shadow projection and the reach attenuation so they agree.
    lightPos.y += static_cast<float>(
        std::clamp<int64_t>(get_int_option(g_cvarHeightOffset, 0), -2000, 2000));
    if (!std::isfinite(lightPos.x + lightPos.y + lightPos.z)) {
        return;
    }

    const uint32_t mapSize = 512u << std::clamp<int64_t>(get_int_option(g_cvarMapSize, 1), 0, 3);
    const float fov =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarFov, 130), 40, 170));
    const float farZ =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarCoverage, 2500), 500, 12000));
    constexpr float kNearZ = 5.0f;

    // Camera AT the light, looking at the receiver region (the player).
    LightCamera lightCamera{};
    if (!build_local_light_camera(lightPos, ref, fov, kNearZ, farZ, lightCamera)) {
        return;
    }
    Mtx44 replayProjection;
    if (!build_light_replay_projection(lightCamera, replayView, replayProjection)) {
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
    const auto restore_game_camera = [&]() {
        j3dSys.setViewMtx(savedView);
        GXSetProjectionv(savedProjection);
        GXSetViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3],
            savedViewport[4], savedViewport[5]);
        GXSetScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
        dKy_setLight();
    };

    if (svc_gfx->create_pass(mod_ctx, mapSize, mapSize) != MOD_OK) {
        return;
    }
    J3DShape::resetVcdVatCache();

    j3dSys.setViewMtx(replayView);
    GXSetProjectionFull(replayProjection);
    GXSetViewport(0.0f, 0.0f, static_cast<float>(mapSize), static_cast<float>(mapSize), 0.0f, 1.0f);
    GXSetViewportRender(
        0.0f, 0.0f, static_cast<float>(mapSize), static_cast<float>(mapSize), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, mapSize, mapSize);
    dKy_setLight();
    // Depth-only: a shadow map needs only depth. Alpha TEST still runs (foliage keeps its holes).
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_FALSE);
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
    resolveDesc.color = false;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr)
    {
        return;
    }
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restore_game_camera();

    g_localShadow.shadowMap = resolved.depth;
    g_localShadow.mapSize = mapSize;
    g_localShadow.lightNear = lightCamera.lightNear;
    g_localShadow.lightFar = lightCamera.lightFar;
    // Approximate world size of one shadow texel at the receiver distance (the normal-offset
    // scale). For a perspective map the texel grows with depth; the receiver region is the
    // representative distance.
    const float dx = ref.x - lightPos.x;
    const float dy = ref.y - lightPos.y;
    const float dz = ref.z - lightPos.z;
    const float distToRef = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float halfFovRad = fov * 0.5f * 0.017453292f;
    g_localShadow.texelWorld =
        (2.0f * std::tan(halfFovRad) * distToRef) / static_cast<float>(mapSize);
    copy_projection(lightCamera.vp, g_localShadow.lightVp);
    g_localShadow.lightPos[0] = lightPos.x;
    g_localShadow.lightPos[1] = lightPos.y;
    g_localShadow.lightPos[2] = lightPos.z;
    g_localShadow.lightPow = light->mPow;
    g_localShadow.ready = true;
}

void composite_pass(int64_t debugMode) {
    const LocalShadow shadow = std::exchange(g_localShadow, {});
    if (!draw_lists_ready() || !get_bool_option(g_cvarEnabled, true)) {
        return;
    }
    if (!shadow.ready || shadow.shadowMap == nullptr || !g_sceneCamera.valid) {
        return;
    }
    const CameraInfo& camera = g_sceneCamera.info;

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = false;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr)
    {
        return;
    }

    // Perspective reversed-Z depth is non-linear (most precision near the light, which is where
    // local-light receivers sit), so bias is a small constant in ndc.z units, tuned by eye rather
    // than normalized against a world range as the ortho sun path does.
    constexpr float kBiasNdcScale = 2.0e-5f;
    const float biasNdc =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarBias, 40), 0, 400)) *
        kBiasNdcScale;
    const float slopeBiasNdc =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarSlopeBias, 40), 0, 400)) *
        kBiasNdcScale;

    LocalShadowUniforms uniforms{};
    std::memcpy(uniforms.world_from_proj, camera.world_from_proj, sizeof(uniforms.world_from_proj));
    store_column_major(shadow.lightVp, uniforms.light_vp);
    uniforms.light_pos_x = shadow.lightPos[0];
    uniforms.light_pos_y = shadow.lightPos[1];
    uniforms.light_pos_z = shadow.lightPos[2];
    uniforms.light_pow = shadow.lightPow;
    uniforms.strength =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarStrength, 45), 0, 100)) / 100.0f;
    uniforms.bias = biasNdc;
    uniforms.slope_bias = slopeBiasNdc;
    uniforms.normal_offset =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarNormalOffset, 100), 0, 300)) /
        100.0f;
    uniforms.map_size = static_cast<float>(shadow.mapSize);
    uniforms.inv_map_size = 1.0f / static_cast<float>(shadow.mapSize);
    uniforms.pcf_taps =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarPcf, 1), 0, 3));
    uniforms.texel_world = shadow.texelWorld;
    uniforms.map_enabled = 1.0f;
    uniforms.debug_mode = static_cast<uint32_t>(debugMode);
    uniforms.atten_power =
        static_cast<float>(std::clamp<int64_t>(get_int_option(g_cvarAttenPower, 200), 10, 400)) /
        100.0f;

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniformRange) != MOD_OK) {
        return;
    }
    DrawPayload payload{};
    payload.sceneDepth = resolved.depth;
    payload.shadowMap = shadow.shadowMap;
    payload.uniform_offset = uniformRange.offset;
    payload.uniform_size = uniformRange.size;
    payload.debug_mode = static_cast<uint32_t>(debugMode);
    svc_gfx->push_draw(mod_ctx, g_drawType, &payload, sizeof(payload));
}

void on_scene_begin(ModContext*, const GfxStageContext* stageCtx, void*) {
    capture_scene_camera(stageCtx);
}

void on_scene_after_terrain(ModContext*, const GfxStageContext* stageCtx, void*) {
    if (g_localShadow.ready) {
        return;
    }
    const view_class* gameView = stage_game_view(stageCtx);
    Mtx replayView;
    Mtx44 replayProjectionMtx;
    f32 replayProjection[7];
    if (!capture_raw_camera(gameView, replayView, replayProjectionMtx, replayProjection)) {
        return;
    }
    render_local_shadow_map(replayView);
}

// Composite before translucency and the game's bloom, so shadows darken the world underneath.
void on_scene_after_opaque(ModContext*, const GfxStageContext*, void*) {
    if (get_debug_mode() != 0) {
        return;
    }
    composite_pass(0);
}

// Debug views replace the scene, drawn after the full opaque scene so nothing obscures them.
void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    const int64_t debugMode = get_debug_mode();
    if (debugMode == 0) {
        g_localShadow = {};
        return;
    }
    composite_pass(debugMode);
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
    svc_ui->pane_add_section(mod_ctx, left, "General");
    add_toggle(left, "Enabled", g_cvarEnabled, "Master toggle for local light shadows.");
    add_number(left, "Strength", g_cvarStrength, 0, 100, 5, "%",
        "How dark shadowed areas become where the local light reaches.");

    svc_ui->pane_add_section(mod_ctx, left, "Light");
    add_number(left, "Light Height Offset", g_cvarHeightOffset, -2000, 2000, 10, nullptr,
        "Raises (or lowers) the shadow-casting light relative to its registered position. The game "
        "registers a torch/fire's light a bit below the visible flame particle; nudge this up until "
        "shadows radiate from the flame you see.");
    add_number(left, "Cone Angle", g_cvarFov, 40, 170, 5, "deg",
        "Field of view of the light's shadow frustum (it points from the light toward the player). "
        "Wider covers more of the scene around you at lower detail; a point light is omnidirectional "
        "so this is the visible cone the first attempt renders.");
    add_number(left, "Range", g_cvarCoverage, 500, 12000, 250, nullptr,
        "Far distance of the shadow frustum from the light, in world units. Casters beyond it do "
        "not appear in the map. Set around the light's reach; larger costs a little depth precision.");

    svc_ui->pane_add_section(mod_ctx, left, "Shadow Map");
    static const char* kMapSizes[] = {"512", "1024", "2048", "4096"};
    add_select(left, "Map Size", g_cvarMapSize, kMapSizes, 4,
        "Resolution of the local light's shadow map. Larger is sharper and slower.");
    add_number(left, "Falloff", g_cvarAttenPower, 10, 400, 10, "%",
        "Shapes how fast the shadow fades with distance from the light (attenuation exponent). "
        "Higher concentrates the shadow near the light; 100% is linear.");
    static const char* kPcfOptions[] = {"Off", "3x3", "5x5", "7x7"};
    add_select(left, "Soft Shadows", g_cvarPcf, kPcfOptions, 4,
        "Shadow-map edge softening (percentage-closer filtering).");
    add_number(left, "Bias", g_cvarBias, 0, 400, 5, nullptr,
        "Constant depth bias. Raise to remove shadow-map acne (shimmering self-shadow); lower to "
        "reduce peter-panning (shadows detaching from feet).");
    add_number(left, "Slope Bias", g_cvarSlopeBias, 0, 400, 5, nullptr,
        "Extra bias that grows with surface slope relative to the light. Targets sloped-surface "
        "acne without detaching flat-ground shadows.");
    add_number(left, "Normal Offset", g_cvarNormalOffset, 0, 300, 10, "%",
        "Shifts the shadow-map lookup along the surface normal, scaled to one texel. The most "
        "effective acne fix with the least peter-panning; 100% = one texel.");
    add_toggle(left, "Two-Sided Casters", g_cvarTwoSidedCasters,
        "Renders casters with backface culling disabled, fixing light leaking through "
        "single-sided geometry (walls, roofs) that faces away from the light.");

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugViews[] = {"Off", "Occlusion", "Reach", "Light UV", "Depth Compare"};
    add_select(left, "Debug View", g_cvarDebugView, kDebugViews, 5,
        "Occlusion = raw shadow term; Reach = light attenuation x facing; Light UV = the "
        "receiver's projection into the shadow map (red/green = uv, blue = inside the map); Depth "
        "Compare = red is the receiver's depth-from-light, green is the stored map depth at that "
        "spot - on directly-lit surfaces they should match (yellow), diverging means a bias/"
        "projection issue.");
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
        svc_log->error(mod_ctx, "failed to open Local Light Shadows controls window");
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
        return mods::set_error(error, MOD_ERROR, "failed to register option");
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
        return mods::set_error(error, MOD_ERROR, "failed to register option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "shadow.wgsl", &g_shaderSource);
    if (result != MOD_OK || g_shaderSource.data == nullptr) {
        return mods::set_error(error, result, "failed to load shadow.wgsl");
    }

    struct IntVar {
        const char* name;
        int64_t def;
        ConfigVarHandle* handle;
    };
    if ((result = register_bool_option("effectEnabled", true, g_cvarEnabled, error)) != MOD_OK) {
        return result;
    }
    if ((result = register_bool_option(
             "twoSidedCasters", true, g_cvarTwoSidedCasters, error)) != MOD_OK)
    {
        return result;
    }
    const IntVar intVars[] = {
        {"strength", 45, &g_cvarStrength},
        {"mapSize", 1, &g_cvarMapSize},
        {"coverage", 2500, &g_cvarCoverage},
        {"fov", 130, &g_cvarFov},
        {"heightOffset", 0, &g_cvarHeightOffset},
        {"bias", 40, &g_cvarBias},
        {"slopeBias", 40, &g_cvarSlopeBias},
        {"normalOffset", 100, &g_cvarNormalOffset},
        {"pcf", 1, &g_cvarPcf},
        {"attenPower", 200, &g_cvarAttenPower},
        {"debugView", 0, &g_cvarDebugView},
    };
    for (const IntVar& v : intVars) {
        if ((result = register_int_option(v.name, v.def, *v.handle, error)) != MOD_OK) {
            return result;
        }
    }

    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }
    if (!build_pipeline(true, g_compositePipeline, g_compositeLayout) ||
        !build_pipeline(false, g_compositeDebugPipeline, g_compositeDebugLayout))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create composite pipeline");
    }

    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.label = {"local shadow pcf gather", WGPU_STRLEN};
    samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    samplerDesc.magFilter = WGPUFilterMode_Nearest;
    samplerDesc.minFilter = WGPUFilterMode_Nearest;
    samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    samplerDesc.maxAnisotropy = 1;
    g_shadowSampler = wgpuDeviceCreateSampler(g_deviceInfo.device, &samplerDesc);
    if (g_shadowSampler == nullptr) {
        return mods::set_error(error, MOD_ERROR, "failed to create shadow PCF sampler");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "local light shadow composite";
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
    stageDesc.callback = on_scene_after_terrain;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_SCENE_AFTER_TERRAIN, &stageDesc,
            &g_sceneAfterTerrainHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stageDesc,
            &g_sceneAfterOpaqueHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }
    stageDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    if (mods::hook_add_pre<CopyTex>(svc_hook, on_copy_tex_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook GXCopyTex");
    }
    if (mods::hook_add_pre<CullMode>(svc_hook, on_cull_mode_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook GXSetCullMode");
    }
    if (mods::hook_add_pre<ShapeDrawFast>(svc_hook, on_shape_draw_pre) != MOD_OK) {
        svc_log->warn(mod_ctx,
            "failed to hook J3DShape::drawFast (missing dusklight.symdb?); Two-Sided Casters will "
            "not affect J3D geometry");
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
    if (g_shadowSampler != nullptr) {
        wgpuSamplerRelease(g_shadowSampler);
        g_shadowSampler = nullptr;
    }
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
    releasePipeline(g_compositePipeline);
    releasePipeline(g_compositeDebugPipeline);
    releaseLayout(g_compositeLayout);
    releaseLayout(g_compositeDebugLayout);

    g_cvarEnabled = g_cvarStrength = g_cvarMapSize = g_cvarCoverage = 0;
    g_cvarFov = g_cvarHeightOffset = 0;
    g_cvarBias = g_cvarSlopeBias = g_cvarNormalOffset = g_cvarPcf = 0;
    g_cvarAttenPower = g_cvarTwoSidedCasters = g_cvarDebugView = 0;
    g_drawType = 0;
    g_sceneBeginHook = g_sceneAfterTerrainHook = g_sceneAfterOpaqueHook = g_frameBeforeHudHook = 0;
    g_controlsWindow = 0;
    g_replayingSceneLists = false;
    g_replayTwoSided = false;
    g_localShadow = {};
    g_sceneCamera.valid = false;
    g_sceneCamera.raw_valid = false;
    return MOD_OK;
}
}
