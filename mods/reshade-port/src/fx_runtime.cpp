#include "fx_runtime.hpp"

#include "fx_images.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <optional>
#include <set>
#include <thread>

namespace rsp {

const char* insertion_point_label(InsertionPoint p) {
    switch (p) {
    case InsertionPoint::BeforeTransparency: return "Before transparency";
    case InsertionPoint::BeforeParticles: return "Before particles & post-processing";
    case InsertionPoint::BeforeHud: return "Before HUD";
    case InsertionPoint::AfterHud: return "After HUD (standalone ReShade)";
    }
    return "?";
}

const char* insertion_point_key(InsertionPoint p) {
    switch (p) {
    case InsertionPoint::BeforeTransparency: return "BeforeTransparency";
    case InsertionPoint::BeforeParticles: return "BeforeParticles";
    case InsertionPoint::BeforeHud: return "BeforeHUD";
    case InsertionPoint::AfterHud: return "AfterHUD";
    }
    return "BeforeParticles";
}

bool parse_insertion_point(const std::string& key, InsertionPoint& out) {
    for (size_t i = 0; i < kInsertionPointCount; ++i) {
        const auto p = static_cast<InsertionPoint>(i);
        if (key == insertion_point_key(p)) {
            out = p;
            return true;
        }
    }
    return false;
}

namespace {

constexpr const char* kDusklightSection = "DUSKLIGHT";

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// A null-backend device configured like the game's: same core/compat mode, limits no looser.
bool create_validation_device(const GpuApi& game, GpuApi& out, WGPUAdapter& outAdapter, std::string& errors) {
    WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
    id.requiredFeatureCount = 1;
    id.requiredFeatures = features;
    out.instance = wgpuCreateInstance(&id);
    if (out.instance == nullptr) {
        errors += "cannot create a WebGPU instance for validation\n";
        return false;
    }
    const bool core = wgpuDeviceHasFeature(game.device, WGPUFeatureName_CoreFeaturesAndLimits);
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    ao.backendType = WGPUBackendType_Null;
    ao.featureLevel = core ? WGPUFeatureLevel_Core : WGPUFeatureLevel_Compatibility;
    struct AdapterResult {
        WGPUAdapter adapter = nullptr;
    } ar;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_WaitAnyOnly;
    aci.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void* ud, void*) {
        if (status == WGPURequestAdapterStatus_Success) {
            static_cast<AdapterResult*>(ud)->adapter = adapter;
        }
    };
    aci.userdata1 = &ar;
    WGPUFutureWaitInfo wait{wgpuInstanceRequestAdapter(out.instance, &ao, aci), false};
    wgpuInstanceWaitAny(out.instance, 1, &wait, UINT64_MAX);
    if (ar.adapter == nullptr) {
        errors += "the null WebGPU backend is not available\n";
        return false;
    }
    outAdapter = ar.adapter;

    WGPULimits gameLimits = WGPU_LIMITS_INIT;
    WGPULimits nullLimits = WGPU_LIMITS_INIT;
    wgpuDeviceGetLimits(game.device, &gameLimits);
    wgpuAdapterGetLimits(ar.adapter, &nullLimits);
    WGPULimits req = gameLimits;
    req.nextInChain = nullptr;
#define RSP_MIN_LIMIT(f) req.f = std::min(gameLimits.f, nullLimits.f)
#define RSP_MAX_LIMIT(f) req.f = std::max(gameLimits.f, nullLimits.f)
    RSP_MIN_LIMIT(maxTextureDimension1D);
    RSP_MIN_LIMIT(maxTextureDimension2D);
    RSP_MIN_LIMIT(maxTextureDimension3D);
    RSP_MIN_LIMIT(maxTextureArrayLayers);
    RSP_MIN_LIMIT(maxBindGroups);
    RSP_MIN_LIMIT(maxBindGroupsPlusVertexBuffers);
    RSP_MIN_LIMIT(maxBindingsPerBindGroup);
    RSP_MIN_LIMIT(maxDynamicUniformBuffersPerPipelineLayout);
    RSP_MIN_LIMIT(maxDynamicStorageBuffersPerPipelineLayout);
    RSP_MIN_LIMIT(maxSampledTexturesPerShaderStage);
    RSP_MIN_LIMIT(maxSamplersPerShaderStage);
    RSP_MIN_LIMIT(maxStorageBuffersPerShaderStage);
    RSP_MIN_LIMIT(maxStorageTexturesPerShaderStage);
    RSP_MIN_LIMIT(maxUniformBuffersPerShaderStage);
    RSP_MIN_LIMIT(maxUniformBufferBindingSize);
    RSP_MIN_LIMIT(maxStorageBufferBindingSize);
    RSP_MAX_LIMIT(minUniformBufferOffsetAlignment);
    RSP_MAX_LIMIT(minStorageBufferOffsetAlignment);
    RSP_MIN_LIMIT(maxVertexBuffers);
    RSP_MIN_LIMIT(maxBufferSize);
    RSP_MIN_LIMIT(maxVertexAttributes);
    RSP_MIN_LIMIT(maxVertexBufferArrayStride);
    RSP_MIN_LIMIT(maxInterStageShaderVariables);
    RSP_MIN_LIMIT(maxColorAttachments);
    RSP_MIN_LIMIT(maxColorAttachmentBytesPerSample);
    RSP_MIN_LIMIT(maxComputeWorkgroupStorageSize);
    RSP_MIN_LIMIT(maxComputeInvocationsPerWorkgroup);
    RSP_MIN_LIMIT(maxComputeWorkgroupSizeX);
    RSP_MIN_LIMIT(maxComputeWorkgroupSizeY);
    RSP_MIN_LIMIT(maxComputeWorkgroupSizeZ);
    RSP_MIN_LIMIT(maxComputeWorkgroupsPerDimension);
    RSP_MIN_LIMIT(maxImmediateSize);
#undef RSP_MIN_LIMIT
#undef RSP_MAX_LIMIT

