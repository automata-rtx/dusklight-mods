// ReShade FX -> WGSL code generation. See codegen_wgsl.hpp for the binding layout.
//
// Structure follows ReShade's own GLSL back-end (third_party/reshadefx is driven the same way), with
// these WGSL-specific parts:
//
// Matrices. As in ReShade's GLSL back-end, an HLSL floatRxC is stored as WGSL matRxC: R columns of
// C components, so column i is HLSL row i. mul(a, b) is then emitted as b * a, constructors take
// their arguments in HLSL order unchanged, and `m[i]` is HLSL row i. Matrices are always f32.
//
// Samplers. WGSL has no combined texture-sampler type and textures cannot be stored in variables, so
// every texture operation goes through a helper function generated per sampler and per operation
// (`_rsp_<op>_s<index>`). A user function that takes a sampler (or storage) parameter is
// specialised per actual argument when an entry point is assembled: its text carries placeholders
// (\x01<param id>\x02) that are resolved to the concrete sampler, and calls carry
// \x03<function id>|<tokens>\x04 markers that name the specialisation. This also lets each helper be
// generated for the stage it runs in (implicit-LOD sampling is fragment-only in WGSL).
//
// Emulated sampling. The game's device has no optional features: 32-bit float and integer textures
// cannot be filtered, there is no border address mode and no sampler LOD bias. For samplers that
// need any of these the helpers filter in the shader from textureLoad (bilinear, mip selection,
// trilinear), apply the address mode per texel and return transparent black outside a border
// sampler, which is what ReShade's D3D samplers do.
//
// Stage-dependent code (discard, derivatives, barriers) is written as \x05...\x05 markers and
// resolved per entry point.

#include "codegen_wgsl.hpp"

#include "effect_parser.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace reshadefx;

namespace rsp {
namespace {

constexpr char kObjBegin = '\x01';
constexpr char kObjEnd = '\x02';
constexpr char kCallBegin = '\x03';
constexpr char kCallEnd = '\x04';
constexpr char kStage = '\x05';
constexpr char kFnName = '\x0E';

const char* scalar_name(type::datatype base) {
    switch (base) {
    case type::t_bool: return "bool";
    case type::t_min16int:
    case type::t_int: return "i32";
    case type::t_min16uint:
    case type::t_uint: return "u32";
    default: return "f32";
    }
}

const char* sample_scalar(SampleType t) {
    switch (t) {
    case SampleType::Sint: return "i32";
    case SampleType::Uint: return "u32";
    default: return "f32";
    }
}

std::string swizzle_prefix(unsigned int count) {
    static const char* s[] = {"", "x", "xy", "xyz", "xyzw"};
    return s[count > 4 ? 4 : count];
}

// Shortest float text that reads back as the same f32, always with a decimal point or exponent.
std::string format_float(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s(buf);
    for (char& c : s) {
        if (c == ',') {
            c = '.'; // a locale with a decimal comma must not leak into WGSL
        }
    }
    if (s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }
    return s + "f";
}

bool parse_semantic(const std::string& semantic, std::string& base, uint32_t& index) {
    size_t digit = semantic.size();
    while (digit > 0 && semantic[digit - 1] >= '0' && semantic[digit - 1] <= '9') {
        --digit;
    }
    base = semantic.substr(0, digit);
    index = 0;
    for (size_t i = digit; i < semantic.size(); ++i) {
        index = index * 10 + static_cast<uint32_t>(semantic[i] - '0');
    }
    return !base.empty();
}

const std::unordered_set<std::string>& reserved_names() {
    static const std::unordered_set<std::string> names = {
        // keywords
        "alias", "break", "case", "const", "const_assert", "continue", "continuing", "default",
        "diagnostic", "discard", "else", "enable", "false", "fn", "for", "if", "let", "loop",
        "override", "requires", "return", "struct", "switch", "true", "var", "while",
        // reserved words
        "NULL", "Self", "abstract", "active", "alignas", "alignof", "as", "asm", "asm_fragment",
        "async", "attribute", "auto", "await", "become", "binding_array", "cast", "catch", "class",
        "co_await", "co_return", "co_yield", "coherent", "column_major", "common", "compile",
        "compile_fragment", "concept", "const_cast", "consteval", "constexpr", "constinit", "crate",
        "debugger", "decltype", "delete", "demote", "demote_to_helper", "do", "dynamic_cast",
        "enum", "explicit", "export", "extends", "extern", "external", "fallthrough", "filter",
        "final", "finally", "friend", "from", "fxgroup", "get", "goto", "groupshared", "highp",
        "impl", "implements", "import", "inline", "instanceof", "interface", "layout", "lowp",
        "macro", "macro_rules", "match", "mediump", "meta", "mod", "module", "move", "mut",
        "mutable", "namespace", "new", "nil", "noexcept", "noinline", "nointerpolation",
        "non_coherent", "noncoherent", "noperspective", "null", "nullptr", "of", "operator",
        "package", "packoffset", "partition", "pass", "patch", "pixelfragment", "precise",
        "precision", "premerge", "priv", "protected", "pub", "public", "readonly", "ref",
        "regardless", "register", "reinterpret_cast", "require", "resource", "restrict", "self",
        "set", "shared", "sizeof", "smooth", "snorm", "static", "static_assert", "static_cast",
        "std", "subroutine", "super", "target", "template", "this", "thread_local", "throw",
        "trait", "try", "type", "typedef", "typeid", "typename", "typeof", "union", "unless",
        "unorm", "unsafe", "unsized", "use", "using", "varying", "virtual", "volatile", "wgsl",
        "where", "with", "writeonly", "yield",
        // predeclared types and enumerants
        "array", "atomic", "bool", "f16", "f32", "i32", "u32", "mat2x2", "mat2x3", "mat2x4",
        "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "ptr", "sampler",
        "sampler_comparison", "vec2", "vec3", "vec4", "vec2f", "vec3f", "vec4f", "vec2i", "vec3i",
        "vec4i", "vec2u", "vec3u", "vec4u", "vec2h", "vec3h", "vec4h", "mat2x2f", "mat3x3f",
        "mat4x4f", "texture_1d", "texture_2d", "texture_2d_array", "texture_3d", "texture_cube",
        "texture_cube_array", "texture_multisampled_2d", "texture_storage_1d", "texture_storage_2d",
        "texture_storage_2d_array", "texture_storage_3d", "texture_depth_2d",
        "texture_external", "read", "write", "read_write", "function", "private", "workgroup",
        "uniform", "storage", "handle",
        // built-in functions
        "abs", "acos", "acosh", "all", "any", "arrayLength", "asin", "asinh", "atan", "atanh",
        "atan2", "bitcast", "ceil", "clamp", "cos", "cosh", "countLeadingZeros", "countOneBits",
        "countTrailingZeros", "cross", "degrees", "determinant", "distance", "dot", "exp", "exp2",
        "extractBits", "faceForward", "firstLeadingBit", "firstTrailingBit", "floor", "fma",
        "fract", "frexp", "insertBits", "inverseSqrt", "ldexp", "length", "log", "log2", "max",
        "min", "mix", "modf", "normalize", "pow", "quantizeToF16", "radians", "reflect", "refract",
        "reverseBits", "round", "saturate", "select", "sign", "sin", "sinh", "smoothstep", "sqrt",
        "step", "tan", "tanh", "transpose", "trunc", "dpdx", "dpdxCoarse", "dpdxFine", "dpdy",
        "dpdyCoarse", "dpdyFine", "fwidth", "fwidthCoarse", "fwidthFine", "textureDimensions",
        "textureGather", "textureGatherCompare", "textureLoad", "textureNumLayers",
        "textureNumLevels", "textureNumSamples", "textureSample", "textureSampleBias",
        "textureSampleCompare", "textureSampleCompareLevel", "textureSampleGrad",
        "textureSampleLevel", "textureSampleBaseClampToEdge", "textureStore", "textureBarrier",
        "atomicLoad", "atomicStore", "atomicAdd", "atomicSub", "atomicMax", "atomicMin",
        "atomicAnd", "atomicOr", "atomicXor", "atomicExchange", "atomicCompareExchangeWeak",
        "pack4x8snorm", "pack4x8unorm", "pack2x16snorm", "pack2x16unorm", "pack2x16float",
        "unpack4x8snorm", "unpack4x8unorm", "unpack2x16snorm", "unpack2x16unorm",
        "unpack2x16float", "storageBarrier", "workgroupBarrier", "workgroupUniformLoad",
        "main",
    };
    return names;
}

std::string escape_name(std::string name) {
    if (reserved_names().count(name) != 0) {
        name = '_' + name;
    }
    return name;
}

} // namespace

class codegen_wgsl final : public codegen {
public:
    explicit codegen_wgsl(WgslHost host) : _host(std::move(host)) {
        _blocks.emplace(0, std::string()).first->second.reserve(8192);
    }

    std::string finalize_code() const override {
        std::string code = _blocks.at(0);
        for (const auto& f : _functions) {
            if (const auto it = _blocks.find(f->id); it != _blocks.end()) {
                code += it->second;
            }
        }
        return code;
    }

    bool assemble_code_for_entry_point(const std::string& name, std::string& binary,
        std::string& assembly, std::string& errors) const override {
        WgslEntryPoint ep;
        if (!assemble(name, ep, errors)) {
            return false;
        }
        binary = ep.code;
        assembly = ep.code;
        return true;
    }

    bool assemble(const std::string& entryName, WgslEntryPoint& out, std::string& errors) const;

private:
    enum class naming { unique, general, reserved, expression };

    struct UniformInfo {
        type t;
        std::string member;
    };
    struct FuncExtra {
        std::vector<id> object_params; // sampler / storage parameters, in declaration order
        std::string prologue;          // copies of by-value parameters into mutable locals
        bool returns_value = false;
        std::string return_type;
    };
    struct EntryExtra {
        id wrapper = 0;
        shader_type stype = shader_type::unknown;
        int threads[3] = {1, 1, 1};
        uint32_t fs_outputs = 0;
        SampleType fs_types[8] = {};
        std::string text;
    };

    WgslHost _host;
    std::unordered_map<id, std::string> _names;
    std::unordered_map<id, std::string> _blocks;
    std::string _ubo_members;
    std::vector<std::tuple<type, constant, id>> _constant_lookup;
    std::unordered_map<id, id> _remapped_objects;
    std::unordered_map<id, uint32_t> _sampler_index;
    std::unordered_map<id, uint32_t> _storage_index;
    std::unordered_set<id> _object_params;
    std::unordered_map<id, UniformInfo> _uniforms;
    std::unordered_set<id> _atomic_vars;
    std::unordered_set<id> _atomic_vectors; // subset of _atomic_vars stored as array<atomic<T>, N>
    std::unordered_map<id, id> _lvalue_root;
    std::unordered_map<id, FuncExtra> _func_extra;
    std::unordered_map<std::string, EntryExtra> _entries;
    std::unordered_map<std::string, uint32_t> _varying_locations;
    std::string _current_function_declaration;
    std::vector<std::string> _unsupported; // features found that cannot be expressed; fail assembly

    // ---------------------------------------------------------------------------------------------
    // Names and types

    std::string id_to_name(id v) const {
        if (const auto it = _remapped_objects.find(v); it != _remapped_objects.end() && it->second != 0) {
            v = it->second;
        }
        if (const auto it = _names.find(v); it != _names.end()) {
            return it->second;
        }
        return '_' + std::to_string(v);
    }

    template <naming naming_type = naming::general>
    void define_name(const id v, std::string name) {
        assert(!name.empty());
        if constexpr (naming_type != naming::expression) {
            if (name[0] == '_') {
                return;
            }
            name = escape_name(std::move(name));
        }
        if constexpr (naming_type == naming::general) {
            for (const auto& [other, existing] : _names) {
                if (existing == name) {
                    name += '_' + std::to_string(v);
                    break;
                }
            }
        }
        _names[v] = std::move(name);
    }

    void write_type(std::string& s, const type& t) const {
        if (t.is_array()) {
            type elem = t;
            elem.array_length = 0;
            s += "array<";
            write_type(s, elem);
            s += ", " + std::to_string(t.is_bounded_array() ? t.array_length : 1u) + '>';
            return;
        }
        if (t.is_struct()) {
            s += id_to_name(t.struct_definition);
            return;
        }
        if (t.is_void()) {
            return;
        }
        if (!t.is_numeric()) {
            s += "f32"; // objects never reach a declaration (parameters are specialised away)
            return;
        }
        if (t.is_matrix()) {
            s += "mat" + std::to_string(t.rows) + 'x' + std::to_string(t.cols) + "<f32>";
        } else if (t.is_vector()) {
            s += "vec" + std::to_string(t.rows) + '<' + scalar_name(t.base) + '>';
        } else {
            s += scalar_name(t.base);
        }
    }
    std::string type_name(const type& t) const {
        std::string s;
        write_type(s, t);
        return s;
    }

    std::string scalar_literal(type::datatype base, const constant& c, unsigned int i) {
        switch (base) {
        case type::t_bool:
            return c.as_uint[i] ? "true" : "false";
        case type::t_min16int:
        case type::t_int:
            if (c.as_int[i] == INT32_MIN) {
                return "i32(-2147483648)";
            }
            return c.as_int[i] < 0 ? "-" + std::to_string(-static_cast<int64_t>(c.as_int[i])) + 'i'
                                    : std::to_string(c.as_int[i]) + 'i';
        case type::t_min16uint:
        case type::t_uint:
            return std::to_string(c.as_uint[i]) + 'u';
        default: {
            const float v = c.as_float[i];
            if (std::isnan(v)) {
                return "_rsp_nan()";
            }
            if (std::isinf(v)) {
                return v < 0 ? "(-_rsp_inf())" : "_rsp_inf()";
            }
            return format_float(v);
        }
        }
    }

    void write_constant(std::string& s, const type& t, const constant& c) {
        if (t.is_array()) {
            type elem = t;
            elem.array_length = 0;
            s += type_name(t) + '(';
            for (unsigned int a = 0; a < t.array_length; ++a) {
                write_constant(s, elem, a < c.array_data.size() ? c.array_data[a] : constant{});
                s += ", ";
            }
            if (t.array_length != 0) {
                s.erase(s.size() - 2);
            }
            s += ')';
            return;
        }
        if (t.is_struct()) {
            s += type_name(t) + "()";
            return;
        }
        if (t.is_scalar()) {
            s += scalar_literal(t.base, c, 0);
            return;
        }
        s += type_name(t) + '(';
        const type::datatype base = t.is_matrix() ? type::t_float : t.base;
        for (unsigned int i = 0; i < t.components(); ++i) {
            if (t.is_matrix() && !t.is_floating_point()) {
                // Integer matrices are stored as f32 matrices.
                constant f{};
                f.as_float[0] = t.is_signed() ? static_cast<float>(c.as_int[i])
                                              : static_cast<float>(c.as_uint[i]);
                s += scalar_literal(type::t_float, f, 0);
            } else {
                s += scalar_literal(base, c, i);
            }
            s += ", ";
        }
        s.erase(s.size() - 2);
        s += ')';
    }

