// Drives the mod's GPU half (src/bridge_gpu.cpp) on Dawn's null backend, standing in for the game:
// a device configured like Aurora's (an uncaptured error is fatal there, so any error fails this
// run), fake scene snapshots at every insertion point, the GfxService compute and draw callbacks
// in frame order (both scale-up modes, and the change-layer debug draw), a resize, both scene
// colour formats, a multisampled scene pass with a normal attachment, frames larger than, equal to
// and of another aspect ratio than ReShade's screen, and all of it again on a compatibility-mode
// device. It checks the frame's placement on the screen against Aurora's present, and the depth
// encoding against ReShade.fxh's GetLinearizedDepth under drb::kDepthDefinitions, so that ReShade
// sees the linear depth the mod meant to give it.
//
// The null backend runs no shaders. When a Vulkan adapter exists (on Linux, Mesa's lavapipe: the
// mesa-vulkan-drivers package) it also runs the scaling shaders for real and checks their output
// against values worked out on the CPU: the area-average scale-down, the one-sample depth, an
// exact round trip at the screen's size, and the edge-aware scale-up keeping a change on its own
// side of a silhouette where the simple one bleeds. Without one that part is skipped.
//
// What it cannot check: anything on the D3D12 side (the add-on, ReShade, the marker copies as
// Dawn records them). Those need the game.
//
// Build and run: docs/reshade_bridge.md, "Offline check".

#include "bridge_gpu.hpp"

#include <webgpu/webgpu_cpp.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

std::atomic<int> g_errors{0};

struct Device {
    wgpu::Instance instance;
    wgpu::Device device;
};

// Core features when `core` (what Aurora gets on D3D12), else a compatibility-mode device.
Device make_device(bool core, wgpu::BackendType backend = wgpu::BackendType::Null) {
    Device d;
    const wgpu::InstanceFeatureName instanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor id{};
    id.requiredFeatureCount = 1;
    id.requiredFeatures = instanceFeatures;
    d.instance = wgpu::CreateInstance(&id);
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = backend;
    ao.featureLevel = wgpu::FeatureLevel::Compatibility;
    wgpu::Adapter adapter;
    d.instance.WaitAny(d.instance.RequestAdapter(&ao, wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::RequestAdapterStatus, wgpu::Adapter a, wgpu::StringView) { adapter = std::move(a); }),
        UINT64_MAX);
    if (!adapter) {
        return d;
    }
    wgpu::DeviceDescriptor dd{};
    const wgpu::FeatureName coreFeature = wgpu::FeatureName::CoreFeaturesAndLimits;
    if (core) {
        dd.requiredFeatureCount = 1;
        dd.requiredFeatures = &coreFeature;
    }
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m) {
        ++g_errors;
        std::fprintf(stderr, "GAME DEVICE ERROR (fatal in Aurora): %.*s\n", static_cast<int>(m.length), m.data);
    });
    d.instance.WaitAny(adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::RequestDeviceStatus, wgpu::Device dev, wgpu::StringView) { d.device = std::move(dev); }),
        UINT64_MAX);
    return d;
}

wgpu::Texture texture(const wgpu::Device& dev, uint32_t w, uint32_t h, wgpu::TextureFormat f, wgpu::TextureUsage u, uint32_t samples = 1) {
    wgpu::TextureDescriptor td{};
    td.size = {w, h, 1};
    td.format = f;
    td.usage = u;
    td.sampleCount = samples;
    return dev.CreateTexture(&td);
}

// A reversed-Z perspective projection (column-major), near n, far f: depth 1 at n, 0 at f.
void reversed_z_projection(float n, float f, float m[16]) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = n / (f - n);
    m[11] = -1.0f;
    m[14] = f * n / (f - n);
}

// ReShade.fxh GetLinearizedDepth with the bridge's definitions (reversed, linear, far plane F).
float reshade_linearize(float stored) {
    const float F = drb::kDepthFarPlane;
    float depth = 1.0f - stored;
    return depth / (F - depth * (F - 1.0f));
}

struct Scene {
    uint32_t width, height;               // the game's internal resolution
    uint32_t screen_width, screen_height; // ReShade's screen
    uint32_t samples;
    wgpu::TextureFormat color_format;
    bool normals;
};

