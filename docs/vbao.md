# VBAO — Visibility Bitmask Ambient Occlusion

Mod id `dev.automata.vbao` (directory `mods/vbao/`). Service-only (no game code): stages + snapshots from the
gfx service, matrices from the camera service.

**Normals come from the gfx service** (GfxService 1.3), resolved alongside depth —
`GfxResolveDesc::normal` → `GfxResolvedTargets::normal`, reached through
`common/gfx_normal_compat.h`. (Until the move to upstream Dusklight this was a separate
`get_scene_normals` call on our own fork; upstream implements the same feature through the resolve
instead, and there is no such call in the upstream vtable.) The game's
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
- **Two things make it permanently unavailable, and the first is a SETTING.**
  - **MSAA.** The renderer will not create the normal buffer unless antialiasing is off
    (`msaaSamples == 1`); with MSAA on it does not even record the request. So a perfectly capable
    GPU gets no normals, and VBAO disables itself. The log line **names MSAA** in that case — it
    re-queries `GfxDeviceInfo::sample_count` at the moment it warns, rather than trusting the value
    cached at init, because the user can change MSAA mid-session. Telling them their GPU is at fault
    when the fix is one setting would be the worst kind of wrong diagnostic.
  - **The compatibility renderers.** On D3D11 and OpenGL ES the attachment cannot exist at all
    (no WebGPU core features). VBAO disables itself with a one-time log line saying so; it needs a
    D3D12 / Vulkan / Metal device.

The stored direction carries the sign the game gave it and is **never** flipped toward the camera —
see `docs/authored_normals.md` §2a for the three separate places that guard had to be deleted from.

**Install Deferred Fog alongside it.** VBAO composites at `SCENE_AFTER_OPAQUE`, which is *inside*
the fogged frame: without Deferred Fog the game has already applied fog per draw, so the AO
multiplies over fogged pixels and distant occlusion reads as grime on the haze rather than depth in
the world. Deferred Fog moves the fog after the composite and the AO lands under it.

**There is no dependency between the two mods, in either direction.** VBAO composites at
`SCENE_AFTER_OPAQUE` and the fog quad draws at `FRAME_BEFORE_HUD`, so the ordering that matters is
guaranteed by stage separation alone. VBAO's debug views draw at `FRAME_AFTER_HUD` — the last stage
in the frame — so they land on top of the fog without needing the two mods to agree on anything.
See `docs/deferred_fog.md`.

## Pipeline (per frame, at `GFX_STAGE_SCENE_AFTER_OPAQUE`)

1. One `resolve_pass` snapshots **both** depth (R32Float, reversed-Z, single-sample) and the
   scene normal (RGB10A2Unorm, view space). Colour is **not** resolved — the composite blends over
   the live target. Without either input VBAO disables itself for the frame.

   **The normal snapshot latches on.** The first resolve that asks for it enables the attachment
   for the *next* frame and returns null for this one, so an early null means "not yet", not
   "never". VBAO counts consecutive null frames (`kNormalLatchGraceFrames`) and only reports the
   device as unable once the count is past the handful the latch can plausibly take — otherwise the
   "compatibility renderer" warning would fire on every cold start.
2. **`preprocess_depth.wgsl`** — builds a 5-level MIP depth chain (XeGTAO-style weighted
   downsample) so distant AO samples read small MIPs instead of thrashing bandwidth.
