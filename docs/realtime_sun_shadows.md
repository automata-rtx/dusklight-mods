# Realtime Sun Shadows

Mod id `dev.automata.realtime_sun_shadows`. Game-linked: includes game headers, hooks game
functions, and (on Windows) links the platform release's `windows-amd64.lib` import library.

**Depends on the Depth to Normal mod** (`dev.automata.depth_to_normal`) as of 1.7.0. Shadows no
longer reconstructs its own receiver normals — it imports the shared world-space normal from that
provider and only applies its own bilateral smoothing (the one normal treatment unique to the
shadow bias). This is a hard dependency (a required service import): the loader disables Realtime
Sun Shadows if Depth to Normal is not installed and enabled. Install both together.

## Architecture (based on the upstream `shadow_mod` demo)

1. **Light cameras** (`build_light_camera` / `build_light_camera_core`): sun/moon world
   position from `sun_moon_offset(daytime)`; if the sun is below the horizon, use
   daytime+180° (the moon). Horizon fade `clamp((dirY-0.05)/0.15)` softens shadows at
   dawn/dusk. One ortho box **per cascade**: nested world boxes centered near the player
   with camera-forward lookahead (radii = Coverage × the split percentages, near → far),
   each snapped to whole texels of its own map (kills crawling); near/far extents scale
   with each radius. The optional **Link cascade** is a small box (`linkCoverage`) snapped
   to the player's position with a deliberately short light distance for maximum depth
   discrimination. When it is enabled, Link's models are **excluded from the world cascades**
   rather than drawn into both — see `linkCascade` in the settings table for why.
2. **Caster capture** (`replay_cascade`, once per cascade at `SCENE_AFTER_TERRAIN`): replay
   the game's own opaque draw lists (`dComIfGd_drawOpaList*`) into a `create_pass` offscreen
   pass with `GXSetProjectionFull(lightReplayProjection)` + `j3dSys.setViewMtx` — the game
   re-renders the world from the light, animation included. During replay, hooks bypass
   `J3DUClipper::clip` (sphere + box overloads) so casters outside the *camera* frustum
   still render, and skip `GXCopyTex`. `resolve_pass` depth = that cascade's shadow map
   (reversed light depth: 1 = near light). Each cascade is a full save-replay-resolve
   bracket, so cost scales with cascade count. The replay is **depth-only**: color writes are
   disabled (`GXSetColorUpdate(GX_FALSE)`) and the color target is not resolved — a shadow map
   needs only depth, and the resolved color is read solely by the Camera Replay debug view, so
   normal frames skip the per-pixel color ROP and a full `mapSize²` color resolve per cascade
   (the largest single cost in the map render; it scales with map size and cascade count).
   Alpha *test* still runs, so alpha-cut foliage keeps punching holes in the depth map. The Link cascade replays only the lists the
   player's models enter (Middle/Opa/Dark) with a position filter in the
   `J3DShape::drawFast` pre-hook: `J3DShapePacket::prepareDraw` sets `j3dSys`'s current
   model right before every drawFast, so the filter reads `j3dSys.getModel()` and skips
   models whose base translation is beyond 2× `linkCoverage` from the player — Link's body
   and equipment all anchor at his position, world geometry anchors far away or at the
   origin. No private `daAlink_c` fields are touched.

   **Culling is two-level (1.8.0).** All the replay culls (light column, sub-texel casters,
   the Link filter) decide first at the **mat-packet** level — a `J3DMatPacket::draw`
   pre-hook walks the packet's shape-packet chain (each entry carries its own `mpModel`, so
   no `j3dSys` state is touched) and skips the whole packet when every entry is culled,
   which skips the material load + display-list streaming that a drawFast-level skip still
   pays. A packet with any surviving entry draws, and the `J3DShape::drawFast` hook culls
   the remaining entries individually (mat packets can hold shape packets merged from
   several models — `J3DDrawBuffer::entryMatSort` merges same-material packets). Since the
   near/mid cascades cull almost all world geometry, packet-level decisions make those
   replays cheap list-walks instead of full material-decode passes — the largest CPU
   reduction in the map render. Both hooks resolve through the symbol manifest and degrade
   gracefully (packet-level culling off, per-shape culling still on) if `J3DMatPacket::draw`
   can't be hooked.

   **Staggered cascade updates (`cascadeStagger`, 1.8.0, default on).** The near cascade
   re-renders every frame (it carries the player and everything close); the middle cascade
   re-renders every other frame, and since 1.9.0 the outermost re-renders every **fourth**
   frame when its radius is ≥8000 (else every other), with phases that never collide — no
   frame runs more than two world replays, and most run fewer. A skipped cascade composites
   from a mod-owned copy of its last rendered map: on render frames a tiny compute pass
   (`res/shadow_copy.wgsl`) copies the frame-pooled depth resolve into a persistent R32Float
   texture (frame-pooled views must never be cached across frames), and the composite binds
   that copy with the stored `light_vp`/`texel_world`/near/far metadata so receiver
   projection stays consistent with the cached content. `cascade_cache_usable` force-refreshes
   on anything that would make a stale map wrong: >0.5° sun/moon jumps (sleeping, warps),
   box-center drift over 5% of the cascade radius — the center leads the camera by the
   forward lookahead, so this catches snap camera turns as well as teleports — map-size /
   radius config changes, or any gap in the map pass (menus, indoors, loads). Staleness is
   otherwise invisible in normal play: the affected cascades hold distant, mostly static
   geometry, and a lagged box edge lands inside the cascade cross-fade band. Debug views
   disable staggering so every visualization stays live.

   **Outermost-cascade interior exclusion (1.9.0, part of `cascadeCull`).** The composite
   picks the FIRST cascade containing the receiver, so the outermost map is only ever
   sampled for receivers in the inner cascade's outer blend band (`fit > 1 - blend_frac`) or
   beyond its box — and a caster occludes only receivers at its own lateral light-space
   position. Therefore casters whose footprint lies entirely inside the inner box's
   guaranteed-sampled interior can be culled from the outermost replay wholesale: they can
   only shadow receivers the inner map already covers. The exclusion square's half-extent is
   `innerRadius × (1 − blend_frac)` minus the PCF + normal-offset sampling reach (8 outer + 4
   inner texels), texel snapping, and a drift allowance (`min(1000, 10% of innerRadius)`)
   for the inner box's movement over a cached outer map's lifetime; a cached outer map
   records the exclusion it was rendered with and is invalidated if the inner box drifts
   past that allowance or settings would shrink the hole. This removes the densest central
   geometry (everything near the camera) from the most expensive replay. Disabled in debug
   modes so map visualizations stay complete.