    WGPUFeatureName featureList[1];
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.label = {"ReShade port validation", WGPU_STRLEN};
    if (core && wgpuAdapterHasFeature(ar.adapter, WGPUFeatureName_CoreFeaturesAndLimits)) {
        featureList[0] = WGPUFeatureName_CoreFeaturesAndLimits;
        dd.requiredFeatureCount = 1;
        dd.requiredFeatures = featureList;
    }
    dd.requiredLimits = &req;
    // Errors on this device are expected (that is its purpose) and always captured by scopes.
    dd.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType, WGPUStringView, void*, void*) {};
    struct DeviceResult {
        WGPUDevice device = nullptr;
        std::string message;
    } dr;
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_WaitAnyOnly;
    dci.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* ud, void*) {
        auto* r = static_cast<DeviceResult*>(ud);
        if (status == WGPURequestDeviceStatus_Success) {
            r->device = device;
        } else if (message.data != nullptr) {
            r->message.assign(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length);
        }
    };
    dci.userdata1 = &dr;
    wait = WGPUFutureWaitInfo{wgpuAdapterRequestDevice(ar.adapter, &dd, dci), false};
    wgpuInstanceWaitAny(out.instance, 1, &wait, UINT64_MAX);
    if (dr.device == nullptr) {
        errors += "cannot create the validation device: " + dr.message + '\n';
        return false;
    }
    out.device = dr.device;
    out.queue = wgpuDeviceGetQueue(dr.device);
    return true;
}

} // namespace

// -------------------------------------------------------------------------------------------------
// Builder: the background thread that compiles effects and builds their GPU objects.

class Runtime::Builder {
public:
    struct Request {
        uint64_t generation = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> include_paths;
        std::vector<std::filesystem::path> texture_paths;
        std::vector<std::pair<std::string, std::string>> global_definitions;
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> effect_definitions;
        std::set<std::string> gpu_files;
    };
    struct Event {
        enum Kind { Ready, GenerationReady, GenerationFailed, Effect, Message } kind = Message;
        uint64_t generation = 0;
        std::shared_ptr<Generation> gen;
        std::shared_ptr<EffectBuild> effect;
        std::string message;
    };

    explicit Builder(const GpuApi& game) : _game(game) { _thread = std::thread([this] { run(); }); }

    ~Builder() {
        {
            std::lock_guard lock(_mutex);
            _stop = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
            _thread.join();
        }
        _nullFrame.release();
        _nullPool.reset();
        _nullShared.release();
        if (_null.queue != nullptr) {
            wgpuQueueRelease(_null.queue);
        }
        if (_null.device != nullptr) {
            wgpuDeviceRelease(_null.device);
        }
        if (_nullAdapter != nullptr) {
            wgpuAdapterRelease(_nullAdapter);
        }
        if (_null.instance != nullptr) {
            wgpuInstanceRelease(_null.instance);
        }
        _gameShared.release();
    }

    void submit(Request r) {
        {
            std::lock_guard lock(_mutex);
            _pending = std::move(r);
            _gpuQueue.clear();
        }
        _cv.notify_all();
    }

    void request_gpu(uint64_t generation, const std::string& file) {
        {
            std::lock_guard lock(_mutex);
            _gpuQueue.emplace_back(generation, file);
        }
        _cv.notify_all();
    }

    std::vector<Event> take_events() {
        std::lock_guard lock(_mutex);
        return std::exchange(_events, {});
    }

    const GpuShared& game_shared() const { return _gameShared; }
    bool validating() const { return _validate.load(); }

    std::atomic<size_t> compile_total{0};
    std::atomic<size_t> compile_done{0};

private:
    void post(Event e) {
        std::lock_guard lock(_mutex);
        _events.push_back(std::move(e));
    }

    bool superseded() {
        std::lock_guard lock(_mutex);
        return _stop || _pending.has_value();
    }

    void run() {
        std::string errors;
        const bool sharedOk = _gameShared.init(_game, errors);
        std::string validationErrors;
        _validate = create_validation_device(_game, _null, _nullAdapter, validationErrors) && _nullShared.init(_null, validationErrors);
        Event ready;
        ready.kind = Event::Ready;
        if (!sharedOk) {
            ready.message = "error: cannot create the runtime's pipelines on the game's device: " + errors;
        } else if (!_validate) {
            ready.message = "warning: effects cannot be validated before use (" + validationErrors +
                            "); an invalid effect may crash the game";
        }
        post(std::move(ready));
        if (!sharedOk) {
            return;
        }

        for (;;) {
            std::optional<Request> request;
            std::pair<uint64_t, std::string> gpuJob;
            bool haveGpuJob = false;
            {
                std::unique_lock lock(_mutex);
                _cv.wait(lock, [&] { return _stop || _pending.has_value() || !_gpuQueue.empty(); });
                if (_stop) {
                    return;
                }
                if (_pending.has_value()) {
                    request = std::move(_pending);
                    _pending.reset();
                } else {
                    gpuJob = std::move(_gpuQueue.front());
                    _gpuQueue.pop_front();
                    haveGpuJob = true;
                }
            }
            if (request.has_value()) {
                _request = std::move(*request);
                process_generation();
            } else if (haveGpuJob && _generation != nullptr && gpuJob.first == _generation->id) {
                build_gpu(gpuJob.second);
            }
        }
    }

