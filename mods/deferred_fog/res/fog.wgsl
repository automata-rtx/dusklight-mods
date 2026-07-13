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

struct FogUniforms {
    color: vec4f,   // fog color (rgb; a unused)
    a: f32,         // decoded fog coefficients, see above
    b: f32,
    c: f32,
    fog_type: u32,  // low 3 bits of GXFogType: 2 LIN, 4 EXP, 5 EXP2, 6 REVEXP, 7 REVEXP2
    debug_mode: u32, // 1 = output the fog factor as grayscale (unblended pipeline)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
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
    _pad0: f32,
    _pad1: f32,
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

// Aurora's fog term, verbatim: (1.0 - depth) is the GC screen-z convention (0 = near).
fn fog_z_for(a: f32, b: f32, c: f32, fog_type: u32, depth: f32) -> f32 {
    var fog_f = clamp((a / (b - (1.0 - depth))) - c, 0.0, 1.0);
    var fog_z: f32;
    switch fog_type {
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

    let fog_z = fog_z_for(uniforms.a, uniforms.b, uniforms.c, uniforms.fog_type, depth);
    if uniforms.debug_mode != 0u {
        return vec4f(fog_z, fog_z, fog_z, 1.0);
    }
    return vec4f(uniforms.color.rgb, fog_z);
}

// Decode the sparse config ID at this pixel; 0 = invalid/uncovered -> config 0 (reference).
fn config_index_at(uv: vec2f) -> u32 {
    let size = vec2<i32>(textureDimensions(config_ids));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    let v = i32(round(textureLoad(config_ids, texel, 0i).r * 255.0));
    let slot = (v + 12i) / 24i;
    if slot >= 1i && u32(slot) <= mixed.count && abs(v - slot * 24i) <= 4i {
        return u32(slot) - 1u;
    }
    return 0u;
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
    if mixed.debug_mode == 2u {
        // Config-ID visualization: distinct gray band per config.
        let value = (f32(index) + 1.0) / max(f32(mixed.count), 1.0);
        return vec4f(value, value, value, 1.0);
    }
    let entry = mixed.configs[index];
    let fog_z = fog_z_for(entry.a, entry.b, entry.c, entry.fog_type, depth);
    if mixed.debug_mode != 0u {
        return vec4f(fog_z, fog_z, fog_z, 1.0);
    }
    return vec4f(entry.color.rgb, fog_z);
}
