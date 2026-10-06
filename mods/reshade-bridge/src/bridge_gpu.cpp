#include "bridge_gpu.hpp"

#include "gfx_scene_pass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace rsb {
namespace {

WGPUStringView sv(const char* s) { return {s, WGPU_STRLEN}; }

// The fullscreen triangle, shared by both modules.
const char* kVertexWgsl = R"(
@vertex fn vs_fullscreen(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
	let x = f32((i << 1u) & 2u);
	let y = f32(i & 2u);
	return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}
)";

// The depth conversion, at the frame's size. It is the inverse of ReShade.fxh's GetLinearizedDepth
// under drb::kDepthDefinitions (reversed, not logarithmic, far plane F): view distance z -> linear
// depth L = (z - near) / (range - near), clamped to [0, 1], stored as 1 - L * F / (1 + L * (F - 1)),
// which that function maps back to L. The view distance comes from the game's reversed-Z depth r
// through the projection: vz = (b - r * d) / (r * c - a) is view-space z, negative in front of the
// camera. Sky (r = 0) lands on L = 1, stored as 0.
const char* kUtilityWgsl = R"(
@group(0) @binding(0) var src : texture_2d<f32>;
struct DepthParams { a : f32, b : f32, c : f32, d : f32, near_plane : f32, range : f32, valid : f32, pad : f32 }
@group(0) @binding(1) var<uniform> dp : DepthParams;

@fragment fn fs_depth(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	let r = textureLoad(src, vec2<i32>(p.xy), 0).x;
	if (dp.valid == 0.0) {
		return vec4<f32>(r, 0.0, 0.0, 1.0);
	}
	let vz = (dp.b - r * dp.d) / (r * dp.c - dp.a);
	let l = clamp((-vz - dp.near_plane) / max(dp.range - dp.near_plane, 0.001), 0.0, 1.0);
	let f = FAR_PLANE;
	return vec4<f32>(1.0 - l * f / (1.0 + l * (f - 1.0)), 0.0, 0.0, 1.0);
}
)";

// Scaling between the frame (the game's internal resolution) and the hand-over textures (ReShade's
// screen size). mp.vp_* is the rectangle of the hand-over texture the frame maps to, exactly where
// Dusklight's present puts it (black bars around it when the aspect ratios differ).
const char* kScaleWgsl = R"(
// Mirrors rsb::MapParams.
struct MapParams {
	vp_offset : vec2<f32>,
	vp_size : vec2<f32>,
	mode : u32,
	debug : u32,
	pad0 : u32,
	pad1 : u32,
}

// Scale-down passes: t0 is the source (the scene colour snapshot, or the frame-size depth).
// Composite: t0 is the scene colour snapshot the change is added to.
@group(0) @binding(0) var t0 : texture_2d<f32>;
@group(0) @binding(1) var<uniform> mp : MapParams;
@group(0) @binding(2) var t_result : texture_2d<f32>;
@group(0) @binding(3) var t_input : texture_2d<f32>;
@group(0) @binding(4) var t_depth_full : texture_2d<f32>;
@group(0) @binding(5) var t_depth_screen : texture_2d<f32>;

fn in_frame(p : vec2<f32>) -> bool {
	return all(p >= mp.vp_offset) && all(p < mp.vp_offset + mp.vp_size);
}

