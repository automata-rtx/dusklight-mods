// SSILVB (Screen Space Indirect Lighting with Visibility Bitmask) - sampling pass.
//
// Implements Therrien, Levesque, Gilet 2023 ("Screen Space Indirect Lighting with Visibility
// Bitmask", The Visual Computer / arXiv 2301.11376) on VBAO's sampling chain: the pass framework
// (MIP-prefiltered depth reads, hilbert/R2 noise, edge output for the spatial denoiser) follows
// Encounter's ao_mod demo, ported from Bevy Engine's SSAO (MIT OR Apache-2.0) / Intel XeGTAO
// (MIT); see res/licenses/. The slice walk and 32-sector bitmask carving are shared with our
// VBAO mod (same estimator, same thickness model, same depth-proportional radius).
//
// The GI extension (paper Algorithm 1, line 23): when a marched sample covers sector bits `bits`,
// the subset that is NEWLY occluded this step (`bits & occ`, with occ holding still-visible
// sectors) is exactly the solid angle through which that sample's surface is visible from the
// receiver. The sample's radiance - scene color MIP (linear, from preprocess_color.wgsl) times
// the sample's own cosine toward the receiver - is accumulated with that weight BEFORE the mask
// is updated. That ordering is what lets light pass behind thin occluders and prevents any
// double counting: a surface behind an already-occluded sector contributes nothing.
//
// RECEIVER COSINE: the sector mapping runs the horizon angles through a smoothstep cosine-lobe
// warp (inherited from VBAO), so sector COUNTS already integrate the receiver's cos(theta) lobe.
// The paper's explicit (n_p . l_j) factor is therefore deliberately absent - including it would
// double-apply the receiver cosine. Only the emitter cosine (n_j . -l_j) is explicit.
//
// Output: rgba16float (GI.rgb in linear light, AO visibility in .a).

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
    sky_saturation: f32,     // sky tint saturation: 0 = white light at sky brightness, 1 = full
    gi_saturation: f32,      // bounce chroma boost applied in the composite (1 = neutral)
    _pad0: f32,
}

@group(0) @binding(0) var preprocessed_depth: texture_2d<f32>;
@group(0) @binding(1) var hilbert_index_lut: texture_2d<u32>;
@group(0) @binding(2) var gi_output: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var depth_differences: texture_storage_2d<r32uint, write>;
@group(0) @binding(4) var<uniform> uniforms: Uniforms;
// Depth to Normal provider output (world-space normal + raw depth), full render resolution.
// Hard dependency: receiver normal AND per-march-sample emitter normals both come from it.
@group(0) @binding(5) var d2n_normal: texture_2d<f32>;
// Linear scene radiance MIP chain (preprocess_color.wgsl), chain resolution.
@group(0) @binding(6) var color_chain: texture_2d<f32>;
// Smoothed 1x1 sky estimate (preprocess_color.wgsl reduce_sky): rgb = linear sky radiance
// sampled from the game's own time-of-day-tinted skybox pixels, a = confidence.
@group(0) @binding(7) var sky_ambient: texture_2d<f32>;

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

// World-space normal from the Depth to Normal provider (full render resolution), rotated into
// view space. Used for the receiver AND for every march sample's emitter cosine.
fn load_view_normal(uv: vec2<f32>) -> vec3<f32> {
    let n_dims = vec2<f32>(textureDimensions(d2n_normal));
    let texel = clamp(vec2<i32>(uv * n_dims), vec2<i32>(0i), vec2<i32>(n_dims) - vec2<i32>(1i));
    let world_n = textureLoad(d2n_normal, texel, 0i).xyz;
    let r = uniforms.view_from_world;
    return normalize(r[0].xyz * world_n.x + r[1].xyz * world_n.y + r[2].xyz * world_n.z);
}

// The angular sectors [h.x, h.y) (normalized to [0,1] across the slice) as a 32-bit field.
// Shift amounts are kept < 32 (WGSL UB).
fn sector_bits(h: vec2<f32>) -> u32 {
    let a = min(u32(clamp(h.x, 0.0, 1.0) * 32.0), 31u);
    let e = u32(clamp(h.y, 0.0, 1.0) * 32.0);
    let b = select(0u, e - a, e > a);
    let bs = min(b, 31u);
    let ones = select((1u << bs) - 1u, 0xFFFFFFFFu, b >= 32u);
    return ones << a;
}

