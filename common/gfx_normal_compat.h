// Compile-time shim for the thin g-buffer fields the mod SDK gained in `platform-gbuffer-test`:
//
//     GfxDeviceInfo::normal_format      GfxResolveDesc::normal
//     GfxDrawContext::normal_format     GfxResolvedTargets::normal
//
// **An SDK without those fields is a supported configuration, not an error.** Upstream Dusklight
// has no normal buffer of any kind, so re-platforming onto it (or onto any base that predates the
// thin g-buffer) removes them. Every accessor here degrades to "this build has no normal buffer",
// which is precisely the state the mods already handle at runtime on `platform-v2-test`: Graphics
// Hub falls back to the 5-tap depth reconstruction per pixel, and the scene-pass composites drop
// back to a single colour target. The result is that such a re-platform stays a one-line
// DUSKLIGHT_VERSION bump instead of a source rescue across five mods.
//
// Use these accessors instead of touching the fields directly. A direct `g_deviceInfo.normal_format`
// compiles today and breaks on the next platform move; `gfx_compat::normal_format(g_deviceInfo)`
// does not. See CLAUDE.md, "The ABI pin", and `docs/normal_buffer_portability.md`.
//
// Detection is by member name, so the same helper serves both structs that spell a field the same
// way (GfxDeviceInfo and GfxDrawContext both use `normal_format`).

#pragma once

#include <type_traits>
#include <utility>

#include "mods/svc/gfx.h"

namespace gfx_compat {

template <class T, class = void>
struct has_normal_format : std::false_type {};
template <class T>
struct has_normal_format<T, std::void_t<decltype(std::declval<const T&>().normal_format)>>
    : std::true_type {};

template <class T, class = void>
struct has_normal : std::false_type {};
template <class T>
struct has_normal<T, std::void_t<decltype(std::declval<const T&>().normal)>> : std::true_type {};

/// Format of the host's authored-normal attachment, or `Undefined` when this SDK (or this game
/// build) has none. Accepts `GfxDeviceInfo` and `GfxDrawContext`.
///
/// Undefined is the correct "absent" answer for both callers: a scene-pass pipeline compares it
/// against Undefined to decide whether to declare the second colour target, and a draw callback
/// compares the context's value against the device's to detect a mid-session layout change. When
/// the fields are gone both sides read Undefined, so the comparison still agrees and no draw is
/// skipped.
template <class T>
inline WGPUTextureFormat normal_format(const T& v) {
    if constexpr (has_normal_format<T>::value) {
        return v.normal_format;
    } else {
        (void)v;
        return WGPUTextureFormat_Undefined;
    }
}

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
