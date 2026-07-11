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
   hardware-bilinear comparison taps, add screen-space contact shadows (short raymarch),
   darken the scene. The composite runs right after the opaque scene — before translucency
   and, critically, before the game's bloom filter (`m_Do_graphic.cpp` draws bloom between
   `SCENE_AFTER_OPAQUE` and `FRAME_BEFORE_HUD`; compositing at `FRAME_BEFORE_HUD` darkened
   the bloom glow itself). Debug views visualize map/coverage/factors and still draw at
   `FRAME_BEFORE_HUD` so nothing the scene layers on afterwards obscures them.
4. **Game-shadow suppression**: pre-hooks skip `dDlst_shadowControl_c::imageDraw/draw` and
   `drawCloudShadow` while the mod is active (typed hooks only — no symbol manifest needed).
5. **Indoor auto-disable**: `dKy_Indoor_check() != 0` (+ `indoorDisable` on) gates both map
   rendering and compositing — interiors revert to the vanilla look.

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
| `mapSize` | 2 | shadow map: 0=1024 1=2048 2=4096 3=8192 |
| `boxRadius` | 8000 | coverage radius in world units (1000–30000) |
| `strength` | 45 | shadow darkening % |
| `bias` | 55 | constant depth bias (normalized against light range) |
| `slopeBias` | 30 | bias added ∝ surface slope vs light |
| `normalOffset` | 100 | receiver offset, % of one shadow texel's world size |
| `pcf` | 2 | PCF kernel: 0=1×1 1=3×3 2=5×5 ... |
| `contactShadows` | on | screen-space contact raymarch |
| `contactThickness` / `contactLength` | 25 / 60 | contact ray assumptions |
| `noFrustumClipping` | on | the anti-popping clipper bypass (issue 5) |
| `twoSidedCasters` | on | render casters with backface culling off (issue 6) |
| `indoorDisable` | on | vanilla look indoors (issue 3) |
| `debugView` | 0 | map/coverage/factor visualizations |

Tuning order for acne: raise `slopeBias` first, then `normalOffset`; lower `bias` if shadows
detach at feet (contact shadows hide small gaps).

## Known caveats

- **Midna**: the game's projected blob shadow (which the mod hooks out) is where Midna
  "lives" during her summon/emergence animation. A retain path (re-enable the game shadow
  for Link only, or anchor her to our sun ground-projection) is a known follow-up.
- Single map: very large `boxRadius` spreads texels thin → cascades are the planned fix.
- ABI-coupled: after any re-platform this mod must be rebuilt against the new
  `dusklight.lib`; the `GameService` major version rejects a mismatched load cleanly.
