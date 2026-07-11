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
   Direct drawers are covered by a `GXSetFog` pre-hook that rewrites the type argument.
3. **Re-apply** as a fullscreen alpha-blended pass over the resolved opaque depth, pushed
   from a pre-hook on `dDlst_list_c::drawXluDrawList` — the first translucent list. That
   point is after *every* mod's `SCENE_AFTER_OPAQUE` stage callbacks regardless of mod load
   order, and before water, particles, DOF, and bloom, which keep their native forward fog
   (the painter's dedicated particle-fog passes included).
4. **Exactness**: aurora's only per-fragment fog input is the raw depth value — the same
   value in the depth snapshot — and `src/fog_math.h` mirrors the full `J3DGDSetFog` BP
   encode → aurora command-processor decode round trip (11-bit mantissa truncation
   included), so the deferred pass reproduces forward fog bit-identically for opaque pixels
   (verified: coefficients match an independent transcription of both sides exactly, and the
   shader executed on llvmpipe matches a CPU reference of aurora's formula to 0 ULP across
   all five fog curves). GX fog *range adjustment* is set by the game but ignored by
   aurora's command processor, so the deferred pass correctly ignores it too.

## Special fog configurations

Materials are only suppressed when their fog matches the frame's **reference
configuration** (the first fogged draw of the frame — normally stage geometry), within
tolerances that absorb per-room palette-blend differences (Δcolor ≤ 6, Δstart/end ≤ 2% of
the fog span). Anything beyond that is a *deviant*: it keeps its forward fog, and its
presence turns suppression off from the next frame on — the scene reverts to exact vanilla
fog until it is uniform again (one transition frame may double-fog deviant pixels). This is
how the special cases stay correct rather than being flattened:

- **Twilight black fog**: materials authored with fog type 7, converted by `d_kankyo` to
  linear *black* — color mismatch → vanilla path.
- **Wolf-senses white fog**: type 6 (REVEXP, forced white) → type mismatch → vanilla path.
- Room/weather palette transitions where actors lag the stage by more than the tolerance.

A scene that uses a special configuration *uniformly* (all draws agree) is deferred
normally — all five GX fog curves (LIN/EXP/EXP2/REVEXP/REVEXP2) are implemented in
`res/fog.wgsl`.

## Tunables

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle (off = vanilla forward fog) |
| `debugView` | 0 | 1 = deferred fog factor as grayscale |

## Known caveats

- Translucents (water, particles) blend over the *fogged* opaque scene and then receive
  their own forward fog — matching vanilla layering.
- If the `J3DShape::drawFast` hook cannot install (missing `dusklight.symdb`), the mod
  loads but stays inert (vanilla fog) and logs a warning.
- Degenerate fog ranges (start == end) produce a zero fog term with a 0/0 singularity in
  vanilla; the deferred pass skips the quad entirely for those.
- ABI-coupled: rebuild against the new `dusklight.lib` after any re-platform.
