// Realtime Sun Shadows - deferred shadow composite: reconstructs the world position of every
// scene pixel from the depth snapshot (CameraService matrices), transforms it into the light's
// clip space, and PCF-compares against the shadow map rendered earlier this frame. Drawn as a
// fullscreen triangle with multiply blending (srcFactor = Dst, dstFactor = Zero) right after the
// opaque scene, so translucency and the game's bloom filter layer over an already-shadowed world
// (debug views instead draw unblended before the HUD, on top of everything).
//
// Acne control (on top of the demo this derives from): a constant bias, a SLOPE-SCALED bias that
// grows with the surface's angle to the light (sloped surfaces alias hardest), and a
// NORMAL-OFFSET receiver that shifts the lookup point along the reconstructed surface normal by
// a fraction of one shadow texel's world size. Slope bias + normal offset only act where the
// geometry needs them, so the constant bias can stay small and shadows stay attached at contact
// points instead of peter-panning.
//
// Depth conventions (both reversed-Z): the scene snapshot has 1.0 at the camera near plane;
// the shadow map, rendered through the game's GX pipeline with a GC-convention light matrix,
// stores clip.z, i.e. 1.0 nearest to the light and 0.0 at the light frustum far plane.
//
// The optional screen-space shadow term is Bend Studio's Days Gone technique, computed by a
// separate compute pass (res/bend_sss.wgsl) into a screen-sized visibility texture that this
// composite combines with the mapped occlusion. Fine contact detail and geometry thinner
// than a shadow-map texel both come from that term.

