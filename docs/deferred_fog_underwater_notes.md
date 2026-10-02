# Deferred Fog: underwater fog term (design, not built)

A designed feature for Deferred Fog that is **not implemented**: nothing in `mods/deferred_fog`
does any of this. This note holds what is needed to build it.

## The problem

With VBAO on, ambient occlusion darkens submerged terrain, and seen from above the water that
darkening does not fade with distance the way above-water AO fades into the fog. Over a large deep
lake the lakebed keeps sharp AO far out, so the occlusion reads as dark patches under the water.

The fix must re-apply a missing attenuation after AO, in the style of the deferred fog. Masking or
fading the AO by height is not the goal.

## What the game does

Checked against the game source at the `v2.0.0` pin.

1. **No fog setter distinguishes geometry below the water.** The global fog comes from
   `g_env_light.mFogNear`, `mFogFar` and `fog_col` through `GxFog_set` → `GxFogSet_Sub` →
   `GXSetFog`. Room fog goes through `dKy_GxFog_tevstr_set`. Neither looks at the water plane:
   submerged terrain gets the same fog as the terrain around it. So there is no underwater fog to
   capture and re-apply; it has to be synthesized.
2. **The game's underwater treatment depends on the camera, not on the geometry.**
   `g_env_light.camera_water_in_status` is set by the camera when its eye is below the water surface
   (`dCamera_c`, `getWaterSurfaceHeight`). It selects the underwater palette (`pselect_id[8]` /
   `[9]`, except in `D_MN08D` and `D_MN01A`), the underwater light-colour ratio
   (`water_in_col_ratio_*`), and the full-screen underwater filter (`dKy_undwater_filter_draw`, in
   the 2D-screen list). None of it applies while the camera is above the water looking down.
3. **The haze over submerged terrain, seen from above, is the water surface itself.** Water actors
   such as `daLv3Water_c::Draw` and `daGrdWater_c::Draw` draw their surface translucent in the
   `XluListDarkBG` list with BTK-animated materials, plus a second model in the Invisible list
   (`daLv3Water_c` gives it a screen-projected texture matrix). Nothing in their draw code depends on
   the depth of water beneath, so the surface does not deepen its tint with the water column. Distant
   lakebed looks softer only because more ordinary stage fog accumulates over farther terrain.

So the feature synthesizes the missing depth attenuation. The deferred fog pass is the place for
it: it runs after every mod's `SCENE_AFTER_OPAQUE` composite, over the opaque scene, before the
water surfaces draw.

## Design: a synthesized underwater term

Add a term to the fog pass: for opaque pixels below the water surface, an extra fog that deepens
with the **water-column depth** (surface height − pixel world height), composited with the stage
fog. Because the pass runs after AO, this fades the lakebed's AO the way distance fog fades
above-water AO.

### Inputs

- **Water surface height.** `fopAcM_getWaterY(const cXyz* pos, f32* waterY)`
  (`f_op/f_op_actor_mng.h`) returns 1 and writes the surface height when there is water at that
  position, otherwise returns 0 and writes −∞. Probe at the player's position,
  `dComIfGp_getLinkPlayer()->current.pos`: the player is the reliable anchor at the water. The camera
  eye is an alternative but fails when the camera is over the shore.
- **World position per pixel.** `CameraService::get_camera` (`mods/svc/camera.h`) with the stage
  context's `game_view` returns `CameraInfo::world_from_proj`, the one-step depth-buffer → world
  matrix. The pass already samples the same reversed-Z depth.

### Shader (`res/fog.wgsl`)

A new `UnderwaterUniforms` at **binding 4**, used by both `fs_main` and `fs_mixed` (it collides with
neither's bindings 0–3):

```
struct UnderwaterUniforms {
    world_from_clip: mat4x4f,
    color: vec4f,      // water colour the terrain fades toward (rgb)
    water_y: f32,
    half_depth: f32,   // water-column depth at which the term reaches 50%
    max_strength: f32, // cap, 0..1
    enabled: f32,
}
```

A helper composites the stage fog `(base_rgb, base_f)` with the underwater term as one src-over
`(colour, alpha)`:

```
fn apply_underwater(base_rgb, base_f, uv, depth) -> vec4f {
    if enabled == 0 { return (base_rgb, base_f); }
    let clip = vec4(uv.x*2-1, 1-2*uv.y, depth, 1);
    let w = world_from_clip * clip;
    let world_y = w.y / w.w;
    if world_y >= water_y { return (base_rgb, base_f); }
    let col = water_y - world_y;
    let uw_f = clamp((1 - exp2(-col / max(half_depth,1))) * max_strength, 0, 1);
    let out_a = base_f + uw_f - base_f*uw_f;
    let out_rgb = out_a>1e-5 ? ((1-uw_f)*base_f*base_rgb + uw_f*color.rgb)/out_a : base_rgb;
    return (out_rgb, out_a);
}
```

Call it in `fs_main` and `fs_mixed` right after the stage fog factor is computed, return
`(o.rgb, o.a)`, and in the Fog Factor debug view return `o.a`. The sky early-outs stay (the sky is
above the water). Validate the shader with `build/wgsl_check` (`CONTRIBUTING.md` "Check your
change").

### Host (`src/mod.cpp`)

- Mirror `UnderwaterUniforms` in C++: `float world_from_clip[16]; float color[4]; float water_y,
  half_depth, max_strength, enabled;` (96 bytes, a multiple of 16), with `static_assert`s.
- Import `CameraService` and include `mods/svc/camera.h` and `f_op/f_op_actor_mng.h`.
- In `on_scene_after_opaque`, call `get_camera` with `stageCtx->game_view` and keep
  `world_from_proj`; probe the player's position with `fopAcM_getWaterY`. Disable the term when
  there is no water or the option is off.
- In `push_fog_quad`, build and `push_uniform` the underwater block as a second uniform range, add
  its offset and size to `DrawPayload` (it stays well under 128 bytes), and add binding 4 to both
  bind groups in `on_draw`.
- Options in the controls window: `underwaterFog` (bool, default off), `underwaterHalfDepth` (world
  units, about 400), `underwaterStrength` (0–100 %, about 70), `underwaterColorR/G/B` (0–255, a murky
  teal around 25/55/55).
- Reset the new state in `shutdown`, and document the feature and its limits in
  `docs/deferred_fog.md`.

## Limitations and open questions

- **Flat water plane.** One `water_y` per frame from one probe is right for a lake, wrong for sloped
  rivers, waterfalls, or several water bodies at different heights in view. A per-pixel water depth,
  made by replaying the water actors into a depth buffer (the same machinery as the
  configuration-ID replay) and comparing view-space depths, is exact and handles every case, at the
  cost of one more replay per frame.
- **Probe location.** A view of water from a distant hill, with the player away from the water, gets
  no term. A per-pixel water depth removes this limit too.
- **Colour.** Fading toward the fog colour would brighten deep water, so the term needs its own murky
  water colour. The values are a matter of taste and need tuning in-game.
- **The water surface draws on top.** The term tints the lakebed before the translucent water draws
  over it. Check that it reads correctly through the water's own tint rather than doubling it; the
  underwater colour may need to sit close to the water tint.
- **Cost.** One more small uniform and a world-position reconstruction per fog pixel: negligible. The
  per-pixel-water-depth variant adds a water-actor replay per frame.
