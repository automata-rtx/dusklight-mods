// Realtime Sun Shadows - deferred cascaded-shadow composite: reconstructs the world position
// of every scene pixel from the depth snapshot (CameraService matrices), picks the sharpest
// shadow cascade containing it, and PCF-compares against that cascade's map (cross-fading to
// the next cascade near the boundary so transitions stay invisible). Drawn as a fullscreen
// triangle with multiply blending right after the opaque scene, so translucency and the game's
// bloom filter layer over an already-shadowed world.
//
// Cascades: slots 0..cascade_count-1 are nested world boxes (near -> far, radii split from the
// Coverage setting); slot 3 is the optional Link cascade - a small, very dense box snapped to
// the player containing ONLY his models, combined with max() so it can only add detail, never
// remove world shadows. Bias / slope bias are normalized per cascade (each has its own ortho
// depth range), texel_world and the PCF kernel are also per cascade.
//
// Acne control (per selected cascade): a constant bias, a SLOPE-SCALED bias that grows with
// the surface's angle to the light, and a NORMAL-OFFSET receiver that shifts the lookup point
// along the reconstructed surface normal by a fraction of one texel's world size.
//
// Depth conventions (both reversed-Z): the scene snapshot has 1.0 at the camera near plane;
// the shadow maps, rendered through the game's GX pipeline with GC-convention light matrices,
// store clip.z, i.e. 1.0 nearest to the light and 0.0 at the light frustum far plane.
//
// The optional screen-space shadow term is Bend Studio's Days Gone technique, computed by a
// separate compute pass (res/bend_sss.wgsl) into a screen-sized visibility texture that this
// composite combines with the mapped occlusion.

