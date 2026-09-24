// VBAO (Visibility Bitmask Ambient Occlusion) - temporal accumulation pass.
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
//    (mean +- k*sigma over 3x3, k tightened under screen motion), so stale values snap to the
//    present instead of ghosting.
//  - velocity + content response: screen-space motion (capped, see below) and a sigma-normalised
//    outlier test of the history against the local mean both shorten the accumulation so AO
//    tracks geometry instead of dragging behind it.
//
// THE VELOCITY TERM IS CAPPED, AND THE CAP DEPENDS ON FRAME TIME (`velocity_cap`, host-set).
// `motion_px * velocity_scale` is pixels of screen motion PER FRAME, so for one and the same camera
// pan it is twice as large at 30 fps as at 60 fps and five times as large as at 144 fps. At the
// default response (0.1 per pixel) any ordinary pan at 30-60 fps drove the blend weight to 1.0 -
// that is, threw the whole history away every frame and displayed the raw single-frame estimate,
// whose sampling pattern advances every frame. At 144 Hz the eye fuses that into a mild shimmer;
// at 30-60 Hz it is plain flicker/boiling the moment the camera moves, and the lower the frame rate
// the worse it looks. (That was the 1.0.x "flickers in motion" report; the record is in
// docs/vbao.md "Temporal accumulation: history and diagnostics".) The response also fades with view
// depth - full up to velocity_range, gone at twice it - see the velocity_alpha block below.
// The host therefore measures the frame interval and hands down a ceiling that only lets the term
// reach a full reset when frames are short enough for per-frame noise to fuse (see
// kVelocityFusionFrameTime in mod.cpp). The disocclusion and content rejects are NOT capped: those
// are correctness terms, and a wrong-surface history must still be discarded outright.
//
// History format: rgba16float = (accumulated AO, view depth / far plane, octahedral view-space
// normal .xy). The normal is what lets the pass tell two history candidates apart - see
// "TWO HISTORY CANDIDATES" in temporal_accumulate.

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
    fade_start: f32,     // distance fade start, world units of view depth
    fade_end: f32,       // distance fade end, world units of view depth
    debug_view: u32,
    frame_index: u32,
    flags: u32, // bit 0 = temporal enabled, bit 1 = history valid, bit 2 = distance fade
    thick_dist_scale: f32,  // extra occluder thickness, fraction of the view-space radius
    inv_debug_depth: f32,   // debug depth view gradient scale (1 / world units)
    radius_far: f32,        // far effect radius (fraction of view depth); 0 disables the ramp
    radius_ramp_start: f32, // radius ramp band start, world units of view depth
    radius_ramp_end: f32,   // radius ramp band end, world units of view depth
    denoise_strength: f32,  // spatial denoise blend, 0 raw .. 1 fully blurred
    velocity_cap: f32,      // ceiling on the motion-response alpha (frame-time aware, host-set)
    velocity_range: f32,    // motion response fades out from this view depth to 2x it (world units; 0 = never)
    _pad2: f32,
}

// The AO chain runs at `size` (chain res, half the render size in Half Res). History, output and
// the raw depth snapshot are at the FULL render size (`size * depth_scale`). In half-res + temporal
// mode this pass is a temporal UPSAMPLER: each frame's jittered half-res estimate covers a
// different full-res pixel, and history reconstructs the full resolution over ~4 frames. Uncovered
// pixels (and fresh disocclusions) fall back to the depth-aware bilinear upscale so nothing is
// sparse. At full res depth_scale is 1: every pixel is "covered" and this reduces to the original
// per-pixel accumulation.
@group(0) @binding(0) var ao_current: texture_2d<f32>;        // denoised half-res AO (chain res)
@group(0) @binding(1) var history_in: texture_2d<f32>;        // full-res (ao, depth, oct normal) previous frame
@group(0) @binding(2) var preprocessed_depth: texture_2d<f32>; // half-res MIP0, for upscale weights
@group(0) @binding(3) var raw_depth: texture_2d<f32>;         // full-res raw reversed-Z snapshot
@group(0) @binding(4) var history_out: texture_storage_2d<rgba16float, write>; // full-res
@group(0) @binding(5) var<uniform> uniforms: Uniforms;
// The scene's authored view-space normal snapshot (full res, xyz*0.5+0.5, alpha 1 where valid) -
// the same texture vbao.wgsl shades with; here it is the second surface-identity test.
@group(0) @binding(6) var scene_normal: texture_2d<f32>;

