# Deferred Fog

Mod id `dev.automata.deferred_fog`. Game-linked: hooks game/J3D functions, so it is coupled
to the pinned game build like the shadow mod. Standalone by design: **other mods need no
changes and no awareness of this mod to benefit** — anything composited over the opaque
scene at `SCENE_AFTER_OPAQUE` (Enhanced AO, Realtime Sun Shadows, third-party effects)
automatically ends up *under* the fog.

## The problem it solves

The game applies fog per fragment while drawing: every J3D material bakes fog BP state into
its material display list (`J3DGDSetFog`, written per frame by `d_kankyo`'s
`setLightTevColorType_MAJI_sub` from the environment palette), and a few direct drawers call
`GXSetFog`. Aurora replays those BP registers into per-draw fog state and applies
`fogF = clamp(a / (b − (1 − rawDepth)) − c)` + curve in its generated fragment shaders.
Any screen-space effect composited after the opaque scene therefore multiplies over
*already-fogged* color — AO and shadows visibly darken the fog / aerial perspective itself.

## Architecture

1. **Suppression scope** opens at `GFX_STAGE_SCENE_BEGIN` (the sky lists draw earlier and
   keep their fog) and closes at the first translucent list draw.
2. **J3D interception** (`J3DShape::drawFast` pre-hook, the same pattern as the shadow mod's
   two-sided casters): the material display list has already executed when a shape draws, so
   an immediate `GXSetFog(GX_FOG_NONE)` overrides its fog for that shape's geometry. The
   material's true parameters are read from `material->getPEBlock()->getFog()` for capture.
   Direct drawers are covered by a `GXSetFog` pre-hook that rewrites the type argument, and by
   an identical `GFSetFog` pre-hook: field/tall grass (`dGrass_packet_c`) and flowers
   (`dFlower_packet_c`) are self-drawing opaque packets that program per-room fog through
   `GFSetFog` (a direct BP write, not `GXSetFog`) and never call `J3DShape::drawFast`, so
   without that hook their fog escapes suppression and the deferred quad double-fogs them.
   `GFSetFog` has the same signature as `GXSetFog`, so the same callback captures and
   suppresses it (its only game call site is the grass/flower fog helper, so normal terrain is
   unaffected).
3. **Re-apply** as a fullscreen alpha-blended pass over the resolved opaque depth, pushed at
   the first `J3DShape::drawFast` after `SCENE_AFTER_OPAQUE` — i.e. right before the first
   translucent geometry (water included) rasterizes — with a `FRAME_BEFORE_HUD` fallback for
   frames with no translucent J3D at all. That lands after *every* mod's
   `SCENE_AFTER_OPAQUE` stage callbacks regardless of mod load order, and before water,
   particles, DOF, and bloom, which keep their native forward fog (the painter's dedicated
   particle-fog passes included). Do NOT anchor this on the painter's own list functions
   (`dDlst_list_c::drawXluDrawList` etc.): they inline into their callsites, so a detour
   fires at some unrelated later call — the original implementation did exactly that and the
   fog landed after bloom.
4. **Exactness**: aurora's only per-fragment fog input is the raw depth value — the same
   value in the depth snapshot — and `src/fog_math.h` mirrors the full `J3DGDSetFog` BP
   encode → aurora command-processor decode round trip (11-bit mantissa truncation
   included), so the deferred pass reproduces forward fog bit-identically for opaque pixels
   (verified: coefficients match an independent transcription of both sides exactly, and the
   shader executed on llvmpipe matches a CPU reference of aurora's formula to 0 ULP across
   all five fog curves). GX fog *range adjustment* is set by the game but ignored by
   aurora's command processor, so the deferred pass correctly ignores it too.

## Mixed fog configurations

Fog configurations are compared with tolerances that absorb per-room palette-blend
differences (Δcolor ≤ 6, Δstart/end ≤ 2% of the fog span); anything beyond that is a
distinct configuration. Many areas mix several (rooms lagging the stage's palette blend,
special-fog materials), and the two modes handle that differently (`mixedMode`):