    void process_generation() {
        const Request& r = _request;
        _builds.clear();
        _generation.reset();
        compile_total = r.files.size();
        compile_done = 0;

        // Frame textures on both devices.
        auto gen = std::make_shared<Generation>();
        gen->id = r.generation;
        gen->width = r.width;
        gen->height = r.height;
        gen->color_format = r.format;
        std::string errors;
        if (!gen->frame.create(_game, r.width, r.height, r.format, errors)) {
            Event e;
            e.kind = Event::GenerationFailed;
            e.generation = r.generation;
            e.message = errors;
            post(std::move(e));
            return;
        }
        gen->frame.generation = r.generation;
        _nullFrame.release();
        _nullPool = std::make_shared<TexturePool>();
        if (_validate && !_nullFrame.create(_null, r.width, r.height, r.format, errors)) {
            _validate = false;
        }
        _nullFrame.generation = r.generation;
        _generation = gen;
        {
            Event e;
            e.kind = Event::GenerationReady;
            e.generation = r.generation;
            e.gen = gen;
            post(std::move(e));
        }

        // Compile every effect file, in parallel: parse all, merge how they use shared textures,
        // then generate WGSL (a texture's storage format depends on every effect that uses it).
        const size_t n = r.files.size();
        std::vector<std::shared_ptr<CompiledEffect>> parsed(n);
        bool cancelled = false;
        if (!run_parallel(n, [&](size_t i) { parsed[i] = parse_one(r.files[i]); })) {
            return;
        }
        TextureFlagMap merged;
        for (const auto& c : parsed) {
            if (c != nullptr && c->codegen != nullptr && c->texture_flags != nullptr) {
                for (const auto& [name, f] : *c->texture_flags) {
                    merged[name].storage = merged[name].storage || f.storage;
                    merged[name].blended = merged[name].blended || f.blended;
                }
            }
        }
        std::vector<std::shared_ptr<EffectBuild>> results(n);
        cancelled = !run_parallel(n, [&](size_t i) {
            auto build = std::make_shared<EffectBuild>();
            build->generation = r.generation;
            build->path = r.files[i];
            build->file_name = path_utf8(r.files[i].filename());
            const std::shared_ptr<CompiledEffect>& c = parsed[i];
            if (c->codegen != nullptr && c->ok) {
                for (auto& [name, f] : *c->texture_flags) {
                    f = merged[name];
                }
                assemble_effect(*c);
            }
            build->messages = c->errors;
            build->compile_ok = c->ok;
            if (c->codegen != nullptr) {
                build->compiled = c;
            }
            results[i] = build;
            ++compile_done;
            post(Event{Event::Effect, r.generation, nullptr, build, {}});
        });
        if (cancelled) {
            return;
        }
        for (const auto& b : results) {
            if (b != nullptr) {
                _builds[b->file_name] = b;
            }
        }
        for (const std::string& file : r.gpu_files) {
            if (superseded()) {
                return;
            }
            build_gpu(file);
        }
    }

