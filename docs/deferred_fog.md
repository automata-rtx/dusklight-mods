# Deferred Fog

> **History:** this was folded into a combined "Graphics Hub" mod for a while, alongside a Depth to
> Normal provider. Graphics Hub is retired — GfxService 1.3's `get_scene_normals` gives every mod the
> game's authored normals directly, so the provider had nothing left to do — and Deferred Fog is a
> standalone mod again, which is what this document already described.

## The exported service is for ORDERING, not data

Deferred Fog exports `dev.automata.deferred_fog` (`include/deferred_fog_service.h`) with a single
`get_state` reporting whether the frame actually deferred. Consumers do not need the *data*; they
need the **import**, because the mod API has no priority field on a stage hook. Hooks run in
registration order, registration happens in `mod_initialize`, and the loader initializes in
dependency order — so importing a mod's service is the only way to say "initialize that one first".

Which matters only for one case. The fog quad draws at `FRAME_BEFORE_HUD`; the
`SCENE_AFTER_OPAQUE` hook merely arms it. So:

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

1. **Suppression scope** opens at `GFX_STAGE_SCENE_BEGIN` (the sky lists draw earlier and
   keep their fog) and closes at the first translucent list draw.
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
   translucent geometry (water included) rasterizes — with a `FRAME_BEFORE_HUD` fallback for
   frames with no translucent J3D at all. That lands after *every* mod's
   `SCENE_AFTER_OPAQUE` stage callbacks regardless of mod load order, and before water,
   particles, DOF, and bloom, which keep their native forward fog (the painter's dedicated
   particle-fog passes included). Do NOT anchor this on the painter's own list functions
   (`dDlst_list_c::drawXluDrawList` etc.): they inline into their callsites, so a detour
   fires at some unrelated later call — the original implementation did exactly that and the
   fog landed after bloom.
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
  from, which in the field is most of the distant scenery) draw themselves:
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
long way further on.** An open field view can genuinely contain no translucent J3D at all: the trees
are alpha-tested opaque, the grass and flowers are self-drawing packets (not `J3DShape`), the
particles are JPA. The game runs `FRAME_BEFORE_HUD` at `:2795`, which is after motion blur (`:2483`),
depth of field (`:2492`), every particle pass, and **bloom** (`:2663`). Fog applied after bloom is
fog the bloom never saw, so the bright distant subjects vanilla blooms hardest — Death Mountain, the
Ganon barrier — come out dimmer and sharper than vanilla. A **pre-hook on the bloom draw**
(`mDoGph_gInf_c::bloom_c::draw`, out-of-line and called unconditionally) is a much closer fallback,
and it only fires when the translucent anchor did not.

The Status line reports which anchor the frame used: `[at translucents]` is the good one,
`[before bloom]` is the fallback, `[AFTER BLOOM]` means even the fallback did not fire.

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
forward fog is simply fogged **twice**. Fixing that proper needs a per-pixel "no deferred fog" mark
in the ID buffer, not a suppression exemption. Do not reintroduce the exemption on its own.

## The Ganon barrier, and what its signature must NOT match

The Hyrule Castle barrier dome (`d_a_obj_ganonwall2`, `d_a_obj_ganonwall`) is translucent but
draws in the **opaque BG list**, so it lands inside the suppression scope. Both actors rewrite
their material fog to pure black with `mStartZ = 1000.0f`, `mEndZ = 250000.0f` every frame
(`d_a_obj_ganonwall2.cpp:112-116`). Deferring that config breaks two ways at once: the
config-ID replay rasterizes the dome **solid** and stamps its black fog onto the castle and
trees inside it, and the dome's own fog-then-blend compositing is lost. So the mod recognises
that one signature and leaves the barrier entirely on its vanilla forward fog — never
suppressed, never registered as a frame config — and in the replay lets it write no colour, so
its pixels keep the config of the castle and hills behind it. That is the same treatment every
blended draw now gets (see above); the signature survives as a second trigger in case a
material's blend state cannot be read.

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
used** — grass and flowers, which are almost all of the uncovered area. (Blended draws are *not*
in this category: they write no colour in the replay, so their pixels carry the config of whatever
is behind them rather than falling back.)

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
the config of the geometry behind it like every other blended draw.

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
  captured into a per-frame table (up to 8 distinct). A uniform frame takes the ordinary
  single-quad path at no extra cost. A mixed frame replays the opaque draw lists once into
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
    config; pixels not covered by the shape override (rare non-J3D direct drawers) decode
    as invalid and fall back to config 0 (the frame's reference) — exactly what the
    single-config quad applied to them before.
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

- The mod panel's **Status** line: "Deferring fog (exact: N draws, K configs)" is the
  working state in exact mode ("... replay failed" indicates the ID replay could not run
  and mixed pixels got the reference config); "REVERTED: mixed fog configs" appears only
  in Vanilla mode.
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
| `fogLogConfigs` | off | dump the frame's captured fog-config table to the log whenever it changes |

These are the names `register_var` is actually called with; an earlier revision of this table
listed `effectEnabled` / `mixedMode` / `debugView`, which the mod has never registered.
`exact_mode()`'s read fallback must stay equal to `fogMixedMode`'s registered default — the two
disagreed once, so a failed config read would have silently run the mode the UI was not showing.

The mods panel shows the **Enabled** toggle, a read-only **Status** line (see "Diagnosing
fog issues"), and an **Open Controls** button; `fogMixedMode` and `fogDebug` are SELECT
controls, which the UI only renders inside a window tab (not the flat panel), so they live in
the Open Controls window.

## Known caveats

- Translucents (water, particles) blend over the *fogged* opaque scene and then receive
  their own forward fog — matching vanilla layering.
- If the `J3DShape::drawFast` by-name hook cannot resolve (e.g. the game's embedded symbol
  manifest is unavailable), the mod loads but stays inert (vanilla fog) and logs a warning.
- Degenerate fog ranges (start == end) produce a zero fog term with a 0/0 singularity in
  vanilla; the deferred pass skips the quad entirely for those.
- ABI-coupled: rebuild against the new `windows-amd64.lib` import library after any re-platform.