    // Converts the value `e` of type `from` to type `to` (same-shape conversions, truncations,
    // splats and matrix reshapes as HLSL allows them).
    std::string cast_expr(const std::string& e, const type& from, const type& to) const {
        if (from.is_array() || to.is_array() || from.is_struct() || to.is_struct() ||
            !from.is_numeric() || !to.is_numeric()) {
            return e;
        }
        const bool sameBase = from.base == to.base ||
            (from.is_floating_point() && to.is_floating_point()) ||
            (from.is_matrix() && to.is_matrix());
        if (sameBase && from.rows == to.rows && from.cols == to.cols) {
            return e;
        }
        const std::string target = type_name(to);
        const char* scalar = scalar_name(to.base);
        if (to.is_matrix()) {
            std::string r = target + '(';
            const std::string col = "vec" + std::to_string(to.cols) + "<f32>";
            for (unsigned int row = 0; row < to.rows; ++row) {
                if (from.is_matrix()) {
                    const std::string c = '(' + e + ")[" + std::to_string(row) + ']';
                    r += to.cols < from.cols ? c + '.' + swizzle_prefix(to.cols) : c;
                } else if (from.is_scalar()) {
                    r += col + "(f32(" + e + "))";
                } else {
                    r += col + '(';
                    for (unsigned int c = 0; c < to.cols; ++c) {
                        r += "f32((" + e + ")[" + std::to_string(row * to.cols + c) + "]), ";
                    }
                    r.erase(r.size() - 2);
                    r += ')';
                }
                r += ", ";
            }
            r.erase(r.size() - 2);
            return r + ')';
        }
        if (from.is_matrix()) {
            if (to.is_scalar()) {
                return std::string(scalar) + "((" + e + ")[0][0])";
            }
            std::string r = target + '(';
            for (unsigned int i = 0; i < to.rows; ++i) {
                r += std::string(scalar) + "((" + e + ")[" + std::to_string(i / from.cols) + "][" +
                     std::to_string(i % from.cols) + "]), ";
            }
            r.erase(r.size() - 2);
            return r + ')';
        }
        if (to.is_scalar()) {
            return std::string(scalar) + '(' + (from.is_vector() ? '(' + e + ").x" : e) + ')';
        }
        if (from.is_scalar()) {
            return target + '(' + scalar + '(' + e + "))";
        }
        if (from.rows > to.rows) {
            return target + "((" + e + ")." + swizzle_prefix(to.rows) + ')';
        }
        return target + '(' + e + ')';
    }

    // Uniform arrays are stored with a 16-byte element stride (array<vec4<T>, N>); reading one
    // element extracts the components and converts booleans.
    std::string unpack_uniform_element(const std::string& e, const type& elem) const {
        std::string r = elem.rows < 4 ? e + '.' + swizzle_prefix(elem.rows) : e;
        if (elem.is_boolean()) {
            r = elem.rows > 1 ? "(" + r + " != vec" + std::to_string(elem.rows) + "<u32>(0u))"
                              : "(" + r + " != 0u)";
        }
        return r;
    }

    // ---------------------------------------------------------------------------------------------
    // Objects (samplers and storages)

    id resolve_object(id v) const {
        if (const auto it = _remapped_objects.find(v); it != _remapped_objects.end() && it->second != 0) {
            return resolve_object(it->second);
        }
        return v;
    }

    std::string object_token(id v) {
        v = resolve_object(v);
        if (const auto it = _sampler_index.find(v); it != _sampler_index.end()) {
            return 's' + std::to_string(it->second);
        }
        if (const auto it = _storage_index.find(v); it != _storage_index.end()) {
            return 'u' + std::to_string(it->second);
        }
        if (_object_params.count(v) != 0) {
            return std::string(1, kObjBegin) + std::to_string(v) + kObjEnd;
        }
        _unsupported.push_back("a sampler or storage value that is not a global or parameter");
        return "s0";
    }

    std::string helper_call(const char* op, id object) {
        return std::string("_rsp_") + op + '_' + object_token(object) + '(';
    }

    // ---------------------------------------------------------------------------------------------
    // Access chains

    // Builds the WGSL expression for an access chain. For a load from an atomic variable
    // (`atomicLoad` true) the load is applied to the reference part of the chain, before any cast
    // or swizzle turns it into a value.
    std::string access_path(const expression& exp, bool forStore, size_t opCount, bool atomicLoad = false) const {
        std::string expr = id_to_name(exp.base);
        const bool atomicVector = _atomic_vectors.count(exp.base) != 0;
        const UniformInfo* uniformArray = nullptr;
        if (const auto it = _uniforms.find(exp.base); it != _uniforms.end() && it->second.t.is_array()) {
            uniformArray = &it->second;
        }
        bool unpackPending = uniformArray != nullptr;
        // A swizzle directly after another swizzle is composed into one (`v.xyz.x` -> `v.x`). Tint
        // models a swizzle of a reference as a "swizzle view" and fails to lower a swizzle of one
        // ("swizzle view instruction still has usages after lowering").
        std::string lastSwizzle; // components of the swizzle `expr` ends with, if any
        size_t lastSwizzleAt = 0;
        const auto compose = [&](const std::string& components) {
            expr.resize(lastSwizzleAt);
            expr += '.' + components;
            lastSwizzle = components;
        };
        for (size_t i = 0; i < opCount; ++i) {
            const expression::operation& op = exp.chain[i];
            const bool composable = !lastSwizzle.empty() &&
                ((op.op == expression::operation::op_swizzle && !op.from.is_scalar()) ||
                 (op.op == expression::operation::op_constant_index && op.from.is_vector() && !op.from.is_array()));
            if (composable) {
                std::string components;
                if (op.op == expression::operation::op_swizzle) {
                    for (unsigned int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) {
                        components += lastSwizzle[static_cast<size_t>(op.swizzle[k])];
                    }
                } else {
                    components += lastSwizzle[op.index];
                }
                compose(components);
                continue;
            }
            lastSwizzle.clear();
            const bool valueOp = op.op == expression::operation::op_cast ||
                                 op.op == expression::operation::op_swizzle ||
                                 op.op == expression::operation::op_matrix_swizzle;
            if (atomicLoad && valueOp) {
                expr = atomicVector && i == 0 ? atomic_vector_load(expr, op.from) : "atomicLoad(&" + expr + ')';
                atomicLoad = false;
            }
            const size_t lengthBefore = expr.size();
            switch (op.op) {
            case expression::operation::op_cast:
                expr = cast_expr(expr, op.from, op.to);
                break;
            case expression::operation::op_member:
                expr += '.' + escape_name(get_struct(op.from.struct_definition).member_list[op.index].name);
                break;
            case expression::operation::op_dynamic_index:
                expr += "[i32(" + id_to_name(op.index) + ")]";
                if (unpackPending) {
                    type elem = uniformArray->t;
                    elem.array_length = 0;
                    expr = unpack_uniform_element(expr, elem);
                    unpackPending = false;
                }
                break;
            case expression::operation::op_constant_index:
                if (op.from.is_vector() && !op.from.is_array() && !(atomicVector && i == 0)) {
                    expr += '.';
                    expr += "xyzw"[op.index];
                    lastSwizzle = "xyzw"[op.index];
                    lastSwizzleAt = lengthBefore;
                } else {
                    expr += '[' + std::to_string(op.index) + ']';
                    if (unpackPending) {
                        type elem = uniformArray->t;
                        elem.array_length = 0;
                        expr = unpack_uniform_element(expr, elem);
                        unpackPending = false;
                    }
                }
                break;
            case expression::operation::op_swizzle: {
                unsigned int n = 0;
                while (n < 4 && op.swizzle[n] >= 0) {
                    ++n;
                }
                if (op.from.is_scalar()) {
                    if (n > 1 && !forStore) {
                        expr = type_name(op.to) + '(' + expr + ')';
                    }
                    break;
                }
                expr += '.';
                for (unsigned int k = 0; k < n; ++k) {
                    expr += "xyzw"[op.swizzle[k]];
                    lastSwizzle += "xyzw"[op.swizzle[k]];
                }
                lastSwizzleAt = lengthBefore;
                break;
            }
            case expression::operation::op_matrix_swizzle: {
                unsigned int n = 0;
                while (n < 4 && op.swizzle[n] >= 0) {
                    ++n;
                }
                const auto element = [&](int s) {
                    return '[' + std::to_string(s / 4) + "][" + std::to_string(s % 4) + ']';
                };
                if (n == 1) {
                    expr += element(op.swizzle[0]);
                } else {
                    std::string v = "vec" + std::to_string(n) + "<f32>(";
                    for (unsigned int k = 0; k < n; ++k) {
                        v += '(' + expr + ')' + element(op.swizzle[k]) + ", ";
                    }
                    v.erase(v.size() - 2);
                    expr = v + ')';
                }
                break;
            }
            }
        }
        if (atomicLoad) {
            expr = atomicVector && opCount == 0 ? atomic_vector_load(expr, exp.type) : "atomicLoad(&" + expr + ')';
        }
        if (unpackPending && opCount == exp.chain.size() && !forStore) {
            // Whole uniform array: rebuild a plain array value.
            type elem = uniformArray->t;
            elem.array_length = 0;
            std::string r = type_name(uniformArray->t) + '(';
            for (unsigned int a = 0; a < uniformArray->t.array_length; ++a) {
                r += unpack_uniform_element(expr + '[' + std::to_string(a) + ']', elem) + ", ";
            }
            r.erase(r.size() - 2);
            expr = r + ')';
        }
        return expr;
    }

    bool is_atomic_root(id base) const { return _atomic_vars.count(base) != 0; }

    // groupshared int/uint vectors are stored as arrays of atomics so that single components can
    // be targets of atomic operations (WGSL atomics are scalar).
    std::string atomic_vector_load(const std::string& ref, const type& t) const {
        std::string r = "vec" + std::to_string(t.rows) + '<' + scalar_name(t.base) + ">(";
        for (unsigned int k = 0; k < t.rows; ++k) {
            r += "atomicLoad(&" + ref + '[' + std::to_string(k) + "]), ";
        }
        r.erase(r.size() - 2);
        return r + ')';
    }

    // ---------------------------------------------------------------------------------------------
    // Interface (entry points)

    uint32_t varying_location(const std::string& semantic) {
        if (const auto it = _varying_locations.find(semantic); it != _varying_locations.end()) {
            return it->second;
        }
        const uint32_t loc = static_cast<uint32_t>(_varying_locations.size());
        _varying_locations.emplace(semantic, loc);
        return loc;
    }

    struct IoVar {
        std::string decl;   // "@location(0) v0 : vec4<f32>"
        std::string field;  // "v0"
        type value_type;    // the HLSL-side type this variable carries
        bool is_builtin = false;
        std::string builtin_type; // WGSL type of a builtin
        uint32_t location = 0xFFFFFFFF;
    };