// One marched sample: view-space delta from the center -> front/back horizon angles of a thick
// occluder, mapped into [0,1] across the slice (centred on the projected-normal angle n) and run
// through a cosine-lobe smoothstep, then returned as the sector bits the sample covers. `flip`
// selects the -direction mapping (front/back pair negated and swapped onto the opposite half of
// the slice). Same math as VBAO's carve_sample, returning the bits instead of carving in place
// (the GI accumulate needs the pre-carve mask delta).
fn sample_sector_bits(dvec: vec3<f32>, v: vec3<f32>, n: f32, t_base: f32, depth_range: f32, flip: bool) -> u32 {
    // Occluder thickness fades with the VIEW-SPACE DEPTH difference: connected crevice walls
    // (small depth diff even when laterally far) keep full thickness so deep seams reach full
    // darkness; silhouette jumps fade to nothing and stop haloing past outlines.
    let t_eff = t_base * clamp(1.0 - abs(dvec.z) / depth_range, 0.0, 1.0);
    if t_eff <= 1.0e-4 {
        return 0u;
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
    return sector_bits(hh);
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

// Linear scene radiance at a march sample, from the SAME MIP level as its depth fetch: the
// pre-averaged level approximates the mean radiance of the surface span a wide sector sees.
fn load_sample_radiance(uv: vec2<f32>, sample_mip_level: f32) -> vec3<f32> {
    let mip_level = i32(sample_mip_level + 0.5);
    let mip_size = max(vec2<i32>(uniforms.size) >> vec2<u32>(u32(mip_level)), vec2<i32>(1i));
    let coords = clamp(vec2<i32>(uv * vec2<f32>(mip_size)), vec2<i32>(0i), mip_size - 1i);
    return textureLoad(color_chain, coords, mip_level).rgb;
}

// The bounce contribution of one march sample (paper Algorithm 1 line 23): radiance times the
// newly-un-occluded sector fraction (receiver cosine folded into the warped sectors - see the
// header note) times the EMITTER cosine (surfaces facing away from the receiver send no light).
// A luminance ceiling on the radiance (firefly guard) keeps a single very bright emissive spot
// (a fairy core boosted by emissive_boost) from spiking one sample into visible flicker that
// the temporal clamp then has to fight.
fn sample_bounce(sample_uv: vec2<f32>, mip: f32, sample_pos: vec3<f32>, pixel_pos: vec3<f32>,
    newly: u32) -> vec3<f32> {
    let l = normalize(sample_pos - pixel_pos);
    let emitter_cos = clamp(dot(load_view_normal(sample_uv), -l), 0.0, 1.0);
    if emitter_cos <= 0.0 {
        return vec3<f32>(0.0);
    }
    var radiance = load_sample_radiance(sample_uv, mip);
    let luma = dot(radiance, vec3<f32>(0.299, 0.587, 0.114));
    // The ceiling SCALES with emissive_boost: the boost exists to stand in for the HDR range
    // the LDR target clipped away, so a fixed cap would silently neutralize the very slider
    // that raises it (the v0.9.2 bug: boosts past ~100% did nothing).
    let luma_cap = max(4.0, 2.0 * uniforms.emissive_boost);
    radiance *= min(1.0, luma_cap / max(luma, 1.0e-4));
    return radiance * (f32(countOneBits(newly)) / 32.0) * emitter_cos;
}

// Inverse of the cosine-lobe smoothstep warp (y = 3t^2 - 2t^3): recovers the LINEAR angular
// position of a warped sector coordinate. Exact closed form; used once per slice to convert the
// visible-arc midpoint back into an angle for the bent (sky) direction.
fn unwarp_sector(y: f32) -> f32 {
    return 0.5 - sin(asin(clamp(1.0 - 2.0 * y, -1.0, 1.0)) / 3.0);
}

@compute
@workgroup_size(8, 8, 1)
fn ssilvb(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coordinates = vec2<i32>(global_id.xy);
    let uv = chain_uv(pixel_coordinates);

    let raw_depth = calculate_neighboring_depth_differences(pixel_coordinates);
    if raw_depth <= 0.0 {
        // Reversed-Z background/sky: fully visible, no bounce.
        textureStore(gi_output, pixel_coordinates, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    var pixel_position = reconstruct_view_space_position(raw_depth, uv);
    // Receiver normal from the Depth to Normal provider (hard dependency - the host skips the
    // frame entirely when the provider has no scene yet), rotated world -> view. In half-res
    // mode the (jittered) full-res position walks full-res normal detail, which temporal
    // accumulation integrates - same trick as VBAO's provider path.
    var pixel_normal = load_view_normal(uv);
    pixel_position *= 1.0 - uniforms.depth_bias; // bias toward the camera suppresses self-occlusion
    let view_vec = normalize(-pixel_position);
    var normal = pixel_normal;
    // Face the normal toward the camera only when CLEARLY back-facing (double-sided foliage seen
    // from behind); the margin keeps grazing surfaces from toggling per pixel.
    if dot(normal, view_vec) < -0.15 {
        normal = -normal;
    }

    // Depth-proportional radius with a distance ramp; base thickness grows logarithmically with
    // the view-space radius plus a radius-proportional floor. All inherited from VBAO - see
    // vbao.wgsl for the full rationale.
    let abs_z = max(-pixel_position.z, 1.0e-4);
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
    let gi_enabled = (uniforms.flags & 8u) != 0u;

    // Directional sky light: the smoothed sky radiance arrives through each slice's VISIBLE
    // sectors, weighted by how sky-facing the visible arc's bent direction is (world up rotated
    // into view space). A floor pixel under open sky gets the full tint; a wall gets partial;
    // anything under cover gets none - the paper's "directionally occluded ambient" with the
    // game's own time-of-day sky as the ambient source.
    let sky = textureLoad(sky_ambient, vec2<i32>(0i, 0i), 0i);
    let sky_on = (uniforms.flags & 128u) != 0u && sky.a > 0.001 && uniforms.sky_intensity > 0.0;
    // Saturation control: full sky color can cast a blue pall over warm areas (a light-blue
    // zenith over orange desert); pulling the tint toward its own luminance keeps the
    // brightness-and-direction behavior while softening the hue shift.
    let sky_luma = dot(sky.rgb, vec3<f32>(0.299, 0.587, 0.114));
    let sky_tint = mix(vec3<f32>(sky_luma), sky.rgb, clamp(uniforms.sky_saturation, 0.0, 1.5));
    let sky_radiance = sky_tint * (sky.a * uniforms.sky_intensity);
    let up_view = normalize(uniforms.view_from_world[1].xyz); // world +Y in view space

    var visibility = 0.0;
    var gi = vec3<f32>(0.0);
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

        // occ holds STILL-VISIBLE sectors (all ones = nothing occluded). Each sample's newly
        // occluded subset (bits & occ) is its bounce weight, taken before the mask update.
        var occ: u32 = 0xFFFFFFFFu;
        var slice_gi = vec3<f32>(0.0);
        for (var step = 1.0; step <= steps; step += 1.0) {
            let s01 = clamp((step - noise.y) / steps, 0.0, 1.0);
            let dist = s01 * s01 * radius_pix; // x^2 sample distribution
            let offset = dir * dist * uniforms.inv_size;
            // MIP level from the sample's screen distance in pixels (bandwidth optimization).
            let sample_mip_level = clamp(log2(max(dist, 1.0)) - 3.3, 0.0, 4.0);

            let sp = load_sample_position(uv + offset, sample_mip_level);
            if sp.w > 0.0 {
                let bits = sample_sector_bits(
                    sp.xyz - pixel_position, view_vec, n, t_base, depth_range, false);
                let newly = bits & occ;
                if gi_enabled && newly != 0u {
                    slice_gi += sample_bounce(
                        uv + offset, sample_mip_level, sp.xyz, pixel_position, newly);
                }
                occ &= ~bits;
            }
            let sn = load_sample_position(uv - offset, sample_mip_level);
            if sn.w > 0.0 {
                let bits = sample_sector_bits(
                    sn.xyz - pixel_position, view_vec, n, t_base, depth_range, true);
                let newly = bits & occ;
                if gi_enabled && newly != 0u {
                    slice_gi += sample_bounce(
                        uv - offset, sample_mip_level, sn.xyz, pixel_position, newly);
                }
                occ &= ~bits;
            }
        }

        // Sky light through this slice's still-visible sectors. The visible set is usually one
        // contiguous arc, so its midpoint (between the lowest and highest visible sector,
        // unwarped back to a linear angle) is a cheap, good bent-direction estimate. The sector
        // count carries the cosine-weighted openness (same integral the AO term uses); the bent
        // direction gates it by sky-facingness, letting light in sideways through a window but
        // not "up" through a ceiling.
        let visible_count = f32(countOneBits(occ));
        if sky_on && visible_count > 0.0 {
            let lo = f32(firstTrailingBit(occ));
            let hi = f32(firstLeadingBit(occ));
            let mid = unwarp_sector(((lo + hi) * 0.5 + 0.5) / 32.0);
            // Invert the sector mapping: hh = (angle + n)/PI + 0.5 (pre-warp).
            let bent_angle = PI * (mid - 0.5) - n;
            let bent_dir = cos(bent_angle) * view_vec + sin(bent_angle) * tang;
            let sky_facing = smoothstep(-0.15, 0.5, dot(bent_dir, up_view));
            slice_gi += sky_radiance * (visible_count / 32.0) * sky_facing;
        }

        // Slice visibility = fraction of sectors still unoccluded; both terms weighted by the
        // projected normal length (the slice's share of the hemisphere).
        visibility += (visible_count / 32.0) * proj_n_len;
        gi += slice_gi * proj_n_len;
        norm_sum += proj_n_len;
    }

    var ao = 1.0;
    var gi_out = vec3<f32>(0.0);
    if norm_sum > 1.0e-4 {
        ao = clamp(visibility / norm_sum, 0.0, 1.0);
        gi_out = max(gi / norm_sum, vec3<f32>(0.0));
    }
    textureStore(gi_output, pixel_coordinates, vec4<f32>(gi_out, ao));
}
