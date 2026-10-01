// VBAO temporal accumulation (compute, full render resolution).
//
// Blends this frame's denoised AO estimate into a camera-reprojected history
// (reproject = previous proj_from_world * current world_from_view). The occlusion pass changes its
// sampling noise every frame, so this averaging is the main noise reducer; with accumulation off
// the spatial denoiser is the only filter.
//
// History: rgba16float = (accumulated AO, view depth / far plane, octahedral view-space normal .xy).
//
// Per pixel:
//  - Two history candidates, camera-reprojected and un-reprojected, each scored on depth and normal
//    agreement with the current surface (a stand-in for per-object motion vectors, which a
//    service-only mod cannot have). The chosen candidate's mismatch drives a disocclusion reject.
//  - Covered pixels clamp history to the 3x3 mean +- k * sigma (k tightened under screen motion).
//  - Blend weight = max(1 / Temporal Frames, disocclusion reject, velocity term, outlier term).
//    The velocity term (screen motion in px/frame * velocity_scale) fades out with view depth
//    (velocity_range) and is capped by velocity_cap, which the host derives from frame time
//    (kVelocityFusionFrameTime in mod.cpp). The disocclusion and outlier terms are not capped:
//    they detect history that is wrong, not merely old.
//
// Design and field history: docs/vbao.md "Temporal accumulation".

// Mirrors AoUniforms in src/mod.cpp and the copies in the other shaders, byte for byte.
struct Uniforms {
    projection: mat4x4f,          // proj_from_view
    inverse_projection: mat4x4f,  // view_from_proj
    reproject: mat4x4f,           // current view -> previous frame's clip space
    size: vec2f,        // AO chain size in pixels (half the render size in Half Res)
    inv_size: vec2f,
    depth_scale: vec2f, // render (snapshot) pixels per chain pixel: 1 or 2
    effect_radius: f32, // near radius, fraction of view depth
    intensity: f32,     // composite strength, 1 = 100%
    slice_count: f32,
    steps_per_side: f32,
    thickness: f32,     // base occluder thickness multiplier
    contrast: f32,      // exponent applied to visibility in the composite
    temporal_alpha: f32,   // base history blend weight, 1 / Temporal Frames
    temporal_clamp_k: f32, // history clamp half-width, in sigmas of the 3x3 neighbourhood
    inv_far: f32,          // 1 / far plane; normalises the depth stored in the history
    radius_max: f32,     // screen-space radius cap, fraction of viewport height
    depth_bias: f32,     // self-occlusion bias: view position scaled by (1 - depth_bias)
    thick_fade: f32,     // occluder-thickness fade range, multiple of the view radius
    velocity_scale: f32, // velocity blend weight per pixel/frame of screen motion
    content_thresh: f32, // outlier-test threshold scale (1 = 1..2.5 sigma)
    disocc_tol: f32,     // disocclusion depth tolerance, fraction of depth (shader floor 0.015)
    black_point: f32,    // occlusion floor removed in the composite
    fade_start: f32,     // distance fade start, world units of view depth
    fade_end: f32,       // distance fade end, world units of view depth
    debug_view: u32,
    frame_index: u32,    // advances per frame while accumulating, else 0
    flags: u32, // bit 0 = temporal enabled, bit 1 = history valid, bit 2 = distance fade
    thick_dist_scale: f32,  // extra occluder thickness, fraction of the view-space radius
    inv_debug_depth: f32,   // debug depth view gradient scale (1 / world units)
    radius_far: f32,        // far effect radius (fraction of view depth); 0 disables the ramp
    radius_ramp_start: f32, // radius ramp band start, world units of view depth
    radius_ramp_end: f32,   // radius ramp band end, world units of view depth
    denoise_strength: f32,  // spatial denoise blend, 0 raw .. 1 fully blurred
    velocity_cap: f32,      // ceiling on the velocity blend weight (frame-time aware, host-set)
    velocity_range: f32,    // velocity term fades over [this, 2x] view depth, world units; 0 = off
    _pad2: f32,
}

