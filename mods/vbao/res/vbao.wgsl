// VBAO occlusion pass (compute, AO chain resolution).
//
// The pass framework (MIP-prefiltered depth reads, Hilbert/R2 noise, edge output for the spatial
// denoiser) follows Encounter's ao_mod demo, which is ported from Bevy Engine's SSAO
// (MIT OR Apache-2.0) / Intel XeGTAO (MIT); see res/licenses/.
//
// The estimator replaces GTAO's horizon tracking with a 32-sector visibility bitmask per slice
// (Therrien, Levesque and Gilet, 2023, arXiv:2301.11376, applied here to AO only): each occluder
// clears only the angular sectors it spans, front to back given a thickness, instead of raising a
// single horizon. Gaps, separated occluders and thin geometry such as grass therefore do not
// darken everything behind them.
//
// The shading normal is the game's authored normal from the gfx service; there is no
// reconstruction fallback. A separate plane derived from depth is used only to reject samples
// below the surface (geometric_normal_view).
//
// The radius is a fraction of view depth, so the setting does not depend on the game's world-unit
// scale. It ramps from Radius to Far Radius over a world-unit band of view depth, so the on-screen
// radius grows with distance until Max Screen Radius caps it.
//
// Outputs: raw visibility (1 = unoccluded) and packed edge weights for denoise.wgsl.

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

@group(0) @binding(0) var preprocessed_depth: texture_2d<f32>;
// Binding 1 is unused (it was a noise LUT; the Hilbert index is now computed in-shader).
@group(0) @binding(2) var ambient_occlusion: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var depth_differences: texture_storage_2d<r32uint, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;
// The game's authored surface normals, snapshotted by the same resolve_pass call as the depth
// (VBAO's SCENE_AFTER_OPAQUE hook). Full render resolution, view space, encoded xyz * 0.5 + 0.5;
// alpha 1 where the normal is usable, 0 where not. A draw writes a normal exactly when it writes
// depth, but a draw without an NRM vertex attribute (or whose interpolated normal is zero) stores
// alpha 0. Alpha is the only validity test.
@group(0) @binding(5) var scene_normal: texture_2d<f32>;

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

// Hilbert curve index of a pixel within its 64x64 tile (order-6 curve, indices 0..4095), the same
// construction as Bevy's generate_hilbert_index_lut / XeGTAO, computed per pixel instead of read
// from a LUT texture.
fn hilbert_index(px: u32, py: u32) -> u32 {
    var x = px & 63u;
    var y = py & 63u;
    var index = 0u;
    for (var level = 32u; level > 0u; level = level >> 1u) {
        let rx = select(0u, 1u, (x & level) != 0u);
        let ry = select(0u, 1u, (y & level) != 0u);
        index += level * level * ((3u * rx) ^ ry);
        if ry == 0u {
            if rx == 1u {
                x = 63u - x;
                y = 63u - y;
            }
            let t = x;
            x = y;
            y = t;
        }
    }
    return index;
}

fn load_noise(pixel_coordinates: vec2<i32>) -> vec2<f32> {
    let index = hilbert_index(u32(pixel_coordinates.x), u32(pixel_coordinates.y));
    // R2 sequence offset by frame_index (mod 64), so successive frames sample different directions
    // for the accumulator to average. The host pins frame_index to 0 when accumulation is off.
    return fract(0.5 + (f32(index) + f32(uniforms.frame_index % 64u)) *
                           vec2<f32>(0.75487766624669276005, 0.5698402909980532659114));
}

fn load_depth(pixel_coordinates: vec2<i32>, mip_level: i32) -> f32 {
    let mip_size = max(vec2<i32>(uniforms.size) >> vec2<u32>(u32(mip_level)), vec2<i32>(1i));
    let coordinates = clamp(pixel_coordinates, vec2<i32>(0i), mip_size - 1i);
    return textureLoad(preprocessed_depth, coordinates, mip_level).r;
}