3. **`vbao.wgsl`** — the occlusion estimator. Per pixel: unproject the view position, read the
   scene normal from the service snapshot (skipping to full visibility where it has none), derive a
   separate 4-tap geometric plane from depth for sample rejection, **replace the authored normal
   by that geometric one where the two disagree by more than ~37° (see "Untrusted authored
   normals")**, then walk `slice_count`
   hemisphere slices × `steps_per_side` marching steps, carving a 32-bit sector bitmask
   per slice (Therrien et al. 2022 visibility bitmask). Occlusion = carved fraction weighted
   by a cosine lobe. Sampling noise: an order-6 Hilbert index computed in-shader + R2 sequence,
   advanced per frame when temporal accumulation is on (so successive frames measure different
   directions). There is no noise LUT and no init-time upload.
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
   History = rgba16float (ao, viewDepth/far, octahedral view-space normal) at full res,
   ping-ponged; invalidated on resize/toggle. **Two history candidates per pixel**: the
   camera-reprojected one and the un-reprojected one at the pixel's own position, each scored on
   depth *and* normal agreement with the current surface; the camera one is preferred and the static
   one taken only when clearly the better surface match. That is the substitute for per-object
   motion vectors: a screen-static character (Link under a following camera) takes the static
   candidate on every curved part of his body, which is what stopped his AO smearing along the
   world's motion.
   In **Half Res** this pass is also a **temporal upsampler** — see below.
6. **`composite.wgsl`** — reads the AO source at its native resolution: full-res history 1:1 when
   temporal accumulation is on, else a depth-aware 4-tap bilinear upscale of the half-res estimate.
   Then black point, contrast power, optional distance fade, multiply over scene color. Debug
   views 1–8 (AO / normals / depth / staircase detector / geometric normal / normal agreement /
   raw AO / depth MIP 3 — the last four are described under "Temporal accumulation: history and
   diagnostics").

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

### Untrusted authored normals (1.1.1)

**Symptom:** after 1.1.0 a small number of users still saw AO on open surfaces — a tree trunk, a
fence — that changed with viewing angle and flickered in motion. Raising the black point hid it.
Their debug views located it exactly: in Normals (view 2) the vertical fence posts were **green**,
i.e. their authored normals point straight up, while Geo Normal (view 5) showed them facing the
camera as they geometrically do. Normal Agreement (view 6) was red and white on the fence and the
trunk and green on the ground and Link — and the red/white regions were the broken AO.

**Cause: NOT established.** The reports that still show this come from old AMD drivers (an RDNA2
laptop on a November 2024 driver, an RDNA1 5600 XT on maintenance-only drivers). Two game-side
candidates exist — props and foliage whose normals were authored for flat lighting, and J3D shapes
that load their normal matrix by index from an array `J3DModel::viewCalc` fills at the simulation
tick (`J3DShapeMtx::loadMtxIndx_PNGP`) — but **both are CPU-side data and would reproduce on every
GPU.** If an NVIDIA machine shows correct (camera-facing, blue) fence posts in view 2 at the same
spot, the normal buffer's *contents* differ per driver, and the cause is in the renderer path those
drivers run: aurora's generated shader transforms the normal as
`vec4f(nrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx]`, a dynamically indexed `mat3x4` array in a uniform
buffer placed after the larger `postex_mtx` array, and writes it to a second `RGB10A2Unorm` colour
target. A driver that fetched the wrong array element would produce exactly what view 2 shows — a
coherent normal in the wrong frame, varying with the angle between that frame and the view — while
positions stay correct. That would be an aurora/Dawn/driver report, not a mod bug. What decides
it: view 2 from an NVIDIA machine at the same spot, and the affected user's `adapter:` log line
plus the same view with the other backend (D3D12 ↔ Vulkan) and, if possible, a current driver.
Either way, the runtime check below catches it, because it tests the normal the mod actually
receives. A hemisphere
centred 40–90° off the real surface carves sectors out of the very plane the samples lie in —
AO on open geometry, varying with the angle between the wrong normal and the view.

**Fix:** `vbao.wgsl` compares the authored normal with the geometric one it already builds for
sample rejection. At `dot ≥ 0.8` (≤ ~37°, which covers smooth shading on any low-poly curvature — a
hexagonal trunk deviates at most 30°) the authored normal is kept in full; below that it blends to
the geometric normal, fully geometric at `dot ≤ 0.6` or when it points into the surface. The
temporal pass applies the same rule (with a full-res geometric normal from the raw depth) before
using the normal for history identity, so a wrong, unstable normal no longer rejects history from
frame to frame. The geometric normal is a flat facet, but it is the plane the samples lie in, so it
carves nothing on open geometry. View 6 still shows the *raw* agreement, so it keeps working as the
diagnostic for this.

### Temporal accumulation: history and diagnostics

**Status: resolved in 1.1.0, confirmed in the field at each step.** This section is the record of
the 1.0.x report and of what each change was for, so the design above is not mistaken for a set of
arbitrary knobs.

**The report.** Flickering AO in motion, overwhelmingly on AMD GPUs, with two NVIDIA reports as
well. It was the temporal path: disabling Temporal Accumulation removed the flicker, and Motion
Response 0–1 with accumulation on almost entirely removed it. The wrong-looking AO in motion
(improper angles, hard edges, with a smooth normal buffer) was the **raw single-frame estimate**
being displayed whenever the velocity term drove the blend weight to 1, its slice directions
advancing every frame. Frame interpolation is on by default in the shipped build, so frame rate was
not the variable; a first pass that said so was wrong and is withdrawn.

**What was ruled out**, each read in the pinned upstream source, so it need not be re-derived:

| Ruled out | Evidence |
|---|---|
| Depth snapshot format / precision | `Depth32Float` (compile-time in aurora) blitted to `R32Float` by a fullscreen pass on every backend (`tex_copy_conv.cpp` `snapshot_depth`) |
| Normal attachment path | `RGB10A2Unorm`, written `unit_nrm*0.5+0.5` with alpha 1/0, **no blend state** on that target, write mask tied to depth writes, copied 1:1 at the pass break (`gx.cpp:332`, `shader.cpp:1648`, `encoding.cpp:317`) |
| Buffer size mismatch | frame, depth and normal buffers all created at the same `(width, height)` in `resize_swapchain_internal`; snapshots copied at the pass's colour-attachment size |
| Viewport inset | the scene viewport is forced to `(0,0,FB_WIDTH,FB_HEIGHT)` (`m_Do_graphic.cpp:2252`) and both user policies map it to the full target (`map_logical_viewport`) |
| Camera identity under interpolation | the stage hook receives `&camera_p->view`, the same object `camera_apply_presentation()` rewrites through `dComIfGd_getView()` (`view_setup` → `dComIfGd_setView(view)`) |
| Projection convention | camera service `e=-p22, f=-p23` matches aurora's `proj.m2 *= -1` reversed-Z conversion; reversed-Z is `constexpr` |
| Uniform staging | mapped staging buffers copied per frame (`FrameSlotCount = 2`), aurora already clamps `minUniformBufferOffsetAlignment` for AMD (`f88a7e7`) |
| Pipeline cache | keys are Dawn's, which hash the WGSL source |
| Vendor-dependent shader semantics | shifts are masked, every `textureLoad` clamps, no `var<workgroup>` race in the MIP prefilter (Bevy's, re-checked), no wave ops, NaN guards negated (`!(len > eps)`) |
| Upstream aurora after the pin | four commits, none touching rendering |

**What shipped, in order, and what each fixed:**

1. **Frame-time cap on the velocity term** (`kVelocityFusionFrameTime`, next section): a full
   history reset is only reachable when frames are short enough for per-frame noise to fuse.
2. **In-shader Hilbert noise.** `vbao.wgsl` computes the order-6 Hilbert index per pixel instead of
   reading a 64×64 `R16Uint` texture the host uploaded with `wgpuQueueWriteTexture` at init. That
   upload ran on the game thread while the render worker submitted, on a host that does **not**
   enable Dawn's `implicit_device_synchronization` toggle — the one path in the chain that could
   genuinely differ per driver, and a LUT reading as zero gives every pixel the same slice
   directions: directional, hard-edged AO that changes every frame. Not proven to be the cause, and
   correct either way (the procedural index is verified to be the permutation the LUT held).
3. **Low Motion Response** removed the flicker in the field, and traded it for ghosting: moving
   occluders left trails (Link's contact AO staying on ground he had left — the receiver reprojects
   correctly, its stored AO is simply stale, and the depth test cannot see that). The content
   reject had been an absolute `|history − current|` test against the noisy single-frame sample,
   firing on noise and missing trails; it became a **σ-normalised outlier test against the 3×3
   mean** (discard from ~1σ to 2.5σ), and the clamp tightens to 0.6k under screen motion.
4. Environment ghosting gone; a **full-body trail behind Link** remained regardless of settings.
   The disocclusion tolerance floor was `0.002` of the **far plane** — on TP's per-stage far planes
   hundreds of world units, more than a character's separation from the ground behind him, so the
   trail passed as "same surface". The tolerance is now **relative to the pixel's own depth**
   (≥ 1.5%). Nothing in this scene is measured in far-plane fractions any more.
5. A very soft trail just behind Link remained: structural, because the reprojection is the camera's
   and Link is nearly static on screen while the world moves, so the camera-reprojected history for
   a pixel on his body is a *neighbouring* part of his body. Per-object motion vectors would fix it
   exactly, but the port's interpolation matrices live in the game (`dusk::interp`), not in aurora,
   and a per-pixel motion attachment is a renderer change plus game-side plumbing. Within the mod:
   the **two-candidate history** (the history stores the octahedral normal, the temporal pass reads
   the scene normal, and the un-reprojected candidate wins wherever depth or normal says the
   reprojected one is a different part of the surface). Cost: one extra history fetch and a normal
   load per pixel; the history stays 8 bytes/pixel. Confirmed "phenomenal for Link".
6. Link's AO then read less full; Motion Response 100% fixed that and made distant, broad AO sparse
   in motion. Both are one fact at two distances: the raw estimate is dense close to the camera
   (constant pixel radius, fine world sampling) and sparse far away. The velocity response now
   **fades with view depth** (`motionRange`), and the default response is 100%. That is 1.1.0.

**Could the original report have been a driver issue?** Possibly; it cannot be proven or excluded
from source. The paths where a driver can differ are the noise upload (removed), Dawn's
inter-dispatch barriers for storage textures (Dawn-managed, heavily exercised), and frame pacing
(AMD's Vulkan driver exposes no Mailbox present mode), which feeds the per-frame velocity term and
is what the cap addresses.

**If a temporal report comes in again,** measure before theorising. View 7 (Raw AO) at rest is the
first thing to look at: uniform directional streaking there means the noise, tiles mean the
prefilter, and a clean view 7 with flicker in view 1 means the temporal pass itself.

| View | Shows | If it is broken on the affected machine |
|---|---|---|
| 5 Geo Normal | the face normal `vbao.wgsl` derives from depth for its rejection plane | the depth chain / position reconstruction is wrong |
| 6 Normal Agreement | `dot(authored, geometric)` banded: green > 0.95, yellow 0.8–0.95 (normal on smoothed low-poly curvature), red < 0.8, **white = negative** (authored normal points away from the surface), blue = no authored normal, magenta = degenerate geometry | red/white on **flat ground** with a clean view 5 means the authored normal reaches the mod in the wrong space, sign or frame |
| 7 Raw AO | the single-frame estimate before denoise/accumulation, unshaped | wrong here = the occlusion pass itself; fine here but wrong in view 1 = denoise or temporal |
| 8 Depth MIP 3 | the coarse prefiltered level the march samples at distance | tiles/blocks = the prefilter chain |

**Protocol for the reporter:** same spot, standing still, screenshots of views 1, 2, 5, 6, 7 and 8,
plus the mod's log lines `adapter: …` (GPU and backend) and `frame time …`. A green view 6 with a
clean view 5 and a broken view 7 points into `vbao.wgsl`'s march; anything else points upstream of
it. Do not propose a mechanism without those.

### Motion response and frame rate

The velocity term in `temporal.wgsl` is `screen motion in pixels per FRAME × motionResponse`, so
one and the same camera pan produces twice the pixels per frame at 30 fps as at 60, and five times
as many as at 144. At the 1.0.x default (0.1 per pixel, uncapped) a pan of 10 px/frame drove the
blend weight to 1.0, i.e. threw the whole history away every frame and displayed the raw
single-frame estimate, whose R2 sampling pattern advances every frame. At 144 Hz the eye fuses that
into a mild shimmer; at 30–60 Hz it is plain boiling. The term is now **ceilinged by
`kVelocityFusionFrameTime / frame time`** (`update_velocity_cap()` in `mod.cpp`, 4 ms), measured on
the stage hook itself so it stays service-only: a full reset stays available above 250 fps, 144 fps
allows ~0.58, 60 fps ~0.24 and 30 fps ~0.12, where the term drops below the base blend weight and
is inert. The disocclusion and content rejects are **not** capped. The mod logs
`frame time X ms (Y fps): motion response ceiling Z` whenever the smoothed interval moves by more
than 25%. The response itself (default 100%) also fades with view depth — full up to `motionRange`,
gone at twice it — see the tunables.

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
| `temporalFrames` | 8 | accumulation length → alpha = 1/frames |
| `temporalClamp` | 200 | neighborhood clamp k ×0.01 (mean ± kσ over 3×3); tightened to 0.6k at ≥ 16 px/frame of screen motion |
| `motionResponse` | 100 | accumulation shortening per pixel of screen motion **per frame** ×0.01, applied within `motionRange` and faded out beyond it, and capped by a frame-time-aware ceiling — see "Motion response and frame rate" below. Field-validated at 100 for full, responsive AO on characters once the range fade protected distant AO |
| `motionRange` | 5000 | world units of view depth up to which `motionResponse` applies in full; fades to nothing at 2×. Near geometry is sampled densely (clean single frames), distant AO sparsely (needs the accumulation); 0 = no fade |
| `contentThresh` | 100 | history outlier threshold ×0.01, in **sigmas of the 3×3 local AO distribution** (100 = discard from ~1σ to 2.5σ from the local mean). Was an absolute `|history − current|` test against the noisy single-frame sample, which fired on noise and missed trails; the σ-normalised test against the mean is what removes moving-occluder ghosting now that the velocity term no longer resets history |
| `disoccTol` | 0 | disocclusion depth tolerance, % of the pixel's own depth (0–20; below 1.5 acts as 1.5). The old floor was `0.002` of the **far plane**, i.e. 400 world units on a 200000-unit stage, larger than Link, so ground he had just vacated kept his AO as a full-body trail |
| `denoisePasses` | 1 | spatial passes 0–3 (ping-pong parity is mirrored on the CPU side —
  see mod-api-notes) |
| `denoiseStrength` | 60 | per-pass blur blend, % (0 raw, 100 full blur). Lowered from full so the sharper temporal result keeps its detail |
| `halfRes` | off | compute occlusion at half resolution. With temporal accumulation on, a jittered temporal upsampler reconstructs full-res detail (near-full-res look at ¼ the occlusion cost); with it off, a depth-aware bilinear upscale (softer) |
| `distanceFade` | off | fade AO out toward the far plane |
| `fadeStart` / `fadeEnd` | 15000 / 40000 | fade band, world units of view depth (converted from far-plane % for the same reason as the radius ramp band) |
| `debugMode` | 0 | 0 off, 1 AO, 2 normals, 3 depth, 4 staircase, 5 geometric normal, 6 normal agreement, 7 raw AO, 8 depth MIP 3 (see "Temporal accumulation: history and diagnostics") |
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