3. **Composite** (`SCENE_AFTER_OPAQUE`, `res/shadow.wgsl`): unproject scene depth to world,
   reconstruct a world normal from depth (side-selected crosses), pick the sharpest cascade
   whose box contains the receiver, apply that cascade's acne bias and normal-offset receiver
   (`world + n * normal_offset * texel_world[cascade]`), PCF over bilinearly-weighted
   comparison taps (kernel = base + Far Softening × cascade index). **Acne bias — two modes:**
   with `receiverPlaneBias` on (default), the **geometric face normal** (`geometric_normal_at`,
   from the depth buffer — see the two-normals note below) spans a tangent plane whose depth
   gradient `d(depth)/d(uv)` in the
   ortho light space is solved directly (`receiver_plane_bias_uv_from_normal`, Isidoro 2006); each
   PCF tap then compares against `receiver + base_bias + dot(grad, tapOffset)`, so the comparison
   plane follows the surface under the footprint instead of paying a flat margin that detaches the
   shadow. `base_bias = bias + clamped fractional-sampling error`; the gradient is clamped to
   `rpdb_max` per texel. Off = the classic `bias_eff = bias + slope_bias * tan_t` (tan clamped
   at 4) flat margin. **Both** modes scale the slope/plane term by a light-facing gate
   `smoothstep(-0.1, 0.05, n·L)`: a surface facing away from the light is shadowed by the
   two-sided map's front-most face (occlusion, not self-shadow) so it needs no slope bias, and the
   old code instead MAXED it there (the `cos_t` 0.05 floor drove `tan_t` to its cap), lifting the
   comparison until thin low-poly geometry leaked back into light as faceted holes. The constant
   `bias` and the normal-offset receiver still apply on both faces. Biases normalized per cascade
   against its own light range. Each PCF tap fetches its
   2×2 depth neighborhood with a single `textureGather` (a mod-owned non-filtering clamp
   sampler, created via raw wgpu since the maps are R32Float and the gfx service exposes no
   sampler API) instead of four `textureLoad`s — a quarter of the texture fetches for the
   same bilinear-of-comparisons result (GPU-verified bit-identical). A true hardware
   comparison sampler would also move the compare into fixed-function, but that needs a
   `texture_depth_2d`-bindable depth view the gfx service does not currently hand back
   (`resolve_pass` depth is R32Float). Inside the outer
   `blend_frac` band of a cascade's box the result cross-fades to the next cascade so the
   resolution step never shows as a line. The Link cascade is evaluated separately and
   combined with `max()` — its map contains only the player, so it can only add occlusion,
   never remove world shadows. Then combine with the screen-space shadow term (below) and
   darken the scene. **Colored shadows (`shadowTint`, 1.11.0):** the darkening multiply is
   bent toward the current skylight color (`dScnKy_env_light_c::vrbox_sky_col`, read straight
   off env light since this mod is game-linked, peak-normalized so it only shifts hue and
   never brightens under the multiply blend) in proportion to occlusion, so shadows read as
   skylit — cool by day, warm at dusk — instead of neutral gray. It follows area / time /
   weather for free; SSILVB reconstructs the same sky value by reducing the rendered skybox
   pixels because it is service-only, so the two stay visually coherent. `0` restores the
   neutral multiply. The composite runs right after the opaque scene — before translucency
   and, critically, before the game's bloom filter (`m_Do_graphic.cpp` draws bloom between
   `SCENE_AFTER_OPAQUE` and `FRAME_BEFORE_HUD`; compositing at `FRAME_BEFORE_HUD` darkened
   the bloom glow itself). Debug views visualize map/coverage/factors and still draw at
   `FRAME_BEFORE_HUD` so nothing the scene layers on afterwards obscures them.
