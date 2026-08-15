// Depth to Normal - reconstruct.wgsl
//
// Produces the per-pixel world-space surface normal (+ raw depth) that other mods consume
// (ambient occlusion, GI, shadows, antialiasing), once per frame. There are two sources:
//
//   * AUTHORED (preferred): the game's own interpolated vertex normal, written by the renderer
//     into the scene pass's second color attachment and handed to us as a snapshot (RGB10A2Unorm:
//     xyz = view-space normal * 0.5 + 0.5, w = 1 when that draw supplied a normal attribute).
//     Smooth by construction - no faceting, so no smoothing pass is needed. Ten bits per axis is
//     what keeps low-curvature surfaces from banding; the two alpha bits only ever carry a flag.
//   * RECONSTRUCTED (fallback): atyuwen's accurate 5-tap depth-gradient method, adapted UNCHANGED
//     from Encounter's ao_mod demo (which ports it from Bevy Engine's SSAO; see res/licenses/).
//     A cross product of screen-space position deltas is the flat face normal of the rasterized
//     triangle, so this is per-facet by construction. Used per-pixel wherever the authored normal
//     is absent (w = 0: sky, UI, billboards, non-depth-writing effects), and globally when the
//     user turns authored normals off or the platform has no normal buffer.
//
// Both are defined in view space (camera at the origin, so the camera-facing test is just
// dot(normal, position) > 0) and both are rotated into world space for output, so any consumer
// gets a canonical world-space normal and rotates to its own space if needed.
//
// Output rgba32float: xyz = world-space normal (unit length, camera-facing), w = raw reversed-Z
// depth (carried along so consumers get a bilateral/rejection reference without a second fetch).
// Sky / cleared pixels (raw depth 0) are written as (0,0,1, 0): w = 0 marks them invalid.
//
// When debug_compare is set, the OTHER normal (whichever of the two did not become the output) is
// also written to alt_out as xyz = world normal, w = authored validity, purely so the debug view
// can show the two side by side and diff them. It costs a second full-size target and is only on
// while a comparison view is selected.

struct Uniforms {
    view_from_proj: mat4x4f,   // depth-buffer -> view-space position (unproject)
    world_from_view: mat4x4f,  // view -> world; its 3x3 rotates the normal to world space
    inv_size: vec2f,           // 1 / render size (full resolution)
    use_authored: f32,         // 1 = prefer the authored normal, 0 = always reconstruct
    debug_compare: f32,        // 1 = also write the other normal to alt_out
    basis_flip: vec3f,         // per-axis sign for the decoded authored normal (basis diagnostic)
    _pad0: f32,
}

@group(0) @binding(0) var scene_depth: texture_2d<f32>;
@group(0) @binding(1) var normal_out: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> uniforms: Uniforms;
// Authored view-space normal snapshot. Always bound: a 1x1 zero-filled stand-in takes its place
// when the platform has no normal buffer (a bind group must always match its layout), and a zero
// texel reads as validity 0, i.e. "reconstruct this pixel".
@group(0) @binding(3) var authored_normal: texture_2d<f32>;
// Debug-only alternate output; a 1x1 stand-in when no comparison view is active.
@group(0) @binding(4) var alt_out: texture_storage_2d<rgba32float, write>;

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

// Rotate a view-space normal into world space (3x3 of world_from_view, column-major).
fn to_world(view_normal: vec3f) -> vec3f {
    let m = uniforms.world_from_view;
    return normalize(
        m[0].xyz * view_normal.x + m[1].xyz * view_normal.y + m[2].xyz * view_normal.z);
}

