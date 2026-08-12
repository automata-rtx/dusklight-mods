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
                // bit 6 emissive bounce, bit 7 sky fill, bit 8 environment probe
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
    probe_intensity: f32,    // environment-probe ambient strength (0 disables it in the sampler)
    probe_saturation: f32,   // probe tint saturation: 0 = neutral grey at the probe's brightness
    probe_response: f32,     // probe adaptation rate scale (1 = the ~0.3s default)
    _pad0: f32,
    _pad1: f32,
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
// accumulate_probe entry point only: the finished color MIP chain (read at MIP 4), the previous
// frame's probe, this frame's sky estimate (for the up-axis fill), and the new probe.
@group(0) @binding(14) var probe_color: texture_2d<f32>;
@group(0) @binding(15) var probe_prev: texture_2d<f32>;
@group(0) @binding(16) var probe_sky: texture_2d<f32>;
@group(0) @binding(17) var probe_out: texture_storage_2d<rgba32float, write>;

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

// =================================================================================================
// ENVIRONMENT PROBE
//
// A world-space AMBIENT CUBE (Valve's HL2 basis): six radiance values, one per world axis, plus a
// coverage confidence each. It answers "how bright, and what color, is the world in direction d?"
// for the directions a pixel's visibility bitmask found NOTHING in - the light that arrives from
// beyond the march's reach. That is the one thing screen-space bounce structurally cannot do.
//
// WHY AN AMBIENT CUBE AND NOT SH9: for any unit direction the three facing axes' squared cosines
// sum to exactly 1 (d.x^2 + d.y^2 + d.z^2 = 1), so evaluation is an exact partition of unity -
// no normalization, no ringing, never negative, and no chance of the ambient gaining or losing
// energy as a surface turns. SH9 would ring badly on the narrow, partial angular coverage a
// single camera frustum provides, and ringing shows up as dark or oversaturated blotches.
//
// WHERE THE RADIANCE COMES FROM: MIP 4 of the color chain this pass already built - roughly 2k
// pre-averaged texels covering the whole frame. Each is projected onto the six axes by its world
// direction, weighted by the solid angle a screen-uniform texel actually subtends. No geometry is
// re-rendered and no game code is touched; the capture is one workgroup reading a tiny texture.
//
// HOW IT COVERS WHAT IS OFF-SCREEN: the probe PERSISTS. Each axis is updated only in proportion
// to how much the current frame constrained it, so directions the camera is not looking at keep
// what was measured when it last did. Walk past a torch and turn away and the torch's warmth
// stays in the -Z axis for as long as the adaptation rate holds it. This is what removes the
// screen-edge popping of a pure screen-space gather: the light does not vanish when it leaves
// the frame, it decays.
//
// The trade, stated plainly: directions the camera has NEVER looked at are unknown, and fall
// back to the frame's overall mean (plus the sky estimate for "up", which is the direction a
// player looks at least and every floor pixel faces most).
// =================================================================================================

// 64 threads x 6 axes of (radiance-weighted sum, weight sum). Axis order is +X,+Y,+Z,-X,-Y,-Z so
// the three positive and three negative weights each pack into one vec3.
var<workgroup> wg_probe: array<vec4f, 384>;

const PROBE_LUMA = vec3f(0.299, 0.587, 0.114);