// Depth differences between neighbour pixels, packed as (left, right, top, bottom) edge weights for
// the spatial denoiser, as in XeGTAO / the demo.
// Addition: `has_normal` false writes zero weights. A pixel without an authored normal is written
// as fully visible, and it is usually depth-continuous with its surroundings (a draw that simply
// has no NRM attribute), so with normal weights its 1.0 would bleed into the neighbours as a bright
// rim. Because denoise.wgsl also weights each tap by the neighbour's opposing edge, zeroing here
// isolates the pixel in both directions.
fn calculate_neighboring_depth_differences(pixel_coordinates: vec2<i32>, has_normal: bool) -> f32 {
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
    let edge_info_packed =
        vec4<u32>(select(0u, pack4x8unorm(edge_info), has_normal), 0u, 0u, 0u);
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
// position agree. Otherwise this is the plain chain-space texel centre.
fn chain_uv(coord: vec2<i32>) -> vec2<f32> {
    if uniforms.depth_scale.x >= 1.5 && (uniforms.flags & 1u) != 0u {
        let full_size = uniforms.size * uniforms.depth_scale;
        return (vec2<f32>(coord) * uniforms.depth_scale + vec2<f32>(taau_jitter()) + 0.5) / full_size;
    }
    return (vec2<f32>(coord) + 0.5) * uniforms.inv_size;
}

// Clear the angular sectors [h.x, h.y) (normalized to [0,1] across the slice) from the
// visibility bitfield. occ starts all-ones (fully visible); occluders AND away the sectors
// they cover. Shift amounts are kept below 32: WGSL takes a runtime shift amount modulo 32, so a
// shift by 32 would silently shift by 0.
fn carve_occluded_sectors(occ: u32, h: vec2<f32>) -> u32 {
    let a = min(u32(clamp(h.x, 0.0, 1.0) * 32.0), 31u);
    let e = u32(clamp(h.y, 0.0, 1.0) * 32.0);
    let b = select(0u, e - a, e > a);
    let bs = min(b, 31u);
    let ones = select((1u << bs) - 1u, 0xFFFFFFFFu, b >= 32u);
    return occ & ~(ones << a);
}

// One marched sample: view-space delta from the centre -> front/back horizon angles of a thick
// occluder, mapped into [0,1] across the slice (centred on the projected-normal angle n) and run
// through a smoothstep approximating cosine weighting, then carved out of the visibility mask.
// `flip` selects the negative-direction mapping (front/back pair negated and swapped onto the
// opposite half of the slice).
fn carve_sample(occ: u32, dvec: vec3<f32>, v: vec3<f32>, n: f32, t_base: f32, depth_range: f32, flip: bool) -> u32 {
    // Occluder thickness fades with the view-space depth difference: crevice walls (small depth
    // difference even when laterally far) keep full thickness, while silhouette jumps fade to
    // nothing so they do not halo past outlines.
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
    hh = hh * hh * (3.0 - 2.0 * hh); // approximate cosine-lobe weighting
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

// Geometric (face) normal of the depth surface at the centre pixel, view space: four MIP-0 taps at
// +/-1 pixel, cross product of the position deltas, each axis using the side with the smaller
// depth step, oriented toward the camera (the cross product's sign is arbitrary). This is not a
// shading normal; it only defines the rejection plane in the march.
//
// Keep the taps at +/-1. A thin mid-distance feature is 1-2 chain pixels wide, so wider taps land
// on the background and return its plane, and the rejection then discards samples that really
// occlude the feature (docs/authored_normals.md 8.11a).
//
// `fallback` must be the shading normal, never a zero vector: the rejection is
// `dot(delta, geo_n) > 0`, so a zero geo_n rejects every sample and switches AO off entirely.
fn geometric_normal_view(uv: vec2<f32>, centre: vec3<f32>, fallback: vec3<f32>) -> vec3<f32> {
    let px = uniforms.inv_size;
    let r = load_sample_position(uv + vec2<f32>(px.x, 0.0), 0.0).xyz;
    let l = load_sample_position(uv - vec2<f32>(px.x, 0.0), 0.0).xyz;
    let d = load_sample_position(uv + vec2<f32>(0.0, px.y), 0.0).xyz;
    let u = load_sample_position(uv - vec2<f32>(0.0, px.y), 0.0).xyz;
    let ddx = select(centre - l, r - centre, abs(r.z - centre.z) < abs(l.z - centre.z));
    let ddy = select(centre - u, d - centre, abs(d.z - centre.z) < abs(u.z - centre.z));
    let g = cross(ddy, ddx);
    let len = length(g);
    if !(len > 1.0e-12) { // also catches NaN from a degenerate cross product
        return fallback;
    }
    let gn = g / len;
    return select(gn, -gn, dot(gn, centre) > 0.0);
}

@compute
@workgroup_size(8, 8, 1)
fn vbao(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coordinates = vec2<i32>(global_id.xy);
    let uv = chain_uv(pixel_coordinates);

    // The scene normal is read first because the edge-weight write depends on it. That write must
    // happen on every pixel, including the early-outs below: `depth_differences` is never cleared,
    // so a skipped pixel would hand the denoiser last frame's weights.
    let n_dims = vec2<f32>(textureDimensions(scene_normal));
    let n_texel = clamp(vec2<i32>(uv * n_dims), vec2<i32>(0i), vec2<i32>(n_dims) - vec2<i32>(1i));
    let scene_n = textureLoad(scene_normal, n_texel, 0i);
    let has_normal = scene_n.w >= 0.5;

    let raw_depth = calculate_neighboring_depth_differences(pixel_coordinates, has_normal);
    if raw_depth <= 0.0 {
        // Reversed-Z background/sky: fully visible.
        textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(1.0, 0.0, 0.0, 0.0));
        return;
    }

    var pixel_position = reconstruct_view_space_position(raw_depth, uv);
    // The scene normal read above is full resolution and already in view space. In half-res mode
    // chain_uv() carries the temporal jitter, so this reads the chain pixel's jittered full-res
    // texel and accumulation recovers full-res normal detail.
    //
    // Alpha 0: no authored normal here (geometry drawn without normals, such as billboards, which
    // still writes depth like any surface). With no orientation to build a hemisphere from, the
    // pixel is fully visible.
    if !has_normal {
        textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(1.0, 0.0, 0.0, 0.0));
        return;
    }
    // Renormalize: interpolation and quantization both denormalize the stored direction. The alpha
    // test must come first: a no-normal draw stores (0.5,0.5,0.5,0), which decodes to a zero
    // vector and would normalize() to NaN.
    let pixel_normal = normalize(scene_n.xyz * 2.0 - 1.0);
    // geo_n is the depth surface's face normal and does not shade the AO; `pixel_normal` centres
    // the visibility mask and carries the cosine lobe. geo_n only decides which samples lie below
    // the surface (the rejection in the march loop). It is needed because an authored normal is
    // smoothed and tilts off its triangle (often by 10-30 degrees on low-poly terrain), so a
    // hemisphere around it alone would let samples in the surface's own plane occlude it
    // (docs/authored_normals.md 8.11).
    let geo_n = geometric_normal_view(uv, pixel_position, pixel_normal);
    pixel_position *= 1.0 - uniforms.depth_bias; // bias toward the camera suppresses self-occlusion
    let view_vec = normalize(-pixel_position);
    // Important: no camera-facing flip of the authored normal, at any threshold. dot(n, view_ray)
    // varies across a flat surface, so a flip on it negates everything past one line and seams
    // large ground planes at grazing angles (docs/authored_normals.md 2a).
    let normal = pixel_normal;

    // Base thickness grows logarithmically with the view-space radius (keeps close-up foliage from
    // overdarkening), which thins out distant occlusion; thick_dist_scale adds a term proportional
    // to the radius so mid/far occluders still clear meaningful sector spans.
    let abs_z = max(-pixel_position.z, 1.0e-4);
    // Radius ramp: from effect_radius to radius_far across [ramp_start, ramp_end] world units of
    // view depth, for tight contact detail up close and broad occlusion at range (world units
    // rather than far-plane fractions, because the far plane varies per stage). radius_far 0
    // disables the ramp.
    var eff_radius = uniforms.effect_radius;
    if uniforms.radius_far > 0.0 {
        eff_radius = mix(uniforms.effect_radius, uniforms.radius_far,
            smoothstep(uniforms.radius_ramp_start,
                max(uniforms.radius_ramp_end, uniforms.radius_ramp_start + 1.0), abs_z));
    }
    let view_radius = abs_z * eff_radius;
    // On-screen radius in chain pixels. For a fixed eff_radius it does not depend on depth, so it
    // only grows across the ramp band. Clamped to [4 px, radius_max * height]: with the default
    // Far Radius the cap engages beyond roughly 4,600 world units at a 60-degree vertical FOV. The
    // cap does not shrink view_radius, so thickness and fade range still use the uncapped radius.
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

            // Geometric rejection: only samples above the surface's own plane (geo_n) can occlude
            // it. Without this, the tilt of the smoothed authored normal would let the surface
            // occlude itself and shade areas with no occluder nearby. Sky samples (w = 0) are
            // skipped.
            let sp = load_sample_position(uv + offset, sample_mip_level);
            if sp.w > 0.0 && dot(sp.xyz - pixel_position, geo_n) > 0.0 {
                occ = carve_sample(occ, sp.xyz - pixel_position, view_vec, n, t_base, depth_range, false);
            }
            let sn = load_sample_position(uv - offset, sample_mip_level);
            if sn.w > 0.0 && dot(sn.xyz - pixel_position, geo_n) > 0.0 {
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
