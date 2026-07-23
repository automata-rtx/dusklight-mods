# Underwater AO Fade (VBAO / SSILVB)

Fades screen-space ambient occlusion back toward "no occlusion" on submerged terrain as the water
column deepens, so a large deep lakebed does not keep harsh full-strength AO far out. It mirrors, for
underwater surfaces, what Deferred Fog's distance fade already does above water (wash distant AO into
aerial perspective).

Status:
- **VBAO** — implemented (1.6.0). Awaiting in-game tuning of the defaults.
- **SSILVB** — pending (same three edits, AO term only; ported after VBAO validates in-game).
- Provider — **Graphics Hub** `hub_water` (1.1.0).

Background / why not synthesized fog: see `docs/deferred_fog_underwater_notes.md`. That shelved
approach painted synthesized fog in the deferred-fog pass, which adds haze *even with no AO* and so
violates the non-destructive requirement below. This feature supersedes it.

## Design (non-destructive by construction)

Overriding requirement: with no AO (or the feature off) the frame must be byte-identical to vanilla.
Two choices satisfy that:

1. **Water height from a game query, not a re-render.** `hub_water` calls `fopAcM_getWaterY` at the
   player XZ once per frame and caches the surface world Y. A query draws nothing and touches no GX
   state, so the provider is provably invisible unless a consumer reads it.
2. **Fade the AO term, don't paint anything.** Inside each effect's own composite:

   ```
   uw_fade = clamp((1 - exp2(-(water_y - world_y) / max(half_depth, 1))) * strength, 0, 1)  // submerged only
   visibility = mix(visibility, 1.0, uw_fade)     // visibility in [0,1]; 1.0 = no darkening
   ```

   No AO ⇒ `visibility == 1.0` ⇒ `mix(1,1,x) == 1` ⇒ pixel untouched. `uw_fade` is gated off entirely
   when the service is absent, there is no water at the probe, the pixel is above the plane, or the
   toggle is off — so **only already-AO-darkened submerged pixels ever change.**

Topology: Graphics Hub (game-linked) publishes the service; VBAO and SSILVB (service-only) consume
it, staying off the ABI treadmill exactly like they already consume Depth-to-Normal.

## Service — `mods/graphics_hub/include/water_plane_service.h`

`dev.automata.water_plane` (v1.0). `get_frame(ctx, WaterPlaneFrame*)` returns `{ water_y, has_water }`
for the frame; `has_water == false` is a valid "no water" result, not an error. Call it from a
game-thread stage callback (the provider probes at `GFX_STAGE_SCENE_BEGIN` and `get_frame` just hands
back the cached scalars).

## Consumer integration (VBAO; SSILVB identical, AO term only)

- Import `IMPORT_OPTIONAL_SERVICE(WaterPlaneService, svc_water)` (optional ⇒ mod still loads without
  the provider; fade simply disabled).
- In the `SCENE_AFTER_OPAQUE` uniform-build callback, call `get_frame` and set `water_y`,
  `uw_half_depth`, `uw_strength` in the uniform, plus a **flag bit** gated on
  `underwaterToggle && has_water` (VBAO **bit 4 / `16u`** — bit 3 is the external-normal flag;
  SSILVB **bit 8 / `256u`**). `has_water` is folded into the flag bit, so the uniform carries exactly
  three water floats, not four.
- **World-Y reconstruction:** the composites reconstruct *view* space only, but `water_y` is world
  space. The uniform carries `world_from_view` (from `CameraInfo.world_from_view`); the composite
  does `world_y = (world_from_view * vec4(view_pos, 1)).y`.
- **Uniform mirroring:** `world_from_view` (mat4) + the three water floats are added to the C struct
  and **every** shader that declares the `Uniforms` struct (VBAO: 5 shaders; SSILVB: 6), byte-
  identical, size `%16==0`, `static_assert` kept true. VBAO `AoUniforms` is now 464 B (consumed all
  three trailing pad floats); SSILVB `SsilvbUniforms` will re-pad to two trailing floats.

## Config / UI (per mod, "Underwater Fade" section)

- `underwaterFade` — bool, default **on** (safe: non-destructive when there is no AO / no water).
- `underwaterHalfDepth` — int world units, default **400**. Water-column depth at which the fade
  reaches half strength; larger fades in more gradually.
- `underwaterStrength` — int %, default **100**. Max fade applied to deep submerged AO.

Floats aren't config-bindable — register ints and scale in the host (`÷100` for strength).

## Known limitation + upgrade path (not built)

The player probe yields **one horizontal plane per frame** — correct for a single lake, wrong for
sloped rivers / waterfalls / multiple water bodies at different heights, and it produces no fade when
the player is not over water (distant-hill view). The additive upgrade is **per-pixel water-surface
depth**: replay the water actors (`daLv3Water_c::Draw` / `daGrdWater_c::Draw`, `XluListDarkBG`) into
an offscreen depth-only pass and take the view-ray water column per pixel — same `WaterPlaneService`
API, returning a depth texture instead of a scalar. Ship the flat-plane version first; only build the
upgrade if a real multi-level-water scene shows the seam.
