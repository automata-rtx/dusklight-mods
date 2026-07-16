// SSILVB - fullscreen composite.
//
// Unlike VBAO's multiply-only composite, this pass outputs BOTH terms of the effect in one draw
// and lets the blend state combine them with whatever is already in the scene target:
//
//     fragment output:  rgb = GI_add (gamma-encoded bounce light),  a = AO_mul (occlusion)
//     Add mode blend:   out = GI_add             + dst * AO_mul
//     Screen mode:      out = GI_add * (1 - dst) + dst * AO_mul   (never clips on the LDR target)
//
// Reading the scene is still never required for the combine itself - the only scene-color read
// here is the RECEIVER ALBEDO PROXY: bounce light should be multiplied by the receiver's albedo,
// which this game never materializes (forward TEV renderer, no G-buffer). chroma_lift blends the
// proxy between the raw snapshot color (near-exact under vanilla TP's flat ambient lighting,
// where scene color ~= albedo x constant) and full chroma normalization (robust when our shadow
// and AO mods darken the snapshot for lighting reasons). See docs/ssilvb_plan.md 5.1.
//
// The GI source is read at its native resolution: full-res history 1:1 when temporal
// accumulation is on, else a depth-aware 4-tap upscale of the (half-res) chain output - both
// inherited from VBAO's composite.
//
// Debug views (drawn with the no-blend pipeline at FRAME_BEFORE_HUD so fog/bloom cannot repaint
// them): 1 = bounce light only, 2 = AO only, 3 = albedo proxy, 4 = light input MIP (what the
// bounce actually sees), 5 = provider world normals.

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
}

@group(0) @binding(0) var gi_source: texture_2d<f32>;          // (GI.rgb linear, AO)
@group(0) @binding(1) var preprocessed_depth: texture_2d<f32>; // chain MIP0, upscale weights
@group(0) @binding(2) var scene_depth_raw: texture_2d<f32>;    // full-res raw snapshot
@group(0) @binding(3) var<uniform> uniforms: Uniforms;
@group(0) @binding(4) var scene_color: texture_2d<f32>;        // full-res snapshot (albedo proxy)
@group(0) @binding(5) var d2n_normal: texture_2d<f32>;         // provider normals (debug view)
@group(0) @binding(6) var color_chain: texture_2d<f32>;        // linear light MIPs (debug view)

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

