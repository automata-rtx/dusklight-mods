// ReShade FX -> WGSL code generation.
//
// A code-generation back-end for ReShade's own effect compiler (third_party/reshadefx). The parser
// calls into it exactly as it calls ReShade's HLSL, GLSL and SPIR-V back-ends; this one emits WGSL,
// the only shader language the game's WebGPU device accepts (SPIR-V input is disabled by Aurora).
//
// Each entry point is assembled into its own WGSL module. Bindings:
//   @group(0) @binding(0)       the effect's uniform block
//   @group(1) @binding(2b)      texture of the sampler at ReShade binding b
//   @group(1) @binding(2b + 1)  that sampler
//   @group(2) @binding(b)       storage texture at ReShade storage binding b
// `b` is the entry_point_binding from the pass's sampler_bindings / storage_bindings, as in ReShade.

#pragma once

#include "effect_codegen.hpp"
#include "fx_formats.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rsp {

struct WgslSamplerBinding {
    uint32_t binding = 0;       // ReShade entry-point binding b
    uint32_t sampler_index = 0; // index into effect_module::samplers
    SampleType sample_type = SampleType::Float;
    bool unfilterable = false;  // unfilterable-float / integer texture with a non-filtering sampler
    bool is_3d = false;
};

enum class StorageAccess : uint8_t { Write, Read, ReadWrite };

struct WgslStorageBinding {
    uint32_t binding = 0;
    uint32_t storage_index = 0; // index into effect_module::storages
    StorageAccess access = StorageAccess::Write;
    SampleType sample_type = SampleType::Float;
    bool is_3d = false;
    std::string format;         // WGSL texel format
};

struct WgslEntryPoint {
    std::string code;
    reshadefx::shader_type type = reshadefx::shader_type::unknown;
    std::vector<WgslSamplerBinding> samplers;
    std::vector<WgslStorageBinding> storages;
    uint32_t fragment_outputs = 0;         // bit i: the shader writes @location(i)
    SampleType fragment_output_types[8] = {};
    uint32_t workgroup_size[3] = {1, 1, 1};
};

struct WgslHost {
    // Physical storage of a texture. For semantic textures ("COLOR", "DEPTH", ...) the host decides.
    std::function<TexturePhysical(const reshadefx::texture&)> physical;
};

reshadefx::codegen* create_codegen_wgsl(WgslHost host);

// Assembles one entry point (unique name starting with 'E', as listed in effect_module::entry_points).
bool assemble_wgsl(const reshadefx::codegen& codegen, const std::string& entryPoint,
    WgslEntryPoint& out, std::string& errors);

} // namespace rsp
