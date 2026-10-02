# VBAO: Visibility Bitmask Ambient Occlusion

Screen-space ambient occlusion using a per-slice 32-sector visibility bitmask (Therrien, Levesque
and Gilet, 2023, arXiv:2301.11376), with temporal accumulation, an edge-aware denoiser and a
depth-aware composite.

| | |
| :-- | :-- |
| Mod id | `dev.automata.vbao` (`mods/vbao/`) |
| Version | see `mods/vbao/mod.json` (1.1.0 at the time of writing) |
| Kind | Service-only: graphics, camera, config, UI, resource and log services. No game headers, no hooks, no dependency on other mods |
| Runs at | `GFX_STAGE_SCENE_AFTER_OPAQUE` (debug views at `GFX_STAGE_FRAME_AFTER_HUD`) |
| Needs | The game's authored normals from the graphics service: a D3D12, Vulkan or Metal device |

A bitmask records *which* parts of each hemisphere slice are occluded rather than tracking a single
horizon angle. Gaps between separate occluders, and thin geometry such as grass, therefore do not
over-darken the way they do with horizon-based AO.

"Enhanced AO" is the mod's former name. It still appears in pipeline, texture and log labels, so
search for both when reading logs or GPU captures.

## Files

| File | Contents |
| :-- | :-- |
| `src/mod.cpp` | Option tables and UI, the per-frame stage hook (`on_scene_after_opaque`), compute callback (`on_compute`), draw callback (`on_draw`), texture management (`ensure_targets`), lazily built composite pipelines (`ensure_composite_pipelines`), the frame-time cap (`update_velocity_cap`) |
| `res/preprocess_depth.wgsl` | Depth MIP chain |
| `res/vbao.wgsl` | The occlusion estimator |
| `res/denoise.wgsl` | Edge-aware spatial filter |
| `res/temporal.wgsl` | Temporal accumulation and half-res upsampling |
| `res/composite.wgsl` | Final shaping and composite, and all debug views |
| `res/licenses/` | Bevy (MIT/Apache-2.0) and XeGTAO (MIT) notices for the adapted MIP chain and denoiser |

The framework (MIP depth chain, compute scheduling, denoiser) comes from upstream Dusklight's
`ao_mod` demo, which itself adapts Bevy and XeGTAO. The bitmask estimator, temporal accumulation,
half-res upsampling and the composite controls are this mod's own.

## Requirements and interactions

**Normals.** VBAO reads the game's authored vertex normal, which the renderer writes into a second
colour attachment of the scene pass. It asks for it through `resolve_pass` (via
`common/gfx_normal_compat.h`). Without it VBAO does nothing and logs one warning:

- **Start-up.** The first request turns the normal attachment on for the *next* frame and returns
  nothing for this one. VBAO waits `kNormalLatchGraceFrames` (8) frames before treating missing
  normals as permanent, so a normal start-up is silent.
- **Compatibility renderers** (D3D11, OpenGL ES) cannot carry the attachment. VBAO needs D3D12,
  Vulkan or Metal.
- **MSAA.** The renderer only creates the attachment without MSAA. The current game build
  (Dusklight `v2.0.0`) never enables MSAA, so this cannot happen today; the warning that names MSAA
  is kept for builds that do.

The normal is in view space and keeps the sign the game gave it; it is never flipped toward the
camera. Pixels with no authored normal (alpha 0: sky, billboards, draws without a normal attribute)
get full visibility, and their denoiser edge weights are zeroed so that value does not bleed into
neighbours as a bright rim. Debug view 2 shows them black.

**Deferred Fog.** Without Deferred Fog, the game has already fogged each surface when VBAO
composites, so AO darkens the fog itself and distant occlusion reads as grime on the haze. With it,
the fog is re-applied after VBAO and the AO sits under the fog. The two mods do not know about each
other; the ordering comes from the frame: Deferred Fog draws its fog at the start of the translucent
lists, directly after `SCENE_AFTER_OPAQUE`, so the game's later framebuffer copies, screen filters
and bloom all see the AO under the fog.

**SMAA** also runs at `SCENE_AFTER_OPAQUE`. Their relative order follows mod load order and is not
fixed; either order looks fine.

