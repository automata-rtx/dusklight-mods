// GPU side of the ReShade port: textures, pipelines and pass execution on a WebGPU device.
//
// Uses only webgpu.h, no mod services, so the offline checker (tools/fx_check.cpp) runs exactly
// this code on Dawn's null backend.
//
// Threads in the mod: init() and build() run on the build thread; record_*(), upload_pending()
// and write_uniforms() run on the render worker inside a GfxService compute callback. A GpuShared
// and an EffectGpu are immutable once built, so the render worker reads them without locks. Objects
// are released by dropping references (never wgpuTextureDestroy), so a texture an in-flight frame
// still uses stays valid.
//
// Everything that can fail is created inside error scopes (ScopedErrors): Aurora treats any
// uncaptured WebGPU error as fatal. Aurora's release builds also turn Dawn's validation off
// (skip_validation), so the mod first builds every effect on a private null-backend device with
// validation on (see the runtime) and only then on the game's device.

#pragma once

#include "fx_compile.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

namespace rsp {

struct GpuApi {
    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
};

// Captures validation, out-of-memory and internal errors between construction and finish().
class ScopedErrors {
public:
    ScopedErrors(const GpuApi& api, const char* what);
    ~ScopedErrors();
    // Returns true if no error was captured; appends a message to `errors` otherwise.
    bool finish(std::string& errors);

private:
    const GpuApi& _api;
    const char* _what;
    bool _finished = false;
};

struct GpuTexture {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;      // all levels
    WGPUTextureView view_srgb = nullptr; // all levels, sRGB format (RGBA8 / BGRA8 only)
    std::vector<WGPUTextureView> level_views;      // one level each (render targets, storage, mipgen)
    std::vector<WGPUTextureView> level_views_srgb; // one level each, sRGB
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    bool is_3d = false;

    bool create(const GpuApi& api, const char* label, WGPUTextureFormat format, uint32_t width,
        uint32_t height, uint32_t depth, uint32_t levels, bool is3d, WGPUTextureUsage usage, bool srgbViews);
    void release();
};

// Textures the runtime owns for every effect.
struct FrameTextures {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureFormat color_format = WGPUTextureFormat_Undefined;
    GpuTexture backbuffer; // effects render "to the back buffer" here
    GpuTexture color_copy; // what the COLOR semantic samples; refreshed as ReShade refreshes it
    GpuTexture depth;      // the DEPTH semantic, in the encoding ReShade.fxh expects (fs_depth in fx_gpu.cpp)
    GpuTexture stencil;    // ReShade's effect stencil buffer (Depth24PlusStencil8)
    // Zero-filled 1x1 placeholders. `dummy` stands in for semantics nobody provides (ReShade binds an
    // empty R16F texture, which reads as (0, 0, 0, 1)); the others replace a sampled texture that the
    // same pass writes, which WebGPU forbids.
    GpuTexture dummy;
    GpuTexture dummy_3d;
    GpuTexture dummy_uint;
    GpuTexture dummy_sint;
    uint64_t generation = 0; // which runtime generation these belong to

    WGPUTextureView placeholder(SampleType type, bool is3d) const;

    bool create(const GpuApi& api, uint32_t width, uint32_t height, WGPUTextureFormat colorFormat, std::string& errors);
    void release();
};

// A texture declared by an effect. Textures with the same unique name are shared by all effects,
// as in ReShade.
struct PoolTexture {
    reshadefx::texture desc;
    TexturePhysical phys;
    GpuTexture gpu;
    uint32_t refs = 0;
    // Pixels from a `source` annotation, uploaded on the render worker before first use.
    std::vector<uint8_t> pixels;
    uint32_t bytes_per_row = 0;
    std::atomic<bool> needs_upload{false};
};

// Owned by one runtime generation and only modified by the build thread. Entries are never
// removed while the pool lives, so EffectGpu keeps plain pointers to them.
struct TexturePool {
    std::map<std::string, std::unique_ptr<PoolTexture>> textures;
    ~TexturePool() { release_all(); }
    void release_all();
};

// Fills `out.pixels` (tightly packed rows of out.gpu.format at out.desc size, level 0) from an image
// file named by a `source` annotation. Returns false (and an error) if the file cannot be used.
using SourceLoader = std::function<bool(const std::string& source, PoolTexture& out, std::string& errors)>;

struct DepthParams {
    // clip.z = a * view.z + b and clip.w = c * view.z + d (rows 2 and 3 of proj_from_view).
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 1.0f;
    float near_plane = 1.0f, far_plane = 1000.0f;
    float valid = 0.0f; // 0: no camera this frame, pass raw depth through
    float pad = 0.0f;
};

// Pipelines shared by all effects: mip generation, back-buffer blit, depth conversion, samplers.
class GpuShared {
public:
    bool init(const GpuApi& api, std::string& errors);
    void release();

