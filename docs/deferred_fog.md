# Deferred Fog

> **History:** this was folded into a combined "Graphics Hub" mod for a while, alongside a Depth to
> Normal provider. Graphics Hub is retired — GfxService 1.3's `get_scene_normals` gives every mod the
> game's authored normals directly, so the provider had nothing left to do — and Deferred Fog is a
> standalone mod again, which is what this document already described.

## STATUS — read this first if you are picking the mod up cold

**Confirmed in-game by the user:** the mod overall ("mostly been a success"), and specifically the
grass/flower uncovered-pixel fix — grass darkens with distance again.

**Shipped but NOT separately confirmed in-game** — do not describe these as verified: fog range
adjustment, the `dBgp_c` map-unit path, the exact-literal Ganon-barrier signature, Exact-as-default,
the quad-anchor readout, and the fog-off / additive counters. They are verified against the *game
source*, which is a different claim.

**ONE OPEN QUESTION, and it is unresolved.** Distant landmarks — the user reports Death Mountain
specifically, and the Ganon barrier — read **brighter with Deferred Fog OFF** than with it on, with
no other mods enabled. In the user's words: *"chunks of the far off Death Mountain geometry appear
to overpower the fog so you can see the light from the incredibly far distance"*, and the mod
*"seems to be drawing on top of Death Mountain's glow"*.

Two mechanisms are **established from the game source** (both documented in full below). Which one
is at work **cannot be settled from source**: the fetched `dusklight/` tree carries no stage
archives, so no material's authored `J3DBlendInfo` or `J3DFogInfo` is readable.

1. **The `K` factor** — a fullscreen pass cannot reproduce a pixel vanilla built from an
   over-unity blend. See "What a fullscreen pass can and cannot reproduce".
2. **Fog switched off per material** (`mType == 0`) — vanilla applies literally zero fog, the quad
   fogs it anyway. See "Geometry the game draws with no fog at all".

**Two fixes have already shipped for this and both were wrong.** They are recorded below so they
are not re-attempted: a *blended draws keep vanilla fog* exemption (measured worse in-game than not
having it, and it changed nothing about the symptom), and *the quad lands after bloom* (refuted by
the user — the view does contain translucent geometry, so the translucent anchor fires). Both were
reasoned from a plausible mechanism with no per-view measurement behind them. **Do not add a third
without reading the counters first.**

### The reading from the Death Mountain view — MECHANISM 2, measured

```
Deferring fog (exact: 75 draws, 2 configs; 0 shared-DL, 3 fog-off, 0 additive/0 no-Z) [not pushed]
```

- **`3 fog-off`** — mechanism 2 is present. Three materials in that view are drawn by the game with
  no fog at all, and the quad was fogging them.
- **`0 additive`** — **mechanism 1 is refuted for this view.** No material in the opaque scope uses
  an over-unity blend.
- **`0 shared-DL`** — Death Mountain is not reaching the screen through the `dBgp_c` map-unit path
  here, so none of that work bears on this.
- **`[not pushed]`** — **a bug in the diagnostic, not a real state.** The status string is built in
  `on_scene_after_opaque` and every anchor writes its value *after* that, so the line reported the
  value `on_scene_begin` had just cleared, in every frame. It now reports the **previous** frame's
  anchor. (If the quad genuinely had not been pushed the scene would be unfogged, and it is
  over-fogged, so this never described reality.)

The side-by-side screenshots agree independently: with the mod off the mountain shows **its own
orange and rock colours**, crisply; with it on the same geometry is washed to the haze colour. That
is the *silhouette* difference mechanism 2 predicts, not the *fog-coloured* difference mechanism 1
predicts. Two independent lines of evidence, same answer.

**So the fix is `fogSkipUnfogged`** — provided the mark can actually fire on those three materials,
which is what the `markable / no-Z / alpha` breakdown now reports.

### How to resolve it — the decision table

The Status line was built for exactly this. Stand in the view that looks wrong and read it.

| Reading | What it means | What to do |
| :-- | :-- | :-- |
| anchor is **not** `[at translucents]` | placement, not fog maths | stop and fix the anchor; see "Where in the frame the fog quad lands" |
| `fog-off` > 0, `markable` > 0 | mechanism 2 is present and the mark can fire | turn on **Skip Unfogged Geometry** and re-compare |
| `fog-off` > 0, `markable` == 0, `alpha` > 0 | the geometry is alpha-**tested**, so marking it would stamp its whole quad | the mark needs to carry the material's own alpha instead of forcing `GX_ALWAYS`; see "Geometry the game draws with no fog at all" |
| `fog-off` > 0, `markable` == 0, `no-Z` > 0 | the geometry does not own its depth | **stop.** Marking it would blank the fog on everything behind it |
| `additive` > 0, `no-Z` < `additive` | mechanism 1 is present and some of it owns its depth | the same sentinel can be extended to it — see "If the counters say this is the mechanism" |
| `additive` > 0, `no-Z` == `additive` | mechanism 1, but none of it owns its depth | **stop.** Marking those pixels would blank the fog on the terrain behind them. Say so rather than shipping it |
| both counters 0 | both mechanisms refuted for this view | start again; do not guess |