// The AO chain runs at `size` (half the render size in Half Res). History, output and the raw
// depth snapshot are at the full render size (`size * depth_scale`). In Half Res this pass is a
// temporal upsampler: each frame's jittered half-res estimate covers one pixel of every 2x2 block,
// and the history reconstructs full resolution over ~4 frames. Pixels without valid history fall
// back to the depth-aware bilinear upscale. At full res every pixel is covered every frame.
@group(0) @binding(0) var ao_current: texture_2d<f32>;        // denoised half-res AO (chain res)
@group(0) @binding(1) var history_in: texture_2d<f32>;        // full-res (ao, depth, oct normal) previous frame
@group(0) @binding(2) var preprocessed_depth: texture_2d<f32>; // half-res MIP0, for upscale weights
@group(0) @binding(3) var raw_depth: texture_2d<f32>;         // full-res raw reversed-Z snapshot
@group(0) @binding(4) var history_out: texture_storage_2d<rgba16float, write>; // full-res
@group(0) @binding(5) var<uniform> uniforms: Uniforms;
// The scene's authored view-space normal snapshot (full res, xyz*0.5+0.5, alpha 1 where valid),
// the same texture vbao.wgsl shades with; here it is the second surface-identity test.
@group(0) @binding(6) var scene_normal: texture_2d<f32>;

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
    let n_cur = select(vec3f(0.0, 0.0, 1.0), normalize(scene_n_raw.xyz * 2.0 - 1.0), has_n);
    let n_oct = select(vec2f(0.0), oct_encode(n_cur), has_n);

    let taau = uniforms.depth_scale.x >= 1.5;
    let hc = select(p, p / vec2<i32>(2i), taau); // half-res texel this pixel maps to
    let jit = taau_jitter();
    // "Covered" = this pixel has a fresh sample this frame (every pixel at full res; the jittered
    // pixel of each 2x2 block in Half Res). Uncovered pixels carry history forward and move toward
    // the spatial upscale only under disocclusion or screen motion, or when there is no history.
    let covered = !taau || ((p.x & 1i) == jit.x && (p.y & 1i) == jit.y);
    var cur: f32;
    if covered {
        cur = load_ao(hc);
    } else {
        cur = upscale_ao(uv, rd);
    }

    // 3x3 statistics of the chain-resolution AO, for the clamp and the outlier test. Both apply
    // only to covered pixels: clamping an uncovered pixel to the coarse half-res distribution
    // would erase the detail the upsampler is reconstructing.
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
                // (at least 1.5%), and the normal term, (1 - cos) / 0.15 (about 32 degrees = 1).
                //
                // Important: keep the depth tolerance relative to depth, never a fraction of the
                // far plane. The far plane is per-stage and huge, so a far-plane floor is hundreds
                // of world units, wider than a character, and lets the character's AO trail over
                // the ground behind it. 1.5% is 15 units at 1000 and 150 at 10000.
                let rel_tol = max(uniforms.disocc_tol, 0.015);

                // Two history candidates. The reprojection only follows the camera, so it is wrong
                // for anything that moves in the world. The case that matters is a character the
                // camera follows: nearly static on screen, so the camera-reprojected history for a
                // pixel on his body is a neighbouring part of the same body, with the same depth,
                // and his AO smears along the world's motion. Candidate A is the camera-reprojected
                // history; candidate B is the history at this pixel's own screen position, which
                // is right for a screen-static object. Both are scored on depth and normal (the
                // normal tells two parts of one body apart where depth cannot). A is preferred; B
                // is taken only when its summed score is lower by more than 0.5 and the camera
                // moved this pixel by more than half a pixel. The chosen candidate's mismatch
                // drives the disocclusion reject.
                //
                // A is compared at the current point's depth in the previous view (clip w), in the
                // history's normalisation; B against the current depth, since a screen-static
                // object barely changes depth between frames.
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

                // Covered pixels accumulate the fresh sample; uncovered pixels keep history unless
                // disocclusion or screen motion pushes them toward the spatial fill.
                //
                // The remaining ghosting source is an occluder that moved in the world, such as a
                // character's contact shadow left on the ground he walked off: that ground
                // reprojects correctly, so the depth test cannot see that its AO is stale. Two
                // guards handle it, for covered pixels:
                //  - the clamp bounds history to the current 3x3 distribution, tightened to
                //    k * 0.6 at 16 px/frame of motion or more;
                //  - the outlier test measures history against the 3x3 mean in sigma units, not
                //    against the noisy single-frame sample: a trail sits several sigma away and is
                //    dropped within a frame or two, while history within ~1 sigma keeps
                //    accumulating.
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
                // Velocity term, capped by the frame-time-aware velocity_cap (see the header) and
                // faded out with view depth: full up to velocity_range world units, gone at twice
                // that. The single-frame estimate is clean close to the camera and sparse far away,
                // so a short accumulation costs nothing on a nearby character and visibly thins
                // distant AO.
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
