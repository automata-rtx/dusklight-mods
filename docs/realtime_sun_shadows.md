# Realtime Sun Shadows

Mod id `dev.automata.realtime_sun_shadows`. Game-linked: includes game headers, hooks game
functions, and (on Windows) links the platform release's `dusklight.lib`.

## Architecture (based on the upstream `shadow_mod` demo)

1. **Light cameras** (`build_light_camera` / `build_light_camera_core`): sun/moon world
   position from `sun_moon_offset(daytime)`; if the sun is below the horizon, use
   daytime+180° (the moon). Horizon fade `clamp((dirY-0.05)/0.15)` softens shadows at
   dawn/dusk. One ortho box **per cascade**: nested world boxes centered near the player
   with camera-forward lookahead (radii = Coverage × the split percentages, near → far),
   each snapped to whole texels of its own map (kills crawling); near/far extents scale
   with each radius. The optional **Link cascade** is a small box (`linkCoverage`) snapped
   to the player's position with a deliberately short light distance for maximum depth
   discrimination.
2. **Caster capture** (`replay_cascade`, once per cascade at `SCENE_AFTER_TERRAIN`): replay
   the game's own opaque draw lists (`dComIfGd_drawOpaList*`) into a `create_pass` offscreen
   pass with `GXSetProjectionFull(lightReplayProjection)` + `j3dSys.setViewMtx` — the game
   re-renders the world from the light, animation included. During replay, hooks bypass
   `J3DUClipper::clip` (sphere + box overloads) so casters outside the *camera* frustum
   still render, and skip `GXCopyTex`. `resolve_pass` depth = that cascade's shadow map
   (reversed light depth: 1 = near light). Each cascade is a full save-replay-resolve
   bracket, so cost scales with cascade count. The Link cascade replays only the lists the
   player's models enter (Middle/Opa/Dark) with a position filter in the
   `J3DShape::drawFast` pre-hook: `J3DShapePacket::prepareDraw` sets `j3dSys`'s current
   model right before every drawFast, so the filter reads `j3dSys.getModel()` and skips
   models whose base translation is beyond 2× `linkCoverage` from the player — Link's body
   and equipment all anchor at his position, world geometry anchors far away or at the
   origin. No private `daAlink_c` fields are touched.
