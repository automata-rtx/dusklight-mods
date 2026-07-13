// Enhanced Ambient Occlusion - VBAO (visibility-bitmask ambient occlusion) pass.
//
// The pass framework (MIP-prefiltered depth reads, hilbert/R2 noise, edge output for the
// spatial denoiser, accurate 5-tap normal reconstruction) follows the ao_mod demo, which is
// ported from Bevy Engine's SSAO (MIT OR Apache-2.0) / Intel XeGTAO (MIT); see res/licenses/.
//
// The occlusion estimator itself replaces the classic horizon-tracking GTAO inner loop with a
// 32-sector VISIBILITY BITMASK per slice (Therrien et al. 2022, adapted from indirect lighting
// to AO): each occluder carves only the angular sectors it actually spans (front..back, using a
// thickness term) instead of raising a monotonic horizon. Separated occluders, gaps and thin
// geometry (grass!) are handled correctly, where a horizon tracker treats everything below the
// highest sample as one solid wall and overdarkens behind thin occluders.
//
// The sampling radius is DEPTH-PROPORTIONAL (a fraction of the view-space depth) rather than
// fixed world units, so screen-space coverage stays roughly uniform with distance and the
// setting is independent of the game's large world-unit scale.

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
    thick_dist_scale: f32,  // extra occluder thickness, fraction of the view-space radius
    inv_debug_depth: f32,   // debug depth view gradient scale (1 / world units)
    radius_far: f32,        // far effect radius (fraction of view depth); 0 disables the ramp
    radius_ramp_start: f32, // radius ramp band start, fraction of the far plane
    radius_ramp_end: f32,   // radius ramp band end, fraction of the far plane
    denoise_strength: f32,  // spatial denoise blend, 0 raw .. 1 fully blurred
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var preprocessed_depth: texture_2d<f32>;
@group(0) @binding(1) var hilbert_index_lut: texture_2d<u32>;
@group(0) @binding(2) var ambient_occlusion: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var depth_differences: texture_storage_2d<r32uint, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;

const PI: f32 = 3.141592653589793;
const HALF_PI: f32 = 1.5707963267948966;

fn fast_sqrt(x: f32) -> f32 {
    return bitcast<f32>(0x1fbd1df5 + (bitcast<i32>(x) >> 1u));
}

fn fast_acos(in_x: f32) -> f32 {
    let x = abs(in_x);
    var res = -0.156583 * x + HALF_PI;
    res *= fast_sqrt(1.0 - x);
    return select(PI - res, res, in_x >= 0.0);
}

fn load_noise(pixel_coordinates: vec2<i32>) -> vec2<f32> {
    let index = textureLoad(hilbert_index_lut, pixel_coordinates % 64, 0).r;
    // R2 sequence, advanced per frame when temporal accumulation is on so the accumulator
    // averages decorrelated samples (frame_index is pinned to 0 by the host otherwise).
    return fract(0.5 + (f32(index) + f32(uniforms.frame_index % 64u)) *
                           vec2<f32>(0.75487766624669276005, 0.5698402909980532659114));
}

fn load_depth(pixel_coordinates: vec2<i32>, mip_level: i32) -> f32 {
    let mip_size = max(vec2<i32>(uniforms.size) >> vec2<u32>(u32(mip_level)), vec2<i32>(1i));
    let coordinates = clamp(pixel_coordinates, vec2<i32>(0i), mip_size - 1i);
    return textureLoad(preprocessed_depth, coordinates, mip_level).r;
}

