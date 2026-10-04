// The mod's GPU work. Per insertion point that has techniques:
//
//   game thread    ensure_point() / ensure_depth() create the textures (sizes follow the scene),
//                  push_uniform() carries the depth conversion parameters.
//   render worker  record_point() (a GfxService compute task, outside any render pass):
//                    depth  resolved scene depth -> the shared R32Float depth texture, encoded as
//                           drb_protocol.hpp describes (or cleared to "far" if there is none)
//                    colour resolved scene colour -> the point's colour texture
//                    markers  depth texture -> depth marker, then colour texture -> colour marker,
//                           one texel each. When Dawn writes the colour marker copy into its D3D12
//                           command list, the add-on runs the point's techniques on the colour
//                           texture right there, before the copy.
//                  composite() (a GfxService draw, back in the scene pass): the colour texture is
//                  drawn over the scene, RGB only (the game's alpha is left alone).
//
// Textures are replaced, never resized; replaced ones are kept for kRetireFrames frames so that no
// frame in flight still refers to them.

#pragma once

#include "mods/svc/gfx.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

#include "drb_protocol.hpp"

namespace rsb {

// Mirrored in kUtilityWgsl (struct DepthParams). proj_from_view entries a = m[10], b = m[14],
// c = m[11], d = m[15] (column-major), the camera's near plane, the depth range (the view distance
// that linear depth 1.0 stands for) and whether the camera is valid.
struct DepthParams {
    float a, b, c, d;
    float near_plane, range, valid, pad;
};
static_assert(sizeof(DepthParams) % 16 == 0);

struct PointTargets {
    WGPUTexture color = nullptr;
    WGPUTextureView color_view = nullptr;
    WGPUTexture color_marker = nullptr;
    WGPUTexture depth_marker = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
};

struct DepthTarget {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// What one record_point() call does. Fits a GfxService payload; all handles stay alive (retire
// ring) until the frame is done.
enum RecordFlags : uint32_t {
    kRecordConvertDepth = 1u << 0, // resolved depth -> depth texture
    kRecordClearDepth = 1u << 1,   // no depth this frame: clear the depth texture to "far"
    kRecordColor = 1u << 2,        // resolved colour -> colour texture, then the two markers
};
struct RecordPayload {
    WGPUTextureView src_color;
    WGPUTextureView src_depth;
    WGPUTexture color;
    WGPUTextureView color_view;
    WGPUTexture color_marker;
    WGPUTexture depth;
    WGPUTextureView depth_view;
    WGPUTexture depth_marker;
    WGPUTextureFormat color_format;
    uint32_t uniform_offset;
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(RecordPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

struct CompositePayload {
    WGPUTextureView color_view;
};
static_assert(sizeof(CompositePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

class BridgeGpu {
public:
    static constexpr int kRetireFrames = 4;

    // Game thread, mod_initialize.
    bool init(const GfxDeviceInfo& device, std::string& error);
    // Game thread, mod_shutdown (after the render worker can no longer call in).
    void release();

    // Game thread.
    PointTargets* ensure_point(uint32_t point, uint32_t width, uint32_t height, WGPUTextureFormat format);
    DepthTarget* ensure_depth(uint32_t width, uint32_t height);
    DepthTarget* depth() { return _depth.texture != nullptr ? &_depth : nullptr; }
    void tick_retired();

    // Render worker.
    void record_point(const GfxComputeContext& ctx, const RecordPayload& p);
    void composite(const GfxDrawContext& ctx, const CompositePayload& p);

    // Validation hook for tools/bridge_check.cpp: the blit pipeline for a colour format.
    WGPURenderPipeline blit_pipeline(WGPUTextureFormat format) const;

private:
    struct Retired {
        PointTargets point;
        DepthTarget depth;
        int frames_left;
    };
    static void release_point(PointTargets& t);
    static void release_depth(DepthTarget& t);
    WGPURenderPipeline ensure_composite(const GfxDrawContext& ctx);

    GfxDeviceInfo _device = GFX_DEVICE_INFO_INIT;
    bool _srgbViews = false; // core features: view formats, per-target write masks (Aurora on D3D12)
    WGPUShaderModule _module = nullptr;
    WGPUBindGroupLayout _textureLayout = nullptr;      // binding 0: texture
    WGPUBindGroupLayout _depthLayout = nullptr;        // binding 0: depth texture, 1: DepthParams
    WGPUPipelineLayout _texturePipelineLayout = nullptr;
    WGPUPipelineLayout _depthPipelineLayout = nullptr;
    WGPURenderPipeline _depthPipeline = nullptr;
    std::map<WGPUTextureFormat, WGPURenderPipeline> _blit; // per colour format
    std::map<uint64_t, WGPURenderPipeline> _composites;   // per scene-pass layout key (render worker)

    PointTargets _points[drb::kPointCount];
    DepthTarget _depth;
    std::vector<Retired> _retired;
};

} // namespace rsb
