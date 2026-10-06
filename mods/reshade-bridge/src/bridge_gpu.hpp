// The mod's GPU work. Per insertion point that has techniques:
//
//   game thread    ensure_point() / ensure_depth() create the textures (sizes follow the frame and
//                  ReShade's screen), push_uniform() carries the depth conversion and the frame's
//                  place on the screen (MapParams).
//   render worker  record_point() (a GfxService compute task, outside any render pass):
//                    depth  resolved scene depth -> the frame-size R32Float depth texture, encoded as
//                           drb_protocol.hpp describes (or cleared to "far" if there is none), then
//                           one sample per screen pixel of it -> the screen-size depth texture
//                           ReShade gets
//                    colour resolved scene colour -> the point's hand-over texture, scaled to the
//                           screen with an area average (Dusklight's "Area" resampler) and placed
//                           where Dusklight's present puts the frame; then a copy of it -> the
//                           point's input texture, the "before" ReShade's change is measured against
//                    markers  screen depth -> depth marker, then hand-over -> colour marker, one
//                           texel each. When Dawn writes the colour marker copy into its D3D12
//                           command list, the add-on runs the point's techniques on the hand-over
//                           texture right there, before the copy.
//                  composite() (a GfxService draw, back in the scene pass): ReShade's change
//                  (hand-over minus input) is scaled back up to the frame and added to it, RGB only
//                  (the game's alpha is left alone). Edge-aware: each frame pixel takes the change
//                  of the nearby screen pixel whose depth matches its own where they disagree, so
//                  a change made on one side of a silhouette does not bleed onto the other. Simple:
//                  plain bilinear. With `debug` the change is drawn on mid-grey instead.
//
// When the frame is the screen's size the same path runs with an identity mapping: the change
// added back is then exactly ReShade's.
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

// How ReShade's change is scaled back up to the frame (the mod's "Scale-up" option).
enum SpreadMode : uint32_t {
    kSpreadEdgeAware = 0,
    kSpreadSimple = 1,
};

// Mirrored in kScaleWgsl (struct MapParams). Where the frame sits in the screen-size hand-over
// texture, in screen pixels (vp_*: the viewport Dusklight's present uses), the SpreadMode, and
// whether the composite draws the change layer instead of applying it.
struct MapParams {
    float vp_offset[2];
    float vp_size[2];
    uint32_t mode;
    uint32_t debug;
    uint32_t pad[2];
};
static_assert(sizeof(MapParams) % 16 == 0);

// The frame's place on a screen of screen_w x screen_h, exactly as Aurora's
// calculate_present_viewport (lib/webgpu/gpu.cpp) computes it: as wide as the screen, or as tall,
// keeping the frame's aspect ratio, centred. A screen size of 0 means the frame's own size.
MapParams map_params(uint32_t screen_w, uint32_t screen_h, uint32_t frame_w, uint32_t frame_h, SpreadMode mode, bool debug);

struct PointTargets {
    WGPUTexture color = nullptr;          // the hand-over texture; ReShade works on it in place
    WGPUTextureView color_view = nullptr;
    WGPUTexture input = nullptr;          // copy of the hand-over texture before ReShade ran
    WGPUTextureView input_view = nullptr;
    WGPUTexture color_marker = nullptr;
    WGPUTexture depth_marker = nullptr;
    uint32_t width = 0;                   // the screen's size
    uint32_t height = 0;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
};

struct DepthTarget {
    WGPUTexture full = nullptr;           // the frame's size: what the composite compares against
    WGPUTextureView full_view = nullptr;
    WGPUTexture screen = nullptr;         // the screen's size: what ReShade gets
    WGPUTextureView screen_view = nullptr;
    uint32_t full_width = 0;
    uint32_t full_height = 0;
    uint32_t screen_width = 0;
    uint32_t screen_height = 0;
};

// What one record_point() call does. Fits a GfxService payload; all handles stay alive (retire
// ring) until the frame is done.
enum RecordFlags : uint32_t {
    kRecordConvertDepth = 1u << 0, // resolved depth -> both depth textures
    kRecordClearDepth = 1u << 1,   // no depth this frame: clear both depth textures to "far"
    kRecordColor = 1u << 2,        // resolved colour -> hand-over and input textures, then the markers
};
struct RecordPayload {
    WGPUTextureView src_color;
    WGPUTextureView src_depth;
    WGPUTexture color;
    WGPUTextureView color_view;
    WGPUTexture input;
    WGPUTexture color_marker;
    WGPUTexture depth_marker;
    WGPUTexture depth_screen;
    WGPUTextureView depth_full_view;
    WGPUTextureView depth_screen_view;
    WGPUTextureFormat color_format;
    uint32_t depth_uniform_offset; // DepthParams
    uint32_t map_uniform_offset;   // MapParams
    uint32_t flags;
};
static_assert(sizeof(RecordPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

struct CompositePayload {
    WGPUTextureView frame;        // the scene colour snapshot (resolve_pass) the composite adds to
    WGPUTextureView result;       // the hand-over texture after ReShade
    WGPUTextureView input;        // the hand-over texture before ReShade
    WGPUTextureView depth_full;   // null: no depth yet (the composite then spreads like Simple)
    WGPUTextureView depth_screen;
    uint32_t uniform_offset;      // MapParams
    uint32_t pad;
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
    DepthTarget* ensure_depth(uint32_t full_width, uint32_t full_height, uint32_t screen_width, uint32_t screen_height);
    DepthTarget* depth() { return _depth.full != nullptr ? &_depth : nullptr; }
    void tick_retired();

    // Render worker.
    void record_point(const GfxComputeContext& ctx, const RecordPayload& p);
    void composite(const GfxDrawContext& ctx, const CompositePayload& p);

    // Validation hook for tools/bridge_check.cpp: the scale-down pipeline for a colour format.
    WGPURenderPipeline down_pipeline(WGPUTextureFormat format) const;

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
    WGPUShaderModule _utilityModule = nullptr; // depth conversion
    WGPUShaderModule _scaleModule = nullptr;   // scale down, composite
    WGPUBindGroupLayout _depthLayout = nullptr;     // 0: depth texture, 1: DepthParams
    WGPUBindGroupLayout _downLayout = nullptr;      // 0: source texture, 1: MapParams
    WGPUBindGroupLayout _compositeLayout = nullptr; // 0: frame, 1: MapParams, 2: result, 3: input, 4/5: depth
    WGPUPipelineLayout _depthPipelineLayout = nullptr;
    WGPUPipelineLayout _downPipelineLayout = nullptr;
    WGPUPipelineLayout _compositePipelineLayout = nullptr;
    WGPURenderPipeline _depthPipeline = nullptr;     // raw depth -> encoded depth, frame size
    WGPURenderPipeline _depthDownPipeline = nullptr; // frame-size depth -> screen-size depth
    std::map<WGPUTextureFormat, WGPURenderPipeline> _down; // per colour format
    std::map<uint64_t, WGPURenderPipeline> _composites;   // per scene-pass layout key (render worker)
    // 1x1 "far" depth for a composite before any depth exists (WebGPU textures start zeroed).
    WGPUTexture _farDepth = nullptr;
    WGPUTextureView _farDepthView = nullptr;

    PointTargets _points[drb::kPointCount];
    DepthTarget _depth;
    std::vector<Retired> _retired;
};

} // namespace rsb