struct Uniforms {
    world_from_proj: mat4x4f,             // scene depth unproject (camera)
    light_vp: array<mat4x4f, 4>,          // world -> light receiver projection per cascade
    map_size: vec4f,                      // per-cascade map size in texels (square)
    inv_map_size: vec4f,
    bias: vec4f,                          // per-cascade constant bias (normalized depth units)
    slope_bias: vec4f,                    // per-cascade slope-scaled bias
    texel_world: vec4f,                   // per-cascade world units per texel
    pcf_taps: vec4f,                      // per-cascade PCF kernel radius (0 = bilinear tap)
    strength: f32,            // final darkening amount, horizon fade baked in
    contact_enabled: f32,     // screen-space shadow term available this frame
    normal_offset: f32,       // receiver offset along the surface normal, in shadow texels
    map_enabled: f32,         // 0 = screen-space-only mode (map bindings are stand-ins)
    debug_mode: u32,          // 0 = composite; nonzero modes are diagnostic views
    cascade_count: f32,       // world cascades bound (1..3)
    link_enabled: f32,        // 1 = slot 3 is the Link cascade
    blend_frac: f32,          // cascade cross-fade band, fraction of the light NDC half-extent
    light_dir_world_x: f32,   // toward the light, world space
    light_dir_world_y: f32,
    light_dir_world_z: f32,
    smoothed_normals: f32,    // 1 = the smoothed-normal buffer is bound (normal_smooth.wgsl)
    camera_eye_x: f32,        // camera world position (screen-space shadow distance fade)
    camera_eye_y: f32,
    camera_eye_z: f32,
    sss_fade_start: f32,      // world units; screen-space shadow full below this distance
    sss_fade_end: f32,        // world units; screen-space shadow gone beyond this distance
    edge_fade: f32,           // 1 = fade the outermost cascade's shadow out at its box edge
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var shadow_map0: texture_2d<f32>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
@group(0) @binding(3) var light_color: texture_2d<f32>;   // far cascade's color (debug views)
@group(0) @binding(4) var screen_shadow: texture_2d<f32>; // Bend SSS visibility (1 = lit)
@group(0) @binding(5) var smooth_normal: texture_2d<f32>; // (normal.xyz, raw depth)
@group(0) @binding(6) var shadow_map1: texture_2d<f32>;
@group(0) @binding(7) var shadow_map2: texture_2d<f32>;
@group(0) @binding(8) var shadow_map3: texture_2d<f32>;   // Link cascade

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

// Texture bindings cannot be indexed dynamically in WGSL, so cascade reads go through a switch.
fn load_map(map: u32, texel: vec2<i32>) -> f32 {
    let clamped = clamp(texel, vec2<i32>(0i), vec2<i32>(i32(uniforms.map_size[map]) - 1i));
    switch map {
        case 0u: { return textureLoad(shadow_map0, clamped, 0i).r; }
        case 1u: { return textureLoad(shadow_map1, clamped, 0i).r; }
        case 2u: { return textureLoad(shadow_map2, clamped, 0i).r; }
        default: { return textureLoad(shadow_map3, clamped, 0i).r; }
    }
}

// Returns 1.0 when the pixel at light-space depth `receiver` is shadowed by the map texel.
fn shadow_test(map: u32, texel: vec2<i32>, receiver: f32, bias: f32) -> f32 {
    // Reversed depth: a larger stored value is closer to the light, i.e. an occluder.
    return select(0.0, 1.0, load_map(map, texel) > receiver + bias);
}

// Bilinearly weighted comparison (what a hardware comparison sampler would do): filter the
// four *comparison results*, never the depths themselves. This is what turns per-texel
// staircases into smooth penumbra edges.
fn shadow_compare_bilinear(map: u32, light_uv: vec2f, receiver: f32, bias: f32) -> f32 {
    let coordinates = light_uv * uniforms.map_size[map] - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let texel = vec2<i32>(base);
    let s00 = shadow_test(map, texel, receiver, bias);
    let s10 = shadow_test(map, texel + vec2<i32>(1i, 0i), receiver, bias);
    let s01 = shadow_test(map, texel + vec2<i32>(0i, 1i), receiver, bias);
    let s11 = shadow_test(map, texel + vec2<i32>(1i, 1i), receiver, bias);
    let top = mix(s00, s10, fraction.x);
    let bottom = mix(s01, s11, fraction.x);
    return mix(top, bottom, fraction.y);
}

fn sample_shadow_pcf(map: u32, light_uv: vec2f, receiver: f32, bias: f32) -> f32 {
    let radius = i32(uniforms.pcf_taps[map]);
    var sum = 0.0;
    var count = 0.0;
    for (var y = -radius; y <= radius; y += 1i) {
        for (var x = -radius; x <= radius; x += 1i) {
            let offset = vec2f(f32(x), f32(y)) * uniforms.inv_map_size[map];
            sum += shadow_compare_bilinear(map, light_uv + offset, receiver, bias);
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

// World-space surface normal for the slope-scaled bias and the normal-offset receiver.
//
// With Normal Smoothing on, the normal comes from the dedicated buffer (normal_smooth.wgsl),
// sampled with a depth-weighted bilinear so silhouettes stay crisp. Off (or when the buffer
// is unavailable), a single side-selected 1px cross reconstructs the raw facet normal inline.
//
// The normal is oriented toward the CAMERA (visible surfaces face it). There is deliberately
// no light-based flip: mirroring the normal wherever the surface crosses the light terminator
// put a hard, artificial discontinuity into the bias inputs on every curved surface.
fn world_normal_at(uv: vec2f, world: vec3f, depth: f32, inv_screen: vec2f) -> vec3f {
    var normal = vec3f(0.0);
    if uniforms.smoothed_normals != 0.0 {
        let size = vec2f(textureDimensions(smooth_normal));
        let coordinates = uv * size - 0.5;
        let base = floor(coordinates);
        let fraction = coordinates - base;
        let max_texel = vec2<i32>(size) - 1i;
        let tolerance = max(depth * 0.05, 1.0e-6);
        var weight_sum = 0.0;
        for (var i = 0; i < 4; i += 1) {
            let corner = vec2<i32>(i & 1, i >> 1);
            let texel = clamp(vec2<i32>(base) + corner, vec2<i32>(0i), max_texel);
            let tap = textureLoad(smooth_normal, texel, 0i);
            if tap.w <= 0.0 {
                continue;
            }
            let bilinear = select(1.0 - fraction.x, fraction.x, corner.x == 1) *
                select(1.0 - fraction.y, fraction.y, corner.y == 1);
            let dz = (tap.w - depth) / tolerance;
            let weight = bilinear * exp2(-dz * dz);
            normal += tap.xyz * weight;
            weight_sum += weight;
        }
        if weight_sum < 1.0e-4 {
            normal = vec3f(0.0); // all taps rejected (thin silhouette): rebuild inline
        }
    }
    if length(normal) < 1.0e-6 {
        normal = cross_normal(
            uv, world, depth, vec2f(inv_screen.x, 0.0), vec2f(0.0, inv_screen.y));
        // Orient toward the camera (the near-plane center approximates its position).
        let camera = world_position_at(vec2f(0.5, 0.5), 1.0);
        if dot(normal, world - camera) > 0.0 {
            normal = -normal;
        }
    }
    let len = length(normal);
    if len < 1.0e-8 {
        return vec3f(0.0, 1.0, 0.0);
    }
    return normal / len;
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

fn light_depth_debug_at(map: u32, uv: vec2f) -> vec3f {
    let texel = vec2<i32>(uv * uniforms.map_size[map]);
    let depth = load_map(map, texel);
    if depth <= 0.0 {
        return vec3f(0.0);
    }

    let dx = abs(depth - load_map(map, texel + vec2<i32>(1i, 0i)));
    let dy = abs(depth - load_map(map, texel + vec2<i32>(0i, 1i)));
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

// Projection of a (possibly normal-offset) receiver into one cascade's light space.
struct CascadeProj {
    uv: vec2f,
    receiver: f32,   // reversed light depth, 1 = nearest to the light
    fit: f32,        // max NDC coordinate magnitude: < 1 means inside the cascade box
    valid: bool,
}

fn project_cascade(map: u32, receiver_world: vec3f) -> CascadeProj {
    let clip = uniforms.light_vp[map] * vec4f(receiver_world, 1.0);
    let ndc = clip.xyz / clip.w;
    var out: CascadeProj;
    out.uv = vec2f(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    out.receiver = ndc.z;
    out.fit = max(abs(ndc.x), abs(ndc.y));
    out.valid = out.fit < 1.0 && ndc.z > 0.0 && ndc.z <= 1.0;
    return out;
}

// Full occlusion evaluation against one cascade: per-cascade normal-offset receiver,
// per-cascade slope-scaled bias, per-cascade PCF kernel.
fn cascade_occlusion(map: u32, world: vec3f, n: vec3f, tan_t: f32) -> f32 {
    let receiver_world = world + n * (uniforms.normal_offset * uniforms.texel_world[map]);
    let p = project_cascade(map, receiver_world);
    if !p.valid {
        return 0.0;
    }
    let bias_eff = uniforms.bias[map] + uniforms.slope_bias[map] * tan_t;
    return sample_shadow_pcf(map, p.uv, p.receiver, bias_eff);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    let map_on = uniforms.map_enabled != 0.0;
    let count = u32(uniforms.cascade_count);
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
    // 1 = the cascade depth buffers tiled 2x2: near / mid over far / Link.
    if uniforms.debug_mode == 1u {
        let tile = vec2<u32>(in.uv * 2.0);
        let map = min(tile.y, 1u) * 2u + min(tile.x, 1u);
        let local_uv = fract(in.uv * 2.0);
        let bound = map < count || (map == 3u && uniforms.link_enabled != 0.0);
        if !bound {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        let value = load_map(map, vec2<i32>(local_uv * uniforms.map_size[map]));
        return vec4f(value, value, value, 1.0);
    }
    if uniforms.debug_mode == 9u || uniforms.debug_mode == 10u {
        let color = light_color_at(in.uv);
        let color_luma = max(color.r, max(color.g, color.b));
        let depth_color = light_depth_debug_at(count - 1u, in.uv);
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

    // World position of this pixel (the map receivers and the SSS distance fade use it).
    let ndc = vec4f(in.uv.x * 2.0 - 1.0, 1.0 - 2.0 * in.uv.y, depth, 1.0);
    let world4 = uniforms.world_from_proj * ndc;
    let world = world4.xyz / world4.w;

    var occlusion = 0.0;
    if map_on {
        // Receiver-side acne control: the smoothed normal feeds the slope-scaled bias and
        // the per-cascade normal-offset receivers.
        var n = vec3f(0.0);
        var tan_t = 0.0;
        if uniforms.slope_bias[0] > 0.0 || uniforms.normal_offset > 0.0 {
            let inv_screen = 1.0 / vec2f(textureDimensions(scene_depth));
            n = world_normal_at(in.uv, world, depth, inv_screen);
            let light_dir = vec3f(
                uniforms.light_dir_world_x, uniforms.light_dir_world_y, uniforms.light_dir_world_z);
            let cos_t = clamp(dot(n, light_dir), 0.05, 1.0);
            tan_t = min(sqrt(max(1.0 - cos_t * cos_t, 0.0)) / cos_t, 4.0);
        }

        // Cascade selection: the first (sharpest) cascade whose box contains the receiver
        // wins; inside the outer blend band, cross-fade toward the next cascade so the
        // resolution step never shows as a line.
        var selected = count - 1u;
        var sel = project_cascade(count - 1u, world);
        for (var i = 0u; i < count; i += 1u) {
            let p = project_cascade(i, world);
            if p.valid {
                selected = i;
                sel = p;
                break;
            }
        }

        // Debug views diagnose the selected cascade.
        let shadow_depth = load_map(selected, vec2<i32>(sel.uv * uniforms.map_size[selected]));
        if uniforms.debug_mode == 4u {
            let valid = select(0.0, 1.0, sel.valid);
            return vec4f(saturate(sel.uv.x), saturate(sel.uv.y), valid, 1.0);
        }
        if uniforms.debug_mode == 5u {
            if !sel.valid {
                return vec4f(0.0, 0.0, 0.0, 1.0);
            }
            let bias0 = uniforms.bias[selected];
            let current_compare = select(0.0, 1.0, shadow_depth > sel.receiver + bias0);
            let opposite_compare = select(0.0, 1.0, shadow_depth < sel.receiver - bias0);
            return vec4f(current_compare, 0.0, opposite_compare, 1.0);
        }
        if uniforms.debug_mode == 6u {
            let valid = select(0.0, 1.0, sel.valid);
            return vec4f(saturate(sel.receiver), shadow_depth, valid, 1.0);
        }
        if uniforms.debug_mode == 7u {
            let beyond_far = select(0.0, 1.0, sel.receiver <= 0.0);
            let valid_depth = select(0.0, 1.0, sel.receiver > 0.0 && sel.receiver <= 1.0);
            let before_near = select(0.0, 1.0, sel.receiver > 1.0);
            return vec4f(beyond_far, valid_depth, before_near, 1.0);
        }
        if uniforms.debug_mode == 8u {
            let valid_x = select(0.0, 1.0, sel.uv.x >= 0.0 && sel.uv.x <= 1.0);
            let valid_y = select(0.0, 1.0, sel.uv.y >= 0.0 && sel.uv.y <= 1.0);
            let valid_depth = select(0.0, 1.0, sel.receiver > 0.0 && sel.receiver <= 1.0);
            return vec4f(valid_x, valid_y, valid_depth, 1.0);
        }

        if sel.valid {
            occlusion = cascade_occlusion(selected, world, n, tan_t);
            let blend_start = 1.0 - uniforms.blend_frac;
            if sel.fit > blend_start && selected + 1u < count {
                let t = saturate((sel.fit - blend_start) / uniforms.blend_frac);
                occlusion = mix(occlusion, cascade_occlusion(selected + 1u, world, n, tan_t), t);
            } else if uniforms.edge_fade != 0.0 && selected + 1u >= count && sel.fit > blend_start {
                // Outermost cascade: no farther cascade to hand off to, so its box edge is a
                // hard coverage boundary. Fade the mapped shadow out across the same band so
                // distant shadows dissolve into unshadowed geometry (and into the fog) instead
                // of drawing in as a sharp line on far mountains as they enter coverage.
                let t = saturate((sel.fit - blend_start) / uniforms.blend_frac);
                occlusion = occlusion * (1.0 - t);
            }
        }

        // The Link cascade contains only the player's models, so it can only ADD occlusion
        // (an empty map compares as fully lit); max() keeps whichever term is darker.
        var link_occlusion = 0.0;
        if uniforms.link_enabled != 0.0 {
            link_occlusion = cascade_occlusion(3u, world, n, tan_t);
            occlusion = max(occlusion, link_occlusion);
        }

        // 14 = cascade coverage: red / green / blue = near / mid / far cascade shading this
        // pixel (blended in the cross-fade bands), white overlay = Link cascade adding here.
        if uniforms.debug_mode == 14u {
            var rgb = vec3f(0.0);
            if sel.valid {
                var color0 = vec3f(0.0);
                var color1 = vec3f(0.0);
                color0[min(selected, 2u)] = 1.0;
                color1[min(selected + 1u, 2u)] = 1.0;
                let blend_start = 1.0 - uniforms.blend_frac;
                let t = saturate((sel.fit - blend_start) / uniforms.blend_frac);
                rgb = mix(color0, color1, select(0.0, t, selected + 1u < count));
            }
            rgb = mix(rgb, vec3f(1.0), saturate(link_occlusion) * 0.75);
            return vec4f(rgb, 1.0);
        }
    }

    if uniforms.debug_mode == 3u {
        return vec4f(occlusion, occlusion, occlusion, 1.0);
    }

    // Combine with the Bend SSS term: it catches contact detail and thin casters the map
    // misses at any distance (its thickness is depth-relative, not a fixed world size). Fade
    // it out with world distance from the camera so distant, fogged-out geometry isn't
    // shadowed at full strength (the shadow map, bounded by its coverage radius, is already
    // near the camera).
    if uniforms.contact_enabled != 0.0 && occlusion < 1.0 {
        let camera = vec3f(uniforms.camera_eye_x, uniforms.camera_eye_y, uniforms.camera_eye_z);
        let distance = length(world - camera);
        let fade = 1.0 - smoothstep(
            uniforms.sss_fade_start, max(uniforms.sss_fade_end, uniforms.sss_fade_start + 1.0),
            distance);
        occlusion = max(occlusion, (1.0 - screen_shadow_at(in.uv)) * fade);
    }

    let value = 1.0 - uniforms.strength * occlusion;
    return vec4f(value, value, value, 1.0);
}
