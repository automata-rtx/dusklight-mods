#include "fx_compile.hpp"

#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include <cstdio>

namespace rsp {

uint32_t reshade_color_format(WGPUTextureFormat format) {
    switch (format) {
    case WGPUTextureFormat_RGBA8Unorm: return 28;  // r8g8b8a8_unorm
    case WGPUTextureFormat_BGRA8Unorm: return 87;  // b8g8r8a8_unorm
    case WGPUTextureFormat_RGB10A2Unorm: return 24; // r10g10b10a2_unorm
    case WGPUTextureFormat_RGBA16Float: return 10; // r16g16b16a16_float
    default: return 0;
    }
}

TexturePhysical texture_physical(const reshadefx::texture& tex, WGPUTextureFormat colorFormat, const TextureFlagMap* flags) {
    if (tex.semantic == "COLOR") {
        return physical_for_backbuffer(colorFormat);
    }
    if (tex.semantic == "DEPTH") {
        return physical_for_depth();
    }
    if (!tex.semantic.empty()) {
        // Unknown semantics are bound to an empty R16F texture, as in ReShade: reads return (0, 0, 0, 1).
        return physical_for_format(reshadefx::texture_format::r16f, false);
    }
    TextureFlags f{tex.storage_access, false};
    if (flags != nullptr) {
        if (const auto it = flags->find(tex.unique_name); it != flags->end()) {
            f.storage = f.storage || it->second.storage;
            f.blended = it->second.blended;
        }
    }
    return physical_for_format(tex.format, f.storage, f.blended);
}

bool compile_effect(const std::filesystem::path& path, const CompileOptions& options, CompiledEffect& out) {
    return parse_effect(path, options, out) && assemble_effect(out);
}

bool parse_effect(const std::filesystem::path& path, const CompileOptions& options, CompiledEffect& out) {
    out.path = path;
    out.ok = false;

    reshadefx::preprocessor pp;
    pp.add_include_path(path.parent_path());
    for (const auto& include : options.include_paths) {
        pp.add_include_path(include);
    }

    // The first definition of a name wins, so the fixed depth definitions go first: the depth
    // buffer the effects see is written to match exactly these settings (see fs_depth in fx_gpu.cpp), and a
    // user definition must not override them.
    char farPlane[32];
    std::snprintf(farPlane, sizeof(farPlane), "%.1f", static_cast<double>(kDepthLinearizationFarPlane));
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "1");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_MIRRORED", "0");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
    pp.add_macro_definition("RESHADE_DEPTH_MULTIPLIER", "1");
    pp.add_macro_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", farPlane);
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_SCALE", "1");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_SCALE", "1");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_OFFSET", "0");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_OFFSET", "0");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0");
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0");

    // ReShade's predefined macros. __RENDERER__ reports D3D12 (0xc000): the generated code follows
    // D3D conventions (texture origin, clip space, compute support) and the target is D3D12.
    pp.add_macro_definition("__RESHADE__", "60800");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", std::to_string(options.vendor_id));
    pp.add_macro_definition("__DEVICE__", std::to_string(options.device_id));
    pp.add_macro_definition("__RENDERER__", std::to_string(0xc000));
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("__DUSKLIGHT__", "1");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(options.width));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(options.height));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1"); // sRGB: the game renders in 8-bit gamma space
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", std::to_string(reshade_color_format(options.color_format)));
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

    for (const auto& [name, value] : options.definitions) {
        if (!name.empty()) {
            pp.add_macro_definition(name, value.empty() ? "1" : value);
        }
    }

    // Compatibility macros ReShade's runtime adds for effects written for older versions.
    pp.append_string(
        "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
        "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
        "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
        "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
        "#define tex2Dgather0 tex2DgatherR\n"
        "#define tex2Dgather1 tex2DgatherG\n"
        "#define tex2Dgather2 tex2DgatherB\n"
        "#define tex2Dgather3 tex2DgatherA\n");

    if (!pp.append_file(path)) {
        out.errors = pp.errors();
        return false;
    }
    out.errors = pp.errors(); // warnings
    for (const auto& [name, value] : pp.used_macro_definitions()) {
        if (name.size() < 8 || name[0] == '_' || name.compare(0, 7, "BUFFER_") == 0 ||
            name.compare(0, 8, "RESHADE_") == 0 || name.find("INCLUDE_") != std::string::npos) {
            continue;
        }
        const size_t first = value.find_first_not_of(" \t");
        const size_t last = value.find_last_not_of(" \t");
        out.used_definitions.emplace_back(name, first == std::string::npos ? std::string() : value.substr(first, last - first + 1));
    }

    const WGPUTextureFormat colorFormat = options.color_format;
    out.texture_flags = std::make_shared<TextureFlagMap>();
    WgslHost host;
    host.physical = [colorFormat, flags = out.texture_flags](const reshadefx::texture& tex) {
        return texture_physical(tex, colorFormat, flags.get());
    };
    out.codegen.reset(create_codegen_wgsl(std::move(host)));

    reshadefx::parser parser;
    const bool parsed = parser.parse(pp.output(), out.codegen.get());
    out.errors += parser.errors();
    if (!parsed) {
        return false;
    }

    // Entry points are assembled after parsing, so the formats they are generated for can depend on
    // how the techniques use each texture.
    for (const reshadefx::texture& tex : out.module().textures) {
        if (tex.semantic.empty()) {
            (*out.texture_flags)[tex.unique_name].storage = tex.storage_access;
        }
    }
    for (const reshadefx::technique& tech : out.module().techniques) {
        for (const reshadefx::pass& pass : tech.passes) {
            for (int i = 0; i < 8 && !pass.render_target_names[i].empty(); ++i) {
                if (pass.blend_enable[i]) {
                    (*out.texture_flags)[pass.render_target_names[i]].blended = true;
                }
            }
        }
    }
    out.ok = true;
    return true;
}

bool assemble_effect(CompiledEffect& out) {
    if (out.codegen == nullptr) {
        return false;
    }
    for (const reshadefx::texture& tex : out.module().textures) {
        const auto it = out.texture_flags->find(tex.unique_name);
        if (it != out.texture_flags->end() && it->second.blended &&
            !physical_for_format(tex.format, it->second.storage).blendable &&
            physical_for_format(tex.format, it->second.storage, true).blendable) {
            out.errors += "warning: texture '" + tex.name +
                          "' is stored as 16-bit float because a pass blends into it (the game's device cannot blend 32-bit float)\n";
        }
    }

    bool ok = true;
    for (const auto& [name, type] : out.codegen->module().entry_points) {
        WgslEntryPoint ep;
        std::string errors;
        if (!assemble_wgsl(*out.codegen, name, ep, errors)) {
            out.errors += errors;
            ok = false;
            continue;
        }
        out.entry_points.emplace(name, std::move(ep));
    }
    out.ok = ok;
    return ok;
}

} // namespace rsp
