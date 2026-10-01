// 3x3 bilateral filter (edge-preserving blur)
// https://people.csail.mit.edu/sparis/bf_course/course_notes.pdf
//
// Note: Does not use the Gaussian kernel part of a typical bilateral blur
// From the paper: "use the information gathered on a neighborhood of 4 x 4 using a bilateral filter for
// reconstruction, using _uniform_ convolution weights"
//
// Note: The paper does a 4x4 (not quite centered) filter, offset by +/- 1 pixel every other frame
// XeGTAO does a 3x3 filter, on two pixels at a time per compute thread, applied twice
// We do a 3x3 filter, on 1 pixel per compute thread, applied 0-3 times (Denoise Passes; the host
// ping-pongs aoNoisy and aoFinal between passes)
//
// Ported from Bevy Engine, crates/bevy_pbr/src/ssao/spatial_denoise.wgsl (v0.13.2), licensed
// MIT OR Apache-2.0 (see res/licenses/), itself derived from Intel XeGTAO (MIT).
//
// PORT: the textureGather calls are rewritten as explicit per-neighbor textureLoads (r32float
// and r32uint are unfilterable); Bevy view uniforms -> the mod's uniform block; r16float -> r32float.
// Edge weights come from vbao.wgsl, which zeroes them on pixels without an authored normal.

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

@group(0) @binding(0) var ambient_occlusion_noisy: texture_2d<f32>;
@group(0) @binding(1) var depth_differences: texture_2d<u32>;
@group(0) @binding(2) var ambient_occlusion: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var<uniform> uniforms: Uniforms;

fn clamp_coordinates(pixel_coordinates: vec2<i32>) -> vec2<i32> {
    return clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
}

// Each pixel's packed edge info is (left, right, top, bottom) weights, packed by the VBAO pass.
fn load_edges(pixel_coordinates: vec2<i32>) -> vec4<f32> {
    return unpack4x8unorm(textureLoad(depth_differences, clamp_coordinates(pixel_coordinates), 0i).r);
}

fn load_visibility(pixel_coordinates: vec2<i32>) -> f32 {
    return textureLoad(ambient_occlusion_noisy, clamp_coordinates(pixel_coordinates), 0i).r;
}

@compute
@workgroup_size(8, 8, 1)
fn spatial_denoise(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coordinates = vec2<i32>(global_id.xy);

    let left_edges = load_edges(pixel_coordinates + vec2<i32>(-1i, 0i));
    let right_edges = load_edges(pixel_coordinates + vec2<i32>(1i, 0i));
    let top_edges = load_edges(pixel_coordinates + vec2<i32>(0i, -1i));
    let bottom_edges = load_edges(pixel_coordinates + vec2<i32>(0i, 1i));
    var center_edges = load_edges(pixel_coordinates);
    // Cross-check each edge against the neighbor's opposing edge weight.
    center_edges *= vec4<f32>(left_edges.y, right_edges.x, top_edges.w, bottom_edges.z);

    let center_weight = 1.2;
    let left_weight = center_edges.x;
    let right_weight = center_edges.y;
    let top_weight = center_edges.z;
    let bottom_weight = center_edges.w;
    let top_left_weight = 0.425 * (top_weight * top_edges.x + left_weight * left_edges.z);
    let top_right_weight = 0.425 * (top_weight * top_edges.y + right_weight * right_edges.z);
    let bottom_left_weight = 0.425 * (bottom_weight * bottom_edges.x + left_weight * left_edges.w);
    let bottom_right_weight = 0.425 * (bottom_weight * bottom_edges.y + right_weight * right_edges.w);

    let center_visibility = load_visibility(pixel_coordinates);
    let left_visibility = load_visibility(pixel_coordinates + vec2<i32>(-1i, 0i));
    let right_visibility = load_visibility(pixel_coordinates + vec2<i32>(1i, 0i));
    let top_visibility = load_visibility(pixel_coordinates + vec2<i32>(0i, -1i));
    let bottom_visibility = load_visibility(pixel_coordinates + vec2<i32>(0i, 1i));
    let top_left_visibility = load_visibility(pixel_coordinates + vec2<i32>(-1i, -1i));
    let top_right_visibility = load_visibility(pixel_coordinates + vec2<i32>(1i, -1i));
    let bottom_left_visibility = load_visibility(pixel_coordinates + vec2<i32>(-1i, 1i));
    let bottom_right_visibility = load_visibility(pixel_coordinates + vec2<i32>(1i, 1i));

    // PORT: Bevy sums the center sample unweighted while still counting center_weight in the
    // denominator; XeGTAO's original weights the value too, which is what we do here.
    var sum = center_visibility * center_weight;
    sum += left_visibility * left_weight;
    sum += right_visibility * right_weight;
    sum += top_visibility * top_weight;
    sum += bottom_visibility * bottom_weight;
    sum += top_left_visibility * top_left_weight;
    sum += top_right_visibility * top_right_weight;
    sum += bottom_left_visibility * bottom_left_weight;
    sum += bottom_right_visibility * bottom_right_weight;

    var sum_weight = center_weight;
    sum_weight += left_weight;
    sum_weight += right_weight;
    sum_weight += top_weight;
    sum_weight += bottom_weight;
    sum_weight += top_left_weight;
    sum_weight += top_right_weight;
    sum_weight += bottom_left_weight;
    sum_weight += bottom_right_weight;

    // Denoise Strength blends this pass's input back in (0 = unchanged, 1 = fully blurred), to
    // keep fine detail when temporal accumulation does most of the noise reduction.
    let denoised_visibility =
        mix(center_visibility, sum / sum_weight, clamp(uniforms.denoise_strength, 0.0, 1.0));

    textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(denoised_visibility, 0.0, 0.0, 0.0));
}