    // Runs fn(0..n-1) on a few threads. Returns false if a newer request arrived meanwhile.
    template <class Fn>
    bool run_parallel(size_t n, Fn&& fn) {
        std::atomic<size_t> next{0};
        std::atomic<bool> cancelled{false};
        const auto worker = [&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= n || cancelled.load()) {
                    return;
                }
                if (superseded()) {
                    cancelled = true;
                    return;
                }
                fn(i);
            }
        };
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned count = std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u);
        std::vector<std::thread> threads;
        for (unsigned t = 1; t < count; ++t) {
            threads.emplace_back(worker);
        }
        worker();
        for (std::thread& t : threads) {
            t.join();
        }
        return !cancelled.load();
    }

    std::shared_ptr<CompiledEffect> parse_one(const std::filesystem::path& path) {
        const Request& r = _request;
        const std::string fileName = path_utf8(path.filename());
        CompileOptions options;
        options.width = r.width;
        options.height = r.height;
        options.color_format = r.format;
        options.include_paths = r.include_paths;
        if (const auto it = r.effect_definitions.find(fileName); it != r.effect_definitions.end()) {
            options.definitions = it->second;
        }
        options.definitions.insert(options.definitions.end(), r.global_definitions.begin(), r.global_definitions.end());
        auto compiled = std::make_shared<CompiledEffect>();
        parse_effect(path, options, *compiled);
        return compiled;
    }

    // Validates `file` on the null device, then builds it on the game's device, and publishes a
    // new EffectBuild with the result.
    void build_gpu(const std::string& file) {
        const auto it = _builds.find(file);
        if (it == _builds.end() || _generation == nullptr) {
            return;
        }
        const std::shared_ptr<EffectBuild> base = it->second;
        if (base->gpu_done) {
            return;
        }
        auto out = std::make_shared<EffectBuild>(*base);
        out->gpu_done = true;
        if (!base->compile_ok || base->compiled == nullptr) {
            _builds[file] = out;
            post(Event{Event::Effect, _generation->id, nullptr, out, {}});
            return;
        }
        const CompiledEffect& effect = *base->compiled;
        const size_t techniqueCount = effect.module().techniques.size();
        std::vector<uint8_t> valid(techniqueCount, 1);
        std::string messages;

        if (_validate) {
            EffectGpu probe;
            const SourceLoader zeros = [](const std::string&, PoolTexture& t, std::string&) {
                const uint32_t bpt = bytes_per_texel(t.gpu.format);
                t.bytes_per_row = t.gpu.width * bpt;
                t.pixels.assign(static_cast<size_t>(t.bytes_per_row) * t.gpu.height * (t.gpu.is_3d ? t.gpu.depth : 1u), 0);
                return bpt != 0;
            };
            std::string probeErrors;
            probe.build(_null, _nullShared, _nullFrame, _nullPool, effect, zeros, probeErrors);
            for (size_t t = 0; t < techniqueCount; ++t) {
                if (!probe.technique_built(t)) {
                    valid[t] = 0;
                    continue;
                }
                ScopedErrors scope(_null, "validation");
                WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
                WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(_null.device, &ed);
                RecordState state{encoder, &_nullFrame, &_nullShared};
                probe.upload_pending(state);
                std::vector<uint8_t> zerosUniform(probe.uniform_size(), 0);
                probe.write_uniforms(_null.queue, zerosUniform.data(), zerosUniform.size());
                probe.record_technique(state, t);
                WGPUCommandBufferDescriptor cd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
                WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder, &cd);
                wgpuQueueSubmit(_null.queue, 1, &cb);
                wgpuCommandBufferRelease(cb);
                wgpuCommandEncoderRelease(encoder);
                std::string recordErrors;
                if (!scope.finish(recordErrors)) {
                    valid[t] = 0;
                    probeErrors += "technique '" + effect.module().techniques[t].name + "': " + recordErrors;
                }
            }
            if (!probeErrors.empty()) {
                messages += "error: " + probeErrors;
            }
        }

        bool any = false;
        for (const uint8_t v : valid) {
            any = any || v != 0;
        }
        if (any) {
            auto gpu = std::make_shared<EffectGpu>();
            const std::vector<std::filesystem::path> texturePaths = _request.texture_paths;
            const SourceLoader loader = [texturePaths](const std::string& source, PoolTexture& t, std::string& err) {
                return load_texture_source(texturePaths, source, t, err);
            };
            std::string buildErrors;
            gpu->build(_game, _gameShared, _generation->frame, _generation->pool, effect, loader, buildErrors);
            if (!buildErrors.empty() && !_validate) {
                messages += "error: " + buildErrors;
            } else if (!buildErrors.empty()) {
                // Only image loading or a back-end compiler can fail here after validation passed.
                messages += buildErrors;
            }
            for (size_t t = 0; t < techniqueCount; ++t) {
                valid[t] = valid[t] && gpu->technique_built(t);
            }
            out->gpu = std::move(gpu);
        }
        out->technique_ok = std::move(valid);
        out->messages += messages;
        _builds[file] = out;
        post(Event{Event::Effect, _generation->id, nullptr, out, {}});
    }

    GpuApi _game;
    GpuShared _gameShared;
    GpuApi _null;
    WGPUAdapter _nullAdapter = nullptr;
    GpuShared _nullShared;
    std::atomic<bool> _validate{false};
    FrameTextures _nullFrame;
    std::shared_ptr<TexturePool> _nullPool;

    Request _request;
    std::shared_ptr<Generation> _generation;
    std::map<std::string, std::shared_ptr<EffectBuild>> _builds;

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _stop = false;
    std::optional<Request> _pending;
    std::deque<std::pair<uint64_t, std::string>> _gpuQueue;
    std::vector<Event> _events;
};

// -------------------------------------------------------------------------------------------------

Runtime::Runtime() = default;

Runtime::~Runtime() { shutdown(); }

bool Runtime::init(const GpuApi& game, const std::filesystem::path& baseDir, std::string& errors) {
    _game = game;
    _baseDir = baseDir;
    std::error_code ec;
    std::filesystem::create_directories(shaders_dir(), ec);
    std::filesystem::create_directories(textures_dir(), ec);
    _start = _lastUpdate = std::chrono::steady_clock::now();
    load_preset();
    _builder = std::make_unique<Builder>(game);
    _initialized = true;
    (void)errors;
    return true;
}

void Runtime::shutdown() {
    if (!_initialized) {
        return;
    }
    if (_presetDirty) {
        save_preset_now();
    }
    {
        std::lock_guard lock(_planMutex);
        for (Plan& p : _plans) {
            p = Plan{};
        }
    }
    _builder.reset();
    _effects.clear();
    _generation.reset();
    _initialized = false;
}

// --- Preset ---------------------------------------------------------------------------------------

void Runtime::load_preset() {
    _preset.load(preset_path());
    _techniques.clear();
    const IniFile::Values* sorting = _preset.find("", "TechniqueSorting");
    const IniFile::Values* enabled = _preset.find("", "Techniques");
    const IniFile::Values* order = sorting != nullptr ? sorting : enabled;
    if (order != nullptr) {
        for (const std::string& unique : *order) {
            const size_t at = unique.find('@');
            if (at == std::string::npos) {
                continue;
            }
            TechniqueEntry t;
            t.name = unique.substr(0, at);
            t.file = unique.substr(at + 1);
            if (enabled != nullptr) {
                t.enabled = std::find(enabled->begin(), enabled->end(), unique) != enabled->end();
            }
            if (const IniFile::Values* point = _preset.find(kDusklightSection, unique); point != nullptr && !point->empty()) {
                parse_insertion_point(point->front(), t.point);
            }
            _techniques.push_back(std::move(t));
        }
    }
    ++_techniqueVersion;
}

