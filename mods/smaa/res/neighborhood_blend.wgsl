// SMAA pass 3: neighborhood blending (full-screen triangle drawn into the live scene target).
//
// Each pixel gathers the four weights on its boundaries (its own .r/.b from BlendTex, plus .g of
// the pixel below and .a of the pixel to the right), keeps the dominant axis, and takes one bilinear
// tap toward each neighbour on that axis, offset by that side's weight: a tap offset by w pixels
// mixes in w of the neighbour. With taps on both sides the two are averaged, weighted by their
// weights. Pixels with no weight discard, so only edge pixels are rewritten.
//
// Colour is read from the frame's scene snapshot (a copy), so reading it while writing the live
// target is safe. Drawn at SCENE_AFTER_OPAQUE, before the game's translucency and bloom.
//
// Debug views 1 and 2 bypass the blend and write an opaque colour to every pixel (no discard).

// Mirrors SmaaUniforms in src/mod.cpp (and the copies in the other two shaders).
struct Uniforms {
    screen_size: vec2f,
    inv_screen_size: vec2f,
    threshold: f32,
    normal_threshold: f32,
    depth_threshold: f32,
    max_search_steps: f32,
    local_contrast_factor: f32,
    blend_strength: f32,
    corner_rounding: f32,
    flags: u32,
    debug_view: u32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
}

@group(0) @binding(0) var scene_color: texture_2d<f32>;
@group(0) @binding(1) var blend_tex: texture_2d<f32>;
@group(0) @binding(2) var color_sampler: sampler;
@group(0) @binding(3) var<uniform> uniforms: Uniforms;
@group(0) @binding(4) var edges_tex: texture_2d<f32>;

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

fn clamp_px(p: vec2i) -> vec2i {
    let dims = vec2i(textureDimensions(blend_tex));
    return clamp(p, vec2i(0i), dims - vec2i(1i));
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let px_i = vec2i(in.uv * uniforms.screen_size);

    if (uniforms.debug_view == 1u) {
        // Edge mask: red = left-boundary (vertical) edge, green = top-boundary (horizontal) edge.
        let e = textureLoad(edges_tex, clamp_px(px_i), 0i).xy;
        return vec4f(e.x, e.y, 0.0, 1.0);
    }
    if (uniforms.debug_view == 2u) {
        // This pixel's own BlendTex values: red = r + g (vertical blending, from its top edge),
        // green = b + a (horizontal blending, from its left edge).
        let w = textureLoad(blend_tex, clamp_px(px_i), 0i);
        let vert = w.r + w.g;
        let horiz = w.b + w.a;
        return vec4f(vert, horiz, 0.0, 1.0);
    }

    // Gather the four boundary weights (see packing in blend_weights.wgsl).
    let w_up = textureLoad(blend_tex, clamp_px(px_i), 0i).r;
    let w_down = textureLoad(blend_tex, clamp_px(px_i + vec2i(0i, 1i)), 0i).g;
    let w_left = textureLoad(blend_tex, clamp_px(px_i), 0i).b;
    let w_right = textureLoad(blend_tex, clamp_px(px_i + vec2i(1i, 0i)), 0i).a;

    // a = (right, down, left, up) to match the SMAA neighbourhood convention.
    let a = vec4f(w_right, w_down, w_left, w_up);
    if (dot(a, vec4f(1.0)) < 1.0e-5) {
        discard;
    }

    let horizontal = max(a.x, a.z) > max(a.y, a.w);
    let offset = select(vec4f(0.0, a.y, 0.0, a.w), vec4f(a.x, 0.0, a.z, 0.0), horizontal);
    var bw = select(a.yw, a.xz, horizontal);
    let sum = bw.x + bw.y;
    if (sum < 1.0e-5) {
        discard;
    }
    bw = bw / max(sum, 1.0e-5);

    // coord1 is shifted toward the right (or lower) neighbour, coord2 toward the left (or upper).
    let px = uniforms.inv_screen_size;
    let coord1 = in.uv + offset.xy * px;
    let coord2 = in.uv + offset.zw * (-px);
    let color = bw.x * textureSampleLevel(scene_color, color_sampler, coord1, 0.0) +
        bw.y * textureSampleLevel(scene_color, color_sampler, coord2, 0.0);
    return color;
}