// Scene colour -> hand-over: the average over the area of the frame that a screen pixel covers,
// each frame pixel weighted by how much of it is covered (Aurora's "Area" resampler). When the
// frame is smaller than the screen the area is one frame pixel wide, which is bilinear
// interpolation. Outside the frame: black, as on the screen.
@fragment fn fs_down(@builtin(position) pos : vec4<f32>) -> @location(0) vec4<f32> {
	if (!in_frame(pos.xy)) {
		return vec4<f32>(0.0, 0.0, 0.0, 1.0);
	}
	let size = vec2<f32>(textureDimensions(t0));
	let scale = size / mp.vp_size;
	let centre = (pos.xy - mp.vp_offset) * scale;
	let half_extent = 0.5 * max(scale, vec2<f32>(1.0));
	let lo = clamp(centre - half_extent, vec2<f32>(0.0), size);
	let hi = clamp(centre + half_extent, vec2<f32>(0.0), size);
	let first = vec2<i32>(floor(lo));
	let last = min(vec2<i32>(ceil(hi)), first + vec2<i32>(16));
	var sum = vec4<f32>(0.0);
	var total = 0.0;
	for (var y = first.y; y < last.y; y = y + 1) {
		let wy = max(min(hi.y, f32(y) + 1.0) - max(lo.y, f32(y)), 0.0);
		for (var x = first.x; x < last.x; x = x + 1) {
			let wx = max(min(hi.x, f32(x) + 1.0) - max(lo.x, f32(x)), 0.0);
			sum = sum + wx * wy * textureLoad(t0, vec2<i32>(x, y), 0);
			total = total + wx * wy;
		}
	}
	return sum / max(total, 1e-6);
}

// Frame-size depth -> screen-size depth: the frame pixel under each screen pixel's centre, never
// an average (an average across a silhouette is a surface that is not there). Outside the frame:
// far (stored 0).
@fragment fn fs_depth_down(@builtin(position) pos : vec4<f32>) -> @location(0) vec4<f32> {
	if (!in_frame(pos.xy)) {
		return vec4<f32>(0.0, 0.0, 0.0, 1.0);
	}
	let size = textureDimensions(t0);
	let uv = (pos.xy - mp.vp_offset) / mp.vp_size;
	let texel = min(vec2<u32>(uv * vec2<f32>(size)), size - vec2<u32>(1u));
	return vec4<f32>(textureLoad(t0, texel, 0).x, 0.0, 0.0, 1.0);
}

// Linear depth from the stored encoding (fs_depth, inverted): 0 at the near plane, 1 at the depth
// range and beyond.
fn linear_depth(stored : f32) -> f32 {
	let u = 1.0 - stored;
	return u / (FAR_PLANE - u * (FAR_PLANE - 1.0));
}

// What ReShade changed at hand-over pixel c.
fn change_at(c : vec2<i32>) -> vec3<f32> {
	return textureLoad(t_result, c, 0).rgb - textureLoad(t_input, c, 0).rgb;
}

// ReShade's change at frame pixel `pos` (a pixel centre, in frame pixels) of a frame of `size`.
fn spread(pos : vec2<f32>, size : vec2<f32>) -> vec3<f32> {
	// The same point in screen pixels, and the four screen pixel centres around it (kept inside the
	// frame's rectangle).
	let q = mp.vp_offset + pos / size * mp.vp_size - vec2<f32>(0.5);
	let base = floor(q);
	let f = q - base;
	let b = vec2<i32>(base);
	let lo = vec2<i32>(mp.vp_offset);
	let hi = vec2<i32>(mp.vp_offset + mp.vp_size) - vec2<i32>(1);
	var taps = array<vec2<i32>, 4>(
		clamp(b, lo, hi),
		clamp(b + vec2<i32>(1, 0), lo, hi),
		clamp(b + vec2<i32>(0, 1), lo, hi),
		clamp(b + vec2<i32>(1, 1), lo, hi));
	var weights = array<f32, 4>(
		(1.0 - f.x) * (1.0 - f.y),
		f.x * (1.0 - f.y),
		(1.0 - f.x) * f.y,
		f.x * f.y);
	var changes : array<vec3<f32>, 4>;
	var blended = vec3<f32>(0.0);
	for (var i = 0; i < 4; i = i + 1) {
		changes[i] = change_at(taps[i]);
		blended = blended + weights[i] * changes[i];
	}
	if (mp.mode == SPREAD_SIMPLE) {
		return blended;
	}

	// Edge-aware. Where the depths of the four screen pixels agree with this frame pixel's own, the
	// bilinear blend; where one of them disagrees (a silhouette runs between them), the change of
	// the screen pixel whose depth is closest to this pixel's, so a change made on one side of an
	// edge (ambient occlusion on a shoulder) does not bleed onto the other (the wall behind it).
	let full_size = textureDimensions(t_depth_full);
	let own = min(vec2<u32>(pos / size * vec2<f32>(full_size)), full_size - vec2<u32>(1u));
	let z = linear_depth(textureLoad(t_depth_full, own, 0).x);
	let screen_last = vec2<i32>(textureDimensions(t_depth_screen)) - vec2<i32>(1);
	var worst = 0.0;
	var nearest = 0;
	var nearest_diff = 3.0e38;
	for (var i = 0; i < 4; i = i + 1) {
		let zi = linear_depth(textureLoad(t_depth_screen, min(taps[i], screen_last), 0).x);
		let diff = abs(zi - z) / max(z, 1e-4);
		if (weights[i] > 1e-3) { // (float rounding leaves dust weights on far taps)
			worst = max(worst, diff);
		}
		if (diff < nearest_diff) {
			nearest_diff = diff;
			nearest = i;
		}
	}
	if (worst <= EDGE_THRESHOLD) {
		return blended;
	}
	return changes[nearest];
}

