// The attachment layout a render pipeline must declare to draw into the game's scene pass.
//
// A render pipeline is valid only in a pass whose attachments it matches exactly: colour target
// count and formats, depth format, sample count. GfxService 1.2+ reports that layout as a
// `GfxRenderTargetLayout` (one entry per colour attachment, tagged with a `GfxAttachmentSemantic`),
// both from `get_scene_target_layout` and as `GfxDrawContext::layout`. The SDK's inline
// `gfx_init_color_target_states` turns it into a `WGPUColorTargetState[]` with every attachment
// except the scene colour write-masked off. This header wraps those so call sites stay short.
//
// Important: the scene pass changes shape at runtime. The first `resolve_pass` that asks for
// normals adds a normal attachment from the next frame on, and WebGPU rejects a pipeline built for
// the old shape with nothing logged: the draw just stops appearing. So build scene-pass pipelines
// lazily in the draw callback, from the draw context, and rebuild them when the key changes:
//
//     bool ensure_pipelines(const GfxDrawContext& ctx) {  // first thing in the draw callback
//         const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
//         if (g_pipeline != nullptr && g_layoutValid && g_layoutKey == key) { return true; }
//         release_pipelines();
//         gfx_compat::ScenePassLayout layout;
//         if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, layout)) { return false; }
//         layout.color_targets[0].blend = &myBlendState;  // optional; the default is replace
//         fragment.targetCount = layout.color_target_count;
//         fragment.targets = layout.color_targets;
//         depthStencil.format = layout.depth_format;
//         pipelineDesc.multisample.count = layout.sample_count;
//         ...create the pipeline...
//         g_layoutKey = key; g_layoutValid = true;
//         return true;
//     }
//
// Built mods all use this form: VBAO (ensure_composite_pipelines), SMAA
// (ensure_neighborhood_pipeline) and Deferred Fog (ensure_fog_pipelines). `scene_pass_layout()`
// asks the service instead and returns no key, so it suits a one-off inspection, not a cached
// pipeline. Only the unreleased mods (ssilvb, realtime_sun_shadows; not built) still use it, and
// they need to move to the draw-context form before they are built again.
//
// `color_targets[0]` is the scene colour and the only target a caller may write. The others are
// renderer-owned and come back write-masked, so a composite leaves the game's normals untouched.
// Offscreen passes from `create_pass` are single-target and need none of this.
//
// Why a preprocessor guard, and why an #error (gfx_normal_compat.h uses neither):
//  - The API used here is types and macros (`GfxRenderTargetLayout`, `GFX_ATTACHMENT_NORMAL`,
//    `GFX_MAX_COLOR_ATTACHMENTS`). A discarded `if constexpr` branch still looks up non-dependent
//    names, so only `#if` can drop this code on an SDK that lacks them.
//  - A failed probe means the SDK is either older than 1.2 or has renamed the API, and a renamed
//    API looks exactly like an absent one. Silently assuming a one-target pass would make every
//    scene-pass composite vanish with no diagnostic, so an unrecognised SDK is a build error.
//    Define GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT only for a base that really predates the query.
//
// Background: CONTRIBUTING.md "Rules that have bitten before", docs/normal_buffer_portability.md.

#pragma once

#include <type_traits>
#include <utility>

#include "gfx_normal_compat.h"
#include "mods/svc/gfx.h"

// GFX_MAX_COLOR_ATTACHMENTS sizes GfxRenderTargetLayout's attachment array, so any SDK with the
// scene-layout API defines it; one probe covers every name this header uses.
#if defined(GFX_MAX_COLOR_ATTACHMENTS)
#define GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT 1
#else
#define GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT 0
#endif

#if !GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT && !defined(GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT)
#error \
    "This SDK has no GFX_MAX_COLOR_ATTACHMENTS, so GfxService's scene-target-layout query is \
missing or has been renamed again. Do NOT assume the scene pass has one colour target: if the host \
has more, every scene-pass composite in this repo is rejected at draw time with no other symptom. \
Port this header to the new query. If the base genuinely predates the query (pre-1.2, no normal \
attachment can exist), define GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT to take the GfxDeviceInfo path."
#endif

