// Compile-time shim for the two scene-normal-buffer fields that are fork-local to our platform
// (GfxService 1.3, `platform-normals-test`):
//
//     GfxResolveDesc::normal      GfxResolvedTargets::normal
//
// Set the first to snapshot the game's authored vertex normals alongside depth; read the second to
// get the resulting view. That pair is the ENTIRE normal-buffer API now, and the entire remaining
// fork delta — everything else the mods use is upstream GfxService 1.2.
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
// silent. Upstream Dusklight has no normal snapshot, so re-platforming onto it removes them, and
// both accessors degrade to "this build has no normal buffer": Graphics Hub falls back to the
// 5-tap depth reconstruction per pixel, exactly as it already does whenever the user has the
// game's Scene Normal Buffer setting switched off. That keeps such a move a one-line
// DUSKLIGHT_VERSION bump instead of a source rescue across five mods.
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
