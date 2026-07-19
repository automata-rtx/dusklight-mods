// Cascade depth copy (Staggered Cascades). The gfx service's cascade depth resolve is
// frame-pooled - its view is only valid for the frame that rendered it - so a cascade that
// skips its replay next frame needs the map preserved somewhere it owns. This copies the
// resolve bit-exactly (R32Float load/store) into the mod-owned cache texture the composite
// binds on skip frames.

@group(0) @binding(0) var src_depth: texture_2d<f32>;
@group(0) @binding(1) var dst_depth: texture_storage_2d<r32float, write>;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
    let dims = textureDimensions(dst_depth);
    if (id.x >= dims.x || id.y >= dims.y) {
        return;
    }
    let depth = textureLoad(src_depth, vec2i(id.xy), 0).r;
    textureStore(dst_depth, vec2i(id.xy), vec4f(depth, 0.0, 0.0, 0.0));
}
