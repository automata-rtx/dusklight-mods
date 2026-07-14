// Depth to Normal - debug view. A fullscreen pass that draws the reconstructed world-space
// normal buffer over the entire screen at the very end of the frame (FRAME_BEFORE_HUD), so no
// other mod's effects composite over it - a clean diagnostic of exactly what the provider hands
// consumers. World normal xyz in [-1,1] maps to RGB in [0,1]; sky / invalid texels (w = 0) show
// black.

@group(0) @binding(0) var normal_tex: texture_2d<f32>;

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

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let dims = vec2<i32>(textureDimensions(normal_tex));
    let texel = clamp(vec2<i32>(in.position.xy), vec2<i32>(0i), dims - vec2<i32>(1i));
    let n = textureLoad(normal_tex, texel, 0i);
    if n.w <= 0.0 {
        // Sky / cleared / invalid: black, so gaps in reconstruction are obvious.
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    return vec4f(n.xyz * 0.5 + 0.5, 1.0);
}