// Geometric (face) normal of the full-res depth surface at pixel p, view space: 4 taps at +/-1,
// side-selected on the smaller depth step, flipped to face the camera - the same construction as
// vbao.wgsl's geometric_normal_view. Sky taps and degenerate cross products fall back.
fn full_res_view_pos(q: vec2<i32>, fs: vec2f) -> vec3f {
    let c = clamp(q, vec2<i32>(0i), vec2<i32>(fs) - 1i);
    let d = textureLoad(raw_depth, c, 0i).r;
    return reconstruct_view_space_position(d, (vec2f(c) + 0.5) / fs);
}

fn full_res_geometric_normal(p: vec2<i32>, centre: vec3f, fallback: vec3f) -> vec3f {
    let fs = full_size();
    let r = full_res_view_pos(p + vec2<i32>(1i, 0i), fs);
    let l = full_res_view_pos(p - vec2<i32>(1i, 0i), fs);
    let d = full_res_view_pos(p + vec2<i32>(0i, 1i), fs);
    let u = full_res_view_pos(p - vec2<i32>(0i, 1i), fs);
    let ddx = select(centre - l, r - centre, abs(r.z - centre.z) < abs(l.z - centre.z));
    let ddy = select(centre - u, d - centre, abs(d.z - centre.z) < abs(u.z - centre.z));
    let g = cross(ddy, ddx);
    let len = length(g);
    if !(len > 1.0e-12) {
        return fallback;
    }
    let gn = g / len;
    return select(gn, -gn, dot(gn, centre) > 0.0);
}

// Octahedral normal encoding, so the history carries a unit normal in two f16 channels.
fn oct_encode(n: vec3f) -> vec2f {
    let l1 = abs(n.x) + abs(n.y) + abs(n.z);
    var e = n.xy / max(l1, 1.0e-6);
    if n.z < 0.0 {
        e = (1.0 - abs(e.yx)) * select(vec2f(-1.0), vec2f(1.0), e >= vec2f(0.0));
    }
    return e;
}

