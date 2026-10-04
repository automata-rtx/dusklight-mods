// How ReShade texture formats are stored on the game's WebGPU device.
//
// Aurora creates its device with no optional features, so some ReShade formats have no direct
// equivalent with the same capabilities. Each one is stored in a format that holds the same values
// and the generated shader code makes up the difference:
//   - 16-bit unorm (R16, RG16, RGBA16) is stored as 32-bit float. Values are not rounded to 16 bits.
//   - Formats a compute shader cannot write (R8, RG8, R16F, RG16F, RGB10A2) are widened when the
//     effect uses the texture as storage. Reads are swizzled back so the effect sees exactly what the
//     narrow format would return (R8 reads as (r, 0, 0, 1)).
//   - RG11B10F cannot be a render target, so it is stored as RGBA16F.
//   - 32-bit float and integer textures cannot be sampled through a filtering sampler; the shader
//     generator filters them itself (see codegen_wgsl.cpp, "emulated sampling").
//   - 32-bit float textures cannot be blended into. A float texture an effect blends into is stored
//     as 16-bit float instead (`blended`): this one loses precision; the effect still runs.
// The shader generator and the runtime both call physical_for_format, so they always agree.

#pragma once

#include "effect_module.hpp"

#include <cstdint>
#include <webgpu/webgpu.h>

namespace rsp {

enum class SampleType : uint8_t { Float, Sint, Uint };

struct TexturePhysical {
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
    SampleType sample_type = SampleType::Float;
    bool filterable = true;  // may be sampled through a filtering sampler
    bool blendable = true;   // may be a render target with blending enabled
    uint8_t logical_components = 4;  // components the ReShade format has
    uint8_t physical_components = 4; // components of `format`
    const char* storage_format = nullptr; // WGSL texel format when bound as storage; null if none
    uint8_t store_quantize_bits = 0; // non-zero: round storage writes to this many unorm bits
    bool srgb_view = false;          // an sRGB view format exists (RGBA8 / BGRA8)
};

inline TexturePhysical physical_for_format(reshadefx::texture_format format, bool storage_access, bool blended = false) {
    using reshadefx::texture_format;
    TexturePhysical p;
    switch (format) {
    case texture_format::r8:
        p.logical_components = 1;
        if (storage_access) {
            p.format = WGPUTextureFormat_RGBA8Unorm;
            p.storage_format = "rgba8unorm";
        } else {
            p.format = WGPUTextureFormat_R8Unorm;
            p.physical_components = 1;
        }
        break;
    case texture_format::r16f:
        p.logical_components = 1;
        if (storage_access) {
            p.format = WGPUTextureFormat_RGBA16Float;
            p.storage_format = "rgba16float";
        } else {
            p.format = WGPUTextureFormat_R16Float;
            p.physical_components = 1;
        }
        break;
    case texture_format::r16:
        p.format = WGPUTextureFormat_R32Float;
        p.logical_components = p.physical_components = 1;
        p.storage_format = "r32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::r32f:
        p.format = WGPUTextureFormat_R32Float;
        p.logical_components = p.physical_components = 1;
        p.storage_format = "r32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::r32u:
        p.format = WGPUTextureFormat_R32Uint;
        p.sample_type = SampleType::Uint;
        p.logical_components = p.physical_components = 1;
        p.storage_format = "r32uint";
        p.filterable = p.blendable = false;
        break;
    case texture_format::r32i:
        p.format = WGPUTextureFormat_R32Sint;
        p.sample_type = SampleType::Sint;
        p.logical_components = p.physical_components = 1;
        p.storage_format = "r32sint";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rg8:
        p.logical_components = 2;
        if (storage_access) {
            p.format = WGPUTextureFormat_RGBA8Unorm;
            p.storage_format = "rgba8unorm";
        } else {
            p.format = WGPUTextureFormat_RG8Unorm;
            p.physical_components = 2;
        }
        break;
    case texture_format::rg16f:
        p.logical_components = 2;
        if (storage_access) {
            p.format = WGPUTextureFormat_RGBA16Float;
            p.storage_format = "rgba16float";
        } else {
            p.format = WGPUTextureFormat_RG16Float;
            p.physical_components = 2;
        }
        break;
    case texture_format::rg16:
        p.format = WGPUTextureFormat_RG32Float;
        p.logical_components = p.physical_components = 2;
        p.storage_format = "rg32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rg32f:
        p.format = WGPUTextureFormat_RG32Float;
        p.logical_components = p.physical_components = 2;
        p.storage_format = "rg32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rgba16f:
        p.format = WGPUTextureFormat_RGBA16Float;
        p.storage_format = "rgba16float";
        break;
    case texture_format::rgba16:
        p.format = WGPUTextureFormat_RGBA32Float;
        p.storage_format = "rgba32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rgba32f:
        p.format = WGPUTextureFormat_RGBA32Float;
        p.storage_format = "rgba32float";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rgba32u:
        p.format = WGPUTextureFormat_RGBA32Uint;
        p.sample_type = SampleType::Uint;
        p.storage_format = "rgba32uint";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rgba32i:
        p.format = WGPUTextureFormat_RGBA32Sint;
        p.sample_type = SampleType::Sint;
        p.storage_format = "rgba32sint";
        p.filterable = p.blendable = false;
        break;
    case texture_format::rgb10a2:
        if (storage_access) {
            p.format = WGPUTextureFormat_RGBA16Float;
            p.storage_format = "rgba16float";
            p.store_quantize_bits = 10;
        } else {
            p.format = WGPUTextureFormat_RGB10A2Unorm;
        }
        break;
    case texture_format::rg11b10f:
        p.format = WGPUTextureFormat_RGBA16Float;
        p.logical_components = 3;
        p.storage_format = "rgba16float";
        break;
    case texture_format::rgba8:
    default:
        p.format = WGPUTextureFormat_RGBA8Unorm;
        p.storage_format = "rgba8unorm";
        p.srgb_view = true;
        break;
    }
    if (blended && !p.blendable && p.sample_type == SampleType::Float) {
        p.filterable = p.blendable = true;
        if (storage_access || p.logical_components == 4) {
            p.format = WGPUTextureFormat_RGBA16Float;
            p.physical_components = 4;
            p.storage_format = "rgba16float";
        } else {
            p.format = p.logical_components == 1 ? WGPUTextureFormat_R16Float : WGPUTextureFormat_RG16Float;
            p.physical_components = p.logical_components;
            p.storage_format = nullptr;
        }
    }
    return p;
}

// The physical description of the scene colour as ReShade's COLOR semantic sees it.
inline TexturePhysical physical_for_backbuffer(WGPUTextureFormat sceneFormat) {
    TexturePhysical p;
    p.format = sceneFormat;
    p.srgb_view = sceneFormat == WGPUTextureFormat_RGBA8Unorm ||
                  sceneFormat == WGPUTextureFormat_BGRA8Unorm;
    return p;
}

// The DEPTH semantic: the runtime's own R32Float copy of the scene depth (written by fs_depth in fx_gpu.cpp).
inline TexturePhysical physical_for_depth() {
    TexturePhysical p;
    p.format = WGPUTextureFormat_R32Float;
    p.logical_components = p.physical_components = 1;
    p.filterable = p.blendable = false;
    return p;
}

// Whether a sampler binding must be declared unfilterable (unfilterable-float or integer texture,
// non-filtering sampler). The generated shader then filters by itself where the effect asked for it.
inline bool binding_is_unfilterable(const TexturePhysical& p) {
    return !p.filterable || p.sample_type != SampleType::Float;
}

} // namespace rsp
