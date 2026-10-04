// Drives the mod's GPU half (src/bridge_gpu.cpp) on Dawn's null backend, standing in for the game:
// a device configured like Aurora's (an uncaptured error is fatal there, so any error fails this
// run), fake scene snapshots at every insertion point, the GfxService compute and draw callbacks
// in frame order, a resize, both scene colour formats, a multisampled scene pass with a normal
// attachment, and all of it again on a compatibility-mode device. It then reads the depth texture back and decodes it the way ReShade.fxh's
// GetLinearizedDepth does under drb::kDepthDefinitions, to check that ReShade will see the linear
// depth the mod meant to give it.
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
Device make_device(bool core) {
    Device d;
    const wgpu::InstanceFeatureName instanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor id{};
    id.requiredFeatureCount = 1;
    id.requiredFeatures = instanceFeatures;
    d.instance = wgpu::CreateInstance(&id);
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = wgpu::BackendType::Null;
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
    uint32_t width, height, samples;
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
        dctx.layout = GFX_RENDER_TARGET_LAYOUT_INIT;
        dctx.layout.key = (static_cast<uint64_t>(sc.samples) << 32) | (sc.normals ? 2u : 1u) | (static_cast<uint64_t>(colorFmt) << 8);
        dctx.layout.color_attachment_count = sc.normals ? 2 : 1;
        dctx.layout.color_attachments[0] = {GFX_ATTACHMENT_SCENE_COLOR, colorFmt, w, h};
        dctx.layout.color_attachments[1] = {GFX_ATTACHMENT_NORMAL, WGPUTextureFormat_RGB10A2Unorm, w, h};
        dctx.layout.depth_stencil_format = WGPUTextureFormat_Depth32Float;
        dctx.layout.sample_count = sc.samples;

        // Every point, in frame order, as mod.cpp's run_point drives them (point 0 and 1 convert
        // depth; 2 and 3 reuse it).
        for (uint32_t point = 0; point < drb::kPointCount; ++point) {
            rsb::PointTargets* t = gpu.ensure_point(point, w, h, colorFmt);
            rsb::DepthTarget* depth = gpu.ensure_depth(w, h);
            if (t == nullptr || depth == nullptr) {
                std::fprintf(stderr, "target creation failed\n");
                return false;
            }
            rsb::RecordPayload p{};
            p.flags = rsb::kRecordColor | (point < 2 ? rsb::kRecordConvertDepth : 0u);
            p.src_color = snapColorView.Get();
            p.src_depth = snapDepthView.Get();
            p.uniform_offset = 256;
            p.color = t->color;
            p.color_view = t->color_view;
            p.color_marker = t->color_marker;
            p.depth = depth->texture;
            p.depth_view = depth->view;
            p.depth_marker = t->depth_marker;
            p.color_format = t->format;
            gpu.record_point(cctx, p);

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
            gpu.composite(dctx, rsb::CompositePayload{t->color_view});
            pass.End();
        }

        // Depth-only and clear-only payloads (point 1 with only later points enabled; no depth).
        rsb::DepthTarget* depth = gpu.depth();
        rsb::RecordPayload p{};
        p.flags = rsb::kRecordClearDepth;
        p.depth = depth->texture;
        p.depth_view = depth->view;
        gpu.record_point(cctx, p);
        p.flags = rsb::kRecordConvertDepth;
        p.src_depth = snapDepthView.Get();
        p.uniform_offset = 256;
        gpu.record_point(cctx, p);

        // Read the depth texture back on the last frame.
        wgpu::Buffer readback;
        const uint32_t rowBytes = (w * 4 + 255) / 256 * 256;
        if (frame == frames - 1) {
            wgpu::BufferDescriptor rbd{};
            rbd.size = static_cast<uint64_t>(rowBytes) * h;
            rbd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            readback = dev.CreateBuffer(&rbd);
            wgpu::TexelCopyTextureInfo src{};
            wgpuTextureAddRef(depth->texture); // the C++ wrapper releases what it holds
            src.texture = wgpu::Texture::Acquire(depth->texture);
            wgpu::TexelCopyBufferInfo bdst{};
            bdst.buffer = readback;
            bdst.layout.bytesPerRow = rowBytes;
            bdst.layout.rowsPerImage = h;
            enc.CopyTextureToBuffer(&src, &bdst, &size);
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

} // namespace

int main() {
    if (!check_encoding()) {
        std::fprintf(stderr, "FAIL: depth encoding does not round-trip through ReShade's decode\n");
        return 1;
    }
    const Scene scenes[] = {
        {1280, 720, 1, wgpu::TextureFormat::RGBA8Unorm, false},
        {1920, 1080, 1, wgpu::TextureFormat::BGRA8Unorm, true},
        {1600, 900, 4, wgpu::TextureFormat::RGBA8Unorm, true},
    };
    for (const bool core : {true, false}) {
        Device d = make_device(core);
        if (!d.device) {
            std::fprintf(stderr, "no null-backend device\n");
            return 1;
        }
        for (const Scene& sc : scenes) {
            if (!run_scene(d, sc, 6)) {
                std::fprintf(stderr, "FAIL: %ux%u, %u samples (%s device)\n", sc.width, sc.height, sc.samples, core ? "core" : "compat");
                return 1;
            }
            std::printf("ok (%s device): %ux%u, %u samples, %s, normals %s\n", core ? "core" : "compat", sc.width, sc.height,
                sc.samples, sc.color_format == wgpu::TextureFormat::RGBA8Unorm ? "RGBA8" : "BGRA8", sc.normals ? "yes" : "no");
        }
        d.device.Tick();
        if (g_errors.load() != 0) {
            std::fprintf(stderr, "FAIL: %d WebGPU errors\n", g_errors.load());
            return 1;
        }
    }
    std::printf("PASS\n");
    return 0;
}
