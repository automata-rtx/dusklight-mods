// VBAO (visibility-bitmask ambient occlusion) pass.
//
// The pass framework (MIP-prefiltered depth reads, hilbert/R2 noise, edge output for the
// spatial denoiser) follows Encounter's ao_mod demo,
// which is ported from Bevy Engine's SSAO (MIT OR Apache-2.0) / Intel XeGTAO (MIT); see
// res/licenses/. The shading normal is NOT reconstructed here - it comes from the gfx service.
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
    radius_falloff: f32,   // occluder thickness taper over the outer fraction of the radius; 0 = off
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var preprocessed_depth: texture_2d<f32>;
@group(0) @binding(1) var hilbert_index_lut: texture_2d<u32>;
@group(0) @binding(2) var ambient_occlusion: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var depth_differences: texture_storage_2d<r32uint, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;
// The game's authored surface normals, snapshotted once per frame by GfxService immediately after
// the opaque lists. Full render resolution, VIEW SPACE, encoded xyz * 0.5 + 0.5 with alpha 1 where
// the normal is usable and 0 where it is not. The ATTACHMENT's coverage is exactly the depth
// buffer's - a draw writes a normal iff it writes depth - but alpha 1 is NOT implied by depth
// coverage: a draw with no NRM vertex attribute writes depth and stores alpha 0, and so does a
// vertex normal that interpolation cancelled to zero. Treat alpha as the only validity test.
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
// `has_normal` false zeroes this pixel's edge weights instead of writing the depth-derived ones.
// That is not cosmetic. The denoiser gates purely on depth (denoise.wgsl), and a pixel we skip for
// having no authored normal is written as FULLY VISIBLE - so if it kept normal edge weights, that
// 1.0 would bleed into its neighbours at full strength and read as a bright rim. Sky never did this
// because sky is depth-DISCONTINUOUS, so the depth gate already rejected it; a depth-writing draw
// that simply carries no NRM vertex attribute is depth-CONTINUOUS with the surface around it and
// sails straight through. Zeroing here makes the rejection symmetric, because denoise.wgsl weights
// each tap by the NEIGHBOUR's opposing edge: the pixel neither bleeds out nor is polluted.
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
// position agree. Otherwise this is the plain chain-space texel center (unchanged behavior).
fn chain_uv(coord: vec2<i32>) -> vec2<f32> {
    if uniforms.depth_scale.x >= 1.5 && (uniforms.flags & 1u) != 0u {
        let full_size = uniforms.size * uniforms.depth_scale;
        return (vec2<f32>(coord) * uniforms.depth_scale + vec2<f32>(taau_jitter()) + 0.5) / full_size;
    }
    return (vec2<f32>(coord) + 0.5) * uniforms.inv_size;
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

// GEOMETRIC (face) normal of the depth surface at the centre pixel, view space. Four MIP-0 taps and
// a cross product of the screen-space position deltas, side-selected on the smaller depth step.
//
// The +/-1 tap radius is deliberate. At mid distance a thin feature is 1-2 chain pixels wide, so a
// wider stencil (one tapping +/-2 pixels) lands both far taps on the BACKGROUND and returns the
// background's plane. The rejection then discards samples that legitimately occlude the thin
// feature and its occlusion breaks up per pixel, which through the half-res upscale reads as lower
// resolution. A +/-1 tap still has a chance of straddling the feature. Wide taps are the right tool
// for a SHADING normal, where silhouette robustness is what you want - but this is a per-pixel
// plane, and the two wants are opposite. See docs/authored_normals.md 8.11a.
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

    // The scene normal is read up front because the edge-weight write below depends on whether this
    // pixel has one. `depth_differences` is persistent and never cleared, so that write must happen
    // on EVERY pixel including the ones we early-out on - skipping it leaves last frame's weights
    // for the denoiser's neighbourhood read.
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
    // Normal source: the service's scene normal snapshot, read above. It is FULL RES and already in
    // VIEW SPACE, so there is no basis conversion to do - in half-res mode chain_uv() already
    // carries the TAAU jitter, so we sampled this chain pixel's jittered full-res texel and temporal
    // accumulation integrates full-res normal detail even at half res.
    //
    // Alpha 0 means nothing with an authored normal covers this pixel: the sky, geometry drawn
    // without normals (billboards), or a draw whose NRM vertex attribute is simply absent - that
    // last one writes depth like any other surface, so this is NOT only a silhouette case. There is
    // no surface orientation to build a hemisphere from, so it takes full visibility.
    if !has_normal {
        textureStore(ambient_occlusion, pixel_coordinates, vec4<f32>(1.0, 0.0, 0.0, 0.0));
        return;
    }
    // Renormalize: interpolation and quantization both denormalize the stored direction. Note this
    // is why the alpha test comes first - a no-normal draw stores (0.5,0.5,0.5,0), which decodes to
    // a zero vector and would normalize() to NaN.
    let pixel_normal = normalize(scene_n.xyz * 2.0 - 1.0);
    // geo_n is the GEOMETRIC (face) normal of the depth surface, and is not what shades the AO -
    // `pixel_normal` above still centres the visibility mask and carries the cosine lobe. Its only
    // job is to answer "which directions lie below the surface that actually occludes", which is a
    // property of the GEOMETRY rather than of the artist's smoothed vertex normal. An authored
    // normal is deliberately NOT perpendicular to its triangle, so on smoothed low-poly terrain the
    // two differ by 10-30 degrees and the mask would otherwise swallow the very plane the samples
    // lie in. See the rejection in the march loop, and docs/authored_normals.md 8.11.
    //
    // It is a 4-tap +/-1 plane on purpose: a wider tap lands both far samples on the BACKGROUND
    // across a thin mid-distance feature and returns the background's plane, and the rejection then
    // eats that feature's occlusion. See 8.11a.
    let geo_n = geometric_normal_view(uv, pixel_position, pixel_normal);
    pixel_position *= 1.0 - uniforms.depth_bias; // bias toward the camera suppresses self-occlusion
    let view_vec = normalize(-pixel_position);
    // NO camera-facing flip. The scene normal carries the sign the game gave it and must not be
    // second-guessed: dot(n, view_ray) is not a property of the surface (the ray sweeps across the
    // screen), so flipping on it negates everything past the line where the ray crosses the surface
    // plane and seams flat ground. The only flip left in this file is inside geometric_normal_view,
    // whose cross product genuinely has an arbitrary sign. See docs/authored_normals.md 2a.
    //
    // This used to flip at dot(normal, view_vec) < -0.15, nominally for double-sided foliage seen
    // from behind, and it seamed flat ground: on a large surface at a grazing angle the test flips
    // along a line and negates everything past it, nearest the horizon where ground planes are
    // widest. The threshold was the wrong thing to argue about - the test itself was. It is gone,
    // and the full account of the three separate places it had to be deleted from is in
    // docs/authored_normals.md 2a.
    let normal = pixel_normal;

    // Depth-proportional radius: constant screen-space search radius. Base thickness grows
    // logarithmically with the view-space radius (keeps close-up foliage from overdarkening),
    // which starves distant occlusion - the log term becomes a vanishing fraction of the
    // radius - so thick_dist_scale adds a radius-PROPORTIONAL floor that keeps mid/far
    // occluders carving meaningful sector spans.
    let abs_z = max(-pixel_position.z, 1.0e-4);
    // Distance-scaled radius: ramp the (depth-proportional) radius from effect_radius up to
    // radius_far across [ramp_start, ramp_end] WORLD UNITS of view depth - tight contact detail
    // up close, broad occlusion that gives distant landmarks depth at range. World units (not a
    // far-plane fraction): the far plane is per-stage and far beyond the visible field, so
    // fractions of it are both scene-dependent and absurdly compressed. radius_far 0 disables.
    var eff_radius = uniforms.effect_radius;
    if uniforms.radius_far > 0.0 {
        eff_radius = mix(uniforms.effect_radius, uniforms.radius_far,
            smoothstep(uniforms.radius_ramp_start,
                max(uniforms.radius_ramp_end, uniforms.radius_ramp_start + 1.0), abs_z));
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
            // Radial jitter, advanced per (slice, step) along a golden-ratio sequence - the demo's
            // XeGTAO inner loop does the same. This used to be one per-pixel offset (`noise.y`)
            // reused for every step of every slice, which leaves the slices radially CORRELATED:
            // they all sample the same set of radii, so a missed or hit occluder is missed or hit
            // by all of them at once and the error shows as concentric structure rather than as
            // noise the denoiser and the temporal accumulator can average away. Same sample count,
            // same expected value - only the distribution of the error changes.
            let step_noise = fract(noise.y + (s * steps + (step - 1.0)) * 0.6180339887498949);
            let s01 = clamp((step - step_noise) / steps, 0.0, 1.0);
            let radial = s01 * s01;            // x^2 sample distribution
            let dist = radial * radius_pix;
            let offset = dir * dist * uniforms.inv_size;
            // MIP level from the sample's screen distance in pixels (bandwidth optimization).
            let sample_mip_level = clamp(log2(max(dist, 1.0)) - 3.3, 0.0, 4.0);
            // Radial falloff, off by default. GTAO (and the demo) fades a sample's horizon back
            // toward "no occlusion" over the outer part of the search radius, so an occluder
            // crossing the radius boundary fades out instead of popping; VBAO's only distance term
            // is `thick_fade`, which acts on the DEPTH difference and so does nothing for an
            // occluder that leaves the radius sideways at constant depth. The bitmask analogue of
            // pulling the horizon back is tapering the occluder's THICKNESS, which shrinks the
            // sector span it carves to nothing - the same mechanism thick_fade already uses.
            // radius_falloff is the fraction of the radius the taper spans (the demo's constant is
            // 0.615); 0 disables it and restores the hard cutoff exactly.
            var t_step = t_base;
            if uniforms.radius_falloff > 0.0 {
                let falloff_from = 1.0 - clamp(uniforms.radius_falloff, 0.0, 1.0);
                t_step = t_base * (1.0 - smoothstep(falloff_from, 1.0, radial));
            }

            // Geometric rejection: only samples ABOVE the occluding surface's own plane can
            // occlude it. This is always load-bearing now, because the shading normal is always the
            // authored one: the artist's smoothed vertex normal tilts off the geometry by design,
            // and a hemisphere centred on it swallows the very plane the samples lie in - which
            // reads as AO shading on surfaces with no occluder anywhere near them. (It used to be a
            // no-op whenever the shading normal came from a depth reconstruction, since that normal
            // IS the plane. There is no such path any more.)
            let sp = load_sample_position(uv + offset, sample_mip_level);
            if sp.w > 0.0 && dot(sp.xyz - pixel_position, geo_n) > 0.0 {
                occ = carve_sample(occ, sp.xyz - pixel_position, view_vec, n, t_step, depth_range, false);
            }
            let sn = load_sample_position(uv - offset, sample_mip_level);
            if sn.w > 0.0 && dot(sn.xyz - pixel_position, geo_n) > 0.0 {
                occ = carve_sample(occ, sn.xyz - pixel_position, view_vec, n, t_step, depth_range, true);
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
