# Deferred Fog — underwater AO fade (SUPERSEDED — synthesized-fog design not built)

> **Superseded.** The underwater AO fade was shipped by a **different, non-destructive** mechanism:
> a Water Plane service (Graphics Hub `hub_water`) that probes the water height, plus an AO-term fade
> inside VBAO's / SSILVB's own composite — no painted fog. See `docs/underwater_ao_fade.md`. This
> note is kept only for the engine investigation in the "Investigation findings" section below (still
> valid) and to record why the synthesized-fog design here was **rejected**: it adds haze even with
> no AO, which violates the non-destructive requirement. **Do not build the "Proposed design" below.**

Status (original): **investigated and designed, intentionally not built.** The user asked to shelve
it after the design was validated but before shipping. Nothing in this doc is wired into the mod; the
synthesized-fog path here was never added to `deferred_fog`.

## The problem

With VBAO on, ambient occlusion darkens submerged (underwater) terrain and that darkening does
**not** fade with distance the way above-water AO does. Above water, deferred fog washes distant
terrain toward the fog color, which softens the AO into the aerial perspective — the effect the
user likes. Underwater, the lakebed keeps sharp AO even far out over a large deep lake, so the
occlusion reads as harsh dark patches under the water instead of fading.

The user's constraints: they want a **deferred-fog-style solution** (re-apply a missing
attenuation after AO), not a crude AO mask/height-fade (which was tried on the old aurora fork
and looked poor).

## Investigation findings (definitive — from the game source in the fetched dusklight/ tree)

1. **There is no separate underwater fog on the terrain.** The engine programs one global stage
   fog (`g_env_light.mFogNear`/`mFogFar`/`fog_col`, `d_kankyo.h:351,372,373`) onto ALL geometry
   via `GXSetFog` (`d_kankyo.cpp:9388`, `GxFogSet_Sub`). Submerged lakebed and dry shore share
   the same fog. There is no "this poly is below the water plane → different fog" branch.
   → So there is **nothing to capture and re-apply** here; the deferred-fog capture path is not
   the fix.

2. **The engine's only "underwater" treatment is gated on the CAMERA being submerged**, not on
   the geometry. `dKy_camera_water_in_status_check()` / `g_env_light.camera_water_in_status`
   (set in `d_camera.cpp:1303-1310`; getter/setter `d_kankyo.cpp:10525-10532`;
   field `d_kankyo.h:449`) drives a palette/fog swap (`d_kankyo.cpp:1937-1958`), an underwater
   color multiply (`water_in_col_ratio_*`, `water_in_light_col`, gated at 2300-2307), and the
   full-screen distortion overlay (`dKy_undwater_filter_draw`, `d_kankyo.cpp:8125-8177`). All of
   it is skipped in the above-water-looking-down case.

3. **The submerged "haze" seen from above is purely the flat translucent water surface.** The
   water models (`daLv3Water_c::Draw` `d_a_obj_lv3Water.cpp:336`; `daGrdWater_c::Draw`
   `d_a_obj_groundwater.cpp:264`) draw translucent into `XluListDarkBG` with **material-driven,
   BTK-animated alpha — NOT depth-driven**. The `C_MTXLightPerspective`/`setEffectMtx` block is
   a projected reflection texgen, not a scene-depth read. So the water does not deepen its tint
   with the water column; distant lakebed looks softer only because more of that flat tint plus
   the ordinary stage fog accumulate over farther terrain.

Conclusion: to fade underwater AO we must **synthesize** the missing depth attenuation
ourselves. The deferred-fog pass is the right place (it runs after AO, over the opaque scene).

## Proposed design — synthesized underwater fog

Add an "underwater fog" term to the existing deferred-fog fullscreen pass: for opaque pixels
below the water surface, apply an extra fog that deepens with the **water-column depth**
(surface Y − pixel world Y), composited WITH the normal stage fog. Because the fog pass runs
after every mod's `SCENE_AFTER_OPAQUE` composite, this fades the AO on distant lakebed exactly
like distance fog fades above-water AO.

### Inputs (all confirmed available)