bool run_scene(const Device& d, const Scene& sc, int frames) {
    const wgpu::Device& dev = d.device;
    wgpu::Queue queue = dev.GetQueue();
    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    info.instance = d.instance.Get();
    info.device = dev.Get();
    info.queue = queue.Get();
    rsb::BridgeGpu gpu;
    std::string error;
    if (!gpu.init(info, error)) {
        std::fprintf(stderr, "init failed: %s\n", error.c_str());
        return false;
    }

    // The frame's uniform buffer (GfxService's push_uniform ring) and the scene pass targets.
    wgpu::BufferDescriptor ubd{};
    ubd.size = 4096;
    ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer uniforms = dev.CreateBuffer(&ubd);

    const float nearPlane = 10.0f, farPlane = 50000.0f;
    float proj[16];
    reversed_z_projection(nearPlane, farPlane, proj);

    bool ok = true;
    for (int frame = 0; frame < frames && ok; ++frame) {
        // Resize half-way through, as a window resize would.
        const uint32_t w = frame < frames / 2 ? sc.width : sc.width / 2 + 3;
        const uint32_t h = frame < frames / 2 ? sc.height : sc.height / 2 + 1;
        const uint32_t sw = frame < frames / 2 ? sc.screen_width : sc.screen_width / 2 + 5;
        const uint32_t sh = frame < frames / 2 ? sc.screen_height : sc.screen_height / 2 + 2;
        const auto colorFmt = static_cast<WGPUTextureFormat>(sc.color_format);

        wgpu::Texture sceneColor = texture(dev, w, h, sc.color_format, wgpu::TextureUsage::RenderAttachment, sc.samples);
        wgpu::Texture sceneNormal = texture(dev, w, h, wgpu::TextureFormat::RGB10A2Unorm, wgpu::TextureUsage::RenderAttachment, sc.samples);
        wgpu::Texture sceneDepth = texture(dev, w, h, wgpu::TextureFormat::Depth32Float, wgpu::TextureUsage::RenderAttachment, sc.samples);
        // Snapshots as resolve_pass returns them: single-sample colour, R32Float raw depth.
        wgpu::Texture snapColor = texture(dev, w, h, sc.color_format, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
        wgpu::Texture snapDepth = texture(dev, w, h, wgpu::TextureFormat::R32Float, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
        // Raw depth: a ramp of view distances across x.
        std::vector<float> raw(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float z = nearPlane + (farPlane - nearPlane) * (static_cast<float>(x) / static_cast<float>(w - 1));
                raw[y * w + x] = (proj[10] * -z + proj[14]) / (proj[11] * -z + proj[15]);
            }
        }
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = snapDepth;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;
        const wgpu::Extent3D size{w, h, 1};
        queue.WriteTexture(&dst, raw.data(), raw.size() * 4, &layout, &size);

        rsb::DepthParams dp{};
        dp.a = proj[10];
        dp.b = proj[14];
        dp.c = proj[11];
        dp.d = proj[15];
        dp.near_plane = nearPlane;
        dp.range = farPlane;
        dp.valid = 1.0f;
        queue.WriteBuffer(uniforms, 256, &dp, sizeof(dp));

        wgpu::TextureView snapColorView = snapColor.CreateView();
        wgpu::TextureView snapDepthView = snapDepth.CreateView();
        wgpu::CommandEncoder enc = dev.CreateCommandEncoder();

        GfxComputeContext cctx{};
        cctx.struct_size = sizeof(cctx);
        cctx.device = dev.Get();
        cctx.queue = queue.Get();
        cctx.encoder = enc.Get();
        cctx.uniform_buffer = uniforms.Get();

        GfxDrawContext dctx{};
        dctx.struct_size = sizeof(dctx);
        dctx.device = dev.Get();
        dctx.queue = queue.Get();
        dctx.uniform_buffer = uniforms.Get();
        dctx.layout = GFX_RENDER_TARGET_LAYOUT_INIT;
        dctx.layout.key = (static_cast<uint64_t>(sc.samples) << 32) | (sc.normals ? 2u : 1u) | (static_cast<uint64_t>(colorFmt) << 8);
        dctx.layout.color_attachment_count = sc.normals ? 2 : 1;
        dctx.layout.color_attachments[0] = {GFX_ATTACHMENT_SCENE_COLOR, colorFmt, w, h};
        dctx.layout.color_attachments[1] = {GFX_ATTACHMENT_NORMAL, WGPUTextureFormat_RGB10A2Unorm, w, h};
        dctx.layout.depth_stencil_format = WGPUTextureFormat_Depth32Float;
        dctx.layout.sample_count = sc.samples;

        // The frame's place on the screen (both scale-up modes) and the debug variant.
        const rsb::MapParams maps[3] = {
            rsb::map_params(sw, sh, w, h, rsb::kSpreadEdgeAware, false),
            rsb::map_params(sw, sh, w, h, rsb::kSpreadSimple, false),
            rsb::map_params(sw, sh, w, h, rsb::kSpreadEdgeAware, true),
        };
        for (uint32_t i = 0; i < 3; ++i) {
            queue.WriteBuffer(uniforms, 512 + 256 * i, &maps[i], sizeof(maps[i]));
        }

        // Every point, in frame order, as mod.cpp's run_point drives them (point 0 and 1 convert
        // depth; 2 and 3 reuse it), alternating the scale-up mode.
        rsb::CompositePayload last{};
        for (uint32_t point = 0; point < drb::kPointCount; ++point) {
            rsb::PointTargets* t = gpu.ensure_point(point, sw, sh, colorFmt);
            rsb::DepthTarget* depth = gpu.ensure_depth(w, h, sw, sh);
            if (t == nullptr || depth == nullptr) {
                std::fprintf(stderr, "target creation failed\n");
                return false;
            }
            const uint32_t mapOffset = 512 + 256 * (point % 2);
            rsb::RecordPayload p{};
            p.flags = rsb::kRecordColor | (point < 2 ? rsb::kRecordConvertDepth : 0u);
            p.src_color = snapColorView.Get();
            p.src_depth = snapDepthView.Get();
            p.depth_uniform_offset = 256;
            p.map_uniform_offset = mapOffset;
            p.color = t->color;
            p.color_view = t->color_view;
            p.input = t->input;
            p.color_marker = t->color_marker;
            p.depth_marker = t->depth_marker;
            p.depth_screen = depth->screen;
            p.depth_full_view = depth->full_view;
            p.depth_screen_view = depth->screen_view;
            p.color_format = t->format;
            gpu.record_point(cctx, p);

            rsb::CompositePayload cp{};
            cp.frame = snapColorView.Get();
            cp.result = t->color_view;
            cp.input = t->input_view;
            // Point 3 stands in for a frame without depth (the simple spread over the stand-in).
            cp.depth_full = point == 3 ? nullptr : depth->full_view;
            cp.depth_screen = point == 3 ? nullptr : depth->screen_view;
            cp.uniform_offset = mapOffset;
            last = cp;

            // The scene pass resumes; the composite draws into it.
            wgpu::RenderPassColorAttachment ca[2]{};
            ca[0].view = sceneColor.CreateView();
            ca[0].loadOp = wgpu::LoadOp::Load;
            ca[0].storeOp = wgpu::StoreOp::Store;
            ca[1].view = sceneNormal.CreateView();
            ca[1].loadOp = wgpu::LoadOp::Load;
            ca[1].storeOp = wgpu::StoreOp::Store;
            wgpu::RenderPassDepthStencilAttachment da{};
            da.view = sceneDepth.CreateView();
            da.depthLoadOp = wgpu::LoadOp::Load;
            da.depthStoreOp = wgpu::StoreOp::Store;
            wgpu::RenderPassDescriptor rp{};
            rp.colorAttachmentCount = sc.normals ? 2 : 1;
            rp.colorAttachments = ca;
            rp.depthStencilAttachment = &da;
            wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
            dctx.pass = pass.Get();
            gpu.composite(dctx, cp);
            if (point == drb::kPointCount - 1) {
                // The change-layer debug view, drawn at the end of the frame (mod.cpp draw_debug_view).
                last.uniform_offset = 512 + 256 * 2;
                gpu.composite(dctx, last);
            }
            pass.End();
        }

        // Depth-only and clear-only payloads (point 1 with only later points enabled; no depth).
        rsb::DepthTarget* depth = gpu.depth();
        rsb::RecordPayload p{};
        p.flags = rsb::kRecordClearDepth;
        p.depth_full_view = depth->full_view;
        p.depth_screen_view = depth->screen_view;
        gpu.record_point(cctx, p);
        p.flags = rsb::kRecordConvertDepth;
        p.src_depth = snapDepthView.Get();
        p.depth_uniform_offset = 256;
        p.map_uniform_offset = 512;
        gpu.record_point(cctx, p);

        // Read the depth texture ReShade gets back on the last frame.
        wgpu::Buffer readback;
        const uint32_t rowBytes = (sw * 4 + 255) / 256 * 256;
        const wgpu::Extent3D screenSize{sw, sh, 1};
        if (frame == frames - 1) {
            wgpu::BufferDescriptor rbd{};
            rbd.size = static_cast<uint64_t>(rowBytes) * sh;
            rbd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            readback = dev.CreateBuffer(&rbd);
            wgpu::TexelCopyTextureInfo src{};
            wgpuTextureAddRef(depth->screen); // the C++ wrapper releases what it holds
            src.texture = wgpu::Texture::Acquire(depth->screen);
            wgpu::TexelCopyBufferInfo bdst{};
            bdst.buffer = readback;
            bdst.layout.bytesPerRow = rowBytes;
            bdst.layout.rowsPerImage = sh;
            enc.CopyTextureToBuffer(&src, &bdst, &screenSize);
        }

        wgpu::CommandBuffer cb = enc.Finish();
        queue.Submit(1, &cb);
        gpu.tick_retired();

        if (readback) {
            bool mapped = false;
            d.instance.WaitAny(readback.MapAsync(wgpu::MapMode::Read, 0, readback.GetSize(), wgpu::CallbackMode::WaitAnyOnly,
                                   [&](wgpu::MapAsyncStatus s, wgpu::StringView) { mapped = s == wgpu::MapAsyncStatus::Success; }),
                UINT64_MAX);
            if (!mapped) {
                std::fprintf(stderr, "readback failed\n");
                return false;
            }
            // The null backend executes nothing, so the contents are not the shader's output; this
            // only proves the readback path. The encoding is checked on the CPU below instead.
            readback.Unmap();
        }
    }
    gpu.release();
    return ok && g_errors.load() == 0;
}

// The frame's place on the screen against Aurora's calculate_present_viewport (the cases worked out
// by hand from its code).
bool check_placement() {
    struct Case {
        uint32_t sw, sh, fw, fh;
        float x, y, w, h;
    };
    const Case cases[] = {
        {3440, 1440, 6421, 2688, 0, 0, 3440, 1440},   // 6x internal resolution on a 21:9 screen
        {1920, 1080, 1920, 1080, 0, 0, 1920, 1080},   // same size
        {1920, 1080, 1440, 1080, 240, 0, 1440, 1080}, // 4:3 frame: pillarboxed
        {1080, 1920, 1920, 1080, 0, 656, 1080, 608},  // wide frame on a tall screen: letterboxed
        {0, 0, 1280, 720, 0, 0, 1280, 720},           // screen unknown: the frame's own size
    };
    bool ok = true;
    for (const Case& c : cases) {
        const rsb::MapParams m = rsb::map_params(c.sw, c.sh, c.fw, c.fh, rsb::kSpreadEdgeAware, false);
        if (m.vp_offset[0] != c.x || m.vp_offset[1] != c.y || m.vp_size[0] != c.w || m.vp_size[1] != c.h) {
            std::fprintf(stderr, "placement %ux%u on %ux%u: got %g,%g %gx%g, expected %g,%g %gx%g\n", c.fw, c.fh, c.sw, c.sh,
                m.vp_offset[0], m.vp_offset[1], m.vp_size[0], m.vp_size[1], c.x, c.y, c.w, c.h);
            ok = false;
        }
    }
    std::printf("frame placement: %s\n", ok ? "matches Aurora's present" : "MISMATCH");
    return ok;
}

// The encoding fs_depth implements, evaluated on the CPU, against ReShade's decode.
bool check_encoding() {
    const float F = drb::kDepthFarPlane;
    float worst = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float l = static_cast<float>(i) / 1000.0f;
        const float stored = 1.0f - l * F / (1.0f + l * (F - 1.0f));
        worst = std::fmax(worst, std::fabs(reshade_linearize(stored) - l));
    }
    std::printf("depth encoding: worst |decode(encode(L)) - L| = %g\n", static_cast<double>(worst));
    return worst < 1e-4f;
}


