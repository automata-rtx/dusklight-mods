// 3x3 bilateral filter (edge-preserving blur), rgba: GI.rgb + AO in one pass.
// https://people.csail.mit.edu/sparis/bf_course/course_notes.pdf
//
// Structure and edge weighting identical to VBAO's denoise.wgsl (ported from Bevy Engine's SSAO
// spatial_denoise.wgsl v0.13.2, MIT OR Apache-2.0, itself derived from Intel XeGTAO, MIT - see
// res/licenses/), widened from a scalar AO channel to vec4 so the bounce light and the occlusion
// denoise together with the same edge weights (the depth_differences packed by the SSILVB pass).

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
    sky_intensity: f32,      // directional sky-light strength (0 disables in the sampler)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var gi_noisy: texture_2d<f32>;
@group(0) @binding(1) var depth_differences: texture_2d<u32>;
@group(0) @binding(2) var gi_denoised: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> uniforms: Uniforms;

fn clamp_coordinates(pixel_coordinates: vec2<i32>) -> vec2<i32> {
    return clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
}

// Each pixel's packed edge info is (left, right, top, bottom) weights, packed by the SSILVB pass.
fn load_edges(pixel_coordinates: vec2<i32>) -> vec4<f32> {
    return unpack4x8unorm(textureLoad(depth_differences, clamp_coordinates(pixel_coordinates), 0i).r);
}

fn load_value(pixel_coordinates: vec2<i32>) -> vec4<f32> {
    return textureLoad(gi_noisy, clamp_coordinates(pixel_coordinates), 0i);
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

    let center_value = load_value(pixel_coordinates);
    var sum = center_value * center_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(-1i, 0i)) * left_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(1i, 0i)) * right_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(0i, -1i)) * top_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(0i, 1i)) * bottom_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(-1i, -1i)) * top_left_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(1i, -1i)) * top_right_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(-1i, 1i)) * bottom_left_weight;
    sum += load_value(pixel_coordinates + vec2<i32>(1i, 1i)) * bottom_right_weight;

    var sum_weight = center_weight;
    sum_weight += left_weight;
    sum_weight += right_weight;
    sum_weight += top_weight;
    sum_weight += bottom_weight;
    sum_weight += top_left_weight;
    sum_weight += top_right_weight;
    sum_weight += bottom_left_weight;
    sum_weight += bottom_right_weight;

    // Strength blends the raw estimate back in (0 = raw, 1 = fully blurred) so fine detail can be
    // preserved now that the temporal chain carries most of the noise reduction.
    let denoised =
        mix(center_value, sum / sum_weight, clamp(uniforms.denoise_strength, 0.0, 1.0));

    textureStore(gi_denoised, pixel_coordinates, denoised);
}