void Runtime::mark_preset_dirty() {
    _presetDirty = true;
    _presetChanged = std::chrono::steady_clock::now();
}

void Runtime::save_preset_now() {
    IniFile::Values enabled;
    IniFile::Values sorting;
    for (const TechniqueEntry& t : _techniques) {
        sorting.push_back(t.unique_name());
        if (t.enabled) {
            enabled.push_back(t.unique_name());
        }
        if (t.point != kDefaultInsertionPoint) {
            _preset.set(kDusklightSection, t.unique_name(), {insertion_point_key(t.point)});
        } else {
            _preset.remove(kDusklightSection, t.unique_name());
        }
    }
    _preset.set("", "Techniques", enabled);
    _preset.set("", "TechniqueSorting", sorting);
    if (!_preset.save(preset_path())) {
        _lastError = "cannot write " + path_utf8(preset_path());
    }
    _presetDirty = false;
}

void Runtime::apply_preset_values(const std::string& file, EffectState& state) {
    if (state.build == nullptr || state.build->compiled == nullptr) {
        return;
    }
    const reshadefx::effect_module& module = state.build->compiled->module();
    state.uniforms.init(module);
    for (const reshadefx::uniform& u : module.uniforms) {
        if (special_of(u) != SpecialUniform::None || annotation_int(u.annotations, "nosave") != 0) {
            continue;
        }
        const IniFile::Values* values = _preset.find(file, u.name);
        if (values == nullptr) {
            continue;
        }
        const size_t n = u.type.components();
        const auto element = [&](size_t i) -> const char* { return i < values->size() ? (*values)[i].c_str() : "0"; };
        switch (u.type.base) {
        case reshadefx::type::t_int: {
            std::vector<int32_t> v(n);
            for (size_t i = 0; i < n; ++i) {
                v[i] = static_cast<int32_t>(std::strtol(element(i), nullptr, 10));
            }
            state.uniforms.set(u, v.data(), n);
            break;
        }
        case reshadefx::type::t_bool:
        case reshadefx::type::t_uint: {
            std::vector<uint32_t> v(n);
            for (size_t i = 0; i < n; ++i) {
                v[i] = static_cast<uint32_t>(std::strtoul(element(i), nullptr, 10));
            }
            state.uniforms.set(u, v.data(), n);
            break;
        }
        case reshadefx::type::t_float: {
            std::vector<float> v(n);
            for (size_t i = 0; i < n; ++i) {
                v[i] = std::strtof(element(i), nullptr);
            }
            state.uniforms.set(u, v.data(), n);
            break;
        }
        default:
            break;
        }
    }
}

void Runtime::uniform_changed(const std::string& file, const reshadefx::uniform& u) {
    EffectState* state = effect(file);
    if (state == nullptr || annotation_int(u.annotations, "nosave") != 0) {
        return;
    }
    const size_t n = u.type.components();
    IniFile::Values values;
    switch (u.type.base) {
    case reshadefx::type::t_int: {
        std::vector<int32_t> v(n);
        state->uniforms.get(u, v.data(), n);
        for (const int32_t x : v) {
            values.push_back(std::to_string(x));
        }
        break;
    }
    case reshadefx::type::t_bool:
    case reshadefx::type::t_uint: {
        std::vector<uint32_t> v(n);
        state->uniforms.get(u, v.data(), n);
        for (const uint32_t x : v) {
            values.push_back(std::to_string(x));
        }
        break;
    }
    case reshadefx::type::t_float: {
        std::vector<float> v(n);
        state->uniforms.get(u, v.data(), n);
        for (const float x : v) {
            values.push_back(format_float(x));
        }
        break;
    }
    default:
        return;
    }
    _preset.set(file, u.name, std::move(values));
    mark_preset_dirty();
}

void Runtime::reset_uniform(const std::string& file, const reshadefx::uniform& u) {
    EffectState* state = effect(file);
    if (state == nullptr) {
        return;
    }
    state->uniforms.reset(u);
    _preset.remove(file, u.name);
    mark_preset_dirty();
}

std::string Runtime::global_definitions() const {
    return format_definitions(_preset.get_definitions("", "PreprocessorDefinitions"));
}

void Runtime::set_global_definitions(const std::string& text) {
    const auto defs = parse_definitions(text);
    if (defs == _preset.get_definitions("", "PreprocessorDefinitions")) {
        return;
    }
    _preset.set_definitions("", "PreprocessorDefinitions", defs);
    mark_preset_dirty();
    _reloadPending = true;
}

std::string Runtime::effect_definitions(const std::string& file) const {
    return format_definitions(_preset.get_definitions(file, "PreprocessorDefinitions"));
}

void Runtime::set_effect_definitions(const std::string& file, const std::string& text) {
    const auto defs = parse_definitions(text);
    if (defs == _preset.get_definitions(file, "PreprocessorDefinitions")) {
        return;
    }
    _preset.set_definitions(file, "PreprocessorDefinitions", defs);
    mark_preset_dirty();
    _reloadPending = true;
}

// --- Techniques -----------------------------------------------------------------------------------

