// The attachment layout a render pipeline must declare to draw into the game's scene pass.
//
// A pipeline is only valid in a pass whose attachments it describes exactly — same colour target
// count, same formats, same depth format, same sample count. Mods used to rebuild that layout by
// hand out of `GfxDeviceInfo`. That is a copy of the renderer's own logic, and it goes silently
// wrong the moment the pass changes shape: WebGPU rejects the pipeline and the composite simply
// never appears.
//
// GfxService 1.2 answers the question directly with `get_scene_target_layout`, which returns a
// `GfxRenderTargetLayout` — one `GfxColorAttachmentLayout` per attachment, each tagged with a
// `GfxAttachmentSemantic` (`GFX_ATTACHMENT_SCENE_COLOR`, `GFX_ATTACHMENT_NORMAL`,
// `GFX_ATTACHMENT_AUXILIARY`). The SDK also ships the inline helper `gfx_init_color_target_states`,
// which turns that into a `WGPUColorTargetState[]` with every attachment the mod does not own
// already write-masked off. This header is a thin wrapper over those two so the call sites stay
// short and identical across SDK versions.
//
// THE SCENE PASS CHANGES SHAPE AT RUNTIME, SO READ THE LAYOUT FROM THE DRAW CONTEXT.
//
// The normal attachment is created on demand — the first `resolve_pass` that asks for normals adds
// it to the pass from the NEXT frame on. A pipeline built during `mod_initialize` therefore
// describes a one-attachment pass and stops matching the moment that happens, and WebGPU's
// rejection is silent: the composite just never appears again. Build lazily inside the draw
// callback and rebuild when the pass's key changes:
//
//     // in the draw callback, before recording anything
//     if (!ensure_pipelines(*ctx)) { return; }
//
//     bool ensure_pipelines(const GfxDrawContext& ctx) {
//         const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
//         if (g_pipeline != nullptr && g_layoutValid && g_layoutKey == key) { return true; }
//         release_pipelines();
//         gfx_compat::ScenePassLayout layout;
//         if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, layout)) { return false; }
//         ...build from `layout` as below...
//         g_layoutKey = key; g_layoutValid = true;
//         return true;
//     }
//
// Filling the descriptor is the same either way:
//
//     layout.color_targets[0].blend = &myBlendState;  // blend state is the caller's
//     fragment.targetCount = layout.color_target_count;
//     fragment.targets = layout.color_targets;
//     depthStencil.format = layout.depth_format;
//     pipelineDesc.multisample.count = layout.sample_count;
//
// `scene_pass_layout(mod_ctx, svc_gfx, g_deviceInfo, out)` asks the service the same question
// outside a draw callback. It is correct for a one-shot inspection, and WRONG as the basis for a
// pipeline you then cache — it answers for the pass as it is now, with no key to notice it change.
// The mods still calling it that way (`ssilvb`, `realtime_sun_shadows`, `deferred_fog`) are out of
// the build and must move to the draw-context form before they go back in.
//
// `color_targets[0]` is the scene colour (`GFX_SCENE_COLOR_ATTACHMENT_INDEX`) and is the only one
// the caller may write; everything past it is renderer-owned and comes back write-masked, so a
// composite that only reads the scene leaves the game's authored normals untouched without having
// to know what they are. Offscreen passes from `create_pass` are single-target and skip all this.
//
// `has_normal_attachment` answers "does this build actually carry authored normals" from the same
// query, which is the ONLY correct way to ask now. It used to be
// `normal_format(g_deviceInfo) != Undefined`; `GfxDeviceInfo::normal_format` no longer exists in
// any SDK. See gfx_normal_compat.h for the rest of that story.
//
// WHY A PREPROCESSOR GUARD, when `gfx_normal_compat.h` next door detects its fields with a type
// trait and no `#if` at all. That trait works because a *member* of an existing type can be probed
// with SFINAE. Here the whole vocabulary is types and constants — `GfxRenderTargetLayout`,
// `GfxColorAttachmentLayout`, `GFX_ATTACHMENT_NORMAL`, `GFX_MAX_COLOR_ATTACHMENTS` — and an
// `if constexpr` branch is still parsed and name-looked-up even when discarded, so naming any of
// them would be a hard compile error on an older SDK rather than a quietly unused branch.
// (Verified: it was written that way first and the stripped-SDK build rejected it.) Only the
// preprocessor removes code from the translation unit outright.
//
// WHY THE MISSING CASE IS AN #error AND NOT A SILENT FALLBACK. This header previously guarded on
// `GFX_MAX_COLOR_TARGETS` and fell back to a hand-assembled single-target layout when it was
// absent. Upstream then shipped its own version of the same feature under different names, our
// guard went false, and THE WHOLE TREE STILL COMPILED — every composite quietly reverted to
// declaring one colour target against a two-attachment pass, which WebGPU rejects at draw time.
// A renamed API is not an absent one, and "degrade to absent" cannot tell them apart. Getting the
// scene layout wrong makes six mods draw nothing with no diagnostic, so the missing case is loud
// by construction. The legacy path is still available, but only when asked for by name.

