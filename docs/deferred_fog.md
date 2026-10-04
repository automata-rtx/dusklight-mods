# Deferred Fog

Applies the game's fog after the opaque world has been drawn, so screen-space effects such as
ambient occlusion darken the surfaces *under* the fog instead of darkening the fog itself.

| | |
| :-- | :-- |
| Mod id | `dev.automata.deferred_fog` (`mods/deferred_fog/`) |
| Version | `2.0.0-d`, a test build (see `mods/deferred_fog/mod.json`) |
| Kind | **Game-linked**: includes game headers, calls game functions and hooks ten of them. It must be built against the game build it runs on |
| Game build | Dusklight `v2.0.0` |

**Why it exists.** The game fogs each opaque draw while drawing it. A mod that composites over the
finished opaque image, as VBAO does at `GFX_STAGE_SCENE_AFTER_OPAQUE`, therefore multiplies over
pixels that are already fogged, and distant AO reads as grime floating on the haze. Deferred Fog
switches the game's fog off for the opaque world and applies the same fog afterwards in one
fullscreen pass. The pass runs after every mod's `SCENE_AFTER_OPAQUE` work and before anything the
game draws, copies or post-processes later in the frame.

No other mod depends on it and it depends on none: the ordering comes from the frame's stages.

## Files

| File | Contents |
| :-- | :-- |
| `src/mod.cpp` | Everything host-side: hooks, fog capture and suppression, the configuration table, the configuration-ID replay, the fog pass, options, UI, the exported service |
| `src/fog_math.h` | `compute_fog_coefficients`: a fog configuration → the `a, b, c` coefficients exactly as the renderer decodes them |
| `res/fog.wgsl` | The fullscreen fog pass: `fs_main` (one configuration), `fs_mixed` (per-pixel configuration) and the debug views |
| `include/deferred_fog_service.h` | The exported `dev.automata.deferred_fog` service |

Key functions in `mod.cpp`: `on_scene_begin`, `on_set_fog_pre`, `on_shape_draw_pre`,
`on_material_shared_dl_post`, `on_scene_after_opaque`, `replay_config_ids`, `on_xlu_list_bg_pre`,
`push_fog_quad`, `on_draw`, `ensure_fog_pipelines`, `install_hooks`.

## Using it

### Options

The mod's pane in the Mods menu has **Enabled**, a read-only **Status** line and an **Open Fog
Controls** button. The controls window repeats Enabled and holds the rest.

