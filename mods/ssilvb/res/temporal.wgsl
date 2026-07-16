// SSILVB - temporal accumulation pass (rgba: GI.rgb + AO, plus a split depth history plane).
//
// Structurally identical to VBAO's temporal.wgsl (reprojection via the camera motion,
// expected-prev-depth disocclusion rejection, mean +- k*sigma neighborhood clamp, velocity and
// content response, and the 4-phase half-res jittered temporal UPSAMPLER), widened from a scalar
// AO channel to vec4. Because the rgba16float history has no spare channel for the stored view
// depth, the depth rides a second ping-ponged r32float plane (history_depth_*) - VBAO packs it
// in .g of an rg32float instead; the math is unchanged.
//
// GI-specific deltas from VBAO:
//  - the neighborhood clamp is applied PER CHANNEL (vec4 mean/sigma over the same 3x3 window):
//    colored light varies more per frame than scalar occlusion, and clamping channels jointly
//    would let a hue shift hide inside an unchanged luminance.
//  - the content-mismatch response uses the max channel delta (GI channels are linear [0,1]
//    like AO, so the same thresholds apply).
//
// History: history_color = rgba16float (GI.rgb linear, AO), history_depth = r32float
// (view depth / far plane).

struct Uniforms {
    projection: mat4x4f,
    inverse_projection: mat4x4f,
    reproject: mat4x4f,
    view_from_world: mat4x4f,  // rotates the Depth to Normal provider's world normal into view
    size: vec2f,        // chain size in pixels (may be half the render size)
    inv_size: vec2f,
    depth_scale: vec2f, // input snapshot pixels per chain pixel (1 or 2)
    effect_radius: f32, // fraction of view depth
    intensity: f32,     // AO strength (composite)
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
    flags: u32, // bit 0 temporal, bit 1 history valid, bit 2 distance fade,
                // bit 3 GI enabled, bit 4 AO apply, bit 5 white bounce proxy
    thick_dist_scale: f32,  // extra occluder thickness, fraction of the view-space radius
    radius_far: f32,        // far effect radius (fraction of view depth); 0 disables the ramp
    radius_ramp_start: f32, // radius ramp band start, world units of view depth
    radius_ramp_end: f32,   // radius ramp band end, world units of view depth
    denoise_strength: f32,  // spatial denoise blend, 0 raw .. 1 fully blurred
    gi_intensity: f32,      // indirect bounce strength (composite)
    chroma_lift: f32,       // receiver albedo proxy: 0 = raw scene color .. 1 = full chroma norm
    emissive_boost: f32,     // emissive-delta bounce gain (fire, fairies, glows)
    emissive_threshold: f32, // linear floor for the emissive delta extract
}

// The GI chain runs at `size` (chain res, half the render size in Half Res). History, output and
// the raw depth snapshot are at the FULL render size (`size * depth_scale`). In half-res +
// temporal mode this pass is a temporal UPSAMPLER: each frame's jittered half-res estimate covers
// a different full-res pixel, and history reconstructs the full resolution over ~4 frames.
// Uncovered pixels (and fresh disocclusions) fall back to the depth-aware bilinear upscale so
// nothing is sparse. At full res depth_scale is 1: every pixel is "covered" and this reduces to
// the original per-pixel accumulation.
@group(0) @binding(0) var gi_current: texture_2d<f32>;         // denoised (GI, AO), chain res
@group(0) @binding(1) var history_color_in: texture_2d<f32>;   // full-res previous accumulation
@group(0) @binding(2) var preprocessed_depth: texture_2d<f32>; // chain-res MIP0, upscale weights
@group(0) @binding(3) var raw_depth: texture_2d<f32>;          // full-res raw reversed-Z snapshot
@group(0) @binding(4) var history_color_out: texture_storage_2d<rgba16float, write>; // full-res
@group(0) @binding(5) var<uniform> uniforms: Uniforms;
@group(0) @binding(6) var history_depth_in: texture_2d<f32>;   // full-res stored view depth
@group(0) @binding(7) var history_depth_out: texture_storage_2d<r32float, write>; // full-res

fn full_size() -> vec2f {
    return uniforms.size * uniforms.depth_scale;
}

