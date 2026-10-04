// Offline checker for the ReShade port: compiles effect files with the mod's own compiler and WGSL
// back-end, then creates every shader module on Dawn's null backend (no GPU needed) with the device
// configured as Aurora configures the game's: no optional features, default limits, and SPIR-V
// disabled. With --gpu it also runs the mod's GPU layer (fx_gpu.cpp) on that device: creates every
// texture, pipeline and bind group, records every technique as the mod records a frame, and submits
// it, so anything WebGPU validation would reject (and Aurora would treat as fatal) is reported here.
// Reports per file and a summary; exits non-zero if anything failed.
//
//   fx_check [--quiet] [--gpu] [--size <w>x<h>] [--dump <dir>] [--include <dir>]... <file.fx>...
//
// Build recipe: docs/reshade_port.md "Offline checker".

#include "fx_compile.hpp"
#include "fx_gpu.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Device {
    wgpu::Instance instance;
    wgpu::Device device;
};

Device make_device() {
    Device d;
    // No allow_unsafe_apis (the game's instance has it): without it Dawn also lowers each pipeline's
    // shaders to Tint IR, as the D3D12, Vulkan and Metal back-ends do, which catches more.
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
    if (adapter.HasFeature(core)) {
        dd.requiredFeatureCount = 1;
        dd.requiredFeatures = &core;
    }
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m) {
        std::fprintf(stderr, "UNCAPTURED: %.*s\n", static_cast<int>(m.length), m.data);
    });
    d.instance.WaitAny(adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::RequestDeviceStatus, wgpu::Device dev, wgpu::StringView) { d.device = std::move(dev); }),
        UINT64_MAX);
    return d;
}

std::string pop_error(Device& d) {
    std::string err;
    d.instance.WaitAny(d.device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                           [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView m) {
                               if (type != wgpu::ErrorType::NoError) {
                                   err.assign(m.data, m.length);
                               }
                           }),
        UINT64_MAX);
    return err;
}

std::string compile_module(Device& d, const std::string& code) {
    wgpu::ShaderSourceWGSL src{};
    src.code = wgpu::StringView(code.data(), code.size());
    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &src;
    d.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderModule mod = d.device.CreateShaderModule(&smd);
    std::string err = pop_error(d);
    if (err.empty()) {
        d.instance.WaitAny(mod.GetCompilationInfo(wgpu::CallbackMode::WaitAnyOnly,
                               [&](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* info) {
                                   if (info == nullptr) {
                                       return;
                                   }
                                   for (size_t i = 0; i < info->messageCount; ++i) {
                                       if (info->messages[i].type == wgpu::CompilationMessageType::Error && err.empty()) {
                                           err.assign(info->messages[i].message.data, info->messages[i].message.length);
                                       }
                                   }
                               }),
            UINT64_MAX);
    }
    return err;
}

std::string one_line(std::string s, size_t max = 300) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    if (s.size() > max) {
        s.resize(max);
    }
    return s;
}