// --- The scaling shaders, run for real ------------------------------------------------------------

struct GpuRun {
    const Device& d;
    wgpu::Queue queue;
    wgpu::Buffer uniforms;
    wgpu::RenderPipeline blit; // stands in for ReShade writing the hand-over texture (a render target)

    void make_blit() {
        const char* code = R"(
@group(0) @binding(0) var src : texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
	let x = f32((i << 1u) & 2u);
	let y = f32(i & 2u);
	return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}
@fragment fn fs(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {
	return textureLoad(src, vec2<i32>(p.xy), 0);
}
)";
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = code;
        wgpu::ShaderModuleDescriptor md{};
        md.nextInChain = &wgsl;
        wgpu::ShaderModule module = d.device.CreateShaderModule(&md);
        wgpu::ColorTargetState target{};
        target.format = wgpu::TextureFormat::RGBA8Unorm;
        wgpu::FragmentState fs{};
        fs.module = module;
        fs.entryPoint = "fs";
        fs.targetCount = 1;
        fs.targets = &target;
        wgpu::RenderPipelineDescriptor rpd{};
        rpd.vertex.module = module;
        rpd.vertex.entryPoint = "vs";
        rpd.fragment = &fs;
        blit = d.device.CreateRenderPipeline(&rpd);
    }

    // Draws `data` (w x h RGBA8) into `view`, as ReShade's techniques write the hand-over texture.
    void draw_into(WGPUTextureView view, const std::vector<uint8_t>& data, uint32_t w, uint32_t h) {
        wgpu::Texture staging = texture(d.device, w, h, wgpu::TextureFormat::RGBA8Unorm,
            wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
        upload(staging.Get(), data.data(), w, h, 4);
        wgpu::BindGroupEntry e{};
        e.binding = 0;
        e.textureView = staging.CreateView();
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = blit.GetBindGroupLayout(0);
        bgd.entryCount = 1;
        bgd.entries = &e;
        wgpu::BindGroup group = d.device.CreateBindGroup(&bgd);
        wgpu::CommandEncoder enc = d.device.CreateCommandEncoder();
        wgpu::RenderPassColorAttachment ca{};
        wgpuTextureViewAddRef(view);
        ca.view = wgpu::TextureView::Acquire(view);
        ca.loadOp = wgpu::LoadOp::Load;
        ca.storeOp = wgpu::StoreOp::Store;
        wgpu::RenderPassDescriptor rp{};
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
        pass.SetPipeline(blit);
        pass.SetBindGroup(0, group);
        pass.Draw(3);
        pass.End();
        submit(enc);
    }

    void submit(wgpu::CommandEncoder& enc) {
        wgpu::CommandBuffer cb = enc.Finish();
        queue.Submit(1, &cb);
    }

    void upload(WGPUTexture tex, const void* data, uint32_t w, uint32_t h, uint32_t bpp) {
        wgpu::TexelCopyTextureInfo dst{};
        wgpuTextureAddRef(tex);
        dst.texture = wgpu::Texture::Acquire(tex);
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = w * bpp;
        layout.rowsPerImage = h;
        const wgpu::Extent3D size{w, h, 1};
        queue.WriteTexture(&dst, data, static_cast<size_t>(w) * h * bpp, &layout, &size);
    }

    // Reads a texture (4 bytes per texel) back, rows packed.
    std::vector<uint8_t> download(WGPUTexture tex, uint32_t w, uint32_t h) {
        const uint32_t rowBytes = (w * 4 + 255) / 256 * 256;
        wgpu::BufferDescriptor bd{};
        bd.size = static_cast<uint64_t>(rowBytes) * h;
        bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        wgpu::Buffer buffer = d.device.CreateBuffer(&bd);
        wgpu::TexelCopyTextureInfo src{};
        wgpuTextureAddRef(tex);
        src.texture = wgpu::Texture::Acquire(tex);
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = buffer;
        dst.layout.bytesPerRow = rowBytes;
        dst.layout.rowsPerImage = h;
        const wgpu::Extent3D size{w, h, 1};
        wgpu::CommandEncoder enc = d.device.CreateCommandEncoder();
        enc.CopyTextureToBuffer(&src, &dst, &size);
        submit(enc);
        bool mapped = false;
        d.instance.WaitAny(buffer.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::WaitAnyOnly,
                               [&](wgpu::MapAsyncStatus st, wgpu::StringView) { mapped = st == wgpu::MapAsyncStatus::Success; }),
            UINT64_MAX);
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
        if (mapped) {
            const auto* data = static_cast<const uint8_t*>(buffer.GetConstMappedRange());
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, data + static_cast<size_t>(y) * rowBytes, w * 4);
            }
            buffer.Unmap();
        }
        return out;
    }
};