// ReShade's change, scaled up and added to the frame. Debug: the change alone, on mid-grey (darker
// where ReShade darkened, brighter where it brightened), at the strength it is applied with.
@fragment fn fs_composite(@builtin(position) pos : vec4<f32>) -> @location(0) vec4<f32> {
	let size = vec2<f32>(textureDimensions(t0));
	let change = spread(pos.xy, size);
	if (mp.debug != 0u) {
		return vec4<f32>(vec3<f32>(0.5) + change, 1.0);
	}
	let frame = textureLoad(t0, vec2<i32>(pos.xy), 0);
	return vec4<f32>(frame.rgb + change, frame.a);
}
)";

// Relative depth difference above which the edge-aware scale-up treats two pixels as lying on
// different surfaces (5%: a character a metre in front of a wall ten metres away is 10%).
constexpr const char* kEdgeThreshold = "0.05";

void replace_all(std::string& s, const std::string& token, const std::string& value) {
    for (size_t at = s.find(token); at != std::string::npos; at = s.find(token, at + value.size())) {
        s.replace(at, token.size(), value);
    }
}

WGPUShaderModule make_module(WGPUDevice device, std::string code, const char* label) {
    char farPlane[32];
    std::snprintf(farPlane, sizeof(farPlane), "%.1f", static_cast<double>(drb::kDepthFarPlane));
    replace_all(code, "FAR_PLANE", farPlane);
    replace_all(code, "EDGE_THRESHOLD", kEdgeThreshold);
    replace_all(code, "SPREAD_SIMPLE", std::to_string(static_cast<uint32_t>(kSpreadSimple)) + "u");
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {code.data(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    md.label = sv(label);
    return wgpuDeviceCreateShaderModule(device, &md);
}

WGPUTextureFormat srgb_view_format(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
    case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
    default: return WGPUTextureFormat_Undefined;
    }
}

WGPURenderPipeline make_fullscreen_pipeline(WGPUDevice device, WGPUShaderModule module, WGPUPipelineLayout layout,
    const char* fragment, WGPUTextureFormat format, const char* label) {
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = module;
    fs.entryPoint = sv(fragment);
    fs.targetCount = 1;
    fs.targets = &target;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = sv(label);
    rpd.layout = layout;
    rpd.vertex.module = module;
    rpd.vertex.entryPoint = sv("vs_fullscreen");
    rpd.fragment = &fs;
    return wgpuDeviceCreateRenderPipeline(device, &rpd);
}

WGPUTexture make_texture(WGPUDevice device, const char* label, uint32_t w, uint32_t h, WGPUTextureFormat format,
    WGPUTextureUsage usage, WGPUTextureFormat viewFormat = WGPUTextureFormat_Undefined) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.label = sv(label);
    td.size = {w, h, 1};
    td.format = format;
    td.usage = usage;
    if (viewFormat != WGPUTextureFormat_Undefined) {
        td.viewFormatCount = 1;
        td.viewFormats = &viewFormat;
    }
    return wgpuDeviceCreateTexture(device, &td);
}