// Runs the GPU layer for one compiled effect. Returns an empty string on success.
std::string run_gpu(Device& d, rsp::GpuShared& shared, rsp::FrameTextures& frame, WGPUTextureView snapshot,
    WGPUTextureView rawDepth, const rsp::CompiledEffect& effect, size_t& techniques) {
    const rsp::GpuApi api{d.instance.Get(), d.device.Get(), d.device.GetQueue().MoveToCHandle()};
    std::string errors;
    auto pool = std::make_shared<rsp::TexturePool>();
    rsp::EffectGpu gpu;
    const rsp::SourceLoader loader = [](const std::string&, rsp::PoolTexture& t, std::string& err) {
        const uint32_t bpt = rsp::bytes_per_texel(t.gpu.format);
        if (bpt == 0) {
            err += "no upload format\n";
            return false;
        }
        t.bytes_per_row = t.gpu.width * bpt;
        t.pixels.assign(static_cast<size_t>(t.bytes_per_row) * t.gpu.height * (t.gpu.is_3d ? t.gpu.depth : 1u), 0x3c);
        return true;
    };
    const bool built = gpu.build(api, shared, frame, pool, effect, loader, errors);
    techniques = gpu.technique_count();
    {
        rsp::ScopedErrors scope(api, "frame");
        WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(api.device, &ed);
        rsp::RecordState state{encoder, &frame, &shared};
        shared.blit_to_backbuffer(encoder, snapshot, frame);
        rsp::DepthParams dp;
        dp.a = 0.0f;
        dp.b = 1.0f;
        dp.c = -1.0f;
        dp.d = 0.0f;
        dp.valid = 1.0f;
        shared.convert_depth(encoder, rawDepth, frame, dp);
        gpu.upload_pending(state);
        std::vector<uint8_t> zeros(gpu.uniform_size(), 0);
        gpu.write_uniforms(api.queue, zeros.data(), zeros.size());
        for (size_t t = 0; t < gpu.technique_count(); ++t) {
            gpu.record_technique(state, t);
        }
        WGPUCommandBufferDescriptor cd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder, &cd);
        wgpuQueueSubmit(api.queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(encoder);
        scope.finish(errors);
    }
    gpu.release();
    pool.reset();
    wgpuQueueRelease(api.queue);
    if (built && errors.empty()) {
        return {};
    }
    return errors.empty() ? std::string("build failed without a message") : errors;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> includes;
    std::vector<std::filesystem::path> files;
    std::filesystem::path dumpDir;
    bool quiet = false;
    bool runGpu = false;
    uint32_t width = 1920, height = 1080;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%ux%u", &width, &height);
            continue;
        }
        if (std::strcmp(argv[i], "--gpu") == 0) {
            runGpu = true;
            continue;
        }
        if (std::strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            includes.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpDir = argv[++i];
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else {
            files.emplace_back(argv[i]);
        }
    }
    Device dev = make_device();
    if (!dev.device) {
        std::fprintf(stderr, "no Dawn null device\n");
        return 2;
    }

    rsp::GpuShared shared;
    rsp::FrameTextures frame;
    wgpu::Texture snapshotTex, rawDepthTex;
    wgpu::TextureView snapshot, rawDepth;
    if (runGpu) {
        const rsp::GpuApi api{dev.instance.Get(), dev.device.Get(), dev.device.GetQueue().Get()};
        std::string errors;
        if (!shared.init(api, errors) || !shared.has_blit(WGPUTextureFormat_BGRA8Unorm) ||
            !frame.create(api, width, height, WGPUTextureFormat_BGRA8Unorm, errors)) {
            std::fprintf(stderr, "GPU layer init failed: %s\n", errors.c_str());
            return 2;
        }
        // Stand-ins for the game's snapshots (GfxResolvedTargets::color / ::depth).
        wgpu::TextureDescriptor td{};
        td.size = {width, height, 1};
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        td.format = wgpu::TextureFormat::BGRA8Unorm;
        snapshotTex = dev.device.CreateTexture(&td);
        snapshot = snapshotTex.CreateView();
        td.format = wgpu::TextureFormat::R32Float;
        rawDepthTex = dev.device.CreateTexture(&td);
        rawDepth = rawDepthTex.CreateView();
    }

    int filesOk = 0, filesFail = 0, modules = 0, modulesOk = 0, gpuOk = 0, gpuFail = 0;
    size_t techniquesTotal = 0;
    for (const auto& path : files) {
        rsp::CompileOptions options;
        options.width = width;
        options.height = height;
        options.color_format = WGPUTextureFormat_BGRA8Unorm;
        options.include_paths = includes;
        rsp::CompiledEffect effect;
        rsp::compile_effect(path, options, effect);
        bool fileOk = effect.ok;
        std::string firstError = effect.ok ? std::string() : effect.errors;
        if (effect.codegen) {
            for (const auto& [name, ep] : effect.entry_points) {
                ++modules;
                if (!dumpDir.empty()) {
                    std::ofstream(dumpDir / (path.stem().string() + "." + name + ".wgsl")) << ep.code;
                }
                const std::string err = compile_module(dev, ep.code);
                if (err.empty()) {
                    ++modulesOk;
                } else {
                    fileOk = false;
                    if (firstError.empty()) {
                        firstError = name + ": " + err;
                    }
                }
            }
        }
        if (fileOk && runGpu) {
            size_t techniques = 0;
            const std::string err = run_gpu(dev, shared, frame, snapshot.Get(), rawDepth.Get(), effect, techniques);
            techniquesTotal += techniques;
            if (err.empty()) {
                ++gpuOk;
            } else {
                ++gpuFail;
                fileOk = false;
                firstError = "gpu: " + err;
            }
        }
        (fileOk ? filesOk : filesFail)++;
        if (!fileOk || !quiet) {
            std::printf("%s %s\n", fileOk ? "OK  " : "FAIL", path.string().c_str());
            if (!fileOk) {
                std::printf("     %s\n", one_line(firstError).c_str());
            }
        }
    }
    std::printf("\nfiles ok=%d failed=%d, shader modules ok=%d/%d\n", filesOk, filesFail, modulesOk, modules);
    if (runGpu) {
        std::printf("gpu layer: effects ok=%d failed=%d, techniques %zu\n", gpuOk, gpuFail, techniquesTotal);
        frame.release();
        shared.release();
    }
    return filesFail == 0 ? 0 : 1;
}