- **Exact (replay), the default**: every fogged draw is suppressed and its configuration
  captured into a per-frame table (up to 8 distinct). A uniform frame takes the ordinary
  single-quad path at no extra cost. A mixed frame replays the opaque draw lists once into
  a mod-owned offscreen pass — same save-replay-resolve bracket as the shadow mod's
  cascades, but with the game's own camera — with each shape's output forced to a flat
  sparse-encoded index color (`(index+1)·24` in red; the material display list has already
  run, so a per-shape TEV/channel override sets it). The resolved buffer selects each
  pixel's exact fog configuration in `fs_mixed`. Caveats:
  - One extra opaque scene of vertex/index streaming per mixed frame. With heavy shadow
    cascade settings this can crowd aurora's fixed per-frame streaming buffers (see the
    shadow doc's budget section) — use Vanilla mode there until adaptive buffers land.
  - Alpha-tested cutouts (foliage holes) replay solid, so a hole resolves to its tree's
    config; pixels not covered by the shape override (rare non-J3D direct drawers) decode
    as invalid and fall back to config 0 (the frame's reference) — exactly what the
    single-config quad applied to them before.
  - Grass/flower packets draw their own geometry (not `J3DShape::drawFast`), so the replay
    can't force them to a flat ID color — their pixels write real texture colors that decode
    as invalid and take config 0. Since their (now suppressed + captured) fog is the room's
    environment fog, config 0 is normally the correct one; only near a room-fog boundary
    could a grass tuft take a slightly wrong config, and grass sits in light near-camera fog
    where the difference is imperceptible.
  - MSAA silhouettes may resolve to an invalid ID on 1-px fringes → reference config.
- **Vanilla**: the original behavior — only draws matching the frame's reference config
  are suppressed; any deviant reverts the scene to forward fog from the next frame until
  it is uniform again. Twilight black fog (type 7 → linear black), wolf-senses white fog
  (type 6), and room transitions all take the vanilla path in this mode.

A scene that uses a special configuration *uniformly* (all draws agree) is deferred
normally in both modes — all five GX fog curves (LIN/EXP/EXP2/REVEXP/REVEXP2) are
implemented in `res/fog.wgsl`, and in exact mode a mixed twilight scene simply carries the
special config in its table like any other.

## Diagnosing fog issues

The symptom of fog NOT being deferred is distinctive: screen-space AO/shadows darken the
fog itself at range (unnatural darkening on distant fog-washed terrain). Tools:

- The mod panel's **Status** line: "Deferring fog (exact: N draws, K configs)" is the
  working state in exact mode ("... replay failed" indicates the ID replay could not run
  and mixed pixels got the reference config); "REVERTED: mixed fog configs" appears only
  in Vanilla mode.
- Transitions are **logged**: mixed↔uniform in exact mode, revert/re-engage (with both
  configs' type/range/color) in Vanilla mode.
- Debug views: **Fog Factor** (the deferred term as grayscale — black while the scene is
  visibly foggy means no quad ran) and **Config IDs** (exact mode, mixed frames: one gray
  band per captured config, showing exactly which geometry resolved to which fog).

## Tunables

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle (off = vanilla forward fog) |
| `mixedMode` | 1 (Exact) | mixed-scene handling: 0 = Vanilla (revert to forward fog), 1 = Exact (per-pixel config-ID replay) |
| `debugView` | 0 | 1 = deferred fog factor as grayscale, 2 = config IDs (exact mode, mixed frames) |

The mods panel shows the **Enabled** toggle, a read-only **Status** line (see "Diagnosing
fog issues"), and an **Open Controls** button; `mixedMode` and `debugView` are SELECT
controls, which the UI only renders inside a window tab (not the flat panel), so they live in
the Open Controls window.

## Known caveats

- Translucents (water, particles) blend over the *fogged* opaque scene and then receive
  their own forward fog — matching vanilla layering.
- If the `J3DShape::drawFast` hook cannot install (missing `dusklight.symdb`), the mod
  loads but stays inert (vanilla fog) and logs a warning.
- Degenerate fog ranges (start == end) produce a zero fog term with a 0/0 singularity in
  vanilla; the deferred pass skips the quad entirely for those.
- ABI-coupled: rebuild against the new `dusklight.lib` after any re-platform.
