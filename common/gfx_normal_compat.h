// Compile-time shim for the two scene-normal fields GfxService 1.3 added:
//
//     GfxResolveDesc::normal       set it to ask resolve_pass for a normal snapshot
//     GfxResolvedTargets::normal   the snapshot: the game's authored view-space normal
//                                  (RGB10A2Unorm in aurora), xyz * 0.5 + 0.5, alpha 1 = valid
//
// Always reach them through request_normal() / resolved_normal() below. Detection is by member
// name (SFINAE), so on an SDK older than 1.3 the request is a no-op and the view is nullptr. That
// quiet fallback is acceptable here because every consumer must already handle a null view at
// runtime; contrast gfx_scene_pass.h, which refuses to build on an SDK it does not recognise.
//
// A null view has three causes, and they should not be reported the same way:
//   not yet          The snapshot latches: the first resolve that asks enables the normal
//                    attachment for the next frame and returns null for this one. Do not report it;
//                    VBAO and SMAA wait kNormalLatchGraceFrames before reporting missing normals.
//   MSAA on          aurora creates the normal buffer only when msaaSamples == 1
//                    (enable_normal_buffer() in lib/webgpu/gpu.cpp; resolve_pass() in
//                    lib/gfx/recording.cpp). GfxDeviceInfo::sample_count shows it. msaaSamples is
//                    fixed at renderer init and Dusklight v2.0.0 never enables MSAA, so this case
//                    only matters for builds that do.
//   no core features The D3D11 / OpenGL ES compatibility renderers (no WebGPU core features)
//                    cannot carry the attachment. Nothing the player can change.
// While the view is null, VBAO draws no AO and SMAA falls back to luma-only edges; both pick the
// normals up once they arrive. The latch also changes the scene pass's attachment count;
// gfx_scene_pass.h has the pipeline rule that follows from that.
//
// Rules:
//  - These accessors are safe for reading a value, never for comparing it with a live one: a shim
//    that answers "absent" invents a difference in a comparison. A guard of exactly that kind once
//    disabled every composite.
//  - No SDK struct has a `normal_format` field; do not add an accessor for one. To ask whether the
//    scene pass carries normals, use gfx_compat::ScenePassLayout::has_normal_attachment.
//  - SFINAE works here because both fields are members of types that exist either way. A missing
//    type or macro cannot be probed like this; see gfx_scene_pass.h.
//
// Background: CONTRIBUTING.md "Rules that have bitten before", docs/normal_buffer_portability.md.

#pragma once

#include <type_traits>
#include <utility>

#include "mods/svc/gfx.h"

namespace gfx_compat {

template <class T, class = void>
struct has_normal : std::false_type {};
template <class T>
struct has_normal<T, std::void_t<decltype(std::declval<const T&>().normal)>> : std::true_type {};

/// Requests (or declines) the normal snapshot on a `GfxResolveDesc`. A no-op when the SDK has no
/// such field; the resolve then returns colour/depth only and resolved_normal() gives nullptr.
template <class T>
inline void request_normal(T& desc, bool want) {
    if constexpr (has_normal<T>::value) {
        desc.normal = want;
    } else {
        (void)desc;
        (void)want;
    }
}

/// The normal snapshot view from a `GfxResolvedTargets`, or `nullptr` when the SDK has no such
/// field or the host returned none this frame. Callers must handle `nullptr` anyway (see above),
/// so the missing-field case needs no branch of its own.
template <class T>
inline WGPUTextureView resolved_normal(const T& targets) {
    if constexpr (has_normal<T>::value) {
        return targets.normal;
    } else {
        (void)targets;
        return nullptr;
    }
}

}  // namespace gfx_compat