| Config key | UI label | Default | Meaning |
| :-- | :-- | :-- | :-- |
| `fogEnabled` | Enabled | on | Off: the game draws its own fog |
| `fogSkipUnfogged` | Skip Unfogged Geometry | on | Leave unfogged the pixels of materials the game draws with fog switched off, as the game does. Runs the configuration-ID replay in every frame with a markable fog-off draw. See [Skip Unfogged](#skip-unfogged) |
| `fogDebug` | Debug View | 0 | 0 off, 1 Fog Factor, 2 Config IDs, 3 Replay Coverage. See [Debug views](#debug-views) |
| `fogLogConfigs` | Log Fog Configs | off | Log the frame's fog configurations when their number, any start or end distance, or configuration 0's type or colour changes |
| `fogDeferInSenses` | Defer Fog During Wolf Senses (diagnostic) | off | Take over the fog during Wolf Senses as well. For examination only; see [Wolf Senses](#wolf-senses) |

Config keys are stored as `mod.dev.automata.deferred_fog.<key>` in the game's `config.json`. The
defaults are the second argument of the `register_bool` / `register_int` calls in `init()`.

### Status line

Rebuilt every frame in `on_scene_after_opaque`. The working state reads:

```
Deferring fog (N draws, K configs[, M merged][, replay failed]; A shared-DL, B fog-off (P markable, T by alpha/Z no-Z/U unmarkable), C additive/D no-Z)
```

| Field | Meaning |
| :-- | :-- |
| `N draws` | Draws whose fog was captured and switched off this frame (each J3D shape, map-unit material and direct fog-setter call counts once) |
| `K configs` | Distinct fog configurations this frame. More than 1 runs the configuration-ID replay (so does Skip Unfogged with any markable fog-off draw) |
| `M merged` | Draws whose configuration did not fit in the 8-entry table; they use configuration 0 |
| `replay failed` | The replay could not run; the frame used configuration 0 everywhere |
| `A shared-DL` | Of the N draws, map-unit (`dBgp_c`) material draws |
| `B fog-off (P markable, T by alpha / Z no-Z / U unmarkable)` | Draws whose material has fog switched off. `P` of them write their own depth and Skip Unfogged can mark them, `T` of those through their own alpha (they are alpha-tested). `Z` write no depth. `U` write depth but cannot be marked (no alpha test of their own, or all 16 TEV stages in use). `P + Z + U = B` |
| `C additive / D no-Z` | Draws with an additive or subtractive blend (see [Limitations](#limitations)); of those, how many write no depth |

The counts cover the draws that pass through the capture hooks: J3D shapes, map units and the fog
setters. They do not cover particles or the game's shadows.

Other states:

| Text | Meaning |
| :-- | :-- |
| `Waiting for the first frame` | No world frame has been drawn since the mod loaded |
| `Off: the game's own fog is used` | Enabled is off |
| `Wolf Senses: the game's own fog is used` | Wolf Link's senses are active |
| `No fogged draws in view` | Nothing in the opaque world set fog this frame |
| `Sky depth snapshot failed: the game's own fog is used` | The depth snapshot at `SCENE_BEGIN` failed, so the scope stayed closed this frame; see [The sky](#the-sky) |
| `Inactive: <function> could not be hooked in this game build; the game's own fog is used` | A required hook failed to attach at load; see [Hooks](#hooks) |

### Debug views

Each replaces the image with what the fog pass computes, drawn opaque where the fog pass draws.
Anything the game draws after that point (translucent geometry, particles) still draws over it.
Pixels left to the sky's own fog are black: depth 0, and depth the sky lists wrote, such as the
clouds' (see [The sky](#the-sky)).

| `fogDebug` | View | Shows |
| :-- | :-- | :-- |
| 1 | Fog Factor | The fog amount per pixel (white = full fog) |
| 2 | Config IDs | One gray level per fog configuration (white = the last), on frames that run the configuration-ID replay; otherwise Fog Factor. A frame that runs the replay only for Skip Unfogged has one configuration and shows white |
| 3 | Replay Coverage | What the configuration-ID replay recorded per pixel, and why. Green: the draw's own fog configuration. Yellow: its configuration did not fit in the table and uses configuration 0 (`merged`). Cyan: a fog-off draw that writes depth but is not marked (Skip Unfogged off, or `unmarkable`), stamped as configuration 0. Magenta: a draw with no fog block, stamped as configuration 0. Blue: nothing the replay draws (grass, flowers, particles and other directly drawn geometry), which takes the fallback configuration. Runs the replay every frame while selected |

In every view, **red** pixels are ones Skip Unfogged leaves unfogged. If a debug view shows the normal
scene, the fog pass did not draw this frame; an all-black view means it drew and computed no fog.

### Log messages

- `ready` on load, or `inactive: a required game hook is missing` together with one
  `could not hook <function> in this game build` error per missing hook.
- `Wolf Senses: the game's own fog is used` and `Wolf Senses over: deferring fog`.
- `per-pixel replay on: K fog configurations, P markable fog-off draws in view` and
  `per-pixel replay off` when the replay starts or stops running (`P` is 0 with Skip Unfogged off).
- One-time warnings: `sky depth snapshot failed; such frames use the game's own fog`,
  `configuration-ID replay failed; such frames use one fog configuration` and
  `depth snapshot failed; no fog pass this frame`.
- With Log Fog Configs on, the configuration table when it changes.

## How it works

### Where it acts in the frame

The world camera's part of the frame is `mDoGph_Painter` in `dusklight/src/m_Do/m_Do_graphic.cpp`.
Line numbers are at the `v2.0.0` pin. The Japanese labels are the game's own CPU-timer names for
each step.

| Line | Game | This mod |
| :-- | :-- | :-- |
| 2328 | Sky lists | Untouched; the sky keeps the game's fog |
| 2334 | `GFX_STAGE_SCENE_BEGIN` | `on_scene_begin` snapshots the depth the sky lists left and opens the capture scope |
| 2344–2390 | Opaque world lists, `Pri0_B` particles, the game's shadows | Each draw's fog is captured and switched off |
| 2395 | `GFX_STAGE_SCENE_AFTER_OPAQUE` | VBAO and SMAA composite. `on_scene_after_opaque` closes the scope, arms the fog pass and runs the replay if needed |
| 2405 | `dComIfGd_drawXluListBG`: the translucent lists begin | **`on_xlu_list_bg_pre` pushes the fog pass** |
| 2405–2432 | Translucent lists and their particles | Drawn over the fogged image with the game's own fog |
| 2452 | Motion blur (when active): blends the last framebuffer copy over the frame | |
| 2461 | Depth of field (`drawDepth2`): copies the frame and the Z buffer whenever the player exists | |
| 2545 | 「フレームバッファキャプチャー２回目」 *framebuffer capture, 2nd time* (`retry_captue_frame`) | |
| 2565–2577 | Full-projection particles and the indirect-screen list (the *moya* haze packet, water pillars). Particles and haze modes that use the framebuffer texture sample the copy | |
| 2601 | 「完全投影用スクリーン」 *full-projection screen* list: screen-sized models such as the underwater filter (`dKy_undwater_filter_draw`) and `kytag15`'s | |
| 2615–2620 | Underwater, or in `D_MN08`, with bloom on: a 3rd copy | |
| 2632 | Bloom, 「飽和加算フィルター」 *saturating-add filter*. It reads the last copy, not the screen | |

Steps 2452 to 2632 run only while the game is not paused.

`mDoGph_Painter` calls `dComIfGd_drawXluListBG` unconditionally, directly after the
`SCENE_AFTER_OPAQUE` stage, so the fog pass lands at the same point in every frame whatever is on
screen. Every later step that reads or redraws the frame works from the fogged image, as it does in
the game without the mod:

- the depth-of-field and framebuffer copies contain the fog;
- bloom is computed from a fogged copy, and so is its first step, 「彩度減算」 *saturation subtract*,
  which blends a desaturated copy back over the screen (its strength is set per palette);
- the screen-sized filters drawn after the copy, such as the underwater one, draw over a fogged
  frame;
- translucent geometry and particles draw after the fog pass with their own fog, as in vanilla.

### Capture and suppression

From `SCENE_BEGIN` to `SCENE_AFTER_OPAQUE` the mod catches every way the game sets fog on opaque
geometry, records the configuration, and switches the fog off:

- **J3D shapes.** A material's display list sets the fog from its fog block, then
  `J3DShape::drawFast` draws the shape. A pre-hook on `drawFast` reads the block
  (`getPEBlock()->getFog()`), registers that configuration and issues `GXSetFog(GX_FOG_NONE, ...)`.
- **Direct fog setters.** Pre-hooks on `GXSetFog` and `GFSetFog` record the arguments and rewrite
  the type to `GX_FOG_NONE`. The game's setters are `dKy_GxFog_set`, `dKy_GxFog_tevstr_set` and
  `dKy_GfFog_tevstr_set`; the last is the only `GFSetFog` caller and is used by grass.
- **Map units** (`dBgp_c`, the shared pieces stages are assembled from) bypass `drawFast`:
  `dBgp_c::modelMaterial_c::drawSimple` loads the material's shared display list, which sets the
  material's fog, then draws through `J3DShapeDraw::draw`. A post-hook on `loadSharedDL` (all three
  material classes) does what the `drawFast` hook does. It acts only inside `drawSimple`, bracketed
  by a pre/post hook pair: the other `loadSharedDL` callers (`dMdl_c::draw`,
  `dPa_modelEcallBack::model_c::draw`, the chain actors) set the room fog after the display list, so
  the material's own fog never reaches their geometry and the `GXSetFog` hook captures what does.
- **Grass and flowers** (`dGrass_packet_c`, `dFlower_packet_c`) load their material display list,
  set the room fog with the setters above and send raw geometry. A pre/post pair on their `draw`
  records which configuration they used, for the replay's fallback (below).

A configuration is the fog type, start/end, near/far, colour and range adjustment. Two are the same
if type and range adjustment are equal, colour is within 6 per channel, start and end are within 2%
of the span, near is within 1 and far within 1% + 1 (`config_matches`). The first configuration in a
frame is configuration 0. The table holds 8; a draw whose configuration does not fit uses
configuration 0 and is counted as `merged`.

### The sky

The sky lists draw before the scope opens, so they keep the game's fog, and the fog pass must leave
their pixels alone. Most of the sky writes no depth, but not all of it: while the sun is on screen,
`drawVrkumo` draws the drifting clouds (*kumo*, 雲 cloud) twice, first as depth-only shapes cut out by
their texture's alpha, then in colour without depth. `dKyr_sun_move` reads that depth back around
the sun (`dComIfGd_peekZ`) to judge how much of it is hidden. World geometry behind a cloud's depth
then fails the depth test, so in the game those pixels show the cloud.

`on_scene_begin` therefore snapshots the depth buffer as the sky lists left it (`resolve_pass`) when
it opens the scope. In `fog.wgsl`, `is_sky` treats a pixel as the sky's when its depth is 0 or
still equals that snapshot: nothing drawn after the sky replaced it. Both snapshots are copies of the
same buffer made the same way, so an untouched pixel compares exactly equal. If the snapshot fails,
the scope stays closed and the game draws its own fog that frame.

### One configuration

If the frame used one configuration, `push_fog_quad` snapshots the depth buffer and draws one
fullscreen pass (`fs_main`). It computes the fog factor from each pixel's depth and blends the fog
colour over the scene, `mix(scene, fog colour, f)`, exactly as the game's shaders do per draw.

### Several configurations: the configuration-ID replay

Most outdoor frames use several configurations. Rooms lag the stage's palette blend, and water
materials carry black or white fog by polygon code (`docs/japanese-naming.md` §4.5). In such a frame
`on_scene_after_opaque` re-draws the opaque lists into an offscreen pass (`create_pass`) with the
game's camera, forcing every shape to a flat colour that encodes its configuration: red =
`(index + 1) × 24`, green and blue 0. The fog pass (`fs_mixed`) reads that buffer to pick each
pixel's configuration.

- The replay draws the six opaque lists (BG, DarkBG, Middle, main, Dark, Packet), not the `Pri0_B`
  particles or the game's shadows.
- Draws the replay cannot recolour (grass, flowers and other self-drawing packets) render their lit
  colours. A pixel with green or blue above 0.03, or a red value outside a valid slot, takes the
  **fallback configuration**: the one the grass and flower packets drew with this frame, or
  configuration 0 if there were none.
- Red 216 (C++ `kNoFogSlot` = 8) means "leave this pixel unfogged", written by Skip Unfogged
  (below).
- Blue carries why the draw got its slot (`StampReason`, as `reason × 2/255`), read only by the
  Replay Coverage debug view. It stays below the 0.03 that marks a pixel unstamped.
- Some draws write nothing (`stamp_nothing`), so their pixels keep the ID of the surface whose depth
  the fog pass uses there: the barrier (below) and fog-off draws that write no depth. They use a
  blend that keeps the destination rather than switching colour writes off, because J3D materials
  reload the blend mode but not the colour-update switch.
- If the replay cannot run, the frame uses configuration 0 everywhere. The game's fog is already off
  for that frame, so this is the closest available result. The Status line shows `replay failed`.

### Skip Unfogged

A material's fog block can have type 0 (`GX_FOG_NONE`): the game then draws it with no fog at any
distance, while the fog pass would fog it like everything else. With Skip Unfogged on (the default),
the replay writes the no-fog mark (red 216) for such a material instead of a configuration, and the
fog pass leaves the marked pixels alone. A frame with a markable fog-off draw runs the replay even
with one configuration.

- Only a material that writes its own depth is marked. One that writes none does not own its
  pixels' depth, and marking it would unfog the surface behind it. It writes nothing in the replay
  instead, so a fog-off glow or swirl layered over a marked surface leaves that surface's mark in
  place.
- A material without an alpha test is marked with the replay's usual flat stamp.
- An alpha-tested material is marked through its own alpha, so only the pixels its alpha test keeps
  are marked and the replay's depth matches the frame's (`stamp_no_fog_through_alpha`). Its display
  list has already set its textures, TEV stages, alpha test and Z mode; the replay keeps all of them
  and appends one TEV stage. That stage outputs the mark colour from colour channel 0 (switched to
  its material-colour register) and copies the alpha the material's last stage wrote, from that
  stage's output register and with its clamp setting. The material-colour register is shared with
  the alpha channel, so its alpha keeps the material's value (`plan_alpha_mark`).
- A material that sets no alpha test of its own, or already uses all 16 TEV stages, cannot be marked
  this way; it is counted as `unmarkable` and fogged.

### Exemptions

- **The Hyrule Castle barrier.** Both barrier actors (`d_a_obj_ganonwall`, `d_a_obj_ganonwall2`)
  draw in the opaque lists and set their material fog to black over 1000..250000 every frame.
  Deferring it would put the dome's black fog on the castle and trees behind it. The mod recognises
  that **exact** triple (`is_barrier_fog`) and leaves those draws on the game's fog; in the replay
  they write nothing, so their pixels take the configuration behind them. The match must stay
  exact: the game's black-fog water materials (polygon codes MA03, MA17, MA19 and MA20) are also
  black with a far end, and must be deferred.
- **Wolf Senses.** See below.

### Wolf Senses

While Wolf Link's senses are active (`daPy_py_c::checkNowWolfPowerUp()`, after a check that the
player exists), the capture scope does not open: nothing is suppressed, no replay or fog pass runs,
and the frame is the game's own.

Senses replaces every environment fog with black fog over a short range
(`dKy_WolfPowerup_FogNearFar`). Black fog only scales colour, `(1 − f)·x`, so a multiplicative
composite such as AO gives the same image under the game's fog as under this mod's:
`m·(1 − f)·x = (1 − f)·(m·x)`. Leaving it to the game also avoids the fog pass's
one-depth-per-pixel limits (below), which matter most when the fog reaches black within a short
distance. An additive composite would differ, but none is built.

`fogDeferInSenses` makes the mod take over the senses fog anyway, for examination with the debug
views. The result can then differ from the game's own look wherever the fog pass's single depth per
pixel does not match the surface.

### Fog math and range adjustment

`fog_math.h` reproduces how the game encodes fog parameters into the GX registers
(`J3DGDSetFog`, `GXSetFog`) and how aurora decodes them (`lib/gx/regs.cpp`), mantissa truncation
included, so `a, b, c` match the renderer's. `fog.wgsl` applies aurora's formula
(`lib/gx/shader.cpp`): `a / (b − (1 − depth))`, the range factor, `− c`, then one of the five curves,
then `mix`.

The game enables **fog range adjustment** ("XFog", `GxXFog_set`) in `envcolor_init` and every fog it
sets carries it. It multiplies the fog term by a per-column factor because a pixel at the screen
edge is further from the eye than its depth says. Aurora bakes it into a per-column table
(`build_fog_range_lut`); `fog.wgsl` evaluates the same function per pixel (`fog_range_factor`).

Three approximations: the range factor is computed rather than read from aurora's table, it assumes
the world viewport spans the whole render target, and orthographic fog types are treated as
perspective.

### Pipelines

The four render pipelines (`fs_main` and `fs_mixed`, each blended and debug) are built lazily in
`on_draw` from the live `GfxDrawContext::layout` and rebuilt when `layout.key` changes
(`ensure_fog_pipelines`). This mod never asks for normals, but VBAO and SMAA do, and the scene pass
gains a second colour attachment the frame after they ask; a pipeline built for the old shape would
be rejected and the fog would vanish. Blend: colour `SrcAlpha / OneMinusSrcAlpha` with the fog
factor in alpha; the target's alpha is kept, as the game's fog does not change alpha.

### Hooks

The mod hooks ten game functions, all of them required:

| Hook | Purpose |
| :-- | :-- |
| `GXSetFog`, `GFSetFog` (pre) | Capture direct fog setters |
| `J3DShape::drawFast` (pre) | Capture material fog; stamp the replay |
| `dBgp_c::modelMaterial_c::drawSimple` (pre/post) | Bracket map-unit drawing |
| `J3DMaterial`, `J3DPatchedMaterial`, `J3DLockedMaterial` `::loadSharedDL` (post) | Capture map-unit material fog; stamp the replay |
| `dGrass_packet_c::draw`, `dFlower_packet_c::draw` (pre/post) | Record the grass and flower configuration |
| `dComIfGd_drawXluListBG` (pre) | Push the fog pass |

`install_hooks` attempts all of them. If any fails to attach (a game build this mod was not compiled
for), the capture scope never opens, the hooks that did attach have nothing to do, and the game draws
its own fog. The log names each missing hook and the Status line names the first.

### The exported service

`dev.automata.deferred_fog` (`include/deferred_fog_service.h`) has one call, `get_state`. Its
`deferring` field is true when the frame armed the fog pass at `SCENE_AFTER_OPAQUE`. Before that
stage in a frame it holds the previous frame's value, and it stays true if the depth snapshot then
fails. It is false while the mod is off, inactive or leaving Wolf Senses to the game, in a frame
whose sky-depth snapshot failed, and in a frame with no fogged draws. A mod that composites at `SCENE_AFTER_OPAQUE` is
already under the fog without importing it, and an overlay that must sit on top of the fog can draw
at `GFX_STAGE_FRAME_AFTER_HUD`. Importing the service only matters for ordering inside one stage:
hooks on a stage run in registration order, which follows load order, and an import makes the
importer load later. Nothing imports it today.

## Limitations

What one fullscreen pass over the finished opaque image cannot reproduce exactly:

- **One depth per pixel.** The game fogs each fragment at its own depth; the fog pass fogs each pixel
  at the depth the depth buffer holds. They differ wherever the surface that owns the depth is not
  the one the colour comes from:
  - a see-through opaque-list surface that writes depth fogs what is behind it at its own, nearer,
    depth;
  - a surface that writes no depth is fogged at the depth of what is behind it, and over the sky
    not at all.
- **Additive and subtractive blends.** The game fogs a draw's colour before blending it. For layers
  with blend factors (sᵢ, dᵢ), the game and the fog pass differ by `f·F·(K − 1)` where
  `K = Σᵢ sᵢ·Π_{j>i} dⱼ`. An ordinary alpha blend over an opaque surface has `K = 1`, no difference.
  An additive blend (destination factor 1) or `GX_BM_SUBTRACT` gives `K ≠ 1`. The `additive`
  counter shows such draws. Leaving them on the game's fog does not help: the fog pass still fogs
  their pixels, so they would be fogged twice.
- **Materials with fog switched off.** Skip Unfogged leaves them unfogged only where they write
  their own depth and can be marked (see [Skip Unfogged](#skip-unfogged)); the `no-Z` and
  `unmarkable` ones are fogged. With Skip Unfogged off, all of them are fogged.
- **Draws the capture does not see.** `Pri0_B` particles and the game's shadows draw inside the
  scope through `GXSetFog`, so their fog is switched off like everything else, but they are not in
  the Status counts and not replayed.
- **Eight configurations per frame.** More than eight distinct configurations in one frame merge
  into configuration 0 (the `merged` count).

## Diagnosing a difference from vanilla

Compare against the mod off, then take per-pixel evidence before proposing a cause: a per-frame
count shows that a mechanism is present in the view, not that it is what a given pixel looks like.

1. Read the Status line in the view.
2. **Fog Factor**: where the fog lands per pixel. A surface that keeps its own colours through the
   haze in vanilla but is gray-white here is a fog-off or depth-ownership difference; one that is
   *more* fogged than the fog colour in vanilla points at an additive blend.
3. **Config IDs**: which configuration each pixel resolves to.
4. **Replay Coverage**: per pixel, whether the replay recorded the draw's own configuration
   (green), configuration 0 for a reason (yellow merged, cyan fog-off not marked, magenta no fog
   block), the no-fog mark (red) or nothing (blue).
5. **Log Fog Configs**: the configurations themselves.

| Reading | Meaning |
| :-- | :-- |
| `fog-off` > 0, `markable` > 0 | Skip Unfogged marks those draws; red in the debug views shows where. Turning it off shows whether they are the difference |
| `fog-off` > 0, `unmarkable` > 0 | Fog-off materials that write depth but cannot be marked (no alpha test of their own, or 16 TEV stages) |
| `fog-off` > 0, `no-Z` > 0 | Fog-off materials that write no depth. They write nothing in the replay; the surface behind them decides the fog |
| A surface is blue in Replay Coverage | Its depth comes from something the replay does not draw (a particle, a shadow, a directly drawn packet); it takes the fallback configuration |
| A surface the game leaves unfogged is fogged, and is not red with Skip Unfogged on | It is not a fog-off material the replay reaches: check whether it draws in the sky lists (black in the debug views) or through a path the capture does not see |
| `additive` > `no-Z` | Additive blends on depth-owning geometry |
| `additive` = `no-Z` > 0 | Additive blends that own no depth; one pass cannot correct them |
| `replay failed` or `merged` | The frame fell back to configuration 0 in places |

## Changing this mod: rules

- Scene-pass pipelines are built lazily from the live layout and rebuilt on `layout.key`.
- The fog pass is pushed only from `on_xlu_list_bg_pre`. After a pin bump, confirm that
  `dComIfGd_drawXluListBG` is still an out-of-line function and is still called directly after the
  `SCENE_AFTER_OPAQUE` stage.
- Every hook is required. Keep `install_hooks` all-or-nothing.
- Keep `needs_id_buffer()` as the single test for both building and using the replay.
- The scope opens only with the sky-depth snapshot in hand; without it the fog pass would fog what
  the sky lists drew depth for.
- Keep `is_barrier_fog` an exact match.
- Do not leave blended draws on the game's fog (see [Limitations](#limitations)).
- The uniform structs (`FogUniforms` 112 bytes, `MixedFogUniforms` 336 bytes, `FogRangeUniform`
  64 bytes) are mirrored in `fog.wgsl`; keep the `static_assert`s true.
- After a pin bump, re-check every `DEFINE_HOOK` target by name in the new tree (see
  `CONTRIBUTING.md` "Moving to a newer game build"). A clean compile does not prove a hook resolves.
- Before changing anything for a visual difference, get per-pixel evidence (above).
