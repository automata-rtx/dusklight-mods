#include "fx_gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace rsp {
namespace {

WGPUStringView sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

WGPUTextureFormat srgb_variant(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
    case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
    default: return WGPUTextureFormat_Undefined;
    }
}

bool is_integer_format(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint: return true;
    default: return false;
    }
}

uint32_t max_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = std::max({w, h, d});
    uint32_t levels = 1;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

// Fullscreen triangle plus the utility fragment shaders (back-buffer blit, mip generation, depth
// conversion). The depth encoding is the inverse of ReShade.fxh's GetLinearizedDepth with the
// definitions fx_compile.cpp fixes: reversed, not logarithmic, far plane kDepthLinearizationFarPlane.
// With linear depth L = (z - near) / (far - near) for view distance z, the encoded value is
// 1 - L * F / (1 + L * (F - 1)), which that function maps back to exactly L.
const char* kUtilityWgsl = R"(
@group(0) @binding(0) var src : texture_2d<f32>;
struct DepthParams { a : f32, b : f32, c : f32, d : f32, near_plane : f32, far_plane : f32, valid : f32, pad : f32 }
@group(0) @binding(1) var<uniform> dp : DepthParams;

@vertex fn vs_fullscreen(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
	let x = f32((i << 1u) & 2u);
	let y = f32(i & 2u);
	return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}

@fragment fn fs_copy(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	return textureLoad(src, vec2<i32>(p.xy), 0);
}

@fragment fn fs_mip(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	let n = vec2<i32>(textureDimensions(src, 0)) - vec2<i32>(1);
	let c = vec2<i32>(p.xy) * 2;
	return 0.25 * (textureLoad(src, min(c, n), 0) + textureLoad(src, min(c + vec2<i32>(1, 0), n), 0) +
	               textureLoad(src, min(c + vec2<i32>(0, 1), n), 0) + textureLoad(src, min(c + vec2<i32>(1, 1), n), 0));
}

@fragment fn fs_depth(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	let r = textureLoad(src, vec2<i32>(p.xy), 0).x;
	if (dp.valid == 0.0) {
		return vec4<f32>(r, 0.0, 0.0, 1.0);
	}
	let vz = (dp.b - r * dp.d) / (r * dp.c - dp.a);
	let l = clamp((-vz - dp.near_plane) / (dp.far_plane - dp.near_plane), 0.0, 1.0);
	let f = FAR_PLANE;
	return vec4<f32>(1.0 - l * f / (1.0 + l * (f - 1.0)), 0.0, 0.0, 1.0);
}
)";

WGPUBlendOperation convert_blend_op(reshadefx::blend_op op) {
    switch (op) {
    case reshadefx::blend_op::subtract: return WGPUBlendOperation_Subtract;
    case reshadefx::blend_op::reverse_subtract: return WGPUBlendOperation_ReverseSubtract;
    case reshadefx::blend_op::min: return WGPUBlendOperation_Min;
    case reshadefx::blend_op::max: return WGPUBlendOperation_Max;
    default: return WGPUBlendOperation_Add;
    }
}

WGPUBlendFactor convert_blend_factor(reshadefx::blend_factor f) {
    switch (f) {
    case reshadefx::blend_factor::zero: return WGPUBlendFactor_Zero;
    case reshadefx::blend_factor::source_color: return WGPUBlendFactor_Src;
    case reshadefx::blend_factor::one_minus_source_color: return WGPUBlendFactor_OneMinusSrc;
    case reshadefx::blend_factor::dest_color: return WGPUBlendFactor_Dst;
    case reshadefx::blend_factor::one_minus_dest_color: return WGPUBlendFactor_OneMinusDst;
    case reshadefx::blend_factor::source_alpha: return WGPUBlendFactor_SrcAlpha;
    case reshadefx::blend_factor::one_minus_source_alpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case reshadefx::blend_factor::dest_alpha: return WGPUBlendFactor_DstAlpha;
    case reshadefx::blend_factor::one_minus_dest_alpha: return WGPUBlendFactor_OneMinusDstAlpha;
    default: return WGPUBlendFactor_One;
    }
}

WGPUBlendComponent blend_component(reshadefx::blend_op op, reshadefx::blend_factor src, reshadefx::blend_factor dst) {
    WGPUBlendComponent c = WGPU_BLEND_COMPONENT_INIT;
    c.operation = convert_blend_op(op);
    // D3D ignores the factors of MIN and MAX; WebGPU requires them to be One.
    const bool minmax = op == reshadefx::blend_op::min || op == reshadefx::blend_op::max;
    c.srcFactor = minmax ? WGPUBlendFactor_One : convert_blend_factor(src);
    c.dstFactor = minmax ? WGPUBlendFactor_One : convert_blend_factor(dst);
    return c;
}

WGPUStencilOperation convert_stencil_op(reshadefx::stencil_op op) {
    switch (op) {
    case reshadefx::stencil_op::zero: return WGPUStencilOperation_Zero;
    case reshadefx::stencil_op::replace: return WGPUStencilOperation_Replace;
    case reshadefx::stencil_op::increment_saturate: return WGPUStencilOperation_IncrementClamp;
    case reshadefx::stencil_op::decrement_saturate: return WGPUStencilOperation_DecrementClamp;
    case reshadefx::stencil_op::invert: return WGPUStencilOperation_Invert;
    case reshadefx::stencil_op::increment: return WGPUStencilOperation_IncrementWrap;
    case reshadefx::stencil_op::decrement: return WGPUStencilOperation_DecrementWrap;
    default: return WGPUStencilOperation_Keep;
    }
}

WGPUCompareFunction convert_compare(reshadefx::stencil_func f) {
    switch (f) {
    case reshadefx::stencil_func::never: return WGPUCompareFunction_Never;
    case reshadefx::stencil_func::less: return WGPUCompareFunction_Less;
    case reshadefx::stencil_func::equal: return WGPUCompareFunction_Equal;
    case reshadefx::stencil_func::less_equal: return WGPUCompareFunction_LessEqual;
    case reshadefx::stencil_func::greater: return WGPUCompareFunction_Greater;
    case reshadefx::stencil_func::not_equal: return WGPUCompareFunction_NotEqual;
    case reshadefx::stencil_func::greater_equal: return WGPUCompareFunction_GreaterEqual;
    default: return WGPUCompareFunction_Always;
    }
}

