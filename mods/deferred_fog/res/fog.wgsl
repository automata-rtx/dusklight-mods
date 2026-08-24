// Deferred Fog - fullscreen re-application of the game's fog after other mods' screen-space
// effects (AO, shadows) have composited.
//
// This reproduces aurora's fog EXACTLY: aurora's generated fragment shaders compute
//     fogF = clamp(a / (b - (1.0 - in.pos.z)) - c, 0.0, 1.0)     (reversed-Z)
// followed by one of five curves, then mix(pixel, fogColor, fogZ). The only per-fragment
// input is the raw depth value - exactly what the scene depth snapshot holds - so applying
// the same math per pixel over the opaque scene yields the same result forward fog would
// have produced. The (a, b, c) coefficients arrive pre-computed from mod.cpp, which mirrors
// the exact J3DGDSetFog BP encode -> aurora command-processor decode round trip (including
// the 11-bit mantissa truncation), so even the quantization matches the vanilla path.
//
// Blending: (srcAlpha, oneMinusSrcAlpha) on color with fogZ in alpha reproduces aurora's
// mix(); the target's alpha channel is left untouched (Zero/One), matching forward fog
// which never wrote alpha.
//
// Sky pixels (raw depth 0) are skipped: the sky draws before the world lists, outside the
// suppression scope, and keeps its own forward fog.

// GX fog range adjustment ("XFog"), reproduced from aurora's build_fog_range_lut. Fog is driven by
// Z, the distance along the view axis, but a pixel at the screen edge is genuinely further from the
// eye than a centre pixel of the same Z; the hardware corrects for that with a per-column
// multiplier on the fog term, applied BEFORE the start-Z bias c is subtracted. TP enables it
// globally (envcolor_init, d_kankyo.cpp:1257) and re-arms it after every fog set, so it is part
// of what vanilla renders. Aurora bakes one multiplier per target column into a storage buffer;
// this evaluates the identical function per pixel instead, which needs no storage binding.
struct FogRange {
    center: f32,    // fog-range centre column in NDC x (2 * center / viewport_width - 1)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    k: array<vec4f, 3>,  // the 10 range constants, pair-swapped and scaled by 1/64 as aurora does
}

struct FogUniforms {
    color: vec4f,   // fog color (rgb; a unused)
    a: f32,         // decoded fog coefficients, see above
    b: f32,
    c: f32,
    fog_type: u32,  // low 3 bits of GXFogType: 2 LIN, 4 EXP, 5 EXP2, 6 REVEXP, 7 REVEXP2.
                    // Bit 4 (0x10) additionally means "apply the range adjustment" — GXFogType's
                    // own bit 3 means orthographic, so the flag sits above the masked type.
    debug_mode: u32, // 1 = output the fog factor as grayscale (unblended pipeline)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    range: FogRange,
}

// Mixed-configuration mode (fs_mixed): a per-pixel config-ID buffer, produced by replaying the
// opaque draw lists with each shape's output forced to a flat index color, selects which of up
// to 8 captured fog configurations applies to each pixel. IDs are encoded sparsely as
// (index + 1) * 24 in the red channel so colors written by geometry outside the ID override
// (rare non-J3D drawers) decode as invalid and fall back to config 0 - the frame's reference
// config, i.e. exactly what the single-config path would have applied to them.
struct MixedFogEntry {
    color: vec4f,
    a: f32,
    b: f32,
    c: f32,
    fog_type: u32,
}