**Fog Factor** view discriminates the two directly: unfogged geometry is a *silhouette* difference
(the landmark shows its own texture through the haze); an over-unity blend is a *fog-coloured*
difference (the landmark shows **more haze than the haze**).

## The exported service is for ORDERING, not data

Deferred Fog exports `dev.automata.deferred_fog` (`include/deferred_fog_service.h`) with a single
`get_state` reporting whether the frame actually deferred. Consumers do not need the *data*; they
need the **import**, because the mod API has no priority field on a stage hook. Hooks run in
registration order, registration happens in `mod_initialize`, and the loader initializes in
dependency order — so importing a mod's service is the only way to say "initialize that one first".

Which matters only for one case. The `SCENE_AFTER_OPAQUE` hook only *arms* the quad; the quad
itself is pushed later — normally at the first translucent J3D packet, and at worst at
`FRAME_BEFORE_HUD` (see "Where in the frame the fog quad lands"). Either way it is after the stage.
So:

- A mod compositing at `SCENE_AFTER_OPAQUE` is **already** ordered before the fog by stage
  separation, and needs no import. That is the main path and the whole point of the mod.
- A mod that *also* draws at `FRAME_BEFORE_HUD` and wants to be **on top of** the fog — a debug
  overlay — would have to register after Deferred Fog, so it would have to import this.

**In practice, prefer `FRAME_AFTER_HUD` to the import.** That stage runs after everything including
the HUD, so an overlay lands on top of the fog with no dependency at all. VBAO used the import for
its debug views and now uses the later stage instead; nothing currently imports this service. Keep
it exported anyway — it is the only lever available if a future mod genuinely needs to interleave
*within* `FRAME_BEFORE_HUD`. Import it **optionally** if so: Deferred Fog is a separate install.


Mod id `dev.automata.deferred_fog`. Game-linked: hooks game/J3D functions, so it is coupled
to the pinned game build like the shadow mod. Standalone by design: **other mods need no
changes and no awareness of this mod to benefit** — anything composited over the opaque
scene at `SCENE_AFTER_OPAQUE` (Enhanced AO, Realtime Sun Shadows, third-party effects)
automatically ends up *under* the fog.

## The problem it solves

The game applies fog per fragment while drawing: every J3D material bakes fog BP state into
its material display list (`J3DGDSetFog`, written per frame by `d_kankyo`'s
`setLightTevColorType_MAJI_sub` from the environment palette), and a few direct drawers call
`GXSetFog`. Aurora replays those BP registers into per-draw fog state and applies
`fogF = clamp(a / (b − (1 − rawDepth)) − c)` + curve in its generated fragment shaders.
Any screen-space effect composited after the opaque scene therefore multiplies over
*already-fogged* color — AO and shadows visibly darken the fog / aerial perspective itself.

## Architecture

1. **Suppression scope** opens at `GFX_STAGE_SCENE_BEGIN` (`m_Do_graphic.cpp:2361`; the sky lists
   draw earlier and keep their fog) and closes at `GFX_STAGE_SCENE_AFTER_OPAQUE` (`:2426`), which
   the game runs one line before the first translucent list.
2. **J3D interception** (`J3DShape::drawFast` pre-hook, the same pattern as the shadow mod's
   two-sided casters): the material display list has already executed when a shape draws, so
   an immediate `GXSetFog(GX_FOG_NONE)` overrides its fog for that shape's geometry. The
   material's true parameters are read from `material->getPEBlock()->getFog()` for capture.
   Direct drawers are covered by a `GXSetFog` pre-hook that rewrites the type argument, and by
   an identical `GFSetFog` pre-hook: field/tall grass (`dGrass_packet_c`) and flowers
   (`dFlower_packet_c`) are self-drawing opaque packets that program per-room fog through
   `GFSetFog` (a direct BP write, not `GXSetFog`) and never call `J3DShape::drawFast`, so
   without that hook their fog escapes suppression and the deferred quad double-fogs them.
   `GFSetFog` has the same signature as `GXSetFog`, so the same callback captures and
   suppresses it (its only game call site is the grass/flower fog helper, so normal terrain is
   unaffected).