WGPUPrimitiveTopology convert_topology(reshadefx::primitive_topology t) {
    switch (t) {
    case reshadefx::primitive_topology::point_list: return WGPUPrimitiveTopology_PointList;
    case reshadefx::primitive_topology::line_list: return WGPUPrimitiveTopology_LineList;
    case reshadefx::primitive_topology::line_strip: return WGPUPrimitiveTopology_LineStrip;
    case reshadefx::primitive_topology::triangle_strip: return WGPUPrimitiveTopology_TriangleStrip;
    default: return WGPUPrimitiveTopology_TriangleList;
    }
}

WGPUAddressMode convert_address(reshadefx::texture_address_mode m) {
    switch (m) {
    case reshadefx::texture_address_mode::wrap: return WGPUAddressMode_Repeat;
    case reshadefx::texture_address_mode::mirror: return WGPUAddressMode_MirrorRepeat;
    default: return WGPUAddressMode_ClampToEdge; // border is filtered in the shader
    }
}

WGPUTextureSampleType convert_sample_type(SampleType t, bool unfilterable) {
    switch (t) {
    case SampleType::Sint: return WGPUTextureSampleType_Sint;
    case SampleType::Uint: return WGPUTextureSampleType_Uint;
    default: return unfilterable ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
    }
}

template <class T, class F>
void release_handle(T& h, F fn) {
    if (h != nullptr) {
        fn(h);
        h = nullptr;
    }
}

} // namespace

WGPUTextureFormat storage_format_from_wgsl(const std::string& name) {
    static const std::unordered_map<std::string, WGPUTextureFormat> map = {
        {"rgba8unorm", WGPUTextureFormat_RGBA8Unorm},
        {"rgba16float", WGPUTextureFormat_RGBA16Float},
        {"r32float", WGPUTextureFormat_R32Float},
        {"rg32float", WGPUTextureFormat_RG32Float},
        {"rgba32float", WGPUTextureFormat_RGBA32Float},
        {"r32uint", WGPUTextureFormat_R32Uint},
        {"r32sint", WGPUTextureFormat_R32Sint},
        {"rgba32uint", WGPUTextureFormat_RGBA32Uint},
        {"rgba32sint", WGPUTextureFormat_RGBA32Sint},
    };
    const auto it = map.find(name);
    return it != map.end() ? it->second : WGPUTextureFormat_Undefined;
}

uint32_t bytes_per_texel(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_R8Unorm: return 1;
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_R16Float: return 2;
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_BGRA8Unorm:
    case WGPUTextureFormat_RGB10A2Unorm:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint: return 4;
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RG32Float: return 8;
    case WGPUTextureFormat_RGBA32Float:
    case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint: return 16;
    default: return 0;
    }
}

// -------------------------------------------------------------------------------------------------

ScopedErrors::ScopedErrors(const GpuApi& api, const char* what) : _api(api), _what(what) {
    wgpuDevicePushErrorScope(_api.device, WGPUErrorFilter_Validation);
    wgpuDevicePushErrorScope(_api.device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(_api.device, WGPUErrorFilter_Internal);
}

ScopedErrors::~ScopedErrors() {
    if (!_finished) {
        std::string ignored;
        finish(ignored);
    }
}

bool ScopedErrors::finish(std::string& errors) {
    _finished = true;
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        struct Result {
            WGPUErrorType type = WGPUErrorType_NoError;
            std::string message;
        } result;
        struct Wait {
            Result* result;
            std::atomic<bool> done{false};
        } state{&result};
        // Errors are known synchronously when the scope is popped, so the callback normally fires
        // inside PopErrorScope; waiting on the instance is only the fallback.
        WGPUPopErrorScopeCallbackInfo info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message, void* ud1, void*) {
            auto* w = static_cast<Wait*>(ud1);
            w->result->type = type;
            if (message.data != nullptr) {
                w->result->message.assign(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length);
            }
            w->done.store(true, std::memory_order_release);
        };
        info.userdata1 = &state;
        WGPUFutureWaitInfo wait{wgpuDevicePopErrorScope(_api.device, info), false};
        if (!state.done.load(std::memory_order_acquire)) {
            wgpuInstanceWaitAny(_api.instance, 1, &wait, UINT64_MAX);
        }
        if (result.type != WGPUErrorType_NoError) {
            if (ok) {
                errors += std::string(_what) + ": " + result.message + '\n';
            }
            ok = false;
        }
    }
    return ok;
}

// -------------------------------------------------------------------------------------------------

bool GpuTexture::create(const GpuApi& api, const char* label, WGPUTextureFormat fmt, uint32_t w, uint32_t h,
    uint32_t d, uint32_t lv, bool three, WGPUTextureUsage usage, bool srgbViews) {
    release();
    format = fmt;
    width = std::max(w, 1u);
    height = std::max(h, 1u);
    depth = std::max(d, 1u);
    levels = std::clamp(lv, 1u, max_levels(width, height, three ? depth : 1u));
    is_3d = three;

    const WGPUTextureFormat srgb = srgbViews ? srgb_variant(fmt) : WGPUTextureFormat_Undefined;
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = sv(label);
    desc.usage = usage;
    desc.dimension = three ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    desc.size = {width, height, three ? depth : 1u};
    desc.format = fmt;
    desc.mipLevelCount = levels;
    desc.sampleCount = 1;
    if (srgb != WGPUTextureFormat_Undefined) {
        desc.viewFormatCount = 1;
        desc.viewFormats = &srgb;
    }
    texture = wgpuDeviceCreateTexture(api.device, &desc);
    if (texture == nullptr) {
        return false;
    }
    const auto make_view = [&](WGPUTextureFormat vf, uint32_t base, uint32_t count, WGPUTextureUsage viewUsage) {
        WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        v.format = vf;
        v.dimension = three ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
        v.baseMipLevel = base;
        v.mipLevelCount = count;
        v.arrayLayerCount = 1;
        v.usage = viewUsage;
        return wgpuTextureCreateView(texture, &v);
    };
    const bool depthStencil = fmt == WGPUTextureFormat_Depth24PlusStencil8;
    if (!depthStencil) {
        view = make_view(fmt, 0, levels, usage & (WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst));
    }
    const WGPUTextureUsage srgbUsage = usage & (WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment);
    if (srgb != WGPUTextureFormat_Undefined) {
        view_srgb = make_view(srgb, 0, levels, srgbUsage & WGPUTextureUsage_TextureBinding);
    }
    for (uint32_t l = 0; l < levels; ++l) {
        WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        v.format = fmt;
        v.dimension = three ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
        v.baseMipLevel = l;
        v.mipLevelCount = 1;
        v.arrayLayerCount = 1;
        if (depthStencil) {
            v.aspect = WGPUTextureAspect_All;
        }
        level_views.push_back(wgpuTextureCreateView(texture, &v));
        if (srgb != WGPUTextureFormat_Undefined) {
            level_views_srgb.push_back(make_view(srgb, l, 1, srgbUsage));
        }
    }
    return true;
}