fn oct_decode(e: vec2f) -> vec3f {
    var n = vec3f(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    let t = max(-n.z, 0.0);
    n.x += select(t, -t, n.x >= 0.0);
    n.y += select(t, -t, n.y >= 0.0);
    return normalize(n);
}

fn full_size() -> vec2f {
    return uniforms.size * uniforms.depth_scale;
}

fn clamp_half(pixel_coordinates: vec2<i32>) -> vec2<i32> {
    return clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
}

fn load_ao(pixel_coordinates: vec2<i32>) -> f32 {
    return textureLoad(ao_current, clamp_half(pixel_coordinates), 0i).r;
}

fn load_preproc_depth(pixel_coordinates: vec2<i32>) -> f32 {
    return textureLoad(preprocessed_depth, clamp_half(pixel_coordinates), 0i).r;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

// 4-phase sub-pixel jitter, matching preprocess_depth.wgsl / vbao.wgsl.
fn taau_jitter() -> vec2<i32> {
    if uniforms.depth_scale.x < 1.5 || (uniforms.flags & 1u) == 0u {
        return vec2<i32>(0i, 0i);
    }
    switch uniforms.frame_index & 3u {
        case 0u: { return vec2<i32>(0i, 0i); }
        case 1u: { return vec2<i32>(1i, 1i); }
        case 2u: { return vec2<i32>(1i, 0i); }
        default: { return vec2<i32>(0i, 1i); }
    }
}

// Depth-aware bilinear upscale of the half-res AO (port of composite.wgsl sample_visibility): each
// bilinear tap is reweighted by how well its half-res depth agrees with this full-res pixel's depth,
// so the fill does not bleed AO across silhouettes. Used for uncovered pixels and disocclusions.
fn upscale_ao(uv: vec2f, reference_depth: f32) -> f32 {
    let half_size = uniforms.size;
    let coordinates = uv * half_size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let maxc = vec2<i32>(half_size) - 1i;
    let p00 = clamp(vec2<i32>(base), vec2<i32>(0i), maxc);
    let p11 = clamp(vec2<i32>(base) + 1i, vec2<i32>(0i), maxc);
    let depth_tolerance = max(reference_depth * 0.05, 1.0e-6);

    var sum = 0.0;
    var weight_sum = 0.0;
    for (var i = 0; i < 4; i += 1) {
        let tap = vec2<i32>(select(p00.x, p11.x, (i & 1) != 0), select(p00.y, p11.y, (i & 2) != 0));
        let bw = select(1.0 - fraction.x, fraction.x, (i & 1) != 0) *
            select(1.0 - fraction.y, fraction.y, (i & 2) != 0);
        let dz = (load_preproc_depth(tap) - reference_depth) / depth_tolerance;
        let w = bw * exp2(-dz * dz);
        sum += w * load_ao(tap);
        weight_sum += w;
    }
    if weight_sum < 1.0e-4 {
        return load_ao(clamp(vec2<i32>(round(coordinates)), vec2<i32>(0i), maxc));
    }
    return sum / weight_sum;
}

// Manual bilinear history fetch at full res (a plain textureLoad blend; no sampler needed).
fn sample_history(uv: vec2f) -> vec4f {
    let fs = full_size();
    let coordinates = uv * fs - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let maxc = vec2<i32>(fs) - 1i;
    let p00 = clamp(vec2<i32>(base), vec2<i32>(0i), maxc);
    let p11 = clamp(vec2<i32>(base) + 1i, vec2<i32>(0i), maxc);
    let v00 = textureLoad(history_in, vec2<i32>(p00.x, p00.y), 0i);
    let v10 = textureLoad(history_in, vec2<i32>(p11.x, p00.y), 0i);
    let v01 = textureLoad(history_in, vec2<i32>(p00.x, p11.y), 0i);
    let v11 = textureLoad(history_in, vec2<i32>(p11.x, p11.y), 0i);
    return mix(mix(v00, v10, fraction.x), mix(v01, v11, fraction.x), fraction.y);
}

@compute
@workgroup_size(8, 8, 1)
fn temporal_accumulate(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let fs = full_size();
    let p = vec2<i32>(global_id.xy);
    if p.x >= i32(fs.x) || p.y >= i32(fs.y) {
        return;
    }
    let uv = (vec2f(p) + 0.5) / fs;

    let rd = textureLoad(raw_depth, clamp(p, vec2<i32>(0i), vec2<i32>(fs) - 1i), 0i).r;
    if rd <= 0.0 {
        textureStore(history_out, p, vec4f(1.0, 1.0, 0.0, 0.0)); // sky: no occlusion, far depth, no normal
        return;
    }
    let view_pos = reconstruct_view_space_position(rd, uv);
    let depth_norm = clamp(max(-view_pos.z, 0.0) * uniforms.inv_far, 0.0, 1.0);
    let scene_n_raw = textureLoad(scene_normal, clamp(p, vec2<i32>(0i), vec2<i32>(fs) - 1i), 0i);
    let has_n = scene_n_raw.w >= 0.5;
    let n_auth = select(vec3f(0.0, 0.0, 1.0), normalize(scene_n_raw.xyz * 2.0 - 1.0), has_n);
    // Same trust rule as vbao.wgsl: an authored normal that contradicts the depth geometry (debug
    // view 6 red/white) is replaced by the geometric one, so history identity is judged on the
    // surface that is really there. An unstable wrong normal would otherwise reject history from
    // frame to frame and read as flicker.
    let geo_n = full_res_geometric_normal(p, view_pos, n_auth);
    let n_cur = normalize(mix(geo_n, n_auth, smoothstep(0.6, 0.8, dot(n_auth, geo_n))));
    let n_oct = select(vec2f(0.0), oct_encode(n_cur), has_n);

    let taau = uniforms.depth_scale.x >= 1.5;
    let hc = select(p, p / vec2<i32>(2i), taau); // half-res texel this pixel maps to
    let jit = taau_jitter();
    // "Covered" = this pixel has a genuine fresh half-res sample this frame (always, at full res;
    // the jittered pixel, in half-res upsampling). Uncovered pixels carry history forward and only
    // fall back to the spatial upscale when there is no valid history to keep.
    let covered = !taau || ((p.x & 1i) == jit.x && (p.y & 1i) == jit.y);
    var cur: f32;
    if covered {
        cur = load_ao(hc);
    } else {
        cur = upscale_ao(uv, rd);
    }

    // Local statistics from the half-res AO neighborhood size the clamp band (applied only to
    // covered pixels, where `cur` is a real sample; clamping an uncovered pixel against the coarse
    // half-res distribution would erase the very detail the upsampler is reconstructing).
    var msum = 0.0;
    var m2 = 0.0;
    for (var dy = -1; dy <= 1; dy += 1) {
        for (var dx = -1; dx <= 1; dx += 1) {
            let s = load_ao(hc + vec2<i32>(dx, dy));
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
                let motion_px = length((prev_uv - uv) * fs);

                // Surface-identity mismatch of a history sample, in units of tolerance (1 = at
                // tolerance, 3 = full reject): the depth term, relative to the point's depth
                // (>= 1.5%), and the normal term, (1 - cos) over 0.15 (~30 degrees = 1).
                //
                // The depth tolerance is RELATIVE to depth on purpose. It used to have a floor of
                // 0.002 of the far plane, and TP's far plane is per-stage and huge: on a
                // 200000-unit stage that floor was 400 world units, larger than Link, so ground he
                // had just vacated matched his body's stored depth and kept his AO as a full-body
                // trail. Relative tolerance follows the scene: 15 units at 1000, 150 at 10000,
                // which still admits the same surface at grazing angles while separating a
                // character from the ground behind it.
                let rel_tol = max(uniforms.disocc_tol, 0.015);

                // TWO HISTORY CANDIDATES. There are no per-object motion vectors: the reprojection
                // is the CAMERA's, so for anything that moves in the world it is wrong. The case
                // that matters is Link: the camera follows him, so he is nearly static on screen
                // while the world moves, and the camera-reprojected history for a pixel on his
                // body is a NEIGHBOURING part of his body (the world's motion away). Same depth,
                // similar AO, so nothing rejected it, and his AO smeared along the world's motion
                // as a soft trail. Candidate B is the history at this pixel's OWN screen position
                // (no reprojection), which is exactly right for a screen-static object. Each
                // candidate is scored on how well it is the same surface as the current pixel
                // (depth AND normal - depth alone cannot tell two parts of a body apart, the
                // normal can), the camera candidate stays preferred, and B is taken only when it
                // is clearly the better match. Static world geometry under camera motion scores
                // A near zero and keeps it; on Link's curved parts B wins and the smear stops;
                // on his flattest regions the two tie, A stays, and a smear of near-identical AO
                // values is invisible. Where neither candidate is the same surface, the chosen
                // one's mismatch drives the disocclusion reject as before.
                // Depth of the current point in the previous view (clip w), in the history's
                // normalization; candidate B is compared against the current depth, since a
                // screen-static object barely changes depth between frames.
                let expected_prev_d = clamp(clip_prev.w * uniforms.inv_far, 0.0, 1.0);
                let hist_a = sample_history(prev_uv);
                let mis_a = abs(expected_prev_d - hist_a.y) / max(expected_prev_d * rel_tol, 1.0e-6);
                let nrm_a = select(0.0, (1.0 - dot(n_cur, oct_decode(hist_a.zw))) / 0.15, has_n);
                var hist = hist_a;
                var mismatch = max(mis_a, nrm_a);
                if motion_px > 0.5 {
                    let hist_b = sample_history(uv); // exact texel: uv is this pixel's centre
                    let mis_b = abs(depth_norm - hist_b.y) / max(depth_norm * rel_tol, 1.0e-6);
                    let nrm_b = select(0.0, (1.0 - dot(n_cur, oct_decode(hist_b.zw))) / 0.15, has_n);
                    if (mis_b + nrm_b) + 0.5 < (mis_a + nrm_a) {
                        hist = hist_b;
                        mismatch = max(mis_b, nrm_b);
                    }
                }
                let depth_reject = smoothstep(1.0, 3.0, mismatch);

                // Covered pixels accumulate the fresh sample (clamp + content-reject guard against
                // ghosting); uncovered pixels keep history unless camera motion / disocclusion
                // forces them toward the spatial fill.
                //
                // GHOSTING, with the velocity term no longer resetting history in motion, comes
                // from occluders that moved in the WORLD: Link's contact shadow stays on the ground
                // he just left, because that ground reprojects correctly and its stored AO is
                // simply stale. The depth test cannot see it (same surface). Two guards handle it:
                //  - the clamp bounds the stale value to the current local distribution, and it
                //    tightens under screen motion (k * 0.6 at >= 16 px/frame), where stale
                //    occluders are likeliest;
                //  - the content reject is an OUTLIER test in sigma units against the 3x3 MEAN,
                //    not an absolute difference against the noisy single-frame sample: a trail sits
                //    several sigma from the unoccluded ground around it and is discarded within a
                //    frame or two, while in-distribution history (|dev| < 1 sigma) keeps
                //    accumulating, which is what keeps the noise averaging that removed the flicker.
                let motion_tighten = mix(1.0, 0.6, clamp(motion_px / 16.0, 0.0, 1.0));
                let k_eff = uniforms.temporal_clamp_k * motion_tighten;
                let hist_used = select(hist.x,
                    clamp(hist.x, nmean - k_eff * nsigma, nmean + k_eff * nsigma),
                    covered);
                let base_alpha = select(0.0, uniforms.temporal_alpha, covered);
                let hist_dev = abs(hist.x - nmean) / max(nsigma, 0.02);
                let content_motion = select(0.0,
                    smoothstep(1.0 * uniforms.content_thresh, 2.5 * uniforms.content_thresh,
                        hist_dev),
                    covered);
                // Velocity response, ceilinged by the frame-time-aware cap (see the header) and
                // faded out with VIEW DEPTH: full up to velocity_range world units, gone at twice
                // it. The raw single-frame estimate is dense and clean close to the camera and
                // sparse at distance (constant pixel radius, growing world radius), so a short
                // accumulation is free on a character and ruinous on a far landmark - which is
                // exactly what the field showed at a high response: Link full, distant AO sparse.
                let view_depth = max(-view_pos.z, 0.0);
                let range_w = select(1.0,
                    1.0 - smoothstep(uniforms.velocity_range, uniforms.velocity_range * 2.0, view_depth),
                    uniforms.velocity_range > 0.0);
                let velocity_alpha =
                    min(motion_px * uniforms.velocity_scale * range_w, uniforms.velocity_cap);
                let a = clamp(max(max(base_alpha, depth_reject),
                                  max(velocity_alpha, content_motion)),
                    0.0, 1.0);
                out_ao = mix(hist_used, cur, a);
            }
        }
    }
    textureStore(history_out, p, vec4f(clamp(out_ao, 0.0, 1.0), depth_norm, n_oct.x, n_oct.y));
}
