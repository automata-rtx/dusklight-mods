# Deferred Fog

Moves the game's fog to after the opaque world has been drawn, so screen-space effects such as AO
darken the surfaces *under* the fog instead of darkening the fog itself.

| | |
| :-- | :-- |
| Mod id | `dev.automata.deferred_fog` (`mods/deferred_fog/`) |
| Version | see `mods/deferred_fog/mod.json` (1.0.2 at the time of writing) |
| Kind | **Game-linked**: includes game headers, calls game functions, and hooks ten game functions. It must be built against the game build it runs on |
| Status | Released and in use on Dusklight `v2.0.0`. One open issue: [some distant landmarks are brighter with the mod off](#distant-landmarks-brighter-with-the-mod-off) |

**Why it exists.** The game fogs every draw as it renders it. A mod that composites after the
opaque world (VBAO at `SCENE_AFTER_OPAQUE`) therefore multiplies over pixels that are already
fogged, and distant AO reads as grime floating on the haze. This mod stops the game fogging the
opaque world lists and re-applies the same fog afterwards as one fullscreen pass, after every mod's
`SCENE_AFTER_OPAQUE` composite and before the translucent geometry.

No other mod depends on it, and it depends on none. The ordering comes from the frame's stages.

## Files

| File | Contents |
| :-- | :-- |
| `src/mod.cpp` | Everything host-side: hooks, fog capture and suppression, the config table, the config-ID replay, the fog quad, options, UI, the exported service |
| `src/fog_math.h` | `compute_fog_coefficients`: the game's fog parameters → the `a, b, c` coefficients exactly as the renderer decodes them |
| `res/fog.wgsl` | The fullscreen fog pass: `fs_main` (one config), `fs_mixed` (per-pixel config), and both debug views |
| `include/deferred_fog_service.h` | The exported `dev.automata.deferred_fog` service |
| `../../docs/deferred_fog_underwater_notes.md` | A designed but unbuilt feature (fading AO on submerged terrain) |

Key functions in `mod.cpp`: `on_scene_begin`, `on_set_fog_pre`, `on_shape_draw_pre`,
`on_material_shared_dl_post`, `on_scene_after_opaque`, `replay_config_ids`, `push_fog_quad`,
`on_draw`, `ensure_fog_pipelines`, `wolf_senses_active`.

## Using it

### Options

The mod's pane has **Enabled**, a read-only **Status** line, and an **Open Fog Controls** button.
The window repeats Enabled and holds everything else.

| Config key | UI label | Default | Meaning |
| :-- | :-- | :-- | :-- |
| `fogEnabled` | Enabled | on | Master switch. Off = the game's own fog |
| `fogMixedMode` | Mixed Scenes | 1 (Exact) | How a frame that uses several fog configurations is handled. **Exact (replay)**: always defer, with a per-pixel record of which configuration each pixel used (one extra opaque geometry pass on such frames). **Vanilla**: hand those frames back to the game's fog. Most outdoor scenes mix configurations, so Vanilla gives up the benefit in exactly the scenes that need it |
| `fogSkipUnfogged` | Skip Unfogged Geometry (experimental) | off | Leave unfogged the pixels of materials the game draws with fog switched off. Forces the replay. Exact mode only. See [Limitations](#limitations) |
| `fogDeferInSenses` | Defer Fog During Wolf Senses (diagnostic) | off | Keep deferring while Wolf Link's senses are active. Brings back a known bug; for measurement only. See [Wolf Senses](#wolf-senses) |
| `fogDebug` | Debug View | 0 | 0 off, 1 Fog Factor, 2 Config IDs. See below |
| `fogLogConfigs` | Log Fog Configs | off | Log the frame's fog-configuration table whenever it changes |

### Status line

The Status line is rebuilt every frame (in `on_scene_after_opaque`). In Exact mode it reads:

```
Deferring fog (exact: N draws, K configs; A shared-DL, B fog-off (M markable/Z no-Z/T alpha), C additive/D no-Z) [anchor]
```

| Field | Meaning |
| :-- | :-- |
| `N draws` | Fogged draws captured this frame (each `drawFast`, map-unit material and direct fog setter counts once) |
| `K configs` | Distinct fog configurations this frame. More than 1 means the replay runs |
| `, replay failed` | Appears if the replay could not run; the whole frame then used config 0 |
| `A shared-DL` | Map-unit (`dBgp_c`) material draws that carried live fog |
| `B fog-off (M markable / Z no-Z / T alpha)` | Draws whose material has fog switched off; of those, how many `fogSkipUnfogged` can mark, and how many it must skip because they write no depth or are alpha-tested |
| `C additive / D no-Z` | Draws with an over-unity blend (see [Limitations](#limitations)), and how many of those write no depth |
| `[anchor]` | Where the fog quad landed **on the previous frame**: `at translucents` (normal), `before bloom`, `AFTER BLOOM`, or `not pushed` |

Counts are per draw, not per unique material, and only cover J3D materials reached through the
hooks (not particles, the game's own shadows, or self-drawing packets).

Other states: `Wolf Senses: fog left to the game ...`, `No fogged draws this frame`, `REVERTED:
mixed fog configs (...)` (Vanilla mode), and a Vanilla-mode variant of the line above without
`exact:`. The `Disabled` state is never shown; with the mod disabled the line keeps its last text
(see [Code issues](#code-issues)).

### Debug views

Both replace the frame with an opaque grayscale image drawn by the fog pass itself. Sky pixels
(depth 0) are black.

| `fogDebug` | View | Shows |
| :-- | :-- | :-- |
| 1 | Fog Factor | The fog amount per pixel (white = fully fogged) |
| 2 | Config IDs | One gray level per fog configuration, on frames that use more than one. On single-configuration frames it shows Fog Factor instead |

In both, **red** pixels are ones deliberately left unfogged (the `fogSkipUnfogged` mark). If the
view shows the normal scene, the fog pass did not run this frame. An all-black view means it ran
and computed zero fog.

### Log messages

`deferred_fog ready` on load; Wolf Senses on/off transitions; `scene went mixed (N configs)` /
`scene uniform again` (Exact mode); `deferred fog REVERTED to vanilla: ...` (Vanilla mode, with both
configurations); `deferred fog engaged`; one-time warnings `config-ID replay failed ...` and
`depth resolve failed; fog lost this frame`; and the `fogLogConfigs` table dump. Hooks that fail to
attach at load each produce a warning naming what is lost.

## How it works

### Where it acts in the frame

Line numbers are in `dusklight/src/m_Do/m_Do_graphic.cpp` at the `v2.0.0` pin.

| Line | Game | This mod |
| :-- | :-- | :-- |
| 2311 | World projection set | |
| 2328 | Sky lists | Untouched; sky keeps the game's fog |
| 2334 | `GFX_STAGE_SCENE_BEGIN` | `on_scene_begin`: reset the frame's state and open the suppression scope (unless disabled or in Wolf Senses) |
| 2344–2390 | Opaque world lists, plus `Pri0_B` particles and the game's shadows | Capture each draw's fog configuration and suppress it |
| 2395 | `GFX_STAGE_SCENE_AFTER_OPAQUE` | VBAO and SMAA composite. `on_scene_after_opaque`: close the scope, arm the fog quad, run the replay if needed, build the Status line |
| 2405 | Translucent lists begin | **Fog quad pushed** at the first `J3DShape::drawFast` (the normal anchor) |
| 2632 | Bloom (`bloom_c::draw`) | Fallback anchor: a pre-hook on it pushes the quad if nothing earlier did |
| 2759 | `GFX_STAGE_FRAME_BEFORE_HUD` | Last-resort anchor |

The quad wants to land right after `SCENE_AFTER_OPAQUE`, but there is no stage there. So the mod
anchors on the first J3D shape drawn after the stage, which is the first translucent J3D draw. In a
frame with no translucent J3D draw, the quad would land just before bloom, or at worst after it,
where bloom has already used the unfogged image. The Status line's `[anchor]` field says which
happened. (`dComIfGd_drawXluListBG` is an out-of-line function on this pin, so hooking it directly
may now be possible. That is untested.)

### Capture and suppression

While the scope is open, every way the game applies fog to opaque geometry is caught:

- **J3D shapes**: a pre-hook on `J3DShape::drawFast` runs after the material's display list has
  set the fog. It reads the material's fog block (`getPEBlock()->getFog()`), registers that
  configuration, and issues `GXSetFog(GX_FOG_NONE, ...)` to switch it off for the shape.
- **Direct fog setters**: pre-hooks on `GXSetFog` and `GFSetFog` record the arguments and rewrite
  the type to `GX_FOG_NONE`. `GFSetFog` is a direct register write used only by field grass
  (`dKy_GfFog_tevstr_set`).
- **Map units** (`dBgp_c`, the shared pieces stages are assembled from) bypass `drawFast`: they
  load the material's shared display list, which re-issues the material's fog, and draw the shapes
  directly. A post-hook on `loadSharedDL` (all three material classes) catches that. It is limited
  to calls made inside `dBgp_c::modelMaterial_c::drawSimple` by a pre/post hook pair on
  `drawSimple`. That limit is required: every other `loadSharedDL` caller sets the packet's fog
  *after* the display list, so the material's own fog never renders there, and registering it would
  invent a configuration the game never draws with.
- **Grass and flowers** draw themselves (`dGrass_packet_c`, `dFlower_packet_c`): material display
  list, then the room's fog setter, then raw geometry. Their fog setter is caught by the hooks
  above; a pre/post pair on their `draw` records which configuration they used, for the replay's
  fallback (below).

A configuration is the fog type, start/end, near/far, colour and range adjustment. Two count as the
same if type and range adjustment are equal, colour is within 6 per channel, start/end within 2% of
the span, near within 1 and far within 1% + 1 (`config_matches`). The first configuration seen in
the frame becomes config 0, the reference. Up to 8 are kept; beyond that, extras silently merge into
config 0.

### One configuration: the simple path

If the whole frame used one configuration, `push_fog_quad` resolves the depth buffer and draws one
fullscreen pass (`fs_main`) that computes the fog factor from each pixel's depth and blends the fog
colour over the scene: `mix(scene, fog colour, f)`, exactly as the game's shaders do per draw.

### Several configurations: the config-ID replay (Exact mode)

Rooms lag the stage's palette blend, and some materials have special fog, so most outdoor frames
mix configurations. In Exact mode `on_scene_after_opaque` then replays the opaque lists into an
offscreen pass (`create_pass`) with the game's own camera, forcing every shape's output to a flat
colour encoding its configuration index: red = `(index + 1) × 24`, green and blue 0. The fog pass
(`fs_mixed`) reads that buffer to pick each pixel's configuration.

- Only the six opaque lists are replayed (BG, DarkBG, Middle, main, Dark, Packet), not particles or
  the game's shadows.
- Draws the replay cannot recolour (grass, flowers, other packet-list models) render their real
  colours. Any pixel with green or blue above 0.03, or a red value outside a valid slot, decodes as
  "unknown" and takes the **fallback configuration**: the one grass and flowers used this frame, or
  config 0 if there was none. Grass and flowers are most of those pixels, which is why the fallback
  is their configuration.
- Red 216 (C++ `kNoFogSlot` = 8, shader slot 9) is reserved: "leave this pixel unfogged", used by
  `fogSkipUnfogged`.
- If the replay fails, the frame falls back to the simple path with config 0.

### Exemptions

- **The Hyrule Castle barrier.** Both barrier actors (`d_a_obj_ganonwall`, `d_a_obj_ganonwall2`)
  rewrite their material fog every frame to black over 1000..250000. Deferring that would stamp the
  barrier's black fog onto the castle and trees inside it in the replay. So the mod recognises that
  **exact** triple (`is_barrier_fog`) and leaves those draws on the game's fog; in the replay they
  write no colour, so their pixels take the configuration of whatever is behind them. The match must
  stay exact: a looser `black && endZ > 100000` once also caught the game's own black-fog type
  (`mType 7`, used on water and `MA20`) and double-fogged it.
- **Wolf Senses.** See below.

### Wolf Senses

While Wolf Link's senses are active (`daPy_py_c::checkNowWolfPowerUp()`, guarded for a missing
player), no scope opens: nothing is suppressed, no quad or replay runs, and the frame is entirely
the game's own.

Senses replaces every environment fog with black fog over a short range (`dKy_WolfPowerup_FogNearFar`:
750..1750 outdoors and 1000..1800 indoors by default, other ranges on some stages). Black fog is a
pure attenuation, `(1 − f)·x`, so a multiplicative composite gives the same result applied before or
after it: `m·(1 − f)·x = (1 − f)·(m·x)`. Deferring it gains nothing. It also exposed the main
weakness of a fullscreen pass (one depth per pixel, see [Limitations](#limitations)): with fog that
reaches full black within ~1750 units, some camera directions showed far more of the world than
senses allow. This exemption was confirmed in-game on `v2.0.0`. An *additive* composite (indirect
light) would differ, but none is built.

### Range adjustment and exactness

`fog_math.h` reproduces how the game encodes fog parameters into GX registers (`J3DGDSetFog`) and
how aurora decodes them (`lib/gx/regs.cpp`), mantissa truncation included, so `a, b, c` match the
renderer's. `fog.wgsl` applies aurora's fog formula and all five curves.

The game also enables **fog range adjustment** ("XFog") globally (`envcolor_init`,
`d_kankyo.cpp:1257`). It multiplies the fog term by a per-column factor because a pixel at the
screen edge is further from the eye than its depth says. Aurora bakes it into a per-column table
(`build_fog_range_lut`); `fog.wgsl` evaluates the same function per pixel (`fog_range_factor`). It
matters most for the narrow, far-starting fog bands of distant haze.

So the fog math matches the renderer's, with three caveats: the range factor is computed rather
than read from aurora's table, it assumes the world viewport spans the whole render target, and
orthographic fog types are treated as perspective. None of these has shown a visible difference.

### Pipelines

The four render pipelines (simple and mixed, each blended and debug) are built lazily in `on_draw`
from the live `GfxDrawContext::layout` and rebuilt when `layout.key` changes
(`ensure_fog_pipelines`). This mod never asks for normals itself, but VBAO and SMAA do, and the
scene pass gains a second colour attachment the frame after they ask. A pipeline built at init
would then be silently rejected and the fog would vanish. Blend: colour `srcAlpha / 1 − srcAlpha`
with the fog factor in alpha; the target's alpha is left alone.

### The exported service

`dev.automata.deferred_fog` (`include/deferred_fog_service.h`) has one call, `get_state`, which
reports whether the last frame armed the fog quad. It exists mainly as an ordering lever: hooks on
one stage run in registration order, registration follows load order, and importing a service is
the only way to load after another mod. Nothing imports it today. A mod that composites at
`SCENE_AFTER_OPAQUE` is already under the fog by stage separation, and an overlay that must sit on
top of the fog can draw at `FRAME_AFTER_HUD` (as VBAO's debug views do). Import it optionally if you
ever need to interleave within a stage.

## Limitations

What one fullscreen pass over the finished opaque image cannot reproduce exactly:

- **One depth per pixel.** The game fogs each fragment at its own depth; the quad fogs each pixel at
  the depth the depth buffer holds. They differ wherever the depth owner is not the colour source:
  - a see-through surface that writes depth fogs what is behind it at its own (nearer) depth;
  - a surface that writes no depth is fogged at the depth of what is behind it, and over the sky
    (depth 0) not at all.
- **Over-unity blends (the `K` factor).** The game fogs a draw's colour before blending it. For
  layers with blend factors (sᵢ, dᵢ), vanilla and the quad differ by `f·F·(K − 1)` where
  `K = Σᵢ sᵢ·Π_{j>i} dⱼ`. An ordinary alpha blend over an opaque surface at the same depth has
  `K = 1`, no difference. An additive blend (destination factor 1) or `GX_BM_SUBTRACT` gives
  `K ≠ 1`, and at full fog vanilla tends to `K·F` while the quad tends to exactly `F`.
  `material_over_unity_blend` and the `additive` counter detect these.
- **Materials with fog switched off.** A material's fog block can have type 0 (`GX_FOG_NONE`); the
  game then applies no fog however far away it is, while the quad fogs it. `fogSkipUnfogged` marks
  such pixels, but only for materials that write their own depth (otherwise it would unfog what is
  behind them) and have no alpha test (the replay draws cutouts solid).
- **Draws the capture does not see.** `Pri0_B` particles and the game's own shadows draw inside
  the scope through `GXSetFog`, so their fog is suppressed like everything else, but they are
  invisible to the Status counters and not replayed.
- Translucent geometry draws after the quad with its own fog, as in vanilla, *if* the quad landed at
  the normal anchor. On the fallback anchors translucents are fogged twice.

## Changing this mod: rules

- Scene-pass pipelines: lazily from the live layout, rebuilt on `layout.key` (above).
- `exact_mode()`'s fallback value must equal `fogMixedMode`'s registered default (1). They
  disagreed once, so a failed config read ran a mode the UI was not showing.
- Keep `needs_id_buffer()` as the single gate for both building and using the replay.
- Keep `is_barrier_fog` an exact match.
- Do not add a rule that leaves all *blended* draws on the game's fog. It was tried: it exempted
  `K = 1` draws that were already exact, and they were fogged twice. It measured worse in-game.
- Uniform structs (`FogUniforms` 112 bytes, `MixedFogUniforms` 336 bytes, `FogRangeUniform`
  64 bytes) are mirrored in `fog.wgsl`; keep the `static_assert`s true.
- After a pin bump, re-check every `DEFINE_HOOK` target by name in the new tree (see
  `CONTRIBUTING.md` "Moving to a newer game build"). A clean compile does not prove a hook resolves.
- Before proposing a fix for a visual difference, get per-pixel evidence (the debug views), not
  only the Status counters. See the next section for why.

## Known issues

### Distant landmarks brighter with the mod off

**Report.** Distant landmarks, Death Mountain in particular and the Ganon barrier, look brighter
with Deferred Fog off than on, with no other mods enabled: *"chunks of the far off Death Mountain
geometry appear to overpower the fog so you can see the light from the incredibly far distance"*.
With the mod off the mountain shows its own orange and rock colours; with it on, the same geometry
is washed to the haze colour. The maintainer has accepted the mod as-is for now; nobody is working
on it.

**The one measurement taken** (on the retired fork platform, before the anchor readout was fixed):

```
Deferring fog (exact: 75 draws, 2 configs; 0 shared-DL, 3 fog-off, 0 additive/0 no-Z)
```

What it does and does not show:

- `3 fog-off`: three draws in that view are materials with fog switched off, which the quad was
  fogging. Whether those draws are Death Mountain was never established.
- `0 additive`: no over-unity blend among the draws the counters see. The counters do not see
  particles, the game's shadows or packet-list models, so this does not rule out the `K` factor for
  the whole view.
- `0 shared-DL`: no map-unit material with *live* fog. A map-unit material with fog switched off is
  counted under fog-off instead, so this does not show Death Mountain is not a map unit.

**Fixes tried, all failed:**

| # | Fix | Result |
| :-- | :-- | :-- |
| 1 | Leave all blended draws on the game's fog | Worse in-game, and no change to the symptom. Reverted (see rules above) |
| 2 | Move the fallback anchor before bloom | No change: the view contains translucent geometry, so the quad already lands at the normal anchor (by the maintainer's account; the anchor readout was buggy when the view was measured) |
| 3 | `fogSkipUnfogged` | No change in-game. Kept, default off |

Fix 3's failure disproves the fix, not the mechanism: the reading that separates them,
`fog-off (M markable / Z no-Z / T alpha)`, was never taken in that view. If `markable` is 0 there,
the mark never fired.

**Next step.** Stand in that view and capture, in order:

1. The Status line with `fogSkipUnfogged` **on**, and its `[anchor]`.
2. The **Fog Factor** view. A landmark that keeps its own texture through the haze in vanilla but
   is gray-white here is a fog-off or depth-ownership difference; a landmark that is *more* fogged
   than the haze colour in vanilla points at an over-unity blend.
3. The **Config IDs** view, to see which configuration the landmark resolves to.

| Reading | Meaning | Action |
| :-- | :-- | :-- |
| anchor not `at translucents` | placement, not fog math | fix the anchor first |
| fog-off > 0, markable > 0, skip on, no change | the fog-off mechanism is present but is not this landmark | use the debug views to find which draw the landmark is |
| fog-off > 0, markable = 0, alpha > 0 | the material is alpha-tested, so the mark is refused | the mark would need to carry the material's own alpha (bind its texture, take TEV alpha from `GX_CA_TEXA`) |
| fog-off > 0, markable = 0, no-Z > 0 | the material writes no depth | marking it would unfog what is behind it: stop |
| additive > 0, no-Z < additive | over-unity blend on depth-owning geometry | the same mark could be extended to it, or a second pass could add the extra `K·F` term |
| additive > 0, no-Z = additive | over-unity blend, none of it owns depth | cannot be fixed with one pass |
| everything 0 | neither known mechanism | start from the debug views; also consider the depth-ownership limitation |

### Code issues

Found in the documentation audit; none is known to cause a visible problem.

- The `Disabled` Status state is unreachable: `on_scene_after_opaque` returns early when the scope
  is closed, so the line keeps its last text.
- If the replay's colour resolve fails after `create_pass` succeeded, the offscreen pass is left
  open; the host then force-closes it and fails the mod.
- More than 8 fog configurations in a frame silently merge into config 0.
- `CameraService` is imported but unused, and the mod includes the deprecated `<mods/hook.hpp>`
  (a build warning).
- `deferring` in the exported service means "armed at `SCENE_AFTER_OPAQUE`"; a later depth-resolve
  failure still reports true.
- The warning logged when the `GFSetFog` hook fails says grass *and flowers* lose deferral; only
  grass uses `GFSetFog`.
- Any other mod that replays the game's draw lists while the scope is open (Realtime Sun Shadows
  does, from `SCENE_AFTER_TERRAIN`; it is unreleased) would have those draws captured and their fog
  suppressed, and a J3D draw from another mod's `SCENE_AFTER_OPAQUE` hook registered after this one
  would trigger the translucent anchor early.

## History

- Originally half of a combined "Graphics Hub" mod, alongside a Depth to Normal provider. When the
  graphics service started providing the game's authored normals, the provider became pointless,
  Graphics Hub was retired, and this became a standalone mod again.
- The uncovered-pixel fallback once ranked configurations by widest `endZ`, which is the *weakest*
  fog at any depth: grass stopped darkening with distance. It now uses the grass and flower packets'
  own configuration (confirmed in-game on the fork platform). An even earlier version ranked by far
  plane, which in TP always picked config 0 (every configuration in a frame shares the view's
  near/far).
- Fog range adjustment was added after an earlier version of this document wrongly claimed aurora
  ignores it.
- The Status line's `[anchor]` read `not pushed` in every frame for a while, because it was built
  before any anchor fired. It now reports the previous frame's anchor.
- 1.0.0: version reset for the public release. 1.0.1: description and icon. 1.0.2: Wolf Senses
  exemption.
