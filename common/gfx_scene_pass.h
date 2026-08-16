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
// Usage at a pipeline build site (unchanged from the previous SDK):
//
//     gfx_compat::ScenePassLayout layout;
//     if (!gfx_compat::scene_pass_layout(mod_ctx, svc_gfx, g_deviceInfo, layout)) {
//         return false;
//     }
//     layout.color_targets[0].blend = &myBlendState;  // blend state is the caller's
//     fragment.targetCount = layout.color_target_count;
//     fragment.targets = layout.color_targets;
//     depthStencil.format = layout.depth_format;
//     pipelineDesc.multisample.count = layout.sample_count;
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
    /// True when the pass carries a `GFX_ATTACHMENT_NORMAL` attachment, i.e. this game build is
    /// actually producing authored normals right now. Always false on a base without the feature
    /// and while the user has the game's own Scene Normal Buffer setting switched off.
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

}  // namespace gfx_compat