void Runtime::set_technique_enabled(size_t index, bool on) {
    if (index >= _techniques.size() || _techniques[index].enabled == on) {
        return;
    }
    _techniques[index].enabled = on;
    ++_techniqueVersion;
    mark_preset_dirty();
    if (on) {
        request_gpu_for_enabled();
    }
}

void Runtime::set_technique_point(size_t index, InsertionPoint p) {
    if (index >= _techniques.size() || _techniques[index].point == p) {
        return;
    }
    _techniques[index].point = p;
    ++_techniqueVersion;
    mark_preset_dirty();
}

void Runtime::move_technique(size_t index, int delta) {
    if (index >= _techniques.size()) {
        return;
    }
    // Skip over entries the UI does not list (hidden or not present), so one press moves one row.
    size_t target = index;
    for (int steps = std::abs(delta); steps > 0;) {
        if (delta < 0 && target == 0) {
            break;
        }
        if (delta > 0 && target + 1 >= _techniques.size()) {
            break;
        }
        target = delta < 0 ? target - 1 : target + 1;
        if (_techniques[target].present && !_techniques[target].hidden) {
            --steps;
        }
    }
    if (target == index) {
        return;
    }
    TechniqueEntry moved = std::move(_techniques[index]);
    _techniques.erase(_techniques.begin() + static_cast<std::ptrdiff_t>(index));
    _techniques.insert(_techniques.begin() + static_cast<std::ptrdiff_t>(target), std::move(moved));
    ++_techniqueVersion;
    mark_preset_dirty();
}

EffectState* Runtime::effect(const std::string& file) {
    const auto it = _effects.find(file);
    return it != _effects.end() ? &it->second : nullptr;
}

void Runtime::request_gpu_for_enabled() {
    if (_builder == nullptr || _generation == nullptr) {
        return;
    }
    for (const TechniqueEntry& t : _techniques) {
        if (!t.enabled || !t.present) {
            continue;
        }
        EffectState* state = effect(t.file);
        if (state == nullptr || state->build == nullptr || state->gpu_requested || state->build->gpu_done ||
            state->build->generation != _generation->id) {
            continue;
        }
        state->gpu_requested = true;
        _builder->request_gpu(_generation->id, t.file);
    }
}

void Runtime::sort_new_techniques(size_t firstNew) {
    // As in ReShade, techniques no preset has placed are sorted by label.
    std::stable_sort(_techniques.begin() + static_cast<std::ptrdiff_t>(firstNew), _techniques.end(),
        [](const TechniqueEntry& a, const TechniqueEntry& b) {
            return upper(a.label.empty() ? a.name : a.label) < upper(b.label.empty() ? b.name : b.label);
        });
}

// --- Generations ----------------------------------------------------------------------------------

std::vector<std::filesystem::path> Runtime::find_effect_files(std::string& warnings) const {
    std::vector<std::filesystem::path> files;
    std::set<std::string> names;
    std::error_code ec;
    std::vector<std::filesystem::path> all;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             shaders_dir(), std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec)) {
            continue;
        }
        std::string ext = path_utf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".fx") {
            all.push_back(entry.path());
        }
    }
    std::sort(all.begin(), all.end());
    for (const auto& p : all) {
        const std::string name = path_utf8(p.filename());
        if (!names.insert(name).second) {
            warnings += "warning: skipping '" + path_utf8(p) + "': another effect file has the same name\n";
            continue;
        }
        files.push_back(p);
    }
    return files;
}

void Runtime::reload() { _reloadPending = true; }

void Runtime::start_generation(uint32_t width, uint32_t height, WGPUTextureFormat format) {
    Builder::Request r;
    r.generation = ++_generationCounter;
    r.width = width;
    r.height = height;
    r.format = format;
    std::string warnings;
    r.files = find_effect_files(warnings);
    if (!warnings.empty()) {
        _lastError = warnings;
    }
    // ReShade's include search: the effect's folder (added by the compiler) and every folder below
    // the shader root.
    std::error_code ec;
    r.include_paths.push_back(shaders_dir());
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             shaders_dir(), std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code dec;
        if (entry.is_directory(dec)) {
            r.include_paths.push_back(entry.path());
        }
    }
    r.texture_paths.push_back(textures_dir() / "**");
    r.texture_paths.push_back(shaders_dir() / "**");
    r.global_definitions = _preset.get_definitions("", "PreprocessorDefinitions");
    for (const auto& [section, keys] : _preset.sections()) {
        if (!section.empty() && keys.count("PreprocessorDefinitions") != 0) {
            r.effect_definitions[section] = _preset.get_definitions(section, "PreprocessorDefinitions");
        }
    }
    for (const TechniqueEntry& t : _techniques) {
        if (t.enabled) {
            r.gpu_files.insert(t.file);
        }
    }
    _requestedGeneration = r.generation;
    _requestedWidth = width;
    _requestedHeight = height;
    _requestedFormat = format;
    for (auto& [file, state] : _effects) {
        state.gpu_requested = r.gpu_files.count(file) != 0;
    }
    _builder->submit(std::move(r));
}

