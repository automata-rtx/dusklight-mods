// Bend Studio's screen-space shadows (Days Gone): WGSL port of the published bend_sss_gpu.h.
//
// Copyright 2023 Sony Interactive Entertainment.
// Licensed under the Apache License, Version 2.0; see licenses/BEND-SSS-APACHE-2.0.txt.
// Original code and integration notes: https://www.bendstudio.com
//
// Per pixel, a wavefront-cooperative ray is marched in screen space toward (or away from) the
// projected light position: each 64-thread workgroup covers one WAVE_SIZE-pixel ray segment,
// loads the depths along it once into workgroup memory, then every thread resolves its own
// pixel's occlusion against the shared, perspective-corrected samples. mod.cpp mirrors
// Bend::BuildDispatchList (src/bend_sss_cpu.h) to produce the dispatch grid; each dispatch
// binds its own uniform slot carrying the shared light coordinate + per-dispatch wave offset.
//
// Deltas from the HLSL original, which targets DXC + border-color samplers + wave intrinsics:
//  - The clamp-to-border point sampler is emulated with a bounds-checked textureLoad that
//    returns the far depth (WebGPU has no border address mode), so offscreen never casts.
//  - The wave-intrinsic workgroup early-out is omitted (WGSL uniformity analysis rejects
//    barriers behind it); only Bend's per-pixel skip remains, after the LDS barrier.
//  - Compile-time config is fixed: WAVE_SIZE 64, SAMPLE_COUNT 60 (= shadow length in pixels),
//    HARD_SHADOW_SAMPLES 4, FADE_OUT_SAMPLES 8, and the recommended defaults are hardcoded
//    (BilinearSamplingOffsetMode = false, UsePrecisionOffset = false).
//  - Reversed-Z is baked in: near depth = 1, far/sky depth = 0.
//
// Output: single-channel visibility, 1 = fully lit, 0 = fully shadowed.

const WAVE_SIZE: u32 = 64u;
const SAMPLE_COUNT: u32 = 60u;
const HARD_SHADOW_SAMPLES: u32 = 4u;
const FADE_OUT_SAMPLES: u32 = 8u;
const READ_COUNT: u32 = SAMPLE_COUNT / WAVE_SIZE + 2u;

const NEAR_DEPTH_VALUE: f32 = 1.0;
const FAR_DEPTH_VALUE: f32 = 0.0;
const Z_SIGN: f32 = -1.0; // near > far (reversed-Z)

