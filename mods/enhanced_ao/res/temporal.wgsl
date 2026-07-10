// Enhanced Ambient Occlusion - temporal accumulation pass.
//
// Reprojects the previous frame's accumulated AO into the current frame using the camera motion
// (reproject = prev proj_from_world * cur world_from_view) and blends it with the current
// denoised estimate. Because the occlusion pass advances its sampling noise every frame, each
// frame is a DIFFERENT noisy estimate, and the accumulation averages them into a clean, stable
// result (the accumulation is the primary noise reducer; the spatial denoiser softens what a
// single frame shows and is the standalone fallback when accumulation is off).
//
// Ghosting control, in order of authority:
//  - depth disocclusion rejection: the current point's EXPECTED depth in the previous view
//    (clip w) is compared against the depth the history stored at the reprojected texel; a large
//    mismatch means the reprojection crossed a silhouette, so the history belongs to a different
//    surface and is discarded. Comparing the expected previous depth (not the current depth)
//    keeps ordinary camera translation from tripping it.
//  - neighborhood clamp: history is clamped into the current local AO distribution
//    (mean +- k*sigma over 3x3), so stale values snap to the present instead of ghosting.
//  - velocity + content response: screen-space motion and a direct |history - current| mismatch
//    both shorten the accumulation so AO tracks geometry instead of dragging behind it.
//
// History format: rg32float = (accumulated AO, view depth / far plane).

struct Uniforms {
    projection: mat4x4f,
    inverse_projection: mat4x4f,
    reproject: mat4x4f,
    size: vec2f,        // AO chain size in pixels (may be half the render size)
    inv_size: vec2f,
    depth_scale: vec2f, // input depth snapshot pixels per chain pixel (1 or 2)
    effect_radius: f32, // fraction of view depth
    intensity: f32,
    slice_count: f32,
    steps_per_side: f32,
    thickness: f32,
    contrast: f32,
    temporal_alpha: f32,
    temporal_clamp_k: f32,
    inv_far: f32,
    radius_max: f32,     // screen-space radius cap, fraction of viewport height
    depth_bias: f32,     // self-occlusion bias, fraction toward the camera
    thick_fade: f32,     // occluder-thickness fade range, multiple of the view radius
    velocity_scale: f32, // accumulation shortening per pixel of screen motion
    content_thresh: f32, // content-mismatch response threshold scale (1 = default)
    disocc_tol: f32,     // disocclusion depth tolerance, fraction of depth
    black_point: f32,    // occlusion floor removed in the composite
    fade_start: f32,     // distance fade start, fraction of far plane
    fade_end: f32,       // distance fade end, fraction of far plane
    debug_view: u32,
    frame_index: u32,
    flags: u32, // bit 0 = temporal enabled, bit 1 = history valid, bit 2 = distance fade
    _pad0: f32,
}

@group(0) @binding(0) var ao_current: texture_2d<f32>;   // denoised current AO
@group(0) @binding(1) var history_in: texture_2d<f32>;   // (ao, depth) from the previous frame
@group(0) @binding(2) var current_depth: texture_2d<f32>; // preprocessed depth MIP 0 (raw reversed-Z)
@group(0) @binding(3) var history_out: texture_storage_2d<rg32float, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;

fn clamp_coordinates(pixel_coordinates: vec2<i32>) -> vec2<i32> {
    return clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
}

fn load_current(pixel_coordinates: vec2<i32>) -> f32 {
    return textureLoad(ao_current, clamp_coordinates(pixel_coordinates), 0i).r;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

// Manual bilinear history fetch (rg32float is unfilterable without optional features).
fn sample_history(uv: vec2f) -> vec2f {
    let coordinates = uv * uniforms.size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let p00 = clamp_coordinates(vec2<i32>(base));
    let p11 = clamp_coordinates(vec2<i32>(base) + 1i);
    let v00 = textureLoad(history_in, vec2<i32>(p00.x, p00.y), 0i).rg;
    let v10 = textureLoad(history_in, vec2<i32>(p11.x, p00.y), 0i).rg;
    let v01 = textureLoad(history_in, vec2<i32>(p00.x, p11.y), 0i).rg;
    let v11 = textureLoad(history_in, vec2<i32>(p11.x, p11.y), 0i).rg;
    return mix(mix(v00, v10, fraction.x), mix(v01, v11, fraction.x), fraction.y);
}

@compute
@workgroup_size(8, 8, 1)
fn temporal_accumulate(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let p = vec2<i32>(global_id.xy);
    if p.x >= i32(uniforms.size.x) || p.y >= i32(uniforms.size.y) {
        return;
    }
    let uv = (vec2f(p) + 0.5) * uniforms.inv_size;

    let raw_depth = textureLoad(current_depth, p, 0i).r;
    if raw_depth <= 0.0 {
        textureStore(history_out, p, vec4f(1.0, 1.0, 0.0, 0.0)); // sky: no occlusion, far depth
        return;
    }
    let view_pos = reconstruct_view_space_position(raw_depth, uv);
    let depth_norm = clamp(max(-view_pos.z, 0.0) * uniforms.inv_far, 0.0, 1.0);

    // Current estimate + local statistics. Sigma reflects the residual per-frame noise, sizing
    // the clamp band: converged static history sits inside it (trusted, keeps averaging) while
    // disocclusion/motion falls outside (clamped, snaps).
    let cur = load_current(p);
    var msum = 0.0;
    var m2 = 0.0;
    for (var dy = -1; dy <= 1; dy += 1) {
        for (var dx = -1; dx <= 1; dx += 1) {
            let s = load_current(p + vec2<i32>(dx, dy));
            msum += s;
            m2 += s * s;
        }
    }
    let nmean = msum / 9.0;
    let nsigma = max(sqrt(max(m2 / 9.0 - nmean * nmean, 0.0)), 0.015);

    var out_ao = cur;
    if (uniforms.flags & 2u) != 0u {
        let clip_prev = uniforms.reproject * vec4f(view_pos, 1.0);
        if clip_prev.w > 1.0e-4 {
            let ndc = clip_prev.xy / clip_prev.w;
            let prev_uv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            if prev_uv.x >= 0.0 && prev_uv.y >= 0.0 && prev_uv.x <= 1.0 && prev_uv.y <= 1.0 {
                let motion_px = length((prev_uv - uv) * uniforms.size);
                let hist = sample_history(prev_uv);

                // Depth disocclusion: clip w is the current point's view depth in the PREVIOUS
                // frame, in the same normalization the history stored its own depth.
                let expected_prev_d = clamp(clip_prev.w * uniforms.inv_far, 0.0, 1.0);
                let depth_tol = max(expected_prev_d * uniforms.disocc_tol, 0.002);
                let depth_reject =
                    smoothstep(depth_tol, depth_tol * 4.0, abs(expected_prev_d - hist.y));

                let hist_clamped = clamp(hist.x, nmean - uniforms.temporal_clamp_k * nsigma,
                    nmean + uniforms.temporal_clamp_k * nsigma);
                let content_motion = smoothstep(
                    0.12 * uniforms.content_thresh, 0.35 * uniforms.content_thresh, abs(hist.x - cur));
                let a = clamp(max(max(uniforms.temporal_alpha, depth_reject),
                                  max(motion_px * uniforms.velocity_scale, content_motion)),
                    0.0, 1.0);
                out_ao = mix(hist_clamped, cur, a);
            }
        }
    }
    textureStore(history_out, p, vec4f(clamp(out_ao, 0.0, 1.0), depth_norm, 0.0, 0.0));
}
