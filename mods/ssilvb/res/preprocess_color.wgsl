// SSILVB - scene-color prefilter: gamma decode + box-filtered MIP chain, emissive injection,
// and the sky-radiance estimate.
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
//
// EMISSIVE CAPTURE (fire, fairies, lantern glows): those are particle/effect draws in the
// TRANSLUCENT phase, after the opaque snapshot this mod samples - so they are invisible to the
// bounce unless captured separately. extract_emissive runs at FRAME_BEFORE_HUD each frame and
// stores max(late_scene - opaque_scene - threshold, 0) in linear light: everything the
// translucent phase ADDED (emissive particles, their bloom), thresholded so small deltas (our
// own composite, deferred fog's re-apply) don't feed back. prefilter_color then reprojects the
// PREVIOUS frame's delta into MIP 0 with the same temporal reprojection matrix the accumulation
// uses, so emissive light rides the normal MIP chain and the march needs no changes at all.
//
// SKY RADIANCE ESTIMATE (for the directional sky light): the skybox pixels in the snapshot were
// drawn by the game with its time-of-day palette already applied, so averaging them measures the
// real, blended sky tint - sunset gradients and weather included - with no game-state access.
// prefilter_color reduces each 8x8 workgroup's sky pixels (raw depth 0 = sky) to one texel of a
// partial-sums texture; reduce_sky collapses those to a single smoothed 1x1 value:
//   .rgb = linear sky radiance, .a = confidence (smoothed on-screen sky fraction). The value is
// temporally smoothed and HELD when no sky is visible (confidence decays slowly), so brief
// look-downs don't flicker the sky light and interiors fade it out rather than snapping.

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
                // bit 3 GI enabled, bit 4 AO apply, bit 5 white bounce proxy,
                // bit 6 emissive bounce, bit 7 sky light
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
    sky_saturation: f32,     // sky tint saturation: 0 = white light at sky brightness, 1 = full
    gi_saturation: f32,      // bounce chroma boost applied in the composite (1 = neutral)
    _pad0: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var color_mip0: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
// reduce_color entry point only (successive MIP reductions; bound per level by the host).
@group(0) @binding(3) var color_in: texture_2d<f32>;
@group(0) @binding(4) var color_out: texture_storage_2d<rgba16float, write>;
// extract_emissive entry point only (runs at FRAME_BEFORE_HUD, full render resolution).
@group(0) @binding(5) var late_color: texture_2d<f32>;
@group(0) @binding(6) var opaque_color: texture_2d<f32>;
@group(0) @binding(7) var emissive_out: texture_storage_2d<rgba16float, write>;
// prefilter_color entry point only: depth MIP 0 (already written this pass) for sky detection +
// emissive reprojection, the PREVIOUS frame's emissive delta, and the sky partial-sums output.
@group(0) @binding(8) var depth_mip0: texture_2d<f32>;
@group(0) @binding(9) var emissive_prev: texture_2d<f32>;
@group(0) @binding(10) var sky_partial_out: texture_storage_2d<rgba32float, write>;
// reduce_sky entry point only.
@group(0) @binding(11) var sky_partial_in: texture_2d<f32>;
@group(0) @binding(12) var sky_prev: texture_2d<f32>;
@group(0) @binding(13) var sky_out: texture_storage_2d<rgba16float, write>;

