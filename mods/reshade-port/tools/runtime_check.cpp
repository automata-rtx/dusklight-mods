// Drives the mod's Runtime (fx_runtime.cpp) end to end on Dawn's null backend, standing in for the
// game: a "game" device configured like Aurora's, a base folder holding reshade-shaders, frames
// with fake scene snapshots at every insertion point, and the GfxService callbacks' calls into the
// runtime. Every technique found is enabled. Any WebGPU error on the game device fails the run,
// because in the game it would be fatal.
//
//   runtime_check <base dir> [frames]
//
// Build recipe: docs/reshade_port.md "Offline checker".

#include "fx_runtime.hpp"

#include <webgpu/webgpu_cpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::atomic<int> g_gameErrors{0};

struct Device {
    wgpu::Instance instance;
    wgpu::Device device;
};

Device make_device() {
    Device d;
    const wgpu::InstanceFeatureName instanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor id{};
    id.requiredFeatureCount = 1;
    id.requiredFeatures = instanceFeatures;
    d.instance = wgpu::CreateInstance(&id);
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = wgpu::BackendType::Null;
    wgpu::Adapter adapter;
    d.instance.WaitAny(d.instance.RequestAdapter(&ao, wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::RequestAdapterStatus, wgpu::Adapter a, wgpu::StringView) { adapter = std::move(a); }),
        UINT64_MAX);
    if (!adapter) {
        return d;
    }
    wgpu::DeviceDescriptor dd{};
    const wgpu::FeatureName core = wgpu::FeatureName::CoreFeaturesAndLimits;
    dd.requiredFeatureCount = 1;
    dd.requiredFeatures = &core;
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m) {
        ++g_gameErrors;
        std::fprintf(stderr, "GAME DEVICE ERROR (fatal in Aurora): %.*s\n", static_cast<int>(m.length), m.data);
    });
    d.instance.WaitAny(adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::RequestDeviceStatus, wgpu::Device dev, wgpu::StringView) { d.device = std::move(dev); }),
        UINT64_MAX);
    return d;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: runtime_check <base dir> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 3;
    Device dev = make_device();
    if (!dev.device) {
        std::fprintf(stderr, "no Dawn null device\n");
        return 2;
    }
    const rsp::GpuApi game{dev.instance.Get(), dev.device.Get(), dev.device.GetQueue().Get()};
    constexpr uint32_t W = 1280, H = 720;

    wgpu::TextureDescriptor td{};
    td.size = {W, H, 1};
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.format = wgpu::TextureFormat::BGRA8Unorm;
    wgpu::Texture colorTex = dev.device.CreateTexture(&td);
    wgpu::TextureView color = colorTex.CreateView();
    td.format = wgpu::TextureFormat::R32Float;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment;
    wgpu::Texture depthTex = dev.device.CreateTexture(&td);
    wgpu::TextureView depth = depthTex.CreateView();

    auto runtime = std::make_unique<rsp::Runtime>();
    std::string errors;
    runtime->init(game, argv[1], errors);

    // Wait for every effect to compile.
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        runtime->update(W, H, WGPUTextureFormat_BGRA8Unorm);
        const rsp::RuntimeStatus s = runtime->status();
        if (s.compile_total != 0 && s.compile_done == s.compile_total && s.effects_found == s.compile_total) {
            break;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(20)) {
            std::fprintf(stderr, "timed out compiling\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("compiled %zu effects (%zu failed) in %.1f s\n", runtime->status().effects_found,
        runtime->status().effects_failed,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    // Enable everything, spread over the insertion points, and wait for the GPU builds.
    auto& techniques = runtime->techniques();
    for (size_t i = 0; i < techniques.size(); ++i) {
        if (techniques[i].present) {
            runtime->set_technique_point(i, static_cast<rsp::InsertionPoint>(i % rsp::kInsertionPointCount));
            runtime->set_technique_enabled(i, true);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (;;) {
        runtime->update(W, H, WGPUTextureFormat_BGRA8Unorm);
        size_t pending = 0;
        for (const auto& t : runtime->techniques()) {
            if (!t.enabled || !t.present) {
                continue;
            }
            rsp::EffectState* e = runtime->effect(t.file);
            if (e == nullptr || e->build == nullptr || !e->build->gpu_done) {
                ++pending;
            }
        }
        if (pending == 0) {
            break;
        }
        if (std::chrono::steady_clock::now() - t1 > std::chrono::minutes(20)) {
            std::fprintf(stderr, "timed out building (%zu pending)\n", pending);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("built enabled effects in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());

    size_t ok = 0, bad = 0;
    for (const auto& [file, state] : runtime->effects()) {
        const auto& b = *state.build;
        for (size_t i = 0; i < b.technique_ok.size(); ++i) {
            (b.technique_ok[i] ? ok : bad)++;
        }
        if (!b.compile_ok || std::find(b.technique_ok.begin(), b.technique_ok.end(), 0) != b.technique_ok.end()) {
            std::string m = b.messages;
            for (char& c : m) {
                if (c == '\n') {
                    c = ' ';
                }
            }
            std::printf("  %-32s %s\n", file.c_str(), m.substr(0, 220).c_str());
        }
    }
    std::printf("techniques usable %zu, rejected %zu\n", ok, bad);

    // Frames: every insertion point, as the mod's GfxService glue drives it.
    rsp::CameraDepth cam;
    const float n = 1.0f, f = 50000.0f;
    cam.proj_from_view[0] = 1.0f;
    cam.proj_from_view[5] = 1.0f;
    cam.proj_from_view[10] = n / (f - n);
    cam.proj_from_view[11] = -1.0f;
    cam.proj_from_view[14] = f * n / (f - n);
    cam.near_plane = n;
    cam.far_plane = f;
    // The scene pass the composite draws into, shaped like Aurora's with the normal attachment on:
    // scene colour, normals, depth. The pipeline is built as mod.cpp ensure_composite builds it.
    td.usage = wgpu::TextureUsage::RenderAttachment;
    td.format = wgpu::TextureFormat::BGRA8Unorm;
    wgpu::TextureView sceneColor = dev.device.CreateTexture(&td).CreateView();
    td.format = wgpu::TextureFormat::RGB10A2Unorm;
    wgpu::TextureView sceneNormal = dev.device.CreateTexture(&td).CreateView();
    td.format = wgpu::TextureFormat::Depth32Float;
    wgpu::TextureView sceneDepth = dev.device.CreateTexture(&td).CreateView();
    const rsp::GpuShared* shared = runtime->game_shared();
    WGPUColorTargetState targets[2] = {WGPU_COLOR_TARGET_STATE_INIT, WGPU_COLOR_TARGET_STATE_INIT};
    targets[0].format = WGPUTextureFormat_BGRA8Unorm;
    targets[0].writeMask = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue;
    targets[1].format = WGPUTextureFormat_RGB10A2Unorm;
    targets[1].writeMask = WGPUColorWriteMask_None;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = shared->utility_module();
    fs.entryPoint = {"fs_copy", WGPU_STRLEN};
    fs.targetCount = 2;
    fs.targets = targets;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.layout = shared->single_texture_pipeline_layout();
    rpd.vertex.module = shared->utility_module();
    rpd.vertex.entryPoint = {"vs_fullscreen", WGPU_STRLEN};
    rpd.depthStencil = &ds;
    rpd.fragment = &fs;
    WGPURenderPipeline composite = wgpuDeviceCreateRenderPipeline(game.device, &rpd);

    int planned = 0;
    for (int frame = 0; frame < frames; ++frame) {
        runtime->update(W, H, WGPUTextureFormat_BGRA8Unorm);
        runtime->set_camera(cam);
        wgpu::CommandEncoder encoder = dev.device.CreateCommandEncoder();
        for (size_t p = 0; p < rsp::kInsertionPointCount; ++p) {
            const auto point = static_cast<rsp::InsertionPoint>(p);
            if (!runtime->point_has_work(point)) {
                continue;
            }
            uint32_t slot = 0;
            uint64_t seq = 0;
            WGPUTextureView result = nullptr;
            if (runtime->prepare(point, color.Get(), depth.Get(), W, H, slot, seq, result)) {
                runtime->execute(slot, seq, encoder.Get(), game.queue);
                wgpu::RenderPassColorAttachment ca[2];
                ca[0].view = sceneColor;
                ca[0].loadOp = wgpu::LoadOp::Load;
                ca[0].storeOp = wgpu::StoreOp::Store;
                ca[1].view = sceneNormal;
                ca[1].loadOp = wgpu::LoadOp::Load;
                ca[1].storeOp = wgpu::StoreOp::Store;
                wgpu::RenderPassDepthStencilAttachment da{};
                da.view = sceneDepth;
                da.depthLoadOp = wgpu::LoadOp::Load;
                da.depthStoreOp = wgpu::StoreOp::Store;
                wgpu::RenderPassDescriptor rp{};
                rp.colorAttachmentCount = 2;
                rp.colorAttachments = ca;
                rp.depthStencilAttachment = &da;
                wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
                WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
                e.binding = 0;
                e.textureView = result;
                WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bgd.layout = shared->single_texture_layout();
                bgd.entryCount = 1;
                bgd.entries = &e;
                WGPUBindGroup group = wgpuDeviceCreateBindGroup(game.device, &bgd);
                wgpuRenderPassEncoderSetPipeline(pass.Get(), composite);
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, group, 0, nullptr);
                pass.Draw(3);
                pass.End();
                wgpuBindGroupRelease(group);
                runtime->finish_plan(slot, seq);
                ++planned;
            }
        }
        wgpu::CommandBuffer cb = encoder.Finish();
        dev.device.GetQueue().Submit(1, &cb);
    }
    dev.instance.ProcessEvents();
    std::printf("ran %d insertion-point plans over %d frames, game device errors: %d\n", planned, frames, g_gameErrors.load());
    const std::string error = runtime->status().last_error;
    if (!error.empty()) {
        std::printf("last error: %s\n", error.c_str());
    }
    wgpuRenderPipelineRelease(composite);
    runtime->shutdown();
    runtime.reset();
    return g_gameErrors.load() == 0 ? 0 : 1;
}