// Stored depth for linear depth L (fs_depth's encoding).
float encode_depth(float l) {
    const float F = drb::kDepthFarPlane;
    return 1.0f - l * F / (1.0f + l * (F - 1.0f));
}

// One hand-over on a real adapter: frame (fw x fh, RGBA8) and raw depth (R32Float, passed through
// unconverted) scaled to a sw x sh screen; ReShade stood in for by `effect` (input -> result, RGBA8
// texels); then the composite in `mode` (or its debug view) into a frame-size target. Returns the
// hand-over input, the screen depth and the composited frame.
struct HandOver {
    std::vector<uint8_t> input;
    std::vector<float> depth;
    std::vector<uint8_t> composited;
};
template <class Effect>
HandOver hand_over(GpuRun& run, rsb::BridgeGpu& gpu, uint32_t fw, uint32_t fh, uint32_t sw, uint32_t sh,
    const std::vector<uint8_t>& frame, const std::vector<float>& rawDepth, Effect effect, rsb::SpreadMode mode, bool debug) {
    const wgpu::Device& dev = run.d.device;
    const WGPUTextureFormat fmt = WGPUTextureFormat_RGBA8Unorm;
    wgpu::Texture snapColor = texture(dev, fw, fh, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
    wgpu::Texture snapDepth = texture(dev, fw, fh, wgpu::TextureFormat::R32Float, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
    wgpu::Texture target = texture(dev, fw, fh, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);
    wgpu::Texture targetDepth = texture(dev, fw, fh, wgpu::TextureFormat::Depth32Float, wgpu::TextureUsage::RenderAttachment);
    run.upload(snapColor.Get(), frame.data(), fw, fh, 4);
    run.upload(snapDepth.Get(), rawDepth.data(), fw, fh, 4);
    wgpu::TextureView snapColorView = snapColor.CreateView();
    wgpu::TextureView snapDepthView = snapDepth.CreateView();

    rsb::PointTargets* t = gpu.ensure_point(0, sw, sh, fmt);
    rsb::DepthTarget* depth = gpu.ensure_depth(fw, fh, sw, sh);
    rsb::DepthParams dp{}; // valid = 0: the raw depth passes through unconverted
    const rsb::MapParams mp = rsb::map_params(sw, sh, fw, fh, mode, debug);
    run.queue.WriteBuffer(run.uniforms, 0, &dp, sizeof(dp));
    run.queue.WriteBuffer(run.uniforms, 256, &mp, sizeof(mp));

    // The record step: depth, scale-down, input copy, markers.
    wgpu::CommandEncoder enc = dev.CreateCommandEncoder();
    GfxComputeContext cctx{};
    cctx.struct_size = sizeof(cctx);
    cctx.device = dev.Get();
    cctx.queue = run.queue.Get();
    cctx.encoder = enc.Get();
    cctx.uniform_buffer = run.uniforms.Get();
    rsb::RecordPayload p{};
    p.flags = rsb::kRecordColor | rsb::kRecordConvertDepth;
    p.src_color = snapColorView.Get();
    p.src_depth = snapDepthView.Get();
    p.depth_uniform_offset = 0;
    p.map_uniform_offset = 256;
    p.color = t->color;
    p.color_view = t->color_view;
    p.input = t->input;
    p.color_marker = t->color_marker;
    p.depth_marker = t->depth_marker;
    p.depth_screen = depth->screen;
    p.depth_full_view = depth->full_view;
    p.depth_screen_view = depth->screen_view;
    p.color_format = fmt;
    gpu.record_point(cctx, p);
    run.submit(enc);

    HandOver out;
    out.input = run.download(t->color, sw, sh);
    const std::vector<uint8_t> depthBytes = run.download(depth->screen, sw, sh);
    out.depth.resize(static_cast<size_t>(sw) * sh);
    std::memcpy(out.depth.data(), depthBytes.data(), depthBytes.size());

    // ReShade's turn: the effect's result goes into the hand-over texture.
    std::vector<uint8_t> result = out.input;
    effect(result, out.depth, sw, sh);
    run.draw_into(t->color_view, result, sw, sh);

    // The composite, into a target whose alpha starts at 0.25 (it must stay).
    enc = dev.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment ca{};
    ca.view = target.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 0.25};
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = targetDepth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = 0.0f;
    wgpu::RenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &da;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    GfxDrawContext dctx{};
    dctx.struct_size = sizeof(dctx);
    dctx.device = dev.Get();
    dctx.queue = run.queue.Get();
    dctx.uniform_buffer = run.uniforms.Get();
    dctx.pass = pass.Get();
    dctx.layout = GFX_RENDER_TARGET_LAYOUT_INIT;
    dctx.layout.key = 0x5ca1e;
    dctx.layout.color_attachment_count = 1;
    dctx.layout.color_attachments[0] = {GFX_ATTACHMENT_SCENE_COLOR, fmt, fw, fh};
    dctx.layout.depth_stencil_format = WGPUTextureFormat_Depth32Float;
    dctx.layout.sample_count = 1;
    rsb::CompositePayload cp{};
    cp.frame = snapColorView.Get();
    cp.result = t->color_view;
    cp.input = t->input_view;
    cp.depth_full = depth->full_view;
    cp.depth_screen = depth->screen_view;
    cp.uniform_offset = 256;
    gpu.composite(dctx, cp);
    pass.End();
    run.submit(enc);
    out.composited = run.download(target.Get(), fw, fh);
    return out;
}

// Area average of a w x h single-channel image over [x0, x1) x [y0, y1), the reference for fs_down.
float area_average(const std::vector<float>& img, uint32_t w, float x0, float x1, float y0, float y1) {
    float sum = 0.0f, total = 0.0f;
    for (int y = static_cast<int>(std::floor(y0)); y < static_cast<int>(std::ceil(y1)); ++y) {
        const float wy = std::fmin(y1, y + 1.0f) - std::fmax(y0, static_cast<float>(y));
        for (int x = static_cast<int>(std::floor(x0)); x < static_cast<int>(std::ceil(x1)); ++x) {
            const float wx = std::fmin(x1, x + 1.0f) - std::fmax(x0, static_cast<float>(x));
            sum += wx * wy * img[static_cast<size_t>(y) * w + x];
            total += wx * wy;
        }
    }
    return sum / total;
}

bool check_on_gpu() {
    Device d = make_device(true, wgpu::BackendType::Vulkan);
    if (!d.device) {
        std::printf("scaling shaders: skipped (no Vulkan adapter; on Linux install mesa-vulkan-drivers)\n");
        return true;
    }
    wgpu::BufferDescriptor ubd{};
    ubd.size = 1024;
    ubd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    GpuRun run{d, d.device.GetQueue(), d.device.CreateBuffer(&ubd), nullptr};
    run.make_blit();
    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    info.instance = d.instance.Get();
    info.device = d.device.Get();
    info.queue = run.queue.Get();
    rsb::BridgeGpu gpu;
    std::string error;
    if (!gpu.init(info, error)) {
        std::fprintf(stderr, "init failed on the Vulkan adapter: %s\n", error.c_str());
        return false;
    }
    bool ok = true;
    const auto fail = [&](const char* what, int x, int y, double got, double expected) {
        std::fprintf(stderr, "  %s at %d,%d: got %g, expected %g\n", what, x, y, got, expected);
        ok = false;
    };
    const auto no_effect = [](std::vector<uint8_t>&, const std::vector<float>&, uint32_t, uint32_t) {};

    // 1. Scale-down at 1.5x against the CPU area average; depth one sample per screen pixel.
    {
        const uint32_t fw = 12, fh = 9, sw = 8, sh = 6;
        std::vector<uint8_t> frame(fw * fh * 4);
        std::vector<float> red(fw * fh), raw(fw * fh);
        for (uint32_t i = 0; i < fw * fh; ++i) {
            const uint8_t v = static_cast<uint8_t>((i * 97u + 13u) % 256u);
            frame[i * 4 + 0] = v;
            frame[i * 4 + 1] = static_cast<uint8_t>(255 - v);
            frame[i * 4 + 2] = 0;
            frame[i * 4 + 3] = 255;
            red[i] = v;
            raw[i] = static_cast<float>(i);
        }
        const HandOver h = hand_over(run, gpu, fw, fh, sw, sh, frame, raw, no_effect, rsb::kSpreadSimple, false);
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) {
                const float expected = area_average(red, fw, x * 1.5f, (x + 1) * 1.5f, y * 1.5f, (y + 1) * 1.5f);
                const float got = h.input[(y * sw + x) * 4];
                if (std::fabs(got - expected) > 1.0f) {
                    fail("scale-down (red)", x, y, got, expected);
                }
                const uint32_t fx = static_cast<uint32_t>((x + 0.5f) * 1.5f), fy = static_cast<uint32_t>((y + 0.5f) * 1.5f);
                if (h.depth[y * sw + x] != raw[fy * fw + fx]) {
                    fail("screen depth", x, y, h.depth[y * sw + x], raw[fy * fw + fx]);
                }
            }
        }
        std::printf("scaling shaders: scale-down and screen depth at 1.5x %s\n", ok ? "match the CPU" : "WRONG");
    }

    // 2. At the screen's size the round trip is exact: the composite gives ReShade's result.
    {
        const uint32_t w = 10, h = 6;
        std::vector<uint8_t> frame(w * h * 4);
        for (uint32_t i = 0; i < w * h * 4; ++i) {
            frame[i] = static_cast<uint8_t>((i * 31u + 7u) % 256u);
        }
        std::vector<float> raw(w * h, 0.5f);
        const auto invert = [](std::vector<uint8_t>& px, const std::vector<float>&, uint32_t sw, uint32_t sh) {
            for (uint32_t i = 0; i < sw * sh; ++i) {
                for (int c = 0; c < 3; ++c) {
                    px[i * 4 + c] = static_cast<uint8_t>(255 - px[i * 4 + c]);
                }
            }
        };
        const HandOver r = hand_over(run, gpu, w, h, w, h, frame, raw, invert, rsb::kSpreadEdgeAware, false);
        const bool before = ok;
        for (uint32_t i = 0; i < w * h; ++i) {
            for (int c = 0; c < 3; ++c) {
                const int expected = 255 - frame[i * 4 + c];
                if (std::abs(r.composited[i * 4 + c] - expected) > 1) {
                    fail("identity round trip", static_cast<int>(i % w), static_cast<int>(i / w), r.composited[i * 4 + c], expected);
                }
            }
            if (std::abs(r.composited[i * 4 + 3] - 64) > 1) {
                fail("alpha kept", static_cast<int>(i % w), static_cast<int>(i / w), r.composited[i * 4 + 3], 64);
            }
        }
        std::printf("scaling shaders: round trip at the screen's size %s\n", ok == before ? "exact" : "WRONG");
    }

    // 3. A silhouette at 1.5x: near on the left of frame column 6 (screen column 4), far on the
    //    right; the effect darkens near screen pixels by 128 (ambient occlusion on the object).
    {
        const uint32_t fw = 12, fh = 3, sw = 8, sh = 2;
        std::vector<uint8_t> frame(fw * fh * 4, 204);
        std::vector<float> raw(fw * fh);
        const float nearStored = encode_depth(0.1f), farStored = encode_depth(0.9f);
        for (uint32_t y = 0; y < fh; ++y) {
            for (uint32_t x = 0; x < fw; ++x) {
                raw[y * fw + x] = x < 6 ? nearStored : farStored;
            }
        }
        const auto darken_near = [nearStored](std::vector<uint8_t>& px, const std::vector<float>& depth, uint32_t sw2, uint32_t sh2) {
            for (uint32_t i = 0; i < sw2 * sh2; ++i) {
                if (depth[i] == nearStored) {
                    for (int c = 0; c < 3; ++c) {
                        px[i * 4 + c] = static_cast<uint8_t>(px[i * 4 + c] - 128);
                    }
                }
            }
        };
        const bool before = ok;
        const HandOver edge = hand_over(run, gpu, fw, fh, sw, sh, frame, raw, darken_near, rsb::kSpreadEdgeAware, false);
        const HandOver simple = hand_over(run, gpu, fw, fh, sw, sh, frame, raw, darken_near, rsb::kSpreadSimple, false);
        const HandOver debug = hand_over(run, gpu, fw, fh, sw, sh, frame, raw, darken_near, rsb::kSpreadEdgeAware, true);
        for (uint32_t y = 0; y < fh; ++y) {
            for (uint32_t x = 0; x < fw; ++x) {
                const size_t i = (static_cast<size_t>(y) * fw + x) * 4;
                const int expected = x < 6 ? 76 : 204;
                if (std::abs(edge.composited[i] - expected) > 1) {
                    fail("edge-aware scale-up", static_cast<int>(x), static_cast<int>(y), edge.composited[i], expected);
                }
                const int expectedDebug = x < 6 ? 0 : 128;
                if (std::abs(debug.composited[i] - expectedDebug) > 1) {
                    fail("change-layer debug view", static_cast<int>(x), static_cast<int>(y), debug.composited[i], expectedDebug);
                }
            }
        }
        // The simple scale-up bleeds the darkening onto the far side of the edge (column 6).
        if (simple.composited[6 * 4] > 204 - 10) {
            fail("simple scale-up (expected to bleed)", 6, 0, simple.composited[6 * 4], 204 - 21);
        }
        std::printf("scaling shaders: silhouette at 1.5x: edge-aware keeps the change on its side (simple bleeds %d "
                    "levels across), debug view %s\n",
            204 - simple.composited[6 * 4], ok == before ? "correct" : "WRONG");
    }

    d.device.Tick();
    gpu.release();
    return ok && g_errors.load() == 0;
}

} // namespace