#pragma once

#include <type_traits>
#include <utility>

#include "gfx_normal_compat.h"
#include "mods/svc/gfx.h"

// Every name this header needs from the scene-layout API arrives together, so one probe covers all.
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
    /// True when the pass carries a `GFX_ATTACHMENT_NORMAL` attachment, i.e. the renderer is
    /// actually producing authored normals right now.
    ///
    /// This is a LIVE, CHANGING fact, not a property of the build. The attachment is created on
    /// demand: it is false until some mod's `resolve_pass` first asks for normals, and true from
    /// the following frame on. It stays false forever on a base without the feature, on an adapter
    /// without WebGPU core features (the D3D11/OpenGL ES compatibility renderers), and whenever
    /// MSAA is on -- aurora refuses to create the buffer unless `msaaSamples == 1`.
    ///
    /// Because it flips at runtime, a pipeline built while it was false does NOT match the pass
    /// afterwards, and WebGPU rejects the draw silently. Build scene-pass pipelines lazily and
    /// rebuild them when `scene_pass_layout_key()` changes.
    bool has_normal_attachment = false;
};

/// Fills `out` with the scene pass's attachment layout. Returns false only when the service call
/// fails outright, which leaves the caller with no valid pipeline to build.
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
    // null blend and a full write mask: a caller that blends overrides color_targets[0] afterward,
    // which is the pattern every call site in this repo uses.
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
    // Pre-1.2: one scene colour target, and no normal attachment can exist to miss.
    out.color_targets[0] = WGPU_COLOR_TARGET_STATE_INIT;
    out.color_targets[0].format = info.color_format;
    out.color_target_count = 1;
    out.depth_format = info.depth_format;
    out.sample_count = info.sample_count;
    return true;
#endif
}

/// Fills `out` from a layout the caller ALREADY HOLDS — `GfxDrawContext::layout`, inside a draw
/// callback — rather than asking the service for it. Two reasons to prefer this in `on_draw`:
///
///  1. It is the layout of the pass this draw is actually being recorded into, which is the only
///     one a pipeline has to match.
///  2. **The scene pass can change shape while the game is running, and a pipeline built against
///     the old shape is silently rejected.** Requesting the authored normals (GfxService 1.3)
///     enables the normal attachment one frame later, so a mod that builds its scene-pass
///     pipelines once at init — when the pass still has a single colour target — has every
///     composite dropped from the moment its own first request takes effect. The SDK says as much:
///     "rebuild pipelines if GfxDrawContext.layout key changes".
///
/// Pair it with `scene_pass_layout_key()` and rebuild when the key moves. Upstream's own reference
/// consumer (`mods/ao_mod`) does exactly this and builds no scene pipeline at init at all.
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

/// The identity of the pass a draw is going into. Compare it against the key the current pipelines
/// were built with; rebuild when it differs. Returns 0 on a pre-1.2 SDK, where there is no key —
/// and no runtime layout change to detect either, so a constant is the right answer there.
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