WGPUTextureView make_view(WGPUTexture texture, WGPUTextureFormat format) {
    if (texture == nullptr) {
        return nullptr;
    }
    WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    vd.format = format;
    return wgpuTextureCreateView(texture, &vd);
}

WGPUBindGroupLayoutEntry texture_entry(uint32_t binding) {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = WGPUShaderStage_Fragment;
    e.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    e.texture.viewDimension = WGPUTextureViewDimension_2D;
    return e;
}

WGPUBindGroupLayoutEntry uniform_entry(uint32_t binding, uint64_t size) {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = WGPUShaderStage_Fragment;
    e.buffer.type = WGPUBufferBindingType_Uniform;
    e.buffer.minBindingSize = size;
    return e;
}

WGPUBindGroupEntry texture_binding(uint32_t binding, WGPUTextureView view) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.textureView = view;
    return e;
}

WGPUBindGroupEntry uniform_binding(uint32_t binding, WGPUBuffer buffer, uint32_t offset, uint64_t size) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.buffer = buffer;
    e.offset = offset;
    e.size = size;
    return e;
}

WGPUBindGroupLayout make_layout(WGPUDevice device, const WGPUBindGroupLayoutEntry* entries, size_t count) {
    WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bld.entryCount = count;
    bld.entries = entries;
    return wgpuDeviceCreateBindGroupLayout(device, &bld);
}

WGPUPipelineLayout make_pipeline_layout(WGPUDevice device, WGPUBindGroupLayout layout) {
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &layout;
    return wgpuDeviceCreatePipelineLayout(device, &pld);
}

template <class T>
void release_handle(T& handle, void (*fn)(T)) {
    if (handle != nullptr) {
        fn(handle);
        handle = nullptr;
    }
}

void copy_texel(WGPUCommandEncoder encoder, WGPUTexture src, WGPUTexture dst) {
    WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    from.texture = src;
    WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    to.texture = dst;
    const WGPUExtent3D one = {1, 1, 1};
    wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &one);
}

} // namespace

MapParams map_params(uint32_t screen_w, uint32_t screen_h, uint32_t frame_w, uint32_t frame_h, SpreadMode mode, bool debug) {
    MapParams m{};
    m.mode = mode;
    m.debug = debug ? 1u : 0u;
    if (screen_w == 0 || screen_h == 0) {
        screen_w = frame_w;
        screen_h = frame_h;
    }
    if (frame_w == 0 || frame_h == 0) {
        m.vp_size[0] = static_cast<float>(screen_w);
        m.vp_size[1] = static_cast<float>(screen_h);
        return m;
    }
    // Aurora's calculate_present_viewport, step for step.
    uint32_t w = screen_w;
    uint32_t h = std::min<uint32_t>(screen_h,
        std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(w) * frame_h / frame_w))));
    if (h == screen_h) {
        w = std::min<uint32_t>(screen_w,
            std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(h) * frame_w / frame_h))));
    }
    m.vp_offset[0] = static_cast<float>((screen_w - w) / 2);
    m.vp_offset[1] = static_cast<float>((screen_h - h) / 2);
    m.vp_size[0] = static_cast<float>(w);
    m.vp_size[1] = static_cast<float>(h);
    return m;
}