4. **Screen-space shadows** (Bend Studio's Days Gone technique, Apache-2.0): a compute pass
   (`res/bend_sss.wgsl`) traces the depth snapshot toward the projected sun position and
   writes a screen-sized visibility texture the composite max-combines with the mapped
   occlusion. `src/bend_sss_cpu.h` (verbatim Bend) builds up to 8 dispatches per frame from
   the homogeneous light coordinate `proj_from_world × (dirToLight, 0)`; each dispatch gets
   its own uniform slot (shared light coordinate + per-dispatch wave offset). Wavefronts of
   64 threads share one ray segment through workgroup memory — ~1.2–1.6 threads per pixel
   total. WGSL port deltas: the border-color point sampler becomes a bounds-checked
   `textureLoad` returning far depth, and the wave-intrinsic early-out is dropped (WGSL
   uniformity rules); trace length is fixed at 60 pixels (`SAMPLE_COUNT`). Because Bend's
   thickness threshold is a fraction of the pixel's remaining depth range, the term resolves
   contact detail and thin casters at *any* distance — it pairs with a reduced `boxRadius`
   (sharper map texels up close, screen-space detail everywhere).
5. **Game-shadow suppression**: pre-hooks skip `dDlst_shadowControl_c::imageDraw/draw` while
   the mod is active (typed hooks only — no symbol manifest needed). `drawCloudShadow` was
   skipped here too until it was established that it is the *moya* (靄, mist/haze) packet
   rather than a shadow routine — see "Known caveats".
6. **Indoor auto-disable**: `dKy_Indoor_check() != 0` (+ `indoorDisable` on) suppresses the
   shadow MAP render and its composite path (interiors read as fully shadowed under a
   sky-light map), and the suppression hooks go inactive so the game's own shadows return —
   but the composite still runs its screen-space-only path, so the Bend SSS term persists
   indoors. Indoors is therefore just "screen-space-only mode" triggered automatically.

The controls are grouped **General** (Enabled, Strength — affect both methods) / **Shadow
Map** (map toggle, size, coverage, cascades/splits/blend, PCF + far softening,
bias/slope/offset/normal-smoothing, clipping, two-sided, disable-map-indoors) / **Link
Cascade** (toggle, its own map size, its coverage) / **Screen Space Shadows** (toggle,
thickness, edge, contrast, shadow length, ignore-edges, distance fade) / **Debug**. Anything
under "Shadow Map" or "Link Cascade" is inert when the map is off; anything under "Screen
Space Shadows" is inert when SSS is off.

## The original issues and where their fixes live

1. **Shadow acne vs peter-panning** → slope-scaled bias + normal-offset receiver
   (shadow.wgsl) + contact shadows filling the contact gap that bias opens.
2. **Daytime direction flipped** → historical: our aurora prototype negated sun x/z to
   compensate for a shadow-map V-flip sampling bug; once the flip was fixed the negation
   itself was the bug (night looked right because the moon is opposite). The mod uses the
   un-negated game sun/moon. If direction ever looks wrong again, check UV convention
   *first* (`0.5 - ndc.y * 0.5` — WebGPU rasterizes +Y NDC to texture row 0).
3. **Interiors fully shadowed** → the `dKy_Indoor_check` gate above.
4. **Draw distance** → cascaded shadow maps: up to 3 nested boxes (radii = Coverage × the
   split percentages) each with a full-resolution map, so 16000+ coverage no longer blurs
   nearby shadows, plus the optional Link cascade for player self-shadow detail.
5. **Shadows popping by camera angle** (Temple of Time ceiling, Lake Hylia mountains) →
   the `J3DUClipper` bypass during replay (the game culls against the *camera* frustum;
   casters behind/above the camera must still cast). The clip pre-hook fires per clip test
   (hundreds–thousands per frame), so the bypass gate (enabled + map on + not indoors + sun
   above the horizon + `noFrustumClipping`) and the game-shadow-skip gate are computed **once
   per frame** in the `SCENE_BEGIN` callback and cached; the hooks then only read a bool
   instead of re-running the config reads, indoor check, and sun-position math every call.

   The bypass has a hidden cost the mod now claws back: the draw lists it inflates are the
   same lists the game's own scene pass consumes, so with the bypass active the **main view**
   also drew every off-screen actor each frame (CPU streaming + GPU raster for nothing
   visible). **Main View Culling** (`mainViewCull`, 1.8.0, default on) restores the lost
   culling at *draw* time instead of list-build time: `SCENE_BEGIN` extracts the scene
   camera's six world-space frustum planes (from the CameraService `proj_from_world`,
   reversed-Z aware), and during the scene window (`SCENE_BEGIN` → `FRAME_BEFORE_HUD`,
   outside the replays — the sky lists draw before the window and the HUD after, so neither
   is ever tested) the mat-packet hook skips packets whose every shape sphere lies fully
   outside the frustum. Margins are deliberately generous — 1.5× radius + 300 world units,
   and spheres over 20000 units (sky domes, oddly-anchored stage pieces) are never culled —
   so visible geometry is not at risk; the payoff is that the main scene pass returns to
   roughly its vanilla cost while the replays keep the complete caster set. Only active while
   the bypass itself is active (`noFrustumClipping` + shadows wanted). Since 1.9.0 mixed mat
   packets are also culled per shape: `entryMatSort` merges same-material shape packets from
   several models into one packet, so one on-screen instance used to keep all its off-screen
   siblings drawing. A `J3DMatPacket::draw` post-hook bounds the window in which `j3dSys`'s
   current model is trustworthy (prepareDraw sets it immediately before every packet-chain
   drawFast), and inside that window the drawFast pre-hook applies the same frustum test per
   shape — never outside it, where a stray drawFast could see a stale model (the 1.6.1
   hazard).
6. **Light leaking through level edges** → single-sided geometry facing the player is
   back-facing from the light, so its material's cull mode dropped it from the shadow map.
   Fix: two-sided casters during replay. Direct GX drawers are covered by a `GXSetCullMode`
   pre-hook that rewrites the argument to `GX_CULL_NONE`; J3D materials bake their cull mode
   into the material display list's genMode BP write (and aurora's command processor ignores
   BP masks), so a `J3DShape::drawFast` pre-hook re-issues genMode through the GX shim —
   material-correct texgen/chan/TEV/ind counts, cull forced off — which the shim flushes at
   the shape's first `GXCallDisplayList`, after the material DL and before any geometry.
   Note the "Light View" debug renders the world through the game's normal path, not the
   replay, so it still shows backfaces culled.
7. **Shadows dimming the bloom glow** → the composite ran at `FRAME_BEFORE_HUD`, which is
   after the game's mid-scene bloom draw; it now runs at `SCENE_AFTER_OPAQUE` (see above).

## Tunables

