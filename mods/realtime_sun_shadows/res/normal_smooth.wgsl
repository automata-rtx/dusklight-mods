// Realtime Sun Shadows - smoothed receiver normals (bilateral blur only).
//
// The slope-scaled bias and normal-offset receivers need a SMOOTH surface direction: on low-poly
// (GameCube-era) models a per-pixel normal jumps at every polygon edge, banding the shadows.
// This pass takes the raw WORLD-SPACE geometric normal from the Depth to Normal provider mod
// (dev.automata.depth_to_normal; rgba32float = normal.xyz + raw depth in w) and smooths it with
// one depth-aware separable Gaussian whose radius scales with the render height, so a given
// Normal Smoothing setting looks the same at any internal resolution / supersampling factor.
//
// The depth->normal reconstruction itself now lives in Depth to Normal, so shadows no longer
// reconstructs normals - it only applies this smoothing, which is the one normal treatment unique
// to the shadow bias (AO and other consumers want the raw normal, so smoothing stays here).
//
// Why a DENSE, bilaterally-weighted blur (learned the hard way): a few sparse taps at a fixed
// distance average in a displaced copy of the surface - a visible ghost past a resolution-
// dependent sweet spot; a dense Gaussian cannot. The loose bilateral weight (on w = depth)
// rejects only silhouette-scale depth jumps, so facets - depth-continuous, only normal-
// discontinuous - blend smoothly while the character never bleeds into the background behind it.

struct BlurUniforms {
    sigma: f32,   // Gaussian standard deviation in pixels
    radius: f32,  // hard cutoff in pixels (<= MAX_BLUR_RADIUS)
    _pad0: f32,
    _pad1: f32,
}

const MAX_BLUR_RADIUS: i32 = 32;

@group(0) @binding(0) var blur_in: texture_2d<f32>;
@group(0) @binding(1) var blur_out: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> blur: BlurUniforms;

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
    // Loose bilateral: reject only silhouette-scale depth jumps, keep facets/curves blending.
    let tolerance = max(center.w * 0.1, 1.0e-6);
    let inv_two_sigma2 = 1.0 / (2.0 * max(blur.sigma * blur.sigma, 1.0e-6));
    let radius = i32(min(blur.radius + 0.5, f32(MAX_BLUR_RADIUS)));

    var sum = center.xyz; // i = 0 (weight 1)
    var weight_sum = 1.0;
    for (var i = 1; i <= MAX_BLUR_RADIUS; i += 1) {
        if i > radius {
            break;
        }
        let gaussian = exp2(-f32(i * i) * inv_two_sigma2 * 1.4426950408); // exp2(x*log2 e)=exp(x)
        for (var s = -1; s <= 1; s += 2) {
            let tap_texel = clamp(texel + axis * (i * s), vec2<i32>(0i), size - 1i);
            let tap = textureLoad(blur_in, tap_texel, 0i);
            if tap.w <= 0.0 {
                continue;
            }
            let dz = (tap.w - center.w) / tolerance;
            let weight = gaussian * exp2(-dz * dz);
            sum += tap.xyz * weight;
            weight_sum += weight;
        }
    }
    var normal = center.xyz;
    let blurred = sum / weight_sum;
    let len = length(blurred);
    if len > 1.0e-6 {
        normal = blurred / len;
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
