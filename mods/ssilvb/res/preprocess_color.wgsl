// SSILVB - scene-color prefilter: gamma decode + box-filtered MIP chain.
//
// The GI march samples radiance at screen positions far from the receiver; reading those from
// the full-resolution snapshot would thrash the cache exactly the way the SSILVB paper warns
// (the technique is bandwidth-bound). Instead this pass mirrors the depth prefilter: MIP 0 is
// the snapshot decimated to the chain resolution (with the same 4-phase jitter as the depth
// chain, so color and depth stay aligned in half-res temporal mode), MIPs 1-4 are box averages.
// Distant march samples then read the same MIP level as their depth fetch. The pre-averaging is
// a feature, not a loss: a wide visibility sector "sees" the average radiance of the surface it
// spans, which is what a box-filtered MIP approximates.
//
// The decode to LINEAR light happens here, once, instead of per march sample: the scene target
// is gamma-encoded, and accumulating bounce light in gamma space would over-brighten. The
// composite re-encodes after accumulation (fixed approximate 2.2 transfer).

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
    _pad0: f32,
    _pad1: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var color_mip0: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
// reduce_color entry point only (successive MIP reductions; bound per level by the host).
@group(0) @binding(3) var color_in: texture_2d<f32>;
@group(0) @binding(4) var color_out: texture_storage_2d<rgba16float, write>;

// 4-phase sub-pixel jitter within each 2x2 full-res block, matching preprocess_depth.wgsl -
// color MIP 0 must decimate the same full-res pixel the depth chain does each frame.
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

@compute
@workgroup_size(8, 8, 1)
fn prefilter_color(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let p = vec2<i32>(global_id.xy);
    if p.x >= i32(uniforms.size.x) || p.y >= i32(uniforms.size.y) {
        return;
    }
    let input_size = vec2<i32>(textureDimensions(scene_color));
    let coordinates = clamp(vec2<i32>(vec2<f32>(p) * uniforms.depth_scale) + taau_jitter(),
        vec2<i32>(0i), input_size - 1i);
    let encoded = textureLoad(scene_color, coordinates, 0i).rgb;
    let linear = pow(max(encoded, vec3f(0.0)), vec3f(2.2));
    textureStore(color_mip0, p, vec4f(linear, 1.0));
}

// One MIP reduction step (box average of the parent level); dispatched once per level 1..4.
@compute
@workgroup_size(8, 8, 1)
fn reduce_color(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let p = vec2<i32>(global_id.xy);
    let out_size = vec2<i32>(textureDimensions(color_out));
    if p.x >= out_size.x || p.y >= out_size.y {
        return;
    }
    let in_size = vec2<i32>(textureDimensions(color_in));
    let base = p * 2i;
    let maxc = in_size - 1i;
    let c0 = textureLoad(color_in, clamp(base, vec2<i32>(0i), maxc), 0i).rgb;
    let c1 = textureLoad(color_in, clamp(base + vec2<i32>(1i, 0i), vec2<i32>(0i), maxc), 0i).rgb;
    let c2 = textureLoad(color_in, clamp(base + vec2<i32>(0i, 1i), vec2<i32>(0i), maxc), 0i).rgb;
    let c3 = textureLoad(color_in, clamp(base + vec2<i32>(1i, 1i), vec2<i32>(0i), maxc), 0i).rgb;
    textureStore(color_out, p, vec4f((c0 + c1 + c2 + c3) * 0.25, 1.0));
}
