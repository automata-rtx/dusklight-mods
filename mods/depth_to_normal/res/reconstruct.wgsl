// Depth to Normal - reconstruct.wgsl
//
// Reconstructs a per-pixel world-space geometric surface normal from the scene depth snapshot,
// once per frame, for other mods to consume (ambient occlusion, shadows, reflections, outlines).
//
// The depth->normal reconstruction is atyuwen's accurate 5-tap method, adapted UNCHANGED from
// Encounter's ao_mod demo (which ports it from Bevy Engine's SSAO; see res/licenses/). The
// method is defined in view space (camera at the origin, so the camera-facing test is just
// dot(normal, position) > 0); the result is rotated into world space for output, so any consumer
// gets a canonical world-space normal and rotates to its own space if needed.
//
// Output rgba32float: xyz = world-space normal (unit length, camera-facing), w = raw reversed-Z
// depth (carried along so consumers get a bilateral/rejection reference without a second fetch).
// Sky / cleared pixels (raw depth 0) are written as (0,0,1, 0): w = 0 marks them invalid.

struct Uniforms {
    view_from_proj: mat4x4f,   // depth-buffer -> view-space position (unproject)
    world_from_view: mat4x4f,  // view -> world; its 3x3 rotates the normal to world space
    inv_size: vec2f,           // 1 / render size (full resolution)
    _pad0: vec2f,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var normal_out: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;

fn load_depth(coord: vec2<i32>) -> f32 {
    let size = vec2<i32>(textureDimensions(scene_depth));
    let c = clamp(coord, vec2<i32>(0i), size - 1i);
    return textureLoad(scene_depth, c, 0i).r;
}

fn reconstruct_view_space_position(depth: f32, uv: vec2f) -> vec3f {
    let clip_xy = vec2f(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = uniforms.view_from_proj * vec4f(clip_xy, depth, 1.0);
    return t.xyz / t.w;
}

fn view_position_at(coord: vec2<i32>) -> vec3f {
    let depth = load_depth(coord);
    let uv = (vec2f(coord) + 0.5) * uniforms.inv_size;
    return reconstruct_view_space_position(depth, uv);
}

// Accurate view-space normal reconstruction from depth (atyuwen's 5-tap method); adapted
// unchanged from Encounter's ao_mod demo. For each axis, extrapolate the center depth from the
// two taps on each side and derive the tangent from whichever side predicts it better - stable
// across depth discontinuities where naive derivatives smear.
fn reconstruct_normal(coord: vec2<i32>, pos: vec3f, depth_center: f32) -> vec3f {
    let dl1 = load_depth(coord + vec2<i32>(-1i, 0i));
    let dl2 = load_depth(coord + vec2<i32>(-2i, 0i));
    let dr1 = load_depth(coord + vec2<i32>(1i, 0i));
    let dr2 = load_depth(coord + vec2<i32>(2i, 0i));
    let dt1 = load_depth(coord + vec2<i32>(0i, -1i));
    let dt2 = load_depth(coord + vec2<i32>(0i, -2i));
    let db1 = load_depth(coord + vec2<i32>(0i, 1i));
    let db2 = load_depth(coord + vec2<i32>(0i, 2i));

    let use_left = abs(2.0 * dl1 - dl2 - depth_center) < abs(2.0 * dr1 - dr2 - depth_center);
    let use_top = abs(2.0 * dt1 - dt2 - depth_center) < abs(2.0 * db1 - db2 - depth_center);

    var ddx: vec3f;
    if use_left {
        ddx = pos - view_position_at(coord + vec2<i32>(-1i, 0i));
    } else {
        ddx = view_position_at(coord + vec2<i32>(1i, 0i)) - pos;
    }
    var ddy: vec3f;
    if use_top {
        ddy = pos - view_position_at(coord + vec2<i32>(0i, -1i));
    } else {
        ddy = view_position_at(coord + vec2<i32>(0i, 1i)) - pos;
    }

    var n = normalize(cross(ddy, ddx));
    // Camera-facing: in view space the camera is at the origin, so the view vector is `pos`.
    if dot(n, pos) > 0.0 {
        n = -n;
    }
    return n;
}

@compute @workgroup_size(8, 8, 1)
fn reconstruct(@builtin(global_invocation_id) gid: vec3u) {
    let size = vec2<i32>(textureDimensions(normal_out));
    let coord = vec2<i32>(gid.xy);
    if coord.x >= size.x || coord.y >= size.y {
        return;
    }
    let uv = (vec2f(coord) + 0.5) * uniforms.inv_size;
    let depth = load_depth(coord);
    if depth <= 0.0 {
        // Sky / cleared: no surface. w = 0 marks the texel invalid for consumers.
        textureStore(normal_out, coord, vec4f(0.0, 0.0, 1.0, 0.0));
        return;
    }
    let pos = reconstruct_view_space_position(depth, uv);
    let view_normal = reconstruct_normal(coord, pos, depth);
    // Rotate the view-space normal into world space (3x3 of world_from_view, column-major).
    let m = uniforms.world_from_view;
    let world_normal = normalize(
        m[0].xyz * view_normal.x + m[1].xyz * view_normal.y + m[2].xyz * view_normal.z);
    textureStore(normal_out, coord, vec4f(world_normal, depth));
}