    // Describes how one interface value with `semantic` is passed. Returns false if dropped.
    bool make_io(shader_type stype, bool input, const type& t, const std::string& semanticIn,
        IoVar& io, uint32_t fieldIndex) {
        const std::string field = "v" + std::to_string(fieldIndex);
        std::string semantic = semanticIn;
        std::transform(semantic.begin(), semantic.end(), semantic.begin(),
            [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
        std::string base;
        uint32_t index = 0;
        parse_semantic(semantic, base, index);
        io.field = field;
        io.value_type = t;

        const auto builtin = [&](const char* name, const char* wtype) {
            io.is_builtin = true;
            io.builtin_type = wtype;
            io.decl = std::string("@builtin(") + name + ") " + field + " : " + wtype;
            return true;
        };
        if (stype == shader_type::vertex) {
            if (input) {
                if (semantic == "SV_VERTEXID") {
                    return builtin("vertex_index", "u32");
                }
                if (semantic == "SV_INSTANCEID") {
                    return builtin("instance_index", "u32");
                }
                return false; // no vertex buffers: other inputs read as zero
            }
            if (semantic == "SV_POSITION" || semantic == "POSITION") {
                return builtin("position", "vec4<f32>");
            }
            if (semantic == "SV_POINTSIZE" || semantic == "PSIZE") {
                return false;
            }
        } else if (stype == shader_type::pixel) {
            if (input) {
                if (semantic == "SV_POSITION" || semantic == "VPOS" || semantic == "POSITION") {
                    return builtin("position", "vec4<f32>");
                }
                if (semantic == "SV_ISFRONTFACE" || semantic == "VFACE") {
                    return builtin("front_facing", "bool");
                }
                if (semantic == "SV_SAMPLEINDEX") {
                    return builtin("sample_index", "u32");
                }
            } else {
                if (semantic == "SV_DEPTH" || semantic == "DEPTH") {
                    return false; // ReShade passes never test depth
                }
                if (base == "SV_TARGET" || base == "COLOR") {
                    io.location = index;
                    const char* scalar = t.is_integral() && !t.is_boolean() ? scalar_name(t.base) : "f32";
                    io.decl = "@location(" + std::to_string(index) + ") " + field + " : vec4<" +
                              scalar + '>';
                    return index < 8;
                }
            }
        } else if (stype == shader_type::compute && input) {
            if (semantic == "SV_DISPATCHTHREADID") {
                return builtin("global_invocation_id", "vec3<u32>");
            }
            if (semantic == "SV_GROUPID") {
                return builtin("workgroup_id", "vec3<u32>");
            }
            if (semantic == "SV_GROUPTHREADID") {
                return builtin("local_invocation_id", "vec3<u32>");
            }
            if (semantic == "SV_GROUPINDEX") {
                return builtin("local_invocation_index", "u32");
            }
            return false;
        }
        if (stype == shader_type::compute) {
            return false;
        }
        // Inter-stage varying: always a 4-component vector so a vertex shader writing float4 and a
        // pixel shader reading float2 under one semantic still link (WebGPU requires equal types).
        const uint32_t loc = varying_location(semantic.empty() ? "TEXCOORD" : semantic);
        io.location = loc;
        const bool integral = t.is_integral() && !t.is_boolean();
        const char* scalar = integral ? scalar_name(t.base) : "f32";
        io.decl = (integral ? "@location(" + std::to_string(loc) + ") @interpolate(flat) "
                            : "@location(" + std::to_string(loc) + ") ") +
                  field + " : vec4<" + scalar + '>';
        return loc < 15;
    }

    // Reads an interface variable into a value of the HLSL-side type.
    std::string read_io(const IoVar& io, const std::string& src) const {
        const type& t = io.value_type;
        if (io.is_builtin) {
            type bt = {};
            if (io.builtin_type == "u32") {
                bt.base = type::t_uint;
                bt.rows = 1;
            } else if (io.builtin_type == "bool") {
                bt.base = type::t_bool;
                bt.rows = 1;
            } else if (io.builtin_type == "vec3<u32>") {
                bt.base = type::t_uint;
                bt.rows = 3;
            } else {
                bt.base = type::t_float;
                bt.rows = 4;
            }
            bt.cols = 1;
            return cast_expr(src, bt, t);
        }
        type vt{};
        vt.base = t.is_integral() && !t.is_boolean() ? t.base : type::t_float;
        vt.rows = 4;
        vt.cols = 1;
        type want = t;
        want.qualifiers = 0;
        return cast_expr(src, vt, want);
    }

    // Writes a value of the HLSL-side type to an interface variable.
    std::string write_io(const IoVar& io, const std::string& value) const {
        const type& t = io.value_type;
        if (io.is_builtin) {
            type bt{};
            bt.base = type::t_float;
            bt.rows = 4;
            bt.cols = 1;
            if (t.rows == 4 && t.is_floating_point()) {
                return value;
            }
            return cast_expr(value, t, bt);
        }
        const bool integral = t.is_integral() && !t.is_boolean();
        const char* scalar = integral ? scalar_name(t.base) : "f32";
        const std::string zero = integral ? (t.is_signed() ? "0i" : "0u") : "0.0";
        const std::string one = integral ? (t.is_signed() ? "1i" : "1u") : "1.0";
        type st = t;
        st.qualifiers = 0;
        type ct = st;
        ct.base = integral ? t.base : type::t_float;
        const std::string v = cast_expr(value, st, ct);
        const std::string vec4 = std::string("vec4<") + scalar + '>';
        switch (t.is_matrix() ? 4 : t.rows) {
        case 1: return vec4 + '(' + v + ", " + zero + ", " + zero + ", " + one + ')';
        case 2: return vec4 + '(' + v + ", " + zero + ", " + one + ')';
        case 3: return vec4 + '(' + v + ", " + one + ')';
        default: return v;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Code generation interface

    id define_struct(const location&, struct_type& info) override {
        const id res = info.id = make_id();
        define_name<naming::unique>(res, info.unique_name);
        _structs.push_back(info);

        std::string& code = _blocks.at(_current_block);
        code += "struct " + id_to_name(res) + " {\n";
        for (const member_type& member : info.member_list) {
            code += '\t' + escape_name(member.name) + " : " + type_name(member.type) + ",\n";
        }
        if (info.member_list.empty()) {
            code += "\t_dummy : f32,\n";
        }
        code += "}\n";
        return res;
    }

    id define_texture(const location&, texture& info) override {
        const id res = info.id = make_id();
        _module.textures.push_back(info);
        return res;
    }

    id define_sampler(const location&, const texture&, sampler& info) override {
        const id res = info.id = make_id();
        define_name<naming::unique>(res, info.unique_name);
        _sampler_index[res] = static_cast<uint32_t>(_module.samplers.size());
        _module.samplers.push_back(info);
        return res;
    }

    id define_storage(const location&, const texture&, storage& info) override {
        const id res = info.id = make_id();
        define_name<naming::unique>(res, info.unique_name);
        _storage_index[res] = static_cast<uint32_t>(_module.storages.size());
        _module.storages.push_back(info);
        return res;
    }

    id define_uniform(const location&, uniform& info) override {
        const id res = make_id();

        // Same layout as ReShade's GLSL back-end (std140): the runtime fills the buffer by offset.
        auto align_up = [](uint32_t size, uint32_t alignment) {
            return (size + alignment - 1) & ~(alignment - 1);
        };
        uint32_t alignment = (info.type.rows == 3 ? 4 : info.type.rows) * 4;
        info.size = info.type.rows * 4;
        if (info.type.is_matrix()) {
            alignment = 16;
            info.size = info.type.rows * alignment;
        }
        if (info.type.is_array()) {
            alignment = 16;
            info.size = align_up(info.size, alignment) * info.type.array_length;
        }
        info.offset = align_up(_module.total_uniform_size, alignment);
        _module.total_uniform_size = info.offset + info.size;

        const std::string member = escape_name(info.unique_name);
        std::string mtype;
        const char* storedScalar = info.type.is_boolean() ? "u32" : scalar_name(info.type.base);
        if (info.type.is_matrix()) {
            const uint32_t count = info.type.rows * (info.type.is_array() ? info.type.array_length : 1u);
            mtype = "array<vec4<f32>, " + std::to_string(count) + '>';
        } else if (info.type.is_array()) {
            mtype = std::string("array<vec4<") + storedScalar + ">, " +
                    std::to_string(info.type.array_length) + '>';
        } else if (info.type.rows > 1) {
            mtype = "vec" + std::to_string(info.type.rows) + '<' + storedScalar + '>';
        } else {
            mtype = storedScalar;
        }
        _ubo_members += '\t' + member + " : " + mtype + ",\n";

        const std::string path = "_rsp_G." + member;
        std::string expr = path;
        if (info.type.is_matrix() && !info.type.is_array()) {
            expr = type_name(info.type) + '(';
            for (unsigned int r = 0; r < info.type.rows; ++r) {
                expr += path + '[' + std::to_string(r) + ']';
                if (info.type.cols < 4) {
                    expr += '.' + swizzle_prefix(info.type.cols);
                }
                expr += ", ";
            }
            expr.erase(expr.size() - 2);
            expr += ')';
        } else if (info.type.is_matrix() && info.type.is_array()) {
            _unsupported.push_back("uniform arrays of matrices");
        } else if (info.type.is_boolean() && !info.type.is_array()) {
            expr = info.type.rows > 1 ? "(" + path + " != vec" + std::to_string(info.type.rows) + "<u32>(0u))"
                                      : "(" + path + " != 0u)";
        }
        _names[res] = expr;
        _uniforms[res] = UniformInfo{info.type, member};
        _module.uniforms.push_back(info);
        return res;
    }

    id define_variable(const location&, const type& t, std::string name, bool global, id initializer) override {
        if (initializer != 0 && t.has(type::q_const) &&
            std::find_if(_constant_lookup.begin(), _constant_lookup.end(),
                [initializer](const auto& x) { return initializer == std::get<2>(x); }) !=
                _constant_lookup.end()) {
            return initializer;
        }

        const id res = make_id();
        if (!global && (t.is_sampler() || t.is_storage())) {
            _remapped_objects[res] = 0;
            return res;
        }
        if (!name.empty()) {
            define_name<naming::general>(res, name);
        }

        std::string& code = _blocks.at(_current_block);
        const std::string nm = id_to_name(res);
        if (global) {
            if (t.has(type::q_groupshared)) {
                type elem = t;
                elem.array_length = 0;
                if (!t.is_array() && elem.is_vector() && elem.is_integral() && !elem.is_boolean()) {
                    code += "var<workgroup> " + nm + " : array<atomic<" + scalar_name(elem.base) + ">, " +
                            std::to_string(elem.rows) + ">;\n";
                    _atomic_vars.insert(res);
                    _atomic_vectors.insert(res);
                } else if (elem.is_scalar() && elem.is_integral() && !elem.is_boolean()) {
                    const std::string at = std::string("atomic<") + scalar_name(elem.base) + '>';
                    code += "var<workgroup> " + nm + " : " +
                            (t.is_array() ? "array<" + at + ", " + std::to_string(t.array_length) + '>' : at) +
                            ";\n";
                    _atomic_vars.insert(res);
                } else {
                    code += "var<workgroup> " + nm + " : " + type_name(t) + ";\n";
                }
            } else if (t.has(type::q_const) && initializer != 0) {
                code += "const " + nm + " : " + type_name(t) + " = " + id_to_name(initializer) + ";\n";
            } else {
                code += "var<private> " + nm + " : " + type_name(t);
                if (initializer != 0) {
                    code += " = " + id_to_name(initializer);
                }
                code += ";\n";
            }
        } else {
            code += "\tvar " + nm + " : " + type_name(t);
            if (initializer != 0) {
                code += " = " + id_to_name(initializer);
            }
            code += ";\n";
        }
        return res;
    }

    id define_function(const location&, function& info) override {
        const id res = info.id = make_id();
        // HLSL overloads share a unique name; WGSL has no overloading, so later ones get a suffix.
        define_name<naming::general>(res, info.unique_name);

        FuncExtra extra;
        std::string decl = std::string("fn ") + kFnName + '(';
        bool first = true;
        for (member_type& param : info.parameter_list) {
            param.id = make_id();
            if (param.type.is_object()) {
                _object_params.insert(param.id);
                extra.object_params.push_back(param.id);
                continue;
            }
            type pt = param.type;
            pt.qualifiers = 0;
            if (!first) {
                decl += ", ";
            }
            first = false;
            if (param.type.has(type::q_out)) {
                const std::string pn = "_p" + std::to_string(param.id);
                decl += pn + " : ptr<function, " + type_name(pt) + '>';
                _names[param.id] = "(*" + pn + ')';
            } else {
                define_name<naming::general>(param.id, param.name);
                const std::string pn = "_p" + std::to_string(param.id);
                decl += pn + " : " + type_name(pt);
                extra.prologue += "\tvar " + id_to_name(param.id) + " : " + type_name(pt) + " = " + pn + ";\n";
            }
        }
        decl += ')';
        if (!info.return_type.is_void()) {
            type rt = info.return_type;
            rt.qualifiers = 0;
            extra.returns_value = true;
            extra.return_type = type_name(rt);
            decl += " -> " + extra.return_type;
        }
        decl += ' ';
        _current_function_declaration = decl;
        _func_extra[res] = std::move(extra);

        _functions.push_back(std::make_unique<function>(info));
        _current_function = _functions.back().get();
        return res;
    }

    void define_entry_point(function& func) override {
        assert(!func.unique_name.empty() && func.unique_name[0] == 'F');
        func.unique_name[0] = 'E';
        if (func.type == shader_type::compute) {
            func.unique_name += '_' + std::to_string(func.num_threads[0]) + '_' +
                                std::to_string(func.num_threads[1]) + '_' +
                                std::to_string(func.num_threads[2]);
        }
        if (std::find_if(_module.entry_points.begin(), _module.entry_points.end(),
                [&func](const auto& ep) { return ep.first == func.unique_name; }) !=
            _module.entry_points.end()) {
            return;
        }
        _module.entry_points.emplace_back(func.unique_name, func.type);

        EntryExtra entry;
        entry.stype = func.type;
        entry.threads[0] = std::max(func.num_threads[0], 1);
        entry.threads[1] = std::max(func.num_threads[1], 1);
        entry.threads[2] = std::max(func.num_threads[2], 1);

        std::vector<IoVar> inputs;
        std::vector<IoVar> outputs;
        std::string body;
        uint32_t fieldIndex = 0;

        // Builds a value from interface inputs.
        std::function<std::string(const type&, const std::string&)> read_param;
        read_param = [&](const type& t, const std::string& semantic) -> std::string {
            if (t.is_struct()) {
                const struct_type& def = get_struct(t.struct_definition);
                std::string ctor = type_name(t) + '(';
                for (const member_type& m : def.member_list) {
                    ctor += read_param(m.type, m.semantic) + ", ";
                }
                if (!def.member_list.empty()) {
                    ctor.erase(ctor.size() - 2);
                }
                return ctor + ')';
            }
            type vt = t;
            vt.qualifiers = 0;
            if (vt.is_matrix()) {
                // One varying per row, as HLSL allocates matrix interface variables.
                std::string base;
                uint32_t index = 0;
                parse_semantic(semantic, base, index);
                type row = vt;
                row.rows = vt.cols;
                row.cols = 1;
                std::string ctor = type_name(vt) + '(';
                for (unsigned int r = 0; r < vt.rows; ++r) {
                    IoVar io;
                    if (make_io(func.type, true, row, base + std::to_string(index + r), io, fieldIndex++)) {
                        inputs.push_back(io);
                        ctor += read_io(io, "_i." + io.field) + ", ";
                    } else {
                        ctor += type_name(row) + "(), ";
                    }
                }
                ctor.erase(ctor.size() - 2);
                return ctor + ')';
            }
            IoVar io;
            if (make_io(func.type, true, vt, semantic, io, fieldIndex++)) {
                inputs.push_back(io);
                return read_io(io, "_i." + io.field);
            }
            return type_name(vt) + "()";
        };
        // Writes a value to interface outputs.
        std::function<void(const type&, const std::string&, const std::string&)> write_value;
        write_value = [&](const type& t, const std::string& semantic, const std::string& value) {
            if (t.is_struct()) {
                const struct_type& def = get_struct(t.struct_definition);
                for (const member_type& m : def.member_list) {
                    write_value(m.type, m.semantic, value + '.' + escape_name(m.name));
                }
                return;
            }
            type vt = t;
            vt.qualifiers = 0;
            if (vt.is_matrix()) {
                std::string base;
                uint32_t index = 0;
                parse_semantic(semantic, base, index);
                type row = vt;
                row.rows = vt.cols;
                row.cols = 1;
                for (unsigned int r = 0; r < vt.rows; ++r) {
                    IoVar io;
                    if (make_io(func.type, false, row, base + std::to_string(index + r), io, fieldIndex++)) {
                        outputs.push_back(io);
                        body += "\t_o." + io.field + " = " + write_io(io, '(' + value + ")[" + std::to_string(r) + ']') + ";\n";
                    }
                }
                return;
            }
            IoVar io;
            if (make_io(func.type, false, vt, semantic, io, fieldIndex++)) {
                outputs.push_back(io);
                body += "\t_o." + io.field + " = " + write_io(io, value) + ";\n";
            }
        };

        std::string call = std::string(1, kCallBegin) + std::to_string(func.id) + '|' + kCallEnd + '(';
        for (size_t i = 0; i < func.parameter_list.size(); ++i) {
            const member_type& param = func.parameter_list[i];
            type pt = param.type;
            pt.qualifiers = 0;
            const std::string local = "_a" + std::to_string(i);
            if (param.type.is_array()) {
                // Arrays of varyings: one interface variable per element.
                type elem = pt;
                elem.array_length = 0;
                std::string base;
                uint32_t index = 0;
                parse_semantic(param.semantic, base, index);
                if (param.type.has(type::q_in)) {
                    std::string ctor = type_name(pt) + '(';
                    for (uint32_t a = 0; a < pt.array_length; ++a) {
                        ctor += read_param(elem, base + std::to_string(index + a)) + ", ";
                    }
                    ctor.erase(ctor.size() - 2);
                    body += "\tvar " + local + " : " + type_name(pt) + " = " + ctor + ");\n";
                } else {
                    body += "\tvar " + local + " : " + type_name(pt) + ";\n";
                }
            } else if (param.type.has(type::q_in)) {
                body += "\tvar " + local + " : " + type_name(pt) + " = " + read_param(pt, param.semantic) + ";\n";
            } else {
                body += "\tvar " + local + " : " + type_name(pt) + ";\n";
            }
            call += (param.type.has(type::q_out) ? "&" : "") + local;
            if (i + 1 < func.parameter_list.size()) {
                call += ", ";
            }
        }
        call += ')';

        std::string afterCall;
        std::swap(body, afterCall); // inputs are read first; output writes are collected below
        const std::string inputCode = afterCall;
        afterCall.clear();

        if (!func.return_type.is_void()) {
            type rt = func.return_type;
            rt.qualifiers = 0;
            body += "\tlet _r : " + type_name(rt) + " = " + call + ";\n";
            write_value(rt, func.return_semantic, "_r");
        } else {
            body += '\t' + call + ";\n";
        }
        for (size_t i = 0; i < func.parameter_list.size(); ++i) {
            const member_type& param = func.parameter_list[i];
            if (!param.type.has(type::q_out)) {
                continue;
            }
            type pt = param.type;
            pt.qualifiers = 0;
            const std::string local = "_a" + std::to_string(i);
            if (pt.is_array()) {
                type elem = pt;
                elem.array_length = 0;
                std::string base;
                uint32_t index = 0;
                parse_semantic(param.semantic, base, index);
                for (uint32_t a = 0; a < pt.array_length; ++a) {
                    write_value(elem, base + std::to_string(index + a), local + '[' + std::to_string(a) + ']');
                }
            } else {
                write_value(pt, param.semantic, local);
            }
        }

        std::string text;
        const std::string tag = std::to_string(func.id) + '_' + std::to_string(static_cast<int>(func.type));
        if (!inputs.empty()) {
            text += "struct _rsp_In" + tag + " {\n";
            for (const IoVar& io : inputs) {
                text += '\t' + io.decl + ",\n";
            }
            text += "}\n";
        }
        if (!outputs.empty()) {
            text += "struct _rsp_Out" + tag + " {\n";
            for (const IoVar& io : outputs) {
                text += '\t' + io.decl + ",\n";
                if (func.type == shader_type::pixel && io.location < 8) {
                    entry.fs_outputs |= 1u << io.location;
                    const type& vt = io.value_type;
                    entry.fs_types[io.location] = vt.is_integral() && !vt.is_boolean()
                        ? (vt.is_signed() ? SampleType::Sint : SampleType::Uint)
                        : SampleType::Float;
                }
            }
            text += "}\n";
        }
        switch (func.type) {
        case shader_type::vertex: text += "@vertex "; break;
        case shader_type::pixel: text += "@fragment "; break;
        default:
            text += "@compute @workgroup_size(" + std::to_string(entry.threads[0]) + ", " +
                    std::to_string(entry.threads[1]) + ", " + std::to_string(entry.threads[2]) + ") ";
            break;
        }
        text += "fn main(" + (inputs.empty() ? std::string() : "_i : _rsp_In" + tag) + ')';
        if (!outputs.empty()) {
            text += " -> _rsp_Out" + tag;
        }
        text += " {\n" + inputCode;
        if (!outputs.empty()) {
            text += "\tvar _o : _rsp_Out" + tag + ";\n";
        }
        text += body;
        if (!outputs.empty()) {
            text += "\treturn _o;\n";
        }
        text += "}\n";
        entry.text = std::move(text);

        function ep = func;
        ep.id = make_id();
        ep.referenced_functions.push_back(func.id);
        ep.return_type = {type::t_void};
        ep.parameter_list.clear();
        entry.wrapper = ep.id;
        _functions.push_back(std::make_unique<function>(std::move(ep)));
        _entries.emplace(func.unique_name, std::move(entry));
    }

    id emit_load(const expression& exp, bool force_new_id) override {
        if (exp.is_constant) {
            return emit_constant(exp.type, exp.constant);
        }
        if (exp.chain.empty() && !force_new_id && !is_atomic_root(exp.base) &&
            _uniforms.find(exp.base) == _uniforms.end()) {
            return exp.base;
        }
        const id res = make_id();
        const std::string expr = access_path(exp, false, exp.chain.size(), is_atomic_root(exp.base));
        if (force_new_id) {
            std::string& code = _blocks.at(_current_block);
            type t = exp.type;
            t.qualifiers = 0;
            code += "\tlet " + id_to_name(res) + " : " + type_name(t) + " = " + expr + ";\n";
        } else {
            define_name<naming::expression>(res, std::move(expr));
        }
        return res;
    }

    id emit_access_chain(const expression& exp, size_t& chain_index) override {
        chain_index = exp.chain.size();
        if (exp.chain.empty()) {
            return resolve_object(exp.base);
        }
        const id res = make_id();
        define_name<naming::expression>(res, access_path(exp, true, exp.chain.size()));
        _lvalue_root[res] = exp.base;
        return res;
    }

    void emit_store(const expression& exp, id value) override {
        if (const auto it = _remapped_objects.find(exp.base); it != _remapped_objects.end()) {
            it->second = resolve_object(value);
            return;
        }
        std::string& code = _blocks.at(_current_block);

        // Normalise trailing swizzles: WGSL can only assign single vector components.
        std::vector<int> components;
        size_t pathOps = exp.chain.size();
        bool matrixSwizzle = false;
        while (pathOps > 0) {
            const expression::operation& op = exp.chain[pathOps - 1];
            if (op.op == expression::operation::op_swizzle && !op.from.is_scalar()) {
                std::vector<int> current;
                for (int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) {
                    current.push_back(op.swizzle[k]);
                }
                if (!components.empty()) {
                    for (int& c : components) {
                        c = current[c];
                    }
                } else {
                    components = current;
                }
                --pathOps;
                continue;
            }
            if (op.op == expression::operation::op_constant_index && op.from.is_vector() &&
                !op.from.is_array() && components.empty() && pathOps >= 2 &&
                exp.chain[pathOps - 2].op == expression::operation::op_swizzle) {
                components.push_back(static_cast<int>(op.index));
                --pathOps;
                continue;
            }
            if (op.op == expression::operation::op_matrix_swizzle && components.empty()) {
                for (int k = 0; k < 4 && op.swizzle[k] >= 0; ++k) {
                    components.push_back(op.swizzle[k]);
                }
                matrixSwizzle = true;
                --pathOps;
            }
            break;
        }

        const std::string path = access_path(exp, true, pathOps);
        std::string v = id_to_name(value);
        if (components.size() > 1 || (_atomic_vectors.count(exp.base) != 0 && pathOps == 0 && components.empty())) {
            // Evaluate the value once before the per-component stores: it may read the target
            // (`v.xy = v.yx`), and Tint cannot lower a swizzle of a swizzled reference.
            const std::string tmp = "_rsp_v" + std::to_string(make_id());
            code += "	let " + tmp + " = " + v + ";\n";
            v = tmp;
        }
        if (_atomic_vectors.count(exp.base) != 0) {
            if (pathOps == 0 && components.empty()) {
                for (unsigned int k = 0; k < exp.type.rows; ++k) {
                    code += "\tatomicStore(&" + path + '[' + std::to_string(k) + "], (" + v + ")[" + std::to_string(k) + "]);\n";
                }
            } else if (pathOps == 0) {
                for (size_t k = 0; k < components.size(); ++k) {
                    const std::string src = components.size() == 1 ? v : '(' + v + ")[" + std::to_string(k) + ']';
                    code += "\tatomicStore(&" + path + '[' + std::to_string(components[k]) + "], " + src + ");\n";
                }
            } else {
                code += "\tatomicStore(&" + path + ", " + v + ");\n";
            }
            return;
        }
        if (components.empty()) {
            if (is_atomic_root(exp.base)) {
                code += "\tatomicStore(&" + path + ", " + v + ");\n";
            } else {
                code += '\t' + path + " = " + v + ";\n";
            }
            return;
        }
        for (size_t k = 0; k < components.size(); ++k) {
            const std::string src = components.size() == 1 ? v : v + '.' + "xyzw"[k];
            if (matrixSwizzle) {
                code += '\t' + path + '[' + std::to_string(components[k] / 4) + "][" +
                        std::to_string(components[k] % 4) + "] = " + src + ";\n";
            } else {
                code += '\t' + path + '.' + "xyzw"[components[k]] + " = " + src + ";\n";
            }
        }
    }

    id emit_constant(const type& t, const constant& data) override {
        const id res = make_id();
        if (t.is_array() || t.is_struct()) {
            if (const auto it = std::find_if(_constant_lookup.begin(), _constant_lookup.end(),
                    [&t, &data](const std::tuple<type, constant, id>& x) {
                        if (!(std::get<0>(x) == t &&
                              std::memcmp(&std::get<1>(x).as_uint[0], &data.as_uint[0], sizeof(uint32_t) * 16) == 0 &&
                              std::get<1>(x).array_data.size() == data.array_data.size())) {
                            return false;
                        }
                        for (size_t i = 0; i < data.array_data.size(); ++i) {
                            if (std::memcmp(&std::get<1>(x).array_data[i].as_uint[0],
                                    &data.array_data[i].as_uint[0], sizeof(uint32_t) * 16) != 0) {
                                return false;
                            }
                        }
                        return true;
                    });
                it != _constant_lookup.end()) {
                return std::get<2>(*it);
            } else if (t.is_array()) {
                _constant_lookup.push_back({t, data, res});
            }
            std::string& code = _blocks.at(0);
            type ct = t;
            ct.qualifiers = 0;
            if (t.is_struct()) {
                code += "var<private> " + id_to_name(res) + " : " + type_name(ct) + ";\n";
            } else {
                code += "const " + id_to_name(res) + " : " + type_name(ct) + " = ";
                write_constant(code, ct, data);
                code += ";\n";
            }
            return res;
        }
        std::string code;
        type ct = t;
        ct.qualifiers = 0;
        write_constant(code, ct, data);
        define_name<naming::expression>(res, std::move(code));
        return res;
    }

    std::string let_prefix(id res, const type& t) const {
        type ct = t;
        ct.qualifiers = 0;
        return "\tlet " + id_to_name(res) + " : " + type_name(ct) + " = ";
    }

    id emit_unary_op(const location&, tokenid op, const type& res_type, id val) override {
        const id res = make_id();
        std::string& code = _blocks.at(_current_block);
        const std::string v = id_to_name(val);
        std::string expr;
        switch (op) {
        case tokenid::minus:
            expr = res_type.is_matrix() ? '(' + v + " * -1.0)" : "-(" + v + ')';
            break;
        case tokenid::tilde:
            expr = "~(" + v + ')';
            break;
        case tokenid::exclaim:
            expr = "!(" + type_name(res_type) + '(' + v + "))";
            break;
        default:
            assert(false);
        }
        code += let_prefix(res, res_type) + expr + ";\n";
        return res;
    }

    std::string matrix_columnwise(const std::string& op, const type& t, const std::string& a, const std::string& b) const {
        std::string r = type_name(t) + '(';
        for (unsigned int i = 0; i < t.rows; ++i) {
            r += '(' + a + ")[" + std::to_string(i) + "] " + op + " (" + b + ")[" + std::to_string(i) + "], ";
        }
        r.erase(r.size() - 2);
        return r + ')';
    }

    id emit_binary_op(const location&, tokenid op, const type& res_type, const type& exp_type, id lhs, id rhs) override {
        const id res = make_id();
        std::string& code = _blocks.at(_current_block);
        const std::string a = id_to_name(lhs);
        std::string b = id_to_name(rhs);
        std::string o;
        switch (op) {
        case tokenid::plus:
        case tokenid::plus_plus:
        case tokenid::plus_equal: o = "+"; break;
        case tokenid::minus:
        case tokenid::minus_minus:
        case tokenid::minus_equal: o = "-"; break;
        case tokenid::star:
        case tokenid::star_equal: o = "*"; break;
        case tokenid::slash:
        case tokenid::slash_equal: o = "/"; break;
        case tokenid::percent:
        case tokenid::percent_equal: o = "%"; break;
        case tokenid::caret:
        case tokenid::caret_equal: o = exp_type.is_boolean() ? "!=" : "^"; break;
        case tokenid::pipe:
        case tokenid::pipe_equal: o = "|"; break;
        case tokenid::ampersand:
        case tokenid::ampersand_equal: o = "&"; break;
        case tokenid::less_less:
        case tokenid::less_less_equal: o = "<<"; break;
        case tokenid::greater_greater:
        case tokenid::greater_greater_equal: o = ">>"; break;
        case tokenid::pipe_pipe: o = exp_type.is_vector() ? "|" : "||"; break;
        case tokenid::ampersand_ampersand: o = exp_type.is_vector() ? "&" : "&&"; break;
        case tokenid::less: o = "<"; break;
        case tokenid::less_equal: o = "<="; break;
        case tokenid::greater: o = ">"; break;
        case tokenid::greater_equal: o = ">="; break;
        case tokenid::equal_equal: o = "=="; break;
        case tokenid::exclaim_equal: o = "!="; break;
        default: assert(false);
        }
        std::string expr;
        if (exp_type.is_matrix() && (o == "*" || o == "/" || o == "%")) {
            expr = matrix_columnwise(o, exp_type, a, b);
        } else if (exp_type.is_matrix() && (o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=")) {
            _unsupported.push_back("component-wise matrix comparison");
            expr = "false";
        } else {
            if ((o == "<<" || o == ">>") && !exp_type.is_unsigned()) {
                b = exp_type.is_vector() ? "vec" + std::to_string(exp_type.rows) + "<u32>(" + b + ')'
                                         : "u32(" + b + ')';
            }
            expr = '(' + a + ' ' + o + ' ' + b + ')';
        }
        code += let_prefix(res, res_type) + expr + ";\n";
        return res;
    }

    id emit_ternary_op(const location&, tokenid op, const type& res_type, id condition, id true_value, id false_value) override {
        assert(op == tokenid::question);
        (void)op;
        const id res = make_id();
        std::string& code = _blocks.at(_current_block);
        type rt = res_type;
        rt.qualifiers = 0;
        if (rt.is_array() || rt.is_struct() || rt.is_matrix()) {
            code += "\tvar " + id_to_name(res) + " : " + type_name(rt) + ";\n";
            code += "\tif (" + id_to_name(condition) + ") { " + id_to_name(res) + " = " +
                    id_to_name(true_value) + "; } else { " + id_to_name(res) + " = " +
                    id_to_name(false_value) + "; }\n";
            return res;
        }
        code += let_prefix(res, rt) + "select(" + id_to_name(false_value) + ", " +
                id_to_name(true_value) + ", " + id_to_name(condition) + ");\n";
        return res;
    }

    id emit_call(const location&, id function, const type& res_type, const std::vector<expression>& args) override {
        const id res = make_id();
        std::string& code = _blocks.at(_current_block);
        std::string tokens;
        std::string params;
        for (const expression& arg : args) {
            if (arg.type.is_object()) {
                if (!tokens.empty()) {
                    tokens += ',';
                }
                tokens += object_token(arg.base);
                continue;
            }
            if (!params.empty()) {
                params += ", ";
            }
            params += (arg.type.has(type::q_out) ? "&" : "") + id_to_name(arg.base);
        }
        code += '\t';
        if (!res_type.is_void()) {
            type rt = res_type;
            rt.qualifiers = 0;
            code += "let " + id_to_name(res) + " : " + type_name(rt) + " = ";
        }
        code += std::string(1, kCallBegin) + std::to_string(function) + '|' + tokens + kCallEnd + '(' + params + ");\n";
        return res;
    }

    id emit_call_intrinsic(const location&, id intrinsic, const type& res_type, const std::vector<expression>& args) override;

    id emit_construct(const location&, const type& res_type, const std::vector<expression>& args) override {
        const id res = make_id();
        std::string& code = _blocks.at(_current_block);
        type rt = res_type;
        rt.qualifiers = 0;
        std::string expr = type_name(rt) + '(';
        for (const expression& arg : args) {
            std::string a = id_to_name(arg.base);
            if (rt.is_matrix() && !arg.type.is_floating_point()) {
                a = "f32(" + a + ')';
            }
            expr += a + ", ";
        }
        if (!args.empty()) {
            expr.erase(expr.size() - 2);
        }
        expr += ')';
        code += let_prefix(res, rt) + expr + ";\n";
        return res;
    }

    static void increase_indentation_level(std::string& block) {
        if (block.empty()) {
            return;
        }
        for (size_t pos = 0; (pos = block.find("\n\t", pos)) != std::string::npos; pos += 3) {
            block.replace(pos, 2, "\n\t\t");
        }
        block.insert(block.begin(), '\t');
    }

    void emit_if(const location&, id condition_value, id condition_block, id true_statement_block, id false_statement_block, unsigned int) override {
        std::string& code = _blocks.at(_current_block);
        std::string& t = _blocks.at(true_statement_block);
        std::string& f = _blocks.at(false_statement_block);
        increase_indentation_level(t);
        increase_indentation_level(f);
        code += _blocks.at(condition_block);
        code += "\tif (" + id_to_name(condition_value) + ") {\n" + t + "\t}";
        if (!f.empty()) {
            code += " else {\n" + f + "\t}";
        }
        code += '\n';
        _blocks.erase(condition_block);
        _blocks.erase(true_statement_block);
        _blocks.erase(false_statement_block);
    }

    id emit_phi(const location&, id condition_value, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type& res_type) override {
        std::string& code = _blocks.at(_current_block);
        std::string& t = _blocks.at(true_statement_block);
        std::string& f = _blocks.at(false_statement_block);
        increase_indentation_level(t);
        increase_indentation_level(f);
        const id res = make_id();
        code += _blocks.at(condition_block);
        type rt = res_type;
        rt.qualifiers = 0;
        code += "\tvar " + id_to_name(res) + " : " + type_name(rt) + ";\n";
        code += "\tif (" + id_to_name(condition_value) + ") {\n";
        code += (true_statement_block != condition_block ? t : std::string());
        code += "\t\t" + id_to_name(res) + " = " + id_to_name(true_value) + ";\n";
        code += "\t} else {\n";
        code += (false_statement_block != condition_block ? f : std::string());
        code += "\t\t" + id_to_name(res) + " = " + id_to_name(false_value) + ";\n";
        code += "\t}\n";
        _blocks.erase(condition_block);
        _blocks.erase(true_statement_block);
        _blocks.erase(false_statement_block);
        return res;
    }

    void emit_loop(const location&, id condition_value, id prev_block, id header_block, id condition_block, id loop_block, id continue_block, unsigned int) override {
        std::string& code = _blocks.at(_current_block);
        std::string& loopData = _blocks.at(loop_block);
        std::string& continueData = _blocks.at(continue_block);
        increase_indentation_level(loopData);
        increase_indentation_level(loopData);
        increase_indentation_level(continueData);
        increase_indentation_level(continueData);
        code += _blocks.at(prev_block);
        const std::string cond = condition_value != 0 ? id_to_name(condition_value) : "true";
        if (condition_block == 0) {
            // do { body } while (cond): the condition is computed in the continue block.
            code += "\tloop {\n\t\t{\n" + loopData + "\t\t}\n\t\tcontinuing {\n" + continueData +
                    "\t\t\tbreak if !(" + cond + ");\n\t\t}\n\t}\n";
        } else {
            std::string& conditionData = _blocks.at(condition_block);
            increase_indentation_level(conditionData);
            code += "\tloop {\n" + conditionData + "\t\tif (!(" + cond + ")) { break; }\n\t\t{\n" +
                    loopData + "\t\t}\n\t\tcontinuing {\n" + continueData + "\t\t}\n\t}\n";
            _blocks.erase(condition_block);
        }
        _blocks.erase(prev_block);
        _blocks.erase(header_block);
        _blocks.erase(loop_block);
        _blocks.erase(continue_block);
    }

    void emit_switch(const location&, id selector_value, id selector_block, id default_label, id default_block, const std::vector<id>& case_literal_and_labels, const std::vector<id>& case_blocks, unsigned int) override {
        std::string& code = _blocks.at(_current_block);
        code += _blocks.at(selector_block);
        code += "\tswitch (" + id_to_name(selector_value) + ") {\n";
        std::vector<id> labels = case_literal_and_labels;
        bool defaultWritten = false;
        for (size_t i = 0; i < labels.size(); i += 2) {
            if (labels[i + 1] == 0) {
                continue;
            }
            code += "\tcase " + std::to_string(static_cast<int32_t>(labels[i]));
            for (size_t k = i + 2; k < labels.size(); k += 2) {
                if (labels[k + 1] == 0 || labels[k + 1] != labels[i + 1]) {
                    continue;
                }
                code += ", " + std::to_string(static_cast<int32_t>(labels[k]));
                labels[k + 1] = 0;
            }
            if (labels[i + 1] == default_label) {
                code += ", default";
                defaultWritten = true;
            }
            std::string& caseData = _blocks.at(case_blocks[i / 2]);
            increase_indentation_level(caseData);
            code += ": {\n" + caseData + "\t}\n";
        }
        if (!defaultWritten) {
            if (default_block != _current_block) {
                std::string& defaultData = _blocks.at(default_block);
                increase_indentation_level(defaultData);
                code += "\tdefault: {\n" + defaultData + "\t}\n";
                _blocks.erase(default_block);
            } else {
                code += "\tdefault: {}\n";
            }
        }
        code += "\t}\n";
        _blocks.erase(selector_block);
        for (const id caseBlock : case_blocks) {
            _blocks.erase(caseBlock);
        }
    }

    void emit_pragma(const std::string&) override {}

    id create_block() override {
        const id res = make_id();
        _blocks.emplace(res, std::string()).first->second.reserve(1024);
        return res;
    }
    id set_block(id v) override {
        _last_block = _current_block;
        _current_block = v;
        return _last_block;
    }
    void enter_block(id v) override { _current_block = v; }

    id leave_block_and_kill() override {
        if (!is_in_block()) {
            return 0;
        }
        std::string& code = _blocks.at(_current_block);
        code += std::string("\t") + kStage + "D" + kStage + ";\n";
        if (!_current_function->return_type.is_void()) {
            type rt = _current_function->return_type;
            rt.qualifiers = 0;
            code += "\treturn " + type_name(rt) + "();\n";
        } else {
            code += "\treturn;\n";
        }
        return set_block(0);
    }
    id leave_block_and_return(id value) override {
        if (!is_in_block()) {
            return 0;
        }
        if (!_current_function->return_type.is_void() && value == 0) {
            return set_block(0);
        }
        std::string& code = _blocks.at(_current_block);
        code += "\treturn";
        if (value != 0) {
            code += ' ' + id_to_name(value);
        }
        code += ";\n";
        return set_block(0);
    }
    id leave_block_and_switch(id, id) override {
        if (!is_in_block()) {
            return _last_block;
        }
        return set_block(0);
    }
    id leave_block_and_branch(id, unsigned int loop_flow) override {
        if (!is_in_block()) {
            return _last_block;
        }
        std::string& code = _blocks.at(_current_block);
        if (loop_flow == 1) {
            code += "\tbreak;\n";
        } else if (loop_flow == 2) {
            code += "\tcontinue;\n";
        }
        return set_block(0);
    }
    id leave_block_and_branch_conditional(id, id, id) override {
        if (!is_in_block()) {
            return _last_block;
        }
        return set_block(0);
    }
    void leave_function() override {
        assert(_current_function != nullptr && _last_block != 0);
        const FuncExtra& extra = _func_extra.at(_current_function->id);
        std::string text = _current_function_declaration + "{\n" + extra.prologue + _blocks.at(_last_block);
        if (extra.returns_value) {
            // WGSL rejects a value-returning function whose end is reachable by its analysis, even
            // where HLSL is satisfied (a loop whose exits all return). This return is never executed
            // in those cases.
            text += "\treturn " + extra.return_type + "();\n";
        }
        text += "}\n";
        _blocks.emplace(_current_function->id, std::move(text));
        _current_function = nullptr;
        _current_function_declaration.clear();
    }

    // ---------------------------------------------------------------------------------------------
    // Assembly helpers

    struct SamplerSpec {
        uint32_t index = 0;
        const sampler* smp = nullptr;
        const texture* tex = nullptr;
        TexturePhysical phys;
        unsigned dims = 2; // 1D textures are created as 2D with height 1
        bool tex1d = false;
        bool emulate = false;
        bool unfilterable = false;
        uint32_t levels = 1;
    };

    bool sampler_spec(uint32_t index, SamplerSpec& spec, std::string& errors) const {
        if (index >= _module.samplers.size()) {
            errors += "sampler index out of range\n";
            return false;
        }
        spec.index = index;
        spec.smp = &_module.samplers[index];
        for (const texture& t : _module.textures) {
            if (t.unique_name == spec.smp->texture_name) {
                spec.tex = &t;
                break;
            }
        }
        if (spec.tex == nullptr) {
            errors += "sampler '" + spec.smp->name + "' references an unknown texture\n";
            return false;
        }
        spec.phys = _host.physical(*spec.tex);
        spec.tex1d = spec.tex->semantic.empty() && spec.tex->type == texture_type::texture_1d;
        spec.dims = spec.tex->semantic.empty() && spec.tex->type == texture_type::texture_3d ? 3 : 2;
        spec.levels = spec.tex->semantic.empty() ? std::max<uint32_t>(spec.tex->levels, 1u) : 1u;
        spec.unfilterable = binding_is_unfilterable(spec.phys);
        const bool linear = spec.smp->filter != filter_mode::min_mag_mip_point;
        const bool border = spec.smp->address_u == texture_address_mode::border ||
                            (spec.dims >= 2 && !spec.tex1d && spec.smp->address_v == texture_address_mode::border) ||
                            (spec.dims == 3 && spec.smp->address_w == texture_address_mode::border);
        spec.emulate = spec.phys.sample_type == SampleType::Float &&
                       ((!spec.phys.filterable && linear) || border || spec.smp->lod_bias != 0.0f);
        return true;
    }

    static std::string fixup(const TexturePhysical& p, const std::string& v) {
        if (p.physical_components <= p.logical_components) {
            return v;
        }
        const char* s = sample_scalar(p.sample_type);
        const std::string zero = p.sample_type == SampleType::Float ? "0.0" : (p.sample_type == SampleType::Sint ? "0i" : "0u");
        const std::string one = p.sample_type == SampleType::Float ? "1.0" : (p.sample_type == SampleType::Sint ? "1i" : "1u");
        switch (p.logical_components) {
        case 1: return std::string("vec4<") + s + ">((" + v + ").x, " + zero + ", " + zero + ", " + one + ')';
        case 2: return std::string("vec4<") + s + ">((" + v + ").xy, " + zero + ", " + one + ')';
        case 3: return std::string("vec4<") + s + ">((" + v + ").xyz, " + one + ')';
        default: return v;
        }
    }

    static const char* addr_code(texture_address_mode m) {
        switch (m) {
        case texture_address_mode::wrap: return "1u";
        case texture_address_mode::mirror: return "2u";
        case texture_address_mode::border: return "4u";
        default: return "3u";
        }
    }

    // Generates the helpers for operation `op` on sampler `spec` into `out`; `have` de-duplicates
    // the shared building blocks.
    void sampler_helper(const SamplerSpec& sp, const std::string& op, shader_type stype,
        std::string& out, std::set<std::string>& have, bool& needAddr) const {
        const std::string sfx = "_s" + std::to_string(sp.index);
        const std::string T = "_rsp_T" + sfx;
        const std::string S = "_rsp_S" + sfx;
        const char* scalar = sample_scalar(sp.phys.sample_type);
        const std::string v4 = std::string("vec4<") + scalar + '>';
        const bool fragment = stype == shader_type::pixel;
        const bool three = sp.dims == 3;
        const std::string fv = three ? "vec3<f32>" : "vec2<f32>";
        const std::string iv = three ? "vec3<i32>" : "vec2<i32>";
        const std::string uvIn = sp.tex1d ? "u : f32" : (three ? "uv : vec3<f32>" : "uv : vec2<f32>");
        const std::string uv = sp.tex1d ? "vec2<f32>(u, 0.5)" : "uv";
        const std::string offIn = sp.tex1d ? "o : i32" : "o : " + iv;
        const std::string off = sp.tex1d ? "vec2<i32>(o, 0)" : "o";
        const std::string zeroOff = iv + "(0)";

        const auto emit_once = [&](const std::string& key, const std::string& code) {
            if (have.insert(key).second) {
                out += code;
            }
        };

        // Texel load with the sampler's address mode; zero outside a border sampler.
        const auto need_load = [&]() {
            needAddr = true;
            std::string c = "fn _rsp_ld" + sfx + "(c : " + iv + ", lv : u32) -> " + v4 + " {\n";
            c += "\tlet n = " + iv + "(textureDimensions(" + T + ", lv));\n";
            c += "\tlet x = _rsp_addr(c.x, n.x, " + std::string(addr_code(sp.smp->address_u)) + ");\n";
            c += "\tlet y = _rsp_addr(c.y, n.y, " + std::string(sp.tex1d ? "3u" : addr_code(sp.smp->address_v)) + ");\n";
            if (three) {
                c += "\tlet z = _rsp_addr(c.z, n.z, " + std::string(addr_code(sp.smp->address_w)) + ");\n";
                c += "\tif (x < 0 || y < 0 || z < 0) { return " + v4 + "(); }\n";
                c += "\treturn " + fixup(sp.phys, "textureLoad(" + T + ", vec3<i32>(x, y, z), lv)") + ";\n}\n";
            } else {
                c += "\tif (x < 0 || y < 0) { return " + v4 + "(); }\n";
                c += "\treturn " + fixup(sp.phys, "textureLoad(" + T + ", vec2<i32>(x, y), lv)") + ";\n}\n";
            }
            emit_once("ld" + sfx, c);
        };

        // Filtering in the shader: bilinear (or point) at one level, then level selection.
        const auto need_level = [&]() {
            need_load();
            std::string c = "fn _rsp_bl" + sfx + "(uv : " + fv + ", lv : u32, o : " + iv + ", lin : bool) -> vec4<f32> {\n";
            c += "\tlet n = " + fv + "(textureDimensions(" + T + ", lv));\n";
            c += "\tif (!lin) { return _rsp_ld" + sfx + "(" + iv + "(floor(uv * n)) + o, lv); }\n";
            c += "\tlet p = uv * n - 0.5;\n\tlet b = floor(p);\n\tlet f = p - b;\n\tlet i = " + iv + "(b) + o;\n";
            if (three) {
                c += "\tlet x0 = mix(mix(_rsp_ld" + sfx + "(i, lv), _rsp_ld" + sfx + "(i + vec3<i32>(1, 0, 0), lv), f.x), "
                     "mix(_rsp_ld" + sfx + "(i + vec3<i32>(0, 1, 0), lv), _rsp_ld" + sfx + "(i + vec3<i32>(1, 1, 0), lv), f.x), f.y);\n";
                c += "\tlet x1 = mix(mix(_rsp_ld" + sfx + "(i + vec3<i32>(0, 0, 1), lv), _rsp_ld" + sfx + "(i + vec3<i32>(1, 0, 1), lv), f.x), "
                     "mix(_rsp_ld" + sfx + "(i + vec3<i32>(0, 1, 1), lv), _rsp_ld" + sfx + "(i + vec3<i32>(1, 1, 1), lv), f.x), f.y);\n";
                c += "\treturn mix(x0, x1, f.z);\n}\n";
            } else {
                c += "\treturn mix(mix(_rsp_ld" + sfx + "(i, lv), _rsp_ld" + sfx + "(i + vec2<i32>(1, 0), lv), f.x), "
                     "mix(_rsp_ld" + sfx + "(i + vec2<i32>(0, 1), lv), _rsp_ld" + sfx + "(i + vec2<i32>(1, 1), lv), f.x), f.y);\n}\n";
            }
            emit_once("bl" + sfx, c);

            const auto f = static_cast<uint32_t>(sp.smp->filter);
            const bool minLin = (f & 0x10) != 0;
            const bool magLin = (f & 0x04) != 0;
            const bool mipLin = (f & 0x01) != 0;
            const float minLod = std::isfinite(sp.smp->min_lod) ? std::max(sp.smp->min_lod, 0.0f) : 0.0f;
            const float maxLod = std::isfinite(sp.smp->max_lod) ? std::min(std::max(sp.smp->max_lod, minLod), 1000.0f) : 1000.0f;
            std::string l = "fn _rsp_lv" + sfx + "(uv : " + fv + ", lod : f32, o : " + iv + ") -> vec4<f32> {\n";
            l += "\tlet nl = textureNumLevels(" + T + ");\n";
            l += "\tlet l = lod + " + format_float(sp.smp->lod_bias) + ";\n";
            l += std::string("\tlet lin = select(") + (minLin ? "true" : "false") + ", " + (magLin ? "true" : "false") + ", l <= 0.0);\n";
            l += "\tlet lc = clamp(l, " + format_float(minLod) + ", min(" + format_float(maxLod) + ", f32(nl - 1u)));\n";
            if (mipLin) {
                l += "\tlet l0 = floor(lc);\n\tlet fr = lc - l0;\n\tlet i0 = u32(l0);\n";
                l += "\tlet a = _rsp_bl" + sfx + "(uv, i0, o, lin);\n";
                l += "\tif (fr <= 0.0 || i0 + 1u >= nl) { return a; }\n";
                l += "\treturn mix(a, _rsp_bl" + sfx + "(uv, i0 + 1u, o, lin), fr);\n}\n";
            } else {
                l += "\treturn _rsp_bl" + sfx + "(uv, u32(floor(lc + 0.5)), o, lin);\n}\n";
            }
            emit_once("lv" + sfx, l);
        };

        const auto implicit_lod = [&](const std::string& c) {
            // Same LOD formula as hardware: log2 of the larger screen-space texel footprint.
            if (!fragment) {
                return std::string("0.0");
            }
            emit_once("lodfn" + sfx,
                "fn _rsp_lod" + sfx + "(c : " + fv + ") -> f32 {\n"
                "\tlet n = " + fv + "(textureDimensions(" + T + ", 0u));\n"
                "\tlet dx = dpdx(c * n);\n\tlet dy = dpdy(c * n);\n"
                "\treturn 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));\n}\n");
            return "_rsp_lod" + sfx + '(' + c + ')';
        };

        const std::string name = "_rsp_" + op + sfx;
        std::string c;
        const bool integer = sp.phys.sample_type != SampleType::Float;

        if (op == "f" || op == "fl") {
            // tex*Dfetch: out-of-range coordinates or levels read zero, as D3D defines it.
            const std::string cIn = sp.tex1d ? "ci : i32" : "c : " + iv;
            const std::string cc = sp.tex1d ? "vec2<i32>(ci, 0)" : "c";
            c = "fn " + name + '(' + cIn + (op == "fl" ? ", lod : i32" : "") + ") -> " + v4 + " {\n";
            c += std::string("\tlet lv = ") + (op == "fl" ? "lod" : "0") + ";\n";
            c += "\tif (lv < 0 || lv >= i32(textureNumLevels(" + T + "))) { return " + v4 + "(); }\n";
            c += "\tlet n = " + iv + "(textureDimensions(" + T + ", lv));\n";
            c += "\tif (any(" + cc + " < " + iv + "(0)) || any(" + cc + " >= n)) { return " + v4 + "(); }\n";
            c += "\treturn " + fixup(sp.phys, "textureLoad(" + T + ", " + cc + ", lv)") + ";\n}\n";
        } else if (op == "z" || op == "zl") {
            const std::string ret = sp.tex1d ? "i32" : iv;
            c = "fn " + name + '(' + (op == "zl" ? "lod : i32" : "") + ") -> " + ret + " {\n";
            const std::string d = "textureDimensions(" + T + (op == "zl" ? ", lod" : "") + ')';
            c += "\treturn " + (sp.tex1d ? "i32(" + d + ".x)" : iv + '(' + d + ')') + ";\n}\n";
        } else if (integer) {
            // Integer textures cannot be sampled; ReShade's HLSL reads the texel under the
            // coordinate (truncated, out of range reads zero).
            const bool hasLod = op == "l" || op == "lo";
            const bool hasOff = op == "to" || op == "lo" || op == "go";
            std::string params = uvIn;
            if (op == "l" || op == "lo") {
                params += ", lod : f32";
            }
            if (op == "g" || op == "go") {
                params += sp.tex1d ? ", dx : f32, dy : f32" : ", dx : " + fv + ", dy : " + fv;
            }
            if (hasOff) {
                params += ", " + offIn;
            }
            c = "fn " + name + '(' + params + ") -> " + v4 + " {\n";
            c += std::string("\tlet lv = ") + (hasLod ? "i32(lod)" : "0") + ";\n";
            c += "\tif (lv < 0 || lv >= i32(textureNumLevels(" + T + "))) { return " + v4 + "(); }\n";
            c += "\tlet n = " + iv + "(textureDimensions(" + T + ", lv));\n";
            c += "\tlet p = " + iv + '(' + uv + " * " + fv + "(n))" + (hasOff ? " + " + off : "") + ";\n";
            c += "\tif (any(p < " + iv + "(0)) || any(p >= n)) { return " + v4 + "(); }\n";
            c += "\treturn " + fixup(sp.phys, "textureLoad(" + T + ", p, lv)") + ";\n}\n";
        } else if (op.rfind("ga", 0) == 0) {
            // Gathers. Components the format does not have read as 0 (alpha as 1).
            const bool four = op.rfind("gaf", 0) == 0;
            const bool withOff = op.rfind("gao", 0) == 0;
            const int comp = op.back() - '0';
            std::string params = "uv : vec2<f32>";
            if (withOff) {
                params += ", o : vec2<i32>";
            }
            if (four) {
                params += ", o0 : vec2<i32>, o1 : vec2<i32>, o2 : vec2<i32>, o3 : vec2<i32>";
            }
            c = "fn " + name + '(' + params + ") -> vec4<f32> {\n";
            if (comp >= sp.phys.logical_components) {
                c += std::string("\treturn vec4<f32>(") + (comp == 3 ? "1.0" : "0.0") + ");\n}\n";
            } else if (four || sp.emulate) {
                need_load();
                const std::string cc = std::string(".") + "xyzw"[comp];
                c += "\tlet n = vec2<f32>(textureDimensions(" + T + ", 0u));\n";
                c += "\tlet b = vec2<i32>(floor(uv * n - 0.5))" + std::string(withOff ? " + o" : "") + ";\n";
                if (four) {
                    // D3D12 semantics (ReShade's SM5 path): texel i is base + offset i.
                    c += "\treturn vec4<f32>(_rsp_ld" + sfx + "(b + o0, 0u)" + cc + ", _rsp_ld" + sfx + "(b + o1, 0u)" + cc +
                         ", _rsp_ld" + sfx + "(b + o2, 0u)" + cc + ", _rsp_ld" + sfx + "(b + o3, 0u)" + cc + ");\n}\n";
                } else {
                    c += "\treturn vec4<f32>(_rsp_ld" + sfx + "(b + vec2<i32>(0, 1), 0u)" + cc + ", _rsp_ld" + sfx + "(b + vec2<i32>(1, 1), 0u)" + cc +
                         ", _rsp_ld" + sfx + "(b + vec2<i32>(1, 0), 0u)" + cc + ", _rsp_ld" + sfx + "(b, 0u)" + cc + ");\n}\n";
                }
            } else {
                const std::string coord = withOff ? "uv + vec2<f32>(o) / vec2<f32>(textureDimensions(" + T + "))" : "uv";
                c += "\treturn textureGather(" + std::to_string(comp) + ", " + T + ", " + S + ", " + coord + ");\n}\n";
            }
        } else {
            // Sampling: t (implicit LOD), l (explicit LOD), g (gradients); with an 'o' suffix the
            // variant takes a texel offset.
            const bool hasOff = op == "to" || op == "lo" || op == "go";
            std::string params = uvIn;
            if (op == "l" || op == "lo") {
                params += ", lod : f32";
            }
            if (op == "g" || op == "go") {
                params += sp.tex1d ? ", dx : f32, dy : f32" : ", dx : " + fv + ", dy : " + fv;
            }
            if (hasOff) {
                params += ", " + offIn;
            }
            c = "fn " + name + '(' + params + ") -> vec4<f32> {\n";
            const std::string dxv = sp.tex1d ? "vec2<f32>(dx, 0.0)" : "dx";
            const std::string dyv = sp.tex1d ? "vec2<f32>(dy, 0.0)" : "dy";
            // A texel offset at level 0 is an exact coordinate shift; with mips it applies at the
            // selected level, which only the emulated path reproduces.
            const bool nativeOffset = sp.levels <= 1;
            if (!sp.emulate && (!hasOff || nativeOffset)) {
                const std::string coord = hasOff ? '(' + uv + " + " + fv + '(' + off + ") / " + fv + "(textureDimensions(" + T + ")))" : uv;
                std::string sample;
                if (op == "t" || op == "to") {
                    sample = fragment ? "textureSample(" + T + ", " + S + ", " + coord + ')'
                                      : "textureSampleLevel(" + T + ", " + S + ", " + coord + ", 0.0)";
                } else if (op == "l" || op == "lo") {
                    sample = "textureSampleLevel(" + T + ", " + S + ", " + coord + ", lod)";
                } else {
                    sample = "textureSampleGrad(" + T + ", " + S + ", " + coord + ", " + dxv + ", " + dyv + ')';
                }
                c += "\treturn " + fixup(sp.phys, sample) + ";\n}\n";
            } else {
                need_level();
                const std::string o = hasOff ? off : zeroOff;
                std::string lod;
                if (op == "t" || op == "to") {
                    lod = implicit_lod(uv);
                } else if (op == "l" || op == "lo") {
                    lod = "lod";
                } else {
                    c += "\tlet n = " + fv + "(textureDimensions(" + T + ", 0u));\n";
                    c += "\tlet gx = " + dxv + " * n;\n\tlet gy = " + dyv + " * n;\n";
                    lod = "0.5 * log2(max(dot(gx, gx), dot(gy, gy)))";
                }
                c += "\treturn _rsp_lv" + sfx + '(' + uv + ", " + lod + ", " + o + ");\n}\n";
            }
        }
        emit_once(name, c);
    }

    void storage_helper(uint32_t index, const TexturePhysical& phys, bool tex1d, bool three,
        const std::string& op, std::string& out, std::set<std::string>& have) const {
        const std::string sfx = "_u" + std::to_string(index);
        const std::string T = "_rsp_ST" + sfx;
        const char* scalar = sample_scalar(phys.sample_type);
        const std::string v4 = std::string("vec4<") + scalar + '>';
        const std::string iv = three ? "vec3<i32>" : "vec2<i32>";
        const std::string cIn = tex1d ? "ci : i32" : "c : " + iv;
        const std::string cc = tex1d ? "vec2<i32>(ci, 0)" : "c";
        const std::string name = "_rsp_" + op + sfx;
        if (!have.insert(name).second) {
            return;
        }
        std::string c;
        if (op == "sl") {
            c = "fn " + name + '(' + cIn + ") -> " + v4 + " {\n";
            c += "\tlet n = " + iv + "(textureDimensions(" + T + "));\n";
            c += "\tif (any(" + cc + " < " + iv + "(0)) || any(" + cc + " >= n)) { return " + v4 + "(); }\n";
            c += "\treturn " + fixup(phys, "textureLoad(" + T + ", " + cc + ')') + ";\n}\n";
        } else if (op == "ss") {
            c = "fn " + name + '(' + cIn + ", v : " + v4 + ") {\n";
            c += "\tlet n = " + iv + "(textureDimensions(" + T + "));\n";
            std::string value = "v";
            if (phys.store_quantize_bits == 10) {
                value = "round(clamp(v, vec4<f32>(0.0), vec4<f32>(1.0)) * vec4<f32>(1023.0, 1023.0, 1023.0, 3.0)) / vec4<f32>(1023.0, 1023.0, 1023.0, 3.0)";
            }
            c += "\tif (all(" + cc + " >= " + iv + "(0)) && all(" + cc + " < n)) { textureStore(" + T + ", " + cc + ", " + value + "); }\n}\n";
        } else {
            const std::string ret = tex1d ? "i32" : iv;
            c = "fn " + name + "() -> " + ret + " {\n";
            c += "\treturn " + (tex1d ? "i32(textureDimensions(" + T + ").x)" : iv + "(textureDimensions(" + T + "))") + ";\n}\n";
        }
        out += c;
    }
};

// -------------------------------------------------------------------------------------------------
// Intrinsics

codegen::id codegen_wgsl::emit_call_intrinsic(const location&, id intrinsic, const type& res_type, const std::vector<expression>& args) {
    const id res = make_id();
    std::string& code = _blocks.at(_current_block);
    const auto a = [&](size_t i) { return id_to_name(args[i].base); };
    type rt = res_type;
    rt.qualifiers = 0;
    const std::string rtName = type_name(rt);
    const bool rv = !rt.is_void();
    std::string e; // expression for the result (or statement when void)

    // Values returned by the per-sampler helpers are 4-component; collapse for scalar results.
    const auto tex = [&](const char* op, std::initializer_list<std::string> params) {
        std::string s = helper_call(op, args[0].base);
        bool first = true;
        for (const std::string& p : params) {
            s += (first ? "" : ", ") + p;
            first = false;
        }
        s += ')';
        if (rt.rows == 1 && !rt.is_void()) {
            s += ".x";
        }
        return s;
    };
    const auto vecu = [&](unsigned rows) { return rows > 1 ? "vec" + std::to_string(rows) + "<u32>" : std::string("u32"); };
    const auto vecn = [&](const type& t, const char* scalar) {
        return t.rows > 1 ? "vec" + std::to_string(t.rows) + '<' + scalar + '>' : std::string(scalar);
    };
    const auto deriv = [&](const char* fn) { return std::string(1, kStage) + 'G' + fn + kStage + '(' + a(0) + ')'; };
    const auto unsupported = [&](const char* what) {
        _unsupported.push_back(what);
        e = rv ? rtName + "()" : std::string();
    };
    const auto storage_value = [&](size_t i) {
        // Storage writes take a 4-component value of the storage's component type.
        const type& t = args[i].type;
        const char* scalar = t.is_integral() ? scalar_name(t.base) : "f32";
        if (t.rows >= 4) {
            return a(i);
        }
        const std::string zero = t.is_integral() ? (t.is_signed() ? "0i" : "0u") : "0.0";
        std::string s = std::string("vec4<") + scalar + ">(" + a(i);
        for (unsigned k = t.rows; k < 4; ++k) {
            s += ", " + zero;
        }
        return s + ')';
    };

    enum {
#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) name##i,
#include "effect_symbol_table_intrinsics.inl"
    };

    switch (intrinsic) {
    case abs0: case abs1: e = "abs(" + a(0) + ')'; break;
    case all0: case any0: e = a(0); break;
    case all1: e = "all(" + a(0) + ')'; break;
    case any1: e = "any(" + a(0) + ')'; break;
    case asin0: e = "asin(" + a(0) + ')'; break;
    case acos0: e = "acos(" + a(0) + ')'; break;
    case atan0: e = "atan(" + a(0) + ')'; break;
    case atan20: e = "atan2(" + a(0) + ", " + a(1) + ')'; break;
    case sin0: e = "sin(" + a(0) + ')'; break;
    case sinh0: e = "sinh(" + a(0) + ')'; break;
    case cos0: e = "cos(" + a(0) + ')'; break;
    case cosh0: e = "cosh(" + a(0) + ')'; break;
    case tan0: e = "tan(" + a(0) + ')'; break;
    case tanh0: e = "tanh(" + a(0) + ')'; break;
    case sincos0:
        code += '\t' + a(1) + " = sin(" + a(0) + ");\n\t" + a(2) + " = cos(" + a(0) + ");\n";
        return res;
    case asint0: e = "bitcast<" + vecn(args[0].type, "i32") + ">(" + a(0) + ')'; break;
    case asuint0: e = "bitcast<" + vecn(args[0].type, "u32") + ">(" + a(0) + ')'; break;
    case asfloat0: case asfloat1: e = "bitcast<" + vecn(args[0].type, "f32") + ">(" + a(0) + ')'; break;
    case f16tof320:
        if (args[0].type.rows > 1) {
            e = "vec" + std::to_string(args[0].type.rows) + "<f32>(";
            for (unsigned i = 0; i < args[0].type.rows; ++i) {
                e += std::string("unpack2x16float(") + a(0) + '.' + "xyzw"[i] + ").x" + (i + 1 < args[0].type.rows ? ", " : "");
            }
            e += ')';
        } else {
            e = "unpack2x16float(" + a(0) + ").x";
        }
        break;
    case f32tof160:
        if (args[0].type.rows > 1) {
            e = "vec" + std::to_string(args[0].type.rows) + "<u32>(";
            for (unsigned i = 0; i < args[0].type.rows; ++i) {
                e += std::string("pack2x16float(vec2<f32>(") + a(0) + '.' + "xyzw"[i] + ", 0.0))" + (i + 1 < args[0].type.rows ? ", " : "");
            }
            e += ')';
        } else {
            e = "pack2x16float(vec2<f32>(" + a(0) + ", 0.0))";
        }
        break;
    case firstbitlow0: e = "firstTrailingBit(" + a(0) + ')'; break;
    case firstbithigh0: case firstbithigh1: e = "firstLeadingBit(" + a(0) + ')'; break;
    case countbits0: e = "countOneBits(" + a(0) + ')'; break;
    case reversebits0: e = "reverseBits(" + a(0) + ')'; break;
    case ceil0: e = "ceil(" + a(0) + ')'; break;
    case floor0: e = "floor(" + a(0) + ')'; break;
    case clamp0: case clamp1: case clamp2: e = "clamp(" + a(0) + ", " + a(1) + ", " + a(2) + ')'; break;
    case saturate0: e = "saturate(" + a(0) + ')'; break;
    case mad0: e = "fma(" + a(0) + ", " + a(1) + ", " + a(2) + ')'; break;
    case rcp0: e = "(1.0 / " + a(0) + ')'; break;
    case pow0: e = "pow(" + a(0) + ", " + a(1) + ')'; break;
    case exp0: e = "exp(" + a(0) + ')'; break;
    case exp20: e = "exp2(" + a(0) + ')'; break;
    case log0: e = "log(" + a(0) + ')'; break;
    case log20: e = "log2(" + a(0) + ')'; break;
    case log100: e = "(log2(" + a(0) + ") / log2(10.0))"; break;
    case sign0: case sign1: e = "sign(" + a(0) + ')'; break;
    case sqrt0: e = "sqrt(" + a(0) + ')'; break;
    case rsqrt0: e = "inverseSqrt(" + a(0) + ')'; break;
    case lerp0: e = "mix(" + a(0) + ", " + a(1) + ", " + a(2) + ')'; break;
    case step0: e = "step(" + a(0) + ", " + a(1) + ')'; break;
    case smoothstep0: {
        // HLSL's formula, which also defines min > max (WGSL leaves that to the implementation).
        const std::string t = "saturate((" + a(2) + " - " + a(0) + ") / (" + a(1) + " - " + a(0) + "))";
        code += "\tlet " + id_to_name(res) + "_t : " + rtName + " = " + t + ";\n";
        e = '(' + id_to_name(res) + "_t * " + id_to_name(res) + "_t * (3.0 - 2.0 * " + id_to_name(res) + "_t))";
        break;
    }
    case frac0: e = "fract(" + a(0) + ')'; break;
    case ldexp0: e = "ldexp(" + a(0) + ", " + a(1) + ')'; break;
    case modf0:
        code += "\tlet " + id_to_name(res) + "_m = modf(" + a(0) + ");\n\t" + a(1) + " = " + id_to_name(res) + "_m.whole;\n";
        e = id_to_name(res) + "_m.fract";
        break;
    case frexp0:
        code += "\tlet " + id_to_name(res) + "_m = frexp(" + a(0) + ");\n\t" + a(1) + " = " + id_to_name(res) + "_m.exp;\n";
        e = id_to_name(res) + "_m.fract";
        break;
    case trunc0: e = "trunc(" + a(0) + ')'; break;
    case round0: e = "round(" + a(0) + ')'; break;
    case min0: case min1: e = "min(" + a(0) + ", " + a(1) + ')'; break;
    case max0: case max1: e = "max(" + a(0) + ", " + a(1) + ')'; break;
    case degrees0: e = "degrees(" + a(0) + ')'; break;
    case radians0: e = "radians(" + a(0) + ')'; break;
    case ddx0: e = deriv("dpdx"); break;
    case ddx_coarse0: e = deriv("dpdxCoarse"); break;
    case ddx_fine0: e = deriv("dpdxFine"); break;
    case ddy0: e = deriv("dpdy"); break;
    case ddy_coarse0: e = deriv("dpdyCoarse"); break;
    case ddy_fine0: e = deriv("dpdyFine"); break;
    case fwidth0: e = deriv("fwidth"); break;
    case dot0: e = '(' + a(0) + " * " + a(1) + ')'; break;
    case dot1: e = "dot(" + a(0) + ", " + a(1) + ')'; break;
    case cross0: e = "cross(" + a(0) + ", " + a(1) + ')'; break;
    case length0: e = args[0].type.rows > 1 ? "length(" + a(0) + ')' : "abs(" + a(0) + ')'; break;
    case distance0: e = args[0].type.rows > 1 ? "distance(" + a(0) + ", " + a(1) + ')' : "abs(" + a(0) + " - " + a(1) + ')'; break;
    case normalize0: e = "normalize(" + a(0) + ')'; break;
    case transpose0: e = "transpose(" + a(0) + ')'; break;
    case determinant0: e = "determinant(" + a(0) + ')'; break;
    case reflect0: e = "reflect(" + a(0) + ", " + a(1) + ')'; break;
    case refract0: e = "refract(" + a(0) + ", " + a(1) + ", " + a(2) + ')'; break;
    case faceforward0:
        if (args[0].type.rows > 1) {
            e = "faceForward(" + a(0) + ", " + a(1) + ", " + a(2) + ')';
        } else {
            e = "select(-" + a(0) + ", " + a(0) + ", (" + a(1) + " * " + a(2) + ") < 0.0)";
        }
        break;
    case mul0: case mul1: case mul2: case mul3:
        e = '(' + a(0) + " * " + a(1) + ')';
        break;
    case mul4: case mul5: case mul6:
        // Matrices are stored transposed (see the file comment), so the operands swap.
        if (!args[0].type.is_floating_point() || !args[1].type.is_floating_point()) {
            unsupported("mul() with integer matrices or vectors");
            break;
        }
        e = '(' + a(1) + " * " + a(0) + ')';
        break;
    case isinf0:
        e = args[0].type.rows > 1
            ? "((bitcast<" + vecu(args[0].type.rows) + ">(" + a(0) + ") & " + vecu(args[0].type.rows) + "(0x7fffffffu)) == " + vecu(args[0].type.rows) + "(0x7f800000u))"
            : "((bitcast<u32>(" + a(0) + ") & 0x7fffffffu) == 0x7f800000u)";
        break;
    case isnan0:
        e = args[0].type.rows > 1
            ? "((bitcast<" + vecu(args[0].type.rows) + ">(" + a(0) + ") & " + vecu(args[0].type.rows) + "(0x7fffffffu)) > " + vecu(args[0].type.rows) + "(0x7f800000u))"
            : "((bitcast<u32>(" + a(0) + ") & 0x7fffffffu) > 0x7f800000u)";
        break;

    case tex1D0: case tex2D0: case tex3D0: e = tex("t", {a(1)}); break;
    case tex1D1: case tex2D1: case tex3D1: e = tex("to", {a(1), a(2)}); break;
    case tex1Dgrad0: case tex2Dgrad0: case tex3Dgrad0: e = tex("g", {a(1), a(2), a(3)}); break;
    case tex1Dgrad1: case tex2Dgrad1: e = tex("go", {a(1), a(2), a(3), a(4)}); break;
    case tex3Dgrad1: e = tex("go", {a(1), a(2), a(3), "vec3<i32>(" + a(4) + ", 0)"}); break;
    case tex1Dlod0: e = tex("l", {a(1) + ".x", a(1) + ".w"}); break;
    case tex2Dlod0: e = tex("l", {a(1) + ".xy", a(1) + ".w"}); break;
    case tex3Dlod0: e = tex("l", {a(1) + ".xyz", a(1) + ".w"}); break;
    case tex1Dlod1: e = tex("lo", {a(1) + ".x", a(1) + ".w", a(2)}); break;
    case tex2Dlod1: e = tex("lo", {a(1) + ".xy", a(1) + ".w", a(2)}); break;
    case tex3Dlod1: e = tex("lo", {a(1) + ".xyz", a(1) + ".w", "vec3<i32>(" + a(2) + ", 0)"}); break;
    case tex1Dfetch0: case tex2Dfetch0: case tex3Dfetch0: e = tex("f", {a(1)}); break;
    case tex1Dfetch1: case tex2Dfetch1: case tex3Dfetch1: e = tex("fl", {a(1), a(2)}); break;
    case tex1Dfetch2: case tex2Dfetch2: case tex3Dfetch2: e = tex("sl", {a(1)}); break;
    case tex2DgatherR0: e = tex("ga0", {a(1)}); break;
    case tex2DgatherG0: e = tex("ga1", {a(1)}); break;
    case tex2DgatherB0: e = tex("ga2", {a(1)}); break;
    case tex2DgatherA0: e = tex("ga3", {a(1)}); break;
    case tex2DgatherR1: e = tex("gao0", {a(1), a(2)}); break;
    case tex2DgatherG1: e = tex("gao1", {a(1), a(2)}); break;
    case tex2DgatherB1: e = tex("gao2", {a(1), a(2)}); break;
    case tex2DgatherA1: e = tex("gao3", {a(1), a(2)}); break;
    case tex2DgatherR2: e = tex("gaf0", {a(1), a(2), a(3), a(4), a(5)}); break;
    case tex2DgatherG2: e = tex("gaf1", {a(1), a(2), a(3), a(4), a(5)}); break;
    case tex2DgatherB2: e = tex("gaf2", {a(1), a(2), a(3), a(4), a(5)}); break;
    case tex2DgatherA2: e = tex("gaf3", {a(1), a(2), a(3), a(4), a(5)}); break;
    case tex1Dstore0: case tex2Dstore0: case tex3Dstore0:
        code += '\t' + helper_call("ss", args[0].base) + a(1) + ", " + storage_value(2) + ");\n";
        return res;
    case tex1Dsize0: case tex2Dsize0: case tex3Dsize0: e = helper_call("z", args[0].base) + ')'; break;
    case tex1Dsize1: case tex2Dsize1: case tex3Dsize1: e = helper_call("zl", args[0].base) + a(1) + ')'; break;
    case tex1Dsize2: case tex2Dsize2: case tex3Dsize2: e = helper_call("sz", args[0].base) + ')'; break;

    case barrier0:
        code += std::string("\t") + kStage + "BworkgroupBarrier()" + kStage + ";\n";
        return res;
    case memoryBarrier0:
        code += std::string("\t") + kStage + "BstorageBarrier(); workgroupBarrier()" + kStage + ";\n";
        return res;
    case groupMemoryBarrier0:
        code += std::string("\t") + kStage + "BworkgroupBarrier()" + kStage + ";\n";
        return res;

    case atomicAdd0: case atomicAnd0: case atomicOr0: case atomicXor0: case atomicMin0: case atomicMin1:
    case atomicMax0: case atomicMax1: case atomicExchange0: {
        const auto root = _lvalue_root.find(args[0].base);
        const id rootVar = root != _lvalue_root.end() ? root->second : args[0].base;
        if (!is_atomic_root(rootVar)) {
            unsupported("atomic operation on a variable that is not groupshared int/uint");
            break;
        }
        const char* fn = "atomicAdd";
        switch (intrinsic) {
        case atomicAnd0: fn = "atomicAnd"; break;
        case atomicOr0: fn = "atomicOr"; break;
        case atomicXor0: fn = "atomicXor"; break;
        case atomicMin0: case atomicMin1: fn = "atomicMin"; break;
        case atomicMax0: case atomicMax1: fn = "atomicMax"; break;
        case atomicExchange0: fn = "atomicExchange"; break;
        default: break;
        }
        e = std::string(fn) + "(&" + a(0) + ", " + a(1) + ')';
        break;
    }
    case atomicCompareExchange0: {
        const auto root = _lvalue_root.find(args[0].base);
        const id rootVar = root != _lvalue_root.end() ? root->second : args[0].base;
        if (!is_atomic_root(rootVar)) {
            unsupported("atomic operation on a variable that is not groupshared int/uint");
            break;
        }
        // HLSL's compare-exchange is strong; WGSL only has the weak form, so retry spurious failures.
        const std::string r = id_to_name(res);
        code += "\tvar " + r + "_v : " + rtName + ";\n";
        code += "\tloop {\n\t\tlet " + r + "_x = atomicCompareExchangeWeak(&" + a(0) + ", " + a(1) + ", " + a(2) + ");\n";
        code += "\t\tif (" + r + "_x.exchanged || " + r + "_x.old_value != " + a(1) + ") { " + r + "_v = " + r + "_x.old_value; break; }\n\t}\n";
        e = r + "_v";
        break;
    }
    case atomicAdd1: case atomicAnd1: case atomicOr1: case atomicXor1: case atomicMin2: case atomicMin3:
    case atomicMax2: case atomicMax3: case atomicExchange1: case atomicCompareExchange1:
        unsupported("atomic operations on storage textures (WebGPU has no texture atomics)");
        break;
    default:
        unsupported("an intrinsic this back-end does not implement");
        break;
    }

    if (rv) {
        code += "\tlet " + id_to_name(res) + " : " + rtName + " = " + e + ";\n";
    } else if (!e.empty()) {
        code += '\t' + e + ";\n";
    }
    return res;
}

// -------------------------------------------------------------------------------------------------
// Assembly

bool codegen_wgsl::assemble(const std::string& entryName, WgslEntryPoint& out, std::string& errors) const {
    const auto entryIt = _entries.find(entryName);
    const function* const ep = find_function(entryName);
    if (entryIt == _entries.end() || ep == nullptr) {
        errors += "unknown entry point '" + entryName + "'\n";
        return false;
    }
    if (!_unsupported.empty()) {
        std::set<std::string> unique(_unsupported.begin(), _unsupported.end());
        for (const std::string& u : unique) {
            errors += "unsupported in WebGPU: " + u + '\n';
        }
        return false;
    }
    const EntryExtra& entry = entryIt->second;
    const shader_type stype = entry.stype;

    // Specialise every function reachable from the entry point for the objects it is called with.
    std::map<std::string, std::string> instanceNames;
    std::vector<std::string> instances;
    bool ok = true;

    std::function<std::string(const std::string&, const std::unordered_map<id, std::string>&)> instantiate_text;
    std::function<std::string(id, const std::vector<std::string>&)> instantiate_function;

    const auto substitute_objects = [&](const std::string& s, const std::unordered_map<id, std::string>& map) {
        std::string r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == kObjBegin) {
                const size_t end = s.find(kObjEnd, i);
                const id pid = static_cast<id>(std::stoul(s.substr(i + 1, end - i - 1)));
                const auto it = map.find(pid);
                if (it == map.end()) {
                    errors += "unresolved sampler/storage parameter\n";
                    ok = false;
                    r += "s0";
                } else {
                    r += it->second;
                }
                i = end;
            } else {
                r += s[i];
            }
        }
        return r;
    };

    instantiate_text = [&](const std::string& s, const std::unordered_map<id, std::string>& map) {
        const std::string t = substitute_objects(s, map);
        std::string r;
        r.reserve(t.size());
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] != kCallBegin) {
                r += t[i];
                continue;
            }
            const size_t end = t.find(kCallEnd, i);
            const std::string body = t.substr(i + 1, end - i - 1);
            const size_t bar = body.find('|');
            const id fid = static_cast<id>(std::stoul(body.substr(0, bar)));
            std::vector<std::string> tokens;
            const std::string list = body.substr(bar + 1);
            for (size_t p = 0; p < list.size();) {
                const size_t comma = list.find(',', p);
                tokens.push_back(list.substr(p, comma == std::string::npos ? std::string::npos : comma - p));
                if (comma == std::string::npos) {
                    break;
                }
                p = comma + 1;
            }
            r += instantiate_function(fid, tokens);
            i = end;
        }
        return r;
    };