bool BridgeGpu::init(const GfxDeviceInfo& device, std::string& error) {
    _device = device;
    WGPUDevice dev = device.device;
    _srgbViews = wgpuDeviceHasFeature(dev, WGPUFeatureName_CoreFeaturesAndLimits);

    _utilityModule = make_module(dev, std::string(kVertexWgsl) + kUtilityWgsl, "ReShade Bridge depth conversion");
    _scaleModule = make_module(dev, std::string(kVertexWgsl) + kScaleWgsl, "ReShade Bridge scaling");

    const WGPUBindGroupLayoutEntry depthEntries[] = {texture_entry(0), uniform_entry(1, sizeof(DepthParams))};
    const WGPUBindGroupLayoutEntry downEntries[] = {texture_entry(0), uniform_entry(1, sizeof(MapParams))};
    const WGPUBindGroupLayoutEntry compositeEntries[] = {texture_entry(0), uniform_entry(1, sizeof(MapParams)),
        texture_entry(2), texture_entry(3), texture_entry(4), texture_entry(5)};
    _depthLayout = make_layout(dev, depthEntries, std::size(depthEntries));
    _downLayout = make_layout(dev, downEntries, std::size(downEntries));
    _compositeLayout = make_layout(dev, compositeEntries, std::size(compositeEntries));
    _depthPipelineLayout = make_pipeline_layout(dev, _depthLayout);
    _downPipelineLayout = make_pipeline_layout(dev, _downLayout);
    _compositePipelineLayout = make_pipeline_layout(dev, _compositeLayout);

    _depthPipeline = make_fullscreen_pipeline(dev, _utilityModule, _depthPipelineLayout, "fs_depth",
        WGPUTextureFormat_R32Float, "ReShade Bridge depth conversion");
    _depthDownPipeline = make_fullscreen_pipeline(dev, _scaleModule, _downPipelineLayout, "fs_depth_down",
        WGPUTextureFormat_R32Float, "ReShade Bridge depth to screen size");
    // Both formats the scene colour can have (it follows the swapchain).
    for (const WGPUTextureFormat f : {WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_BGRA8Unorm}) {
        if (WGPURenderPipeline p = make_fullscreen_pipeline(dev, _scaleModule, _downPipelineLayout, "fs_down", f,
                "ReShade Bridge colour to screen size")) {
            _down[f] = p;
        }
    }

    _farDepth = make_texture(dev, "ReShade Bridge far depth", 1, 1, WGPUTextureFormat_R32Float, WGPUTextureUsage_TextureBinding);
    _farDepthView = make_view(_farDepth, WGPUTextureFormat_R32Float);

    if (_utilityModule == nullptr || _scaleModule == nullptr || _depthPipeline == nullptr ||
        _depthDownPipeline == nullptr || _down.size() != 2 || _farDepthView == nullptr) {
        error = "failed to create the bridge's GPU objects";
        return false;
    }
    return true;
}

void BridgeGpu::release_point(PointTargets& t) {
    release_handle(t.color_view, wgpuTextureViewRelease);
    release_handle(t.color, wgpuTextureRelease);
    release_handle(t.input_view, wgpuTextureViewRelease);
    release_handle(t.input, wgpuTextureRelease);
    release_handle(t.color_marker, wgpuTextureRelease);
    release_handle(t.depth_marker, wgpuTextureRelease);
    t = PointTargets{};
}

void BridgeGpu::release_depth(DepthTarget& t) {
    release_handle(t.full_view, wgpuTextureViewRelease);
    release_handle(t.full, wgpuTextureRelease);
    release_handle(t.screen_view, wgpuTextureViewRelease);
    release_handle(t.screen, wgpuTextureRelease);
    t = DepthTarget{};
}

