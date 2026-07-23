// SMAA — pass 2: blending-weight calculation (compute, CMAA2-style compacted).
//
// For every edge pixel, reconstruct the aliased silhouette as a straight line over the run of
// collinear edge pixels and compute how much of the pixel the line cuts away — the blend weight.
// This is the expensive pass (per-edge-pixel searches), so it uses the CMAA2 optimization
// (Intel, 2018): within each 16x16 workgroup, edge pixels are compacted into contiguous threads
// via a groupshared list, so the sparse, thin edges run in fully-occupied warps instead of being
// scattered one-per-warp. Non-edge pixels return immediately after the compaction scan.
//
// This first version handles ORTHOGONAL patterns (horizontal / vertical edges and the shallow
// stairs they form). Diagonal-specific search and corner rounding are deferred; the neighborhood
// pass still smooths diagonals via the orthogonal weights, just less precisely at ~45 degrees.
//
// Weight packing (matches neighborhood_blend.wgsl):
//   BlendTex.r = this pixel pulls from the pixel ABOVE  (its own top edge)
//   BlendTex.g = the pixel above pulls from this pixel   (its own top edge, other side)
//   BlendTex.b = this pixel pulls from the pixel to the LEFT (its own left edge)
//   BlendTex.a = the pixel to the left pulls from this pixel (its own left edge, other side)

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

// Walk from p along `step` while the edge channel selected by `chan` (0 = left/vertical run,
// 1 = top/horizontal run) keeps continuing. Returns the number of continuation pixels (capped).
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

// Sign of the silhouette turn at a horizontal run's end: +1 if the edge continues one row up just
// past the end, -1 if one row down, 0 if it simply stops.
fn end_sign_h(p: vec2i, dir: i32, d: i32, dims: vec2u) -> f32 {
    let beyond = p + vec2i(dir * (d + 1), 0i);
    if (load_edges(beyond + vec2i(0i, -1i), dims).y > 0.5) { return 1.0; }
    if (load_edges(beyond + vec2i(0i, 1i), dims).y > 0.5) { return -1.0; }
    return 0.0;
}

// Sign of the silhouette turn at a vertical run's end: +1 if it continues one column left just past
// the end (bulging toward the left pixel), -1 if one column right, 0 if it stops.
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
        let t = (f32(dL) + 0.5) / run_len;
        // Reconstructed line height at this pixel's centre, signed (+ = toward the pixel above).
        let h = mix(0.5 * signL, 0.5 * signR, t);
        weights.r = max(-h, 0.0); // line dips into this pixel -> pull from above
        weights.g = max(h, 0.0);  // line bulges up -> the above pixel pulls from this one
    }

    // Vertical edge on the left boundary -> blend horizontally.
    if (e.x > 0.5) {
        let dU = search_run(pos, vec2i(0i, -1i), 0i, dims, max_steps);
        let dD = search_run(pos, vec2i(0i, 1i), 0i, dims, max_steps);
        let signU = end_sign_v(pos, -1i, dU, dims);
        let signD = end_sign_v(pos, 1i, dD, dims);
        let run_len = f32(dU + dD + 1);
        let t = (f32(dU) + 0.5) / run_len;
        let h = mix(0.5 * signU, 0.5 * signD, t); // + = toward the pixel to the left
        weights.b = max(-h, 0.0); // pull from the left
        weights.a = max(h, 0.0);  // the left pixel pulls from this one
    }

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

    // Only the first `count` threads do the expensive work, now packed into contiguous lanes.
    let count = atomicLoad(&worker_count);
    if (lidx >= count) {
        return;
    }
    let wlidx = worker_ids[lidx];
    let pos = vec2i(base + vec2u(wlidx % GROUP_W, wlidx / GROUP_W));
    let weights = compute_weights(pos, dims);
    textureStore(blend_out, pos, weights);
}
