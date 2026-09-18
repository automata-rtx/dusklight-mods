// Compile-time shim for the two scene-normal-buffer fields added in GfxService 1.3:
//
//     GfxResolveDesc::normal      GfxResolvedTargets::normal
//
// Set the first to snapshot the game's authored vertex normals alongside depth; read the second to
// get the resulting view. That pair is the ENTIRE normal-buffer API.
//
// **THESE ARE UPSTREAM FIELDS NOW.** This header was written when they were fork-local to
// automata-rtx/dusklight-ao, and its whole premise used to be "upstream has no normal snapshot, so
// re-platforming onto it removes them". That is inverted: upstream Dusklight ships them, and the
// fork is retired. The shim still earns its place, for two reasons that outlived the fork —
// an SDK OLDER than 1.3 has neither field, and this is the seam that let the move to upstream be a
// pin bump rather than a source rescue: it detects by member name, so it did not care that upstream
// declared `normal` as a `uint32_t` appended to GfxResolveDesc where the fork had a `bool` tucked
// into the struct's tail padding.
//
// **The snapshot LATCHES.** The first resolve that asks for normals enables the attachment for the
// NEXT frame and hands back a null view for this one; upstream's own `mods/ao_mod` documents this
// and simply returns. So `resolved_normal() == nullptr` means "not yet" at least as often as it
// means "this device cannot". A consumer that treats the first null as a permanent failure will
// announce that on every cold start — see VBAO's `kNormalLatchGraceFrames`.
//
// **A null view has THREE causes and a consumer should not report them the same way:**
//
//   not yet          the latch, above. Never report it.
//   MSAA is on       aurora refuses to create the buffer unless `msaaSamples == 1`, and does not
//                    even record the request otherwise (lib/webgpu/gpu.cpp enable_normal_buffer(),
//                    lib/gfx/recording.cpp resolve_pass). **This is a SETTING the user can change**,
//                    so say so — `GfxDeviceInfo::sample_count` distinguishes it, and it must be
//                    re-queried rather than read from a copy cached at init, since MSAA can change
//                    mid-session. Blaming the GPU here is an actively misleading diagnostic.
//   no core features the adapter lacks WebGPU CoreFeaturesAndLimits — the D3D11 / OpenGL ES
//                    compatibility renderers. Genuinely nothing the user can do.
//
// **The latch also changes the SCENE PASS's shape**, which is a separate hazard with its own fix:
// a pipeline built before the attachment appeared no longer matches the pass and is rejected
// silently. See gfx_scene_pass.h — build scene-pass pipelines lazily, keyed on `layout.key`.
//
// **`GfxDeviceInfo::normal_format` IS GONE — do not reintroduce an accessor for it.** Two earlier
// platforms had that field and two separate bugs came out of it: the retired fork put it at an
// offset upstream independently claimed for `WGPUInstance`, and a `GfxDrawContext::normal_format`
// accessor that degraded to `Undefined` was compared against a live device format, so the guard
// fired on every draw and silently disabled all six composites. To ask "does this build carry
// authored normals", use `gfx_compat::ScenePassLayout::has_normal_attachment`
// (`gfx_scene_pass.h`), which reads the semantic tags on the real scene layout.
//
// **An SDK without these two fields is a supported configuration, not an error** — unlike the
// scene-target-layout query next door, which is an #error precisely because getting it wrong is
// silent. Both accessors degrade to "this build has no normal buffer", and every consumer already
// has to handle that at RUNTIME anyway: the compatibility renderers (D3D11 / OpenGL ES) cannot
// carry the attachment, so a null view is a live case on a fully up-to-date build. A mod that
// needs normals disables itself and says so; SMAA, which only uses them to find extra edges,
// quietly falls back to its luma detector.
//
// **These accessors are safe for reading a value, never for comparing one against a live one.** A
// shim that answers "absent" is indistinguishable from a real "absent" only in a read; in a
// comparison it manufactures a difference that was never there.
//
// Detection is by member name via SFINAE, which works here because both fields are members of
// types that exist either way. A missing TYPE or constant cannot be probed this way — see the note
// in gfx_scene_pass.h on why that one needs the preprocessor.

#pragma once

#include <type_traits>
#include <utility>

#include "mods/svc/gfx.h"

namespace gfx_compat {

template <class T, class = void>
struct has_normal : std::false_type {};
template <class T>
struct has_normal<T, std::void_t<decltype(std::declval<const T&>().normal)>> : std::true_type {};

/// Request (or decline) the normal snapshot on a `GfxResolveDesc`. A no-op when the SDK has no
/// such field — the resolve then simply returns colour/depth, which is what the caller's
/// `resolved_normal() == nullptr` path already handles.
template <class T>
inline void request_normal(T& desc, bool want) {
    if constexpr (has_normal<T>::value) {
        desc.normal = want;
    } else {
        (void)desc;
        (void)want;
    }
}

/// The resolved authored-normal view from a `GfxResolvedTargets`, or `nullptr` when this SDK or
/// host has no normal buffer. Callers already treat `nullptr` as "reconstruct instead", so the
/// absent-field case needs no separate branch.
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
