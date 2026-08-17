// Depth to Normal - debug view. A fullscreen pass that draws the world-space normal buffer over
// the entire screen at the very end of the frame (FRAME_BEFORE_HUD), so no other mod's effects
// composite over it - a clean diagnostic of exactly what the provider hands consumers.
//
// Five modes make the authored-vs-reconstructed comparison legible without a rebuild. Mode 0 shows
// only the service output; modes 1-4 need the alternate buffer, which the reconstruct pass fills
// with whichever normal did NOT become the output (xyz = world normal, w = authored validity), so
// between the two textures both normals are always available:
//
//   0 Service Output - what consumers actually get this frame.
//   1 Authored       - the scene normal buffer's normal; black where the pixel has none.
//   2 Reconstructed  - the 5-tap depth-gradient normal, always available.
//   3 Difference     - angle between the two, 0..45 deg as black -> blue -> green -> yellow -> red.
//                      Faceting alone reads dark with bright creases at triangle edges; a whole
//                      screen of yellow/red means the two disagree systematically, i.e. a basis
//                      (sign / axis) mismatch rather than a smoothness difference.
//   4 Coverage       - green where an authored normal exists, red where it falls back.
//
// World normal xyz in [-1,1] maps to RGB in [0,1]; sky / invalid texels (w = 0) show black.

struct Uniforms {
    mode: u32,
    // 1 = alt_tex holds the authored normal (the service output is the reconstruction),
    // 0 = alt_tex holds the reconstruction (the service output is authored where it exists).
    alt_is_authored: u32,
    _pad0: vec2u,
}

@group(0) @binding(0) var normal_tex: texture_2d<f32>;  // service output: xyz world, w raw depth
@group(0) @binding(1) var alt_tex: texture_2d<f32>;     // the other one: xyz world, w validity
@group(0) @binding(2) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    var out: VertexOutput;
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}

// black -> blue -> green -> yellow -> red
fn heat(t: f32) -> vec3f {
    let c = clamp(t, 0.0, 1.0);
    let ramp = vec3f(clamp(c * 3.0 - 1.0, 0.0, 1.0), clamp(1.5 - abs(c * 3.0 - 1.5), 0.0, 1.0),
        clamp(1.0 - c * 3.0, 0.0, 1.0));
    // Fade the low end to black so "no difference" reads as absence, not as a color.
    return ramp * clamp(c * 4.0, 0.0, 1.0);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let dims = vec2<i32>(textureDimensions(normal_tex));
    let texel = clamp(vec2<i32>(in.position.xy), vec2<i32>(0i), dims - vec2<i32>(1i));
    let n = textureLoad(normal_tex, texel, 0i);
    if n.w <= 0.0 {
        // Sky / cleared / invalid: black, so gaps are obvious.
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    if uniforms.mode == 0u {
        return vec4f(n.xyz * 0.5 + 0.5, 1.0);
    }

    let alt_dims = vec2<i32>(textureDimensions(alt_tex));
    let alt = textureLoad(alt_tex, clamp(texel, vec2<i32>(0i), alt_dims - vec2<i32>(1i)), 0i);
    let has_authored = alt.w > 0.5;
    // Sort the two textures into authored / reconstructed. The reconstruction is always one of
    // them; the authored normal only exists where has_authored.
    let authored = select(n.xyz, alt.xyz, uniforms.alt_is_authored == 1u);
    let reconstructed = select(alt.xyz, n.xyz, uniforms.alt_is_authored == 1u);

    switch uniforms.mode {
        case 1u: {
            if !has_authored {
                return vec4f(0.0, 0.0, 0.0, 1.0);
            }
            return vec4f(authored * 0.5 + 0.5, 1.0);
        }
        case 2u: {
            return vec4f(reconstructed * 0.5 + 0.5, 1.0);
        }
        case 3u: {
            if !has_authored {
                // Nothing to compare against: flat dark grey, distinct from a zero difference.
                return vec4f(0.12, 0.12, 0.12, 1.0);
            }
            let angle = acos(clamp(dot(authored, reconstructed), -1.0, 1.0));
            return vec4f(heat(degrees(angle) / 45.0), 1.0);
        }
        case 4u: {
            if has_authored {
                return vec4f(0.05, 0.75, 0.15, 1.0);
            }
            return vec4f(0.85, 0.1, 0.05, 1.0);
        }
        default: {
            return vec4f(n.xyz * 0.5 + 0.5, 1.0);
        }
    }
}
