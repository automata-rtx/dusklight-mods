# SMAA — Subpixel Morphological Antialiasing

Mod id `dev.automata.smaa` (directory `mods/smaa/`). Service-only (no game code, no hooks): stages +
snapshots from the gfx service, config/ui/resource/log, plus the **optional** Depth to Normal service.
Spatial SMAA 1x — no camera jitter, no motion vectors, no temporal component (the mod API can't
inject a jittered projection or expose a velocity buffer, so the temporal SMAA variants aren't
reachable service-only; see "Scope / why 1x").

**Uses the Depth to Normal mod** (`dev.automata.depth_to_normal`, exported by Graphics Hub) as a
geometric edge input. When the provider is present, edge detection unions the luma detector with a
normal-angle + relative-depth discontinuity test, so silhouettes and creases are caught even where
two flat-shaded TP surfaces have almost no brightness contrast. It's an *optional* import: absent the
provider (or with "Geometric Edges" off), the mod does luma-only SMAA and still loads/runs. This is
the "geometry-guided AA" entry from `docs/depth_to_normal_consumers.md`.

## Where it runs, and why

`GFX_STAGE_SCENE_AFTER_OPAQUE` — **before** the game's bloom / translucency / DOF / post (those draw
between `SCENE_AFTER_OPAQUE` and `FRAME_BEFORE_HUD`; see `docs/mod-api-notes.md`). So the game's post
effects operate on antialiased geometry rather than re-processing our blended edges. This is the
right layer for TP specifically:

- TP is **LDR throughout** (GameCube-era pipeline; no HDR tonemap stage), so edge detection on the
  opaque scene colour is already in the perceptual/gamma space SMAA's luma thresholds expect — there
  is no "AA must run after tonemap" constraint to push us later.