3. **Re-apply** as a fullscreen alpha-blended pass over the resolved opaque depth, pushed at
   the first `J3DShape::drawFast` after `SCENE_AFTER_OPAQUE` — i.e. right before the first
   translucent geometry (water included) rasterizes. That lands after *every* mod's
   `SCENE_AFTER_OPAQUE` stage callbacks regardless of mod load order, and before water,
   particles, DOF and bloom, which keep their native forward fog (the painter's dedicated
   particle-fog passes included). **This anchor is not a fixed point in the frame and the
   fallbacks behind it are materially worse** — the whole story, including which anchor a frame
   actually used, is in "Where in the frame the fog quad lands" below. Do NOT anchor this on the
   painter's own list functions (`dDlst_list_c::drawXluDrawList` etc.): they inline into their
   callsites, so a detour fires at some unrelated later call — the original implementation did
   exactly that and the fog landed after bloom.
4. **Exactness**: aurora's only per-fragment fog input is the raw depth value — the same
   value in the depth snapshot — and `src/fog_math.h` mirrors the full `J3DGDSetFog` BP
   encode → aurora command-processor decode round trip (11-bit mantissa truncation
   included), so the deferred pass reproduces forward fog bit-identically for opaque pixels
   (verified: coefficients match an independent transcription of both sides exactly, and the
   shader executed on llvmpipe matches a CPU reference of aurora's formula to 0 ULP across
   all five fog curves).
