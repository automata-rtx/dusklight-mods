# SMAA

Subpixel morphological antialiasing (SMAA 1x) as a post-process.

| | |
| :-- | :-- |
| Mod id | `dev.automata.smaa` (`mods/smaa/`) |
| Version | see `mods/smaa/mod.json` (1.0.0 at the time of writing) |
| Kind | Service-only: graphics, config, UI, resource and log services. No game code, no hooks, no dependency on other mods |
| Runs at | `GFX_STAGE_SCENE_AFTER_OPAQUE` |
| Passes | edge detection (compute) → blend weights (compute) → neighborhood blend (draw) |

It is spatial only: no camera jitter, no motion vectors, no temporal component (see
[Scope](#scope-and-differences-from-reference-smaa)). Edge detection combines the reference SMAA
luma detector with **geometric edges** taken from the game's own surface normals and depth, which
catch silhouettes and creases where two surfaces have little brightness contrast. That matters for
TP's flat, low-contrast art.

## Files

| File | Contents |
| :-- | :-- |
| `src/mod.cpp` | Options and UI, per-frame stage hook (`on_scene_after_opaque`), compute callback (`on_compute`), draw callback (`on_draw`), texture management (`ensure_targets`), the lazily built blend pipeline (`ensure_neighborhood_pipeline`) |
| `res/edge_detection.wgsl` | Pass 1 |
| `res/blend_weights.wgsl` | Pass 2 |
| `res/neighborhood_blend.wgsl` | Pass 3, and both debug views |
| `res/licenses/ATTRIBUTION.txt` | Provenance (shipped in the bundle) |
| `../../common/gfx_scene_pass.h`, `gfx_normal_compat.h` | Shared helpers: scene-pass layout, normal access |

`res/SMAA Logo.png` is packaged but not wired up as an icon (`mod.json` has no `icon` key and the
loader's default name is `res/icon.png`).

## Where it runs, and why

SMAA runs at `SCENE_AFTER_OPAQUE`: after all opaque world geometry, **before** the game's
translucent geometry, particles, depth of field and bloom. So the game's own post effects work on
an already antialiased image.

- TP renders in LDR throughout, so the colour at this point is already in the gamma-encoded space
  SMAA's luma thresholds expect. There is no tonemap later that AA would have to follow.
- Alpha-tested foliage, TP's worst aliasing, is drawn in the opaque lists, so it is present here.
- Translucent edges and the HUD are drawn later and are not antialiased, which is intended:
  alpha-blended edges are already soft, and the HUD should not be blurred. (A few particles, drawn
  inside the opaque phase, are in the input and do get edge-detected.)

VBAO also composites at this stage. Hooks on one stage run in mod load order, and neither mod
imports the other, so their order is not fixed. If VBAO runs first, its AO is in SMAA's colour
input; if SMAA runs first, AO multiplies over antialiased edges. Either looks fine in practice.

## How it works

### Per frame (game thread, `on_scene_after_opaque`)

1. Return early if disabled.
2. `resolve_pass` snapshots the scene: colour (RGBA8/BGRA8 unorm), depth (`R32Float`, reversed-Z)
   and the authored normal (`RGB10A2Unorm`, view space, encoded `n*0.5+0.5`, alpha 1 = valid
   normal, 0 = none). All three are requested every frame.
3. `ensure_targets` (re)creates the two mod-owned textures on resize. Old ones are kept for a few
   frames before release, because work already queued may still reference them.
4. Decide whether geometric edges are available (normal **and** depth present, option on).
5. Fill one 64-byte uniform block shared by all three passes (`push_uniform`).
6. `push_compute` (passes 1 and 2), then `push_draw` (pass 3).

`resolve_pass` and `push_compute` each split the game's scene render pass, which is what orders the
compute work before the draw.

### Passes

| # | Pass | Shader / entry | Work size | Reads | Writes |
| :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | Edge detection (compute) | `edge_detection.wgsl` / `edge_detection` | 8×8 workgroups | colour, normal, depth | **EdgesTex**; clears **BlendTex** to 0 |
| 2 | Blend weights (compute) | `blend_weights.wgsl` / `blend_weights` | 16×16 workgroups | EdgesTex | BlendTex at edge pixels only |
| 3 | Neighborhood blend (draw, fullscreen triangle into the live scene target) | `neighborhood_blend.wgsl` / `vs_main`, `fs_main` | per pixel | colour snapshot, BlendTex, EdgesTex | scene colour; non-edge pixels `discard` |

Pass 3 reads the colour *snapshot* and writes the *live* target, so there is no read/write hazard.

**Mod-owned textures** (both `rgba8unorm`, storage + sampled, render-target size):

| Texture | Channels |
| :-- | :-- |
| EdgesTex | `.r` = edge on this pixel's left boundary (a vertical edge), `.g` = edge on its top boundary (a horizontal edge). Values 0/1. `.ba` unused (`rg8unorm` cannot be a storage texture in core WebGPU) |
| BlendTex | `.r` = this pixel pulls from the pixel above; `.g` = the pixel above pulls from this one; `.b` = pulls from the left; `.a` = the left pixel pulls from this one |

**Pass 1, edge detection.** Per pixel, against its left and top neighbours:

- *Luma edges*: BT.709 luma, edge if the difference is ≥ `edgeThreshold`. Local-contrast adaptation
  as in reference SMAA: an edge is dropped when a neighbouring edge is more than `localContrast`
  times stronger, which removes doubled edges inside high-contrast texture.
- *Geometric edges* (when available): an edge if `1 - dot(n0, n1)` ≥ `normalThreshold`, or the
  relative depth step `|d0 - d1| / max(d0, d1)` ≥ `depthThreshold`. The normal test runs only where
  both pixels have a valid normal. Sky (depth 0) next to geometry is always an edge.
- The result is the union (max) of the two. This pass also clears BlendTex, so pass 2 only has to
  write edge pixels.

**Pass 2, blend weights.** The expensive pass, optimised with CMAA2-style compaction: each 16×16
workgroup first collects its edge pixels into a workgroup-shared list (atomic counter), then the
first `count` threads process that list. Sparse edges therefore run in fully occupied waves instead
of one useful thread per wave. Per edge pixel:

- Walk the run of edge pixels along the edge in both directions, up to `maxSearchSteps` each way.
- At each end, look one row (or column) over, just past the end, for the same kind of edge: that
  decides whether the silhouette steps up, down or not at all at that end.
- Treat the run as a straight line between the two ends and take its height at the pixel centre as
  the coverage (at most 0.5). Scale by `blendStrength` and store it in BlendTex.

No lookup textures: the search is a plain linear walk (no SearchTex) and the coverage is computed
analytically (no AreaTex).

**Pass 3, neighborhood blend.** Gathers the four weights touching the pixel (its own top/left
weights plus the reciprocal ones stored by the pixels below and to the right), picks the dominant
axis, and blends toward that neighbour with a bilinear tap offset by the weight. This step matches
reference SMAA.

### Pipelines and resources

- The two compute pipelines are built in `mod_initialize`. They use automatic bind-group layout, so
  every binding a shader declares must stay referenced by its entry point or bind-group creation
  fails.
- The neighborhood (draw) pipeline is built lazily on the render worker by
  `ensure_neighborhood_pipeline`, from the live `GfxDrawContext::layout`, and rebuilt when
  `layout.key` changes. The scene pass gains a normal attachment at runtime, and a pipeline built
  for the old shape would be silently rejected. See `CONTRIBUTING.md`.
- When geometric edges are off, the normal and depth bindings receive the colour snapshot as a
  stand-in; the shader does not read them in that case.
- Everything the mod creates is released in `mod_shutdown`.

## Geometric edges and normals

The normal is the artist's **authored** vertex normal, written by the renderer into a second
colour attachment. It is smooth across curved surfaces, not flat per triangle, so facet boundaries
on low-poly geometry do not register as edges. That is why `normalThreshold` can default to 5%
(about 18°). It is in view space, which does not matter here: the test compares two normals in the
same space.

Geometric edges switch themselves off, leaving luma-only SMAA (the reference behaviour), when:

- **the normal snapshot has not started yet.** The first request turns the normal attachment on for
  the *next* frame and returns nothing for this one. SMAA runs luma-only for those frames and picks
  the normals up by itself. It waits `kNormalLatchGraceFrames` (8) before logging anything, so a
  normal start-up is silent;
- **the renderer cannot provide normals**, as on the compatibility renderers (D3D11, OpenGL ES);
- **the depth snapshot is unavailable** (the renderer omits depth from the resolve if it could not
  create its depth-copy pipeline). The log line in this case still says "no scene normals";
- **MSAA is on.** The renderer only creates the normal attachment without MSAA. The current game
  build (Dusklight `v2.0.0`) never enables MSAA, so this cannot happen today; the code path exists
  for builds that do.

When one of these persists, SMAA logs one INFO line saying geometric edge detection is off and why.

Unticking **Geometric Edges** stops the geometric test but not the normal and depth snapshots, which
are still requested every frame.

## Options

Registered in `mod_initialize` (`intOptions[]` table plus two `register_bool_option` calls). All are
read every frame into the uniform block, so changes apply live. Integer options are scaled on read.

| Config key | UI label | Default | Range | Meaning |
| :-- | :-- | :-- | :-- | :-- |
| `effectEnabled` | Enabled | on | | Master switch for the stage |
| `blendStrength` | Blend Strength | 100 | 0–150 (×0.01) | Scales every blend weight (each is at most 0.5 px at 100%, 0.75 px at 150%). Lower keeps edges crisper, higher smooths harder |
| `edgeThreshold` | Luma Threshold | 20 | 5–20 (×0.01) | Minimum luma step that counts as an edge. Lower catches more edges and can blur texture detail. The default is the top of the range and double the SMAA reference (10), which treated ordinary texture detail in TP's flat art as edges; geometric edges cover the silhouettes a lower threshold was catching |
| `localContrast` | Local Contrast | 200 | 100–400 (×0.01) | Drop an edge when a neighbouring edge is more than this many times stronger. Higher suppresses less. Reference SMAA uses 2.0 |
| `useNormalEdges` | Geometric Edges | on | | Add the normal/depth detector to the luma one |
| `normalThreshold` | Normal Threshold | 5 | 2–50 (×0.01) | Edge if `1 - dot(n0, n1)` is at least this. 2% ≈ 11.5°, 5% ≈ 18°, 10% ≈ 26° |
| `depthThreshold` | Depth Threshold | 20 | 1–200 (×0.001) | Edge if the relative raw-depth step is at least this. Lower catches more distant silhouettes |
| `maxSearchSteps` | Max Search Steps | 16 | 4–32 | Pixels searched in each direction along an edge. Higher handles longer near-horizontal/vertical edges and costs more per edge pixel |
| `debugMode` | Debug View | 0 | 0–2 | See below |

## Debug views

Drawn by pass 3 at `SCENE_AFTER_OPAQUE`, opaque and full-screen. Translucents, bloom, fog and the
HUD still draw over them.

| `debugMode` | View | Red | Green |
| :-- | :-- | :-- | :-- |
| 1 | Edges | vertical edge (left boundary) | horizontal edge (top boundary) |
| 2 | Weights | vertical blending (from top edges, `r + g`) | horizontal blending (from left edges, `b + a`) |

Yellow means both. The Weights view shows the pixel's own BlendTex texel, which is at most 0.5 per
channel at 100% strength, so it looks dim.

## Scope and differences from reference SMAA

- **Spatial 1x only.** The temporal and subpixel variants (T2x, S2x, 4x) need a jittered projection
  matrix and motion vectors. The camera service cannot offset the projection matrix and there is no
  velocity buffer, so they are out of reach for a service-only mod. 1x smooths static edges well;
  it does not stabilise shimmer on moving foliage.
- **Orthogonal patterns only.** Diagonal search and corner rounding are not implemented. Shallow
  edges (long horizontal or vertical runs) are handled well, but edges at or near 45° get little or
  no blending: a staircase of 1-pixel runs with opposite end steps yields a zero weight. The
  `corner_rounding` uniform field is reserved and always 0.
- **Coverage is analytic, not table-driven.** It is in the SMAA family but not bit-identical to the
  reference. In particular:
  - end detection looks for the same kind of edge one row/column over, where reference SMAA tests
    the crossing edges;
  - L-shapes ramp across the whole run, where the reference ramps across the half nearest the
    corner;
  - U-shapes (both ends turning the same way) get a flat weight across the run, where the reference
    uses a tent that falls to 0 in the middle.

  Both of the last two blur somewhat more than the reference.
- **Not done:** CMAA2's optional skip of workgroups with fewer than 4 edge pixels. It trades a little
  quality on isolated edges for speed, and is a candidate if profiling asks for it.

## Troubleshooting

Log lines (the game console, prefixed with the mod id):

| Line | Meaning |
| :-- | :-- |
| `smaa ready` | Initialised |
| `SMAA chain executed OK` | The compute passes ran once. It does not prove the final draw happened |
| `scene colour snapshot unavailable; SMAA disabled` | `resolve_pass` returned no colour. Logged once; the stage keeps retrying every frame |
| `geometric edge detection off: ...` | Luma-only from now on; the text names MSAA or "no scene normals" |

- The render worker cannot log. If the neighborhood pipeline fails to build, the draw is skipped
  silently. A WGSL error usually shows up as a device error from the host rather than a mod log
  line, so run `tools/wgsl_check` after any shader edit (see `CONTRIBUTING.md`).
- Use the Edges view first: missing edges are a pass 1 problem; edges present but no visible
  smoothing points at pass 2 or 3.

## Known issues

- Edges at or near 45° get little or no smoothing (see Scope above).
- Unticking **Geometric Edges** still requests the normal and depth snapshots every frame, so it
  saves shader work but not the copies.
- The `no scene normals` log line is also used when only the depth snapshot is missing.
- The `SMAA disabled` warning is logged once, but the stage keeps retrying and resumes on its own.
- `DrawPayload::debug_view` is written but never read; the shader uses the uniform's copy.
- At the screen border the edge search keeps counting the clamped border pixel, so a run that
  reaches the border is treated as continuing up to `maxSearchSteps`. Minor.

## Changing things

- New uniform field: update `SmaaUniforms` in `mod.cpp` **and** the `Uniforms` struct in all three
  shaders, and keep the size `static_assert` (64 bytes, a multiple of 16) true.
- Payloads (`ComputePayload`, `DrawPayload`) must stay trivially copyable and at most 128 bytes.
- Compute bindings must stay statically referenced (automatic layout, see above).
- Defaults live in `intOptions[]` and the two `register_bool_option` calls; the read site's fallback
  should match. See `docs/editing-options.md`.

## Provenance

The SMAA algorithm (edge detection, orthogonal search, neighborhood blending) is reimplemented in
WGSL from the published paper and the MIT reference (iryoku/smaa, Jimenez et al.). The compute
compaction in pass 2 is reimplemented from Intel's public CMAA2 description (Strugar, 2018). Pascal Gilcher's iMMERSE SMAA, which is proprietary, was read only to confirm the
combination of the two public techniques is sound; no code from it is used. See
`mods/smaa/res/licenses/ATTRIBUTION.txt`.

## History

- 1.0.0 is the first release. Before the 1.0.0 version reset, the mod had reached 1.1.0 internally.
- `edgeThreshold` defaults to 20 rather than the reference 10 after in-game feedback.
- `normalThreshold` was 10 while normals were reconstructed from depth (every triangle facet
  registered as a crease). Authored normals made 5 safe.