@compute
@workgroup_size(64, 1, 1)
fn accumulate_probe(@builtin(local_invocation_index) local_index: u32) {
    let dims = max(vec2<i32>(uniforms.size) >> vec2<u32>(4u), vec2<i32>(1i));
    let texel_count = dims.x * dims.y;
    let r = uniforms.view_from_world;
    // The same firefly ceiling the march uses. This is a ~2k-sample average, so one boosted
    // emissive texel would otherwise swing the whole environment estimate.
    let luma_cap = max(4.0, 2.0 * uniforms.emissive_boost);

    var acc = array<vec4f, 6>(vec4f(0.0), vec4f(0.0), vec4f(0.0),
        vec4f(0.0), vec4f(0.0), vec4f(0.0));
    for (var t = i32(local_index); t < texel_count; t += 64i) {
        let p = vec2<i32>(t % dims.x, t / dims.x);
        let uv = (vec2f(p) + 0.5) / vec2f(dims);
        // Ray direction through this texel. Every point on the ray normalizes to the same
        // direction, so the depth passed here is arbitrary.
        let vdir = normalize(reconstruct_view_space_position(0.5, uv));
        // A screen-uniform texel subtends solid angle proportional to cos^3 of its angle off the
        // view axis, so frame corners must not count as much as the center.
        let cos_theta = max(-vdir.z, 0.0);
        let solid = cos_theta * cos_theta * cos_theta;
        if solid <= 1.0e-4 {
            continue;
        }
        // View -> world. view_from_world is orthonormal, so its transpose inverts it.
        let d = normalize(
            vec3f(dot(vdir, r[0].xyz), dot(vdir, r[1].xyz), dot(vdir, r[2].xyz)));

        var radiance = textureLoad(probe_color, p, 4i).rgb;
        let luma = dot(radiance, PROBE_LUMA);
        radiance *= min(1.0, luma_cap / max(luma, 1.0e-4));

        let dp = max(d, vec3f(0.0));
        let dn = max(-d, vec3f(0.0));
        let wp = dp * dp * solid;
        let wn = dn * dn * solid;
        acc[0] += vec4f(radiance * wp.x, wp.x);
        acc[1] += vec4f(radiance * wp.y, wp.y);
        acc[2] += vec4f(radiance * wp.z, wp.z);
        acc[3] += vec4f(radiance * wn.x, wn.x);
        acc[4] += vec4f(radiance * wn.y, wn.y);
        acc[5] += vec4f(radiance * wn.z, wn.z);
    }
    for (var i = 0u; i < 6u; i += 1u) {
        wg_probe[local_index * 6u + i] = acc[i];
    }
    for (var stride = 32u; stride > 0u; stride >>= 1u) {
        workgroupBarrier();
        if local_index < stride {
            for (var i = 0u; i < 6u; i += 1u) {
                wg_probe[local_index * 6u + i] += wg_probe[(local_index + stride) * 6u + i];
            }
        }
    }
    if local_index != 0u {
        return;
    }

    var total = vec4f(0.0);
    for (var i = 0u; i < 6u; i += 1u) {
        total += wg_probe[i];
    }
    let global_mean = select(
        vec3f(0.0), total.rgb / max(total.w, 1.0e-6), total.w > 1.0e-6);

    // Texel 6 carries the frame-wide mean and the "probe has ever been primed" marker.
    let prev_global = textureLoad(probe_prev, vec2<i32>(6i, 0i), 0i);
    let primed = prev_global.a > 0.001;

    // Adaptation rate. A large swing in overall brightness - a cave mouth, a room load, a
    // cutscene cut - accelerates it, so ambient does not lag a hard transition by half a second
    // while still being too slow to flicker on ordinary frame-to-frame noise.
    let prev_luma = dot(prev_global.rgb, PROBE_LUMA);
    let new_luma = dot(global_mean, PROBE_LUMA);
    let change = abs(new_luma - prev_luma) / max(max(prev_luma, new_luma), 0.02);
    let base_alpha = clamp(0.05 * uniforms.probe_response, 0.004, 0.5);
    var alpha = clamp(base_alpha * (1.0 + 6.0 * change), base_alpha, 0.6);
    if !primed {
        alpha = 1.0; // first frame ever (or after a resize): lock on rather than fade up
    }

    // Sky fill: the measured skybox radiance stands in for "up" where the camera has not
    // actually looked up. Outdoors this is usually redundant (sky is on screen and already in
    // the +Y bucket); it matters when the player is looking at their feet.
    let sky = textureLoad(probe_sky, vec2<i32>(0i, 0i), 0i);
    let sky_on = (uniforms.flags & 128u) != 0u && sky.a > 0.001;
    let sky_luma = dot(sky.rgb, PROBE_LUMA);
    // The cube stores RAW linear radiance (the sampler applies probe_intensity and the composite
    // the gi multiply), but sky_intensity arrives pre-divided by that same gi multiply - so undo
    // the pre-division here to put the fill in the same units as the measured axes.
    let sky_fill = mix(vec3f(sky_luma), sky.rgb, clamp(uniforms.sky_saturation, 0.0, 1.5)) *
        (sky.a * uniforms.sky_intensity * max(uniforms.gi_intensity, 0.01));

    for (var i = 0u; i < 6u; i += 1u) {
        let s = wg_probe[i];
        // Coverage = this axis's share of the frame's total sampling weight against an even
        // six-way split. An axis pointing behind the camera collects nothing and lands at 0.
        let coverage = clamp((s.w / max(total.w, 1.0e-6)) * 6.0, 0.0, 1.0);
        let measured = select(global_mean, s.rgb / max(s.w, 1.0e-6), s.w > 1.0e-6);

        let prev_axis = textureLoad(probe_prev, vec2<i32>(i32(i), 0i), 0i);
        // THE KEY LINE: each axis moves toward this frame's measurement only as far as this
        // frame actually saw that direction. Unseen directions keep their history intact.
        let axis_alpha = select(alpha * coverage, 1.0, !primed);
        var value = mix(prev_axis.rgb, measured, axis_alpha);
        // Confidence tracks coverage slowly in both directions, so a direction measured a while
        // ago stays trusted for a while and a never-seen one stays at the global fallback.
        var conf = select(mix(prev_axis.a, coverage, 0.05), coverage, !primed);

        if i == 1u && sky_on {
            // +Y only, and only to the extent direct coverage is missing.
            value = mix(value, sky_fill, (1.0 - conf) * clamp(sky.a, 0.0, 1.0));
            conf = max(conf, sky.a);
        }
        textureStore(probe_out, vec2<i32>(i32(i), 0i),
            vec4f(max(value, vec3f(0.0)), clamp(conf, 0.0, 1.0)));
    }
    textureStore(probe_out, vec2<i32>(6i, 0i),
        vec4f(max(mix(prev_global.rgb, global_mean, alpha), vec3f(0.0)), 1.0));
    textureStore(probe_out, vec2<i32>(7i, 0i), vec4f(0.0, 0.0, 0.0, 1.0));
}