5. **Fog range adjustment** ("XFog" in the game's own naming) is reproduced too. GX fog is
   driven by Z — the distance along the view axis — but a pixel at the screen edge is
   genuinely further from the eye than a centre pixel at the same Z, and the hardware
   corrects for it with a per-column multiplier applied to `a / (b − z)` *before* `c` is
   subtracted. TP turns it on at environment init and never clears it (`envcolor_init`,
   `d_kankyo.cpp:1257`: `mFogAdjEnable = true`, table 0, centre `0x140`), and every fog
   setter re-arms it — `GxXFog_set()` after each of the three direct setters, and each BG
   material's `J3DFog` block, stamped from the same globals, re-issues it on load. Aurora
   implements it (`build_fog_range_lut` bakes one multiplier per target column;
   `fogBase *= lut[u32(in.pos.x)]`), so it is part of what vanilla renders. `res/fog.wgsl`
   evaluates the identical function analytically per pixel (`fog_range_factor`) from the
   same constants, in the same pair-swapped order, at the same 1/64 scale. Omitting it cost
   a horizontal gradient the game has: `(lut − 1)·(c + fogF)`, ~3% of the fog term at the
   screen edges, which is a fraction of a point for near fog but reaches double digits for
   the narrow, far-*starting* bands distant haze uses, where `c = startZ/(endZ − startZ)` is
   large. **An earlier revision of this document claimed aurora ignored range adjustment and
   that the mod therefore correctly ignored it too. That was false, and it is why nobody
   looked.**

## Draw paths that do not go through `J3DShape::drawFast`

The `drawFast` pre-hook covers the mainstream J3D packet path, but not everything draws that
way, and a path it misses keeps its forward fog *and* receives the deferred quad — double
fog, worst exactly where the fog term is largest.

- **Grass / flowers** program fog through `GFSetFog`; that hook covers them.
- **Map units** (`dBgp_c`, "bg parts" — the shared, instanced pieces a stage is assembled
  from) draw themselves:
  `dBgp_c::modelMaterial_c::drawSimple` (`d_bg_parts.cpp:20`) calls
  `mpMaterial->loadSharedDL()` and then walks the shape's matrix groups calling
  `J3DShapeDraw::draw()` directly. The packet-level fog it sets first *is* caught (that is a
  real `dKy_GxFog_tevstr_set` → `GXSetFog`), but the material display list replayed by
  `loadSharedDL` re-issues `J3DGDSetFog` from the material's own fog block afterwards, and
  nothing hooked runs in between. In exact mode the same gap meant the replay never stamped
  that geometry, so it rasterized real lit colours, decoded as "uncovered", and fell through
  to the fallback config instead of its own. A **post-hook on `loadSharedDL`** (all three
  material-class overrides) lands exactly between the display list and the shapes and closes
  both.

  It is **scoped to `dBgp_c`** by a bracketing hook on `drawSimple`, and that is required,
  not caution: every other `loadSharedDL` caller (`d_model.cpp:22`, `d_particle.cpp:593`, the
  fchain / wchain / hookshot chain shapes) calls `dKy_GxFog_tevstr_set` *immediately after*
  loading the display list, so the material's own fog is overwritten before a triangle
  rasterizes and never renders at all. Registering it would put a config in the frame table
  that vanilla never draws with — enough to make a uniform scene read as "mixed", which in
  Vanilla mode reverts the whole scene to forward fog. `dBgp_c` is the one caller that sets
  its fog *before* the material loop, so there, and only there, the display list has the last
  word. The Status line reports how many shared-DL materials carried live fog in the frame.

## Where in the frame the fog quad lands — and why it matters

The quad wants to go in **immediately after every mod's `SCENE_AFTER_OPAQUE` composite and before
the translucent lists**. The game runs that stage hook at `m_Do_graphic.cpp:2426`, one line before
`dComIfGd_drawXluListBG`. There is no stage hook at that exact point and every list entry point on
the way in is `inline` (`dComIfGd_drawXluListBG` → `dDlst_list_c::drawXluListBG` →
`drawXluDrawList`), so nothing there can be hooked by symbol. The mod therefore anchors on the
**first `J3DShape::drawFast` after the stage closes** — the first translucent J3D packet. When a
frame has one, that is exactly the right place.

**When a frame has none, the anchor used to fall all the way to `FRAME_BEFORE_HUD`, and that is a
long way further on.** That such a frame exists is an **assumption, not a measurement**: an open
field view plausibly contains no translucent J3D — the trees are alpha-tested opaque, the grass and
flowers are self-drawing packets (not `J3DShape`), the particles are JPA — but nothing in the source
proves it. The Status line's anchor readout is what tests it; if it never reads anything but
`[at translucents]`, this fallback is dead code and the brightness difference is something else.
The game runs `FRAME_BEFORE_HUD` at `:2795`, which is after every particle pass
and — the one that matters — **bloom** (`:2663`). Fog applied after bloom is fog the bloom never
saw, so the bright distant subjects vanilla blooms hardest — Death Mountain, the Ganon barrier —
come out dimmer and sharper than vanilla. A **pre-hook on the bloom draw**
(`mDoGph_gInf_c::bloom_c::draw`, out-of-line and called unconditionally) is a much closer fallback,
and it only fires when the translucent anchor did not.

Bloom is the load-bearing pass here because it is **on by default**: `bloomMode` defaults to
`BloomMode::Dusk` (`dusk/settings.cpp:68`) and `bloom_c::draw2` runs whenever the area's own bloom
is enabled. The other two full-screen passes in that window are not comparable and should not be
cited alongside it — `drawDepth2` (`:2492`) is depth of field, gated on auto-focus, and
`motionBlure` (`:2483`) is **not motion blur**: it blends the *previous* frame's captured
framebuffer over the current one at `getBlureRate()` alpha, gated on `g_env_light.is_blure`, which
ordinary play leaves off. (An earlier revision of this document listed both as things the fog lands
after. That was reading a symbol name as a description — the exact failure mode
`docs/japanese-naming.md` exists to prevent.)

The Status line reports which anchor the frame used: `[at translucents]` is the good one,
`[before bloom]` is the fallback, `[AFTER BLOOM]` means even the fallback did not fire.

**Read the anchor before diagnosing anything else.** If it is not `[at translucents]`, the
explanation for a fog difference is anchor placement, not fog maths.

Alongside it the Status line carries per-frame measurements, which exist because this mod has
already absorbed two fixes built on a plausible mechanism with no per-view measurement behind them:

| Field | Means |
|---|---|
| `N shared-DL` | materials drawn through `loadSharedDL` (the `dBgp_c` map-unit path) that carried live fog |
| `N fog-off (M markable/Z no-Z/T alpha)` | materials in scope that vanilla draws with **no fog at all** (`mType == 0`), and of those how many `fogSkipUnfogged` can actually mark, how many are rejected for not owning their depth, and how many for being alpha-**tested**. `markable = fog-off − no-Z − alpha` |
| `N additive/M no-Z` | materials whose blend makes `K ≠ 1` (see below), and how many of those do not write depth |
| `[anchor]` | which anchor the quad used **on the previous frame** — see the note under `g_lastQuadAnchor`; it cannot be the current frame's, because the status string is built before any anchor fires |

## What a fullscreen pass can and cannot reproduce — the `K` factor

This is the arithmetic the whole "distant landmark looks dimmer" question turns on, and it is worth
having exactly rather than approximately.

**Aurora fogs the fragment source, inside the fragment shader, before the hardware blend.**
`shader.cpp:1579` emits `prev = mix(prev.rgb, fog.color.rgb, fogZ)` into the *fragment function*,
while the GX blend equation is a WebGPU pipeline blend state applied afterwards
(`gx.cpp:332-338`). The deferred pass instead fogs the already-composited framebuffer. For layers
drawn with GX factors `(sᵢ, dᵢ)` over an accumulator, with `F` the fog colour and `f` the fog factor:

```
vanilla:   Cᵢ = sᵢ·mix(xᵢ, F, f) + dᵢ·Cᵢ₋₁
deferred:  Dᵢ = sᵢ·xᵢ            + dᵢ·Dᵢ₋₁ ,  then  out = mix(Dₙ, F, f)

⇒ divergence = f · F · (K − 1),   K = Σᵢ ( sᵢ · Π_{j>i} dⱼ )
```

- **Ordinary alpha blend** (`s = α`, `d = 1 − α`) over an opaque base: `K = α + (1 − α) = 1`.
  **Divergence zero — the deferred result is bit-exact.** This is why the blanket "blended draws
  keep vanilla fog" experiment measured worse: it exempted a pile of draws that were *already
  exact*, and got them fogged twice for the trouble.
- **Additive** (`d = GX_BL_ONE`): `K = 1 + α`, so vanilla is brighter by `α·f·F` — a whole extra
  dose of fog colour. `GX_BM_SUBTRACT` likewise maps to ReverseSubtract One/One (`gx.cpp:129-136`).

And the part that matters at extreme distance: **`mix(scene, F, f)` is bounded by `max(scene, F)`
and converges to exactly `F` as `f → 1`, while vanilla converges to `K·F`.** So the quad clamps
every far pixel to precisely the haze colour, while vanilla can be a *multiple* of it. That is what
"chunks of the far-off geometry overpower the fog" looks like in arithmetic, and it is why a single
fullscreen pass cannot reproduce such a pixel in principle.

Note the narrow test this implies: the question is **not** "is this draw blended", it is "is the
destination factor something other than `1 − src`". `material_over_unity_blend()` tests exactly
that, and the Status line's `additive` counter is how many such materials a frame contains.

### If the counters say this is the mechanism

Two honest options, neither yet implemented. Gate both on `additive > 0` **and**
`no-Z < additive`; if every over-unity material has depth-write off, marking its pixels would blank
the fog on the terrain behind it and the correct answer is "cannot be done with one pass", not
"not done yet".

- **(a) Hand those pixels back to vanilla entirely** — do not suppress the material's fog, *and*
  stamp the no-deferred-fog sentinel so the quad skips them. This is **exact** when what lies behind
  the surface is sky (which the quad never fogs), and wrong by the background's own fog term when
  the surface sits over deferred-fogged world geometry. For a distant silhouette against sky the
  error is zero, which is the case that prompted this.
- **(b) Carry the extra dose** — stamp `Σα` into a second channel and add `k·f·F` in a second quad
  draw. Correct in general, but it needs a second replay pass (GX has one blend state per draw, and
  the flat-ID override throws α away) and a second pipeline. Do not build this speculatively.

## Geometry the game draws with no fog at all

Separately from blending, a material's fog block can simply be **off**. `J3DFog::load()` issues
`J3DGDSetFog(GXFogType(mType), …)` unconditionally (`J3DMatBlock.h:1525`), so `mType == 0` programs
a real `GX_FOG_NONE`; and `setLightTevColorType_MAJI_sub` refuses to overwrite such a block — the
whole fog section is guarded by `if (fog_info->mType != 0)` (`d_kankyo.cpp:4434-4487`). It is an
artist-facing per-material opt-out that TP honours, and it is how a distant landmark can stay at
full brightness however far away it is.

It is invisible to this mod's `GXSetFog` / `GFSetFog` hooks, because `J3DGDSetFog` writes raw BP
commands into the FIFO (`J3DGD.cpp:581-585`) rather than calling GX. The quad, meanwhile, fogs
**every** pixel whose depth is greater than zero. So such geometry gets fog the game never applied.

The **Skip Unfogged Geometry** toggle (`fogSkipUnfogged`, default **off**) marks those pixels in the
config-ID buffer with a sentinel the shader leaves alone. Three restrictions, each of which is a
failure mode if dropped:

- **Only a material that owns its depth may be marked** (`getCompareEnable() && getUpdateEnable()`).
  The quad's fog factor comes from the depth buffer, so only the draw that owns the depth at a pixel
  may decide that pixel's fog. A fog-off overlay that does not write depth sits over geometry whose
  depth is what the quad reads; marking it would blank the fog on everything behind it.
- **Only a material whose alpha test passes everything may be marked.** `stamp_replay_id` forces
  `GXSetAlphaCompare(GX_ALWAYS, …)` and binds no texture, so an alpha-*tested* material stamps its
  whole quad rather than its cutout. Harmless today (every stamped ID is the same room fog); with a
  differing mark it would punch a rectangular hole in the fog.
- **`mType == 0` is kept distinct from "no fog block at all".** In retail TP the latter cannot
  happen — `J3DMaterial::createPEBlock` only picks the fog-less Opa/TexEdge/Xlu blocks when its
  flags argument is 0 (`J3DMaterial.cpp:68-83`) and every retail model load masks to `0x10000000`
  (`d_resorce.cpp:228`/`:378`/`:412`/`:437`, `d_file_select.cpp:5948`, `d_menu_collect.cpp:2805`) —
  but keeping them apart means a future change makes the mark stop firing rather than fire on
  geometry that does have fog.

Turning it on **forces the config-ID replay** in frames that were uniform before, which is a real
framerate cost. The Status line's `fog-off` counter says in advance whether it would do anything,
and its `markable / no-Z / alpha` breakdown says whether the two restrictions above let it fire.

If the blocker turns out to be `alpha`, the way out is to stop `stamp_replay_id` forcing
`GXSetAlphaCompare(GX_ALWAYS, …)` for the sentinel and instead carry the material's own alpha —
bind its texture and take TEV alpha from `GX_CA_TEXA`, leaving the alpha compare the material's
display list already set. That is untested and assumes the material samples `GX_TEXMAP0` /
`GX_TEXCOORD0`, which is usual for a cutout but not guaranteed; do not build it until the counter
says it is needed.

## Blended draws: why there is no "keep vanilla fog" rule

GX fog runs per fragment **before** the blend, so for a see-through surface vanilla computes
`blend(mix(src, fogCol, f), dst)` while a fullscreen pass can only compute
`mix(blend(src, dst), fogCol, f)`. That reasoning is correct, and TP really does draw see-through
surfaces inside the opaque lists:

- **The Hyrule Castle barrier.** `d_a_obj_ganonwall2::Draw` rewrites every material's fog to pure
  black over `startZ 1000` / `endZ 250000` every frame, then enters the model with
  `dComIfGd_setListBG()`.
- **Water.** `dKy_bg_MAxx_proc` stamps `mType = 7` on `MA03`/`MA17`/`MA19` (`d_kankyo.cpp:11390`)
  and on `MA20` (`:11588`), and `mType = 6` on `MA09` (`:11381`), which
  `setLightTevColorType_MAJI_sub` turns into pure **black** and pure **white** fog. The same pass
  moves them into the DarkBG opaque list (`:11371`).

A rule that detected blended materials and left them all on vanilla forward fog was tried and
**measured worse in-game than not having it**, and it changed nothing about the Death Mountain /
barrier difference it was meant to explain. It backfires because the deferred quad still fogs those
pixels — they are in the depth buffer like anything else — so a blended surface that keeps its
forward fog is simply fogged **twice**. Fixing it properly needs a per-pixel "no deferred fog" mark
in the ID buffer, not a suppression exemption. **That mark now exists** — see "Geometry the game
draws with no fog at all" — but it is keyed on `mType == 0`, *not* on blending, and extending it to
over-unity blends is option (a) under the `K` factor, gated on the `additive`/`no-Z` counters. Do
not reintroduce the suppression exemption on its own.

## The Ganon barrier, and what its signature must NOT match

The Hyrule Castle barrier dome (`d_a_obj_ganonwall2`, `d_a_obj_ganonwall`) is translucent but
draws in the **opaque BG list**, so it lands inside the suppression scope. Both actors rewrite
their material fog to pure black with `mStartZ = 1000.0f`, `mEndZ = 250000.0f` every frame
(`d_a_obj_ganonwall2.cpp:112-116`). Deferring that config breaks two ways at once: the
config-ID replay rasterizes the dome **solid** and stamps its black fog onto the castle and
trees inside it, and the dome's own fog-then-blend compositing is lost. So the mod recognises
that one signature and leaves the barrier entirely on its vanilla forward fog — never
suppressed, never registered as a frame config — and in the replay lets it write no colour, so
its pixels keep the config of the castle and hills behind it. **The barrier is the only draw that
gets this treatment.** A rule that generalised it to every blended draw was tried and reverted (see
"Blended draws" above); the barrier keeps the exemption because its own fog is pure black over a
1000..250000 range and putting *that* in the config table is worse than the double-fog residual.

The match is the **exact literal triple**. It used to be `black && endZ > 100000`, which
matched a whole material class rather than an actor: `mType = 7` is the game's own black-fog
sentinel, stamped by `dKy_bg_MAxx_proc` on the terrain **water** family `MA03`/`MA17`/`MA19`
(`d_kankyo.cpp:11390`) and on `MA20` (`:11588`). `setLightTevColorType_MAJI_sub` reads that
sentinel and forces the fog **colour** to pure black (`:4466-4470`) while the start/end Z stay
the room palette's — and per `docs/japanese-naming.md` §4.5 the polygon-code pass runs *after*
the translation and re-stamps the type, so what reaches the GPU is black fog over the palette's
range. In any room whose palette `fog_end_z` exceeded 100000, every water surface and every
`MA20` material therefore looked like the barrier: left on forward fog **and** painted over by
the quad — double fog. Palette fog is interpolated by `float_kankyo_color_ratio_set`
and cannot land on both literals, so the exact test cannot collide with it.

## What an uncovered pixel falls back to

Pixels the ID replay cannot stamp fall back to **the config the self-drawing opaque packets
used** — grass and flowers, which are almost all of the uncovered area. (The Ganon barrier is *not*
in this category: it writes no colour in the replay, so its pixels carry the config of whatever
is behind it rather than falling back — it is the only draw treated that way.)

Field/tall grass (`dGrass_packet_c`) and flowers (`dFlower_packet_c`) do not draw through J3D at
all: they set the room's fog, replay their own static material display lists, and emit raw GX
batches. The replay's per-draw flat-ID override cannot reach them — the material list re-programs
TEV after anything we could set, and the geometry is not a `J3DShape` — so they rasterize real
lit colours into the ID buffer, the shader's green/blue guard correctly rejects those, and every
grass and flower pixel lands on the fallback by construction. Bracketing those two packet draws
and recording the config their own fog setter resolved to makes the fallback the config they
actually drew with, rather than a guess. If the bracket hooks do not resolve it degrades to
config 0 — the frame's reference config, which is that same room fog in an ordinary frame.

**This briefly ranked by the widest `endZ` instead, and that was a regression** — the config with
the largest `endZ` is the one with the *weakest* fog at any given depth, so grass and flowers
stopped darkening with distance and read as if they were lit right next to the camera, most
obviously in heavy fog. That helper is gone entirely: its other caller, the barrier dome, now gets
the config of the geometry behind it instead of a ranking.

Before that it ranked by the projection **far plane**, modelling a "distant scenery fog drawn
with a wider projection" that TP does not have: every fog config in a frame is stamped with the
same near/far, read from the one live view (`d_kankyo.cpp:4461-4463` for every BG material,
`:9394`/`:9429`/`:9451` for the three direct setters), the world lists draw under a single
perspective projection (`m_Do_graphic.cpp:2338`), and nothing sets another one inside the
suppression window. So the strict `farZ >` scan from index 0 never fired and the function was a
long way of writing `return 0`. What TP actually widens for distant scenery is the CPU
**clipper** (`d_a_bg.cpp:298` `changeFar(1000000)`, `d_bg_parts.cpp:681` `changeFar(100000)`),
decided at list-build time by `hide()`/`show()` — which the replay reproduces on its own and
which never touches fog.

## Mixed fog configurations

Fog configurations are compared with tolerances that absorb per-room palette-blend
differences (Δcolor ≤ 6, Δstart/end ≤ 2% of the fog span); anything beyond that is a
distinct configuration. Many areas mix several (rooms lagging the stage's palette blend,
special-fog materials), and the two modes handle that differently (`mixedMode`):

- **Exact (replay), the default**: every fogged draw is suppressed and its configuration
  captured into a per-frame table (up to 8 distinct; slot 8 / red byte 216 is reserved as the
  no-deferred-fog sentinel and can never be a config). A uniform frame takes the ordinary
  single-quad path at no extra cost — *unless* `fogSkipUnfogged` is on and the frame contains
  fog-off geometry, which forces the replay. Both gates live in one `needs_id_buffer()`. A frame
  that needs the buffer replays the opaque draw lists once into
  a mod-owned offscreen pass — same save-replay-resolve bracket as the shadow mod's
  cascades, but with the game's own camera — with each shape's output forced to a flat
  sparse-encoded index color (`(index+1)·24` in red; the material display list has already
  run, so a per-shape TEV/channel override sets it). The resolved buffer selects each
  pixel's exact fog configuration in `fs_mixed`. Caveats:
  - One extra opaque scene of vertex/index streaming per mixed frame, which shares aurora's
    fixed per-frame buffers with the shadow cascade replays. At upstream's current sizes this
    is a framerate cost rather than the overflow risk it once was — see the shadow doc's
    budget section, which is now historical.
  - Alpha-tested cutouts (foliage holes) replay solid, so a hole resolves to its tree's
    config; pixels not covered by the shape override decode as invalid and fall back to the
    config the self-drawing packets used (see "What an uncovered pixel falls back to"). The solid
    replay of a cutout is also why the no-deferred-fog mark refuses to touch an alpha-tested
    material.
  - Grass/flower/waterfall packets draw their own geometry (not `J3DShape::drawFast`), so the
    replay can't force them to a flat ID color — they rasterize real lit colors into the ID
    buffer. The override writes the ID as `(id, 0, 0)` (red only), so the shader rejects any
    pixel with non-zero green/blue — i.e. all such packets, in any lighting — back to the
    fallback, which is recorded from those packets' own fog setter (see "What an uncovered pixel
    falls back to"), so it is the config they actually drew with. (Before the green/blue guard,
    a blade's shaded red channel could land in another config's decode window and flicker
    between configs as the lighting changed — the day/night grass artifact.) Residual: a
    genuinely pure-red bypassed surface could still alias, but none occurs in practice.
  - MSAA silhouettes may resolve to an invalid ID on 1-px fringes → reference config.
