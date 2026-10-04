#include "bridge_gpu.hpp"

#include "gfx_scene_pass.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace rsb {
namespace {

WGPUStringView sv(const char* s) { return {s, WGPU_STRLEN}; }

// Fullscreen triangle, a texel copy, and the depth conversion. The conversion is the inverse of
// ReShade.fxh's GetLinearizedDepth under drb::kDepthDefinitions (reversed, not logarithmic, far
// plane F): view distance z -> linear depth L = (z - near) / (range - near), clamped to [0, 1],
// stored as 1 - L * F / (1 + L * (F - 1)), which that function maps back to L. The view distance
// comes from the game's reversed-Z depth r through the projection: vz = (b - r * d) / (r * c - a)
// is view-space z, negative in front of the camera. Sky (r = 0) lands on L = 1, stored as 0.
const char* kUtilityWgsl = R"(
@group(0) @binding(0) var src : texture_2d<f32>;
struct DepthParams { a : f32, b : f32, c : f32, d : f32, near_plane : f32, range : f32, valid : f32, pad : f32 }
@group(0) @binding(1) var<uniform> dp : DepthParams;

@vertex fn vs_fullscreen(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
	let x = f32((i << 1u) & 2u);
	let y = f32(i & 2u);
	return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}

@fragment fn fs_copy(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	return textureLoad(src, vec2<i32>(p.xy), 0);
}

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

bool BridgeGpu::init(const GfxDeviceInfo& device, std::string& error) {
    _device = device;
    WGPUDevice dev = device.device;
    _srgbViews = wgpuDeviceHasFeature(dev, WGPUFeatureName_CoreFeaturesAndLimits);

    std::string code = kUtilityWgsl;
    char farPlane[32];
    std::snprintf(farPlane, sizeof(farPlane), "%.1f", static_cast<double>(drb::kDepthFarPlane));
    code.replace(code.find("FAR_PLANE"), 9, farPlane);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {code.data(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    md.label = sv("ReShade Bridge utilities");
    _module = wgpuDeviceCreateShaderModule(dev, &md);

    WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(DepthParams);
    WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bld.entryCount = 1;
    bld.entries = entries;
    _textureLayout = wgpuDeviceCreateBindGroupLayout(dev, &bld);
    bld.entryCount = 2;
    _depthLayout = wgpuDeviceCreateBindGroupLayout(dev, &bld);

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &_textureLayout;
    _texturePipelineLayout = wgpuDeviceCreatePipelineLayout(dev, &pld);
    pld.bindGroupLayouts = &_depthLayout;
    _depthPipelineLayout = wgpuDeviceCreatePipelineLayout(dev, &pld);

    _depthPipeline = make_fullscreen_pipeline(dev, _module, _depthPipelineLayout, "fs_depth",
        WGPUTextureFormat_R32Float, "ReShade Bridge depth conversion");
    // Both formats the scene colour can have (it follows the swapchain).
    for (const WGPUTextureFormat f : {WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_BGRA8Unorm}) {
        if (WGPURenderPipeline p = make_fullscreen_pipeline(dev, _module, _texturePipelineLayout, "fs_copy", f, "ReShade Bridge scene copy")) {
            _blit[f] = p;
        }
    }

    if (_module == nullptr || _depthPipeline == nullptr || _blit.size() != 2) {
        error = "failed to create the bridge's GPU objects";
        return false;
    }
    return true;
}

void BridgeGpu::release_point(PointTargets& t) {
    release_handle(t.color_view, wgpuTextureViewRelease);
    release_handle(t.color, wgpuTextureRelease);
    release_handle(t.color_marker, wgpuTextureRelease);
    release_handle(t.depth_marker, wgpuTextureRelease);
    t = PointTargets{};
}

void BridgeGpu::release_depth(DepthTarget& t) {
    release_handle(t.view, wgpuTextureViewRelease);
    release_handle(t.texture, wgpuTextureRelease);
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
    for (auto& [f, p] : _blit) {
        wgpuRenderPipelineRelease(p);
    }
    _blit.clear();
    release_handle(_depthPipeline, wgpuRenderPipelineRelease);
    release_handle(_texturePipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_depthPipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_textureLayout, wgpuBindGroupLayoutRelease);
    release_handle(_depthLayout, wgpuBindGroupLayoutRelease);
    release_handle(_module, wgpuShaderModuleRelease);
}

PointTargets* BridgeGpu::ensure_point(uint32_t point, uint32_t width, uint32_t height, WGPUTextureFormat format) {
    if (point >= drb::kPointCount || width == 0 || height == 0 || _blit.find(format) == _blit.end()) {
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
    // The colour texture: written by the scene copy (render attachment), by the add-on's techniques
    // (D3D12 render target), read by the composite (texture binding) and the marker copy (copy
    // source). The sRGB view format makes Dawn create the D3D12 resource typeless, so the add-on
    // can give ReShade an sRGB render target view for passes that set SRGBWriteEnable. Without it
    // (a compatibility-mode device) such passes write unconverted values.
    t.color = make_texture(dev, "ReShade Bridge colour", width, height, format,
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
        _srgbViews ? srgb_view_format(format) : WGPUTextureFormat_Undefined);
    WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    vd.format = format;
    t.color_view = t.color != nullptr ? wgpuTextureCreateView(t.color, &vd) : nullptr;
    t.color_marker = make_texture(dev, "ReShade Bridge colour marker", drb::kMarkerWidth,
        drb::kColorMarkerHeightBase + point, format, WGPUTextureUsage_CopyDst);
    t.depth_marker = make_texture(dev, "ReShade Bridge depth marker", drb::kMarkerWidth,
        drb::kDepthMarkerHeightBase + point, WGPUTextureFormat_R32Float, WGPUTextureUsage_CopyDst);
    if (t.color == nullptr || t.color_view == nullptr || t.color_marker == nullptr || t.depth_marker == nullptr) {
        release_point(t);
        return nullptr;
    }
    t.width = width;
    t.height = height;
    t.format = format;
    return &t;
}

DepthTarget* BridgeGpu::ensure_depth(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return nullptr;
    }
    if (_depth.texture != nullptr && _depth.width == width && _depth.height == height) {
        return &_depth;
    }
    if (_depth.texture != nullptr) {
        _retired.push_back(Retired{PointTargets{}, std::exchange(_depth, DepthTarget{}), kRetireFrames});
    }
    // Read by ReShade as a shader resource; Dawn sees a render attachment and a copy source.
    _depth.texture = make_texture(_device.device, "ReShade Bridge depth", width, height, WGPUTextureFormat_R32Float,
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc);
    _depth.view = _depth.texture != nullptr ? wgpuTextureCreateView(_depth.texture, nullptr) : nullptr;
    if (_depth.view == nullptr) {
        release_depth(_depth);
        return nullptr;
    }
    _depth.width = width;
    _depth.height = height;
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

WGPURenderPipeline BridgeGpu::blit_pipeline(WGPUTextureFormat format) const {
    const auto it = _blit.find(format);
    return it != _blit.end() ? it->second : nullptr;
}

// --- Render worker -------------------------------------------------------------------------------

void BridgeGpu::record_point(const GfxComputeContext& ctx, const RecordPayload& p) {
    WGPUDevice dev = ctx.device;
    WGPUCommandEncoder enc = ctx.encoder;

    const auto fullscreen_pass = [&](WGPUTextureView target, WGPURenderPipeline pipeline, WGPUBindGroup group,
                                     const char* label, bool clearOnly) {
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = target;
        ca.loadOp = clearOnly ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = {0.0, 0.0, 0.0, 0.0};
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = sv(label);
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        if (!clearOnly) {
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    };

    // Depth: converted from this point's snapshot, or cleared to "far" (stored 0) when there is no
    // depth to convert, so effects never see a stale frame's depth.
    if ((p.flags & kRecordConvertDepth) != 0 && p.src_depth != nullptr && p.depth_view != nullptr) {
        WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        e[0].binding = 0;
        e[0].textureView = p.src_depth;
        e[1].binding = 1;
        e[1].buffer = ctx.uniform_buffer;
        e[1].offset = p.uniform_offset;
        e[1].size = sizeof(DepthParams);
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = _depthLayout;
        bgd.entryCount = 2;
        bgd.entries = e;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &bgd);
        fullscreen_pass(p.depth_view, _depthPipeline, group, "ReShade Bridge depth", false);
        wgpuBindGroupRelease(group);
    } else if ((p.flags & kRecordClearDepth) != 0 && p.depth_view != nullptr) {
        fullscreen_pass(p.depth_view, nullptr, nullptr, "ReShade Bridge depth clear", true);
    }

    if ((p.flags & kRecordColor) == 0 || p.src_color == nullptr || p.color_view == nullptr) {
        return;
    }
    WGPURenderPipeline blit = blit_pipeline(p.color_format);
    if (blit == nullptr) {
        return;
    }
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = 0;
    e.textureView = p.src_color;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = _textureLayout;
    bgd.entryCount = 1;
    bgd.entries = &e;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &bgd);
    fullscreen_pass(p.color_view, blit, group, "ReShade Bridge scene copy", false);
    wgpuBindGroupRelease(group);

    // The markers. Depth first: the add-on binds the depth texture when it sees the depth marker and
    // runs techniques when it sees the colour marker. Both copies also leave the two textures in the
    // copy-source state, which is the state the add-on expects them in.
    if (p.depth != nullptr && p.depth_marker != nullptr) {
        copy_texel(enc, p.depth, p.depth_marker);
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
    fs.module = _module;
    fs.entryPoint = sv("fs_copy");
    fs.targetCount = layout.color_target_count;
    fs.targets = layout.color_targets;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = layout.depth_format;
    ds.depthWriteEnabled = WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = sv("ReShade Bridge composite");
    rpd.layout = _texturePipelineLayout;
    rpd.vertex.module = _module;
    rpd.vertex.entryPoint = sv("vs_fullscreen");
    rpd.depthStencil = layout.depth_format != WGPUTextureFormat_Undefined ? &ds : nullptr;
    rpd.multisample.count = layout.sample_count;
    rpd.fragment = &fs;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(ctx.device, &rpd);
    _composites[key] = pipeline;
    return pipeline;
}

void BridgeGpu::composite(const GfxDrawContext& ctx, const CompositePayload& p) {
    WGPURenderPipeline pipeline = p.color_view != nullptr ? ensure_composite(ctx) : nullptr;
    if (pipeline == nullptr) {
        return;
    }
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = 0;
    e.textureView = p.color_view;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = _textureLayout;
    bgd.entryCount = 1;
    bgd.entries = &e;
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
