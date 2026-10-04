#include "fx_images.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

// stb is compiled into this file only, with internal linkage where stb allows it, so its symbols
// can never collide with a copy in the game or another mod.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_DDS_IMPLEMENTATION
#define STBIR_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#include "stb_image_dds.h"
#include "stb_image_resize2.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace rsp {
namespace {

std::string u8(const std::filesystem::path& p) { return path_utf8(p); }

uint16_t float_to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exponent = (x >> 23) & 0xFFu;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent == 0xFFu) { // inf / nan
        return static_cast<uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    }
    int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u) != 0)) {
            ++half;
        }
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(e) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) {
        ++half; // may carry into the exponent, which is the correct rounding
    }
    return static_cast<uint16_t>(sign | half);
}

bool read_file(const std::filesystem::path& path, std::vector<unsigned char>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

// Cube LUT text (Adobe / Resolve). Produces RGBA float, alpha 1.
bool load_cube(const std::vector<unsigned char>& data, std::vector<float>& pixels, int& width, int& height, int& depth) {
    const std::string text(data.begin(), data.end());
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};
    size_t pos = 0;
    bool sized = false;
    size_t index = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        line = line.substr(start);
        const char* p = line.c_str();
        char* next = nullptr;
        if (line.rfind("TITLE", 0) == 0) {
            continue;
        }
        if (line.rfind("DOMAIN_MIN", 0) == 0 || line.rfind("DOMAIN_MAX", 0) == 0) {
            float* target = line[7] == 'M' && line[8] == 'I' ? domainMin : domainMax;
            p += 10;
            for (int c = 0; c < 3; ++c) {
                target[c] = std::strtof(p, &next);
                p = next;
            }
            continue;
        }
        if (line.rfind("LUT_1D_SIZE", 0) == 0 || line.rfind("LUT_3D_SIZE", 0) == 0) {
            if (sized) {
                return false;
            }
            const int n = static_cast<int>(std::strtol(p + 11, nullptr, 10));
            if (n <= 0 || n > 4096) {
                return false;
            }
            const bool three = line[4] == '3';
            width = n;
            height = three ? n : 1;
            depth = three ? n : 1;
            pixels.assign(static_cast<size_t>(width) * height * depth * 4, 0.0f);
            sized = true;
            continue;
        }
        if (!sized || index + 4 > pixels.size()) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            pixels[index++] = std::strtof(p, &next) * (domainMax[c] - domainMin[c]) + domainMin[c];
            p = next;
        }
        pixels[index++] = 1.0f;
    }
    return sized;
}

} // namespace

bool find_file(const std::vector<std::filesystem::path>& searchPaths, std::filesystem::path& path) {
    std::error_code ec;
    if (path.is_absolute()) {
        return std::filesystem::exists(path, ec);
    }
    for (std::filesystem::path searchPath : searchPaths) {
        const bool recursive = searchPath.filename() == "**";
        if (recursive) {
            searchPath = searchPath.parent_path();
        }
        if (std::filesystem::path candidate = searchPath / path; std::filesystem::exists(candidate, ec)) {
            path = std::move(candidate);
            return true;
        }
        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     searchPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (!entry.is_directory(ec)) {
                    continue;
                }
                if (std::filesystem::path candidate = entry.path() / path; std::filesystem::exists(candidate, ec)) {
                    path = std::move(candidate);
                    return true;
                }
            }
        }
    }
    return false;
}