void Runtime::on_effect(const std::shared_ptr<EffectBuild>& build) {
    // Log what went wrong once per build: compile failures, and techniques rejected by the GPU build.
    const bool rejected = build->gpu_done && std::find(build->technique_ok.begin(), build->technique_ok.end(), 0) != build->technique_ok.end();
    if (!build->compile_ok || rejected || (build->gpu_done && build->gpu == nullptr)) {
        std::string m = build->messages;
        if (m.size() > 2000) {
            m.resize(2000);
            m += "...";
        }
        _log.push_back(build->file_name + (build->compile_ok ? ": some techniques cannot run:\n" : ": failed to compile:\n") + m);
    }
    EffectState& state = _effects[build->file_name];
    const bool sameModule = state.build != nullptr && state.build->compiled == build->compiled;
    state.build = build;
    if (!sameModule) {
        apply_preset_values(build->file_name, state);
        state.specials_frame = UINT64_MAX;
    }
    ++_effectsVersion;

    // Merge the effect's techniques into the list.
    for (TechniqueEntry& t : _techniques) {
        if (t.file == build->file_name) {
            t.present = false;
        }
    }
    if (build->compiled == nullptr) {
        ++_techniqueVersion;
        return;
    }
    const size_t firstNew = _techniques.size();
    const auto& techniques = build->compiled->module().techniques;
    for (size_t i = 0; i < techniques.size(); ++i) {
        const reshadefx::technique& tech = techniques[i];
        auto it = std::find_if(_techniques.begin(), _techniques.end(),
            [&](const TechniqueEntry& t) { return t.file == build->file_name && t.name == tech.name; });
        if (it == _techniques.end()) {
            TechniqueEntry t;
            t.file = build->file_name;
            t.name = tech.name;
            _techniques.push_back(std::move(t));
            it = _techniques.end() - 1;
        }
        it->present = true;
        it->module_index = i;
        it->hidden = annotation_int(tech.annotations, "hidden") != 0;
        it->label = annotation_string(tech.annotations, "ui_label");
        it->tooltip = annotation_string(tech.annotations, "ui_tooltip");
        if (annotation_int(tech.annotations, "enabled") != 0) {
            it->enabled = true;
        }
    }
    if (_techniques.size() > firstNew) {
        sort_new_techniques(firstNew);
    }
    ++_techniqueVersion;
}

// --- Per frame ------------------------------------------------------------------------------------

void Runtime::update(uint32_t sceneWidth, uint32_t sceneHeight, WGPUTextureFormat sceneFormat) {
    if (!_initialized) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    _inputs.frame_time_ms = std::chrono::duration<float, std::milli>(now - _lastUpdate).count();
    _lastUpdate = now;
    ++_inputs.frame_count;
    _inputs.timer_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - _start).count());
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        _inputs.date[0] = tm.tm_year + 1900;
        _inputs.date[1] = tm.tm_mon + 1;
        _inputs.date[2] = tm.tm_mday;
        _inputs.date[3] = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    }

    // Builder events.
    for (Builder::Event& e : _builder->take_events()) {
        switch (e.kind) {
        case Builder::Event::Ready:
            if (!e.message.empty()) {
                _lastError = e.message;
                _log.push_back(e.message);
            }
            break;
        case Builder::Event::GenerationReady:
            if (e.generation == _requestedGeneration) {
                _generation = e.gen;
            }
            break;
        case Builder::Event::GenerationFailed:
            if (e.generation == _requestedGeneration) {
                _lastError = "cannot create the effect render targets: " + e.message;
                _log.push_back("error: " + _lastError);
            }
            break;
        case Builder::Event::Effect:
            if (e.generation == _requestedGeneration && e.effect != nullptr) {
                on_effect(e.effect);
            }
            break;
        case Builder::Event::Message:
            _lastError = e.message;
            break;
        }
    }
    request_gpu_for_enabled();

    // New generation when the scene size or format changes, or a reload was asked for.
    if (sceneWidth >= 16 && sceneHeight >= 16 && sceneFormat != WGPUTextureFormat_Undefined) {
        if (_reloadPending || sceneWidth != _requestedWidth || sceneHeight != _requestedHeight || sceneFormat != _requestedFormat) {
            _reloadPending = false;
            start_generation(sceneWidth, sceneHeight, sceneFormat);
        }
    }

    if (_presetDirty && now - _presetChanged > std::chrono::seconds(1)) {
        save_preset_now();
    }
}

void Runtime::set_camera(const CameraDepth& camera) {
    _camera = camera;
    _cameraValid = true;
    _cameraEver = true;
}

bool Runtime::point_has_work(InsertionPoint p) const {
    if (!_active || _generation == nullptr) {
        return false;
    }
    for (const TechniqueEntry& t : _techniques) {
        if (!t.enabled || !t.present || t.point != p) {
            continue;
        }
        const auto it = _effects.find(t.file);
        if (it == _effects.end() || it->second.build == nullptr || it->second.build->gpu == nullptr) {
            continue;
        }
        const EffectBuild& b = *it->second.build;
        if (b.generation == _generation->id && t.module_index < b.technique_ok.size() && b.technique_ok[t.module_index] != 0) {
            return true;
        }
    }
    return false;
}

