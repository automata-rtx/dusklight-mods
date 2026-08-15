// The attachment layout a render pipeline must declare to draw into the game's scene pass.
//
// A pipeline is only valid in a pass whose attachments it describes exactly — same colour target
// count, same formats, same depth format, same sample count. Mods used to rebuild that layout by
// hand out of `GfxDeviceInfo` (color_format + depth_format + sample_count, plus a second
// write-masked target when `normal_format` was set). That is a copy of the renderer's own logic,
// and it goes silently wrong the moment the pass changes shape: WebGPU rejects the pipeline and
// the composite simply never appears.
//
// GfxService 1.2 answers the question directly with `get_pass_targets`, which is what this header
// prefers, keeping the hand-assembled version as the fallback for an older SDK. One source tree
// therefore stays correct on a base with the scene normal buffer, on a base without it, and on an
// SDK that predates the query altogether.
//
// Usage at a pipeline build site:
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
// `color_targets[0]` is the scene colour. Anything past it is renderer-owned and comes back with
// an empty write mask, so a composite that only reads the scene leaves it untouched without having
// to know what it is. Offscreen passes from `create_pass` are single-target and do not need this.
//
// WHY A PREPROCESSOR GUARD, when `gfx_normal_compat.h` next door detects its fields with a type
// trait and no `#if` at all. That trait works because a *member* of an existing type can be probed
// with SFINAE. Here the entire vocabulary arrived at once in 1.2 — `GfxPassTargets`, `GfxPass`,
// `GFX_PASS_SCENE`, `GFX_PASS_TARGETS_INIT`, `GFX_MAX_COLOR_TARGETS` — and an `if constexpr` branch
// is still parsed and name-looked-up even when discarded, so naming any of them would be a hard
// compile error on an older SDK rather than a quietly unused branch. (Verified: it was written that
// way first and the stripped-SDK build rejected it.) Only the preprocessor removes code from the
// translation unit outright, so it is the right tool for a type that may not exist.

#pragma once

#include <type_traits>
#include <utility>

#include "gfx_normal_compat.h"
#include "mods/svc/gfx.h"

// Every 1.2 name this header needs arrives together, so one probe covers them all.
#if defined(GFX_MAX_COLOR_TARGETS)
#define GFX_COMPAT_HAVE_PASS_TARGETS 1
#else
#define GFX_COMPAT_HAVE_PASS_TARGETS 0
#endif

namespace gfx_compat {

#if GFX_COMPAT_HAVE_PASS_TARGETS
inline constexpr uint32_t kMaxSceneColorTargets = GFX_MAX_COLOR_TARGETS;
#else
// Pre-1.2 SDKs describe at most the scene colour plus a fork-local normal attachment.
inline constexpr uint32_t kMaxSceneColorTargets = 2u;
#endif
static_assert(kMaxSceneColorTargets >= 2u, "the fallback path writes a second colour target");

/// Everything a render pipeline descriptor needs to match the scene pass.
struct ScenePassLayout {
    WGPUColorTargetState color_targets[kMaxSceneColorTargets] = {};
    uint32_t color_target_count = 0;
    WGPUTextureFormat depth_format = WGPUTextureFormat_Undefined;
    uint32_t sample_count = 1;
};

/// Fills `out` with the scene pass's attachment layout. Returns false only when the service call
/// fails outright, which leaves the caller with no valid pipeline to build.
///
/// `ctx`/`gfx` are unused on the fallback path and `info` on the query path; taking all three keeps
/// the call site identical across SDK versions.
template <class Service, class DeviceInfo>
inline bool scene_pass_layout(
    ModContext* ctx, const Service* gfx, const DeviceInfo& info, ScenePassLayout& out) {
    out = ScenePassLayout{};
#if GFX_COMPAT_HAVE_PASS_TARGETS
    (void)info;
    // Not GFX_PASS_TARGETS_INIT: the host overwrites the whole struct from struct_size onward
    // (see gfx_get_pass_targets_impl), so zero-init plus the size is all it reads.
    GfxPassTargets targets{};
    targets.struct_size = sizeof(GfxPassTargets);
    if (gfx->get_pass_targets(ctx, GFX_PASS_SCENE, &targets) != MOD_OK ||
        targets.color_target_count == 0)
    {
        return false;
    }
    out.color_target_count = targets.color_target_count < kMaxSceneColorTargets
        ? targets.color_target_count
        : kMaxSceneColorTargets;
    for (uint32_t i = 0; i < out.color_target_count; ++i) {
        out.color_targets[i] = targets.color_targets[i];
    }
    out.depth_format = targets.depth_format;
    out.sample_count = targets.sample_count;
    return true;
#else
    (void)ctx;
    (void)gfx;
    // Reassemble what the query would have returned. `normal_format` is itself compile-time
    // detected, so on a base with no normal buffer this is a plain single-target layout.
    out.color_targets[0] = WGPU_COLOR_TARGET_STATE_INIT;
    out.color_targets[0].format = info.color_format;
    out.color_target_count = 1;
    const WGPUTextureFormat normal = normal_format(info);
    if (normal != WGPUTextureFormat_Undefined) {
        out.color_targets[1] = WGPU_COLOR_TARGET_STATE_INIT;
        out.color_targets[1].format = normal;
        out.color_targets[1].writeMask = WGPUColorWriteMask_None;
        out.color_target_count = 2;
    }
    out.depth_format = info.depth_format;
    out.sample_count = info.sample_count;
    return true;
#endif
}

}  // namespace gfx_compat
