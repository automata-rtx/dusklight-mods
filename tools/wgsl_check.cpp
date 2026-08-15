// WGSL validator: compiles each .wgsl given on argv through Dawn's null backend and reports
// compilation diagnostics. Exit non-zero if any shader fails to compile.
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    wgpu::InstanceDescriptor id{};
    wgpu::Instance inst = wgpu::CreateInstance(&id);
    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = wgpu::BackendType::Null;
    bool done = false;
    inst.RequestAdapter(&ao, wgpu::CallbackMode::AllowSpontaneous,
        [&](wgpu::RequestAdapterStatus s, wgpu::Adapter a, wgpu::StringView) {
            done = true;
            if (s == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
        });
    for (int i = 0; i < 500 && !done; ++i) inst.ProcessEvents();
    if (!adapter) { std::fprintf(stderr, "no adapter\n"); return 2; }

    wgpu::Device device;
    wgpu::DeviceDescriptor dd{};
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m) {
        std::fprintf(stderr, "device error: %.*s\n", (int)m.length, m.data);
    });
    done = false;
    adapter.RequestDevice(&dd, wgpu::CallbackMode::AllowSpontaneous,
        [&](wgpu::RequestDeviceStatus s, wgpu::Device dev, wgpu::StringView) {
            done = true;
            if (s == wgpu::RequestDeviceStatus::Success) device = std::move(dev);
        });
    for (int i = 0; i < 500 && !done; ++i) inst.ProcessEvents();
    if (!device) { std::fprintf(stderr, "no device\n"); return 2; }

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i]);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[i]); ++failures; continue; }
        std::stringstream ss; ss << f.rdbuf();
        const std::string src = ss.str();

        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = wgpu::StringView(src.data(), src.size());
        wgpu::ShaderModuleDescriptor smd{};
        smd.nextInChain = &wgsl;
        wgpu::ShaderModule mod = device.CreateShaderModule(&smd);

        bool bad = false;
        bool got = false;
        mod.GetCompilationInfo(wgpu::CallbackMode::AllowSpontaneous,
            [&](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* info) {
                got = true;
                if (info == nullptr) return;
                for (size_t m = 0; m < info->messageCount; ++m) {
                    const auto& msg = info->messages[m];
                    const bool err = msg.type == wgpu::CompilationMessageType::Error;
                    bad = bad || err;
                    std::printf("  [%s] %s:%llu:%llu %.*s\n", err ? "ERROR" : "warn", argv[i],
                        (unsigned long long)msg.lineNum, (unsigned long long)msg.linePos,
                        (int)msg.message.length, msg.message.data);
                }
            });
        for (int k = 0; k < 500 && !got; ++k) inst.ProcessEvents();
        if (bad) ++failures;
        std::printf("%-52s %s\n", argv[i], bad ? "FAIL" : "ok");
    }
    std::printf("\n%d shader(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
