# Realtime Sun Shadows

Mod id `dev.automata.realtime_sun_shadows`. Game-linked: includes game headers, hooks game
functions, and (on Windows) links the platform release's `dusklight.lib`.

## Architecture (based on the upstream `shadow_mod` demo)

1. **Light camera** (`build_light_camera`, game thread at `GFX_STAGE_SCENE_BEGIN`): sun/moon
   world position from `sun_moon_offset(daytime)`; if the sun is below the horizon, use
   daytime+180° (the moon). Horizon fade `clamp((dirY-0.05)/0.15)` softens shadows at
   dawn/dusk. Ortho box centered near the player with camera-forward lookahead, snapped to
   whole shadow-map texels (kills crawling); near/far extents scale with the coverage radius.
2. **Caster capture**: replay the game's own opaque draw lists (`dComIfGd_drawOpaList*`)
   into a `create_pass` offscreen pass with `GXSetProjectionFull(lightReplayProjection)` +
   `j3dSys.setViewMtx` — the game re-renders the world from the light, animation included.
   During replay, hooks bypass `J3DUClipper::clip` (sphere + box overloads) so casters
   outside the *camera* frustum still render, and skip `GXCopyTex`. `resolve_pass` depth =
   the shadow map (reversed light depth: 1 = near light).
3. **Composite** (`SCENE_AFTER_OPAQUE`, `res/shadow.wgsl`): unproject scene depth to world,
   reconstruct a world normal from depth (side-selected crosses), apply slope-scaled bias
   (`bias_eff = bias + slope_bias * tan_t`, tan clamped at 4) and a normal-offset receiver
   (`world + n * normal_offset * texel_world`), project into light space, PCF over
   hardware-bilinear comparison taps, combine with the screen-space shadow term (below),
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
Map** (map toggle, size, coverage, PCF, bias/slope/offset/normal-smoothing, clipping,
two-sided, disable-map-indoors) / **Screen Space Shadows** (toggle, thickness, edge, contrast,
ignore-edges, distance fade) / **Debug**. Anything under "Shadow Map" is inert when the map is
off; anything under "Screen Space Shadows" is inert when SSS is off.

## The original issues and where their fixes live

1. **Shadow acne vs peter-panning** → slope-scaled bias + normal-offset receiver
   (shadow.wgsl) + contact shadows filling the contact gap that bias opens.
2. **Daytime direction flipped** → historical: our aurora prototype negated sun x/z to
   compensate for a shadow-map V-flip sampling bug; once the flip was fixed the negation
   itself was the bug (night looked right because the moon is opposite). The mod uses the
   un-negated game sun/moon. If direction ever looks wrong again, check UV convention
   *first* (`0.5 - ndc.y * 0.5` — WebGPU rasterizes +Y NDC to texture row 0).
3. **Interiors fully shadowed** → the `dKy_Indoor_check` gate above.
4. **Draw distance** → coverage radius up to 30000 with light extents scaling; the real fix
   is cascaded shadow maps (planned next major increment — near cascade sharp, far cascade
   coarse).
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
| `mapSize` | 2 | shadow map: 0=1024 1=2048 2=4096 3=8192 |
| `boxRadius` | 8000 | coverage radius in world units (1000–30000) |
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
| `sssBias` | 15 | receiver offset in shadow-window %, pushes the trace off the surface to kill facet self-shadow banding on low-poly casters (SSS counterpart to the map's `bias`) |
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
on): raise `sssBias`. The Bend trace runs against the raw depth buffer, so a low-poly caster
whose facets each tilt a little self-shadows each facet by a slightly different amount and
the polygon banding shows through. `sssBias` pushes the trace off the receiving surface —
raise it until the banding disappears. Higher values also soften genuine near-contact
darkening (the whole point of SSS), so use the lowest value that cleans up the facets. It's
the exact SSS analogue of the shadow map's constant `bias`.

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

- **Map Size** — the photo's resolution. Bigger = sharper edges and less acne at the source,
  at GPU cost. Use the biggest that performs well (4096, try 8192).
- **Coverage** — how wide an area the photo covers. Smaller area = each photo pixel covers
  less ground = sharper AND less acne. The screen-space shadows fill in fine detail
  everywhere, so keep this as small as you can tolerate the shadow cutoff distance
  (4000–6000 works well).
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

Recommended order: (1) Coverage as low as acceptable + Map Size as high as affordable —
this shrinks acne at the source before any biasing. (2) Bias down to ~40. (3) Raise Normal
Offset until flat ground is clean. (4) Raise Slope Bias until cliffs are clean. (5) Normal
Smoothing 2–4 to remove faceted banding on characters. (6) Soft Shadows to taste. If feet
shadows detach: lower Bias first, then Normal Offset — the screen-space term re-grounds
contacts regardless.

## Known caveats

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
- Single map: very large `boxRadius` spreads texels thin → cascades are the planned fix.
  In the meantime the Bend SSS term works at any distance, so a *smaller* `boxRadius` plus
  SSS is often the better trade than a huge, blurry map.
- The SSS trace length is compile-time (`SAMPLE_COUNT` 60 pixels in `res/bend_sss.wgsl`);
  making it configurable means pipeline variants (workgroup memory is sized by it).
- The pixel exactly at the light's screen position is never traced (rays converge toward
  it; inherent to Bend's wavefront projection — verified by a coverage simulation). For a
  directional sun that pixel is sky, which early-outs anyway.
- ABI-coupled: after any re-platform this mod must be rebuilt against the new
  `dusklight.lib`; the `GameService` major version rejects a mismatched load cleanly.
