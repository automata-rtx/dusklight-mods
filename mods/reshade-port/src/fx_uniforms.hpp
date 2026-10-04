// Uniform variables of one effect: CPU-side storage, typed access and the special `source` values.
//
// Mirrors ReShade's runtime (runtime.cpp: get/set_uniform_value, reset_uniform_value and the
// special-uniform update in render_effects), including its conversions between the stored type and
// the accessor type and its 16-byte stride for array elements and matrix rows. The storage layout is
// the one codegen_wgsl.cpp gives the uniform block, so the bytes upload unchanged.
//
// Game thread only.

#pragma once

#include "effect_module.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rsp {

enum class SpecialUniform : uint8_t {
    None,
    FrameTime,
    FrameCount,
    Random,
    PingPong,
    Date,
    Timer,
    Key,
    MousePoint,
    MouseDelta,
    MouseButton,
    MouseWheel,
    OverlayOpen,
    OverlayActive,
    OverlayHovered,
    Screenshot,
    Unknown,
};

SpecialUniform special_of(const reshadefx::uniform& u);

// Annotation helpers with ReShade's semantics (missing annotation -> default, index into vectors).
const reshadefx::annotation* find_annotation(const std::vector<reshadefx::annotation>& list, const char* name);
std::string annotation_string(const std::vector<reshadefx::annotation>& list, const char* name);
int annotation_int(const std::vector<reshadefx::annotation>& list, const char* name, size_t index = 0, int fallback = 0);
float annotation_float(const std::vector<reshadefx::annotation>& list, const char* name, size_t index = 0, float fallback = 0.0f);

struct FrameInputs {
    float frame_time_ms = 0.0f; // duration of the previous frame
    uint64_t frame_count = 0;
    uint32_t timer_ms = 0;      // since the runtime started
    int date[4] = {};           // year, month, day, seconds since midnight
    bool overlay_open = false;
};

class UniformStore {
public:
    void init(const reshadefx::effect_module& module);
    void reset(const reshadefx::uniform& u);
    void reset_all();

    void get(const reshadefx::uniform& u, float* values, size_t count, size_t arrayIndex = 0) const;
    void get(const reshadefx::uniform& u, int32_t* values, size_t count, size_t arrayIndex = 0) const;
    void get(const reshadefx::uniform& u, uint32_t* values, size_t count, size_t arrayIndex = 0) const;
    void get(const reshadefx::uniform& u, bool* values, size_t count, size_t arrayIndex = 0) const;
    void set(const reshadefx::uniform& u, const float* values, size_t count, size_t arrayIndex = 0);
    void set(const reshadefx::uniform& u, const int32_t* values, size_t count, size_t arrayIndex = 0);
    void set(const reshadefx::uniform& u, const uint32_t* values, size_t count, size_t arrayIndex = 0);
    void set(const reshadefx::uniform& u, const bool* values, size_t count, size_t arrayIndex = 0);

    // Updates every uniform with a `source` annotation for the coming frame.
    void update_specials(const FrameInputs& inputs);

    const std::vector<uint8_t>& data() const { return _data; }

private:
    void get_data(const reshadefx::uniform& u, uint8_t* out, size_t size, size_t baseIndex) const;
    void set_data(const reshadefx::uniform& u, const uint8_t* in, size_t size, size_t baseIndex);

    const reshadefx::effect_module* _module = nullptr;
    std::vector<uint8_t> _data;
};

} // namespace rsp