// Depth differences between neighbor pixels, packed for the spatial denoiser (edge preservation).
// Unchanged from the demo/XeGTAO.
fn calculate_neighboring_depth_differences(pixel_coordinates: vec2<i32>) -> f32 {
    let depth_center = load_depth(pixel_coordinates, 0i);
    let depth_left = load_depth(pixel_coordinates + vec2<i32>(-1i, 0i), 0i);
    let depth_top = load_depth(pixel_coordinates + vec2<i32>(0i, -1i), 0i);
    let depth_bottom = load_depth(pixel_coordinates + vec2<i32>(0i, 1i), 0i);
    let depth_right = load_depth(pixel_coordinates + vec2<i32>(1i, 0i), 0i);

    var edge_info = vec4<f32>(depth_left, depth_right, depth_top, depth_bottom) - depth_center;
    let slope_left_right = (edge_info.y - edge_info.x) * 0.5;
    let slope_top_bottom = (edge_info.w - edge_info.z) * 0.5;
    let edge_info_slope_adjusted = edge_info +
        vec4<f32>(slope_left_right, -slope_left_right, slope_top_bottom, -slope_top_bottom);
    edge_info = min(abs(edge_info), abs(edge_info_slope_adjusted));
    let bias = 0.25;
    let scale = depth_center * 0.011;
    edge_info = saturate((1.0 + bias) - edge_info / scale);
    let edge_info_packed = vec4<u32>(pack4x8unorm(edge_info), 0u, 0u, 0u);
    textureStore(depth_differences, pixel_coordinates, edge_info_packed);
    return depth_center;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2<f32>) -> vec3<f32> {
    let clip_xy = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4<f32>(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

// 4-phase sub-pixel jitter within each 2x2 full-res block, matching preprocess_depth.wgsl.
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

// UV of a chain texel. In half-res temporal upsampling each texel stands in for a jittered
// full-res pixel; anchor its uv there so the (jittered) prefiltered depth and the reconstructed
// position agree. Otherwise this is the plain chain-space texel center (unchanged behavior).
fn chain_uv(coord: vec2<i32>) -> vec2<f32> {
    if uniforms.depth_scale.x >= 1.5 && (uniforms.flags & 1u) != 0u {
        let full_size = uniforms.size * uniforms.depth_scale;
        return (vec2<f32>(coord) * uniforms.depth_scale + vec2<f32>(taau_jitter()) + 0.5) / full_size;
    }
    return (vec2<f32>(coord) + 0.5) * uniforms.inv_size;
}

fn view_position_at(pixel_coordinates: vec2<i32>) -> vec3<f32> {
    let depth = load_depth(pixel_coordinates, 0i);
    return reconstruct_view_space_position(depth, chain_uv(pixel_coordinates));
}

// Accurate view-space normal reconstruction from depth (atyuwen's 5-tap method); unchanged
// from the demo. Stable across depth discontinuities where naive derivatives smear.
fn reconstruct_normal(pixel_coordinates: vec2<i32>, pixel_position: vec3<f32>, depth_center: f32) -> vec3<f32> {
    let depth_left1 = load_depth(pixel_coordinates + vec2<i32>(-1i, 0i), 0i);
    let depth_left2 = load_depth(pixel_coordinates + vec2<i32>(-2i, 0i), 0i);
    let depth_right1 = load_depth(pixel_coordinates + vec2<i32>(1i, 0i), 0i);
    let depth_right2 = load_depth(pixel_coordinates + vec2<i32>(2i, 0i), 0i);
    let depth_top1 = load_depth(pixel_coordinates + vec2<i32>(0i, -1i), 0i);
    let depth_top2 = load_depth(pixel_coordinates + vec2<i32>(0i, -2i), 0i);
    let depth_bottom1 = load_depth(pixel_coordinates + vec2<i32>(0i, 1i), 0i);
    let depth_bottom2 = load_depth(pixel_coordinates + vec2<i32>(0i, 2i), 0i);

    let use_left = abs(2.0 * depth_left1 - depth_left2 - depth_center) <
        abs(2.0 * depth_right1 - depth_right2 - depth_center);
    let use_top = abs(2.0 * depth_top1 - depth_top2 - depth_center) <
        abs(2.0 * depth_bottom1 - depth_bottom2 - depth_center);

    var ddx: vec3<f32>;
    if use_left {
        ddx = pixel_position - view_position_at(pixel_coordinates + vec2<i32>(-1i, 0i));
    } else {
        ddx = view_position_at(pixel_coordinates + vec2<i32>(1i, 0i)) - pixel_position;
    }
    var ddy: vec3<f32>;
    if use_top {
        ddy = pixel_position - view_position_at(pixel_coordinates + vec2<i32>(0i, -1i));
    } else {
        ddy = view_position_at(pixel_coordinates + vec2<i32>(0i, 1i)) - pixel_position;
    }

    var normal = normalize(cross(ddy, ddx));
    if dot(normal, pixel_position) > 0.0 {
        normal = -normal;
    }
    return normal;
}

// Clear the angular sectors [h.x, h.y) (normalized to [0,1] across the slice) from the
// visibility bitfield. occ starts all-ones (fully visible); occluders AND away the sectors
// they cover. Shift amounts are kept < 32 (WGSL UB).
fn carve_occluded_sectors(occ: u32, h: vec2<f32>) -> u32 {
    let a = min(u32(clamp(h.x, 0.0, 1.0) * 32.0), 31u);
    let e = u32(clamp(h.y, 0.0, 1.0) * 32.0);
    let b = select(0u, e - a, e > a);
    let bs = min(b, 31u);
    let ones = select((1u << bs) - 1u, 0xFFFFFFFFu, b >= 32u);
    return occ & ~(ones << a);
}

// One marched sample: view-space delta from the center -> front/back horizon angles of a thick
// occluder, mapped into [0,1] across the slice (centred on the projected-normal angle n) and run
// through a cosine-lobe smoothstep, then carved out of the visibility mask. `flip` selects the
// -direction mapping (front/back pair negated and swapped onto the opposite half of the slice).
fn carve_sample(occ: u32, dvec: vec3<f32>, v: vec3<f32>, n: f32, t_base: f32, depth_range: f32, flip: bool) -> u32 {
    // Occluder thickness fades with the VIEW-SPACE DEPTH difference: connected crevice walls
    // (small depth diff even when laterally far) keep full thickness so deep seams reach full
    // darkness; silhouette jumps fade to nothing and stop haloing past outlines.
    let t_eff = t_base * clamp(1.0 - abs(dvec.z) / depth_range, 0.0, 1.0);
    if t_eff <= 1.0e-4 {
        return occ;
    }
    let ddv = dot(dvec, v);
    let ddd = dot(dvec, dvec);
    var fb = vec2<f32>(ddv, ddv - t_eff) *
        inverseSqrt(max(vec2<f32>(ddd, ddd - 2.0 * t_eff * ddv + t_eff * t_eff), vec2<f32>(1.0e-12)));
    fb = clamp(fb, vec2<f32>(-1.0), vec2<f32>(1.0));
    var fbang = vec2<f32>(fast_acos(fb.x), fast_acos(fb.y));
    if flip {
        fbang = vec2<f32>(-fbang.y, -fbang.x);
    }
    var hh = clamp((fbang + n) / PI + 0.5, vec2<f32>(0.0), vec2<f32>(1.0));
    hh = hh * hh * (3.0 - 2.0 * hh); // cosine-lobe (solid angle) weighting
    return carve_occluded_sectors(occ, hh);
}

// Load a marched sample's view position from the prefiltered depth MIP chain (XeGTAO bandwidth
// optimization). w carries the raw depth so sky (reversed-Z clear = 0) can be skipped.
fn load_sample_position(uv: vec2<f32>, sample_mip_level: f32) -> vec4<f32> {
    let mip_level = i32(sample_mip_level + 0.5);
    let mip_size = max(vec2<i32>(uniforms.size) >> vec2<u32>(u32(mip_level)), vec2<i32>(1i));
    let coords = clamp(vec2<i32>(uv * vec2<f32>(mip_size)), vec2<i32>(0i), mip_size - 1i);
    let depth = textureLoad(preprocessed_depth, coords, mip_level).r;
    return vec4<f32>(reconstruct_view_space_position(depth, uv), depth);
}

@compute
@workgroup_size(8, 8, 1)
fn vbao(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coordinates = vec2<i32>(global_id.xy);
    let uv = chain_uv(pixel_coordinates);

    let raw_depth = calculate_neighboring_depth_differences(pixel_coordinates);
    if raw_depth <= 0.0 {
        // Reversed-Z background/sky: fully visible.
        textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(1.0, 0.0, 0.0, 0.0));
        return;
    }

    var pixel_position = reconstruct_view_space_position(raw_depth, uv);
    let pixel_normal = reconstruct_normal(pixel_coordinates, pixel_position, raw_depth);
    pixel_position *= 1.0 - uniforms.depth_bias; // bias toward the camera suppresses self-occlusion
    let view_vec = normalize(-pixel_position);
    var normal = pixel_normal;
    // Face the normal toward the camera only when CLEARLY back-facing (double-sided foliage seen
    // from behind); the margin keeps grazing surfaces from toggling per pixel.
    if dot(normal, view_vec) < -0.15 {
        normal = -normal;
    }

    // Depth-proportional radius: constant screen-space search radius. Base thickness grows
    // logarithmically with the view-space radius (keeps close-up foliage from overdarkening),
    // which starves distant occlusion - the log term becomes a vanishing fraction of the
    // radius - so thick_dist_scale adds a radius-PROPORTIONAL floor that keeps mid/far
    // occluders carving meaningful sector spans.
    let abs_z = max(-pixel_position.z, 1.0e-4);
    // Distance-scaled radius: ramp the (depth-proportional) radius from effect_radius up to
    // radius_far across [ramp_start, ramp_end] of the far plane - tight contact detail up close,
    // broad occlusion that gives distant landmarks depth at range. radius_far 0 disables.
    var eff_radius = uniforms.effect_radius;
    if uniforms.radius_far > 0.0 {
        let dist_norm = clamp(abs_z * uniforms.inv_far, 0.0, 1.0);
        eff_radius = mix(uniforms.effect_radius, uniforms.radius_far,
            smoothstep(uniforms.radius_ramp_start,
                max(uniforms.radius_ramp_end, uniforms.radius_ramp_start + 0.01), dist_norm));
    }
    let view_radius = abs_z * eff_radius;
    let proj_scale_y = 0.5 * uniforms.size.y * uniforms.projection[1][1];
    let radius_pix = clamp(eff_radius * proj_scale_y, 4.0, uniforms.radius_max * uniforms.size.y);
    let t_base = log(1.0 + view_radius) * 0.3333 * uniforms.thickness +
        view_radius * uniforms.thick_dist_scale;
    let depth_range = view_radius * uniforms.thick_fade;

    let noise = load_noise(pixel_coordinates);
    let slices = max(uniforms.slice_count, 1.0);
    let steps = max(uniforms.steps_per_side, 1.0);

    var visibility = 0.0;
    var norm_sum = 0.0;
    for (var s = 0.0; s < slices; s += 1.0) {
        let phi = PI * (s + noise.x) / slices;
        let dir = vec2<f32>(cos(phi), sin(phi)); // screen-space slice direction
        // View-space slice direction (screen y points down in framebuffer space).
        let dir3 = normalize(vec3<f32>(dir.x, -dir.y, 0.0));
        let slice_plane_normal = normalize(cross(dir3, view_vec));
        let proj_n = normal - slice_plane_normal * dot(normal, slice_plane_normal);
        let proj_n_len = length(proj_n);
        if proj_n_len < 1.0e-4 {
            continue;
        }
        let proj_nn = proj_n / proj_n_len;
        let tang = cross(slice_plane_normal, view_vec);
        let n = atan2(dot(proj_nn, tang), dot(proj_nn, view_vec));

        var occ: u32 = 0xFFFFFFFFu;
        for (var step = 1.0; step <= steps; step += 1.0) {
            let s01 = clamp((step - noise.y) / steps, 0.0, 1.0);
            let dist = s01 * s01 * radius_pix; // x^2 sample distribution
            let offset = dir * dist * uniforms.inv_size;
            // MIP level from the sample's screen distance in pixels (bandwidth optimization).
            let sample_mip_level = clamp(log2(max(dist, 1.0)) - 3.3, 0.0, 4.0);

            let sp = load_sample_position(uv + offset, sample_mip_level);
            if sp.w > 0.0 {
                occ = carve_sample(occ, sp.xyz - pixel_position, view_vec, n, t_base, depth_range, false);
            }
            let sn = load_sample_position(uv - offset, sample_mip_level);
            if sn.w > 0.0 {
                occ = carve_sample(occ, sn.xyz - pixel_position, view_vec, n, t_base, depth_range, true);
            }
        }

        // Slice visibility = fraction of sectors still unoccluded, weighted by the projected
        // normal length (the slice's share of the hemisphere).
        visibility += (f32(countOneBits(occ)) / 32.0) * proj_n_len;
        norm_sum += proj_n_len;
    }

    var ao = 1.0;
    if norm_sum > 1.0e-4 {
        ao = clamp(visibility / norm_sum, 0.0, 1.0);
    }
    textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(ao, 0.0, 0.0, 0.0));
}
