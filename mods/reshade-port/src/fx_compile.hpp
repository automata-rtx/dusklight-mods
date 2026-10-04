// Compiles one ReShade effect file to WGSL: preprocess, parse, generate per entry point.
//
// Pure CPU work with no game or WebGPU calls, so it runs on a background thread in the mod and
// unchanged in the offline checker (tools/fx_check.cpp).

#pragma once

#include "codegen_wgsl.hpp"
#include "effect_module.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

namespace rsp {

// RESHADE_DEPTH_LINEARIZATION_FAR_PLANE as the depth pass encodes it (see fs_depth in fx_gpu.cpp).
constexpr float kDepthLinearizationFarPlane = 1000.0f;

struct CompileOptions {
    uint32_t width = 0;  // BUFFER_WIDTH: the scene's render resolution
    uint32_t height = 0;
    WGPUTextureFormat color_format = WGPUTextureFormat_BGRA8Unorm;
    std::vector<std::filesystem::path> include_paths;
    // User preprocessor definitions (NAME, VALUE), most specific first: the first definition of a
    // name wins, as in ReShade. The depth definitions are fixed and win over all of them.
    std::vector<std::pair<std::string, std::string>> definitions;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
};

// How the effects of one generation use a texture, merged over every effect that declares it
// (textures with the same unique name are shared, as in ReShade). Decides its storage format.
struct TextureFlags {
    bool storage = false; // some effect binds it as a storage texture
    bool blended = false; // some pass blends into it
};
using TextureFlagMap = std::unordered_map<std::string, TextureFlags>;

struct CompiledEffect {
    std::filesystem::path path;
    std::unique_ptr<reshadefx::codegen> codegen;
    std::unordered_map<std::string, WgslEntryPoint> entry_points; // by unique entry name ('E...')
    std::string errors;   // preprocessor, parser and back-end messages
    bool ok = false;
    // Usage of each texture the effect declares (unique name). parse_effect() fills it from this
    // effect alone; the runtime merges other effects' usage in before assemble_effect().
    std::shared_ptr<TextureFlagMap> texture_flags;
    // Preprocessor definitions the effect tests (NAME, current value), filtered as ReShade filters
    // the list it shows in its overlay. These are what a user may want to override.
    std::vector<std::pair<std::string, std::string>> used_definitions;

    const reshadefx::effect_module& module() const { return codegen->module(); }
};

// The physical texture layout the shader generator and the runtime agree on.
TexturePhysical texture_physical(const reshadefx::texture& tex, WGPUTextureFormat colorFormat, const TextureFlagMap* flags);

// Preprocess and parse (fills module and texture_flags), then generate WGSL for every entry point.
// compile_effect does both; the runtime runs them separately to merge texture usage in between.
bool parse_effect(const std::filesystem::path& path, const CompileOptions& options, CompiledEffect& out);
bool assemble_effect(CompiledEffect& out);
bool compile_effect(const std::filesystem::path& path, const CompileOptions& options, CompiledEffect& out);

// ReShade's BUFFER_COLOR_FORMAT value (a DXGI-compatible format number) for the scene format.
uint32_t reshade_color_format(WGPUTextureFormat format);

} // namespace rsp
