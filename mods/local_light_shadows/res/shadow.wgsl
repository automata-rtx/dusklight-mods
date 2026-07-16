// Local Light Shadows - deferred composite. Reconstructs each opaque pixel's world position from
// the depth snapshot (CameraService matrices), estimates how much the selected local light reaches
// it (distance attenuation x surface facing), projects it into the light's shadow map, PCF-compares
// against the stored occluder depth, and darkens the pixel by strength x reach x occlusion. Drawn
// as a fullscreen triangle with MULTIPLY blending right after the opaque scene, so translucency and
// the game's bloom layer over an already-shadowed world.
//
// The shadow map is a PERSPECTIVE projection with the camera at the light, looking toward the
// receiver region, so shadows diverge from the light's position like a real point source. The
// reversed-Z depth math still reuses the proven Realtime Sun Shadows path: the composite's light_vp
// negates only the clip z row, so ndc.z = -z_clip/w reproduces aurora's stored reversed depth for a
// perspective matrix exactly as it does for ortho. The per-pixel `reach` term (which fades to zero
// past the light's radius and on surfaces facing away) makes the darkening read as a LOCAL light. A
// single frustum only covers the hemisphere toward the player; full omni (dual-paraboloid / cube)
// and multiple lights are follow-ups.
//
// Depth conventions (both reversed-Z): the scene snapshot has 1.0 at the camera near plane; the
// shadow map, rendered through the game's GX pipeline with a reversed-Z light matrix, stores
// clip.z, i.e. 1.0 nearest to the light and 0.0 at the light frustum far plane.