- **Vanilla**: the original behavior — only draws matching the frame's reference config
  are suppressed; any deviant reverts the scene to forward fog from the next frame until
  it is uniform again. Twilight black fog (type 7 → linear black), wolf-senses white fog
  (type 6), and room transitions all take the vanilla path in this mode.

A scene that uses a special configuration *uniformly* (all draws agree) is deferred
normally in both modes — all five GX fog curves (LIN/EXP/EXP2/REVEXP/REVEXP2) are
implemented in `res/fog.wgsl`, and in exact mode a mixed twilight scene simply carries the
special config in its table like any other.

## Diagnosing fog issues

The symptom of fog NOT being deferred is distinctive: screen-space AO/shadows darken the
fog itself at range (unnatural darkening on distant fog-washed terrain). Tools:

- The mod panel's **Status** line. In exact mode it reads

  ```
  Deferring fog (exact: N draws, K configs; A shared-DL,
                 B fog-off (M markable/Z no-Z/T alpha), C additive/D no-Z) [anchor]
  ```

  `"... replay failed"` means the ID replay could not run and mixed pixels got the fallback
  config; `"REVERTED: mixed fog configs"` appears only in Vanilla mode. The five measurements are
  the point — see the decision table in **STATUS** at the top of this document, and the field
  table under "Where in the frame the fog quad lands".
