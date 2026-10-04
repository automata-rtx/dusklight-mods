// Image files for textures declared with a `source` annotation.
//
// Mirrors ReShade's loader: PNG, JPEG, BMP, TGA, PSD, GIF, HDR and PIC through stb_image, DDS
// through stb_image_dds, and .cube LUT files (float textures only). Only the formats ReShade can
// fill from a file are accepted (R8, RG8, RGBA8, R32F, RG32F, RGBA32F). A 2D image of a different
// size is resized to the texture as ReShade resizes it; 3D images must match exactly.
//
// Pure CPU work: runs on the build thread.

#pragma once

#include "fx_gpu.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace rsp {

// UTF-8 <-> path, without the C++20 deprecation of u8path.
inline std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A search path ending in "**" also searches every directory below it (ReShade's convention).
bool find_file(const std::vector<std::filesystem::path>& searchPaths, std::filesystem::path& path);

// Fills out.pixels / out.bytes_per_row for out.gpu (already created) from the image `source`.
bool load_texture_source(const std::vector<std::filesystem::path>& searchPaths, const std::string& source,
    PoolTexture& out, std::string& errors);

} // namespace rsp
