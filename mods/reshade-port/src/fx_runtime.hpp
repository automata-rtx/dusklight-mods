// The effect runtime: finds effect files, compiles and builds them on a background thread, keeps
// the technique list and uniform values (persisted as a ReShadePreset.ini), and turns each
// insertion point of a frame into a plan the render worker executes.
//
// Threads:
//   game thread    everything public except execute() and finish_plan(): settings, UI access,
//                  update() once per frame, prepare() from stage callbacks and game hooks.
//   render worker  execute() from the GfxService compute callback, finish_plan() from the draw
//                  callback. Both only touch the plan ring (mutex) and immutable GPU objects.
//   build thread   owned by the runtime (fx_runtime.cpp, Builder). Talks to the game thread
//                  through an event queue; never touches game-thread state.
//
// Generations: BUFFER_WIDTH/HEIGHT are compiled into every effect, so a change of scene size (or
// of preprocessor definitions) starts a new generation: new frame textures, new texture pool,
// every effect recompiled. Objects of the old generation stay alive until no plan references them.

#pragma once

#include "fx_compile.hpp"
#include "fx_gpu.hpp"
#include "fx_preset.hpp"
#include "fx_uniforms.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rsp {

// Where in the game's frame a technique runs. Values are persisted; append only.
enum class InsertionPoint : uint8_t {
    BeforeTransparency = 0, // opaque world drawn; water, glass, particles and post-processing follow
    BeforeParticles = 1,    // all world geometry; particles, motion blur, DOF, heat haze, bloom follow
    BeforeHud = 2,          // the game's post-processing is done; the HUD follows
    AfterHud = 3,           // the final image, where standalone ReShade runs
};
constexpr size_t kInsertionPointCount = 4;
constexpr InsertionPoint kDefaultInsertionPoint = InsertionPoint::BeforeParticles;
const char* insertion_point_label(InsertionPoint p);
const char* insertion_point_key(InsertionPoint p);
bool parse_insertion_point(const std::string& key, InsertionPoint& out);

struct Generation {
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureFormat color_format = WGPUTextureFormat_Undefined;
    FrameTextures frame; // on the game's device
    std::shared_ptr<TexturePool> pool = std::make_shared<TexturePool>();
    ~Generation() { frame.release(); }
};

// One effect file as the build thread produced it. Immutable once published.
struct EffectBuild {
    uint64_t generation = 0;
    std::string file_name; // identity, as in ReShade presets ("SMAA.fx")
    std::filesystem::path path;
    std::shared_ptr<const CompiledEffect> compiled; // null when preprocessing failed outright
    std::shared_ptr<EffectGpu> gpu;                 // null until built (only enabled effects are built)
    std::vector<uint8_t> technique_ok;              // per module technique: validated and built
    std::string messages;                           // compiler and build errors / warnings
    bool compile_ok = false;
    bool gpu_done = false; // a GPU build was attempted (gpu may still be null if it failed)
};

struct TechniqueEntry {
    std::string file;
    std::string name;
    bool enabled = false;
    InsertionPoint point = kDefaultInsertionPoint;
    // From the current compiled effect.
    bool present = false;
    bool hidden = false;
    std::string label;
    std::string tooltip;
    size_t module_index = 0;

    std::string unique_name() const { return name + '@' + file; }
};

struct EffectState {
    std::shared_ptr<EffectBuild> build;
    UniformStore uniforms;
    uint64_t specials_frame = UINT64_MAX;
    bool gpu_requested = false;
};

// Camera of the current frame, for the DEPTH conversion.
struct CameraDepth {
    float proj_from_view[16] = {};
    float near_plane = 0.0f;
    float far_plane = 0.0f;
};

struct RuntimeStatus {
    bool builder_ready = false;
    bool validation_device = false;
    size_t effects_found = 0;
    size_t effects_compiled = 0;
    size_t effects_failed = 0;
    size_t compile_total = 0;
    size_t compile_done = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string last_error;
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    // `game` is the game's device (GfxDeviceInfo); the runtime creates its own validation device.
    bool init(const GpuApi& game, const std::filesystem::path& baseDir, std::string& errors);
    void shutdown();

    // Game thread, once per frame: frame timing, builder events, preset autosave.
    // `sceneWidth/Height/Format` is the scene target as GfxService reports it (0 if unknown).
    void update(uint32_t sceneWidth, uint32_t sceneHeight, WGPUTextureFormat sceneFormat);

    // Folders (under the base directory).
    std::filesystem::path base_dir() const { return _baseDir; }
    std::filesystem::path shaders_dir() const { return _baseDir / "reshade-shaders" / "Shaders"; }
    std::filesystem::path textures_dir() const { return _baseDir / "reshade-shaders" / "Textures"; }
    std::filesystem::path preset_path() const { return _baseDir / "ReShadePreset.ini"; }