- Transitions are **logged**: mixed↔uniform in exact mode, revert/re-engage (with both
  configs' type/range/color) in Vanilla mode.
- Debug views: **Fog Factor** (the deferred term as grayscale — black while the scene is
  visibly foggy means no quad ran) and **Config IDs** (exact mode, mixed frames: one gray
  band per captured config, showing exactly which geometry resolved to which fog).

## Tunables

| Var | Default | Meaning |
|---|---|---|
| `fogEnabled` | on | master toggle (off = vanilla forward fog) |
| `fogMixedMode` | 1 (Exact) | mixed-scene handling: 0 = Vanilla (revert to forward fog), 1 = Exact (per-pixel config-ID replay). Exact is the default because most outdoor scenes mix configs, and Vanilla hands those back to forward fog — which is exact, but puts AO on top of the fog again in precisely the scenes the mod exists for |
| `fogDebug` | 0 | 1 = deferred fog factor as grayscale, 2 = config IDs (exact mode, mixed frames) |
| `fogSkipUnfogged` | off | mark pixels the game drew with fog switched off so the quad leaves them alone (see "Geometry the game draws with no fog at all"); forces the ID replay, and needs `fogMixedMode` = Exact to do anything |
| `fogLogConfigs` | off | dump the frame's captured fog-config table to the log whenever it changes |

These are the names `register_var` is actually called with; an earlier revision of this table
listed `effectEnabled` / `mixedMode` / `debugView`, which the mod has never registered.
`exact_mode()`'s read fallback must stay equal to `fogMixedMode`'s registered default — the two
disagreed once, so a failed config read would have silently run the mode the UI was not showing.

The mods panel shows the **Enabled** toggle, a read-only **Status** line (see "Diagnosing
fog issues"), and an **Open Controls** button; `fogMixedMode` and `fogDebug` are SELECT
controls, which the UI only renders inside a window tab (not the flat panel), so they live in
the Open Controls window, along with `fogSkipUnfogged` and `fogLogConfigs`.

## Known caveats

- Translucents (water, particles) blend over the *fogged* opaque scene and then receive
  their own forward fog — matching vanilla layering. That holds while the quad anchors
  `[at translucents]`; on either fallback anchor they blend over an *unfogged* scene and are then
  fogged a second time by the quad.
- If the `J3DShape::drawFast` by-name hook cannot resolve (e.g. the game's embedded symbol
  manifest is unavailable), the mod loads but stays inert (vanilla fog) and logs a warning. Every
  other hook degrades to a coverage loss and warns: the `dBgp_c` bracket (`drawSimple` +
  `loadSharedDL` × 3) leaves map-unit material fog uninspected, the grass/flower packet bracket
  leaves the uncovered-pixel fallback at config 0, and the bloom pre-hook leaves the last-resort
  anchor at `FRAME_BEFORE_HUD`.
- Degenerate fog ranges (start == end) produce a zero fog term with a 0/0 singularity in
  vanilla; the deferred pass skips the quad entirely for those.
- ABI-coupled: rebuild against the new `windows-amd64.lib` import library after any re-platform.
