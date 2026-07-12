// Realtime Sun Shadows - smoothed receiver normals.
//
// The slope-scaled bias and normal-offset receivers need the surface direction, and on
// low-poly (GameCube-era) models a per-pixel depth-reconstructed normal jumps at every
// polygon edge, banding the shadows. Sparse tap-blending at render resolution can't fix
// this cleanly: few taps at a fixed pixel distance produce a displaced "ghost" of the
// surface past a resolution-dependent sweet spot.
//
// This pass instead builds a dedicated normal buffer at a RESOLUTION-CAPPED size
// (<= ~1080p-equivalent, chosen by the host) and smooths it with an iterated, depth-aware
// separable Gaussian:
//   - normal_gen: one side-selected 1-buffer-texel cross per texel over the full-res depth
//     snapshot, oriented toward the camera (approximated by the near-plane center). Output
//     rgba32float = (normal.xyz, raw depth); sky texels get w = 0.
//   - normal_blur_h / normal_blur_v: 5-tap [1 4 6 4 1] Gaussian along one axis, each tap
//     weighted by relative depth agreement so silhouettes never blend. The host ping-pongs
//     H/V for `iterations` rounds; every round widens the effective radius, and because the
//     buffer size is capped, a given setting looks the same at any internal resolution.
//
// A true dense Gaussian cannot produce the displaced-copy artifact, and the capped buffer
// makes the whole thing cheaper than per-pixel smoothing at supersampled render sizes.

struct GenUniforms {
    world_from_proj: mat4x4f, // scene depth unproject (camera)
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var normal_out: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> uniforms: GenUniforms;

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

@compute @workgroup_size(8, 8, 1)
fn normal_gen(@builtin(global_invocation_id) global_id: vec3u) {
    let out_size = vec2<i32>(textureDimensions(normal_out));
    let texel = vec2<i32>(global_id.xy);
    if texel.x >= out_size.x || texel.y >= out_size.y {
        return;
    }
    let inv_out = 1.0 / vec2f(out_size);
    let uv = (vec2f(texel) + 0.5) * inv_out;
    let depth = scene_depth_at(uv);
    if depth <= 0.0 {
        // Sky: no surface. w = 0 marks the texel invalid for the blur and the composite.
        textureStore(normal_out, texel, vec4f(0.0, 0.0, 1.0, 0.0));
        return;
    }
    let world = world_position_at(uv, depth);

    // Side-selected cross, one buffer texel wide (several render pixels when supersampling -
    // the dense depth makes that a stable footing, still tight against facet sizes).
    let du = vec2f(inv_out.x, 0.0);
    let dv = vec2f(0.0, inv_out.y);
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
        textureStore(normal_out, texel, vec4f(0.0, 0.0, 1.0, 0.0));
        return;
    }
    normal /= len;
    // Orient toward the camera (visible surfaces face it; the near-plane center is close
    // enough to the camera position for a sign test). No light-based flip: that mirrors the
    // normal wherever the surface crosses the light terminator - a hard, artificial
    // discontinuity in the bias inputs on every curved surface.
    let camera = world_position_at(vec2f(0.5, 0.5), 1.0);
    if dot(normal, world - camera) > 0.0 {
        normal = -normal;
    }
    textureStore(normal_out, texel, vec4f(normal, depth));
}

// --- Separable depth-aware blur ---------------------------------------------------------

@group(0) @binding(0) var blur_in: texture_2d<f32>;
@group(0) @binding(1) var blur_out: texture_storage_2d<rgba32float, write>;

fn blur_axis(texel: vec2<i32>, axis: vec2<i32>) {
    let size = vec2<i32>(textureDimensions(blur_in));
    if texel.x >= size.x || texel.y >= size.y {
        return;
    }
    let center = textureLoad(blur_in, texel, 0i);
    if center.w <= 0.0 {
        textureStore(blur_out, texel, center);
        return;
    }
    let tolerance = max(center.w * 0.05, 1.0e-6);
    let kernel = array<f32, 5>(1.0, 4.0, 6.0, 4.0, 1.0);

    var sum = vec3f(0.0);
    var weight_sum = 0.0;
    for (var i = -2; i <= 2; i += 1) {
        let tap_texel = clamp(texel + axis * i, vec2<i32>(0i), size - 1i);
        let tap = textureLoad(blur_in, tap_texel, 0i);
        if tap.w <= 0.0 {
            continue;
        }
        let dz = (tap.w - center.w) / tolerance;
        let weight = kernel[i + 2] * exp2(-dz * dz);
        sum += tap.xyz * weight;
        weight_sum += weight;
    }
    var normal = center.xyz;
    if weight_sum > 1.0e-6 {
        let blurred = sum / weight_sum;
        let len = length(blurred);
        if len > 1.0e-6 {
            normal = blurred / len;
        }
    }
    // The depth channel passes through unblurred: it is the bilateral reference.
    textureStore(blur_out, texel, vec4f(normal, center.w));
}

@compute @workgroup_size(8, 8, 1)
fn normal_blur_h(@builtin(global_invocation_id) global_id: vec3u) {
    blur_axis(vec2<i32>(global_id.xy), vec2<i32>(1i, 0i));
}

@compute @workgroup_size(8, 8, 1)
fn normal_blur_v(@builtin(global_invocation_id) global_id: vec3u) {
    blur_axis(vec2<i32>(global_id.xy), vec2<i32>(0i, 1i));
}