// Mirror of SssUniforms in src/mod.cpp (keep byte layouts identical).
struct SssUniforms {
    light_coordinate: vec4f,  // xy = light pixel coordinates, z = light NDC depth, w = w sign
    wave_offset: vec2i,       // per-dispatch wavefront offset from BuildDispatchList
    surface_thickness: f32,   // assumed occluder thickness, fraction of the remaining depth range
    bilinear_threshold: f32,  // depth-difference fraction treated as a geometric edge
    shadow_contrast: f32,     // contrast boost on the shadow transition (>= 1)
    ignore_edge_pixels: u32,  // 1 = pixels detected as edges do not cast
    debug_mode: u32,          // 1 = write the edge-detect mask instead of the shadow
    receiver_bias: f32,       // extra receiver offset in shadow-window units; blunt acne knob
    range_falloff: f32,       // 1 / max shadow length in pixels (0 = full 60px trace)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var shadow_out: texture_storage_2d<r32float, write>;
@group(0) @binding(2) var<uniform> uniforms: SssUniforms;

var<workgroup> depth_data: array<f32, READ_COUNT * WAVE_SIZE>;

// Point sampling with clamp-to-border emulation: reads outside the screen return the far
// depth, so offscreen can never cast (the original relies on a border-color sampler).
fn read_depth(texel: vec2i) -> f32 {
    let size = vec2i(textureDimensions(scene_depth));
    if any(texel < vec2i(0)) || any(texel >= size) {
        return FAR_DEPTH_VALUE;
    }
    return textureLoad(scene_depth, texel, 0).r;
}

struct WavefrontExtents {
    delta_xy: vec2f,      // step to advance WAVE_SIZE pixels along the ray
    pixel_xy: vec2f,      // this thread's start pixel
    pixel_distance: f32,  // pixel distance from the light along the major axis
    x_axis_major: bool,   // abs(delta_xy.x) > abs(delta_xy.y)
}

// Start pixel coordinates for the pixels in the wavefront, plus the delta to the next pixel
// after WAVE_SIZE pixels along the ray.
fn compute_wavefront_extents(group_id: vec3u, thread_id: u32) -> WavefrontExtents {
    let xy_base = vec2i(group_id.yz) * i32(WAVE_SIZE) + uniforms.wave_offset;

    // Integer light position / fractional component.
    let light_xy = floor(uniforms.light_coordinate.xy) + 0.5;
    let light_xy_fraction = uniforms.light_coordinate.xy - light_xy;
    let reverse_direction = uniforms.light_coordinate.w > 0.0;

    let sign_xy = sign(xy_base);
    let horizontal = abs(xy_base.x + sign_xy.y) < abs(xy_base.y - sign_xy.x);
    var axis = vec2i(0, -sign_xy.x);
    if horizontal {
        axis = vec2i(sign_xy.y, 0);
    }

    // Apply the wave offset.
    let xy = axis * i32(group_id.x) + xy_base;
    let xy_f = vec2f(xy);

    // For interpolation to the light center, only the larger of the two axes matters.
    let x_axis_major = abs(xy_f.x) > abs(xy_f.y);
    let major_axis = select(xy_f.y, xy_f.x, x_axis_major);
    let major_axis_start = abs(major_axis);
    let major_axis_end = abs(major_axis) - f32(WAVE_SIZE);

    var ma_light_frac = select(light_xy_fraction.y, light_xy_fraction.x, x_axis_major);
    if major_axis > 0.0 {
        ma_light_frac = -ma_light_frac;
    }

    // Back into screen direction.
    let start_xy = xy_f + light_xy;

    // For the very innermost ring, interpolate to a pixel-centered UV so the UV -> pixel
    // rounding doesn't skip output pixels.
    let end_xy = mix(uniforms.light_coordinate.xy, start_xy,
        (major_axis_end + ma_light_frac) / (major_axis_start + ma_light_frac));

    // The major axis should be a round number.
    let xy_delta = start_xy - end_xy;

    // Inverse the read order when the ray points away from the light.
    let thread_step = f32(thread_id ^ select(WAVE_SIZE - 1u, 0u, reverse_direction));

    var extents: WavefrontExtents;
    extents.pixel_xy = mix(start_xy, end_xy, thread_step / f32(WAVE_SIZE));
    extents.pixel_distance = major_axis_start - thread_step + ma_light_frac;
    extents.delta_xy = xy_delta;
    extents.x_axis_major = x_axis_major;
    return extents;
}

@compute @workgroup_size(64)
fn cs_main(
    @builtin(workgroup_id) group_id: vec3u, @builtin(local_invocation_id) local_id: vec3u) {
    let thread_id = local_id.x;
    let extents = compute_wavefront_extents(group_id, thread_id);

    var sampling_depth: array<f32, READ_COUNT>;
    var shadowing_depth: array<f32, READ_COUNT>;
    var depth_thickness_scale: array<f32, READ_COUNT>;
    var sample_distance: array<f32, READ_COUNT>;

    let direction = -uniforms.light_coordinate.w;
    let write_xy = vec2i(floor(extents.pixel_xy));
    var pixel_xy = extents.pixel_xy;
    var is_edge = false;

    for (var i = 0u; i < READ_COUNT; i++) {
        // Depth is sampled twice per pixel per sample and interpolated with an edge-detect
        // filter. Interpolation only happens on the ray's minor axis - major-axis
        // coordinates sit at pixel centers.
        let read_xy = vec2i(floor(pixel_xy));
        let minor_axis = select(pixel_xy.x, pixel_xy.y, extents.x_axis_major);

        // With edge skipping enabled, an extreme value pushes edge samples out of range.
        let edge_skip = 1e20;

        let bilinear = fract(minor_axis) - 0.5;
        let sample_bias = select(-1, 1, bilinear > 0.0);
        let offset_xy = select(vec2i(sample_bias, 0), vec2i(0, sample_bias), extents.x_axis_major);

        let depth_a = read_depth(read_xy);
        let depth_b = read_depth(read_xy + offset_xy);

        // Depth thresholds (bilinear/shadow thickness) are a fractional ratio of the
        // difference between the sampled depth and the far clip depth.
        depth_thickness_scale[i] = abs(FAR_DEPTH_VALUE - depth_a);

        // If the depth variance crosses the edge threshold, fall back to point filtering.
        let use_point_filter =
            abs(depth_a - depth_b) > depth_thickness_scale[i] * uniforms.bilinear_threshold;
        if i == 0u {
            is_edge = use_point_filter;
        }

        // Bend's BilinearSamplingOffsetMode = false path: the pixel starts sampling at its
        // own depth; the depth it casts from is pushed away by the bilinear gradient across
        // the pixel (any sample in the wavefront may interpolate toward the neighbor).
        sampling_depth[i] = depth_a;
        let edge_depth = select(depth_a, edge_skip, uniforms.ignore_edge_pixels != 0u);
        let shadow_depth = depth_a + abs(depth_a - depth_b) * Z_SIGN;
        shadowing_depth[i] = select(shadow_depth, edge_depth, use_point_filter);

        sample_distance[i] = extents.pixel_distance + f32(WAVE_SIZE * i) * direction;

        // Step to the next read, WAVE_SIZE pixels along the ray.
        pixel_xy += extents.delta_xy * direction;
    }

    // Write the shadow depths to workgroup memory, perspective-corrected so all light rays
    // become parallel in this space.
    for (var i = 0u; i < READ_COUNT; i++) {
        var stored_depth = (shadowing_depth[i] - uniforms.light_coordinate.z) / sample_distance[i];
        if i != 0u {
            // Extended reads for pixels near the light can overshoot the light coordinate;
            // ignore those samples.
            stored_depth = select(1e10, stored_depth, sample_distance[i] > 0.0);
        }
        depth_data[i * WAVE_SIZE + thread_id] = stored_depth;
    }
    workgroupBarrier();

    // Sky pixels (and border reads outside the screen) receive no screen-space shadow. This
    // runs after the barrier because this thread's ray samples are still needed by the
    // other threads in the wavefront.
    if sampling_depth[0] <= FAR_DEPTH_VALUE {
        return;
    }

    // Perspective correct the receiver depth.
    var start_depth = (sampling_depth[0] - uniforms.light_coordinate.z) / sample_distance[0];

    // Start by reading the next value along the ray.
    let sample_index = thread_id + 1u;

    var shadow_value = vec4f(1.0);
    var hard_shadow = 1.0;

    // Inverse size of the shadowing window (see bend_sss_gpu.h for the full derivation):
    // scales the projected depth deltas so the window is 1.0 wide for the configured
    // surface thickness, with a clamped minimum width very close to the light.
    let depth_scale = min(sample_distance[0] + direction, 1.0 / uniforms.surface_thickness) *
        sample_distance[0] / depth_thickness_scale[0];
    start_depth = start_depth * depth_scale - Z_SIGN;

    // Receiver bias: blunt fallback knob. Adding a constant to every sample's depth delta
    // (AFTER the abs, so it is strictly one-sided and can only lighten - never fabricate a
    // shadow) pushes the near-surface response into the fully-lit clamp. It cannot separate
    // facet banding from genuine micro-shadows - the shadow-length falloff below is for that.
    let receiver_bias = uniforms.receiver_bias;

    // Shadow-length falloff: forgives depth deltas in proportion to the caster's distance
    // along the ray, so occlusion aligning within ~1/range_falloff pixels keeps full strength
    // and occlusion beyond it fades smoothly to nothing (a soft, tunable version of Bend's
    // own trace-tail fade-out). This is the facet-banding fix: on a low-poly convex surface
    // (Link's cap) the neighboring polygons genuinely occlude near the light terminator, band
    // by band, and that alignment happens at facet-scale distances - tens of pixels - while
    // genuine micro-detail (the Hylian shield insignia) shadows its receiver within a few
    // pixels of contact. Limiting the shadow length prunes the bands and keeps the detail.
    let range_falloff = uniforms.range_falloff;

    // The first samples produce a hard shadow: a single sample can fully shadow the pixel,
    // trading aliasing for grounding pixels very close to the caster.
    for (var i = 0u; i < HARD_SHADOW_SAMPLES; i++) {
        let forgive = range_falloff * f32(i + 1u) + receiver_bias;
        let depth_delta =
            abs(start_depth - depth_data[sample_index + i] * depth_scale) + forgive;
        hard_shadow = min(hard_shadow, depth_delta);
    }

    // The bulk samples accumulate into four values whose average softens single-pixel
    // shadows.
    for (var i = HARD_SHADOW_SAMPLES; i < SAMPLE_COUNT - FADE_OUT_SAMPLES; i++) {
        let forgive = range_falloff * f32(i + 1u) + receiver_bias;
        let depth_delta =
            abs(start_depth - depth_data[sample_index + i] * depth_scale) + forgive;
        shadow_value[i & 3u] = min(shadow_value[i & 3u], depth_delta);
    }

    // The most distant samples fade out, softening the hard shadow-length cutoff.
    for (var i = SAMPLE_COUNT - FADE_OUT_SAMPLES; i < SAMPLE_COUNT; i++) {
        let forgive = range_falloff * f32(i + 1u) + receiver_bias;
        let depth_delta =
            abs(start_depth - depth_data[sample_index + i] * depth_scale) + forgive;
        let fade_out = f32(i + 1u - (SAMPLE_COUNT - FADE_OUT_SAMPLES)) /
            f32(FADE_OUT_SAMPLES + 1u) * 0.75;
        shadow_value[i & 3u] = min(shadow_value[i & 3u], depth_delta + fade_out);
    }

    // Contrast boost: samples don't have to exactly match the reference depth to produce a
    // full shadow.
    shadow_value = saturate(
        shadow_value * uniforms.shadow_contrast + vec4f(1.0 - uniforms.shadow_contrast));
    hard_shadow = saturate(hard_shadow * uniforms.shadow_contrast + 1.0 - uniforms.shadow_contrast);

    // Average of the four accumulators reduces aliasing noise from the source depth; the
    // separate hard-shadow term keeps near-caster contact crisp.
    var result = min(hard_shadow, dot(shadow_value, vec4f(0.25)));

    if uniforms.debug_mode == 1u {
        result = select(0.0, 1.0, is_edge);
    }

    // The wavefront projection intentionally overshoots the render bounds by up to two
    // waves; out-of-texture writes are discarded by textureStore.
    textureStore(shadow_out, write_xy, vec4f(result, 0.0, 0.0, 0.0));
}
