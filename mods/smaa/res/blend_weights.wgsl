// SMAA pass 2: blend-weight calculation (compute, 16x16 workgroups, compacted).
//
// For each edge pixel: find the run of collinear edge pixels through it, model the aliased
// silhouette as a straight line over that run, and turn the line's offset at this pixel's centre
// into a blend weight.
//
// This is the expensive pass, so it compacts its work as Intel's CMAA2 (2018) describes: each
// workgroup appends its edge pixels to a groupshared list, then the first `count` threads take one
// entry each, so sparse edges run in fully occupied warps. Pixels without an edge are never
// processed; pass 1 already zeroed their weights.
//
// Orthogonal patterns only: there is no diagonal search or corner rounding, so edges at or near
// 45 degrees get little or no blending. The search is linear and the coverage analytic, so there
// are no SearchTex/AreaTex lookup textures. The run-end test also differs from reference SMAA; see
// end_sign_h.
//
// BlendTex packing (read by neighborhood_blend.wgsl):
//   .r = this pixel pulls from the pixel above        (this pixel's top edge)
//   .g = the pixel above pulls from this pixel        (same edge, other side)
//   .b = this pixel pulls from the pixel to the left  (this pixel's left edge)
//   .a = the pixel to the left pulls from this pixel  (same edge, other side)
// Each value is at most 0.5 * blend_strength, so at most 0.75 at the 150% maximum.

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

@group(0) @binding(0) var edges_tex: texture_2d<f32>;
@group(0) @binding(1) var blend_out: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;

const GROUP_W = 16u;

var<workgroup> worker_ids: array<u32, 256>;
var<workgroup> worker_count: atomic<u32>;

fn load_edges(p: vec2i, dims: vec2u) -> vec2f {
    let c = clamp(p, vec2i(0i), vec2i(dims) - vec2i(1i));
    return textureLoad(edges_tex, c, 0i).xy;
}

// Walk from p in direction `dstep` while the edge channel `chan` continues (0 = left edges, a
// vertical run; 1 = top edges, a horizontal run). Returns how many pixels past p the run extends,
// capped at max_steps. Coordinates clamp at the screen border.
fn search_run(p: vec2i, dstep: vec2i, chan: i32, dims: vec2u, max_steps: i32) -> i32 {
    var d = 0;
    for (var i = 1; i <= max_steps; i = i + 1) {
        let e = load_edges(p + dstep * i, dims);
        let cont = select(e.x, e.y, chan == 1);
        if (cont < 0.5) {
            break;
        }
        d = i;
    }
    return d;
}

// Which way a horizontal run steps at one end. At the first pixel past the run's end, look at the
// same top-edge channel one row up and one row down: +1 if the edge continues one row up (toward
// the pixel above), -1 if one row down, 0 if neither (the up test wins if both fire). Reference
// SMAA classifies the end from the crossing, perpendicular edges instead; this is a simplification.
fn end_sign_h(p: vec2i, dir: i32, d: i32, dims: vec2u) -> f32 {
    let beyond = p + vec2i(dir * (d + 1), 0i);
    if (load_edges(beyond + vec2i(0i, -1i), dims).y > 0.5) { return 1.0; }
    if (load_edges(beyond + vec2i(0i, 1i), dims).y > 0.5) { return -1.0; }
    return 0.0;
}

// The same for a vertical run, using the left-edge channel one column left and right of the first
// pixel past the end: +1 if it continues one column left (toward the pixel to the left), -1 if one
// column right, 0 if neither.
fn end_sign_v(p: vec2i, dir: i32, d: i32, dims: vec2u) -> f32 {
    let beyond = p + vec2i(0i, dir * (d + 1));
    if (load_edges(beyond + vec2i(-1i, 0i), dims).x > 0.5) { return 1.0; }
    if (load_edges(beyond + vec2i(1i, 0i), dims).x > 0.5) { return -1.0; }
    return 0.0;
}

fn compute_weights(pos: vec2i, dims: vec2u) -> vec4f {
    let e = load_edges(pos, dims);
    let max_steps = max(i32(uniforms.max_search_steps), 1i);
    var weights = vec4f(0.0);

    // Horizontal edge on the top boundary -> blend vertically.
    if (e.y > 0.5) {
        let dL = search_run(pos, vec2i(-1i, 0i), 1i, dims, max_steps);
        let dR = search_run(pos, vec2i(1i, 0i), 1i, dims, max_steps);
        let signL = end_sign_h(pos, -1i, dL, dims);
        let signR = end_sign_h(pos, 1i, dR, dims);
        let run_len = f32(dL + dR + 1);
        // This pixel centre's position along the run, 0 at the left end and 1 at the right end.
        let t = (f32(dL) + 0.5) / run_len;
        // The line runs from +-0.5 px at each end that steps (0 where the run just stops); h is its
        // offset from the edge at this pixel's centre (+ = into the pixel above).
        let h = mix(0.5 * signL, 0.5 * signR, t);
        weights.r = max(-h, 0.0); // line lies inside this pixel -> pull from above
        weights.g = max(h, 0.0);  // line lies inside the pixel above -> it pulls from this one
    }

    // Vertical edge on the left boundary -> blend horizontally.
    if (e.x > 0.5) {
        let dU = search_run(pos, vec2i(0i, -1i), 0i, dims, max_steps);
        let dD = search_run(pos, vec2i(0i, 1i), 0i, dims, max_steps);
        let signU = end_sign_v(pos, -1i, dU, dims);
        let signD = end_sign_v(pos, 1i, dD, dims);
        let run_len = f32(dU + dD + 1);
        let t = (f32(dU) + 0.5) / run_len;
        let h = mix(0.5 * signU, 0.5 * signD, t); // + = into the pixel to the left
        weights.b = max(-h, 0.0); // pull from the left
        weights.a = max(h, 0.0);  // the left pixel pulls from this one
    }

    // |h| <= 0.5, so each weight is at most 0.5 * blend_strength.
    return weights * uniforms.blend_strength;
}

@compute @workgroup_size(16, 16, 1)
fn blend_weights(
    @builtin(workgroup_id) wg: vec3u,
    @builtin(local_invocation_id) lid: vec3u,
    @builtin(local_invocation_index) lidx: u32) {
    if (lidx == 0u) {
        atomicStore(&worker_count, 0u);
    }
    workgroupBarrier();

    let dims = textureDimensions(edges_tex);
    let base = wg.xy * GROUP_W;
    let my = base + lid.xy;

    // Compaction scan: append this thread's local index if its pixel carries an edge.
    if (my.x < dims.x && my.y < dims.y) {
        let e = textureLoad(edges_tex, vec2i(my), 0i).xy;
        if ((e.x + e.y) > 0.0) {
            let slot = atomicAdd(&worker_count, 1u);
            worker_ids[slot] = lidx;
        }
    }
    workgroupBarrier();

    // The first `count` threads each take one listed edge pixel, so the expensive work runs in
    // contiguous lanes; the rest exit.
    let count = atomicLoad(&worker_count);
    if (lidx >= count) {
        return;
    }
    let wlidx = worker_ids[lidx];
    let pos = vec2i(base + vec2u(wlidx % GROUP_W, wlidx / GROUP_W));
    let weights = compute_weights(pos, dims);
    textureStore(blend_out, pos, weights);
}