@compute @workgroup_size(8, 8, 1)
fn reconstruct(@builtin(global_invocation_id) gid: vec3u) {
    let size = vec2<i32>(textureDimensions(normal_out));
    let coord = vec2<i32>(gid.xy);
    if coord.x >= size.x || coord.y >= size.y {
        return;
    }
    let comparing = uniforms.debug_compare > 0.5;
    let uv = (vec2f(coord) + 0.5) * uniforms.inv_size;
    let depth = load_depth(coord);
    if depth <= 0.0 {
        // Sky / cleared: no surface. w = 0 marks the texel invalid for consumers.
        textureStore(normal_out, coord, vec4f(0.0, 0.0, 1.0, 0.0));
        if comparing {
            textureStore(alt_out, coord, vec4f(0.0, 0.0, 1.0, 0.0));
        }
        return;
    }
    let pos = reconstruct_view_space_position(depth, uv);

    // Decode the authored normal. Renormalize always: vertex interpolation, quantization and any
    // MSAA resolve all denormalize the stored vector.
    let a_size = vec2<i32>(textureDimensions(authored_normal));
    let a = textureLoad(authored_normal, clamp(coord, vec2<i32>(0i), a_size - 1i), 0i);
    var authored_valid = a.w > 0.5;
    var authored_view = vec3f(0.0, 0.0, 1.0);
    if authored_valid {
        let raw = (a.xyz * 2.0 - 1.0) * uniforms.basis_flip;
        // Length is the confidence signal. A texel written by ONE surface decodes to a unit
        // vector - 10-bit quantization moves it by well under 0.01. Anything materially shorter
        // is a HARDWARE MSAA RESOLVE AVERAGE, and its direction is a blend that corresponds to
        // no real surface:
        //
        //   * partial coverage against unwritten (cleared) samples decodes to k*n + (k-1),
        //     which for k = 0.75 is barely half unit length and points somewhere else entirely;
        //   * a silhouette pixel shared by two surfaces decodes to (n1 + n2) / 2, whose length
        //     is cos(half the angle between them).
        //
        // The alpha channel does NOT catch either case: it averages to k, so any pixel more than
        // half covered still reads valid, and a two-surface pixel reads a fully-confident 1.0.
        // That is why these texels survived the validity test and showed up as a wrong-coloured
        // rim on silhouettes - and, downstream, as the shadow mod's attached-shadow term failing
        // on exactly the thin back-lit features (a nose tip, boot and tunic edges) whose pixels
        // are partially covered.
        //
        // Rejecting them here hands those pixels to the depth reconstruction, which is built for
        // silhouettes (side-selected taps). 0.92 keeps genuine curvature - adjacent samples a few
        // degrees apart still measure ~0.999 - while rejecting blends past about 45 degrees.
        // With MSAA off every covered texel measures 1.0, so this costs nothing and changes
        // nothing there.
        let len = length(raw);
        if len < 0.92 {
            authored_valid = false;
        } else {
            authored_view = raw / len;
            // Flip ONLY when the surface is clearly inverted - a two-sided sheet (foliage, cloth)
            // seen from behind, whose normal points almost straight away from the camera.
            //
            // This deliberately does NOT use the reconstruction's zero threshold. That test is
            // safe for a FACE normal, which on a front-facing triangle never legitimately points
            // away. It is wrong for an AUTHORED normal: a smooth, interpolated normal field
            // crosses dot(n, view) = 0 exactly at the visual silhouette by construction, and on
            // low-poly geometry it goes well past perpendicular before the triangle ends (an
            // 8-sided cylinder reaches ~22 degrees, dot ~ 0.37). A zero threshold negates every
            // one of those pixels, which showed up as a wrong-coloured band hugging every
            // silhouette - widening and narrowing with camera angle, since the size of the region
            // where dot(n, view) > 0 is itself view-dependent - and, through the service, as the
            // shadow mod's attached-shadow term failing on curved back-lit features, because a
            // flipped normal inverts n.L.
            //
            // 0.5 separates the two cases cleanly: a silhouette band tops out near 0.4, while a
            // genuinely inverted back-face points nearly straight away (0.7 to 1.0). VBAO and
            // SSILVB already re-apply their own camera-facing guard with a 0.15 margin for the
            // same reason, so AO keeps the orientation it wants while consumers that need the
            // true surface direction (the shadow bias and its n.L terminator) now get it.
            if dot(authored_view, normalize(pos)) > 0.5 {
                authored_view = -authored_view;
            }
        }
    }

    var view_normal: vec3f;
    var alt_view = vec3f(0.0, 0.0, 1.0);
    if uniforms.use_authored > 0.5 && authored_valid {
        view_normal = authored_view;
        if comparing {
            alt_view = reconstruct_normal(coord, pos, depth);
        }
    } else {
        // Either the user picked the reconstruction, or this pixel has no authored normal.
        view_normal = reconstruct_normal(coord, pos, depth);
        // The alternate is the authored normal where there is one; where there isn't, mirror the
        // output so a difference view reads zero instead of noise (w = 0 flags it either way).
        alt_view = select(view_normal, authored_view, authored_valid);
    }

    textureStore(normal_out, coord, vec4f(to_world(view_normal), depth));
    if comparing {
        textureStore(alt_out, coord, vec4f(to_world(alt_view), select(0.0, 1.0, authored_valid)));
    }
}
