// Deferred Fog: fullscreen re-application of the game's fog after other mods' screen-space
// composites (AO, shadows).
//
// Follows aurora's generated fog code. With reversed-Z (1 = near):
//     fogF = clamp(a / (b - (1.0 - z)) * range_mul - c, 0.0, 1.0)
// then one of five curves, then mix(pixel, fogColor, fogZ). The only per-fragment input is depth,
// which the scene depth snapshot holds, so each pixel gets the fog of the surface that owns its
// depth. The (a, b, c) coefficients come from mod.cpp through the same BP-register quantization
// the game's fog goes through (src/fog_math.h). Differences from aurora: range_mul is computed per
// pixel from the full-target uv, where aurora bakes a per-column LUT over the render viewport (the
// two agree when the world viewport spans the target), and orthographic fog is treated as
// perspective.
//
// Blending: (SrcAlpha, OneMinusSrcAlpha) on colour with fogZ in alpha reproduces aurora's mix();
// alpha is left untouched (Zero, One), as forward fog leaves it.
//
// Sky pixels are skipped: the sky lists draw before the suppression scope opens and keep their own
// forward fog. A pixel is the sky's when its raw depth is 0 (cleared) or still equals the sky-depth
// snapshot mod.cpp takes when the scope opens (see is_sky).

// GX fog range adjustment ("XFog"; see FogRangeAdj in mod.cpp): a per-column multiplier on the fog
// term, applied before the start-Z bias c is subtracted, because a pixel near the screen edge is
// further from the eye than a centre pixel with the same Z.
struct FogRange {
    center: f32,    // centre column in NDC x (2 * center / viewport_width - 1)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    k: array<vec4f, 3>,  // the 10 range constants, pair-swapped and scaled by 1/64 as aurora does;
                         // entries 10 and 11 repeat entry 9
}

struct FogUniforms {
    color: vec4f,   // fog colour (rgb; a unused)
    a: f32,         // decoded fog coefficients, see above
    b: f32,
    c: f32,
    fog_type: u32,  // low 3 bits of GXFogType: 2 LIN, 4 EXP, 5 EXP2, 6 REVEXP, 7 REVEXP2.
                    // 0x10 = apply range adjustment (GXFogType's own 0x08, orthographic, is
                    // masked off).
    debug_mode: u32, // 1 = output the fog factor as grayscale (unblended pipeline)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    range: FogRange,
}

// Several configurations (fs_mixed, used whenever the frame runs the replay): a per-pixel config-ID
// buffer, made by replaying the opaque lists with each draw forced to a flat colour, selects one of
// up to 8 captured configs. The ID is
// (index + 1) * 24 in red, so configs 0..7 use 24..192; anything else decodes as unstamped and
// takes mixed.fallback_index. Red 216 (index 8, decoded as slot 9) is the "no fog" sentinel; see
// stamped_index_at.
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
    debug_mode: u32, // 1 = combined fog factor, 2 = config-ID visualization, 3 = replay coverage
    fallback_index: u32, // config for pixels the ID replay could not stamp (see stamped_index_at)
    _pad1: f32,
    range: FogRange, // shared by every config; each config opts in via its fog_type bit 0x10
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(4) var sky_depth: texture_2d<f32>;
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

// Whether nothing after the sky lists wrote this pixel's depth. Some sky draws write depth (the
// drifting clouds do while the sun is on screen); the world drawn later either replaces it or is
// hidden behind it. Both snapshots are copies of the same depth buffer made the same way, so an
// untouched pixel compares exactly equal.
fn is_sky(uv: vec2f, depth: f32) -> bool {
    if depth <= 0.0 {
        return true;
    }
    let size = vec2<i32>(textureDimensions(sky_depth));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(sky_depth, texel, 0i).r == depth;
}

// The range-adjust multiplier, the same function aurora's build_fog_range_lut bakes per column:
// the table runs from entry 9 at the centre column to entry 0 at |offset| >= 1, linearly
// interpolated, and the result is the ratio of eye distance to axial distance at that offset.
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

// Aurora's fog term and curves. (1.0 - depth) converts reversed-Z to GX screen z (0 = near).
// range_mul (1.0 when the config has range adjustment off) scales the a / (b - z) term before c is
// subtracted, as aurora does; c is large for far-starting bands, so the order matters.
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
    if is_sky(in.uv, depth) {
        // The sky keeps its own (forward) fog.
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

// Returns this pixel's config index from the ID buffer, NO_FOG_INDEX, or UNSTAMPED.
//
// The replay's flat-ID override writes (id, 0, 0). Geometry it cannot reach (the self-drawing
// packets: grass, flowers, dMdl_c models, 3D lines, and any other direct GX drawing) rasterizes lit
// colours, which almost always have non-zero green or blue. Rejecting those as UNSTAMPED sends such
// pixels to mixed.fallback_index (the config the grass and flower packets drew with) instead of
// whichever config their red channel happens to decode to, which would flicker with the lighting.
// Pure-red unstamped geometry could still alias.
//
// The Ganon barrier and fog-off draws that write no depth write nothing in the replay, so their
// pixels carry the config of the surface behind them.
// Slot 9 (red 216, kNoFogSlot in mod.cpp) is the "no fog in vanilla" sentinel. It is checked before
// the `slot <= count` test, which would reject it, and cannot collide with a real config (slots
// 1..8, red 24..192).
const NO_FOG_INDEX: u32 = 0xFFFFFFFFu;
const UNSTAMPED: u32 = 0xFFFFFFFEu;

fn stamped_index_at(uv: vec2f) -> u32 {
    let size = vec2<i32>(textureDimensions(config_ids));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    let c = textureLoad(config_ids, texel, 0i);
    if c.g > 0.03 || c.b > 0.03 {
        return UNSTAMPED;
    }
    let v = i32(round(c.r * 255.0));
    let slot = (v + 12i) / 24i;
    if slot == 9i && abs(v - 216i) <= 4i {
        return NO_FOG_INDEX;
    }
    if slot >= 1i && u32(slot) <= mixed.count && abs(v - slot * 24i) <= 4i {
        return u32(slot) - 1u;
    }
    return UNSTAMPED;
}

@fragment
fn fs_mixed(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    if is_sky(in.uv, depth) {
        if mixed.debug_mode != 0u {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        return vec4f(0.0);
    }

    let stamped = stamped_index_at(in.uv);
    // Checked before the config-ID visualization below, where NO_FOG_INDEX would clip to white and
    // read as the highest config.
    if stamped == NO_FOG_INDEX {
        if mixed.debug_mode != 0u {
            return vec4f(1.0, 0.0, 0.0, 1.0);  // red in every debug view = left unfogged
        }
        return vec4f(0.0);
    }
    if mixed.debug_mode == 3u {
        // Replay coverage: blue = nothing the replay draws, green = a stamped config.
        if stamped == UNSTAMPED {
            return vec4f(0.0, 0.25, 1.0, 1.0);
        }
        return vec4f(0.0, 0.8, 0.2, 1.0);
    }
    let index = select(stamped, mixed.fallback_index, stamped == UNSTAMPED);
    if mixed.debug_mode == 2u {
        // Config-ID visualization: one gray level per config.
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