// Depth-aware manual bilinear over the chain-res GI estimate: each tap's bilinear weight is
// multiplied by how well its depth agrees with this render pixel's raw depth, so neither light
// nor occlusion bleeds across silhouettes at half resolution. Falls back to the nearest texel
// when every tap disagrees (a 1px fringe).
fn sample_gi(uv: vec2f, reference_depth: f32) -> vec4f {
    let coordinates = uv * uniforms.size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let max_coordinates = vec2i(uniforms.size) - 1i;
    let p00 = clamp(vec2i(base), vec2i(0i), max_coordinates);
    let p11 = clamp(vec2i(base) + 1i, vec2i(0i), max_coordinates);
    let depth_tolerance = max(reference_depth * 0.05, 1.0e-6);

    var sum = vec4f(0.0);
    var weight_sum = 0.0;
    for (var i = 0; i < 4; i += 1) {
        let tap = vec2i(select(p00.x, p11.x, (i & 1) != 0), select(p00.y, p11.y, (i & 2) != 0));
        let bw = select(1.0 - fraction.x, fraction.x, (i & 1) != 0) *
            select(1.0 - fraction.y, fraction.y, (i & 2) != 0);
        let dz = (load_depth(tap) - reference_depth) / depth_tolerance;
        let w = bw * exp2(-dz * dz);
        sum += w * textureLoad(gi_source, tap, 0i);
        weight_sum += w;
    }
    if weight_sum < 1.0e-4 {
        let nearest = clamp(vec2i(round(coordinates)), vec2i(0i), max_coordinates);
        return textureLoad(gi_source, nearest, 0i);
    }
    return sum / weight_sum;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.inverse_projection * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

// Distance fade factor: 0 = full effect, 1 = fully faded (both AO and GI fade together so far
// terrain, already washed toward fog, is neither darkened nor re-lit).
fn distance_fade(reference_depth: f32, uv: vec2f) -> f32 {
    if (uniforms.flags & 4u) == 0u || reference_depth <= 0.0 {
        return 0.0;
    }
    let view_position = reconstruct_view_space_position(reference_depth, uv);
    return smoothstep(uniforms.fade_start, max(uniforms.fade_end, uniforms.fade_start + 1.0),
        max(-view_position.z, 0.0));
}

// Black point + contrast + intensity shaping, identical to VBAO's composite.
fn shape_ao(raw_visibility: f32, fade: f32) -> f32 {
    if (uniforms.flags & 16u) == 0u {
        return 1.0; // AO term disabled (e.g. running alongside VBAO)
    }
    let occlusion = clamp(
        ((1.0 - raw_visibility) - uniforms.black_point) / max(1.0 - uniforms.black_point, 1.0e-3),
        0.0, 1.0);
    var visibility = pow(clamp(1.0 - occlusion, 0.0, 1.0), uniforms.contrast);
    visibility = mix(visibility, 1.0, fade);
    return clamp(mix(1.0, visibility, uniforms.intensity), 0.0, 1.0);
}

// Receiver albedo proxy from the snapshot color (linear). chroma_lift = 0 keeps the raw scene
// color (near-exact albedo under vanilla TP's flat lighting); 1 divides luminance fully out
// (hue/saturation only - robust when shadow/AO mods darken the receiver). Forced to 1 (white)
// by flags bit 5 for judging raw light transport.
fn albedo_proxy(uv: vec2f) -> vec3f {
    if (uniforms.flags & 32u) != 0u {
        return vec3f(1.0);
    }
    let size = vec2f(textureDimensions(scene_color));
    let texel = clamp(vec2<i32>(uv * size), vec2<i32>(0i), vec2<i32>(size) - 1i);
    let encoded = textureLoad(scene_color, texel, 0i).rgb;
    let s = pow(max(encoded, vec3f(0.0)), vec3f(2.2));
    let luma = dot(s, vec3f(0.299, 0.587, 0.114));
    let divisor = pow(max(luma, 0.08), clamp(uniforms.chroma_lift, 0.0, 1.0));
    return clamp(s / max(divisor, 1.0e-4), vec3f(0.0), vec3f(1.0));
}

// Shaped bounce light in gamma space (what gets added to the gamma-encoded scene target).
fn shape_gi(gi_linear: vec3f, uv: vec2f, fade: f32) -> vec3f {
    if (uniforms.flags & 8u) == 0u {
        return vec3f(0.0);
    }
    let lit = max(gi_linear, vec3f(0.0)) * albedo_proxy(uv) * uniforms.gi_intensity * (1.0 - fade);
    return pow(lit, vec3f(1.0 / 2.2));
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let full_size = vec2f(textureDimensions(scene_depth_raw));
    let reference_depth = load_raw_depth(vec2<i32>(in.uv * full_size));

    if uniforms.debug_view == 3u {
        // The receiver albedo proxy, re-encoded for display.
        return vec4f(pow(albedo_proxy(in.uv), vec3f(1.0 / 2.2)), 1.0);
    }
    if uniforms.debug_view == 4u {
        // The light input the bounce sees: color chain MIP 2, re-encoded for display.
        let mip_size = max(vec2<i32>(uniforms.size) >> vec2<u32>(2u), vec2<i32>(1i));
        let texel = clamp(vec2<i32>(in.uv * vec2f(mip_size)), vec2<i32>(0i), mip_size - 1i);
        let light = textureLoad(color_chain, texel, 2i).rgb;
        return vec4f(pow(max(light, vec3f(0.0)), vec3f(1.0 / 2.2)), 1.0);
    }
    if uniforms.debug_view == 5u {
        // Provider world-space normals, [-1,1] -> RGB.
        let n_dims = vec2f(textureDimensions(d2n_normal));
        let texel = clamp(vec2<i32>(in.uv * n_dims), vec2<i32>(0i), vec2<i32>(n_dims) - 1i);
        let world_n = textureLoad(d2n_normal, texel, 0i).xyz;
        return vec4f(world_n * 0.5 + 0.5, 1.0);
    }

    // The GI source is either full-res (temporal history, or full-res mode) or half-res (temporal
    // off). At full res the reconstruction already happened in the temporal upsampler, so read it
    // 1:1; at half res do the depth-aware bilinear upscale here.
    var value: vec4f;
    if all(textureDimensions(gi_source) == vec2<u32>(full_size)) {
        let px = clamp(vec2<i32>(in.uv * full_size), vec2<i32>(0i), vec2<i32>(full_size) - 1i);
        value = textureLoad(gi_source, px, 0i);
    } else {
        value = sample_gi(in.uv, reference_depth);
    }

    let fade = distance_fade(reference_depth, in.uv);
    let ao_mul = shape_ao(value.a, fade);
    let gi_add = shape_gi(value.rgb, in.uv, fade);

    if uniforms.debug_view == 1u {
        // Bounce light only, over black (with the proxy and intensity applied).
        return vec4f(gi_add, 1.0);
    }
    if uniforms.debug_view == 2u {
        // The shaped occlusion term as grayscale.
        return vec4f(ao_mul, ao_mul, ao_mul, 1.0);
    }

    // Blend combines: out = gi_add [ * (1 - dst) in Screen mode ] + dst * ao_mul.
    return vec4f(gi_add, ao_mul);
}