int main() {
    if (!check_encoding()) {
        std::fprintf(stderr, "FAIL: depth encoding does not round-trip through ReShade's decode\n");
        return 1;
    }
    if (!check_placement()) {
        std::fprintf(stderr, "FAIL: the frame's placement differs from Dusklight's present\n");
        return 1;
    }
    const Scene scenes[] = {
        {1280, 720, 1280, 720, 1, wgpu::TextureFormat::RGBA8Unorm, false},  // no scaling
        {2880, 1620, 1920, 1080, 1, wgpu::TextureFormat::BGRA8Unorm, true}, // 1.5x
        {1600, 900, 1280, 960, 4, wgpu::TextureFormat::RGBA8Unorm, true},   // letterboxed, MSAA
        {960, 540, 1920, 1080, 1, wgpu::TextureFormat::RGBA8Unorm, false},  // frame smaller than the screen
    };
    for (const bool core : {true, false}) {
        Device d = make_device(core);
        if (!d.device) {
            std::fprintf(stderr, "no null-backend device\n");
            return 1;
        }
        for (const Scene& sc : scenes) {
            if (!run_scene(d, sc, 6)) {
                std::fprintf(stderr, "FAIL: %ux%u on %ux%u, %u samples (%s device)\n", sc.width, sc.height, sc.screen_width,
                    sc.screen_height, sc.samples, core ? "core" : "compat");
                return 1;
            }
            std::printf("ok (%s device): %ux%u on a %ux%u screen, %u samples, %s, normals %s\n", core ? "core" : "compat",
                sc.width, sc.height, sc.screen_width, sc.screen_height, sc.samples,
                sc.color_format == wgpu::TextureFormat::RGBA8Unorm ? "RGBA8" : "BGRA8", sc.normals ? "yes" : "no");
        }
        d.device.Tick();
        if (g_errors.load() != 0) {
            std::fprintf(stderr, "FAIL: %d WebGPU errors\n", g_errors.load());
            return 1;
        }
    }
    if (!check_on_gpu()) {
        std::fprintf(stderr, "FAIL: the scaling shaders' output is wrong\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