| Var | Default | Meaning |
|---|---|---|
| `effectEnabled` | on | master toggle |
| `shadowMapEnabled` | on | off = screen-space-only mode: no map render/composite, the game's own simple/real shadows return (the skip hooks go inactive), the Bend SSS term still applies |
| `mapSize` | 2 | EACH world cascade's map: 0=1024 1=2048 2=4096 3=8192 |
| `boxRadius` | 25000 | full coverage radius in world units (1000–30000) = the FAR cascade |
| `cascadeCount` | 2 | world cascades minus one (UI select "1/2/3"): 0=single map, 1=two cascades, 2=three (default). 3 costs a third replay, so it is a framerate decision, not a stability one — see the streaming-budget note, which is historical |
| `cascadeNearPct` / `cascadeMidPct` | 5 / 40 | near / mid cascade radii as % of Coverage (log-uniform for 3 cascades) |
| `cascadeBlend` | 20 | cross-fade band width at each cascade boundary, % of the cascade extent |
| `cascadeCull` | on | light-column culling of replay geometry per cascade (keeps the passes inside the engine's per-frame streaming budget - leave on) |
| `cascadeStagger` | on | staggered cascade updates: near every frame, mid/far alternate, skipped frames composite the cached copy of the last rendered map (see architecture §2) - the main CPU saver at 2-3 cascades |
| `mainViewCull` | on | with `noFrustumClipping`, cull the game's own scene pass per mat packet against the camera frustum at draw time (the bypass otherwise makes the main view draw every off-screen object); replays still see everything |
| `casterMinTexels` | 1 | skip casters whose world bounding radius is smaller than this many of the cascade's texels (sub-texel shadows); the main lever for the per-frame geometry budget. Raise (4-8) to stay in budget at wide coverage / 3 cascades |
| `grassShadows` | 0 (All) | which cascades replay the dDlst packet list — the field grass/flower custom drawers (`d_grass.inc`/`d_flower.inc`, the list's only users). They are immediate-mode per-tuft draws no shadow cull can touch, redrawn in FULL by every included cascade — a large flat CPU cost per replay in grassy areas. 1 = near cascade only (crisp close grass shadows kept, distant dapple dropped), 2 = off (SSS still grounds on-screen grass) |
| `cascadeEdgeFade` | on | fade the widest cascade's shadow out across its outer edge (band = `cascadeBlend`) instead of a hard coverage cutoff |
| `pcfFarStep` | 1 | extra PCF kernel steps per cascade beyond the near one (0–2) |
| `linkCascade` | off | the Link cascade: an extra map covering only the player, combined with `max()`. **On = Link is also removed from the world cascades.** The composite takes whichever cascade is darker (`shadow.wgsl`), so drawing him into both lets a coarse wide-area map win that `max` over the crisp one — reintroducing the blocky edges and self-shadow speckle the Link cascade exists to remove. Trade-off: his cast shadow then comes only from this map, so at very low sun angles a long shadow can run past its edge; raise `linkCoverage` if that shows. Excluding him also makes the world cascades' cached copies (`cascadeStagger`) valid for longer, since his movement no longer changes them. |
| `linkMapSize` | 2 | Link cascade resolution (same scale as `mapSize`), independent of it |
| `linkCoverage` | 300 | Link cascade box radius in world units (100–2000) |
| `strength` | 60 | shadow darkening % |
| `shadowTint` | 50 | tint shadows toward the current skylight color (`vrbox_sky_col`, peak-normalized) instead of neutral gray - reads as skylit, follows area/time/weather; hue-only, never brightens. 0 = neutral. Both methods |
| `receiverPlaneBias` | on | receiver-plane depth bias: derive the exact per-tap bias from the receiver surface's light-space depth gradient (built from the **geometric face normal**, not the shading normal — see the two-normals note), so acne clears with almost no flat margin and shadows stay attached to their casters (Isidoro 2006). When on it **replaces** `slopeBias` (the gradient is the exact slope term) and adds a fractional-sampling term for the centre texel, taken over half a texel and **hard-capped at 0.1% of the cascade depth range** (uncapped it inherited the clamped gradient and contributed up to 4% — hundreds of world units, more than a character is tall, which leaked self-shadowing straight through); `bias` still applies. The whole slope/plane term is scaled by the **light-facing gate** (`smoothstep(-band, band, n·L)`, `band` from `terminatorSoftness`) so surfaces turned away from the light get ~none of it — they're darkened by the two-sided map's front-most face, not self-shadow, and biasing them there leaks thin geometry (fingers, facial features) back into light. Off = the old constant + `slopeBias` margins (also light-facing-gated). Cap `rpdb_max = 0.02` (max 2% of a cascade's depth range per texel). Both methods |
| `bias` | 2 | constant depth bias (normalized against light range), applied every tap. With `receiverPlaneBias` on, keep small; raise only if flat light-facing ground still shows acne |
| `slopeBias` | 2 | bias added ∝ surface slope vs light. **Ignored when `receiverPlaneBias` is on** (that derives the slope term exactly); manual fallback only |
| `attachedShadows` | on | also shadow surfaces facing **away** from the sun (the `n·L` term), which a cast-only shadow map cannot reach when they are unoccluded (a back-lit nose, protruding tunic/boot facets). Combines as two independent visibilities — `occlusion = map·f + (1 − f)` where `f = smoothstep(-band, band, n·L)` — so the map fades out exactly across the band where its comparison stops being trustworthy and the `n·L` term takes over. **Not `max()`**: that form reads half-lit in the middle of a cast shadow at the terminator and showed as a specular-looking stripe (`docs/authored_normals.md` §8.12). Off = map-only (those back-faces read as fully lit) |
| `terminatorSoftness` | 20 | half-width of the light→shadow transition (`band = terminatorSoftness/100 × 0.5`, in `n·L`; floored at 0.02 in-shader). Low = crisp/hard sun-shadow boundary on curved surfaces; high = soft, gradual falloff. Drives both `attachedShadows` and the slope-bias light-facing gate |
| `normalOffset` | 50 | receiver offset, % of one shadow texel's world size (default = 0.5 texel; already conservative — this is a percentage, not a texel count) |
| `pcf` | 2 | PCF kernel: 0=1×1 1=3×3 2=5×5 3=7×7 |
| `contactShadows` | on | the Bend screen-space shadow term (runs even with the map off / indoors) |
| `sssThickness` | 150 | assumed caster thickness, 1/100 % of remaining depth (50 = 0.5%) |
| `sssEdgeThreshold` | 200 | depth delta treated as an edge, 1/100 % (200 = 2%) |
| `sssContrast` | 4 | contrast boost on the SSS transition (1–8) |
| `sssLength` | 20 | NEAR screen-space shadow length in render pixels, smooth falloff (60 = the full trace). The facet-banding fix - see below |
| `sssLengthFar` | 40 | FAR screen-space shadow length; the length ramps from `sssLength` to this by receiver camera distance (see below), so distant grass gets a long trace while Link keeps the short one. Set equal to `sssLength` to disable the ramp |
| `sssLengthRampStart` / `sssLengthRampEnd` | 3000 / 12000 | world-unit distances over which the SSS length grows from `sssLength` (near) to `sssLengthFar` (far). Keep Start past close-up geometry so Link/near structures keep the clean near length |
| `sssBias` | 0 | receiver offset in shadow-window %, uniformly lightens ALL near-surface SSS detail. Blunt fallback; prefer `sssLength` |
| `sssIgnoreEdges` | off | edge pixels don't cast (helps grazing-angle aliasing) |
| `sssFade` | off | fade the screen-space shadows out with distance so distant fogged geometry isn't full-strength shadowed |
| `sssFadeStart` / `sssFadeEnd` | 8000 / 25000 | world-unit distances where the SSS fade begins / completes (set around where the scene washes into fog) |
| `noFrustumClipping` | on | the anti-popping clipper bypass (issue 5) |
| `twoSidedCasters` | on | render casters with backface culling off (issue 6) |
| `indoorDisable` | on | disable the shadow MAP indoors (game shadows return); screen-space shadows still run (issue 3) |
| `perfLog` | off | logs averaged game-thread timings every ~600 frames: whole-frame + scene time, then one line per cascade splitting its replay into setup / J3D-buffer walk / grass (packet list) / finish (resolve) phases with per-run drawn/culled packet counts. The tuning feedback channel - the phase split separates J3D geometry cost, the grass/flower packet list, and fixed pass overhead |
| `debugView` | 0 | map/coverage/factor visualizations + SSS buffer/edge-mask views |

Tuning order for acne: with `receiverPlaneBias` on (default) the slope term is exact, so most
acne is already handled — leave `bias`/`slopeBias` low and only raise `normalOffset` slightly
if a curved surface still bands. With it off, fall back to the manual order: raise `slopeBias`
first, then `normalOffset`; lower `bias` if shadows
detach at feet (the screen-space term hides small gaps). Per Bend's guidance, tune
`sssThickness` in multiples of 2 and scale `sssEdgeThreshold` alongside it; use the "SSS
Edge Mask" debug view when striated patterns appear on flat surfaces (or turn on
`sssIgnoreEdges`).