## How it works

### Per frame

On the game thread, in `on_scene_after_opaque`:

1. Return early if disabled (and invalidate the temporal history).
2. Read the camera, and `resolve_pass` for depth (`R32Float`, reversed-Z) and the normal
   (`RGB10A2Unorm`, view space, `n*0.5+0.5`, alpha = valid). Colour is not resolved: the composite
   multiplies over the live target. If either input is missing, skip the frame.
3. `ensure_targets` (re)creates the textures when the chain size changes. The chain is the render
   size, or half of it with **Half Resolution** on.
4. Fill one uniform block (`AoUniforms`, 336 bytes, shared by every pass) and `push_uniform`.
5. `push_compute` for the compute chain, then `push_draw` for the composite. With a debug view
   selected, the draw is pushed from the `FRAME_AFTER_HUD` hook instead.
6. Flip the temporal history and remember this frame's camera for next frame's reprojection.

On the render worker, `on_compute` records the compute passes and `on_draw` the composite.

### Passes

All compute shaders use 8×8 workgroups and share one compute pass.

| # | Pass | Shader / entry | Resolution | Reads | Writes |
| :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | Depth prefilter | `preprocess_depth.wgsl` / `preprocess_depth` | chain | scene depth | depth MIPs 0–3 |
| 2 | Last MIP | `preprocess_depth.wgsl` / `downsample_mip4` | chain | MIP 3 | MIP 4 |
| 3 | Occlusion | `vbao.wgsl` / `vbao` | chain | depth MIPs, scene normal | `aoNoisy`, packed edge weights |
| 4 | Spatial denoise, 0–3 times | `denoise.wgsl` / `spatial_denoise` | chain | AO, edge weights | AO (ping-pong `aoNoisy` ↔ `aoFinal`) |
| 5 | Temporal (if on) | `temporal.wgsl` / `temporal_accumulate` | **full** | denoised AO, previous history, MIP 0, scene depth, scene normal | new history |
| 6 | Composite (draw) | `composite.wgsl` / `vs_main`, `fs_main` | full | AO source, depths, normal | scene colour, multiply blend |

| Texture | Format | Size |
| :-- | :-- | :-- |
| Preprocessed depth | `R32Float`, 5 MIP levels | chain |
| `aoNoisy`, `aoFinal` | `R32Float` | chain |
| Edge weights | `R32Uint` (packed) | chain |
| History ×2 | `RGBA16Float`: AO, view depth ÷ far, octahedral view-space normal | full render size |