    instantiate_function = [&](id fid, const std::vector<std::string>& tokens) -> std::string {
        std::string key = std::to_string(fid);
        for (const std::string& t : tokens) {
            key += ':' + t;
        }
        if (const auto it = instanceNames.find(key); it != instanceNames.end()) {
            return it->second;
        }
        std::string name = id_to_name(fid);
        for (const std::string& t : tokens) {
            name += '_' + t;
        }
        instanceNames.emplace(key, name);
        const auto block = _blocks.find(fid);
        const auto extra = _func_extra.find(fid);
        if (block == _blocks.end() || extra == _func_extra.end()) {
            errors += "missing function body\n";
            ok = false;
            return name;
        }
        std::unordered_map<id, std::string> map;
        for (size_t k = 0; k < extra->second.object_params.size() && k < tokens.size(); ++k) {
            map[extra->second.object_params[k]] = tokens[k];
        }
        std::string text = block->second;
        if (const size_t pos = text.find(kFnName); pos != std::string::npos) {
            text.replace(pos, 1, name);
        }
        instances.push_back(instantiate_text(text, map));
        return name;
    };

    const std::string wrapper = instantiate_text(entry.text, {});
    if (!ok) {
        return false;
    }

    // Resolve stage markers.
    const auto resolve_stage = [&](std::string s) {
        std::string r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != kStage) {
                r += s[i];
                continue;
            }
            const size_t end = s.find(kStage, i + 1);
            const std::string m = s.substr(i + 1, end - i - 1);
            if (m == "D") {
                r += stype == shader_type::pixel ? "discard" : "";
            } else if (!m.empty() && m[0] == 'G') {
                r += stype == shader_type::pixel ? m.substr(1) : std::string("0.0 * ");
            } else if (!m.empty() && m[0] == 'B') {
                r += stype == shader_type::compute ? m.substr(1) : std::string();
            }
            i = end;
        }
        return r;
    };

    std::string functions;
    for (const std::string& inst : instances) {
        functions += resolve_stage(inst);
    }
    functions += resolve_stage(wrapper);

    // Collect helper uses: _rsp_<op>_<s|u><index>(
    std::set<std::pair<std::string, std::string>> uses;
    for (size_t pos = 0; (pos = functions.find("_rsp_", pos)) != std::string::npos;) {
        pos += 5;
        const size_t us = functions.find('_', pos);
        if (us == std::string::npos) {
            break;
        }
        const std::string op = functions.substr(pos, us - pos);
        size_t p = us + 1;
        if (p < functions.size() && (functions[p] == 's' || functions[p] == 'u')) {
            size_t q = p + 1;
            while (q < functions.size() && functions[q] >= '0' && functions[q] <= '9') {
                ++q;
            }
            if (q > p + 1 && q < functions.size() && functions[q] == '(') {
                uses.insert({op, functions.substr(p, q - p)});
            }
        }
    }

    // Bindings.
    std::string bindings;
    std::unordered_map<uint32_t, uint32_t> samplerBinding;
    for (uint32_t b = 0; b < ep->referenced_samplers.size(); ++b) {
        if (ep->referenced_samplers[b] == 0) {
            continue;
        }
        const auto it = _sampler_index.find(ep->referenced_samplers[b]);
        if (it == _sampler_index.end()) {
            continue;
        }
        SamplerSpec sp;
        if (!sampler_spec(it->second, sp, errors)) {
            return false;
        }
        samplerBinding[it->second] = b;
        const std::string sfx = "_s" + std::to_string(it->second);
        bindings += "@group(1) @binding(" + std::to_string(2 * b) + ") var _rsp_T" + sfx + " : texture_" +
                    (sp.dims == 3 ? "3d" : "2d") + '<' + sample_scalar(sp.phys.sample_type) + ">;\n";
        bindings += "@group(1) @binding(" + std::to_string(2 * b + 1) + ") var _rsp_S" + sfx + " : sampler;\n";
        WgslSamplerBinding sb;
        sb.binding = b;
        sb.sampler_index = it->second;
        sb.sample_type = sp.phys.sample_type;
        sb.unfilterable = sp.unfilterable;
        sb.is_3d = sp.dims == 3;
        out.samplers.push_back(sb);
    }

    // Storage access modes come from the operations the entry point uses.
    std::unordered_map<uint32_t, std::pair<bool, bool>> storageAccess; // index -> (read, write)
    for (const auto& [op, token] : uses) {
        if (token[0] == 'u') {
            auto& rw = storageAccess[static_cast<uint32_t>(std::stoul(token.substr(1)))];
            if (op == "sl") {
                rw.first = true;
            } else if (op == "ss") {
                rw.second = true;
            }
        }
    }
    std::unordered_map<uint32_t, bool> storage3d;
    for (uint32_t b = 0; b < ep->referenced_storages.size(); ++b) {
        if (ep->referenced_storages[b] == 0) {
            continue;
        }
        const auto it = _storage_index.find(ep->referenced_storages[b]);
        if (it == _storage_index.end()) {
            continue;
        }
        const storage& st = _module.storages[it->second];
        const texture* tex = nullptr;
        for (const texture& t : _module.textures) {
            if (t.unique_name == st.texture_name) {
                tex = &t;
            }
        }
        if (tex == nullptr) {
            errors += "storage references an unknown texture\n";
            return false;
        }
        const TexturePhysical phys = _host.physical(*tex);
        if (phys.storage_format == nullptr) {
            errors += "texture '" + tex->name + "' cannot be used as storage\n";
            return false;
        }
        const auto rw = storageAccess[it->second];
        StorageAccess access = StorageAccess::Write;
        const char* accessName = "write";
        if (rw.first && rw.second) {
            const std::string f = phys.storage_format;
            if (f != "r32float" && f != "r32uint" && f != "r32sint") {
                errors += "storage '" + st.name + "' is both read and written in one shader, which WebGPU allows only for R32F/R32U/R32I\n";
                return false;
            }
            access = StorageAccess::ReadWrite;
            accessName = "read_write";
        } else if (rw.first) {
            access = StorageAccess::Read;
            accessName = "read";
        }
        const bool three = tex->type == texture_type::texture_3d;
        storage3d[it->second] = three;
        bindings += "@group(2) @binding(" + std::to_string(b) + ") var _rsp_ST_u" + std::to_string(it->second) +
                    " : texture_storage_" + (three ? "3d" : "2d") + '<' + phys.storage_format + ", " + accessName + ">;\n";
        WgslStorageBinding sb;
        sb.binding = b;
        sb.storage_index = it->second;
        sb.access = access;
        sb.sample_type = phys.sample_type;
        sb.is_3d = three;
        sb.format = phys.storage_format;
        out.storages.push_back(sb);
    }

    // Helpers.
    std::string helpers;
    std::set<std::string> have;
    bool needAddr = false;
    for (const auto& [op, token] : uses) {
        const uint32_t index = static_cast<uint32_t>(std::stoul(token.substr(1)));
        if (token[0] == 's') {
            if (samplerBinding.find(index) == samplerBinding.end()) {
                errors += "a sampler is used that the entry point does not reference\n";
                return false;
            }
            SamplerSpec sp;
            if (!sampler_spec(index, sp, errors)) {
                return false;
            }
            sampler_helper(sp, op, stype, helpers, have, needAddr);
        } else {
            const storage& st = _module.storages[index];
            const texture* tex = nullptr;
            for (const texture& t : _module.textures) {
                if (t.unique_name == st.texture_name) {
                    tex = &t;
                }
            }
            if (tex == nullptr || storage3d.find(index) == storage3d.end()) {
                errors += "a storage is used that the entry point does not reference\n";
                return false;
            }
            storage_helper(index, _host.physical(*tex), tex->type == texture_type::texture_1d,
                tex->type == texture_type::texture_3d, op, helpers, have);
        }
    }

    std::string code = "diagnostic(off, derivative_uniformity);\n";
    if (_module.total_uniform_size != 0) {
        code += "struct _rsp_Globals {\n" + _ubo_members + "}\n@group(0) @binding(0) var<uniform> _rsp_G : _rsp_Globals;\n";
    }
    code += bindings;
    const std::string& globals = _blocks.at(0);
    const std::string all = globals + helpers + functions;
    if (all.find("_rsp_inf()") != std::string::npos) {
        code += "fn _rsp_inf() -> f32 { var b = 0x7f800000u; return bitcast<f32>(b); }\n";
    }
    if (all.find("_rsp_nan()") != std::string::npos) {
        code += "fn _rsp_nan() -> f32 { var b = 0x7fc00000u; return bitcast<f32>(b); }\n";
    }
    if (needAddr) {
        code +=
            "fn _rsp_addr(c : i32, n : i32, m : u32) -> i32 {\n"
            "\tif (m == 1u) { return ((c % n) + n) % n; }\n"
            "\tif (m == 2u) { let p = 2 * n; var k = ((c % p) + p) % p; if (k >= n) { k = p - 1 - k; } return k; }\n"
            "\tif (m == 4u) { if (c < 0 || c >= n) { return -1; } return c; }\n"
            "\treturn clamp(c, 0, n - 1);\n}\n";
    }
    code += globals;
    code += helpers;
    code += functions;

    out.code = std::move(code);
    out.type = stype;
    out.fragment_outputs = entry.fs_outputs;
    std::copy(std::begin(entry.fs_types), std::end(entry.fs_types), std::begin(out.fragment_output_types));
    out.workgroup_size[0] = static_cast<uint32_t>(entry.threads[0]);
    out.workgroup_size[1] = static_cast<uint32_t>(entry.threads[1]);
    out.workgroup_size[2] = static_cast<uint32_t>(entry.threads[2]);
    return ok;
}

reshadefx::codegen* create_codegen_wgsl(WgslHost host) {
    return new codegen_wgsl(std::move(host));
}

bool assemble_wgsl(const reshadefx::codegen& cg, const std::string& entryPoint, WgslEntryPoint& out, std::string& errors) {
    const auto* wgsl = dynamic_cast<const codegen_wgsl*>(&cg);
    if (wgsl == nullptr) {
        errors += "not a WGSL code generator\n";
        return false;
    }
    return wgsl->assemble(entryPoint, out, errors);
}

} // namespace rsp