void GpuTexture::release() {
    for (auto& v : level_views) {
        release_handle(v, wgpuTextureViewRelease);
    }
    for (auto& v : level_views_srgb) {
        release_handle(v, wgpuTextureViewRelease);
    }
    level_views.clear();
    level_views_srgb.clear();
    release_handle(view, wgpuTextureViewRelease);
    release_handle(view_srgb, wgpuTextureViewRelease);
    release_handle(texture, wgpuTextureRelease);
}

bool FrameTextures::create(const GpuApi& api, uint32_t w, uint32_t h, WGPUTextureFormat colorFormat, std::string& errors) {
    release();
    width = w;
    height = h;
    color_format = colorFormat;
    ScopedErrors scope(api, "frame textures");
    const WGPUTextureUsage colorUsage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                                        WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    bool ok = backbuffer.create(api, "ReShade back buffer", colorFormat, w, h, 1, 1, false, colorUsage, true) &&
              color_copy.create(api, "ReShade COLOR", colorFormat, w, h, 1, 1, false, colorUsage, true) &&
              depth.create(api, "ReShade DEPTH", WGPUTextureFormat_R32Float, w, h, 1, 1, false,
                  WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, false) &&
              stencil.create(api, "ReShade stencil", WGPUTextureFormat_Depth24PlusStencil8, w, h, 1, 1, false,
                  WGPUTextureUsage_RenderAttachment, false) &&
              dummy.create(api, "ReShade placeholder", WGPUTextureFormat_R16Float, 1, 1, 1, 1, false,
                  WGPUTextureUsage_TextureBinding, false) &&
              dummy_3d.create(api, "ReShade placeholder 3D", WGPUTextureFormat_R16Float, 1, 1, 1, 1, true,
                  WGPUTextureUsage_TextureBinding, false) &&
              dummy_uint.create(api, "ReShade placeholder uint", WGPUTextureFormat_R32Uint, 1, 1, 1, 1, false,
                  WGPUTextureUsage_TextureBinding, false) &&
              dummy_sint.create(api, "ReShade placeholder sint", WGPUTextureFormat_R32Sint, 1, 1, 1, 1, false,
                  WGPUTextureUsage_TextureBinding, false);
    ok = scope.finish(errors) && ok;
    if (!ok) {
        release();
    }
    return ok;
}

void FrameTextures::release() {
    backbuffer.release();
    color_copy.release();
    depth.release();
    stencil.release();
    dummy.release();
    dummy_3d.release();
    dummy_uint.release();
    dummy_sint.release();
    width = height = 0;
}

WGPUTextureView FrameTextures::placeholder(SampleType type, bool is3d) const {
    switch (type) {
    case SampleType::Uint: return dummy_uint.view;
    case SampleType::Sint: return dummy_sint.view;
    default: return is3d ? dummy_3d.view : dummy.view;
    }
}

void TexturePool::release_all() {
    for (auto& [name, tex] : textures) {
        tex->gpu.release();
    }
    textures.clear();
}

// -------------------------------------------------------------------------------------------------

static WGPURenderPipeline make_fullscreen_pipeline(const GpuApi& api, WGPUShaderModule module, WGPUPipelineLayout layout,
    const char* fragmentEntry, WGPUTextureFormat format, const char* label) {
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = module;
    fs.entryPoint = sv(fragmentEntry);
    fs.targetCount = 1;
    fs.targets = &target;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = sv(label);
    rpd.layout = layout;
    rpd.vertex.module = module;
    rpd.vertex.entryPoint = sv("vs_fullscreen");
    rpd.fragment = &fs;
    return wgpuDeviceCreateRenderPipeline(api.device, &rpd);
}