fn clamp_half(pixel_coordinates: vec2<i32>) -> vec2<i32> {
    return clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
}

fn load_gi(pixel_coordinates: vec2<i32>) -> vec4f {
    return textureLoad(gi_current, clamp_half(pixel_coordinates), 0i);
}

fn load_preproc_depth(pixel_coordinates: vec2<i32>) -> f32 {
    return textureLoad(preprocessed_depth, clamp_half(pixel_coordinates), 0i).r;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

// 4-phase sub-pixel jitter, matching preprocess_depth.wgsl / ssilvb.wgsl.
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

// Depth-aware bilinear upscale of the half-res estimate (port of composite.wgsl sample_gi): each
// bilinear tap is reweighted by how well its half-res depth agrees with this full-res pixel's
// depth, so the fill does not bleed light or occlusion across silhouettes. Used for uncovered
// pixels and disocclusions.
fn upscale_gi(uv: vec2f, reference_depth: f32) -> vec4f {
    let half_size = uniforms.size;
    let coordinates = uv * half_size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let maxc = vec2<i32>(half_size) - 1i;
    let p00 = clamp(vec2<i32>(base), vec2<i32>(0i), maxc);
    let p11 = clamp(vec2<i32>(base) + 1i, vec2<i32>(0i), maxc);
    let depth_tolerance = max(reference_depth * 0.05, 1.0e-6);

    var sum = vec4f(0.0);
    var weight_sum = 0.0;
    for (var i = 0; i < 4; i += 1) {
        let tap = vec2<i32>(select(p00.x, p11.x, (i & 1) != 0), select(p00.y, p11.y, (i & 2) != 0));
        let bw = select(1.0 - fraction.x, fraction.x, (i & 1) != 0) *
            select(1.0 - fraction.y, fraction.y, (i & 2) != 0);
        let dz = (load_preproc_depth(tap) - reference_depth) / depth_tolerance;
        let w = bw * exp2(-dz * dz);
        sum += w * load_gi(tap);
        weight_sum += w;
    }
    if weight_sum < 1.0e-4 {
        return load_gi(clamp(vec2<i32>(round(coordinates)), vec2<i32>(0i), maxc));
    }
    return sum / weight_sum;
}

// Manual bilinear history fetches at full res (rgba16float/r32float are unfilterable without
// optional features).
fn sample_history_color(uv: vec2f) -> vec4f {
    let fs = full_size();
    let coordinates = uv * fs - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let maxc = vec2<i32>(fs) - 1i;
    let p00 = clamp(vec2<i32>(base), vec2<i32>(0i), maxc);
    let p11 = clamp(vec2<i32>(base) + 1i, vec2<i32>(0i), maxc);
    let v00 = textureLoad(history_color_in, vec2<i32>(p00.x, p00.y), 0i);
    let v10 = textureLoad(history_color_in, vec2<i32>(p11.x, p00.y), 0i);
    let v01 = textureLoad(history_color_in, vec2<i32>(p00.x, p11.y), 0i);
    let v11 = textureLoad(history_color_in, vec2<i32>(p11.x, p11.y), 0i);
    return mix(mix(v00, v10, fraction.x), mix(v01, v11, fraction.x), fraction.y);
}

fn sample_history_depth(uv: vec2f) -> f32 {
    let fs = full_size();
    let coordinates = uv * fs - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let maxc = vec2<i32>(fs) - 1i;
    let p00 = clamp(vec2<i32>(base), vec2<i32>(0i), maxc);
    let p11 = clamp(vec2<i32>(base) + 1i, vec2<i32>(0i), maxc);
    let v00 = textureLoad(history_depth_in, vec2<i32>(p00.x, p00.y), 0i).r;
    let v10 = textureLoad(history_depth_in, vec2<i32>(p11.x, p00.y), 0i).r;
    let v01 = textureLoad(history_depth_in, vec2<i32>(p00.x, p11.y), 0i).r;
    let v11 = textureLoad(history_depth_in, vec2<i32>(p11.x, p11.y), 0i).r;
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
        // Sky: no bounce, no occlusion, far depth.
        textureStore(history_color_out, p, vec4f(0.0, 0.0, 0.0, 1.0));
        textureStore(history_depth_out, p, vec4f(1.0, 0.0, 0.0, 0.0));
        return;
    }
    let view_pos = reconstruct_view_space_position(rd, uv);
    let depth_norm = clamp(max(-view_pos.z, 0.0) * uniforms.inv_far, 0.0, 1.0);

    let taau = uniforms.depth_scale.x >= 1.5;
    let hc = select(p, p / vec2<i32>(2i), taau); // half-res texel this pixel maps to
    let jit = taau_jitter();
    // "Covered" = this pixel has a genuine fresh half-res sample this frame (always, at full res;
    // the jittered pixel, in half-res upsampling). Uncovered pixels carry history forward and only
    // fall back to the spatial upscale when there is no valid history to keep.
    let covered = !taau || ((p.x & 1i) == jit.x && (p.y & 1i) == jit.y);
    var cur: vec4f;
    if covered {
        cur = load_gi(hc);
    } else {
        cur = upscale_gi(uv, rd);
    }

    // Local per-channel statistics from the chain-res neighborhood size the clamp band (applied
    // only to covered pixels, where `cur` is a real sample; clamping an uncovered pixel against
    // the coarse half-res distribution would erase the very detail the upsampler reconstructs).
    var msum = vec4f(0.0);
    var m2 = vec4f(0.0);
    for (var dy = -1; dy <= 1; dy += 1) {
        for (var dx = -1; dx <= 1; dx += 1) {
            let s = load_gi(hc + vec2<i32>(dx, dy));
            msum += s;
            m2 += s * s;
        }
    }
    let nmean = msum / 9.0;
    let nsigma = max(sqrt(max(m2 / 9.0 - nmean * nmean, vec4f(0.0))), vec4f(0.015));

    var out_value = cur;
    if (uniforms.flags & 2u) != 0u {
        let clip_prev = uniforms.reproject * vec4f(view_pos, 1.0);
        if clip_prev.w > 1.0e-4 {
            let ndc = clip_prev.xy / clip_prev.w;
            let prev_uv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            if prev_uv.x >= 0.0 && prev_uv.y >= 0.0 && prev_uv.x <= 1.0 && prev_uv.y <= 1.0 {
                let motion_px = length((prev_uv - uv) * fs);
                let hist = sample_history_color(prev_uv);
                let hist_depth = sample_history_depth(prev_uv);

                // Depth disocclusion: clip w is the current point's view depth in the PREVIOUS
                // frame, in the same normalization the history stored its own depth.
                let expected_prev_d = clamp(clip_prev.w * uniforms.inv_far, 0.0, 1.0);
                let depth_tol = max(expected_prev_d * uniforms.disocc_tol, 0.002);
                let depth_reject =
                    smoothstep(depth_tol, depth_tol * 4.0, abs(expected_prev_d - hist_depth));

                // Covered pixels accumulate the fresh sample (clamp + content-reject guard against
                // ghosting); uncovered pixels keep history unless camera motion / disocclusion
                // forces them toward the spatial fill.
                let hist_used = select(hist,
                    clamp(hist, nmean - uniforms.temporal_clamp_k * nsigma,
                        nmean + uniforms.temporal_clamp_k * nsigma),
                    covered);
                let base_alpha = select(0.0, uniforms.temporal_alpha, covered);
                let dv = abs(hist - cur);
                let mismatch = max(max(dv.r, dv.g), max(dv.b, dv.a));
                let content_motion = select(0.0,
                    smoothstep(0.12 * uniforms.content_thresh, 0.35 * uniforms.content_thresh,
                        mismatch),
                    covered);
                let a = clamp(max(max(base_alpha, depth_reject),
                                  max(motion_px * uniforms.velocity_scale, content_motion)),
                    0.0, 1.0);
                out_value = mix(hist_used, cur, a);
            }
        }
    }
    let stored = clamp(out_value, vec4f(0.0), vec4f(4.0, 4.0, 4.0, 1.0));
    textureStore(history_color_out, p, stored);
    textureStore(history_depth_out, p, vec4f(depth_norm, 0.0, 0.0, 0.0));
}