bool load_texture_source(const std::vector<std::filesystem::path>& searchPaths, const std::string& source,
    PoolTexture& out, std::string& errors) {
    using reshadefx::texture_format;
    const reshadefx::texture& tex = out.desc;
    const std::string what = "texture '" + tex.name + "' source '" + source + "'";

    uint32_t components = 0;
    bool isFloat = false;
    switch (tex.format) {
    case texture_format::r8: components = 1; break;
    case texture_format::rg8: components = 2; break;
    case texture_format::rgba8: components = 4; break;
    case texture_format::r32f: components = 1; isFloat = true; break;
    case texture_format::rg32f: components = 2; isFloat = true; break;
    case texture_format::rgba32f: components = 4; isFloat = true; break;
    default:
        errors += "error: " + what + ": loading images into this texture format is not supported (ReShade does not support it either)\n";
        return false;
    }

    std::filesystem::path path = utf8_path(source);
    if (!find_file(searchPaths, path)) {
        errors += "error: " + what + ": file not found in the texture search paths\n";
        return false;
    }
    std::vector<unsigned char> file;
    if (!read_file(path, file) || file.empty()) {
        errors += "error: " + what + ": cannot read '" + u8(path) + "'\n";
        return false;
    }

    // Decode to RGBA, 8-bit or float as ReShade does for the texture's format.
    int width = 0, height = 1, depth = 1, channels = 0;
    std::vector<unsigned char> bytes;
    std::vector<float> floats;
    std::string extension = u8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".cube") {
        if (!isFloat) {
            errors += "error: " + what + ": a Cube LUT file can only be loaded into a floating-point texture\n";
            return false;
        }
        if (!load_cube(file, floats, width, height, depth)) {
            errors += "error: " + what + ": not a valid Cube LUT file\n";
            return false;
        }
    } else if (isFloat) {
        float* data = stbi_loadf_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, STBI_rgb_alpha);
        if (data != nullptr) {
            floats.assign(data, data + static_cast<size_t>(width) * height * 4);
            stbi_image_free(data);
        }
    } else {
        stbi_uc* data = nullptr;
        if (stbi_dds_test_memory(file.data(), static_cast<int>(file.size()))) {
            data = stbi_dds_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &depth, &channels, STBI_rgb_alpha);
        } else {
            data = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, STBI_rgb_alpha);
        }
        if (data != nullptr) {
            bytes.assign(data, data + static_cast<size_t>(width) * height * std::max(depth, 1) * 4);
            stbi_image_free(data);
        }
    }
    if (width <= 0 || (bytes.empty() && floats.empty())) {
        errors += "error: " + what + ": cannot decode '" + u8(path) + "'\n";
        return false;
    }
    depth = std::max(depth, 1);

    const uint32_t tw = out.gpu.width, th = out.gpu.height, td = out.gpu.is_3d ? out.gpu.depth : 1u;
    if (td != static_cast<uint32_t>(depth) || (td != 1 && (tw != static_cast<uint32_t>(width) || th != static_cast<uint32_t>(height)))) {
        errors += "error: " + what + ": image size does not match, and 3D images cannot be resized\n";
        return false;
    }

    // Keep the components the format has (ReShade's collapse step), then resize as ReShade does.
    const size_t srcTexels = static_cast<size_t>(width) * height * depth;
    const auto collapse = [&](auto& v) {
        for (size_t i = 0; i < srcTexels; ++i) {
            for (uint32_t c = 0; c < components; ++c) {
                v[i * components + c] = v[i * 4 + c];
            }
        }
        v.resize(srcTexels * components);
    };
    isFloat ? collapse(floats) : collapse(bytes);

    if (tw != static_cast<uint32_t>(width) || th != static_cast<uint32_t>(height)) {
        const stbir_pixel_layout layout = components == 1 ? STBIR_1CHANNEL : components == 2 ? STBIR_2CHANNEL : STBIR_RGBA;
        if (isFloat) {
            std::vector<float> resized(static_cast<size_t>(tw) * th * components);
            stbir_resize(floats.data(), width, height, 0, resized.data(), static_cast<int>(tw), static_cast<int>(th), 0,
                layout, STBIR_TYPE_FLOAT, STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT);
            floats = std::move(resized);
        } else {
            std::vector<unsigned char> resized(static_cast<size_t>(tw) * th * components);
            stbir_resize(bytes.data(), width, height, 0, resized.data(), static_cast<int>(tw), static_cast<int>(th), 0,
                layout, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT);
            bytes = std::move(resized);
        }
    }

    // Expand into the physical format the texture is stored in (fx_formats.hpp).
    const uint32_t bpt = bytes_per_texel(out.gpu.format);
    const uint32_t physicalComponents = out.phys.physical_components;
    const size_t texels = static_cast<size_t>(tw) * th * td;
    out.bytes_per_row = tw * bpt;
    out.pixels.assign(static_cast<size_t>(out.bytes_per_row) * th * td, 0);
    uint8_t* dst = out.pixels.data();
    switch (out.gpu.format) {
    case WGPUTextureFormat_R8Unorm:
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_RGBA8Unorm:
        for (size_t i = 0; i < texels; ++i) {
            for (uint32_t c = 0; c < physicalComponents; ++c) {
                dst[i * physicalComponents + c] = c < components ? bytes[i * components + c] : (c == 3 ? 255 : 0);
            }
        }
        break;
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RGBA32Float:
        for (size_t i = 0; i < texels; ++i) {
            for (uint32_t c = 0; c < physicalComponents; ++c) {
                const float v = c < components ? floats[i * components + c] : (c == 3 ? 1.0f : 0.0f);
                std::memcpy(dst + (i * physicalComponents + c) * 4, &v, 4);
            }
        }
        break;
    case WGPUTextureFormat_R16Float:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_RGBA16Float:
        for (size_t i = 0; i < texels; ++i) {
            for (uint32_t c = 0; c < physicalComponents; ++c) {
                const uint16_t h = float_to_half(c < components ? floats[i * components + c] : (c == 3 ? 1.0f : 0.0f));
                std::memcpy(dst + (i * physicalComponents + c) * 2, &h, 2);
            }
        }
        break;
    default:
        out.pixels.clear();
        errors += "error: " + what + ": no upload path for this texture's storage format\n";
        return false;
    }
    return true;
}

} // namespace rsp