void BridgeGpu::release() {
    for (PointTargets& t : _points) {
        release_point(t);
    }
    release_depth(_depth);
    for (Retired& r : _retired) {
        release_point(r.point);
        release_depth(r.depth);
    }
    _retired.clear();
    for (auto& [key, p] : _composites) {
        if (p != nullptr) {
            wgpuRenderPipelineRelease(p);
        }
    }
    _composites.clear();
    for (auto& [f, p] : _down) {
        wgpuRenderPipelineRelease(p);
    }
    _down.clear();
    release_handle(_farDepthView, wgpuTextureViewRelease);
    release_handle(_farDepth, wgpuTextureRelease);
    release_handle(_depthPipeline, wgpuRenderPipelineRelease);
    release_handle(_depthDownPipeline, wgpuRenderPipelineRelease);
    release_handle(_depthPipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_downPipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_compositePipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_depthLayout, wgpuBindGroupLayoutRelease);
    release_handle(_downLayout, wgpuBindGroupLayoutRelease);
    release_handle(_compositeLayout, wgpuBindGroupLayoutRelease);
    release_handle(_utilityModule, wgpuShaderModuleRelease);
    release_handle(_scaleModule, wgpuShaderModuleRelease);
}

PointTargets* BridgeGpu::ensure_point(uint32_t point, uint32_t width, uint32_t height, WGPUTextureFormat format) {
    if (point >= drb::kPointCount || width == 0 || height == 0 || _down.find(format) == _down.end()) {
        return nullptr;
    }
    PointTargets& t = _points[point];
    if (t.color != nullptr && t.width == width && t.height == height && t.format == format) {
        return &t;
    }
    if (t.color != nullptr) {
        _retired.push_back(Retired{std::exchange(t, PointTargets{}), DepthTarget{}, kRetireFrames});
    }
    WGPUDevice dev = _device.device;
    // The hand-over texture: written by the scale-down (render attachment), by the add-on's
    // techniques (D3D12 render target), read by the composite (texture binding), the input copy and
    // the marker copy (copy source). The sRGB view format makes Dawn create the D3D12 resource
    // typeless, so the add-on can give ReShade an sRGB render target view for passes that set
    // SRGBWriteEnable. Without it (a compatibility-mode device) such passes write unconverted values.
    t.color = make_texture(dev, "ReShade Bridge hand-over", width, height, format,
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
        _srgbViews ? srgb_view_format(format) : WGPUTextureFormat_Undefined);
    t.color_view = make_view(t.color, format);
    t.input = make_texture(dev, "ReShade Bridge hand-over input", width, height, format,
        WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
    t.input_view = make_view(t.input, format);
    t.color_marker = make_texture(dev, "ReShade Bridge colour marker", drb::kMarkerWidth,
        drb::kColorMarkerHeightBase + point, format, WGPUTextureUsage_CopyDst);
    t.depth_marker = make_texture(dev, "ReShade Bridge depth marker", drb::kMarkerWidth,
        drb::kDepthMarkerHeightBase + point, WGPUTextureFormat_R32Float, WGPUTextureUsage_CopyDst);
    if (t.color_view == nullptr || t.input_view == nullptr || t.color_marker == nullptr || t.depth_marker == nullptr) {
        release_point(t);
        return nullptr;
    }
    t.width = width;
    t.height = height;
    t.format = format;
    return &t;
}

DepthTarget* BridgeGpu::ensure_depth(uint32_t full_width, uint32_t full_height, uint32_t screen_width, uint32_t screen_height) {
    if (full_width == 0 || full_height == 0 || screen_width == 0 || screen_height == 0) {
        return nullptr;
    }
    if (_depth.full != nullptr && _depth.full_width == full_width && _depth.full_height == full_height &&
        _depth.screen_width == screen_width && _depth.screen_height == screen_height) {
        return &_depth;
    }
    if (_depth.full != nullptr) {
        _retired.push_back(Retired{PointTargets{}, std::exchange(_depth, DepthTarget{}), kRetireFrames});
    }
    WGPUDevice dev = _device.device;
    // Frame size: written by the conversion, read by the screen-size sampling and the composite.
    _depth.full = make_texture(dev, "ReShade Bridge depth (frame size)", full_width, full_height,
        WGPUTextureFormat_R32Float, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
    _depth.full_view = make_view(_depth.full, WGPUTextureFormat_R32Float);
    // Screen size: read by ReShade as a shader resource and by the composite; Dawn also sees a
    // render attachment and the marker copy's source.
    _depth.screen = make_texture(dev, "ReShade Bridge depth", screen_width, screen_height, WGPUTextureFormat_R32Float,
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc);
    _depth.screen_view = make_view(_depth.screen, WGPUTextureFormat_R32Float);
    if (_depth.full_view == nullptr || _depth.screen_view == nullptr) {
        release_depth(_depth);
        return nullptr;
    }
    _depth.full_width = full_width;
    _depth.full_height = full_height;
    _depth.screen_width = screen_width;
    _depth.screen_height = screen_height;
    return &_depth;
}

void BridgeGpu::tick_retired() {
    for (auto it = _retired.begin(); it != _retired.end();) {
        if (--it->frames_left <= 0) {
            release_point(it->point);
            release_depth(it->depth);
            it = _retired.erase(it);
        } else {
            ++it;
        }
    }
}

WGPURenderPipeline BridgeGpu::down_pipeline(WGPUTextureFormat format) const {
    const auto it = _down.find(format);
    return it != _down.end() ? it->second : nullptr;
}

// --- Render worker -------------------------------------------------------------------------------

void BridgeGpu::record_point(const GfxComputeContext& ctx, const RecordPayload& p) {
    WGPUDevice dev = ctx.device;
    WGPUCommandEncoder enc = ctx.encoder;

    // One fullscreen draw into `target` (every pixel is written), or only a clear to zero.
    const auto fullscreen_pass = [&](WGPUTextureView target, WGPURenderPipeline pipeline, WGPUBindGroupLayout layout,
                                     const WGPUBindGroupEntry* entries, size_t count, const char* label) {
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = target;
        ca.loadOp = pipeline == nullptr ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = {0.0, 0.0, 0.0, 0.0};
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = sv(label);
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        if (pipeline != nullptr) {
            WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgd.layout = layout;
            bgd.entryCount = count;
            bgd.entries = entries;
            WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &bgd);
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuBindGroupRelease(group);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    };

    // Depth: converted from this point's snapshot at the frame's size, then sampled to the screen's;
    // or both cleared to "far" (stored 0) when there is no depth to convert, so effects never see a
    // stale frame's depth.
    const bool haveDepthTargets = p.depth_full_view != nullptr && p.depth_screen_view != nullptr;
    if ((p.flags & kRecordConvertDepth) != 0 && p.src_depth != nullptr && haveDepthTargets) {
        const WGPUBindGroupEntry convert[] = {texture_binding(0, p.src_depth),
            uniform_binding(1, ctx.uniform_buffer, p.depth_uniform_offset, sizeof(DepthParams))};
        fullscreen_pass(p.depth_full_view, _depthPipeline, _depthLayout, convert, std::size(convert), "ReShade Bridge depth");
        const WGPUBindGroupEntry down[] = {texture_binding(0, p.depth_full_view),
            uniform_binding(1, ctx.uniform_buffer, p.map_uniform_offset, sizeof(MapParams))};
        fullscreen_pass(p.depth_screen_view, _depthDownPipeline, _downLayout, down, std::size(down),
            "ReShade Bridge depth to screen size");
    } else if ((p.flags & kRecordClearDepth) != 0 && haveDepthTargets) {
        fullscreen_pass(p.depth_full_view, nullptr, nullptr, nullptr, 0, "ReShade Bridge depth clear");
        fullscreen_pass(p.depth_screen_view, nullptr, nullptr, nullptr, 0, "ReShade Bridge depth clear");
    }

    if ((p.flags & kRecordColor) == 0 || p.src_color == nullptr || p.color_view == nullptr || p.input == nullptr) {
        return;
    }
    WGPURenderPipeline down = down_pipeline(p.color_format);
    if (down == nullptr) {
        return;
    }
    const WGPUBindGroupEntry entries[] = {texture_binding(0, p.src_color),
        uniform_binding(1, ctx.uniform_buffer, p.map_uniform_offset, sizeof(MapParams))};
    fullscreen_pass(p.color_view, down, _downLayout, entries, std::size(entries), "ReShade Bridge colour to screen size");

    // The "before" the composite measures ReShade's change against.
    WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    from.texture = p.color;
    WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    to.texture = p.input;
    const WGPUExtent3D size = {wgpuTextureGetWidth(p.color), wgpuTextureGetHeight(p.color), 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &from, &to, &size);

    // The markers. Depth first: the add-on binds the depth texture when it sees the depth marker and
    // runs techniques when it sees the colour marker. Both copies also leave the two textures in the
    // copy-source state, which is the state the add-on expects them in.
    if (p.depth_screen != nullptr && p.depth_marker != nullptr) {
        copy_texel(enc, p.depth_screen, p.depth_marker);
    }
    copy_texel(enc, p.color, p.color_marker);
}

WGPURenderPipeline BridgeGpu::ensure_composite(const GfxDrawContext& ctx) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (const auto it = _composites.find(key); it != _composites.end()) {
        return it->second;
    }
    gfx_compat::ScenePassLayout layout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, _device, layout)) {
        return nullptr;
    }
    // Colour only: the game's alpha is left as the game wrote it. A compatibility-mode device
    // requires every target to share one write mask, which a pass with a second (write-masked)
    // attachment cannot satisfy; no composite there. The add-on only exists on D3D12, where Aurora
    // has core features, so this only keeps a stray call from becoming a fatal device error.
    if (!_srgbViews && layout.color_target_count > 1) {
        _composites[key] = nullptr;
        return nullptr;
    }
    layout.color_targets[0].writeMask = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = _scaleModule;
    fs.entryPoint = sv("fs_composite");
    fs.targetCount = layout.color_target_count;
    fs.targets = layout.color_targets;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = layout.depth_format;
    ds.depthWriteEnabled = WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = sv("ReShade Bridge composite");
    rpd.layout = _compositePipelineLayout;
    rpd.vertex.module = _scaleModule;
    rpd.vertex.entryPoint = sv("vs_fullscreen");
    rpd.depthStencil = layout.depth_format != WGPUTextureFormat_Undefined ? &ds : nullptr;
    rpd.multisample.count = layout.sample_count;
    rpd.fragment = &fs;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(ctx.device, &rpd);
    _composites[key] = pipeline;
    return pipeline;
}