fn decode(encoded: vec3f) -> vec3f {
    return pow(max(encoded, vec3f(0.0)), vec3f(2.2));
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

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

// Workgroup sky reduction scratch: (sum.rgb, count) per invocation.
var<workgroup> wg_sky: array<vec4f, 64>;

@compute
@workgroup_size(8, 8, 1)
fn prefilter_color(@builtin(global_invocation_id) global_id: vec3<u32>,
    @builtin(local_invocation_index) local_index: u32,
    @builtin(workgroup_id) workgroup_id: vec3<u32>) {
    let p = vec2<i32>(global_id.xy);
    // No early return: every invocation must reach the workgroup barriers below (uniform
    // control flow), so out-of-bounds threads contribute zero and skip only the stores.
    let in_bounds = p.x < i32(uniforms.size.x) && p.y < i32(uniforms.size.y);

    var radiance = vec3f(0.0);
    var sky_contrib = vec4f(0.0);
    if in_bounds {
        let input_size = vec2<i32>(textureDimensions(scene_color));
        let coordinates = clamp(vec2<i32>(vec2<f32>(p) * uniforms.depth_scale) + taau_jitter(),
            vec2<i32>(0i), input_size - 1i);
        radiance = decode(textureLoad(scene_color, coordinates, 0i).rgb);
        let depth = textureLoad(depth_mip0, p, 0i).r;

        // Sky pixels (reversed-Z clear = 0): the game's skybox with the time-of-day palette
        // already applied. Contribute to the sky-radiance estimate, BEFORE the emissive add.
        // HORIZON WEIGHTING: pixels lower on screen (nearer the horizon) count more - the
        // horizon haze is the sky color that actually blends with an area's palette (Gerudo's
        // warm horizon vs its pale blue zenith), so the estimate leans warm in warm areas
        // instead of casting zenith blue everywhere. Zenith still contributes (floor weight)
        // so looking straight up doesn't lose the estimate.
        if depth <= 0.0 {
            let uv_y = (f32(p.y) + 0.5) * uniforms.inv_size.y;
            let w = 0.15 + uv_y * uv_y;
            sky_contrib = vec4f(radiance * w, w);
        }

        if (uniforms.flags & 64u) != 0u {
            // Add the previous frame's emissive delta, reprojected from the previous frame's
            // screen space (the delta was extracted at the end of that frame). Sky pixels and
            // off-screen reprojections fall back to the same uv - a one-frame smear on fast
            // pans, invisible in practice because the delta is re-extracted every frame.
            let full_size = vec2f(input_size);
            let uv = (vec2f(coordinates) + 0.5) / full_size;
            var prev_uv = uv;
            if depth > 0.0 {
                let view_pos = reconstruct_view_space_position(depth, uv);
                let clip_prev = uniforms.reproject * vec4f(view_pos, 1.0);
                if clip_prev.w > 1.0e-4 {
                    let ndc = clip_prev.xy / clip_prev.w;
                    let cand = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if cand.x >= 0.0 && cand.y >= 0.0 && cand.x <= 1.0 && cand.y <= 1.0 {
                        prev_uv = cand;
                    }
                }
            }
            let e_dims = vec2f(textureDimensions(emissive_prev));
            let e_texel = clamp(
                vec2<i32>(prev_uv * e_dims), vec2<i32>(0i), vec2<i32>(e_dims) - vec2<i32>(1i));
            radiance += textureLoad(emissive_prev, e_texel, 0i).rgb * uniforms.emissive_boost;
        }
    }

    // Tree-reduce this workgroup's sky contributions to one partial-sums texel.
    wg_sky[local_index] = sky_contrib;
    for (var stride = 32u; stride > 0u; stride >>= 1u) {
        workgroupBarrier();
        if local_index < stride {
            wg_sky[local_index] += wg_sky[local_index + stride];
        }
    }
    if local_index == 0u {
        textureStore(sky_partial_out, vec2<i32>(workgroup_id.xy), wg_sky[0]);
    }

    if in_bounds {
        textureStore(color_mip0, p, vec4f(radiance, 1.0));
    }
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

// Collapse the partial sums to the single smoothed sky value (one 64-thread workgroup).
// .rgb = linear sky radiance (held at its last value while no sky is on screen),
// .a   = confidence: the smoothed on-screen sky fraction, rising while sky is visible and
//        decaying slowly indoors so the sky light fades out instead of snapping.
var<workgroup> wg_total: array<vec4f, 64>;

@compute
@workgroup_size(64, 1, 1)
fn reduce_sky(@builtin(local_invocation_index) local_index: u32) {
    let dims = vec2<i32>(textureDimensions(sky_partial_in));
    let texel_count = dims.x * dims.y;
    var acc = vec4f(0.0);
    for (var i = i32(local_index); i < texel_count; i += 64i) {
        acc += textureLoad(sky_partial_in, vec2<i32>(i % dims.x, i / dims.x), 0i);
    }
    wg_total[local_index] = acc;
    for (var stride = 32u; stride > 0u; stride >>= 1u) {
        workgroupBarrier();
        if local_index < stride {
            wg_total[local_index] += wg_total[local_index + stride];
        }
    }
    if local_index == 0u {
        let sum = wg_total[0]; // (horizon-weighted rgb sum, weight sum)
        let prev = textureLoad(sky_prev, vec2<i32>(0i, 0i), 0i);
        let chain_pixels = max(uniforms.size.x * uniforms.size.y, 1.0);
        // Full measurement confidence once >=2% of the screen is sky. 0.483 is the mean of the
        // horizon weight (0.15 + y^2) over the screen, converting weight-sum back to a pixel
        // count equivalent.
        let coverage = clamp((sum.w / (0.483 * chain_pixels)) / 0.02, 0.0, 1.0);
        var value = prev;
        if sum.w > 0.5 {
            let mean = sum.rgb / sum.w;
            // Lock on instantly the first time sky is ever seen; then smooth over ~12 frames,
            // slower when only a sliver of sky is visible (a sliver is a biased sample).
            let alpha = select(0.08 * max(coverage, 0.1), 1.0, prev.a <= 0.001);
            value = vec4f(mix(prev.rgb, mean, alpha), prev.a);
        }
        // Confidence chases the coverage slowly in both directions: brief look-downs barely
        // move it; walking indoors fades the sky light out over a few seconds.
        value.a = clamp(mix(prev.a, coverage, 0.03), 0.0, 1.0);
        textureStore(sky_out, vec2<i32>(0i, 0i), value);
    }
}

// Emissive delta extract: everything the translucent/effect phase ADDED over the opaque scene
// this frame (fire, fairy glow, lantern light, their bloom), in linear light, floored by the
// threshold so low-level differences (our own composite, deferred fog's re-apply, bloom haze)
// do not feed back into next frame's bounce. Runs at FRAME_BEFORE_HUD, full render resolution.
@compute
@workgroup_size(8, 8, 1)
fn extract_emissive(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let p = vec2<i32>(global_id.xy);
    let out_size = vec2<i32>(textureDimensions(emissive_out));
    if p.x >= out_size.x || p.y >= out_size.y {
        return;
    }
    let maxc = vec2<i32>(textureDimensions(late_color)) - 1i;
    let c = clamp(p, vec2<i32>(0i), maxc);
    let late = decode(textureLoad(late_color, c, 0i).rgb);
    let opaque = decode(textureLoad(opaque_color, c, 0i).rgb);
    let delta = max(late - opaque - vec3f(uniforms.emissive_threshold), vec3f(0.0));
    textureStore(emissive_out, p, vec4f(delta, 1.0));
}