    // Build thread. init() creates the mip and blit pipelines for every format up front, so the
    // render worker only ever reads these maps.
    bool has_mipgen(WGPUTextureFormat format) const { return _mipgen.count(format) != 0; }
    bool has_blit(WGPUTextureFormat format) const { return _blit.count(format) != 0; }
    WGPUSampler sampler(const reshadefx::sampler_desc& desc, bool unfilterable, std::string& errors);

    // Render worker.
    void blit_to_backbuffer(WGPUCommandEncoder encoder, WGPUTextureView snapshot, const FrameTextures& frame) const;
    void convert_depth(WGPUCommandEncoder encoder, WGPUTextureView rawDepth, const FrameTextures& frame, const DepthParams& params) const;
    void clear_depth(WGPUCommandEncoder encoder, const FrameTextures& frame) const;
    void generate_mips(WGPUCommandEncoder encoder, const GpuTexture& texture) const;

    const GpuApi& api() const { return _api; }
    // For the mod's scene-pass composite: vs_fullscreen + fs_copy over one unfilterable texture.
    WGPUShaderModule utility_module() const { return _utility; }
    WGPUBindGroupLayout single_texture_layout() const { return _singleTextureLayout; }
    WGPUPipelineLayout single_texture_pipeline_layout() const { return _singleTexturePipelineLayout; }

private:
    GpuApi _api;
    WGPUShaderModule _utility = nullptr;
    WGPUBindGroupLayout _singleTextureLayout = nullptr; // one unfilterable 2D texture
    WGPUBindGroupLayout _depthLayout = nullptr;         // raw depth + parameters
    WGPUPipelineLayout _singleTexturePipelineLayout = nullptr;
    WGPUPipelineLayout _depthPipelineLayout = nullptr;
    WGPURenderPipeline _depthPipeline = nullptr;
    WGPUBuffer _depthParams = nullptr;
    std::map<WGPUTextureFormat, WGPURenderPipeline> _mipgen;
    std::map<WGPUTextureFormat, WGPURenderPipeline> _blit;
    std::map<uint64_t, WGPUSampler> _samplers;
};

// Per technique-run state on the render worker.
struct RecordState {
    WGPUCommandEncoder encoder = nullptr;
    const FrameTextures* frame = nullptr;
    const GpuShared* shared = nullptr;
};

class EffectGpu {
public:
    ~EffectGpu();

    // Techniques whose passes all built are usable; build() returns false if any pass failed.
    bool build(const GpuApi& api, GpuShared& shared, const FrameTextures& frame, std::shared_ptr<TexturePool> pool,
        const CompiledEffect& effect, const SourceLoader& loader, std::string& errors);
    void release();

    // Render worker. upload_pending writes images loaded by the build (and regenerates their mips)
    // the first time it runs after a build.
    void upload_pending(RecordState& state) const;
    void write_uniforms(WGPUQueue queue, const uint8_t* data, size_t size) const;
    void record_technique(RecordState& state, size_t technique) const;

    size_t technique_count() const { return _techniques.size(); }
    bool technique_built(size_t t) const;
    uint32_t uniform_size() const { return _uniformSize; }
    uint64_t generation() const { return _generation; }

private:
    struct Pass {
        WGPURenderPipeline render = nullptr;
        WGPUComputePipeline compute = nullptr;
        WGPUBindGroup groups[3] = {};
        WGPUTextureView targets[8] = {};
        uint32_t target_count = 0; // 0 = the back buffer
        bool srgb_write = false;
        bool clear = false;
        bool stencil = false;
        uint32_t stencil_ref = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t dispatch[3] = {1, 1, 1};
        uint32_t vertex_count = 3;
        std::vector<const PoolTexture*> mip_targets; // regenerate mips after the pass
    };

    GpuApi _api;
    uint64_t _generation = 0;
    std::shared_ptr<TexturePool> _pool;
    std::vector<PoolTexture*> _poolTextures; // the pool entries this effect declares
    std::vector<std::vector<Pass>> _techniques;
    std::vector<WGPUShaderModule> _modules;
    std::vector<WGPUBindGroupLayout> _layouts;
    std::vector<WGPUPipelineLayout> _pipelineLayouts;
    std::vector<WGPUTextureView> _views; // partial-level views made for passes that write lower levels
    WGPUBuffer _uniforms = nullptr;
    uint32_t _uniformSize = 0;
};

// Format name used in WGSL storage declarations -> WebGPU format.
WGPUTextureFormat storage_format_from_wgsl(const std::string& name);

// Bytes per texel of the formats physical_for_format produces; 0 for anything else.
uint32_t bytes_per_texel(WGPUTextureFormat format);

} // namespace rsp