    void reload(); // rescan the folders and rebuild everything

    // Frame recording (game thread).
    void set_camera(const CameraDepth& camera);
    void clear_camera() { _cameraValid = false; }
    void set_depth_distance(float distance) { _depthDistance = distance; } // 0: the camera's far plane
    void set_effects_active(bool on) { _active = on; }
    bool effects_active() const { return _active; }
    bool point_has_work(InsertionPoint p) const;
    // After resolve_pass: builds the plan for this point. Returns false if there is nothing to run
    // (sizes mismatch, effects still building). `outSlot`/`outSeq` identify the plan.
    bool prepare(InsertionPoint p, WGPUTextureView color, WGPUTextureView depth, uint32_t width, uint32_t height,
        uint32_t& outSlot, uint64_t& outSeq, WGPUTextureView& outResult);

    // Render worker.
    void execute(uint32_t slot, uint64_t seq, WGPUCommandEncoder encoder, WGPUQueue queue);
    void finish_plan(uint32_t slot, uint64_t seq);
    // The runtime's pipelines on the game's device; valid (immutable) whenever a plan exists.
    const GpuShared* game_shared() const;

    // UI (game thread).
    std::vector<TechniqueEntry>& techniques() { return _techniques; }
    void set_technique_enabled(size_t index, bool on);
    void set_technique_point(size_t index, InsertionPoint p);
    void move_technique(size_t index, int delta);
    std::map<std::string, EffectState>& effects() { return _effects; }
    EffectState* effect(const std::string& file);
    void uniform_changed(const std::string& file, const reshadefx::uniform& u); // persists the value
    void reset_uniform(const std::string& file, const reshadefx::uniform& u);
    std::string global_definitions() const;
    void set_global_definitions(const std::string& text);
    std::string effect_definitions(const std::string& file) const;
    void set_effect_definitions(const std::string& file, const std::string& text);
    RuntimeStatus status() const;
    uint64_t technique_list_version() const { return _techniqueVersion; }
    uint64_t effects_version() const { return _effectsVersion; }
    bool executed_since_last_check(); // render worker ran a plan since the last call
    std::vector<std::string> take_log(); // messages for the log service, oldest first

private:
    class Builder;

    void load_preset();
    void mark_preset_dirty();
    void save_preset_now();
    void start_generation(uint32_t width, uint32_t height, WGPUTextureFormat format);
    void on_effect(const std::shared_ptr<EffectBuild>& build);
    void apply_preset_values(const std::string& file, EffectState& state);
    void request_gpu_for_enabled();
    void sort_new_techniques(size_t firstNew);
    std::vector<std::filesystem::path> find_effect_files(std::string& warnings) const;

    struct Plan {
        uint64_t seq = 0;
        std::shared_ptr<Generation> generation;
        WGPUTextureView color = nullptr;
        WGPUTextureView depth = nullptr;
        bool has_camera = false;
        DepthParams depth_params;
        struct Uniforms {
            std::shared_ptr<EffectGpu> gpu;
            std::vector<uint8_t> data;
        };
        std::vector<Uniforms> uniforms;
        struct Step {
            std::shared_ptr<EffectGpu> gpu;
            uint32_t technique = 0;
        };
        std::vector<Step> steps;
    };
    static constexpr uint32_t kPlanSlots = 32;

    std::unique_ptr<Builder> _builder;
    GpuApi _game;
    std::filesystem::path _baseDir;
    bool _initialized = false;
    bool _active = true;

    // Generation state.
    uint64_t _generationCounter = 0;
    uint64_t _requestedGeneration = 0; // latest started
    uint32_t _requestedWidth = 0, _requestedHeight = 0;
    WGPUTextureFormat _requestedFormat = WGPUTextureFormat_Undefined;
    std::shared_ptr<Generation> _generation; // current, ready
    bool _reloadPending = true;

    std::map<std::string, EffectState> _effects;
    std::vector<TechniqueEntry> _techniques;
    uint64_t _techniqueVersion = 1;
    uint64_t _effectsVersion = 1;

    IniFile _preset;
    bool _presetDirty = false;
    std::chrono::steady_clock::time_point _presetChanged;

    // Frame.
    FrameInputs _inputs;
    std::chrono::steady_clock::time_point _start;
    std::chrono::steady_clock::time_point _lastUpdate;
    CameraDepth _camera;
    bool _cameraValid = false;
    bool _cameraEver = false;
    float _depthDistance = 0.0f;

    // Plans (shared with the render worker).
    mutable std::mutex _planMutex;
    std::array<Plan, kPlanSlots> _plans;
    uint64_t _planSeq = 0;
    std::atomic<bool> _executed{false};

    std::string _lastError;
    std::vector<std::string> _log;
    std::array<uint64_t, kInsertionPointCount> _mismatchLogged{}; // generation id last warned about
};

} // namespace rsp