bool GpuShared::init(const GpuApi& api, std::string& errors) {
    _api = api;
    ScopedErrors scope(api, "utility pipelines");

    std::string code = kUtilityWgsl;
    char farPlane[32];
    std::snprintf(farPlane, sizeof(farPlane), "%.1f", static_cast<double>(kDepthLinearizationFarPlane));
    code.replace(code.find("FAR_PLANE"), 9, farPlane);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {code.data(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    md.label = sv("ReShade port utilities");
    _utility = wgpuDeviceCreateShaderModule(api.device, &md);

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
    _singleTextureLayout = wgpuDeviceCreateBindGroupLayout(api.device, &bld);
    bld.entryCount = 2;
    _depthLayout = wgpuDeviceCreateBindGroupLayout(api.device, &bld);

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &_singleTextureLayout;
    _singleTexturePipelineLayout = wgpuDeviceCreatePipelineLayout(api.device, &pld);
    pld.bindGroupLayouts = &_depthLayout;
    _depthPipelineLayout = wgpuDeviceCreatePipelineLayout(api.device, &pld);

    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.label = sv("ReShade depth parameters");
    bd.size = sizeof(DepthParams);
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    _depthParams = wgpuDeviceCreateBuffer(api.device, &bd);

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_R32Float;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = _utility;
    fs.entryPoint = sv("fs_depth");
    fs.targetCount = 1;
    fs.targets = &target;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = sv("ReShade depth conversion");
    rpd.layout = _depthPipelineLayout;
    rpd.vertex.module = _utility;
    rpd.vertex.entryPoint = sv("vs_fullscreen");
    rpd.fragment = &fs;
    _depthPipeline = wgpuDeviceCreateRenderPipeline(api.device, &rpd);

    // Every format a texture may be stored in (fx_formats.hpp) that can have mipmaps generated by
    // rendering, and both scene colour formats for the back-buffer copy.
    static const WGPUTextureFormat kMipFormats[] = {
        WGPUTextureFormat_R8Unorm, WGPUTextureFormat_RG8Unorm, WGPUTextureFormat_RGBA8Unorm,
        WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGB10A2Unorm, WGPUTextureFormat_R16Float,
        WGPUTextureFormat_RG16Float, WGPUTextureFormat_RGBA16Float, WGPUTextureFormat_R32Float,
        WGPUTextureFormat_RG32Float, WGPUTextureFormat_RGBA32Float,
    };
    for (const WGPUTextureFormat f : kMipFormats) {
        if (WGPURenderPipeline p = make_fullscreen_pipeline(api, _utility, _singleTexturePipelineLayout, "fs_mip", f, "ReShade mipmaps")) {
            _mipgen[f] = p;
        }
    }
    for (const WGPUTextureFormat f : {WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_BGRA8Unorm}) {
        if (WGPURenderPipeline p = make_fullscreen_pipeline(api, _utility, _singleTexturePipelineLayout, "fs_copy", f, "ReShade back-buffer copy")) {
            _blit[f] = p;
        }
    }

    return scope.finish(errors) && _depthPipeline != nullptr;
}

void GpuShared::release() {
    for (auto& [f, p] : _mipgen) {
        wgpuRenderPipelineRelease(p);
    }
    _mipgen.clear();
    for (auto& [f, p] : _blit) {
        wgpuRenderPipelineRelease(p);
    }
    _blit.clear();
    for (auto& [k, s] : _samplers) {
        wgpuSamplerRelease(s);
    }
    _samplers.clear();
    release_handle(_depthPipeline, wgpuRenderPipelineRelease);
    release_handle(_depthParams, wgpuBufferRelease);
    release_handle(_singleTexturePipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_depthPipelineLayout, wgpuPipelineLayoutRelease);
    release_handle(_singleTextureLayout, wgpuBindGroupLayoutRelease);
    release_handle(_depthLayout, wgpuBindGroupLayoutRelease);
    release_handle(_utility, wgpuShaderModuleRelease);
}

WGPUSampler GpuShared::sampler(const reshadefx::sampler_desc& d, bool unfilterable, std::string& errors) {
    const auto f = static_cast<uint32_t>(d.filter);
    const bool aniso = d.filter == reshadefx::filter_mode::anisotropic;
    const bool minLin = !unfilterable && ((f & 0x10) != 0 || aniso);
    const bool magLin = !unfilterable && ((f & 0x04) != 0 || aniso);
    const bool mipLin = !unfilterable && ((f & 0x01) != 0 || aniso);
    const float minLod = std::isfinite(d.min_lod) ? std::clamp(d.min_lod, 0.0f, 32.0f) : 0.0f;
    const float maxLod = std::isfinite(d.max_lod) ? std::clamp(d.max_lod, minLod, 32.0f) : 32.0f;
    uint64_t key = static_cast<uint64_t>(minLin) | (static_cast<uint64_t>(magLin) << 1) |
                   (static_cast<uint64_t>(mipLin) << 2) | (static_cast<uint64_t>(aniso && !unfilterable) << 3) |
                   (static_cast<uint64_t>(d.address_u) << 4) | (static_cast<uint64_t>(d.address_v) << 8) |
                   (static_cast<uint64_t>(d.address_w) << 12);
    uint32_t lo;
    uint32_t hi;
    std::memcpy(&lo, &minLod, 4);
    std::memcpy(&hi, &maxLod, 4);
    key ^= (static_cast<uint64_t>(lo) << 16) ^ (static_cast<uint64_t>(hi) << 40);
    if (const auto it = _samplers.find(key); it != _samplers.end()) {
        return it->second;
    }
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.addressModeU = convert_address(d.address_u);
    sd.addressModeV = convert_address(d.address_v);
    sd.addressModeW = convert_address(d.address_w);
    sd.minFilter = minLin ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.magFilter = magLin ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.mipmapFilter = mipLin ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.lodMinClamp = minLod;
    sd.lodMaxClamp = maxLod;
    sd.maxAnisotropy = (aniso && !unfilterable) ? 16 : 1;
    ScopedErrors scope(_api, "sampler");
    WGPUSampler s = wgpuDeviceCreateSampler(_api.device, &sd);
    if (!scope.finish(errors) || s == nullptr) {
        return nullptr;
    }
    _samplers[key] = s;
    return s;
}

void GpuShared::blit_to_backbuffer(WGPUCommandEncoder encoder, WGPUTextureView snapshot, const FrameTextures& frame) const {
    const auto it = _blit.find(frame.color_format);
    if (it == _blit.end() || snapshot == nullptr) {
        return;
    }
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = 0;
    e.textureView = snapshot;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = _singleTextureLayout;
    bgd.entryCount = 1;
    bgd.entries = &e;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(_api.device, &bgd);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = frame.backbuffer.level_views[0];
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.label = sv("ReShade: scene to back buffer");
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, it->second);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(bg);
}

void GpuShared::convert_depth(WGPUCommandEncoder encoder, WGPUTextureView rawDepth, const FrameTextures& frame, const DepthParams& params) const {
    if (rawDepth == nullptr) {
        clear_depth(encoder, frame);
        return;
    }
    wgpuQueueWriteBuffer(_api.queue, _depthParams, 0, &params, sizeof(params));
    WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    e[0].binding = 0;
    e[0].textureView = rawDepth;
    e[1].binding = 1;
    e[1].buffer = _depthParams;
    e[1].size = sizeof(DepthParams);
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = _depthLayout;
    bgd.entryCount = 2;
    bgd.entries = e;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(_api.device, &bgd);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = frame.depth.level_views[0];
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.label = sv("ReShade: depth conversion");
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, _depthPipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(bg);
}

void GpuShared::clear_depth(WGPUCommandEncoder encoder, const FrameTextures& frame) const {
    // No scene this frame (title screens, transitions): everything reads as far away.
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = frame.depth.level_views[0];
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = {0.0, 0.0, 0.0, 0.0};
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.label = sv("ReShade: depth clear");
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

void GpuShared::generate_mips(WGPUCommandEncoder encoder, const GpuTexture& tex) const {
    const auto it = _mipgen.find(tex.format);
    if (it == _mipgen.end() || tex.is_3d || tex.levels < 2) {
        return;
    }
    for (uint32_t l = 1; l < tex.levels; ++l) {
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.textureView = tex.level_views[l - 1];
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = _singleTextureLayout;
        bgd.entryCount = 1;
        bgd.entries = &e;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(_api.device, &bgd);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = tex.level_views[l];
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = sv("ReShade: mipmaps");
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, it->second);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(bg);
    }
}

// -------------------------------------------------------------------------------------------------

EffectGpu::~EffectGpu() { release(); }

void EffectGpu::release() {
    for (auto& passes : _techniques) {
        for (Pass& p : passes) {
            release_handle(p.render, wgpuRenderPipelineRelease);
            release_handle(p.compute, wgpuComputePipelineRelease);
            for (auto& g : p.groups) {
                release_handle(g, wgpuBindGroupRelease);
            }
        }
    }
    _techniques.clear();
    for (auto& v : _views) {
        release_handle(v, wgpuTextureViewRelease);
    }
    _views.clear();
    for (auto& m : _modules) {
        release_handle(m, wgpuShaderModuleRelease);
    }
    _modules.clear();
    for (auto& l : _pipelineLayouts) {
        release_handle(l, wgpuPipelineLayoutRelease);
    }
    _pipelineLayouts.clear();
    for (auto& l : _layouts) {
        release_handle(l, wgpuBindGroupLayoutRelease);
    }
    _layouts.clear();
    release_handle(_uniforms, wgpuBufferRelease);
    _poolTextures.clear();
    _pool.reset();
}

bool EffectGpu::technique_built(size_t t) const {
    if (t >= _techniques.size() || _techniques[t].empty()) {
        return false;
    }
    for (const Pass& p : _techniques[t]) {
        if (p.render == nullptr && p.compute == nullptr) {
            return false;
        }
    }
    return true;
}

bool EffectGpu::build(const GpuApi& api, GpuShared& shared, const FrameTextures& frame, std::shared_ptr<TexturePool> poolRef,
    const CompiledEffect& effect, const SourceLoader& loader, std::string& errors) {
    release();
    _api = api;
    _generation = frame.generation;
    _pool = std::move(poolRef);
    TexturePool& pool = *_pool;
    const reshadefx::effect_module& module = effect.module();
    bool ok = true;

    // Textures (shared by unique name across effects).
    for (const reshadefx::texture& tex : module.textures) {
        if (!tex.semantic.empty()) {
            continue;
        }
        auto& slot = pool.textures[tex.unique_name];
        if (!slot) {
            slot = std::make_unique<PoolTexture>();
            slot->desc = tex;
            slot->phys = texture_physical(tex, frame.color_format, effect.texture_flags.get());
            const bool three = tex.type == reshadefx::texture_type::texture_3d;
            const uint32_t height = tex.type == reshadefx::texture_type::texture_1d ? 1u : tex.height;
            const uint32_t levels = std::max<uint32_t>(tex.levels, 1u);
            // Every 2D texture may be rendered to: a texture shared by name may be a render target in
            // an effect built later (ReShade upgrades the shared texture's flags the same way).
            WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
            if (!three) {
                usage |= WGPUTextureUsage_RenderAttachment;
            }
            if (slot->phys.storage_format != nullptr && effect.texture_flags != nullptr && effect.texture_flags->count(tex.unique_name) != 0 &&
                effect.texture_flags->at(tex.unique_name).storage) {
                // Another effect sharing this texture may bind it as storage (flags are merged).
                usage |= WGPUTextureUsage_StorageBinding;
            } else if (tex.storage_access) {
                usage |= WGPUTextureUsage_StorageBinding;
            }
            ScopedErrors scope(api, ("texture '" + tex.name + "'").c_str());
            const bool created = slot->gpu.create(api, tex.unique_name.c_str(), slot->phys.format, tex.width, height,
                tex.depth, levels, three, usage, slot->phys.srgb_view);
            if (!scope.finish(errors) || !created) {
                pool.textures.erase(tex.unique_name);
                ok = false;
                continue;
            }
            (void)shared;
            for (const reshadefx::annotation& a : tex.annotations) {
                if (a.name == "source" && loader) {
                    if (loader(a.value.string_data, *slot, errors)) {
                        slot->needs_upload.store(true);
                    } else {
                        ok = false;
                    }
                }
            }
        } else {
            // The shaders of this effect were generated for this effect's own declaration, so a
            // texture shared by name must be stored the same way.
            const TexturePhysical phys = texture_physical(tex, frame.color_format, effect.texture_flags.get());
            if (phys.format != slot->phys.format || tex.type != slot->desc.type) {
                errors += "texture '" + tex.name + "' is declared with a different format or type by another effect\n";
                ok = false;
                continue;
            }
            if (tex.storage_access && (wgpuTextureGetUsage(slot->gpu.texture) & WGPUTextureUsage_StorageBinding) == 0) {
                errors += "texture '" + tex.name + "' is shared with another effect that does not use it as storage\n";
                ok = false;
                continue;
            }
        }
        if (const auto it = pool.textures.find(tex.unique_name); it != pool.textures.end()) {
            ++it->second->refs;
            _poolTextures.push_back(it->second.get());
        }
    }
    if (!ok) {
        return false;
    }

    // Uniforms: one buffer per effect, group 0.
    _uniformSize = (module.total_uniform_size + 15u) & ~15u;
    WGPUBindGroupLayout group0Layout = nullptr;
    WGPUBindGroup group0 = nullptr;
    {
        ScopedErrors scope(api, "uniform buffer");
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = 0;
        e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.minBindingSize = _uniformSize;
        WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bld.entryCount = _uniformSize != 0 ? 1 : 0;
        bld.entries = &e;
        group0Layout = wgpuDeviceCreateBindGroupLayout(api.device, &bld);
        _layouts.push_back(group0Layout);
        WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
        if (_uniformSize != 0) {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.size = _uniformSize;
            bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            _uniforms = wgpuDeviceCreateBuffer(api.device, &bd);
            be.binding = 0;
            be.buffer = _uniforms;
            be.size = _uniformSize;
        }
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = group0Layout;
        bgd.entryCount = _uniformSize != 0 ? 1 : 0;
        bgd.entries = &be;
        group0 = wgpuDeviceCreateBindGroup(api.device, &bgd);
        if (!scope.finish(errors)) {
            if (group0 != nullptr) {
                wgpuBindGroupRelease(group0);
            }
            return false;
        }
    }

    std::unordered_map<std::string, WGPUShaderModule> modules;
    const auto module_for = [&](const std::string& entry, const WgslEntryPoint*& out) -> WGPUShaderModule {
        const auto ep = effect.entry_points.find(entry);
        if (ep == effect.entry_points.end()) {
            errors += "entry point '" + entry + "' was not generated\n";
            return nullptr;
        }
        out = &ep->second;
        if (const auto it = modules.find(entry); it != modules.end()) {
            return it->second;
        }
        ScopedErrors scope(api, ("shader '" + entry + "'").c_str());
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = {ep->second.code.data(), ep->second.code.size()};
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &wgsl.chain;
        md.label = sv(entry.c_str());
        WGPUShaderModule m = wgpuDeviceCreateShaderModule(api.device, &md);
        if (!scope.finish(errors) || m == nullptr) {
            if (m != nullptr) {
                wgpuShaderModuleRelease(m);
            }
            return nullptr;
        }
        modules[entry] = m;
        _modules.push_back(m);
        return m;
    };

    const auto view_for_sampler = [&](const reshadefx::sampler& smp) -> WGPUTextureView {
        const reshadefx::texture* tex = nullptr;
        for (const reshadefx::texture& t : module.textures) {
            if (t.unique_name == smp.texture_name) {
                tex = &t;
            }
        }
        if (tex == nullptr) {
            return nullptr;
        }
        if (tex->semantic == "COLOR") {
            return smp.srgb && frame.color_copy.view_srgb ? frame.color_copy.view_srgb : frame.color_copy.view;
        }
        if (tex->semantic == "DEPTH") {
            return frame.depth.view;
        }
        if (!tex->semantic.empty()) {
            return frame.dummy.view;
        }
        const auto it = pool.textures.find(tex->unique_name);
        if (it == pool.textures.end()) {
            return nullptr;
        }
        return smp.srgb && it->second->gpu.view_srgb ? it->second->gpu.view_srgb : it->second->gpu.view;
    };

    for (const reshadefx::technique& tech : module.techniques) {
        std::vector<Pass> passes;
        for (size_t pi = 0; pi < tech.passes.size(); ++pi) {
            const reshadefx::pass& rp = tech.passes[pi];
            const std::string where = "technique '" + tech.name + "' pass " + std::to_string(pi);
            Pass p;
            const bool compute = !rp.cs_entry_point.empty();

            // Shaders and their bindings.
            const WgslEntryPoint* cs = nullptr;
            const WgslEntryPoint* vs = nullptr;
            const WgslEntryPoint* ps = nullptr;
            WGPUShaderModule csm = nullptr, vsm = nullptr, psm = nullptr;
            if (compute) {
                csm = module_for(rp.cs_entry_point, cs);
            } else {
                vsm = module_for(rp.vs_entry_point, vs);
                if (!rp.ps_entry_point.empty()) {
                    psm = module_for(rp.ps_entry_point, ps);
                }
            }
            if ((compute && csm == nullptr) || (!compute && (vsm == nullptr || (!rp.ps_entry_point.empty() && psm == nullptr)))) {
                errors += where + ": shader creation failed\n";
                ok = false;
                continue;
            }

            std::map<uint32_t, WgslSamplerBinding> samplerBindings;
            for (const WgslEntryPoint* ep : {cs, vs, ps}) {
                if (ep == nullptr) {
                    continue;
                }
                for (const WgslSamplerBinding& sb : ep->samplers) {
                    const auto [it, inserted] = samplerBindings.emplace(sb.binding, sb);
                    if (!inserted && it->second.sampler_index != sb.sampler_index) {
                        errors += where + ": vertex and pixel shader disagree on sampler binding " + std::to_string(sb.binding) + '\n';
                        ok = false;
                    }
                }
            }
            const WGPUShaderStage stages = compute ? WGPUShaderStage_Compute : (WGPUShaderStage_Vertex | WGPUShaderStage_Fragment);

            // Lowest level of each texture this pass writes. A sampled view that includes a written
            // level is a validation error in WebGPU (D3D12 leaves it undefined), so such a sampler
            // sees only the levels below it, or a zero placeholder when level 0 is written.
            std::map<std::string, uint32_t> written;
            if (compute) {
                for (const WgslStorageBinding& sb : cs->storages) {
                    if (sb.access != StorageAccess::Read) {
                        const reshadefx::storage& st = module.storages[sb.storage_index];
                        auto [it, inserted] = written.emplace(st.texture_name, st.level);
                        it->second = std::min<uint32_t>(it->second, st.level);
                    }
                }
            } else {
                for (const std::string& name : rp.render_target_names) {
                    if (!name.empty()) {
                        written[name] = 0;
                    }
                }
            }

            ScopedErrors scope(api, where.c_str());
            std::vector<WGPUBindGroupLayoutEntry> l1;
            std::vector<WGPUBindGroupEntry> g1;
            for (const auto& [b, sb] : samplerBindings) {
                const reshadefx::sampler& smp = module.samplers[sb.sampler_index];
                WGPUBindGroupLayoutEntry te = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                te.binding = 2 * b;
                te.visibility = stages;
                te.texture.sampleType = convert_sample_type(sb.sample_type, sb.unfilterable);
                te.texture.viewDimension = sb.is_3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
                WGPUBindGroupLayoutEntry se = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                se.binding = 2 * b + 1;
                se.visibility = stages;
                se.sampler.type = sb.unfilterable ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;
                l1.push_back(te);
                l1.push_back(se);
                WGPUBindGroupEntry tv = WGPU_BIND_GROUP_ENTRY_INIT;
                tv.binding = 2 * b;
                tv.textureView = view_for_sampler(smp);
                if (const auto w = written.find(smp.texture_name); w != written.end()) {
                    const auto t = pool.textures.find(smp.texture_name);
                    if (w->second > 0 && t != pool.textures.end()) {
                        const GpuTexture& g = t->second->gpu;
                        const bool srgb = smp.srgb && g.view_srgb != nullptr;
                        WGPUTextureViewDescriptor v = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                        v.format = srgb ? srgb_variant(g.format) : g.format;
                        v.dimension = g.is_3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
                        v.baseMipLevel = 0;
                        v.mipLevelCount = std::min(w->second, g.levels);
                        v.arrayLayerCount = 1;
                        v.usage = WGPUTextureUsage_TextureBinding;
                        tv.textureView = wgpuTextureCreateView(g.texture, &v);
                        _views.push_back(tv.textureView);
                    } else {
                        tv.textureView = frame.placeholder(sb.sample_type, sb.is_3d);
                    }
                }
                WGPUBindGroupEntry sv2 = WGPU_BIND_GROUP_ENTRY_INIT;
                sv2.binding = 2 * b + 1;
                sv2.sampler = shared.sampler(smp, sb.unfilterable, errors);
                if (tv.textureView == nullptr || sv2.sampler == nullptr) {
                    errors += where + ": cannot bind sampler '" + smp.name + "'\n";
                    ok = false;
                }
                g1.push_back(tv);
                g1.push_back(sv2);
            }
            std::vector<WGPUBindGroupLayoutEntry> l2;
            std::vector<WGPUBindGroupEntry> g2;
            if (cs != nullptr) {
                for (const WgslStorageBinding& sb : cs->storages) {
                    const reshadefx::storage& st = module.storages[sb.storage_index];
                    const auto it = pool.textures.find(st.texture_name);
                    if (it == pool.textures.end()) {
                        errors += where + ": storage '" + st.name + "' has no texture\n";
                        ok = false;
                        continue;
                    }
                    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                    e.binding = sb.binding;
                    e.visibility = WGPUShaderStage_Compute;
                    e.storageTexture.access = sb.access == StorageAccess::ReadWrite ? WGPUStorageTextureAccess_ReadWrite
                                              : sb.access == StorageAccess::Read  ? WGPUStorageTextureAccess_ReadOnly
                                                                                  : WGPUStorageTextureAccess_WriteOnly;
                    e.storageTexture.format = storage_format_from_wgsl(sb.format);
                    e.storageTexture.viewDimension = sb.is_3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
                    l2.push_back(e);
                    WGPUBindGroupEntry ge = WGPU_BIND_GROUP_ENTRY_INIT;
                    ge.binding = sb.binding;
                    const uint32_t level = std::min<uint32_t>(st.level, it->second->gpu.levels - 1);
                    ge.textureView = it->second->gpu.level_views[level];
                    g2.push_back(ge);
                    if (sb.access != StorageAccess::Read && rp.generate_mipmaps && it->second->gpu.levels > 1 &&
                        std::find(p.mip_targets.begin(), p.mip_targets.end(), it->second.get()) == p.mip_targets.end()) {
                        p.mip_targets.push_back(it->second.get());
                    }
                }
            }

            WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            bld.entryCount = l1.size();
            bld.entries = l1.data();
            WGPUBindGroupLayout layout1 = wgpuDeviceCreateBindGroupLayout(api.device, &bld);
            bld.entryCount = l2.size();
            bld.entries = l2.data();
            WGPUBindGroupLayout layout2 = wgpuDeviceCreateBindGroupLayout(api.device, &bld);
            _layouts.push_back(layout1);
            _layouts.push_back(layout2);
            WGPUBindGroupLayout layouts[3] = {group0Layout, layout1, layout2};
            WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pld.bindGroupLayoutCount = 3;
            pld.bindGroupLayouts = layouts;
            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(api.device, &pld);
            _pipelineLayouts.push_back(pipelineLayout);

            wgpuBindGroupAddRef(group0);
            p.groups[0] = group0;
            WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgd.layout = layout1;
            bgd.entryCount = g1.size();
            bgd.entries = g1.data();
            p.groups[1] = wgpuDeviceCreateBindGroup(api.device, &bgd);
            bgd.layout = layout2;
            bgd.entryCount = g2.size();
            bgd.entries = g2.data();
            p.groups[2] = wgpuDeviceCreateBindGroup(api.device, &bgd);

            if (compute) {
                p.dispatch[0] = std::max(rp.viewport_width, 1u);
                p.dispatch[1] = std::max(rp.viewport_height, 1u);
                p.dispatch[2] = std::max(rp.viewport_dispatch_z, 1u);
                WGPUComputePipelineDescriptor cpd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
                cpd.label = sv(rp.cs_entry_point.c_str());
                cpd.layout = pipelineLayout;
                cpd.compute.module = csm;
                cpd.compute.entryPoint = sv("main");
                p.compute = wgpuDeviceCreateComputePipeline(api.device, &cpd);
            } else {
                WGPUColorTargetState targets[8];
                WGPUBlendState blends[8];
                p.srgb_write = rp.srgb_write_enable;
                if (rp.render_target_names[0].empty()) {
                    p.target_count = 0;
                    p.width = frame.width;
                    p.height = frame.height;
                    targets[0] = WGPU_COLOR_TARGET_STATE_INIT;
                    targets[0].format = rp.srgb_write_enable && srgb_variant(frame.color_format) != WGPUTextureFormat_Undefined
                        ? srgb_variant(frame.color_format) : frame.color_format;
                } else {
                    for (uint32_t i = 0; i < 8 && !rp.render_target_names[i].empty(); ++i) {
                        const auto it = pool.textures.find(rp.render_target_names[i]);
                        if (it == pool.textures.end()) {
                            errors += where + ": unknown render target '" + rp.render_target_names[i] + "'\n";
                            ok = false;
                            break;
                        }
                        const PoolTexture& rt = *it->second;
                        const bool srgb = rp.srgb_write_enable && !rt.gpu.level_views_srgb.empty();
                        p.targets[i] = srgb ? rt.gpu.level_views_srgb[0] : rt.gpu.level_views[0];
                        targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
                        targets[i].format = srgb ? srgb_variant(rt.gpu.format) : rt.gpu.format;
                        if (rp.blend_enable[i] && !rt.phys.blendable) {
                            errors += where + ": blending into a " + std::string(rt.phys.sample_type == SampleType::Float ? "32-bit float" : "integer") +
                                      " render target is not supported yet\n";
                            ok = false;
                        }
                        if (rp.generate_mipmaps && rt.gpu.levels > 1 &&
                            std::find(p.mip_targets.begin(), p.mip_targets.end(), &rt) == p.mip_targets.end()) {
                            p.mip_targets.push_back(&rt);
                        }
                        p.target_count = i + 1;
                        p.width = rt.gpu.width;
                        p.height = rt.gpu.height;
                    }
                }
                const uint32_t count = std::max(p.target_count, 1u);
                for (uint32_t i = 0; i < count; ++i) {
                    if (rp.blend_enable[i]) {
                        blends[i].color = blend_component(rp.color_blend_op[i], rp.source_color_blend_factor[i], rp.dest_color_blend_factor[i]);
                        blends[i].alpha = blend_component(rp.alpha_blend_op[i], rp.source_alpha_blend_factor[i], rp.dest_alpha_blend_factor[i]);
                        targets[i].blend = &blends[i];
                    }
                    targets[i].writeMask = static_cast<WGPUColorWriteMask>(rp.render_target_write_mask[i] & 0xF);
                    if (ps == nullptr || (ps->fragment_outputs & (1u << i)) == 0) {
                        targets[i].writeMask = WGPUColorWriteMask_None;
                    }
                }

                WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
                fs.module = psm;
                fs.entryPoint = sv("main");
                fs.targetCount = count;
                fs.targets = targets;
                WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                rpd.label = sv(where.c_str());
                rpd.layout = pipelineLayout;
                rpd.vertex.module = vsm;
                rpd.vertex.entryPoint = sv("main");
                rpd.primitive.topology = convert_topology(rp.topology);
                rpd.primitive.cullMode = WGPUCullMode_None;
                rpd.fragment = psm != nullptr ? &fs : nullptr;
                WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
                p.stencil = rp.stencil_enable && p.width == frame.width && p.height == frame.height;
                if (p.stencil) {
                    ds.format = WGPUTextureFormat_Depth24PlusStencil8;
                    ds.depthWriteEnabled = WGPUOptionalBool_False;
                    ds.depthCompare = WGPUCompareFunction_Always;
                    WGPUStencilFaceState face = WGPU_STENCIL_FACE_STATE_INIT;
                    face.compare = convert_compare(rp.stencil_comparison_func);
                    face.failOp = convert_stencil_op(rp.stencil_fail_op);
                    face.depthFailOp = convert_stencil_op(rp.stencil_depth_fail_op);
                    face.passOp = convert_stencil_op(rp.stencil_pass_op);
                    ds.stencilFront = face;
                    ds.stencilBack = face;
                    ds.stencilReadMask = rp.stencil_read_mask;
                    ds.stencilWriteMask = rp.stencil_write_mask;
                    rpd.depthStencil = &ds;
                    p.stencil_ref = rp.stencil_reference_value;
                }
                p.clear = rp.clear_render_targets;
                p.vertex_count = rp.num_vertices;
                if (ok) {
                    p.render = wgpuDeviceCreateRenderPipeline(api.device, &rpd);
                }
            }
            if (!scope.finish(errors) || (compute ? p.compute == nullptr : p.render == nullptr)) {
                ok = false;
            }
            passes.push_back(std::move(p));
        }
        _techniques.push_back(std::move(passes));
    }
    wgpuBindGroupRelease(group0);
    return ok;
}

void EffectGpu::upload_pending(RecordState& state) const {
    for (PoolTexture* tp : _poolTextures) {
        PoolTexture& t = *tp;
        bool expected = true;
        if (!t.needs_upload.compare_exchange_strong(expected, false) || t.pixels.empty()) {
            continue;
        }
        // Queue writes are ordered before the frame's command buffer, so the mip generation recorded
        // below sees the new pixels (ReShade also regenerates mips after loading an image).
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = t.gpu.texture;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = t.bytes_per_row;
        layout.rowsPerImage = t.gpu.height;
        const WGPUExtent3D size = {t.gpu.width, t.gpu.height, t.gpu.is_3d ? t.gpu.depth : 1u};
        wgpuQueueWriteTexture(_api.queue, &dst, t.pixels.data(), t.pixels.size(), &layout, &size);
        state.shared->generate_mips(state.encoder, t.gpu);
        std::vector<uint8_t>().swap(t.pixels);
    }
}

void EffectGpu::write_uniforms(WGPUQueue queue, const uint8_t* data, size_t size) const {
    if (_uniforms != nullptr && data != nullptr && size != 0) {
        wgpuQueueWriteBuffer(queue, _uniforms, 0, data, std::min<size_t>(size, _uniformSize));
    }
}

void EffectGpu::record_technique(RecordState& state, size_t technique) const {
    if (technique >= _techniques.size()) {
        return;
    }
    const FrameTextures& frame = *state.frame;
    bool needsCopy = true; // ReShade refreshes COLOR before a technique's first pass
    bool stencilCleared = false;
    for (const Pass& p : _techniques[technique]) {
        if (p.render == nullptr && p.compute == nullptr) {
            return; // a pass failed to build: skip the rest of the technique
        }
        if (needsCopy) {
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            src.texture = frame.backbuffer.texture;
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = frame.color_copy.texture;
            const WGPUExtent3D size = {frame.width, frame.height, 1};
            wgpuCommandEncoderCopyTextureToTexture(state.encoder, &src, &dst, &size);
        }
        if (p.compute != nullptr) {
            WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(state.encoder, &cpd);
            wgpuComputePassEncoderSetPipeline(pass, p.compute);
            for (uint32_t g = 0; g < 3; ++g) {
                wgpuComputePassEncoderSetBindGroup(pass, g, p.groups[g], 0, nullptr);
            }
            wgpuComputePassEncoderDispatchWorkgroups(pass, p.dispatch[0], p.dispatch[1], p.dispatch[2]);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            needsCopy = false;
        } else {
            WGPURenderPassColorAttachment ca[8];
            uint32_t count = 0;
            if (p.target_count == 0) {
                ca[0] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                ca[0].view = p.srgb_write && !frame.backbuffer.level_views_srgb.empty() ? frame.backbuffer.level_views_srgb[0]
                                                                                       : frame.backbuffer.level_views[0];
                count = 1;
            } else {
                for (uint32_t i = 0; i < p.target_count; ++i) {
                    ca[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                    ca[i].view = p.targets[i];
                }
                count = p.target_count;
            }
            for (uint32_t i = 0; i < count; ++i) {
                ca[i].loadOp = p.clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
                ca[i].storeOp = WGPUStoreOp_Store;
                ca[i].clearValue = {0.0, 0.0, 0.0, 0.0};
            }
            WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rpd.colorAttachmentCount = count;
            rpd.colorAttachments = ca;
            if (p.stencil) {
                dsa.view = frame.stencil.level_views[0];
                dsa.depthLoadOp = WGPULoadOp_Clear;
                dsa.depthStoreOp = WGPUStoreOp_Discard;
                dsa.depthClearValue = 0.0f;
                dsa.stencilLoadOp = stencilCleared ? WGPULoadOp_Load : WGPULoadOp_Clear;
                dsa.stencilStoreOp = WGPUStoreOp_Store;
                dsa.stencilClearValue = 0;
                stencilCleared = true;
                rpd.depthStencilAttachment = &dsa;
            }
            WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(state.encoder, &rpd);
            wgpuRenderPassEncoderSetPipeline(pass, p.render);
            for (uint32_t g = 0; g < 3; ++g) {
                wgpuRenderPassEncoderSetBindGroup(pass, g, p.groups[g], 0, nullptr);
            }
            wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, static_cast<float>(p.width), static_cast<float>(p.height), 0.0f, 1.0f);
            wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, p.width, p.height);
            if (p.stencil) {
                wgpuRenderPassEncoderSetStencilReference(pass, p.stencil_ref);
            }
            wgpuRenderPassEncoderDraw(pass, p.vertex_count, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            needsCopy = p.target_count == 0;
        }
        for (const PoolTexture* t : p.mip_targets) {
            state.shared->generate_mips(state.encoder, t->gpu);
        }
    }
}

} // namespace rsp