namespace gfx_compat {

#if GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT
inline constexpr uint32_t kMaxSceneColorTargets = GFX_MAX_COLOR_ATTACHMENTS;
inline constexpr uint32_t kSceneColorIndex = GFX_SCENE_COLOR_ATTACHMENT_INDEX;
#else
// Pre-1.2 SDKs describe the scene colour and nothing else.
inline constexpr uint32_t kMaxSceneColorTargets = 1u;
inline constexpr uint32_t kSceneColorIndex = 0u;
#endif
static_assert(kSceneColorIndex == 0u, "call sites write color_targets[0] as the scene colour");

/// Everything a render pipeline descriptor needs to match the scene pass.
struct ScenePassLayout {
    WGPUColorTargetState color_targets[kMaxSceneColorTargets] = {};
    uint32_t color_target_count = 0;
    WGPUTextureFormat depth_format = WGPUTextureFormat_Undefined;
    uint32_t sample_count = 1;
    /// True when the pass has a `GFX_ATTACHMENT_NORMAL` attachment, i.e. the renderer is writing
    /// authored normals. This is a runtime state, not a property of the build: it is false until
    /// some mod's `resolve_pass` first asks for normals and true from the next frame on. It stays
    /// false on an SDK or host without the feature, on the D3D11 / OpenGL ES compatibility
    /// renderers (no WebGPU core features), and with MSAA on (aurora creates the normal buffer
    /// only when `msaaSamples == 1`). Always false on the legacy path.
    bool has_normal_attachment = false;
};

/// Asks the service for the scene pass's current layout. Returns false if the call fails or
/// reports no colour attachment. There is no key, so do not cache a pipeline built from this; in a
/// draw callback use scene_pass_layout_for_draw().
///
/// `ctx`/`gfx` are unused on the legacy path and `info` on the query path; taking all three keeps
/// the call site identical across SDK versions.
template <class Service, class DeviceInfo>
inline bool scene_pass_layout(
    ModContext* ctx, const Service* gfx, const DeviceInfo& info, ScenePassLayout& out) {
    out = ScenePassLayout{};
#if GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT
    (void)info;
    GfxRenderTargetLayout layout = GFX_RENDER_TARGET_LAYOUT_INIT;
    layout.struct_size = sizeof(GfxRenderTargetLayout);
    if (gfx->get_scene_target_layout(ctx, &layout) != MOD_OK ||
        layout.color_attachment_count == 0)
    {
        return false;
    }
    // The SDK helper write-masks every attachment off, then re-opens only the scene colour. Pass a
    // null blend (replace) and a full write mask; a caller that blends sets color_targets[0].blend
    // afterwards.
    out.color_target_count =
        gfx_init_color_target_states(&layout, out.color_targets, nullptr, WGPUColorWriteMask_All);
    if (out.color_target_count == 0) {
        return false;
    }
    out.depth_format = layout.depth_stencil_format;
    out.sample_count = layout.sample_count;
    for (uint32_t i = 0; i < layout.color_attachment_count && i < kMaxSceneColorTargets; ++i) {
        if (layout.color_attachments[i].semantic == GFX_ATTACHMENT_NORMAL) {
            out.has_normal_attachment = true;
            break;
        }
    }
    return true;
#else
    (void)ctx;
    (void)gfx;
    // Pre-1.2: a single scene colour target; no normal attachment can exist.
    out.color_targets[0] = WGPU_COLOR_TARGET_STATE_INIT;
    out.color_targets[0].format = info.color_format;
    out.color_target_count = 1;
    out.depth_format = info.depth_format;
    out.sample_count = info.sample_count;
    return true;
#endif
}

/// Fills `out` from `GfxDrawContext::layout`: the layout of the pass this draw is being recorded
/// into, which is the one a pipeline used in the draw must match. Pair it with
/// `scene_pass_layout_key()` and rebuild when the key changes, as the SDK header asks ("rebuild
/// pipelines if GfxDrawContext.layout key changes") and upstream's `mods/ao_mod` does in its
/// `ensure_pipelines()`. Returns false if the layout has no colour attachment.
template <class DrawContext, class DeviceInfo>
inline bool scene_pass_layout_for_draw(
    const DrawContext& ctx, const DeviceInfo& info, ScenePassLayout& out) {
    out = ScenePassLayout{};
#if GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT
    (void)info;
    const GfxRenderTargetLayout& layout = ctx.layout;
    if (layout.color_attachment_count == 0) {
        return false;
    }
    out.color_target_count =
        gfx_init_color_target_states(&layout, out.color_targets, nullptr, WGPUColorWriteMask_All);
    if (out.color_target_count == 0) {
        return false;
    }
    out.depth_format = layout.depth_stencil_format;
    out.sample_count = layout.sample_count;
    for (uint32_t i = 0; i < layout.color_attachment_count && i < kMaxSceneColorTargets; ++i) {
        if (layout.color_attachments[i].semantic == GFX_ATTACHMENT_NORMAL) {
            out.has_normal_attachment = true;
            break;
        }
    }
    return true;
#else
    (void)ctx;
    out.color_targets[0] = WGPU_COLOR_TARGET_STATE_INIT;
    out.color_targets[0].format = info.color_format;
    out.color_target_count = 1;
    out.depth_format = info.depth_format;
    out.sample_count = info.sample_count;
    return true;
#endif
}

/// The identity of the pass layout a draw is going into. Rebuild pipelines when it differs from the
/// key they were built with. Returns 0 on a pre-1.2 SDK, which has no key and no normal attachment
/// that could change the layout.
template <class DrawContext>
inline uint64_t scene_pass_layout_key(const DrawContext& ctx) {
#if GFX_COMPAT_HAVE_SCENE_TARGET_LAYOUT
    return ctx.layout.key;
#else
    (void)ctx;
    return 0u;
#endif
}

}  // namespace gfx_compat