struct Uniforms {
    world_from_proj: mat4x4f,   // scene depth unproject (camera)
    light_vp: mat4x4f,          // world -> light receiver clip
    light_pos_x: f32,           // selected local light, world space
    light_pos_y: f32,
    light_pos_z: f32,
    light_pow: f32,             // light influence radius, world units
    strength: f32,              // darkening amount (0..1)
    bias: f32,                  // constant depth bias (normalized light-depth units)
    slope_bias: f32,            // slope-scaled bias (normalized light-depth units)
    normal_offset: f32,         // receiver offset along the normal, in shadow texels
    map_size: f32,              // shadow map size in texels (square)
    inv_map_size: f32,
    pcf_taps: f32,              // PCF kernel radius (0 = single bilinear tap)
    texel_world: f32,           // world units per shadow-map texel (normal-offset scale)
    map_enabled: f32,           // 1 = a shadow map is bound this frame
    debug_mode: u32,            // 0 = composite; 1 = occlusion, 2 = reach, 3 = light uv
    atten_power: f32,           // distance-attenuation falloff exponent
    _pad0: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var shadow_map: texture_2d<f32>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
// Non-filtering, clamp-to-edge sampler for the PCF textureGather. The map is R32Float
// (unfilterable), so the sampler must be non-filtering; clamp reproduces a per-texel border clamp.
@group(0) @binding(3) var shadow_sampler: sampler;

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

fn scene_depth_at(uv: vec2f) -> f32 {
    let size = vec2<i32>(textureDimensions(scene_depth));
    let texel = clamp(vec2<i32>(uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    return textureLoad(scene_depth, texel, 0i).r;
}

fn world_position_at(uv: vec2f, depth: f32) -> vec3f {
    let ndc = vec4f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    let position = uniforms.world_from_proj * ndc;
    return position.xyz / position.w;
}

// Side-selected depth cross reconstruction of the world-space surface normal (oriented toward the
// camera). Same technique as the Realtime Sun Shadows inline fallback: for each axis pick the
// neighbor whose depth is closest to the center (keeps the normal from smearing across depth
// discontinuities), difference the world positions, and cross.
fn reconstruct_normal(uv: vec2f, world: vec3f, depth: f32, inv_screen: vec2f) -> vec3f {
    let du = vec2f(inv_screen.x, 0.0);
    let dv = vec2f(0.0, inv_screen.y);
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
    var normal = cross(ddy, ddx);
    let len = length(normal);
    if len < 1.0e-8 {
        return vec3f(0.0, 1.0, 0.0);
    }
    normal = normal / len;
    // Orient toward the camera (the near-plane center approximates its position).
    let camera = world_position_at(vec2f(0.5, 0.5), 1.0);
    if dot(normal, world - camera) > 0.0 {
        normal = -normal;
    }
    return normal;
}

// Bilinearly weighted comparison (what a hardware comparison sampler would do): filter the four
// comparison RESULTS, not the depths. One textureGather reads the 2x2 the manual bilinear would.
// WebGPU gather order is (x0,y1), (x1,y1), (x1,y0), (x0,y0).
fn shadow_compare_bilinear(light_uv: vec2f, receiver: f32, bias: f32) -> f32 {
    let coordinates = light_uv * uniforms.map_size - 0.5;
    let base = floor(coordinates);
    let fraction = coordinates - base;
    let depths = textureGather(0, shadow_map, shadow_sampler, light_uv);
    // Reversed depth: a larger stored value is closer to the light, i.e. an occluder.
    let threshold = receiver + bias;
    let occ = select(vec4f(0.0), vec4f(1.0), depths > vec4f(threshold));
    let s00 = occ.w;
    let s10 = occ.z;
    let s01 = occ.x;
    let s11 = occ.y;
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
            let offset = vec2f(f32(x), f32(y)) * uniforms.inv_map_size;
            sum += shadow_compare_bilinear(light_uv + offset, receiver, bias);
            count += 1.0;
        }
    }
    return sum / count;
}

// Projection of a receiver into the light's shadow map.
struct LightProj {
    uv: vec2f,
    receiver: f32,   // reversed light depth, 1 = nearest to the light
    valid: bool,
}

fn project_light(receiver_world: vec3f) -> LightProj {
    let clip = uniforms.light_vp * vec4f(receiver_world, 1.0);
    var out: LightProj;
    let ndc = clip.xyz / clip.w;
    out.uv = vec2f(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    out.receiver = ndc.z;
    out.valid = max(abs(ndc.x), abs(ndc.y)) < 1.0 && ndc.z > 0.0 && ndc.z <= 1.0;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth = scene_depth_at(in.uv);
    if depth <= 0.0 {
        // Sky / cleared pixels receive no shadow.
        return vec4f(1.0);
    }
    if uniforms.map_enabled == 0.0 {
        return vec4f(1.0);
    }

    let world = world_position_at(in.uv, depth);

    // How much the local light reaches this pixel: distance attenuation x surface facing.
    let light_pos = vec3f(uniforms.light_pos_x, uniforms.light_pos_y, uniforms.light_pos_z);
    let to_light = light_pos - world;
    let dist = length(to_light);
    if dist < 1.0e-3 {
        return vec4f(1.0);
    }
    let light_dir = to_light / dist;
    var atten = saturate(1.0 - dist / max(uniforms.light_pow, 1.0));
    atten = pow(atten, max(uniforms.atten_power, 0.01));

    let inv_screen = 1.0 / vec2f(textureDimensions(scene_depth));
    let n = reconstruct_normal(in.uv, world, depth, inv_screen);
    let facing = max(dot(n, light_dir), 0.0);
    let reach = atten * facing;

    if uniforms.debug_mode == 2u {
        return vec4f(reach, reach, reach, 1.0);
    }
    if reach <= 1.0e-4 {
        return vec4f(1.0);
    }

    // Slope-scaled bias from the surface's angle to the light.
    let cos_t = clamp(facing, 0.05, 1.0);
    let tan_t = min(sqrt(max(1.0 - cos_t * cos_t, 0.0)) / cos_t, 4.0);

    // Normal-offset receiver: shift the lookup point along the surface normal by a fraction of one
    // shadow texel's world size, the most effective acne fix with the least peter-panning.
    let receiver_world = world + n * (uniforms.normal_offset * uniforms.texel_world);
    let p = project_light(receiver_world);
    if uniforms.debug_mode == 3u {
        let valid = select(0.0, 1.0, p.valid);
        return vec4f(saturate(p.uv.x), saturate(p.uv.y), valid, 1.0);
    }
    // Depth Compare: red = the receiver's depth-from-light, green = the stored occluder depth at
    // its projected spot. On a directly-lit surface the nearest occluder IS the receiver, so the
    // two should track (yellow); a systematic split is a bias or reversed-Z/projection mismatch.
    if uniforms.debug_mode == 4u {
        if !p.valid {
            return vec4f(0.0, 0.0, 0.0, 1.0);
        }
        let texel = clamp(vec2<i32>(p.uv * uniforms.map_size), vec2<i32>(0i),
            vec2<i32>(i32(uniforms.map_size) - 1i));
        let stored = textureLoad(shadow_map, texel, 0i).r;
        return vec4f(saturate(p.receiver), saturate(stored), 1.0, 1.0);
    }
    if !p.valid {
        // Outside the light's shadow map: we cannot tell, so assume lit.
        return vec4f(1.0);
    }

    let bias_eff = uniforms.bias + uniforms.slope_bias * tan_t;
    let occlusion = sample_shadow_pcf(p.uv, p.receiver, bias_eff);

    if uniforms.debug_mode == 1u {
        return vec4f(occlusion, occlusion, occlusion, 1.0);
    }

    let value = 1.0 - uniforms.strength * occlusion * reach;
    return vec4f(value, value, value, 1.0);
}