- **Water surface height**: `fopAcM_getWaterY(const cXyz* xz, f32* out)` returns 1 + surface Y
  when there is water at that XZ, else 0/`-inf` (`f_op_actor_mng.cpp:2327`, header
  `f_op/f_op_actor_mng.h:704`; also `fopAcM_wt_c::getWaterY()` global cache, header :899).
  Probe at the player's position — `dComIfGp_getLinkPlayer()->current.pos` (the shadow mod uses
  this pattern, `realtime_sun_shadows/src/mod.cpp:1461`). Player is the reliable "at the water"
  anchor; camera eye XZ is an alternative but fails when the camera sits over the shore.
- **World position per pixel**: reconstruct from uv + raw depth using
  `world_from_clip = world_from_view * view_from_proj` (both in `CameraInfo`,
  `mods/svc/camera.h:30,32`; multiply column-major like VBAO's `mat4_mul_col`). Same reversed-Z
  depth the fog already samples.

### Shader (res/fog.wgsl) — validated to compile (naga OK)

New `UnderwaterUniforms` at **binding 4**, referenced by BOTH `fs_main` and `fs_mixed` (binding
4 collides with neither the single path's 0/1 nor the mixed path's 0/2/3):

```
struct UnderwaterUniforms {
    world_from_clip: mat4x4f,
    color: vec4f,      // water color the terrain fades toward (rgb)
    water_y: f32,
    half_depth: f32,   // water-column depth at which haze reaches 50%
    max_strength: f32, // cap 0..1
    enabled: f32,
}
```

Helper composites base stage fog with the underwater term as one src-over (color, alpha):

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

Call it in `fs_main` and `fs_mixed` right after the base `fog_z` is computed; return `(o.rgb,
o.a)`; in the fog-factor debug view return `o.a` (combined factor). Sky early-outs stay (sky is
above water).

### Host (src/mod.cpp)

- Mirror `UnderwaterUniforms` in C++ (`float world_from_clip[16]; float color[4]; float water_y,
  half_depth, max_strength, enabled;` → 96 bytes, `%16==0`).
- Import `CameraService`; include `mods/svc/camera.h`, `f_op/f_op_actor_mng.h` (already have
  `d/d_com_inf_game.h`).
- In `on_scene_after_opaque` (has `stageCtx->game_view`): `get_camera` → compute
  `g_worldFromClip = world_from_view * view_from_proj`; probe player XZ with `fopAcM_getWaterY`
  → `g_waterY`, `g_hasWater`. Disable the term when no water or the toggle is off.
- `push_fog_quad`: build the underwater uniform (world_from_clip, color from cvars, water_y,
  half_depth, max_strength, enabled = hasWater && toggle), `push_uniform` it (a SECOND uniform
  range beside the fog/mixed one), and add binding 4 to BOTH bind groups in `on_draw`.
- `DrawPayload`: add `uw_offset` / `uw_size` (payload stays < 128 B).
- Config vars (in the controls window tab): `underwaterFog` (bool, default OFF),
  `underwaterHalfDepth` (world units, ~400 default), `underwaterStrength` (0-100 %, ~70),
  `underwaterColorR/G/B` (0-255, murky teal default ~25/55/55).
- Version → 1.4.0; shutdown resets; doc the feature + limitations.

## Limitations / open questions to resolve when resuming

- **Flat water plane assumption**: one `water_y` per frame from a single probe. Fine for a lake;
  wrong for sloped rivers, waterfalls, or multiple water bodies at different heights in view. A
  robust version would capture per-pixel water-surface depth by replaying the water actors into
  a depth buffer (same replay machinery as the config-ID pass) and compare view-space depths —
  heavier but exact, and it also fixes the multi-level case.
- **Probe location**: player XZ works when the player is at the water; a scene viewed from a
  distant hill (player/camera not over water) gets no term. Per-pixel water depth (above) also
  removes this limitation.
- **Color/tuning**: fading toward the fog color would brighten deep water (wrong); a dedicated
  murky water color is needed. Values are pure taste — must be tuned in-game.
- **Interaction with the water surface**: the synthesized fog darkens/tints the lakebed BEFORE
  the translucent water draws over it. Verify it reads correctly through the water tint rather
  than double-tinting; may want the underwater color close to the water tint.
- **Cost**: adds a second small uniform + world reconstruction per fog pixel — negligible. The
  per-pixel-water-depth variant adds one water-actor replay per frame (streaming budget, like
  the config-ID replay).