bool Runtime::prepare(InsertionPoint p, WGPUTextureView color, WGPUTextureView depth, uint32_t width, uint32_t height,
    uint32_t& outSlot, uint64_t& outSeq, WGPUTextureView& outResult) {
    if (_generation == nullptr || color == nullptr) {
        return false;
    }
    if (width != _generation->width || height != _generation->height || !_builder->game_shared().has_blit(_generation->color_format)) {
        // Every point is expected to see the scene target the effects were built for. Say so once
        // per generation instead of silently skipping the point every frame.
        const size_t i = static_cast<size_t>(p);
        if (_mismatchLogged[i] != _generation->id) {
            _mismatchLogged[i] = _generation->id;
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "warning: insertion point '%s' sees a %ux%u target but effects were built for %ux%u (format %d); it is skipped",
                insertion_point_label(p), width, height, _generation->width, _generation->height,
                static_cast<int>(_generation->color_format));
            _log.push_back(msg);
            _lastError = msg;
        }
        return false;
    }
    Plan plan;
    plan.generation = _generation;
    plan.color = color;
    plan.depth = depth;
    if (_cameraEver) {
        const float* m = _camera.proj_from_view;
        plan.has_camera = true;
        plan.depth_params.a = m[10];
        plan.depth_params.b = m[14];
        plan.depth_params.c = m[11];
        plan.depth_params.d = m[15];
        plan.depth_params.near_plane = _camera.near_plane;
        plan.depth_params.far_plane = _depthDistance > _camera.near_plane ? _depthDistance : _camera.far_plane;
        plan.depth_params.valid = 1.0f;
    }

    std::set<const EffectGpu*> uploaded;
    for (const TechniqueEntry& t : _techniques) {
        if (!t.enabled || !t.present || t.point != p) {
            continue;
        }
        const auto it = _effects.find(t.file);
        if (it == _effects.end() || it->second.build == nullptr || it->second.build->gpu == nullptr) {
            continue;
        }
        EffectState& state = it->second;
        const EffectBuild& b = *state.build;
        if (b.generation != _generation->id || t.module_index >= b.technique_ok.size() || b.technique_ok[t.module_index] == 0 ||
            b.gpu->generation() != _generation->id) {
            continue;
        }
        if (uploaded.insert(b.gpu.get()).second) {
            if (state.specials_frame != _inputs.frame_count) {
                state.specials_frame = _inputs.frame_count;
                state.uniforms.update_specials(_inputs);
            }
            plan.uniforms.push_back(Plan::Uniforms{b.gpu, state.uniforms.data()});
        }
        plan.steps.push_back(Plan::Step{b.gpu, static_cast<uint32_t>(t.module_index)});
    }
    if (plan.steps.empty()) {
        return false;
    }
    std::lock_guard lock(_planMutex);
    plan.seq = ++_planSeq;
    const uint32_t slot = static_cast<uint32_t>(plan.seq % kPlanSlots);
    outSlot = slot;
    outSeq = plan.seq;
    outResult = _generation->frame.backbuffer.view;
    _plans[slot] = std::move(plan);
    return true;
}

void Runtime::execute(uint32_t slot, uint64_t seq, WGPUCommandEncoder encoder, WGPUQueue queue) {
    Plan plan;
    {
        std::lock_guard lock(_planMutex);
        if (slot >= kPlanSlots || _plans[slot].seq != seq) {
            return;
        }
        // Copy what this needs; the slot keeps the generation alive until finish_plan.
        const Plan& src = _plans[slot];
        plan.generation = src.generation;
        plan.color = src.color;
        plan.depth = src.depth;
        plan.has_camera = src.has_camera;
        plan.depth_params = src.depth_params;
        plan.uniforms = std::move(_plans[slot].uniforms);
        plan.steps = std::move(_plans[slot].steps);
    }
    if (_builder == nullptr || plan.generation == nullptr) {
        return;
    }
    const GpuShared& shared = _builder->game_shared();
    const FrameTextures& frame = plan.generation->frame;
    shared.blit_to_backbuffer(encoder, plan.color, frame);
    if (plan.depth != nullptr && plan.has_camera) {
        shared.convert_depth(encoder, plan.depth, frame, plan.depth_params);
    } else {
        shared.clear_depth(encoder, frame);
    }
    for (const Plan::Uniforms& u : plan.uniforms) {
        u.gpu->write_uniforms(queue, u.data.data(), u.data.size());
    }
    RecordState state{encoder, &frame, &shared};
    for (const Plan::Step& s : plan.steps) {
        s.gpu->upload_pending(state);
        s.gpu->record_technique(state, s.technique);
    }
    _executed.store(true, std::memory_order_release);
}

void Runtime::finish_plan(uint32_t slot, uint64_t seq) {
    Plan done;
    {
        std::lock_guard lock(_planMutex);
        if (slot >= kPlanSlots || _plans[slot].seq != seq) {
            return;
        }
        done = std::move(_plans[slot]);
        _plans[slot] = Plan{};
    }
    // `done` releases its references here, after the draw that samples the result is recorded.
}

bool Runtime::executed_since_last_check() { return _executed.exchange(false, std::memory_order_acq_rel); }

const GpuShared* Runtime::game_shared() const { return _builder != nullptr ? &_builder->game_shared() : nullptr; }

std::vector<std::string> Runtime::take_log() { return std::exchange(_log, {}); }

RuntimeStatus Runtime::status() const {
    RuntimeStatus s;
    s.builder_ready = _builder != nullptr;
    s.validation_device = _builder != nullptr && _builder->validating();
    s.compile_total = _builder != nullptr ? _builder->compile_total.load() : 0;
    s.compile_done = _builder != nullptr ? _builder->compile_done.load() : 0;
    for (const auto& [file, state] : _effects) {
        ++s.effects_found;
        if (state.build != nullptr && state.build->compile_ok) {
            ++s.effects_compiled;
        } else if (state.build != nullptr) {
            ++s.effects_failed;
        }
    }
    if (_generation != nullptr) {
        s.width = _generation->width;
        s.height = _generation->height;
    }
    s.last_error = _lastError;
    return s;
}

} // namespace rsp