void BridgeGpu::composite(const GfxDrawContext& ctx, const CompositePayload& p) {
    if (p.frame == nullptr || p.result == nullptr || p.input == nullptr) {
        return;
    }
    WGPURenderPipeline pipeline = ensure_composite(ctx);
    if (pipeline == nullptr) {
        return;
    }
    // Without depth (the game thread then asks for the Simple spread) any R32Float texture will do.
    const WGPUBindGroupEntry entries[] = {
        texture_binding(0, p.frame),
        uniform_binding(1, ctx.uniform_buffer, p.uniform_offset, sizeof(MapParams)),
        texture_binding(2, p.result),
        texture_binding(3, p.input),
        texture_binding(4, p.depth_full != nullptr ? p.depth_full : _farDepthView),
        texture_binding(5, p.depth_screen != nullptr ? p.depth_screen : _farDepthView),
    };
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = _compositeLayout;
    bgd.entryCount = std::size(entries);
    bgd.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(ctx.device, &bgd);
    const GfxColorAttachmentLayout& target = ctx.layout.color_attachments[GFX_SCENE_COLOR_ATTACHMENT_INDEX];
    wgpuRenderPassEncoderSetPipeline(ctx.pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx.pass, 0, group, 0, nullptr);
    // The game may have left a smaller viewport (2D ports); the composite covers the target.
    if (target.width != 0 && target.height != 0) {
        wgpuRenderPassEncoderSetViewport(ctx.pass, 0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(ctx.pass, 0, 0, target.width, target.height);
    }
    wgpuRenderPassEncoderDraw(ctx.pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(group);
}

} // namespace rsb