struct Uniforms {
    world_from_proj: mat4x4f, // scene depth unproject (camera)
    light_vp: mat4x4f,        // world -> light receiver projection (UV/depth basis)
    size: vec2f,              // shadow map size in texels
    inv_size: vec2f,
    bias: f32,                // shadow-map depth bias (reversed-depth units)
    strength: f32,            // final darkening amount, horizon fade baked in
    pcf_taps: f32,            // 0 = single tap, 1 = 3x3, 2 = 5x5
    contact_enabled: f32,     // screen-space shadow term available this frame
    debug_mode: u32,          // 0 = composite; nonzero modes are diagnostic views
    slope_bias: f32,          // extra bias per unit of surface slope (normalized depth units)
    normal_offset: f32,       // receiver offset along the surface normal, in shadow texels
    texel_world: f32,         // world units per shadow-map texel
    light_dir_world_x: f32,   // toward the light, world space
    light_dir_world_y: f32,
    light_dir_world_z: f32,
    map_enabled: f32,         // 0 = screen-space-only mode (map bindings are stand-ins)
    normal_smooth: f32,       // facet-normal blend tap distance in pixels (0 = off)
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var shadow_map: texture_2d<f32>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
@group(0) @binding(3) var light_color: texture_2d<f32>;
@group(0) @binding(4) var screen_shadow: texture_2d<f32>; // Bend SSS visibility (1 = lit)

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn load_shadow(texel: vec2<i32>) -> f32 {
    let clamped = clamp(texel, vec2<i32>(0i), vec2<i32>(uniforms.size) - 1i);
    return textureLoad(shadow_map, clamped, 0i).r;
}

// Returns 1.0 when the pixel at light-space depth `receiver` is shadowed by the map texel.
fn shadow_test(texel: vec2<i32>, receiver: f32, bias: f32) -> f32 {
    // Reversed depth: a larger stored value is closer to the light, i.e. an occluder.
    return select(0.0, 1.0, load_shadow(texel) > receiver + bias);
}

// Bilinearly weighted comparison (what a hardware comparison sampler would do): filter the
// four *comparison results*, never the depths themselves. This is what turns per-texel
// staircases into smooth penumbra edges.
fn shadow_compare_bilinear(light_uv: vec2f, receiver: f32, bias: f32) -> f32 {
    let coordinates = light_uv * uniforms.size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let texel = vec2<i32>(base);
    let s00 = shadow_test(texel, receiver, bias);
    let s10 = shadow_test(texel + vec2<i32>(1i, 0i), receiver, bias);
    let s01 = shadow_test(texel + vec2<i32>(0i, 1i), receiver, bias);
    let s11 = shadow_test(texel + vec2<i32>(1i, 1i), receiver, bias);
    let top = mix(s00, s10, fraction.x);
    let bottom = mix(s01, s11, fraction.x);
    return mix(top, bottom, fraction.y);
}

fn sample_shadow_pcf(light_uv: vec2f, receiver: f32, bias: f32) -> f32 {
    let radius = i32(uniforms.pcf_taps);
    var sum = 0.0;
    var count = 0.0;
    for (var y = -radius; y <= radius; y += 1i) {
        for (var x = -radius; x <= radius; x += 1i) {
            let offset = vec2f(f32(x), f32(y)) * uniforms.inv_size;
            sum += shadow_compare_bilinear(light_uv + offset, receiver, bias);
            count += 1.0;
        }
    }
    return sum / count;
}

fn world_position_at(uv: vec2f, depth: f32) -> vec3f {
    let ndc = vec4f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    let position = uniforms.world_from_proj * ndc;
    return position.xyz / position.w;
}

// One side-selected depth cross along the axis pair (du, dv): for each axis, pick the
// neighbor whose depth is closer to the center (side selection keeps the normal from
// smearing across depth discontinuities), difference the world positions, and cross.
// Returns the normalized normal, or zero when degenerate.
fn cross_normal(uv: vec2f, world: vec3f, depth: f32, du: vec2f, dv: vec2f) -> vec3f {
    let d_left = scene_depth_at(uv - du);
    let d_right = scene_depth_at(uv + du);
    let d_top = scene_depth_at(uv - dv);
    let d_bottom = scene_depth_at(uv + dv);

    var ddx: vec3f;
    if abs(d_left - depth) < abs(d_right - depth) {
        ddx = world - world_position_at(uv - du, d_left);
    } else {
        ddx = world_position_at(uv + du, d_right) - world;
    }
    var ddy: vec3f;
    if abs(d_top - depth) < abs(d_bottom - depth) {
        ddy = world - world_position_at(uv - dv, d_top);
    } else {
        ddy = world_position_at(uv + dv, d_bottom) - world;
    }
    let normal = cross(ddy, ddx);
    let len = length(normal);
    if len < 1.0e-8 {
        return vec3f(0.0);
    }
    return normal / len;
}

// World-space surface normal from the depth snapshot; drives the slope-scaled bias and the
// normal-offset receiver. Low-poly (GameCube-era) models change facing at every polygon
// edge, and a raw per-pixel normal makes the bias jump with it, drawing faceted bands into
// the shadows.
//
// normal_smooth (tap distance in pixels) fixes that by BLENDING FACET NORMALS, the way
// smooth shading interpolates vertex normals: four neighborhood taps each reconstruct their
// own tight 1px cross - so every sample is a true single-facet normal - and are averaged
// with a bilateral depth weight that rejects taps across silhouettes. Near a polygon edge
// the result rotates smoothly from one facet's normal to the next over ~2x the tap
// distance.
//
// Do NOT widen a single cross instead: near an edge its two arms land on different facets
// and the cross product belongs to no surface, manufacturing a radius-wide band of garbage
// normals along every polygon edge (dense models read as shattered glass).
fn world_normal_at(uv: vec2f, world: vec3f, depth: f32, inv_screen: vec2f) -> vec3f {
    let du = vec2f(inv_screen.x, 0.0);
    let dv = vec2f(0.0, inv_screen.y);
    let center = cross_normal(uv, world, depth, du, dv);
    var normal = center;
    let s = uniforms.normal_smooth;
    if s >= 1.0 {
        // Bilateral tolerance on the raw depth, matching the depth-aware weights used
        // elsewhere in these mods (relative agreement, silhouette taps fall to ~0).
        let tolerance = max(depth * 0.05, 1.0e-6);
        for (var i = 0; i < 4; i += 1) {
            let sign_x = select(-1.0, 1.0, (i & 1) != 0);
            let sign_y = select(-1.0, 1.0, (i & 2) != 0);
            let uv_tap = uv + vec2f(inv_screen.x * s * sign_x, inv_screen.y * s * sign_y);
            let depth_tap = scene_depth_at(uv_tap);
            if depth_tap <= 0.0 {
                continue;
            }
            let dz = (depth_tap - depth) / tolerance;
            let weight = exp2(-dz * dz);
            if weight < 1.0e-3 {
                continue;
            }
            let world_tap = world_position_at(uv_tap, depth_tap);
            normal += cross_normal(uv_tap, world_tap, depth_tap, du, dv) * weight;
        }
    }
    var len = length(normal);
    if len < 1.0e-8 {
        // Tap normals cancelled; fall back to this pixel's own facet normal.
        normal = center;
        len = length(normal);
    }
    if len < 1.0e-8 {
        return vec3f(0.0, 1.0, 0.0);
    }
    normal /= len;
    // Face the light side: offsetting the receiver toward the lit side is always the direction
    // that prevents self-shadowing.
    let light_dir = vec3f(
        uniforms.light_dir_world_x, uniforms.light_dir_world_y, uniforms.light_dir_world_z);
    if dot(normal, light_dir) < 0.0 {
        normal = -normal;
    }
    return normal;
}

fn scene_depth_at(uv: vec2f) -> f32 {
    let size = vec2<i32>(textureDimensions(scene_depth));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(scene_depth, texel, 0i).r;
}

fn light_color_at(uv: vec2f) -> vec4f {
    let size = vec2<i32>(textureDimensions(light_color));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(light_color, texel, 0i);
}

fn light_depth_debug_at(uv: vec2f) -> vec3f {
    let texel = vec2<i32>(uv * uniforms.size);
    let depth = load_shadow(texel);
    if depth <= 0.0 {
        return vec3f(0.0);
    }

    let dx = abs(depth - load_shadow(texel + vec2<i32>(1i, 0i)));
    let dy = abs(depth - load_shadow(texel + vec2<i32>(0i, 1i)));
    let edge = saturate((dx + dy) * 500.0);
    let shade = saturate(depth * 1.5);
    let bands = 0.08 * (0.5 + 0.5 * cos(depth * 96.0));
    return vec3f(saturate(shade + bands + edge));
}

// Bend SSS visibility for this pixel (1 = lit). The texture matches the scene depth
// snapshot's dimensions, so the same UV addressing applies.
fn screen_shadow_at(uv: vec2f) -> f32 {
    let size = vec2<i32>(textureDimensions(screen_shadow));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(screen_shadow, texel, 0i).r;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    let map_on = uniforms.map_enabled != 0.0;
    // 11 = the Bend SSS visibility buffer; 12 = its edge-detect mask (written by the compute
    // pass when this mode is active).
    if uniforms.debug_mode == 11u || uniforms.debug_mode == 12u {
        let value = screen_shadow_at(in.uv);
        return vec4f(value, value, value, 1.0);
    }
    // Every remaining debug view except Shadow Factor and Receiver Normal diagnoses the map;
    // without one the bindings are stand-ins, so show black instead of garbage.
    if !map_on && uniforms.debug_mode != 0u && uniforms.debug_mode != 2u &&
        uniforms.debug_mode != 13u
    {
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    if uniforms.debug_mode == 1u {
        let value = load_shadow(vec2<i32>(in.uv * uniforms.size));
        return vec4f(value, value, value, 1.0);
    }
    if uniforms.debug_mode == 9u || uniforms.debug_mode == 10u {
        let color = light_color_at(in.uv);
        let color_luma = max(color.r, max(color.g, color.b));
        let depth_color = light_depth_debug_at(in.uv);
        let rgb = select(depth_color, color.rgb, color_luma > (1.0 / 255.0));
        return vec4f(rgb, 1.0);
    }

    if depth <= 0.0 {
        // Sky / cleared pixels receive no shadow.
        if uniforms.debug_mode >= 3u {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        return vec4f(1.0);
    }

    // 13 = the receiver normal (world space, [-1,1] -> RGB) after Normal Smoothing: shows
    // exactly the surface direction Slope Bias and Normal Offset act on.
    if uniforms.debug_mode == 13u {
        let ndc = vec4f(in.uv.x * 2.0 - 1.0, 1.0 - 2.0 * in.uv.y, depth, 1.0);
        let world4 = uniforms.world_from_proj * ndc;
        let world = world4.xyz / world4.w;
        let inv_screen = 1.0 / vec2f(textureDimensions(scene_depth));
        let n = world_normal_at(in.uv, world, depth, inv_screen);
        return vec4f(n * 0.5 + 0.5, 1.0);
    }

    var occlusion = 0.0;
    if map_on {
        let ndc = vec4f(in.uv.x * 2.0 - 1.0, 1.0 - 2.0 * in.uv.y, depth, 1.0);
        let world4 = uniforms.world_from_proj * ndc;
        let world = world4.xyz / world4.w;

        // Receiver-side acne control (see the header): slope-scaled bias + normal-offset lookup.
        var receiver_world = world;
        var bias_eff = uniforms.bias;
        if uniforms.slope_bias > 0.0 || uniforms.normal_offset > 0.0 {
            let inv_screen = 1.0 / vec2f(textureDimensions(scene_depth));
            let n = world_normal_at(in.uv, world, depth, inv_screen);
            let light_dir = vec3f(
                uniforms.light_dir_world_x, uniforms.light_dir_world_y, uniforms.light_dir_world_z);
            let cos_t = clamp(dot(n, light_dir), 0.05, 1.0);
            let tan_t = min(sqrt(max(1.0 - cos_t * cos_t, 0.0)) / cos_t, 4.0);
            bias_eff = uniforms.bias + uniforms.slope_bias * tan_t;
            receiver_world = world + n * (uniforms.normal_offset * uniforms.texel_world);
        }

        let light_clip = uniforms.light_vp * vec4f(receiver_world, 1.0);
        let light_ndc = light_clip.xyz / light_clip.w;
        let receiver = light_ndc.z; // reversed light depth, 1 = nearest to the light
        let light_uv = vec2f(0.5 + 0.5 * light_ndc.x, 0.5 - 0.5 * light_ndc.y);
        let in_shadow_bounds = all(light_uv >= vec2f(0.0)) && all(light_uv <= vec2f(1.0)) &&
            receiver > 0.0 && receiver <= 1.0;
        let shadow_depth = load_shadow(vec2<i32>(light_uv * uniforms.size));

        if uniforms.debug_mode == 4u {
            let valid = select(0.0, 1.0, in_shadow_bounds);
            return vec4f(saturate(light_uv.x), saturate(light_uv.y), valid, 1.0);
        }

        if uniforms.debug_mode == 5u {
            if !in_shadow_bounds {
                return vec4f(0.0, 0.0, 0.0, 1.0);
            }
            let current_compare = select(0.0, 1.0, shadow_depth > receiver + uniforms.bias);
            let opposite_compare = select(0.0, 1.0, shadow_depth < receiver - uniforms.bias);
            return vec4f(current_compare, 0.0, opposite_compare, 1.0);
        }

        if uniforms.debug_mode == 6u {
            let valid = select(0.0, 1.0, in_shadow_bounds);
            return vec4f(saturate(receiver), shadow_depth, valid, 1.0);
        }

        if uniforms.debug_mode == 7u {
            let beyond_far = select(0.0, 1.0, receiver <= 0.0);
            let valid_depth = select(0.0, 1.0, receiver > 0.0 && receiver <= 1.0);
            let before_near = select(0.0, 1.0, receiver > 1.0);
            return vec4f(beyond_far, valid_depth, before_near, 1.0);
        }

        if uniforms.debug_mode == 8u {
            let valid_x = select(0.0, 1.0, light_uv.x >= 0.0 && light_uv.x <= 1.0);
            let valid_y = select(0.0, 1.0, light_uv.y >= 0.0 && light_uv.y <= 1.0);
            let valid_depth = select(0.0, 1.0, receiver > 0.0 && receiver <= 1.0);
            return vec4f(valid_x, valid_y, valid_depth, 1.0);
        }

        if in_shadow_bounds {
            occlusion = sample_shadow_pcf(light_uv, receiver, bias_eff);
        }
    }

    if uniforms.debug_mode == 3u {
        return vec4f(occlusion, occlusion, occlusion, 1.0);
    }

    // Combine with the Bend SSS term: it catches contact detail and thin casters the map
    // misses at any distance (its thickness is depth-relative, not a fixed world size), so
    // no near-field fade is needed.
    if uniforms.contact_enabled != 0.0 && occlusion < 1.0 {
        occlusion = max(occlusion, 1.0 - screen_shadow_at(in.uv));
    }

    let value = 1.0 - uniforms.strength * occlusion;
    if uniforms.debug_mode == 2u {
        return vec4f(value, value, value, 1.0);
    }
    return vec4f(value, value, value, 1.0);
}
