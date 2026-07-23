// SMAA — pass 1: edge detection (compute).
//
// Writes a two-channel edge mask (EdgesTex.r = edge on this pixel's LEFT boundary, .g = edge on
// its TOP boundary) and, in the same pass, clears BlendTex to zero so the compacted blend-weight
// pass (pass 2) can write only the sparse edge pixels and leave the rest at 0 (the folded-clear
// trick from Marty's iMMERSE SMAA / CMAA2 — one pass instead of a separate clear).
//
// Two detectors are unioned:
//   * Luma: the reference SMAA luma edge detector with local contrast adaptation (MIT SMAA,
//     Jimenez et al.). Catches shading / texture / alpha-test edges.
//   * Geometric: an angular difference of the Depth to Normal service's reconstructed world-space
//     normal, plus a relative raw-depth discontinuity. Catches silhouettes AND creases (surfaces
//     meeting at an angle with continuous depth) that luma misses on TP's flat-shaded art. Enabled
//     only when the provider is present (flags bit 0).
//
// Reversed-Z: sky raw depth is 0; near is 1. The normal buffer packs xyz = unit world normal,
// w = raw reversed-Z depth.

struct Uniforms {
    screen_size: vec2f,
    inv_screen_size: vec2f,
    threshold: f32,
    normal_threshold: f32,
    depth_threshold: f32,
    max_search_steps: f32,
    local_contrast_factor: f32,
    blend_strength: f32,
    corner_rounding: f32,
    flags: u32,
    debug_view: u32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var normal_tex: texture_2d<f32>;
@group(0) @binding(2) var edges_out: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var blend_clear_out: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;

const LUMA = vec3f(0.2126, 0.7152, 0.0722);

fn clamp_coord(p: vec2i, dims: vec2u) -> vec2i {
    return clamp(p, vec2i(0i), vec2i(dims) - vec2i(1i));
}

fn load_luma(p: vec2i, dims: vec2u) -> f32 {
    return dot(textureLoad(scene_color, clamp_coord(p, dims), 0i).rgb, LUMA);
}

@compute @workgroup_size(8, 8, 1)
fn edge_detection(@builtin(global_invocation_id) gid: vec3u) {
    let dims = textureDimensions(scene_color);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }
    let p = vec2i(gid.xy);

    // Always clear this pixel's blend weights (pass 2 overwrites only the edge pixels).
    textureStore(blend_clear_out, p, vec4f(0.0));

    // --- Luma edge detection (reference SMAA, with local contrast adaptation) ---
    let L = load_luma(p, dims);
    let Lleft = load_luma(p + vec2i(-1i, 0i), dims);
    let Ltop = load_luma(p + vec2i(0i, -1i), dims);

    var delta_lt = abs(L - vec2f(Lleft, Ltop));
    var luma_edges = step(vec2f(uniforms.threshold), delta_lt);

    // Local contrast adaptation: suppress an edge when a much stronger neighbouring gradient runs
    // parallel to it (kills doubled/edges inside high-contrast texture). Only meaningful when at
    // least one edge fired; cheap enough to always run in a compute pass.
    let Lright = load_luma(p + vec2i(1i, 0i), dims);
    let Lbottom = load_luma(p + vec2i(0i, 1i), dims);
    let delta_rb = abs(L - vec2f(Lright, Lbottom));
    var max_delta = max(delta_lt, delta_rb);
    let Lleftleft = load_luma(p + vec2i(-2i, 0i), dims);
    let Ltoptop = load_luma(p + vec2i(0i, -2i), dims);
    let delta_2 = abs(vec2f(Lleft, Ltop) - vec2f(Lleftleft, Ltoptop));
    max_delta = max(max_delta, delta_2);
    let final_delta = max(max_delta.x, max_delta.y);
    luma_edges = luma_edges * step(vec2f(final_delta), uniforms.local_contrast_factor * delta_lt);

    var edges = luma_edges;

    // --- Geometric edge detection from the reconstructed normal + depth ---
    if ((uniforms.flags & 1u) != 0u) {
        let ndims = textureDimensions(normal_tex);
        let nc = textureLoad(normal_tex, clamp_coord(p, ndims), 0i);
        let nl = textureLoad(normal_tex, clamp_coord(p + vec2i(-1i, 0i), ndims), 0i);
        let nt = textureLoad(normal_tex, clamp_coord(p + vec2i(0i, -1i), ndims), 0i);

        // Angular difference: 0 = coplanar, grows with the crease/silhouette angle.
        let normal_delta = vec2f(1.0 - dot(nc.xyz, nl.xyz), 1.0 - dot(nc.xyz, nt.xyz));
        // Relative raw-depth discontinuity (reversed-Z; robust across the depth range and to sky=0).
        let dc = nc.w;
        let eps = 1.0e-5;
        let depth_delta = vec2f(
            abs(dc - nl.w) / max(max(dc, nl.w), eps),
            abs(dc - nt.w) / max(max(dc, nt.w), eps));

        let geo_edges = max(
            step(vec2f(uniforms.normal_threshold), normal_delta),
            step(vec2f(uniforms.depth_threshold), depth_delta));
        edges = max(edges, geo_edges);
    }

    textureStore(edges_out, p, vec4f(edges, 0.0, 0.0));
}