**Depth prefilter.** A 5-level MIP chain (XeGTAO-style weighted downsample), so distant samples read
small MIPs instead of thrashing bandwidth. In half-res mode, MIP 0 is a jittered point sample of the
full-res depth (see [Half-res upsampling](#half-res-upsampling)).

**Occlusion.** Per pixel: reconstruct the view position, read the authored normal (or return full
visibility where there is none), then walk `slice_count` hemisphere slices × `steps_per_side`
steps, carving a 32-bit sector bitmask per slice. Occlusion is the carved fraction weighted by a
cosine lobe.

- *Sampling noise*: an order-6 Hilbert index computed in the shader plus an R2 sequence, advanced
  every frame when temporal accumulation is on, so successive frames sample different directions.
- *Thickness*: front and back horizons with a log-scaled thickness plus a radius-proportional floor
  (`thickDist`), faded by depth difference. This is what keeps grass and foliage from
  over-darkening.
- *Rejection plane*: `geometric_normal_view()` derives a face normal from depth. It is not a
  fallback for the authored normal. It defines the plane below which samples are rejected, which is
  a property of the actual geometry rather than of the smoothed vertex normal. It is load-bearing:
  feeding it a zero vector switches AO off. See `docs/authored_normals.md` §8.11.
- *Radius*: a fraction of view depth that ramps from `radius` to `radiusFar` across
  [`radiusRampStart`, `radiusRampEnd`] world units, then is capped at `radiusMax` of the screen
  height. At default settings the cap is reached from roughly 4,600 world units of view depth (at a
  60° field of view), so beyond that **Max Screen Radius**, not Far Radius, sets the radius.

**Denoise.** An edge-aware 3×3 filter, ping-ponged 0–3 times, blended by `denoiseStrength`. With
temporal on it softens the remaining per-frame noise; with temporal off it is the only denoiser.

**Composite.** Reads the AO 1:1 when the source is full resolution (temporal on, or half-res off);
otherwise it does a depth-aware 4-tap upscale of the half-res result. Then, in order: black point
(remove a small uniform floor and rescale), contrast (power), optional distance fade (back to 1
across [`fadeStart`, `fadeEnd`] world units of view depth), intensity (`mix(1, v, intensity)`),
multiplied over the scene colour.

The composite pipelines are built lazily on the render worker from the live
`GfxDrawContext::layout` and rebuilt when `layout.key` changes (`ensure_composite_pipelines`).
VBAO's own request for normals changes the scene pass shape a frame after start-up; see
`CONTRIBUTING.md` "Rules that have bitten before".

### Temporal accumulation

The history is reprojected with the camera: `reproject = previous proj_from_world × current
world_from_view`. Per pixel:

- **Two history candidates.** The camera-reprojected one, and the un-reprojected one at the pixel's
  own position. Each is scored on surface identity: depth mismatch relative to the pixel's own
  depth, plus normal mismatch against the stored octahedral normal. The reprojected one is
  preferred; the static one is taken only when it is clearly the better match. This stands in for
  per-object motion vectors, which a service-only mod cannot get: a character the camera follows
  (Link) is nearly static on screen, so his pixels take the static candidate.
- **Disocclusion reject.** A candidate whose depth differs by at least `disoccTol` of the pixel's
  own depth (floor 1.5%) is discarded, fully at 3×.
- **Neighbourhood clamp.** History is clamped to mean ± k·σ of the 3×3 neighbourhood
  (`temporalClamp`), tightened to 0.6k at 16 px/frame or more of screen motion.
- **Outlier test.** History far from the 3×3 mean, measured in σ (`contentThresh`), is discarded.
  This is what removes trails behind moving occluders.
- **Motion response.** Screen motion in pixels per frame × `motionResponse` shortens the
  accumulation. It applies fully up to `motionRange` world units of view depth and fades out by
  twice that, because the raw estimate is dense up close and sparse far away. It is also capped by
  frame time (`update_velocity_cap`, `kVelocityFusionFrameTime` = 4 ms): a full history reset is
  only allowed when frames are short enough for per-frame noise to fuse visually. The ceiling is
  about 0.58 at 144 fps, 0.24 at 60 fps and 0.12 at 30 fps. The disocclusion and outlier rejects are
  not capped.

The history is invalidated when the textures are reallocated, when the effect or temporal
accumulation is turned off, and when depth or normals are unavailable.

### Half-res upsampling

With **Half Resolution** and **Temporal Accumulation** both on (the defaults), the half-res
estimate is reconstructed to full resolution instead of blurred up:

- The half-res sample grid is jittered through the 4 positions of each 2×2 full-res block, keyed off
  the frame index (`load_input_depth` in `preprocess_depth.wgsl`, `chain_uv` in `vbao.wgsl`). Each
  frame estimates a different quarter of the full-res pixels.
- The temporal pass runs at full resolution. A pixel covered by this frame's jitter takes the new
  sample and accumulates; an uncovered pixel carries its history forward, falling back to a
  depth-aware upscale only when it has no valid history. Full coverage refreshes every ~4 frames.
- At full resolution every pixel is covered, so this reduces to plain per-pixel accumulation.

## Options

Defaults are registered in the `boolOptions[]` and `intOptions[]` tables in `mod_initialize`. Each
value is read every frame (with a clamp and a scale) into the uniform block, so changes apply live.
Only `quality`, the custom slice/step counts, `halfRes`, `denoisePasses`, `temporal` and
`effectEnabled` change how much GPU work runs; the radius options affect bandwidth through MIP
selection.

| Config key | UI label | Default | Range | Meaning |
| :-- | :-- | :-- | :-- | :-- |
| **Effect** | | | | |
| `effectEnabled` | Enabled | on | | Master switch. Off keeps the mod loaded and idle |
| `intensity` | Intensity | 150 | 0–500 (%) | Final strength |
| `contrast` | Contrast | 150 | 50–300 (%) | Power applied to visibility; >100 deepens the falloff |
| `blackPoint` | Black Point | 1 | 0–30 (%) | Uniform occlusion floor removed before rescaling; cleans open flat surfaces |
| **Occlusion** | | | | |
| `quality` | Quality | 2 (High) | 0–4 | Slices × steps per side: Low 3×2, Medium 5×2, High 7×3, Ultra 9×3, Custom |
| `customSlices` / `customSteps` | Custom Slices / Custom Steps | 7 / 3 | 1–16 / 1–8 | Used when Quality is Custom |
| `radius` | Radius | 200 | 25–800 (‰ of view depth) | Effect radius up close: 200 = 0.2 × view depth |
| `radiusFar` | Far Radius | 800 | 0–800 (‰) | Radius at long range; 0 disables the ramp |
| `radiusRampStart` / `radiusRampEnd` | Far Radius Start / End | 0 / 10000 | world units | View-depth band over which the radius ramps from near to far |
| `radiusMax` | Max Screen Radius | 40 | 10–100 (% of screen height) | Cap on the screen-space radius. Active at defaults beyond ~4,600 units, where it limits Far Radius |
| `thickness` | Thickness | 150 | 25–400 (%) | Assumed occluder thickness (log-scaled) |
| `thickFade` | Thickness Fade Range | 150 | 50–400 (%) | Depth range over which thickness fades, relative to the radius |
| `thickDist` | Distance Thickness | 60 | 0–100 (‰ of radius) | Radius-proportional thickness floor; keeps mid and far occlusion from being starved. 0 disables |
| `depthBias` | Depth Bias | 1 | 0–20 (‰) | Self-occlusion bias toward the camera |
| **Temporal** | | | | |
| `temporal` | Temporal Accumulation | on | | Master switch for accumulation |
| `temporalFrames` | Temporal Frames | 8 | 2–12 | Accumulation length; blend weight = 1/frames |
| `temporalClamp` | Temporal Clamp | 200 | 100–300 (%) | k in mean ± k·σ |
| `motionResponse` | Motion Response | 100 | 0–100 (%) | Accumulation shortening per pixel/frame of motion |
| `motionRange` | Motion Response Range | 5000 | world units | Full motion response up to this depth, none at 2×. 0 = no fade |
| `contentThresh` | Content Response | 100 | 25–300 (%) | Outlier threshold in σ of the 3×3 neighbourhood (100 = reject from ~1σ to 2.5σ) |
| `disoccTol` | Disocclusion Tolerance | 0 | 0–20 (% of own depth) | Below 1.5 acts as 1.5 |
| **Filtering** | | | | |
| `denoisePasses` | Denoise Passes | 1 | 0–3 | Spatial filter passes |
| `denoiseStrength` | Denoise Strength | 60 | 0–100 (%) | Per-pass blend toward the filtered value |
| `halfRes` | Half Resolution | on | | Estimate at half resolution; with temporal on, reconstructed to full res |
| **Distance Fade** | | | | |
| `distanceFade` | Distance Fade | off | | Fade AO out with distance |
| `fadeStart` / `fadeEnd` | Fade Start / Fade End | 15000 / 40000 | world units | Fade band in view depth |
| **Debug** | | | | |
| `debugMode` | Debug View | 0 | 0–8 | See below |
| `debugDepthRange` | Debug Depth Range | 3300 | world units | Gradient scale for the depth views |

World-unit options are absolute distances. The camera's far plane is per stage and far beyond the
visible field, so nothing here is a far-plane fraction; the mod logs the stage's far plane when it
changes, for calibration.

## Debug views

Debug views are drawn at `FRAME_AFTER_HUD`, the last stage, so fog, translucency, bloom and the HUD
cannot paint over them. They replace the whole frame, HUD included, and the normal AO composite is
not drawn while one is selected.

| # | View | Shows | If it looks wrong |
| :-- | :-- | :-- | :-- |
| 1 | AO | The final shaped term in grayscale | |
| 2 | Normals | The authored normal snapshot as `n*0.5+0.5`; black where there is none | |
| 3 | Depth | Prefiltered depth MIP 0, white = near | |
| 4 | Staircase | Raw depth curvature ÷ gradient (R = x, G = y) | |
| 5 | Geo Normal | The face normal derived from depth for the rejection plane; magenta = degenerate, black = sky | Depth chain or position reconstruction is wrong |
| 6 | Normal Agreement | `dot(authored, geometric)`: green > 0.95, yellow 0.8–0.95 (smoothed low-poly curvature), red < 0.8, white < 0, blue = no authored normal, magenta = degenerate | Red or white on **flat ground** with a clean view 5: the normal reaches the mod in the wrong space, sign or frame |
| 7 | Raw AO | `aoNoisy`, unshaped | Wrong here: the occlusion pass. Fine here, wrong in view 1: denoise or temporal |
| 8 | Depth MIP 3 | The coarse level the march samples at distance | Tiles or blocks: the prefilter |

View 7 is the single-frame estimate only with Denoise Passes at 0 or 1. With 2 or 3 passes the
ping-pong overwrites `aoNoisy`, so view 7 shows a partially denoised image (see Known issues).

**For a flicker or artefact report**, ask for screenshots of views 1, 2, 5, 6, 7 and 8 from the
same spot while standing still, plus the `adapter:` and `frame time` log lines. Start with view 7 at
rest: directional streaking means the noise, tiles mean the prefilter, and a clean view 7 with
flicker in view 1 means the temporal pass.

## Log lines

| Line | Meaning |
| :-- | :-- |
| `vbao ready` | Initialised |
| `adapter: ... backend ... vendorID ...` | GPU and backend, logged once |
| `Enhanced AO chain executed OK` | The compute chain ran once |
| `camera far plane: N world units` | The stage's far plane changed |
| `frame time X ms (Y fps): motion response ceiling Z` | The frame-time cap moved by more than 25% |
| a warning naming MSAA or the compatibility renderers | Normals or depth unavailable; VBAO is off. Logged once per session |

## Known issues

- **View 7 is not raw with 2–3 denoise passes** (above). The debug view should read a buffer the
  denoiser never writes.
- **Half-res history after a ±1 pixel resize.** `ensure_targets` keys on the chain size only, so in
  half-res a render size change that leaves the chain size the same (an odd width becoming even,
  for example) keeps full-res history textures of the old size. The composite's size test then
  treats the history as half-res until the next chain resize.
- **No history reset on a camera cut or failed camera read.** Only the per-pixel rejects catch a
  cut. Likewise, if the compute callback skips the chain (a bind group failed to create), the game
  thread has already marked the history valid and flipped it.
- **The MSAA warning tells the player to turn antialiasing off in the video settings**, a setting
  this game build does not have. Harmless today, since MSAA is never on.
- **Thickness uses the uncapped radius.** When Max Screen Radius limits the search radius at long
  range, the thickness and fade range still follow the full Far Radius. Possibly intended; untested.
- **Read-site fallbacks differ from the registered defaults** for `halfRes`, `blackPoint`,
  `depthBias` and `temporalFrames`. The fallback is only used if registration failed, so this is
  cosmetic, but it makes the code misleading to read.
- **Debug views 5 and 6 in half-res + temporal** reconstruct positions from the jittered MIP 0 at
  unjittered coordinates, a sub-texel misregistration against what the occlusion pass uses.
- **No per-object motion vectors.** Moving characters are handled by the two-candidate history, not
  exactly. Real motion vectors would need a renderer change and game-side plumbing.

## Changing things

- **An option** lives in three places: its default in the option table, its read site in
  `on_scene_after_opaque` (fallback, clamp, scale), and its UI control in `build_controls_tab`.
  Keep the read-site clamp and the UI range in step. See `docs/editing-options.md`.
- **A uniform field** means editing `AoUniforms` in `mod.cpp` and the `Uniforms` struct in **all
  five** shaders, and keeping the size `static_assert` true.
- **`ComputePayload` is exactly 128 bytes**, the payload limit, so adding a field to it needs
  something else removed or packed. Resolutions are already packed as `width << 16 | height`.
- **Compute bind-group layouts are automatic** (taken from each pipeline), so a binding that its
  entry point does not use disappears from the layout and bind-group creation fails, skipping the
  chain for that frame. `vbao.wgsl` deliberately has no binding 1 (a removed noise texture).
- Validate shaders with `tools/wgsl_check` before pushing (see `CONTRIBUTING.md`).

## History

### Temporal accumulation in 1.1.0

The 1.0.x builds had reports of AO flickering in motion, mostly on AMD GPUs and some on NVIDIA.
Disabling temporal accumulation removed it. Each change below was confirmed in the field before the
next:

1. **Frame-time cap on the velocity term.** The velocity term reset the whole history on ordinary
   pans and displayed the raw single-frame estimate, whose sampling pattern changes every frame. At
   144 Hz that fuses into mild shimmer; at 30–60 Hz it boils. The cap allows a full reset only above
   250 fps.
2. **In-shader Hilbert noise** replaced a 64×64 noise texture uploaded at init. That upload ran on
   the game thread while the render worker submitted, on a host without Dawn's implicit device
   synchronization: the one path in the chain that could genuinely differ per driver. Not proven to
   be the cause, but correct either way.
3. **σ-normalised outlier test.** A low motion response removed the flicker but left trails behind
   moving occluders. The old content reject compared history against the noisy single-frame sample
   in absolute terms; comparing against the 3×3 mean in units of σ fixed the trails.
4. **Disocclusion tolerance relative to the pixel's depth.** The old floor was 0.002 of the far
   plane, hundreds of world units on TP's per-stage far planes, which let a full-body trail behind
   Link pass as "same surface".
5. **Two-candidate history.** A soft trail behind Link remained because camera reprojection maps a
   pixel on a screen-static character to a neighbouring part of his body. The stored normal plus the
   static candidate fixed it.
6. **Depth-faded motion response, default 100%.** Full response made characters crisp but made
   distant AO sparse in motion. Fading the response with view depth gives both.

Ruled out along the way, by reading the pinned source (kept so it need not be re-derived):

| Ruled out | Why |
| :-- | :-- |
| Depth snapshot precision | `Depth32Float` copied to `R32Float` on every backend |
| Normal attachment path | `RGB10A2Unorm`, no blend state on that target, write mask tied to depth writes, copied 1:1 |
| Buffer size mismatch | Colour, depth and normal buffers are all created at the same size |
| Viewport inset | The scene viewport covers the full target |
| Camera identity under frame interpolation | The stage hook receives the same view object the interpolation rewrites |
| Projection convention | The camera service's reversed-Z terms match aurora's conversion |
| Uniform staging | Per-frame staging buffers; aurora already handles AMD's uniform offset alignment |
| Vendor-dependent shader behaviour | Masked shifts, clamped loads, no workgroup-memory race, no wave ops, NaN-safe guards |

Whether the original report was partly a driver issue cannot be settled from source. The remaining
per-driver variables are Dawn's barriers between storage-texture dispatches and frame pacing (AMD's
Vulkan driver has no Mailbox present mode), and the latter is what the frame-time cap addresses.

### Defaults

The look was first tuned on the pre-mod-API forks: High quality, intensity and contrast 150,
thickness 150. In-game tuning since then moved the radius to 200 near / 800 far, denoise strength
to 60%, temporal frames to 8, black point to 1, depth bias to 1, and made Half Resolution the
default once the jittered temporal upsampler made it look close to full resolution.

### Origin

VBAO was ported from the maintainer's earlier implementation in the retired `dusklight-ao` and
`aurora-ao` forks onto upstream's `ao_mod` demo framework.

The code is this project's own. The store description in `mod.json` credits iMMERSE's MXAO ReShade
filter as an inspiration; that is the **only** place MXAO is mentioned, by design. Do not reference
MXAO in code, comments or other docs: an unqualified mention invites mistaken claims that its code
was copied, which it was not.

### Experiments not merged

Branch `claude/vbao-amd-flickering-xbualq` contains a "1.1.1 Normal Repair" experiment (distrusting
authored normals that contradict the depth-derived geometry). It was a failed experiment and is
intentionally not merged; do not merge it.
