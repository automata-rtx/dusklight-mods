// SMAA — pass 3: neighborhood blending (fullscreen draw into the live scene target).
//
// For each pixel, gather the four blend weights that touch its boundaries (its own top/left edge
// plus the reciprocal weights from the pixel below/right), pick the dominant axis, and pull in the
// neighbouring colour by a sub-pixel bilinear offset. Non-edge pixels discard, leaving the live
// target untouched (so only edges are rewritten). The colour input is the frame's resolved scene
// snapshot, so reading it while writing the live target is hazard-free.
//
// Runs at SCENE_AFTER_OPAQUE (before bloom / translucency), so the game's post effects operate on
// antialiased geometry.

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
        // Edge mask: red = left edge, green = top edge.
        let e = textureLoad(edges_tex, clamp_px(px_i), 0i).xy;
        return vec4f(e.x, e.y, 0.0, 1.0);
    }
    if (uniforms.debug_view == 2u) {
        // Blend weights: warm = vertical (up/down), cool = horizontal (left/right).
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

    let px = uniforms.inv_screen_size;
    let coord1 = in.uv + offset.xy * px;
    let coord2 = in.uv + offset.zw * (-px);
    let color = bw.x * textureSampleLevel(scene_color, color_sampler, coord1, 0.0) +
        bw.y * textureSampleLevel(scene_color, color_sampler, coord2, 0.0);
    return color;
}
