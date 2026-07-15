// VBAO (Visibility Bitmask Ambient Occlusion) - fullscreen composite.
//
// Multiplies the accumulated (or denoised, when temporal accumulation is off) AO visibility over
// the scene. Based on Encounter's ao_mod demo composite with two additions:
//  - DEPTH-AWARE upscale: the 4 bilinear taps are weighted by depth agreement with this pixel,
//    so AO does not bleed across silhouettes when the AO chain runs at half resolution (a plain
//    bilinear sample smears an object's AO onto the background behind its outline).
//  - Contrast: a final value power that deepens/softens the occlusion falloff.
//
// Debug views:
//   1 = AO visibility as grayscale (the exact term the composite would apply)
//   2 = view-space normals reconstructed from depth (keep in sync with vbao.wgsl)
//   3 = the preprocessed depth input
//   4 = depth staircase detector

struct Uniforms {
    projection: mat4x4f,
    inverse_projection: mat4x4f,
    reproject: mat4x4f,
    view_from_world: mat4x4f,  // layout-only: unused here, present so the shared uniform matches vbao.wgsl + the host
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
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var ambient_occlusion: texture_2d<f32>;
@group(0) @binding(1) var preprocessed_depth: texture_2d<f32>;
@group(0) @binding(2) var scene_depth_raw: texture_2d<f32>;
@group(0) @binding(3) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    // Fullscreen triangle
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn load_depth(pixel_coordinates: vec2<i32>) -> f32 {
    let coordinates = clamp(pixel_coordinates, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
    return textureLoad(preprocessed_depth, coordinates, 0i).r;
}

// Raw-snapshot depth at full render resolution.
fn load_raw_depth(pixel_coordinates: vec2<i32>) -> f32 {
    let size = vec2<i32>(textureDimensions(scene_depth_raw));
    let coordinates = clamp(pixel_coordinates, vec2<i32>(0i), size - 1i);
    return textureLoad(scene_depth_raw, coordinates, 0i).r;
}

// Depth-aware manual bilinear: each tap's bilinear weight is multiplied by how well its depth
// (AO-chain preprocessed depth, MIP 0) agrees with this render pixel's own raw depth. At full
// resolution the depths match and this reduces to a plain bilinear sample; at half resolution it
// keeps silhouettes crisp. If every tap disagrees (a 1px fringe), fall back to the nearest texel.
fn sample_visibility(uv: vec2f, reference_depth: f32) -> f32 {
    let coordinates = uv * uniforms.size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let max_coordinates = vec2i(uniforms.size) - 1i;
    let p00 = clamp(vec2i(base), vec2i(0i), max_coordinates);
    let p11 = clamp(vec2i(base) + 1i, vec2i(0i), max_coordinates);
    let depth_tolerance = max(reference_depth * 0.05, 1.0e-6);

    var sum = 0.0;
    var weight_sum = 0.0;
    for (var i = 0; i < 4; i += 1) {
        let tap = vec2i(select(p00.x, p11.x, (i & 1) != 0), select(p00.y, p11.y, (i & 2) != 0));
        let bw = select(1.0 - fraction.x, fraction.x, (i & 1) != 0) *
            select(1.0 - fraction.y, fraction.y, (i & 2) != 0);
        let dz = (load_depth(tap) - reference_depth) / depth_tolerance;
        let w = bw * exp2(-dz * dz);
        sum += w * textureLoad(ambient_occlusion, tap, 0i).r;
        weight_sum += w;
    }
    if weight_sum < 1.0e-4 {
        let nearest = clamp(vec2i(round(coordinates)), vec2i(0i), max_coordinates);
        return textureLoad(ambient_occlusion, nearest, 0i).r;
    }
    return sum / weight_sum;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

fn view_position_at(pixel_coordinates: vec2<i32>) -> vec3f {
    let depth = load_depth(pixel_coordinates);
    let uv = (vec2f(pixel_coordinates) + 0.5) * uniforms.inv_size;
    return reconstruct_view_space_position(depth, uv);
}

fn reconstruct_normal(pixel_coordinates: vec2<i32>, pixel_position: vec3f, depth_center: f32) -> vec3f {
    let depth_left1 = load_depth(pixel_coordinates + vec2<i32>(-1i, 0i));
    let depth_left2 = load_depth(pixel_coordinates + vec2<i32>(-2i, 0i));
    let depth_right1 = load_depth(pixel_coordinates + vec2<i32>(1i, 0i));
    let depth_right2 = load_depth(pixel_coordinates + vec2<i32>(2i, 0i));
    let depth_top1 = load_depth(pixel_coordinates + vec2<i32>(0i, -1i));
    let depth_top2 = load_depth(pixel_coordinates + vec2<i32>(0i, -2i));
    let depth_bottom1 = load_depth(pixel_coordinates + vec2<i32>(0i, 1i));
    let depth_bottom2 = load_depth(pixel_coordinates + vec2<i32>(0i, 2i));

    let use_left = abs(2.0 * depth_left1 - depth_left2 - depth_center) <
        abs(2.0 * depth_right1 - depth_right2 - depth_center);
    let use_top = abs(2.0 * depth_top1 - depth_top2 - depth_center) <
        abs(2.0 * depth_bottom1 - depth_bottom2 - depth_center);

    var ddx: vec3f;
    if use_left {
        ddx = pixel_position - view_position_at(pixel_coordinates + vec2<i32>(-1i, 0i));
    } else {
        ddx = view_position_at(pixel_coordinates + vec2<i32>(1i, 0i)) - pixel_position;
    }
    var ddy: vec3f;
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

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    if uniforms.debug_view == 2u {
        // Reconstructed view-space normals, [-1,1] -> RGB
        let pixel = vec2<i32>(in.uv * uniforms.size);
        let depth = load_depth(pixel);
        let uv = (vec2f(pixel) + 0.5) * uniforms.inv_size;
        let position = reconstruct_view_space_position(depth, uv);
        let normal = reconstruct_normal(pixel, position, depth);
        return vec4f(normal * 0.5 + 0.5, 1.0);
    }
    if uniforms.debug_view == 3u {
        // Preprocessed depth as an exponential distance gradient (white = near, black = far).
        // The range is adjustable (debugDepthRange, world units) so the gradient can span
        // whatever scene scale is being inspected.
        let pixel = vec2<i32>(in.uv * uniforms.size);
        let position = view_position_at(pixel);
        let value = exp(-max(-position.z, 0.0) * uniforms.inv_debug_depth);
        return vec4f(value, value, value, 1.0);
    }
    if uniforms.debug_view == 4u {
        // Staircase detector on the raw snapshot depth
        let size = vec2f(textureDimensions(scene_depth_raw));
        let pixel = vec2<i32>(in.uv * size);
        let d_center = load_raw_depth(pixel);
        let d_left = load_raw_depth(pixel + vec2<i32>(-1i, 0i));
        let d_right = load_raw_depth(pixel + vec2<i32>(1i, 0i));
        let d_top = load_raw_depth(pixel + vec2<i32>(0i, -1i));
        let d_bottom = load_raw_depth(pixel + vec2<i32>(0i, 1i));
        let gradient_x = abs(d_right - d_left) * 0.5;
        let curvature_x = abs(d_right - 2.0 * d_center + d_left);
        let gradient_y = abs(d_bottom - d_top) * 0.5;
        let curvature_y = abs(d_bottom - 2.0 * d_center + d_top);
        let ratio_x = curvature_x / max(gradient_x, 1e-12);
        let ratio_y = curvature_y / max(gradient_y, 1e-12);
        return vec4f(saturate(ratio_x), saturate(ratio_y), 0.0, 1.0);
    }

    let full_size = vec2f(textureDimensions(scene_depth_raw));
    let reference_depth = load_raw_depth(vec2<i32>(in.uv * full_size));
    // The AO source is either full-res (temporal history, or full-res mode) or half-res (temporal
    // off). At full res the reconstruction already happened in the temporal upsampler, so read it
    // 1:1; at half res do the depth-aware bilinear upscale here.
    var visibility: f32;
    if all(textureDimensions(ambient_occlusion) == vec2<u32>(full_size)) {
        let px = clamp(vec2<i32>(in.uv * full_size), vec2<i32>(0i), vec2<i32>(full_size) - 1i);
        visibility = textureLoad(ambient_occlusion, px, 0i).r;
    } else {
        visibility = sample_visibility(in.uv, reference_depth);
    }
    // Black point: remove a small uniform occlusion floor so flat, open surfaces read as exactly
    // 1 (no darkening) while real crevices are preserved and rescaled to full range.
    let occlusion = clamp(
        ((1.0 - visibility) - uniforms.black_point) / max(1.0 - uniforms.black_point, 1.0e-3),
        0.0, 1.0);
    visibility = 1.0 - occlusion;
    // Contrast: final value power - deepens (contrast > 1) or lifts (< 1) the occlusion falloff.
    visibility = pow(clamp(visibility, 0.0, 1.0), uniforms.contrast);
    // Distance fade: fade the AO out across [fade_start, fade_end] WORLD UNITS of view depth
    // (same scale as the radius ramp; not far-plane fractions - see vbao.wgsl). TP washes
    // distant terrain toward fog, and full-strength AO over the haze reads as harsh shading
    // floating on it; this drives the term back to 1 (no darkening) with distance.
    if (uniforms.flags & 4u) != 0u && reference_depth > 0.0 {
        let view_position = reconstruct_view_space_position(reference_depth, in.uv);
        let fade = smoothstep(uniforms.fade_start, max(uniforms.fade_end, uniforms.fade_start + 1.0),
            max(-view_position.z, 0.0));
        visibility = mix(visibility, 1.0, fade);
    }
    let value = clamp(mix(1.0, visibility, uniforms.intensity), 0.0, 1.0);
    return vec4f(value, value, value, 1.0);
}