struct MixedFogUniforms {
    configs: array<MixedFogEntry, 8>,
    count: u32,
    debug_mode: u32, // 1 = combined fog factor, 2 = config-ID visualization
    fallback_index: u32, // config for pixels the ID replay could not cover (see config_index_at)
    _pad1: f32,
    range: FogRange, // shared by every config; each config opts in via its fog_type bit 0x10
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var<uniform> uniforms: FogUniforms;
// fs_mixed only:
@group(0) @binding(2) var config_ids: texture_2d<f32>;
@group(0) @binding(3) var<uniform> mixed: MixedFogUniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn scene_depth_at(uv: vec2f) -> f32 {
    let size = vec2<i32>(textureDimensions(scene_depth));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(scene_depth, texel, 0i).r;
}

// The per-column range-adjust multiplier, matching aurora's build_fog_range_lut exactly: the table
// is indexed from the centre outwards (entry 9 at the centre column, entry 0 at |offset| >= 1) with
// linear interpolation between neighbours, and the multiplier is the ratio between the eye distance
// and the axial distance for a column that far off centre.
fn fog_range_factor(range: FogRange, ndc_x: f32) -> f32 {
    let offset = ndc_x - range.center;
    let index = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
    let lower = u32(index);
    let upper = min(lower + 1u, 9u);
    let fraction = index - f32(lower);
    let k_lower = range.k[lower / 4u][lower % 4u];
    let k_upper = range.k[upper / 4u][upper % 4u];
    let k = max(mix(k_lower, k_upper, fraction), 0.000001);
    return sqrt(offset * offset + k * k) / k;
}

// Aurora's fog term, verbatim: (1.0 - depth) is the GC screen-z convention (0 = near). range_mul is
// the range-adjust multiplier (1.0 when the config has it off); aurora applies it to the a/(b - z)
// term before subtracting c, and where it lands matters — c is a large constant for narrow
// far-starting bands, so folding it in afterwards would scale the bias too.
fn fog_z_for(a: f32, b: f32, c: f32, fog_type: u32, depth: f32, range_mul: f32) -> f32 {
    var fog_f = clamp((a / (b - (1.0 - depth))) * range_mul - c, 0.0, 1.0);
    var fog_z: f32;
    switch fog_type & 7u {
        case 4u: { // GX_FOG_(PERSP|ORTHO)_EXP
            fog_z = 1.0 - exp2(-8.0 * fog_f);
        }
        case 5u: { // GX_FOG_(PERSP|ORTHO)_EXP2
            fog_z = 1.0 - exp2(-8.0 * fog_f * fog_f);
        }
        case 6u: { // GX_FOG_(PERSP|ORTHO)_REVEXP
            fog_z = exp2(-8.0 * (1.0 - fog_f));
        }
        case 7u: { // GX_FOG_(PERSP|ORTHO)_REVEXP2
            fog_f = 1.0 - fog_f;
            fog_z = exp2(-8.0 * fog_f * fog_f);
        }
        default: { // GX_FOG_(PERSP|ORTHO)_LIN
            fog_z = fog_f;
        }
    }
    return clamp(fog_z, 0.0, 1.0);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    if depth <= 0.0 {
        // Sky / cleared pixels keep their own (forward) fog.
        if uniforms.debug_mode != 0u {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        return vec4f(0.0);
    }

    var range_mul = 1.0;
    if (uniforms.fog_type & 0x10u) != 0u {
        range_mul = fog_range_factor(uniforms.range, in.uv.x * 2.0 - 1.0);
    }
    let fog_z =
        fog_z_for(uniforms.a, uniforms.b, uniforms.c, uniforms.fog_type, depth, range_mul);
    if uniforms.debug_mode != 0u {
        return vec4f(fog_z, fog_z, fog_z, 1.0);
    }
    return vec4f(uniforms.color.rgb, fog_z);
}

// Decode the sparse config ID at this pixel; 0 = invalid/uncovered -> config 0 (reference).
//
// The replay's flat-ID override outputs (id, 0, 0): the index in red, green and blue forced to
// zero. Geometry that bypassed the override - self-drawing packets (field/tall grass, flowers,
// waterfalls) and any direct GX drawers - rasterizes real LIT colors instead, which vary with
// the time of day and almost always carry non-zero green/blue. Requiring green and blue to be
// ~0 rejects all of that so it falls back to config 0 (the reference config, which is the
// correct room fog for grass anyway) instead of a per-pixel config that flickers with the
// lighting. (Pure-red bypassed geometry could still alias, but none occurs in practice.)
// Returns the fog-config index for a pixel from the ID buffer. Pixels the replay could not stamp
// (translucent surfaces drawn in the opaque lists, notably the Ganon barrier dome, and any
// remaining non-J3D drawer) decode as invalid and fall back to mixed.fallback_index — the config
// whose fog reaches furthest this frame, since an uncovered pixel is most often distant geometry —
// rather than to config 0.
// Slot 9 (red byte 216) is the "this pixel had no fog in vanilla" sentinel — see kNoFogSlot in
// mod.cpp. It is checked BEFORE the `slot <= count` guard, which would otherwise reject it, and it
// can never collide with a real config: those occupy slots 1..8, red 24..192.
const NO_FOG_INDEX: u32 = 0xFFFFFFFFu;

fn config_index_at(uv: vec2f) -> u32 {
    let size = vec2<i32>(textureDimensions(config_ids));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    let c = textureLoad(config_ids, texel, 0i);
    if c.g > 0.03 || c.b > 0.03 {
        return mixed.fallback_index;
    }
    let v = i32(round(c.r * 255.0));
    let slot = (v + 12i) / 24i;
    if slot == 9i && abs(v - 216i) <= 4i {
        return NO_FOG_INDEX;
    }
    if slot >= 1i && u32(slot) <= mixed.count && abs(v - slot * 24i) <= 4i {
        return u32(slot) - 1u;
    }
    return mixed.fallback_index;
}

@fragment
fn fs_mixed(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    if depth <= 0.0 {
        if mixed.debug_mode != 0u {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        return vec4f(0.0);
    }

    let index = config_index_at(in.uv);
    // Checked before the config-ID visualization below: otherwise the sentinel renders as
    // (9 + 1) / count, clips to white, and reads as "the highest config".
    if index == NO_FOG_INDEX {
        if mixed.debug_mode != 0u {
            return vec4f(1.0, 0.0, 0.0, 1.0);  // red = this pixel is left unfogged
        }
        return vec4f(0.0);
    }
    if mixed.debug_mode == 2u {
        // Config-ID visualization: distinct gray band per config.
        let value = (f32(index) + 1.0) / max(f32(mixed.count), 1.0);
        return vec4f(value, value, value, 1.0);
    }
    let entry = mixed.configs[index];
    var range_mul = 1.0;
    if (entry.fog_type & 0x10u) != 0u {
        range_mul = fog_range_factor(mixed.range, in.uv.x * 2.0 - 1.0);
    }
    let fog_z = fog_z_for(entry.a, entry.b, entry.c, entry.fog_type, depth, range_mul);
    if mixed.debug_mode != 0u {
        return vec4f(fog_z, fog_z, fog_z, 1.0);
    }
    return vec4f(entry.color.rgb, fog_z);
}
