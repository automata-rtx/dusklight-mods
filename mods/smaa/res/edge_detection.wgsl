// SMAA pass 1: edge detection (compute, 8x8 workgroups).
//
// Writes EdgesTex: .r = edge on this pixel's left boundary, .g = edge on its top boundary.
// Also writes 0 to BlendTex at every pixel. Pass 2 writes only the edge pixels, so this stands in
// for a separate clear pass.
//
// Two detectors, unioned:
//   - Luma: the reference SMAA luma edge detector with local contrast adaptation, reimplemented
//     from the MIT reference (iryoku/smaa). Catches shading, texture and alpha-test edges.
//   - Geometric (flags bit 0; set only when the normal and depth snapshots both exist and the
//     option is on): the angle between neighbouring authored normals, plus a relative raw-depth
//     step. Catches silhouettes and creases (surfaces meeting at an angle with continuous depth)
//     that luma misses on TP's flat-shaded art.
//
// Inputs: depth is raw reversed-Z (1 = near, sky = 0). normal_tex holds the game's authored
// view-space normal encoded xyz * 0.5 + 0.5; alpha is validity (1 = valid normal, 0 = none).
// A dot product between two normals only needs them in the same space, so view space is fine.
//
// Provenance: res/licenses/ATTRIBUTION.txt.

// Mirrors SmaaUniforms in src/mod.cpp (and the copies in the other two shaders).
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
@group(0) @binding(5) var depth_tex: texture_2d<f32>;

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

    // Clear this pixel's blend weights; pass 2 overwrites only the edge pixels.
    textureStore(blend_clear_out, p, vec4f(0.0));

    // --- Luma edges (reference SMAA) ---
    let L = load_luma(p, dims);
    let Lleft = load_luma(p + vec2i(-1i, 0i), dims);
    let Ltop = load_luma(p + vec2i(0i, -1i), dims);

    var delta_lt = abs(L - vec2f(Lleft, Ltop));
    var luma_edges = step(vec2f(uniforms.threshold), delta_lt);

    // Local contrast adaptation: drop an edge whose luma step is less than 1/local_contrast_factor
    // of the largest step on this pixel's four boundaries and the boundaries one further left and
    // up. Stops doubled edges inside high-contrast texture. Reference SMAA skips this when no edge
    // fired; here it always runs.
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

    // --- Geometric edges (authored normals + depth) ---
    if ((uniforms.flags & 1u) != 0u) {
        let ndims = textureDimensions(normal_tex);
        let nc = textureLoad(normal_tex, clamp_coord(p, ndims), 0i);
        let nl = textureLoad(normal_tex, clamp_coord(p + vec2i(-1i, 0i), ndims), 0i);
        let nt = textureLoad(normal_tex, clamp_coord(p + vec2i(0i, -1i), ndims), 0i);

        // Decode before comparing: the raw texels are biased positive (xyz * 0.5 + 0.5), so even
        // opposed normals would score as similar.
        let dc_n = normalize(nc.xyz * 2.0 - 1.0);
        let dl_n = normalize(nl.xyz * 2.0 - 1.0);
        let dt_n = normalize(nt.xyz * 2.0 - 1.0);

        // Angular difference, 1 - cos(angle): 0 for parallel normals, growing with the angle. Only
        // used where both texels are valid. A draw without normals stores (0.5, 0.5, 0.5), which
        // decodes to a zero vector and normalizes to NaN, and a NaN comparison would silently drop
        // the edge. Boundaries next to invalid pixels (e.g. sky) are left to the depth test.
        let valid_l = nc.w >= 0.5 && nl.w >= 0.5;
        let valid_t = nc.w >= 0.5 && nt.w >= 0.5;
        let normal_delta = vec2f(
            select(0.0, 1.0 - dot(dc_n, dl_n), valid_l),
            select(0.0, 1.0 - dot(dc_n, dt_n), valid_t));

        // Relative raw-depth step: |difference| / the larger raw depth (the nearer one, in
        // reversed-Z). Sky next to sky gives 0; sky next to geometry gives 1.
        let ddims = textureDimensions(depth_tex);
        let dc = textureLoad(depth_tex, clamp_coord(p, ddims), 0i).r;
        let dl = textureLoad(depth_tex, clamp_coord(p + vec2i(-1i, 0i), ddims), 0i).r;
        let dt = textureLoad(depth_tex, clamp_coord(p + vec2i(0i, -1i), ddims), 0i).r;
        let eps = 1.0e-5;
        let depth_delta = vec2f(
            abs(dc - dl) / max(max(dc, dl), eps),
            abs(dc - dt) / max(max(dc, dt), eps));

        let geo_edges = max(
            step(vec2f(uniforms.normal_threshold), normal_delta),
            step(vec2f(uniforms.depth_threshold), depth_delta));
        edges = max(edges, geo_edges);
    }

    textureStore(edges_out, p, vec4f(edges, 0.0, 0.0));
}
