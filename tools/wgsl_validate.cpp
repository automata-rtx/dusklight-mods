#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <atomic>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.wgsl\n", argv[0]);
    return 2;
  }
  std::ifstream f(argv[1]);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string code = ss.str();

  wgpu::InstanceDescriptor instanceDescriptor{};
  wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);
  if (!instance) { fprintf(stderr, "failed to create instance\n"); return 1; }

  wgpu::RequestAdapterOptions options{};
  options.backendType = wgpu::BackendType::Null;

  wgpu::Adapter adapter;
  std::atomic<bool> adapterDone{false};
  instance.RequestAdapter(&options, wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
        if (status != wgpu::RequestAdapterStatus::Success) {
          fprintf(stderr, "RequestAdapter failed: %.*s\n", (int)msg.length, msg.data);
        } else {
          adapter = a;
        }
        adapterDone = true;
      });
  while (!adapterDone) { instance.ProcessEvents(); }
  if (!adapter) { fprintf(stderr, "no adapter\n"); return 1; }

  wgpu::Device device;
  std::atomic<bool> deviceDone{false};
  wgpu::DeviceDescriptor devDesc{};
  devDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
    fprintf(stderr, "[DEVICE ERROR %d] %.*s\n", (int)type, (int)message.length, message.data);
  });
  adapter.RequestDevice(&devDesc, wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
        if (status != wgpu::RequestDeviceStatus::Success) {
          fprintf(stderr, "RequestDevice failed: %.*s\n", (int)msg.length, msg.data);
        } else {
          device = d;
        }
        deviceDone = true;
      });
  while (!deviceDone) { instance.ProcessEvents(); }
  if (!device) { fprintf(stderr, "no device\n"); return 1; }

  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor moduleDesc{};
  moduleDesc.nextInChain = &wgsl;
  moduleDesc.label = "validate";

  std::atomic<bool> compileDone{false};
  bool ok = false;
  wgpu::ShaderModule mod = device.CreateShaderModule(&moduleDesc);
  mod.GetCompilationInfo(wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::CompilationInfoRequestStatus status, const wgpu::CompilationInfo* info) {
        ok = true;
        for (size_t i = 0; i < info->messageCount; ++i) {
          const auto& m = info->messages[i];
          const char* sev = m.type == wgpu::CompilationMessageType::Error ? "ERROR" :
                             m.type == wgpu::CompilationMessageType::Warning ? "WARN" : "INFO";
          fprintf(stderr, "[%s] line %llu col %llu: %.*s\n", sev,
                  (unsigned long long)m.lineNum, (unsigned long long)m.linePos,
                  (int)m.message.length, m.message.data);
          if (m.type == wgpu::CompilationMessageType::Error) ok = false;
        }
        compileDone = true;
      });
  while (!compileDone) { instance.ProcessEvents(); }

  printf(ok ? "WGSL_VALIDATE_OK\n" : "WGSL_VALIDATE_FAIL\n");
  return ok ? 0 : 1;
}