3. **Composite** (`SCENE_AFTER_OPAQUE`, `res/shadow.wgsl`): unproject scene depth to world,
   reconstruct a world normal from depth (side-selected crosses), pick the sharpest cascade
   whose box contains the receiver, apply that cascade's slope-scaled bias
   (`bias_eff = bias + slope_bias * tan_t`, tan clamped at 4; biases normalized per cascade
   against its own light range) and normal-offset receiver
   (`world + n * normal_offset * texel_world[cascade]`), PCF over hardware-bilinear
   comparison taps (kernel = base + Far Softening × cascade index). Inside the outer
   `blend_frac` band of a cascade's box the result cross-fades to the next cascade so the
   resolution step never shows as a line. The Link cascade is evaluated separately and
   combined with `max()` — its map contains only the player, so it can only add occlusion,
   never remove world shadows. Then combine with the screen-space shadow term (below) and
   darken the scene. The composite runs right after the opaque scene — before translucency
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
5. **Game-shadow suppression**: pre-hooks skip `dDlst_shadowControl_c::imageDraw/draw` and
   `drawCloudShadow` while the mod is active (typed hooks only — no symbol manifest needed).
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
   casters behind/above the camera must still cast).
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
| `shadowMapEnabled` | on | off = screen-space-only mode: no map render/composite, the game's own real/blob shadows return (the skip hooks go inactive), the Bend SSS term still applies |
| `mapSize` | 2 | EACH world cascade's map: 0=1024 1=2048 2=4096 3=8192 |
| `boxRadius` | 8000 | full coverage radius in world units (1000–30000) = the FAR cascade |
| `cascadeCount` | 1 | world cascades minus one (UI select "1/2/3"): 0=single map, 1=two cascades (default), 2=three. See the streaming-budget caveat before defaulting to 3 |
| `cascadeNearPct` / `cascadeMidPct` | 12 / 35 | near / mid cascade radii as % of Coverage (log-uniform for 3 cascades) |
| `cascadeBlend` | 20 | cross-fade band width at each cascade boundary, % of the cascade extent |
| `cascadeCull` | on | light-column culling of replay geometry per cascade (keeps the passes inside the engine's per-frame streaming budget - leave on) |
| `casterMinTexels` | 2 | skip casters whose world bounding radius is smaller than this many of the cascade's texels (sub-texel shadows); the main lever for the per-frame geometry budget. Raise (4-8) to stay in budget at wide coverage / 3 cascades |
| `cascadeEdgeFade` | on | fade the widest cascade's shadow out across its outer edge (band = `cascadeBlend`) instead of a hard coverage cutoff |
| `pcfFarStep` | 1 | extra PCF kernel steps per cascade beyond the near one (0–2) |
| `linkCascade` | on | the Link cascade: an extra map covering only the player, combined with max() |
| `linkMapSize` | 2 | Link cascade resolution (same scale as `mapSize`), independent of it |
| `linkCoverage` | 300 | Link cascade box radius in world units (100–2000) |
| `strength` | 45 | shadow darkening % |
| `bias` | 55 | constant depth bias (normalized against light range) |
| `slopeBias` | 30 | bias added ∝ surface slope vs light |
| `normalOffset` | 100 | receiver offset, % of one shadow texel's world size |
| `normalSmooth` | 3 | smooths the depth-reconstructed normal that Slope Bias / Normal Offset use (`res/normal_smooth.wgsl`): FULL-resolution per-pixel crosses + one separable depth-aware Gaussian whose radius = `normalSmooth * renderHeight / 1080` px (dense, capped 32). Only affects the shadow-MAP bias — SSS fine detail is independent (see note). One value looks the same at any internal resolution. History of failed approaches, do not repeat: (1) widening a single cross straddles facets and manufactures garbage normals (shattered glass); (2) sparse taps at fixed pixel distances ghost past a resolution-dependent sweet spot; (3) a resolution-CAPPED buffer blurs fine geometry away and needs a lossy upscale. NO light-terminator flip (mirrored the normal across curved surfaces' terminator = hard bias discontinuity on faces). 0 = off (inline 1px cross) |
| `pcf` | 2 | PCF kernel: 0=1×1 1=3×3 2=5×5 3=7×7 |
| `contactShadows` | on | the Bend screen-space shadow term (runs even with the map off / indoors) |
| `sssThickness` | 50 | assumed caster thickness, 1/100 % of remaining depth (50 = 0.5%) |
| `sssEdgeThreshold` | 200 | depth delta treated as an edge, 1/100 % (200 = 2%) |
| `sssContrast` | 4 | contrast boost on the SSS transition (1–8) |
| `sssLength` | 20 | max screen-space shadow length in render pixels, smooth falloff (60 = the full trace). The facet-banding fix - see below |
| `sssBias` | 0 | receiver offset in shadow-window %, uniformly lightens ALL near-surface SSS detail. Blunt fallback; prefer `sssLength` |
| `sssIgnoreEdges` | off | edge pixels don't cast (helps grazing-angle aliasing) |
| `sssFade` | on | fade the screen-space shadows out with distance so distant fogged geometry isn't full-strength shadowed |
| `sssFadeStart` / `sssFadeEnd` | 8000 / 25000 | world-unit distances where the SSS fade begins / completes (set around where the scene washes into fog) |
| `noFrustumClipping` | on | the anti-popping clipper bypass (issue 5) |
| `twoSidedCasters` | on | render casters with backface culling off (issue 6) |
| `indoorDisable` | on | disable the shadow MAP indoors (game shadows return); screen-space shadows still run (issue 3) |
| `debugView` | 0 | map/coverage/factor visualizations + SSS buffer/edge-mask views |

Tuning order for acne: raise `slopeBias` first, then `normalOffset`; lower `bias` if shadows
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

## Normals, detail, and the screen-space term (important)

The reconstructed surface normal is used ONLY for the shadow-map receiver's slope bias
and normal offset. The screen-space shadow term (`screen_shadow_at`) reads the depth
buffer directly and never touches the normal, so all the fine SSS detail (Hylian shield
insignia, Master Sword sheath geometry) is independent of how the normal is computed.
That means smoothing the normal cannot remove that detail — and for the coarse shadow
map, a smooth normal is actively better (per-facet normal jumps are what band the
shadows). So `normalSmooth` can go as high as needed for smooth shading with no loss of
screen-space detail. This is why we use cheap depth-reconstructed normals rather than
the game's per-vertex normals.

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
- **Bias** — moves every comparison a fixed distance toward the sun so surfaces stop
  shadowing themselves. It's the bluntest tool: enough to kill all acne everywhere will
  visibly detach shadows. Keep it LOW (30–60) and let the next three do the real work.
- **Slope Bias** — extra bias applied *only where the surface tilts away from the sun*,
  which is where acne concentrates (cliffs, rooftops at grazing light). Safe to raise —
  it does nothing on sun-facing ground. First knob for cliff acne (30–80).
- **Normal Offset** — instead of changing the depth comparison, nudges the *tested point*
  slightly off the surface, about one photo-pixel's worth. The most effective acne killer
  with the least detachment. 100–200%.
- **Normal Smoothing** — Slope Bias and Normal Offset need to know which way the surface
  faces. GameCube-era models are low-poly: the facing jumps at every polygon edge, so the
  bias jumps too and paints *faceted bands* on characters. This rounds the facing over a few
  screen pixels so the bias varies smoothly. 2–4; it's nearly free.
- **Two-Sided Casters / No Frustum Clipping / Disable Indoors** — leave on: they fix light
  leaks at level edges, shadows popping with camera turns, and black interiors respectively.
- **Shadow Map toggle** — off runs only the screen-space shadows and brings back the game's
  own character shadows; useful as a comparison baseline and as a cheap mode.

Recommended order: (1) Coverage wide enough for the landscape (16000 for the big vistas),
3 cascades, splits near-geometric. (2) Bias down to ~40. (3) Raise Normal Offset until flat
ground is clean. (4) Raise Slope Bias until cliffs are clean. (5) Normal Smoothing 2–4 to
remove faceted banding on characters. (6) Soft Shadows + Far Softening to taste; widen
Cascade Blend if a transition line shows. If feet shadows detach: lower Bias first, then
Normal Offset — the screen-space term re-grounds contacts regardless.

## Known caveats

- **2D-menu crash (fixed in 1.6.3)**: with the mod enabled but the shadow map off, the
  screen-space-only composite path still called `compute_light` → `dKy_getEnvlight` /
  `dComIfGs_getTime`, which can touch torn-down environment/time state on a geometry-less
  screen (the file-select menu), crashing the game. `composite_map_pass` now bails early on
  `!draw_lists_ready()` — no populated 3D scene means nothing to shadow, so it never enters
  the game-state calls or the offscreen pass there. Same readiness gate the replay already
  used, so real scenes are unaffected.
- **Distortion particles vanish with the map on** (heat-haze / steam / wind in Kakariko
  Village, Goron Springs) — *open*. Only the shadow **map** triggers it; screen-space-only
  mode (Shadow Map off) shows the particles normally, so that's the current workaround.
  An `earlyShadowPass` experiment that moved the offscreen replay from `SCENE_AFTER_TERRAIN`
  to `SCENE_BEGIN` made *no* difference in-game, which rules out the replay's *timing*
  relative to the framebuffer capture (`GXCopyTex` → `getFrameBufferTex`) as the cause — it's
  something the replay *does* (a GX/PE or texture-cache state it leaves dirty), not when it
  runs. Next step is to widen the GX-state save/restore around the replay. That toggle was
  removed since it did nothing.
- **Midna**: the game's projected blob shadow (which the mod hooks out) is where Midna
  "lives" during her summon/emergence animation. A retain path (re-enable the game shadow
  for Link only, or anchor her to our sun ground-projection) is a known follow-up.
- **The per-frame streaming budget (the v1.6.0/1.6.1 startup crash)**: aurora streams ALL
  GX geometry into fixed-size per-frame buffers — 5 MB vertex, **1 MB index**, 8 MB
  storage, 24 MB uniform (`extern/aurora/lib/gfx/common.hpp:176`) — and these are mapped,
  non-growable ranges whose overflow is an unconditional `abort()`
  (`ByteBuffer::resize`, `common.hpp:155`). The game's own draw plus EVERY cascade replay
  share the same buffers, so uncalled 3-cascade replays (~4× scene geometry, worse with
  `noFrustumClipping`) blew the index buffer on dense scenes — instantly closing the game
  on the first frames after loading a save, exactly when geometry volume peaks. Two
  mitigations ship in 1.6.2: per-cascade **light-column culling** (`cascadeCull`, skips
  shapes laterally outside a cascade's light box before their geometry streams; the axis
  toward the light is kept, so tall distant casters still shadow into near boxes) and a
  default of **2 cascades** (~the proven 1.5.x envelope). 1.6.4 adds **small-caster
  culling** (`casterMinTexels`, default 2): a shape whose world bounding radius is under a
  few of the cascade's texels casts a sub-texel (invisible) shadow, so it is skipped before
  streaming. Because texel size grows with the cascade, this prunes almost nothing in the
  near cascade and a large tail of tiny distant props in the wide far cascade — where the
  budget is actually spent. It is the main mod-side lever for staying in budget; raise it to
  4-8 to run 3 cascades / high coverage in dense areas.

  **Staying in the per-frame budget without a platform change** — the streaming cost is
  vertex/index bytes from the *replays*, so it scales with how much geometry each replay
  streams, NOT with map resolution (Map Size is free). Levers, most effective first: (1)
  raise `casterMinTexels` (4-8); (2) turn `noFrustumClipping` OFF — on, it forces every
  replay to include the whole off-screen world, which is the single biggest multiplier in
  open fields (cost: shadows from off-screen casters pop in as you turn); (3) fewer cascades
  (2, or 1); (4) smaller `boxRadius` (a smaller far box holds less geometry) paired with
  `cascadeEdgeFade` + Deferred Fog to hide the nearer cutoff. The definitive fix for
  unconstrained 3-cascade/high-radius is larger platform buffers (adaptive grow-on-overflow
  is the intended aurora change; a static bump is a re-platform per CLAUDE.md). The Link
  cascade is nearly free vertex-wise: its filter skips at drawFast BEFORE geometry streams.
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
  `dusklight.lib`; the `GameService` major version rejects a mismatched load cleanly.
