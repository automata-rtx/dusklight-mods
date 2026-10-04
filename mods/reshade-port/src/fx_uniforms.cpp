#include "fx_uniforms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rsp {

const reshadefx::annotation* find_annotation(const std::vector<reshadefx::annotation>& list, const char* name) {
    for (const reshadefx::annotation& a : list) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

std::string annotation_string(const std::vector<reshadefx::annotation>& list, const char* name) {
    const reshadefx::annotation* a = find_annotation(list, name);
    return a != nullptr ? a->value.string_data : std::string();
}

int annotation_int(const std::vector<reshadefx::annotation>& list, const char* name, size_t index, int fallback) {
    const reshadefx::annotation* a = find_annotation(list, name);
    if (a == nullptr || index >= 16) {
        return fallback;
    }
    return a->type.is_integral() ? a->value.as_int[index] : static_cast<int>(a->value.as_float[index]);
}

float annotation_float(const std::vector<reshadefx::annotation>& list, const char* name, size_t index, float fallback) {
    const reshadefx::annotation* a = find_annotation(list, name);
    if (a == nullptr || index >= 16) {
        return fallback;
    }
    return a->type.is_floating_point() ? a->value.as_float[index] : static_cast<float>(a->value.as_int[index]);
}

SpecialUniform special_of(const reshadefx::uniform& u) {
    const std::string s = annotation_string(u.annotations, "source");
    if (s.empty()) return SpecialUniform::None;
    if (s == "frametime") return SpecialUniform::FrameTime;
    if (s == "framecount") return SpecialUniform::FrameCount;
    if (s == "random") return SpecialUniform::Random;
    if (s == "pingpong") return SpecialUniform::PingPong;
    if (s == "date") return SpecialUniform::Date;
    if (s == "timer") return SpecialUniform::Timer;
    if (s == "key") return SpecialUniform::Key;
    if (s == "mousepoint") return SpecialUniform::MousePoint;
    if (s == "mousedelta") return SpecialUniform::MouseDelta;
    if (s == "mousebutton") return SpecialUniform::MouseButton;
    if (s == "mousewheel") return SpecialUniform::MouseWheel;
    if (s == "ui_open" || s == "overlay_open") return SpecialUniform::OverlayOpen;
    if (s == "ui_active" || s == "overlay_active") return SpecialUniform::OverlayActive;
    if (s == "ui_hovered" || s == "overlay_hovered") return SpecialUniform::OverlayHovered;
    if (s == "screenshot") return SpecialUniform::Screenshot;
    return SpecialUniform::Unknown;
}

void UniformStore::init(const reshadefx::effect_module& module) {
    _module = &module;
    _data.assign((module.total_uniform_size + 15u) & ~15u, 0);
    reset_all();
}

void UniformStore::reset_all() {
    if (_module == nullptr) {
        return;
    }
    for (const reshadefx::uniform& u : _module->uniforms) {
        reset(u);
    }
}

void UniformStore::reset(const reshadefx::uniform& u) {
    if (u.offset + u.size > _data.size()) {
        return;
    }
    if (special_of(u) != SpecialUniform::None) {
        std::memset(_data.data() + u.offset, 0, u.size);
        return;
    }
    const reshadefx::constant zero = {};
    const size_t arrayLength = u.type.is_array() ? u.type.array_length : 1u;
    for (size_t i = 0; i < arrayLength; ++i) {
        const reshadefx::constant& value = u.has_initializer_value
            ? (u.type.is_array() ? (i < u.initializer_value.array_data.size() ? u.initializer_value.array_data[i] : zero)
                                 : u.initializer_value)
            : zero;
        switch (u.type.base) {
        case reshadefx::type::t_int:
            set(u, value.as_int, u.type.components(), i);
            break;
        case reshadefx::type::t_bool:
        case reshadefx::type::t_uint:
            set(u, value.as_uint, u.type.components(), i);
            break;
        case reshadefx::type::t_float:
            set(u, value.as_float, u.type.components(), i);
            break;
        default:
            break;
        }
    }
}

void UniformStore::get_data(const reshadefx::uniform& u, uint8_t* out, size_t size, size_t baseIndex) const {
    size = std::min(size, static_cast<size_t>(u.size));
    const size_t arrayLength = u.type.is_array() ? u.type.array_length : 1u;
    if (baseIndex >= arrayLength || u.offset + u.size > _data.size()) {
        return;
    }
    const uint8_t* base = _data.data() + u.offset;
    if (u.type.is_matrix()) {
        size_t i = 0;
        for (size_t a = baseIndex; a < arrayLength; ++a) {
            for (size_t row = 0; row < u.type.rows; ++row) {
                for (size_t col = 0; i < size / 4 && col < u.type.cols; ++col, ++i) {
                    std::memcpy(out + ((a - baseIndex) * u.type.components() + row * u.type.cols + col) * 4,
                        base + (a * u.type.rows * 4 + row * 4 + col) * 4, 4);
                }
            }
        }
    } else if (arrayLength > 1) {
        size_t i = 0;
        for (size_t a = baseIndex; a < arrayLength; ++a) {
            for (size_t row = 0; i < size / 4 && row < u.type.rows; ++row, ++i) {
                std::memcpy(out + ((a - baseIndex) * u.type.components() + row) * 4, base + (a * 4 + row) * 4, 4);
            }
        }
    } else {
        std::memcpy(out, base, size);
    }
}

void UniformStore::set_data(const reshadefx::uniform& u, const uint8_t* in, size_t size, size_t baseIndex) {
    size = std::min(size, static_cast<size_t>(u.size));
    const size_t arrayLength = u.type.is_array() ? u.type.array_length : 1u;
    if (baseIndex >= arrayLength || u.offset + u.size > _data.size()) {
        return;
    }
    uint8_t* base = _data.data() + u.offset;
    if (u.type.is_matrix()) {
        size_t i = 0;
        for (size_t a = baseIndex; a < arrayLength; ++a) {
            for (size_t row = 0; row < u.type.rows; ++row) {
                for (size_t col = 0; i < size / 4 && col < u.type.cols; ++col, ++i) {
                    std::memcpy(base + (a * u.type.rows * 4 + row * 4 + col) * 4,
                        in + ((a - baseIndex) * u.type.components() + row * u.type.cols + col) * 4, 4);
                }
            }
        }
    } else if (arrayLength > 1) {
        size_t i = 0;
        for (size_t a = baseIndex; a < arrayLength; ++a) {
            for (size_t row = 0; i < size / 4 && row < u.type.rows; ++row, ++i) {
                std::memcpy(base + (a * 4 + row) * 4, in + ((a - baseIndex) * u.type.components() + row) * 4, 4);
            }
        }
    } else {
        std::memcpy(base, in, size);
    }
}

// Typed access: the stored representation follows the variable's base type (floats as f32, the
// rest as 32-bit integers), converting like ReShade's D3D10+ path.

void UniformStore::get(const reshadefx::uniform& u, float* values, size_t count, size_t arrayIndex) const {
    if (u.type.is_floating_point()) {
        get_data(u, reinterpret_cast<uint8_t*>(values), count * 4, arrayIndex);
        return;
    }
    count = std::min(count, static_cast<size_t>(u.size / 4));
    std::vector<uint8_t> tmp(u.size);
    get_data(u, tmp.data(), u.size, arrayIndex);
    for (size_t i = 0; i < count; ++i) {
        int32_t si;
        uint32_t ui;
        std::memcpy(&si, tmp.data() + i * 4, 4);
        std::memcpy(&ui, tmp.data() + i * 4, 4);
        values[i] = u.type.is_signed() ? static_cast<float>(si) : static_cast<float>(ui);
    }
}

void UniformStore::get(const reshadefx::uniform& u, int32_t* values, size_t count, size_t arrayIndex) const {
    if (u.type.is_integral()) {
        get_data(u, reinterpret_cast<uint8_t*>(values), count * 4, arrayIndex);
        return;
    }
    count = std::min(count, static_cast<size_t>(u.size / 4));
    std::vector<uint8_t> tmp(u.size);
    get_data(u, tmp.data(), u.size, arrayIndex);
    for (size_t i = 0; i < count; ++i) {
        float f;
        std::memcpy(&f, tmp.data() + i * 4, 4);
        values[i] = static_cast<int32_t>(f);
    }
}

void UniformStore::get(const reshadefx::uniform& u, uint32_t* values, size_t count, size_t arrayIndex) const {
    get(u, reinterpret_cast<int32_t*>(values), count, arrayIndex);
}

void UniformStore::get(const reshadefx::uniform& u, bool* values, size_t count, size_t arrayIndex) const {
    count = std::min(count, static_cast<size_t>(u.size / 4));
    std::vector<uint8_t> tmp(u.size);
    get_data(u, tmp.data(), u.size, arrayIndex);
    for (size_t i = 0; i < count; ++i) {
        uint32_t v;
        std::memcpy(&v, tmp.data() + i * 4, 4);
        values[i] = v != 0;
    }
}

void UniformStore::set(const reshadefx::uniform& u, const float* values, size_t count, size_t arrayIndex) {
    if (u.type.is_floating_point()) {
        set_data(u, reinterpret_cast<const uint8_t*>(values), count * 4, arrayIndex);
        return;
    }
    std::vector<int32_t> tmp(count);
    for (size_t i = 0; i < count; ++i) {
        tmp[i] = static_cast<int32_t>(values[i]);
    }
    set_data(u, reinterpret_cast<const uint8_t*>(tmp.data()), count * 4, arrayIndex);
}

void UniformStore::set(const reshadefx::uniform& u, const int32_t* values, size_t count, size_t arrayIndex) {
    if (u.type.is_floating_point()) {
        std::vector<float> tmp(count);
        for (size_t i = 0; i < count; ++i) {
            tmp[i] = static_cast<float>(values[i]);
        }
        set_data(u, reinterpret_cast<const uint8_t*>(tmp.data()), count * 4, arrayIndex);
        return;
    }
    set_data(u, reinterpret_cast<const uint8_t*>(values), count * 4, arrayIndex);
}

void UniformStore::set(const reshadefx::uniform& u, const uint32_t* values, size_t count, size_t arrayIndex) {
    if (u.type.is_floating_point()) {
        std::vector<float> tmp(count);
        for (size_t i = 0; i < count; ++i) {
            tmp[i] = static_cast<float>(values[i]);
        }
        set_data(u, reinterpret_cast<const uint8_t*>(tmp.data()), count * 4, arrayIndex);
        return;
    }
    set_data(u, reinterpret_cast<const uint8_t*>(values), count * 4, arrayIndex);
}

void UniformStore::set(const reshadefx::uniform& u, const bool* values, size_t count, size_t arrayIndex) {
    if (u.type.is_floating_point()) {
        std::vector<float> tmp(count);
        for (size_t i = 0; i < count; ++i) {
            tmp[i] = values[i] ? 1.0f : 0.0f;
        }
        set_data(u, reinterpret_cast<const uint8_t*>(tmp.data()), count * 4, arrayIndex);
        return;
    }
    std::vector<uint32_t> tmp(count);
    for (size_t i = 0; i < count; ++i) {
        tmp[i] = values[i] ? 1u : 0u;
    }
    set_data(u, reinterpret_cast<const uint8_t*>(tmp.data()), count * 4, arrayIndex);
}

void UniformStore::update_specials(const FrameInputs& in) {
    if (_module == nullptr) {
        return;
    }
    for (const reshadefx::uniform& u : _module->uniforms) {
        switch (special_of(u)) {
        case SpecialUniform::FrameTime: {
            const float v = in.frame_time_ms;
            set(u, &v, 1);
            break;
        }
        case SpecialUniform::FrameCount:
            if (u.type.is_boolean()) {
                const bool v = (in.frame_count % 2) == 0;
                set(u, &v, 1);
            } else {
                const uint32_t v = static_cast<uint32_t>(in.frame_count % 0xFFFFFFFFu);
                set(u, &v, 1);
            }
            break;
        case SpecialUniform::Random: {
            const int lo = annotation_int(u.annotations, "min", 0, 0);
            const int hi = annotation_int(u.annotations, "max", 0, RAND_MAX);
            const int32_t v = lo + (std::rand() % (std::abs(hi - lo) + 1));
            set(u, &v, 1);
            break;
        }
        case SpecialUniform::PingPong: {
            const float lo = annotation_float(u.annotations, "min", 0, 0.0f);
            const float hi = annotation_float(u.annotations, "max", 0, 1.0f);
            const float stepMin = annotation_float(u.annotations, "step", 0);
            const float stepMax = annotation_float(u.annotations, "step", 1);
            float increment = stepMax == 0 ? stepMin
                                           : (stepMin + std::fmod(static_cast<float>(std::rand()), stepMax - stepMin + 1));
            const float smoothing = annotation_float(u.annotations, "smoothing");
            const float seconds = in.frame_time_ms * 1e-3f;
            float value[2] = {0, 0};
            get(u, value, 2);
            if (value[1] >= 0) {
                increment = std::max(increment - std::max(0.0f, smoothing - (hi - value[0])), 0.05f);
                increment *= seconds;
                if ((value[0] += increment) >= hi) {
                    value[0] = hi;
                    value[1] = -1;
                }
            } else {
                increment = std::max(increment - std::max(0.0f, smoothing - (value[0] - lo)), 0.05f);
                increment *= seconds;
                if ((value[0] -= increment) <= lo) {
                    value[0] = lo;
                    value[1] = +1;
                }
            }
            set(u, value, 2);
            break;
        }
        case SpecialUniform::Date:
            set(u, in.date, 4);
            break;
        case SpecialUniform::Timer: {
            const uint32_t v = in.timer_ms;
            set(u, &v, 1);
            break;
        }
        case SpecialUniform::OverlayOpen: {
            const bool v = in.overlay_open;
            set(u, &v, 1);
            break;
        }
        case SpecialUniform::OverlayActive:
        case SpecialUniform::OverlayHovered: {
            const int32_t v = 0;
            set(u, &v, 1);
            break;
        }
        case SpecialUniform::Screenshot: {
            const bool v = false;
            set(u, &v, 1);
            break;
        }
        default:
            // Keyboard and mouse sources keep their reset value (zero): there is no input service
            // to read them from yet.
            break;
        }
    }
}

} // namespace rsp
