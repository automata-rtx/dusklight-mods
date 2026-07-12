# VBAO — Enhanced Ambient Occlusion

Mod id `dev.automata.enhanced_ao`. Service-only (no game code): stages + snapshots from the
gfx service, matrices from the camera service.

## Pipeline (per frame, at `GFX_STAGE_SCENE_AFTER_OPAQUE`)

1. `resolve_pass` snapshots scene color + depth (R32Float, reversed-Z, single-sample).
2. **`preprocess_depth.wgsl`** — builds a 5-level MIP depth chain (XeGTAO-style weighted
   downsample) so distant AO samples read small MIPs instead of thrashing bandwidth.
3. **`vbao.wgsl`** — the occlusion estimator. Per pixel: reconstruct view position, pick the
   better-conditioned side per axis for the normal (atyuwen 5-tap), then walk `slice_count`
   hemisphere slices × `steps_per_side` marching steps, carving a 32-bit sector bitmask
   per slice (Therrien et al. 2022 visibility bitmask). Occlusion = carved fraction weighted
   by a cosine lobe. Sampling noise: Hilbert LUT + R2 sequence, advanced per frame when
   temporal accumulation is on (so successive frames measure different directions).
   Thickness handling: front/back horizons with a log-scaled thickness and depth-difference
   fade (`t_eff = t_base * clamp(1 - |dz|/depth_range)`) — this is what keeps grass/foliage
   from over-darkening.
4. **`denoise.wgsl`** — edge-aware 3×3 spatial filter, ping-ponged 0–3 times. With temporal
   ON it softens the residual per-frame noise; with temporal OFF it is the whole denoiser
   (single-frame fallback).
5. **`temporal.wgsl`** (compute) — reprojects last frame's accumulation
   (`reproject = prev.proj_from_world × cur.world_from_view`), rejects history on
   depth disocclusion (expected-prev-depth vs stored depth), clamps history into the local
   mean ± k·σ neighborhood, and shortens accumulation on screen motion and content mismatch.
   History = rg32float (ao, viewDepth/far), ping-ponged; invalidated on resize/toggle.
6. **`composite.wgsl`** — depth-aware 4-tap upscale (matters in Half Res mode), black point,
   contrast power, optional distance fade, multiply over scene color. Debug views 1–4
   (AO / normals / depth / staircase detector).

The chain runs at snapshot resolution, or half of it with **Half Res** on.

## Tunables (config vars; UI shows them in sections)

Ints are fixed-point (usually /100) unless noted.

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle |
| `quality` | 2 (High) | 0 Low 3/2, 1 Med 5/2, 2 High 7/3, 3 Ultra 9/3, 4 Custom — slices/steps |
| `customSlices` / `customSteps` | 7 / 3 | used when quality = Custom (1–16 / 1–8) |
| `radius` | 100 | effect radius, % of view depth (depth-proportional world radius) |
| `radiusMax` | 40 | screen-space radius cap, % of viewport height. The search radius is constant in screen space, so this only engages (bounding sampling cost) when `radius` is pushed very high; at normal values it has no visible effect |
| `intensity` | 150 | final strength multiplier ×0.01 (up to 500) |
| `contrast` | 150 | value power ×0.01 — deepens (>100) or lifts the falloff |
| `blackPoint` | 3 | % occlusion floor removed then rescaled (cleans flat surfaces — VBAO leaves a faint floor on open surfaces that reads as whole-screen darkening; 3 clears it) |
| `thickness` | 150 | occluder thickness ×0.01 (log-scaled internally) |
| `thickFade` | 150 | thickness fade range, ×0.01 of view radius |
| `thickDist` | 60 | distance thickness: radius-proportional thickness floor, ‰ of the view radius. The log-scaled base thickness becomes a vanishing fraction of the (depth-proportional) radius with distance and starves mid/far occlusion; this restores it. 0 = old behavior |
| `depthBias` | 4 | self-occlusion bias, ‰ toward camera |
| `temporal` | on | temporal accumulation master |
| `temporalFrames` | 5 | accumulation length → alpha = 1/frames |
| `temporalClamp` | 200 | neighborhood clamp k ×0.01 |
| `motionResponse` | 10 | accumulation shortening per pixel of motion ×0.01 |
| `contentThresh` | 100 | content-mismatch response threshold ×0.01 |
| `disoccTol` | 0 | disocclusion depth tolerance, % of depth (0–20). 0 rejects most aggressively; a small fixed depth floor still admits history on matching surfaces, minimizing distant ghosting |
| `denoisePasses` | 1 | spatial passes 0–3 (ping-pong parity is mirrored on the CPU side —
  see mod-api-notes) |
| `halfRes` | off | run the chain at half the snapshot resolution |
| `distanceFade` | off | fade AO out toward the far plane |
| `fadeStart` / `fadeEnd` | 40 / 90 | fade band, % of far plane |
| `debugMode` | 0 | 0 off, 1 AO, 2 normals, 3 depth, 4 staircase |
| `debugDepthRange` | 3300 | depth debug view gradient scale in world units (visualization only) |

Debug views draw at `FRAME_BEFORE_HUD` (the normal composite stays at `SCENE_AFTER_OPAQUE`)
so deferred fog, translucency, and bloom never paint over them — judging AO strength through
a fogged debug view reads as much weaker than the effect actually is.

## Defaults rationale + performance notes

Defaults were chosen to match the look the user approved on the aurora branch: High quality,
radius 100, intensity/contrast 150, thickness 150, 5-frame accumulation, 1 denoise pass.
Exposing everything costs nothing per frame — values upload in one uniform buffer that is
written every frame regardless; only `quality`/`halfRes`/`denoisePasses` change the actual
GPU work. Hardcoding would not measurably help: the shader reads the uniform once per pixel.

Suggested experiments (from the porting session): Ultra quality; `denoisePasses 0` with
temporal on (sharpest, tests accumulation quality); `blackPoint` 5–8 to clean broad floors;
`distanceFade` on with 40/90 against TP's fog; `halfRes` at 6× supersampling (the composite
upscale is depth-aware, so silhouettes stay crisp).

## History / provenance

Ported from our aurora-fork implementation (frozen at `standalone-final` in
automata-rtx/dusklight-ao + aurora-ao) onto the upstream `ao_mod` demo's framework: the demo
contributed the MIP depth chain, compute scheduling, and denoiser; ours contributed the
bitmask estimator, temporal accumulation, depth-aware upscale, thickness/contrast/black
point, and distance fade. Never reference MXAO in code or comments.
