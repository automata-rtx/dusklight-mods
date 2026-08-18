# VBAO — Visibility Bitmask Ambient Occlusion

Mod id `dev.automata.vbao` (directory `mods/vbao/`). Service-only (no game code): stages + snapshots from the
gfx service, matrices from the camera service.

**Normals come from the gfx service** (`get_scene_normals`, GfxService 1.3) as of 1.6.0. The game's
renderer writes the artist-authored vertex normal into a second colour attachment on the scene pass,
and the HOST snapshots it once per frame — immediately after the opaque lists, before any
`SCENE_AFTER_OPAQUE` hook — then hands the same texture to every mod that asks. VBAO just asks.

That means **no dependency on any other mod**: VBAO imports only the stock services (gfx, camera,
config, ui, resource, log). It also means **no reconstructed *shading* normal** — no fallback path
to maintain, and none of the faceting a depth-gradient normal has by construction.

> **`geometric_normal_view()` in `vbao.wgsl` is still a depth-derived normal, and it stays.** It is
> not leftover reconstruction. It is the plane used to reject occlusion samples that lie *below* the
> surface, which is a property of the geometry rather than of the artist's smoothed vertex normal —
> and it is now permanently load-bearing, because the shading normal is always the authored one.
> Deleting it does not simplify the port; passing it a zero vector switches AO off entirely. See
> `docs/authored_normals.md` §8.11 and §8.11a. The snapshot is
full render resolution and already in view space, so half-res AO samples it at each chain pixel's
jittered full-res position and temporal accumulation integrates full-res normal detail even in
half-res mode, at no change to the AO sampling cost.

Two consequences worth knowing:

- **Pixels with no authored normal take full visibility.** Alpha 0 means there is no usable normal
  here, so there is no hemisphere to build and VBAO leaves the pixel alone. The *attachment's*
  coverage is exactly the depth buffer's — a draw writes a normal iff it writes depth — but **alpha
  1 is not implied by depth coverage**: a draw whose NRM vertex attribute is simply absent writes
  depth and stores alpha 0, as does a vertex normal that interpolation cancelled to zero. So this is
  not only sky and billboards. Those pixels also have their denoiser edge weights zeroed, which
  keeps their full-visibility value from bleeding into neighbours as a bright rim — sky never needed
  that because sky is depth-discontinuous and the depth gate already rejected it, but an alpha-0
  *surface* is depth-continuous with everything around it. Debug view 2 doubles as the coverage
  map: it paints those pixels black.
- **The compatibility renderers cannot provide it.** On the D3D11 and OpenGL ES backends the
  snapshot is unavailable and VBAO disables itself with a one-time log line saying so. It needs a
  core-features device (D3D12 / Vulkan / Metal).

The stored direction carries the sign the game gave it and is **never** flipped toward the camera —
see `docs/authored_normals.md` §2a for the three separate places that guard had to be deleted from.

## Pipeline (per frame, at `GFX_STAGE_SCENE_AFTER_OPAQUE`)

1. `get_scene_normals` returns the host's per-frame normal snapshot (taken before this stage), and
   `resolve_pass` snapshots depth (R32Float, reversed-Z, single-sample). Colour is **not** resolved —
   the composite blends over the live target. Without either input VBAO disables itself for the
   frame.
2. **`preprocess_depth.wgsl`** — builds a 5-level MIP depth chain (XeGTAO-style weighted
   downsample) so distant AO samples read small MIPs instead of thrashing bandwidth.
3. **`vbao.wgsl`** — the occlusion estimator. Per pixel: unproject the view position, read the
   scene normal from the service snapshot (skipping to full visibility where it has none), derive a
   separate 4-tap geometric plane from depth for sample rejection, then walk `slice_count`
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
5. **`temporal.wgsl`** (compute) — runs at **full render resolution**. Reprojects last frame's
   accumulation (`reproject = prev.proj_from_world × cur.world_from_view`), rejects history on
   depth disocclusion (expected-prev-depth vs stored depth), clamps history into the local
   mean ± k·σ neighborhood, and shortens accumulation on screen motion and content mismatch.
   History = rg32float (ao, viewDepth/far) at full res, ping-ponged; invalidated on resize/toggle.
   In **Half Res** this pass is also a **temporal upsampler** — see below.
6. **`composite.wgsl`** — reads the AO source at its native resolution: full-res history 1:1 when
   temporal accumulation is on, else a depth-aware 4-tap bilinear upscale of the half-res estimate.
   Then black point, contrast power, optional distance fade, multiply over scene color. Debug
   views 1–4 (AO / normals / depth / staircase detector).

The occlusion estimate runs at snapshot resolution, or half of it with **Half Res** on; the
temporal history and composite are always full render resolution.

### Half-res temporal upsampling