- The **alpha-test foliage** (TP's worst aliaser) is drawn in the opaque pass, so it's present here.
- The **reconstructed normal + depth** are derived from the opaque scene depth, so they're freshest
  and most meaningful at this stage.

The colour input is the frame's **resolved scene snapshot** (`resolve_pass`, a copy), while the final
blend writes the **live** target — reading a copy and writing the original is hazard-free.

Trade-off of this placement: translucent/particle edges (drawn later) and the HUD are not
antialiased. Both are intended — alpha-blended edges are already soft, and you never want to AA the
HUD.

## Pipeline (per frame, at `GFX_STAGE_SCENE_AFTER_OPAQUE`)

`resolve_pass` snapshots scene colour (single-sample, in `color_format`). One `push_compute` runs two
compute passes, then one `push_draw` composites. EdgesTex and BlendTex are mod-owned `rgba8unorm`
(storage + sampled), recreated on resize and retired for a few frames so in-flight payloads never
reference a freed view.

1. **`edge_detection.wgsl`** (compute, 8×8) — for every pixel:
   - **Luma edges**: the reference SMAA luma detector (Jimenez et al., MIT) with local-contrast
     adaptation (suppresses an edge when a much stronger parallel gradient sits next to it — kills
     doubled edges inside high-contrast texture). Catches shading / texture / alpha-test edges.
   - **Geometric edges** (when the provider is present): angular difference of the reconstructed
     **world normal** (`1 - dot(n_c, n_neighbor)`) unioned with a **relative raw-depth** discontinuity
     (`|Δd| / max(d, ε)`, robust across reversed-Z and to sky = 0). The normal angle catches
     silhouettes *and* creases (continuous depth, flipping normal); the depth test catches silhouettes
     where two near-parallel surfaces sit at different depths.
   - Output: `EdgesTex.rg` (`.r` = edge on the pixel's left boundary, `.g` = top boundary). The pass
     also writes `0` to `BlendTex` at the same pixel — folding the blend-target clear into edge
     detection (the CMAA2 / iMMERSE trick), so pass 2 can write only the sparse edge pixels.

2. **`blend_weights.wgsl`** (compute, 16×16) — **the CMAA2-compacted pass**, the expensive one:
   - **Compaction**: each thread checks its pixel's edges; edge pixels `atomicAdd` their local index
     into a `groupshared` list (256 slots). After a `workgroupBarrier`, the first `count` threads pull
     from that packed list and do the search — so the sparse, thin edges run in **fully-occupied
     warps** instead of one-edge-pixel-per-warp. Non-edge pixels return right after the scan. This is
     Intel's CMAA2 deferred-processing idea (2018), applied to SMAA's dominant pass.
   - **Search + coverage** (per edge pixel): walk the collinear run of edge pixels left/right (top
     edge) or up/down (left edge) until it ends, capped at `maxSearchSteps`; detect the silhouette
     turn direction at each end from the perpendicular edges; reconstruct the aliased silhouette as a
     straight line over the run and take its signed height at the pixel centre as the coverage. No LUT
     assets — the search is **linear** (no SearchTex) and the coverage is **analytic** (no AreaTex).
   - Output `BlendTex.rgba`, packed as: `.r` this pixel pulls from above · `.g` the above pixel pulls
     from this one · `.b` pulls from left · `.a` the left pixel pulls from this one. Scaled by
     `blendStrength`.

3. **`neighborhood_blend.wgsl`** (fullscreen draw into the live target) — gather the four boundary
   weights that touch this pixel (its own top/left edge, plus the reciprocal weights from the pixel
   below/right), pick the dominant axis, and pull in the neighbour colour by a **sub-pixel bilinear
   offset** (weight ≤ 0.5 px → up to 50 % of the neighbour, exactly SMAA's mechanism). Non-edge pixels
   **discard**, leaving the live target untouched, so only edges are rewritten. Debug views 1 (edge
   mask) / 2 (blend weights) short-circuit here.

### Why two separate compute passes

Pass 1 clears `BlendTex` (all pixels) and pass 2 overwrites the edge pixels; they run as two
`BeginComputePass`/`End` blocks so the writes from pass 1 are ordered before pass 2's reads of
`EdgesTex` and overwrites of `BlendTex`. (Same reason VBAO's chain is ordered — here the write-after-
write on `BlendTex` makes the pass boundary load-bearing, not just cosmetic.)

## Tunables (config vars; UI shows them in sections)

Ints are fixed-point as noted. Every value rides one per-frame uniform block, so changing any of them
live has no rebuild or pipeline cost.

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle |
| `blendStrength` | 100 | overall edge-blend strength, ×0.01 (0–150). Lower keeps edges crisper; higher smooths harder (and softens slightly) |
| `edgeThreshold` | 10 | luma edge threshold, ×0.01 (0.05–0.20). Lower catches more edges (softer, can blur texture); higher is more selective |
| `localContrast` | 200 | local-contrast adaptation factor, ×0.01 (SMAA default 2.0). Suppresses an edge dwarfed by a parallel neighbour gradient |
| `useNormalEdges` | on | union the geometric (normal/depth) detector with luma. No effect if the Depth to Normal provider is absent |
| `normalThreshold` | 10 | geometric edge: `1 - dot(normals)` threshold, ×0.01 (~0.10 ≈ a shallow crease). Lower catches subtler creases |
| `depthThreshold` | 20 | geometric edge: relative depth discontinuity, ‰ (×0.001 → 0.02). Lower catches more distant silhouettes |
| `maxSearchSteps` | 16 | pattern search reach in pixels (4–32). Higher smooths longer near-horizontal/vertical edges, costs more per edge pixel |
| `debugMode` | 0 | 0 off, 1 edges (red = vertical, green = horizontal), 2 weights (warm = vertical blend, cool = horizontal) |

Debug views currently draw at `SCENE_AFTER_OPAQUE` (same as the composite), so later effects (fog,
bloom, translucency) can paint over them — fine for judging edge detection, but if that becomes a
nuisance we can stage them to `FRAME_BEFORE_HUD` like VBAO does.

## Scope / why 1x

Only **spatial SMAA 1x** is reachable service-only. The temporal/subpixel variants (T2x/S2x/4x) need
two things the service surface can't provide: a **jittered camera projection** (the camera service is
a read-only snapshot; we can't offset the game's render matrices) and **motion vectors** (no velocity
buffer is exposed, and depth reprojection only handles camera motion, not animated foliage/characters).
Getting those would mean a game-linked mod with hooks — throwing away the durability that keeps this
off the ABI treadmill. So: 1x smooths static/near-static edges well; it does not stabilise temporal
shimmer on moving foliage (that's the subpixel case 1x is weakest at, and TP's worst aliasing).

**This version handles orthogonal patterns only.** Diagonal-specific search and corner rounding
(SMAA's most intricate extras) are deferred — the neighborhood pass still softens diagonals via the
orthogonal weights, just less precisely at ~45°. The coverage is a from-first-principles trapezoidal
reconstruction, chosen over reproducing iryoku's precomputed AreaTex so the whole thing is
correct-by-construction and asset-free; it is in the SMAA family but not bit-identical to reference
SMAA, and the exact coverage magnitude is expected to want in-game tuning.

## Performance notes

The CMAA2 compaction targets the blend-weight pass, which dominates SMAA's cost (per-edge-pixel
searches); edge detection and neighborhood blending are cheap full-screen passes. Packing sparse edges
into full warps is where the win is — most relevant here because this effect stacks on
VBAO/SSILVB/shadows, so shaving the dominant pass returns real budget. There's a deliberately-omitted
CMAA2 micro-optimization (skipping workgroups with `< 4` edge pixels) that trades a sliver of quality
on ultra-sparse edges for speed; left out so v1 never drops AA on isolated edges — a candidate if
profiling asks for it.

If `GfxDeviceInfo.sample_count > 1` (the scene pass already runs MSAA), SMAA is partly redundant on
silhouettes; TP's forward port is expected to be single-sample, which is exactly why post-process AA
is worth having.

## Provenance / licensing

The SMAA algorithm (edge detection, orthogonal search, neighborhood blending) is **reimplemented from
the MIT reference** (iryoku/smaa, Jimenez et al.). The compute compaction is **reimplemented from
Intel's public CMAA2 description** (2018). Pascal Gilcher's proprietary iMMERSE SMAA ("All rights
reserved") was studied only to confirm the combination of these two public techniques works well —
**no code from it was copied**. Techniques aren't copyrightable; the specific proprietary source is,
and was not used.

## Status

First working version, CI-green on all seven platforms (host code compiles + packages). CI does **not**
validate WGSL or the visual result — shaders are validated by the game at pipeline-creation time, so
shader compilation and visual correctness (especially the coverage sign/magnitude) are confirmed
in-game. Iterate via screenshots + taste feedback per the working model in `docs/ssilvb_plan.md` §0.
Next candidates once the orthogonal base is confirmed: diagonal search, corner rounding, and (if
wanted) staging debug views to `FRAME_BEFORE_HUD`.