**SSS facet banding** (Link's cap, hair, cliffs look faceted only with screen-space shadows
on): shorten `sssLength`. The banding is *convex curvature self-occlusion*: on a low-poly
convex surface the neighboring polygon genuinely rises above the receiver near the light
terminator, so the trace correctly — but uglily — shadows it facet by facet. That occlusion
aligns at facet-scale distances (tens of pixels), while genuine micro-detail (the Hylian
shield insignia, hands, straps) shadows its receiver within a few pixels of contact.
`sssLength` fades shadows out with caster distance along the ray, so shortening it prunes
the bands while contact detail keeps full strength (GPU-verified on a synthetic dome +
micro-ridge). It's in render pixels, so raise it when supersampling. Two things that do
NOT work, tried and discarded: a constant receiver bias (`sssBias`, kept as a fallback)
lightens contact micro-detail exactly as fast as the banding; and receiver-plane slope
compensation targets nothing — Bend's perspective-corrected model already handles planar
receiver tilt, which a tilted-plane GPU sweep confirmed (no acne at any tilt).

**Distance-ramped SSS length (1.11.0)**: the short length that fixes facet banding also
starves *distant* grass — foliage on grazing far ground casts a shadow that stretches across
many screen pixels, so a 20px trace fades it out before it reaches the occluder. A single
global length can't serve both regimes because they are separated by camera distance: the
problem cases for a long trace (Link's facets, over-darkened nearby walls) are all close to
the camera, where the shadow map already does the structural work; the case that *needs* a
long trace (distant grass) is far away. So the trace length now grows with the **receiver's
world distance from the camera** — the compute pass unprojects each receiver pixel from its
own depth (`world_from_proj` + `camera_eye`, sky already early-outs) and lerps `range_falloff`
from `sssLength` at `sssLengthRampStart` to `sssLengthFar` at `sssLengthRampEnd`. Link and
near structures keep the clean near length; only the far field lengthens. This is the natural
partner to Grass Shadows = Near Only (§ Caster capture): the near cascade's map carries close
grass, the lengthened SSS carries distant grass. If distant cliffs/walls read as
over-darkened, lower `sssLengthFar` or push `sssLengthRampStart` out; the ramp self-disables
when `sssLengthFar == sssLength`.

## Normals, detail, and the screen-space term (important)

The reconstructed surface normal is used ONLY for the shadow-map receiver's slope bias
and normal offset. The screen-space shadow term (`screen_shadow_at`) reads the depth
buffer directly and never touches the normal, so all the fine SSS detail (Hylian shield
insignia, Master Sword sheath geometry) is independent of how the normal is computed.
That means the map's normal treatment cannot remove that detail — the screen-space term carries
the fine self-shadowing regardless. The shading normal now comes from the game's own **authored
vertex normals** via the Depth to Normal provider, which are smooth at the source, so the blur
that used to sit here (`normalSmooth`) has been removed entirely.

## Shadow-map tuning guide (plain language)

Mental model: every frame the mod takes a **depth photo of the world from the sun**, then
for each screen pixel asks "can the sun's photo see this spot?" — if not, it's shadowed.
Every artifact comes from that photo having a limited number of pixels: one photo pixel
covers several world units, so a surface can wrongly shadow *itself* where the photo is too
coarse ("acne"). Every acne control trades against the opposite failure: pushing the test
too far makes shadows detach from objects' feet ("peter-panning").

- **Map Size** — each photo's resolution. Bigger = sharper edges and less acne at the
  source, at GPU cost. With 3 cascades, 4096 is the sweet spot — the near cascade already
  concentrates its whole map on a small area.
- **Coverage** — how wide an area the whole system covers (the far cascade). With cascades
  on, big values (16000+) are fine: the near cascade keeps close-up shadows sharp.
- **Cascades / Near Split / Mid Split** — the photo is taken up to 3 times: a small sharp
  one around you, a medium one, and the full-coverage one. The splits set how big the
  small/medium ones are, as a percentage of Coverage. Keep them roughly geometric (each
  step ~3x the previous: 12% / 35% / 100%) so each transition steps sharpness evenly. Use
  the **Cascades debug view** (red/green/blue = near/mid/far) to see who covers what.
  Default is 2 cascades; 3 looks best but can exceed the engine's fixed per-frame geometry
  budget in the densest areas (instant crash to desktop) — if an area reliably crashes on
  3, use 2 there until the platform's buffers are raised.
- **Cascade Culling** — leave on. It skips geometry that can't cast into each cascade's
  box, which is both the perf win and what keeps multiple cascades inside the engine's
  geometry budget.
- **Staggered Cascades** — leave on. The mid and far photos are retaken every *other*
  frame instead of every frame (your close-up photo still updates every frame), which
  removes a whole scene re-render from most frames — the biggest CPU saving in the mod.
  Sudden changes (warps, sleeping, camera cuts) always force fresh photos. The only
  theoretical visual: a distant moving object's shadow updates at half rate — turn it off
  if you ever notice that (you likely won't).
- **Main View Culling** — leave on. Companion to No Frustum Clipping: that option keeps
  everything in the draw lists so off-screen objects still cast shadows, but it also made
  the game draw all of them in your normal view every frame. This skips what you can't see
  while drawing the normal view only — shadows are unaffected.
- **Cascade Blend** — how wide the cross-fade between neighboring cascades is. Raise it if
  you can see a line where sharpness changes; costs extra samples only in the band.
- **Far Softening** — far cascades have chunkier photo pixels; this widens their smoothing
  kernel to hide the stair-steps. +1 step is a good default.
- **Link Shadows / Link Map Size / Link Coverage** — a fourth tiny photo of just Link (plus
  whoever stands right next to him). 4096 over a 300-unit box gives him razor-sharp
  self-shadowing no matter what Coverage is set to. It only draws his models, so it's much
  cheaper than a full cascade; turn it off if you don't care about character close-ups.
- **Strength** — plain darkness of the shadows. Pure taste.
- **Soft Shadows** — averages neighboring photo pixels at the shadow edge. Higher = softer,
  hides stair-stepping on cliffs. Costs a little GPU. 5×5 or 7×7.
- **Receiver-Plane Bias** — leave ON. This is the smart version of Bias: instead of a fixed
  push, it works out *exactly* how much a surface's own depth changes across each photo pixel
  and biases by precisely that. The result is acne clears with almost no flat push, so shadows
  stay glued to their casters instead of sliding off — the fix for "the bias cleaned the acne
  but moved the shadow." When it's on, Slope Bias is unnecessary (it does the slope math
  exactly), and you can keep Bias near zero. Turn it off only to A/B against the old manual knobs.
- **Bias** — moves every comparison a fixed distance toward the sun so surfaces stop
  shadowing themselves. The bluntest tool: enough to kill all acne everywhere will visibly
  detach shadows. With Receiver-Plane Bias on, keep it near zero — raise only if flat,
  sun-facing ground still sparkles.
- **Slope Bias** — extra bias applied *only where the surface tilts away from the sun*,
  which is where acne concentrates (cliffs, rooftops at grazing light). **Ignored when
  Receiver-Plane Bias is on** — it's the manual stand-in for what that does automatically.
  Only relevant with Receiver-Plane Bias off; then it's the first knob for cliff acne (30–80).
- **Normal Offset** — instead of changing the depth comparison, nudges the *tested point*
  slightly off the surface, about one photo-pixel's worth. The most effective acne killer
  with the least detachment. 100–200%.
- **Two normals, two jobs (do not merge them again).** The shadow term consumes the surface
  direction twice, and the two uses want *different* normals:
  - **Shading normal** (the provider's, authored/smooth) — the `n·L` terminator, Attached Shadows,
    and the Normal Offset receiver. These are lighting/shading quantities; a smooth direction is
    what makes the light-to-shadow boundary roll across curvature instead of stepping per facet.
  - **Geometric face normal** (`geometric_normal_at`, from the depth buffer) — Receiver-Plane Bias
    and Slope Bias. These solve the receiver's depth gradient / tilt in light space, which is a
    property of *the triangle the pixel actually sits on*. A smooth normal tilts the comparison
    plane away from the real triangle by the shading-vs-face angle, so every facet gets a
    systematically wrong bias and the shadow factor breaks into per-triangle patches **even though
    the normal buffer is perfectly smooth**.

  Authored normals are what exposed this: a depth-reconstructed normal *is* the face normal, so the
  two uses coincided and the bug was invisible. Switching the provider to smooth normals maximised
  the gap. `normalSmooth` was the earlier partial workaround from the same confusion — it traded
  bias banding at facet edges for bias error inside facets, which is why raising it never fully
  worked.
- **Attached Shadows** — leave ON. A shadow *map* can only darken a surface when something
  *blocks* the sun from it (a cast shadow). It cannot darken a surface that simply *faces away*
  from the sun with nothing in front of it — so a back-lit nose, or protruding tunic/boot
  facets, read as fully lit and *leak* bright through the shadow. This adds the missing half:
  surfaces angled away from the sun are darkened by how far they face away. It only touches
  those leaks — anything already cast-shadowed is unchanged.
- **Terminator Softness** — how sharp the line is where sun meets shadow on a *curved* surface
  (the "terminator"). Low = a crisp, sometimes hard/jagged edge; high = a soft, gradual falloff.
  It shapes both Attached Shadows and the bias gate. Start ~20. Too low → hard edge; too high →
  the shadow washes onto surfaces that should be lit and the character looks flatly dark.
- **Two-Sided Casters / No Frustum Clipping / Disable Indoors** — leave on: they fix light
  leaks at level edges, shadows popping with camera turns, and black interiors respectively.
- **Shadow Map toggle** — off runs only the screen-space shadows and brings back the game's
  own character shadows; useful as a comparison baseline and as a cheap mode.

Recommended order (Receiver-Plane Bias + Attached Shadows ON — the defaults): (1) Coverage wide
enough for the landscape (16000 for the big vistas), 3 cascades, splits near-geometric.
(2) light/shadow edge on characters looks jagged, lower it if fine detail (fingers, folds) rounds
away. (3) **Terminator Softness** to taste (~20): raise for a softer sun-shadow boundary, lower
for a crisper one; back off if lit surfaces start going dark. (4) Bias and Slope Bias near zero —
Receiver-Plane Bias handles map acne; nudge **Normal Offset** up only if a curved *lit* surface
still sparkles. (5) Soft Shadows + Far Softening to taste; widen Cascade Blend if a transition
line shows. Rule of thumb: if you push any bias knob and shadows start *detaching* (floating, gaps
at the feet) or *lit* surfaces go dark, you've gone too far — back off.

Legacy order (Receiver-Plane Bias OFF): (1) Coverage + cascades as above. (2) Bias down to ~40.
(3) Raise Normal Offset until flat ground is clean. (4) Raise Slope Bias until cliffs are clean.
(5) Soft Shadows + Far
Softening to taste; widen Cascade Blend if a transition line shows. If feet shadows detach: lower Bias first, then
Normal Offset — the screen-space term re-grounds contacts regardless.

## Shadow term assembly (current, post authored-normals work)

The occlusion for a pixel is built in this order. Every step here was arrived at by fixing a
specific bug — see `docs/authored_normals.md` §8 for the failures and why the obvious alternatives
are wrong.

```wgsl
// 1. TWO normals, never interchangeable (authored_normals.md 8.6)
n      = world_normal_at(...)        // SHADING: provider's authored normal. n.L, attached
                                     //          shadow, normal-offset receiver.
n_geom = geometric_normal_at(...)    // GEOMETRIC: face normal from depth. Receiver-plane bias
                                     //            and slope-bias tan_t ONLY.

// 2. Terminator from the SHADING normal
ndl          = dot(n, light_dir)
light_facing = smoothstep(-band, band, ndl)     // band from terminatorSoftness

// 3. Per cascade: normal offset scaled by sin (Holbert, complete form)
receiver = world + n * (normal_offset * texel_world[map] * sqrt(1 - ndl*ndl))

// 4. Receiver-plane bias from the GEOMETRIC normal, its fractional term hard-capped
bias_uv    = receiver_plane_bias_uv_from_normal(map, n_geom) * light_facing
bias_uv    = clamp(bias_uv, -cap, cap)                     // cap = rpdb_max * map_size
fractional = (|bias_uv.x| + |bias_uv.y|) * inv_map_size * 0.5
base_bias  = bias[map] + min(fractional, kMaxFractionalBias)   // 0.001 — see 8.8

// 5. Combine: two INDEPENDENT visibilities, so the light that arrives is their product
occlusion = map_occlusion * light_facing + (1 - light_facing)   // = 1 - (1 - m) * f
```

**Step 5 is the one to understand before touching any bias knob.** A shadow-map comparison is only
meaningful where the surface faces the light; edge-on, one texel spans a huge depth range and the
comparison is decided by bias error, not geometry. Acne and light leaks are therefore *the same
failure with opposite sign*, and no bias setting fixes one without causing the other. Fading the map
across that band removes the failure rather than trading it. `light_facing = 1` leaves cast shadows
completely untouched.

**Step 5 must be a sum, never a `max`.** It shipped as `max(m·f, 1 − f)` for a long time. Same two
endpoints, same intent, and wrong in between: for a pixel *fully inside a cast shadow* (`m = 1`) it
reads 1 at `f = 1`, 1 at `f = 0`, and **0.5 at `f = 0.5`** — a V-shaped dip to half darkness through
the middle of the terminator band, inside a shadow. At the default Terminator Softness that band is
±0.1 in `n·L`, so on curved geometry the dip is a thin bright stripe following the terminator: it
reads as a **specular glint**, and appears only where a cast shadow *crosses* the terminator, which
is why it is not at every shadow edge. See 8.12.

**Step 4's cap is not optional.** Uncapped, that term inherits the clamped gradient and contributes
up to `2 × rpdb_max` = 4% of the cascade depth range as a *flat* margin — hundreds of world units on
a wide cascade, more than a character is tall, which leaks self-shadowing straight through. It is
gated by `light_facing`, so it only ever showed on sunlit-facing surfaces of a back-lit character.

### Debug View 15 — "Shadow Terms"

Renders which term is shadowing each pixel: **red** = the map comparison (already weighted by
`light_facing`), **green** = the attached `n·L` term, **yellow** = both, **black** = neither, i.e.
the pixel is reported fully lit.

Use it on any wrongly-lit pixel. The Shadow Factor view shows only the combined result, so it cannot
tell "the map missed an occluder" from "`n·L` misread the surface" — two bugs with identical
symptoms and opposite fixes. Diagnose from view 15, not from Shadow Factor.

Since step 5 is additive, **the two channels sum to the shadow factor**, which makes one failure
mode readable directly: follow a red region into a green one, and the transition should stay at full
brightness throughout. Anywhere it *dims* in between, the two terms are failing to hand over and
that dip is a visible bright artifact in the real frame. That is exactly how 8.12 was found.

### Normal Smoothing: removed

There was a `normalSmooth` setting and a `normal_smooth.wgsl` bilateral blur. Both are gone
(`38386e4`). It existed only to hide the faceting a depth-reconstructed normal has by construction;
the provider now supplies the game's authored vertex normals, which are smooth at the source. After
the two-normal split it was also actively harmful — it flattened the curvature the shading normal
carries. Do not reintroduce it: if bias faceting appears, the cause is a bias term reading the
shading normal instead of `n_geom`.

## Shading history — both problems are closed

This section used to be a TODO listing two open shading problems. Both are resolved; it is kept as
history because the reasoning is what a future regression should be diagnosed against, and because
its old advice was actively wrong once the platform changed.

**1. Faceted normals — conditional, not inherent.** The old text said "this platform has no
authored surface normals". That was true of the upstream base the tree briefly retreated to, and is
**not** true now: the platform is `automata-rtx/dusklight-ao` with GfxService 1.3, which hands
every mod the game's own authored vertex normals. Faceting therefore only appears on the
**reconstruction fallback** — when the user has not turned on *Video → Rendering → Scene Normal
Buffer*, in compatibility mode (D3D11 / OpenGL ES), or per pixel where a draw supplied no normal.
That is expected there, not a defect.

> **Do not "fix" it by reintroducing `normalSmooth`.** The old route 1 suggested exactly that, with
> a caveat about smoothing only `n` and not `n_geom`. The pass is deleted and stays deleted: it
> existed to hide reconstruction faceting, which authored normals remove at the source, and it
> flattened real curvature the shading normal carries. See `docs/authored_normals.md` §8.9.

**2. Broken shading on back-lit Link — fixed and confirmed in-game.** The patchy shading on his
boots, torso, lower tunic and face when back-lit closed across `5300789`..`b426c4d`, and the user
has confirmed it resolved. No single commit is attributable, because they were verified together:

| change | what it did |
|---|---|
| `5300789` | the two-normal split — bias reads the geometric face normal, `n·L` the shading normal (`docs/authored_normals.md` §8.6) |
| `9af4701` | `sin`-scaled normal offset, Holbert's complete form (§8.7) |
| `aa2c723` | **receiver-plane fractional-bias cap** (§8.8) — the strongest single candidate and the only one that was arithmetic rather than inference: the term was contributing several hundred world units of flat bias against a ~150-unit-tall character |
| `b426c4d` | the additive term combine (§8.12) — also fixed a separate terminator glint |

**If it ever regresses, §8.8 is the first place to look**, and Debug View 15 (**Shadow Terms**) is
still the view that separates a missing occluder from a misread `n·L` — they have identical
symptoms otherwise, and Shadow Factor alone cannot tell them apart.

## Known caveats

- **2D-menu crash (fixed in 1.6.3)**: with the mod enabled but the shadow map off, the
  screen-space-only composite path still called `compute_light` → `dKy_getEnvlight` /
  `dComIfGs_getTime`, which can touch torn-down environment/time state on a geometry-less
  screen (the file-select menu), crashing the game. `composite_map_pass` now bails early on
  `!draw_lists_ready()` — no populated 3D scene means nothing to shadow, so it never enters
  the game-state calls or the offscreen pass there. Same readiness gate the replay already
  used, so real scenes are unaffected.
- **Distortion particles vanish with the map on** (heat-haze / steam / wind in Kakariko
  Village, Goron Springs) — **RESOLVED. The mod was deleting them itself.**

  The cause was not dirty GX state and not the replay. Alongside the game's two real shadow
  routines, the mod also pre-hooked `drawCloudShadow` and skipped it unconditionally whenever
  the shadow map was active. `drawCloudShadow` is **not a shadow routine** — the name is
  romanized-Japanese shorthand, and it draws the whole *moya* (靄, mist/haze) packet: the
  drifting haze, mist and steam, and at `mMoyaMode >= 50` the framebuffer heat-shimmer and
  wolf-senses distortion (`d_kankyo_rain.cpp:4549` splits the two branches). The handler had no
  mode check, so turning on the shadow map deleted all of it.

  That explains every symptom that was recorded here, including the one that misled the
  investigation: the `earlyShadowPass` experiment changed nothing because the effects were
  never being drawn at all, so *when* the map rendered was always irrelevant. The previous
  conclusion in this entry — "it's something the replay does, a GX/PE or texture-cache state it
  leaves dirty" — was wrong, and it is what sent sessions after a phantom bug in innocent code.

  **Fix:** the `drawCloudShadow` hook was removed from this mod entirely. Nothing it drew was a
  projected shadow this mod replaces, and Effect Remover's **Haze Removal** already owns moya
  with per-mode toggles that deliberately spare the `>= 50` modes. A side effect of the old
  hook was that it silently overrode those toggles whenever both mods were installed; that is
  fixed too. **Expected change in game:** with the shadow map on, drifting haze/steam and the
  heat-shimmer now appear where they previously did not. If you want them gone, that is Haze
  Removal's job — per mode, which is the control the old behaviour bypassed.
- **Midna**: the game's projected shadow (which the mod hooks out) is where Midna
  "lives" during her summon/emergence animation. A retain path (re-enable the game shadow
  for Link only, or anchor her to our sun ground-projection) is a known follow-up.
  Naming note: the game's own classes are `dDlst_shadowSimple_c` and `dDlst_shadowReal_c`
  (`d_drawlist.h:202`, `:254`), and `d_bg_s.cpp` calls the projected geometry kind
  リアル影 ("real *kage*"). **"Blob shadow" is this project's coinage, not the game's word** —
  it is fine when describing the look to a player, but do not grep the game for it.
- **The per-frame streaming budget (the v1.6.0/1.6.1 startup crash) — HISTORICAL; not a live
  risk.** Both halves of what caused it have since been fixed, on both sides. Kept here
  because the tuning levers below are still the right ones for framerate, and because the
  mechanism explains what the culling settings are *for*.

  Aurora streams ALL GX geometry into fixed-size per-frame buffers, mapped non-growable
  ranges whose overflow is an unconditional `abort()` (`ByteBuffer::resize`). The game's own
  draw plus EVERY cascade replay share them, so unculled 3-cascade replays (~4× scene
  geometry, worse with `noFrustumClipping`) blew the **index** buffer on dense scenes —
  instantly closing the game on the first frames after loading a save, exactly when geometry
  volume peaks.

  At the time that buffer was **1 MB**. Upstream aurora has since raised both of the ones
  that mattered, independently of us:

  | date | upstream commit | change |
  |---|---|---|
  | 2026-07-07 | `b979ff6` | Vertex 3 MB → **5 MB** |
  | 2026-07-19 | `1b484d4` "Bump IndexBufferSize" | Index 1 MB → **2 MB** |

  Current sizes are Vertex 5 MB / Index 2 MB / Storage 8 MB / Uniform 24 MB
  (`extern/aurora/lib/gfx/resources.hpp:8`). So the index budget is **double** what the
  crash happened on, and the mod meanwhile gained three mitigations that did not exist then
  (below). The fork's enlarged 16/4/16 buffers were sized against the *old* numbers and are
  no longer carried; that is not a regression, and this doc previously called it one in
  error. The two
  mitigations that shipped in 1.6.2: per-cascade **light-column culling** (`cascadeCull`, skips
  shapes laterally outside a cascade's light box before their geometry streams; the axis
  toward the light is kept, so tall distant casters still shadow into near boxes) and a
  default of **2 cascades** (~the proven 1.5.x envelope). 1.6.4 adds **small-caster
  culling** (`casterMinTexels`, default 2): a shape whose world bounding radius is under a
  few of the cascade's texels casts a sub-texel (invisible) shadow, so it is skipped before
  streaming. Because texel size grows with the cascade, this prunes almost nothing in the
  near cascade and a large tail of tiny distant props in the wide far cascade — where the
  budget is actually spent. It is the main mod-side lever for staying in budget; raise it to
  4-8 to run 3 cascades / high coverage in dense areas.

  **Keeping the streaming cost down** — worth doing for framerate; no longer a crash-avoidance
  exercise. The streaming cost is
  vertex/index bytes from the *replays*, so it scales with how much geometry each replay
  streams, NOT with map resolution (Map Size is free). `cascadeStagger` (default on) already
  halves the worst frame: at 3 cascades only two replays share any one frame's buffers.
  Levers on top of that, most effective first: (1) raise `casterMinTexels` (4-8); (2) turn
  `noFrustumClipping` OFF — on, it forces every replay to include the whole off-screen
  world, which is the single biggest multiplier in open fields (cost: shadows from
  off-screen casters pop in as you turn; note `mainViewCull` already removes its main-view
  cost, so turning it off only buys replay streaming); (3) fewer cascades (2, or 1); (4)
  smaller `boxRadius` (a smaller far box holds less geometry) paired with `cascadeEdgeFade`
  + Deferred Fog to hide the nearer cutoff. Adaptive grow-on-overflow remains the *right*
  aurora change in principle, but with the index buffer doubled and the three culling levers
  in place there is no longer a case to make for it from this mod. The Link cascade is nearly
  free vertex-wise: its filter skips at drawFast BEFORE geometry streams.
- The Link cascade's position filter is by model anchor, so a character standing within
  2× Link Coverage of Link is included (harmless — more detail) and a huge world model
  whose origin happens to sit nearby would be too (its geometry mostly clips out of the
  tiny ortho box).
- **Stale-model hazard (fixed in 1.6.1, do not reintroduce)**: the filter reads
  `j3dSys.getModel()`, which `J3DShapePacket::prepareDraw` sets fresh per packet draw — but
  shapes drawn through any other path leave the LAST value in place, and on the first
  frames after a stage teardown (loading a save, the attract intro) that stale pointer can
  reference a model of the destroyed scene → use-after-free crash. The Link replay must
  clear the current model (`j3dSys.setModel(nullptr)`) before drawing and restore it after,
  so non-packet draws are skipped rather than dereferenced. The player's position is also
  finite-checked before use.
- The SSS trace length is compile-time (`SAMPLE_COUNT` 60 pixels in `res/bend_sss.wgsl`);
  making it configurable means pipeline variants (workgroup memory is sized by it).
- The pixel exactly at the light's screen position is never traced (rays converge toward
  it; inherent to Bend's wavefront projection — verified by a coverage simulation). For a
  directional sun that pixel is sky, which early-outs anyway.
- ABI-coupled: after any re-platform this mod must be rebuilt against the new
  `windows-amd64.lib` import library; the `GameService` major version rejects a mismatched load
  cleanly.