With **Half Res** and **Temporal Accumulation** both on, the half-res estimate is reconstructed
back to full resolution rather than blurred up (restores the aurora fork's checkerboard quality):

- The half-res sampling grid is **jittered** through the 4 sub-positions of each 2×2 full-res block
  (4-phase, keyed off `frame_index`), in `preprocess_depth.wgsl` (`load_input_depth`) and
  `vbao.wgsl` (`chain_uv`). Each frame therefore estimates a different quarter of the full-res
  pixels. The jitter is derived shader-side (no uniform-layout change) and is a no-op at full res
  or with temporal off.
- `temporal.wgsl` runs at full res: a full-res pixel **covered** by this frame's jitter takes the
  fresh half-res sample and accumulates it (clamp + content-reject guard ghosting); an **uncovered**
  pixel carries history forward, falling back to the depth-aware bilinear upscale only when it has
  no valid history (fresh disocclusion) or camera motion/disocclusion forces it. Full coverage
  refreshes every ~4 frames. Full-res depth comes from the raw snapshot (the temporal pass reuses
  the same depth texture the prefilter consumes); the history textures are sized to the full render
  resolution (`ensure_targets`).
- At full res every pixel is trivially "covered", so this reduces to the original per-pixel
  accumulation with no behavior change. GPU-validated in `scratchpad/halfres_taau_test.py`.

## Tunables (config vars; UI shows them in sections)

Ints are fixed-point (usually /100) unless noted.

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle |
| `quality` | 2 (High) | 0 Low 3/2, 1 Med 5/2, 2 High 7/3, 3 Ultra 9/3, 4 Custom — slices/steps |
| `customSlices` / `customSteps` | 7 / 3 | used when quality = Custom (1–16 / 1–8) |
| `radius` | 200 | effect radius up close, % of view depth (depth-proportional world radius) |
| `radiusFar` | 800 | effect radius at long view distance (same scale). The radius ramps from `radius` to this across the band below — tight contact detail near, broad landmark depth far. 0 disables (constant `radius`) |
| `radiusRampStart` / `radiusRampEnd` | 0 / 10000 | radius ramp band, **world units** of view depth (same scale as the shadow mod's Coverage). Not far-plane fractions: the far plane is per-stage and far beyond the visible field, so fractions of it were scene-dependent and absurdly compressed (the useful range was 0–5%). The mod logs the stage's far plane on change for calibration |
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
| `denoiseStrength` | 60 | per-pass blur blend, % (0 raw, 100 full blur). Lowered from full so the sharper temporal result keeps its detail |
| `halfRes` | off | compute occlusion at half resolution. With temporal accumulation on, a jittered temporal upsampler reconstructs full-res detail (near-full-res look at ¼ the occlusion cost); with it off, a depth-aware bilinear upscale (softer) |
| `distanceFade` | off | fade AO out toward the far plane |
| `fadeStart` / `fadeEnd` | 15000 / 40000 | fade band, world units of view depth (converted from far-plane % for the same reason as the radius ramp band) |
| `debugMode` | 0 | 0 off, 1 AO, 2 normals, 3 depth, 4 staircase |
| `debugDepthRange` | 3300 | depth debug view gradient scale in world units (visualization only) |

Debug views draw at `FRAME_BEFORE_HUD` (the normal composite stays at `SCENE_AFTER_OPAQUE`)
so deferred fog, translucency, and bloom never paint over them — judging AO strength through
a fogged debug view reads as much weaker than the effect actually is.

## Defaults rationale + performance notes

Defaults were chosen to match the look the user approved on the aurora branch: High quality,
intensity/contrast 150, thickness 150, 5-frame accumulation, 1 denoise pass; later in-game
tuning moved radius to 200 near / 800 far (distance ramp) and denoise strength to 60%.
Exposing everything costs nothing per frame — values upload in one uniform buffer that is
written every frame regardless; only `quality`/`halfRes`/`denoisePasses` change the actual
GPU work. Hardcoding would not measurably help: the shader reads the uniform once per pixel.

Suggested experiments (from the porting session): Ultra quality; `denoisePasses 0` with
temporal on (sharpest, tests accumulation quality); `blackPoint` 5–8 to clean broad floors;
`distanceFade` on with 40/90 against TP's fog; `halfRes` with temporal on (the jittered upsampler
reconstructs full-res detail, so it stays close to full-res at ~¼ the occlusion cost — a strong
default candidate) or at high supersampling.

## History / provenance

Ported from our earlier pre-mod-API implementation in the `dusklight-ao` + `aurora-ao`
forks onto Encounter's upstream `ao_mod` demo framework: the demo
contributed the MIP depth chain, compute scheduling, and denoiser; ours contributed the
bitmask estimator, temporal accumulation, depth-aware upscale, thickness/contrast/black
point, and distance fade. Never reference MXAO in code or comments.
