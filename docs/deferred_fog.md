# Deferred Fog

Applies the game's fog after the opaque world has been drawn, so screen-space effects such as
ambient occlusion darken the surfaces *under* the fog instead of darkening the fog itself.

| | |
| :-- | :-- |
| Mod id | `dev.automata.deferred_fog` (`mods/deferred_fog/`) |
| Version | `2.0.0` (see `mods/deferred_fog/mod.json`) |
| Kind | **Game-linked**: includes game headers, calls game functions and hooks eleven of them. It must be built against the game build it runs on |
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
`on_material_shared_dl_post`, `on_mat_packet_draw_pre`, `is_terrain_overlay`,
`on_scene_after_opaque`, `replay_config_ids`, `restore_packet_state`, `on_xlu_list_bg_pre`,
`draw_held_back_layers`, `push_fog_quad`, `on_draw`, `ensure_fog_pipelines`, `install_hooks`.

## Using it

### Options

The mod's pane in the Mods menu has three toggles:

| Config key | UI label | Default | Meaning |
| :-- | :-- | :-- | :-- |
| `fogEnabled` | Enabled | on | Off: the game draws its own fog |
| `fogSkipUnfogged` | Skip Unfogged Geometry | on | Leave unfogged the pixels of materials the game draws with fog switched off, as the game does. Runs the configuration-ID replay in every frame with a markable fog-off draw. See [Skip Unfogged](#skip-unfogged) |
| `fogExceptions` | Enable Exceptions | on | Keep the mod's exceptions, where it leaves the fog to the game. Today that is Wolf Senses; see [Wolf Senses](#wolf-senses). Off is for examination only |

Config keys are stored as `mod.dev.automata.deferred_fog.<key>` in the game's `config.json`. The
defaults are the second argument of the `register_bool` / `register_int` calls in `init()`.

**Diagnostics.** The Status line, the debug views and the diagnostic log lines below are in the
code but not in the released UI: `kShowDiagnostics` in `mod.cpp` is `false`, so their options
(`fogDebug`, `fogLogConfigs`) are not registered and their controls are not shown. Set it to `true`
and rebuild to get a Status line in the pane and an **Open Fog Diagnostics** button, whose window
holds the **Debug View** selector (`fogDebug`, 0 off, 1 Fog Factor, 2 Config IDs, 3 Replay
Coverage) and **Log Diagnostics** (`fogLogConfigs`). Both builds compile from the same source.

### Status line (diagnostics)

Shown with `kShowDiagnostics`. Rebuilt every frame in `on_scene_after_opaque`. The working state
reads:

```
Deferring fog (N draws, K configs[, M merged][, replay failed]; H see-through held back[ (+O in place)]; A shared-DL, B fog-off (P markable, T by alpha/Z no-Z/U unmarkable), C additive/D no-Z)
```

| Field | Meaning |
| :-- | :-- |
| `N draws` | Draws whose fog was captured and switched off this frame (each J3D shape, map-unit material and direct fog-setter call counts once) |
| `K configs` | Distinct fog configurations this frame. More than 1 runs the configuration-ID replay (so does Skip Unfogged with any markable fog-off draw) |
| `M merged` | Draws whose configuration did not fit in the 8-entry table; they use configuration 0 |
| `replay failed` | The replay could not run; the frame used configuration 0 everywhere |
| `H see-through held back` | J3D material draws held back as see-through layers and drawn after the fog pass (see [See-through layers](#see-through-layers)). `+O in place`: layers drawn in place because the list of 512 was full |
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
| `No fogged draws in view (H see-through held back)` | Nothing in the opaque world set fog this frame |
| `Sky depth snapshot failed: the game's own fog is used` | The depth snapshot at `SCENE_BEGIN` failed, so the scope stayed closed this frame; see [The sky](#the-sky) |
| `Inactive: <function> could not be hooked in this game build; the game's own fog is used` | A required hook failed to attach at load; see [Hooks](#hooks) |

### Debug views (diagnostics)

Shown with `kShowDiagnostics`. Each replaces the image with what the fog pass computes, drawn opaque
where the fog pass draws. Anything the game draws after that point (translucent geometry, particles)
still draws over it. Pixels left to the sky's own fog are black: depth 0, and depth the sky lists
wrote, such as the clouds' (see [The sky](#the-sky)).

| `fogDebug` | View | Shows |
| :-- | :-- | :-- |
| 1 | Fog Factor | The fog amount per pixel (white = full fog) |
| 2 | Config IDs | One gray level per fog configuration (white = the last), on frames that run the configuration-ID replay; otherwise Fog Factor. A frame that runs the replay only for Skip Unfogged has one configuration and shows white |
| 3 | Replay Coverage | What the configuration-ID replay recorded per pixel, and why. Green: the draw's own fog configuration. Orange: the same, on a see-through (blended) surface that writes depth, so the pixel's fog also lands on what shows through it. Yellow: its configuration did not fit in the table and uses configuration 0 (`merged`). Cyan: a fog-off draw that writes depth but is not marked (Skip Unfogged off, or `unmarkable`), stamped as configuration 0. Magenta: a draw with no fog block, stamped as configuration 0. Blue: nothing the replay draws (grass, flowers, particles and other directly drawn geometry), which takes the fallback configuration. Runs the replay every frame while selected |

Held-back see-through layers draw after the fog pass, so they appear over every debug view in their
normal colours, as translucent geometry does. In every view, **red** pixels are ones Skip Unfogged
leaves unfogged. If a debug view shows the normal scene, the fog pass did not draw this frame; an
all-black view means it drew and computed no fog.

### Log messages

Always:

- `ready` on load, or `inactive: a required game hook is missing` together with one
  `could not hook <function> in this game build` error per missing hook.
- One-time warnings: `sky depth snapshot failed; such frames use the game's own fog`,
  `configuration-ID replay failed; such frames use one fog configuration` and
  `depth snapshot failed; no fog pass this frame`.

With Log Diagnostics on (`kShowDiagnostics` builds only, `diagnostic_logging`):

- `Wolf Senses: the game's own fog is used` and `Wolf Senses over: deferring fog`.
- `per-pixel replay on: K fog configurations, P markable fog-off draws in view` and
  `per-pixel replay off` when the replay starts or stops running (`P` is 0 with Skip Unfogged off).
- The configuration table when it changes.

## How it works

### Where it acts in the frame

The world camera's part of the frame is `mDoGph_Painter` in `dusklight/src/m_Do/m_Do_graphic.cpp`.
Line numbers are at the `v2.0.0` pin. The Japanese labels are the game's own CPU-timer names for
each step.

| Line | Game | This mod |
| :-- | :-- | :-- |
| 2328 | Sky lists | Untouched; the sky keeps the game's fog |
| 2334 | `GFX_STAGE_SCENE_BEGIN` | `on_scene_begin` snapshots the depth the sky lists left and opens the capture scope |
| 2344–2390 | Opaque world lists, `Pri0_B` particles, the game's shadows | Each draw's fog is captured and switched off; see-through J3D layers are held back |
| 2395 | `GFX_STAGE_SCENE_AFTER_OPAQUE` | VBAO and SMAA composite. `on_scene_after_opaque` closes the scope, arms the fog pass and runs the replay if needed |
| 2405 | `dComIfGd_drawXluListBG`: the translucent lists begin | **`on_xlu_list_bg_pre` pushes the fog pass, then draws the held-back layers** |
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

### See-through layers

The game draws some see-through surfaces in the opaque lists: domes, glows, the base of a twilight
portal. It composites each over geometry that is already fogged, with the layer's own fog. One fog
pass after the opaque world cannot reproduce that. It would fog the layer's pixels again with the
fog of whatever is behind (or, over the sky, not at all), and a fog-off layer marked by Skip
Unfogged would unfog what shows through it.

So the mod holds such layers back. A J3D material that is depth-tested and either blends with what
is behind it (`GX_BM_BLEND` or `GX_BM_SUBTRACT`) or writes no depth is a see-through layer
(`is_see_through_layer`). One drawn without the depth test stays in place: drawn late, it would
cover opaque geometry the game drew over it. Inside the scope, the pre-hook on `J3DMatPacket::draw`,
which loads one material and draws every shape that uses it, records such a packet and skips it
(`on_mat_packet_draw_pre`). After the fog pass, `draw_held_back_layers` draws the recorded packets
in their original order. The scope is closed by then, so each draws with its own fog over the fogged
image, depth-tested against the opaque world, as in the game. The replay skips the same packets, so
they take no part in which configuration the pixels behind them get. The layers are drawn every
frame they were held back, whether or not the fog pass ran.

**Overlays on the terrain stay in place** (`is_terrain_overlay`). The terrain materials
`dKy_bg_MAxx_proc` treats as ground, by the polygon code at name positions 3..6 (`MA00`, `MA01`,
`MA04`, `MA16`: the materials that carry the cloud shadow), are never held back. While the camera is
above water it turns `MA01` into an overlay that writes no depth (`l_zmodeUpDisable`), such as a
road over the ground. Such an overlay lies on the terrain under it, under the same room fog, so both
layers have the same fog factor *f* and `a·fog(O) + (1 − a)·fog(G) = fog(a·O + (1 − a)·G)`: the fog
pass fogs the composite exactly as the game fogs each layer, and nothing about it needs the late
draw. Drawn late, a layer inherits whatever the draw before it left in the GPU state its material
does not set itself, not what the terrain draw before it left. GX light 1 is one such piece of
state: `setLightTevColorType_MAJI_sub` gives terrain materials their own lights in slots 0 and 2–7,
never 1, and each room, map unit and grass draw reloads slot 1 with its own room and light ratio
(`dKy_setLight_nowroom_common` → `dKy_GlobalLight_set`). A test build that held the road back drew
it darker than the game; it also had the replay's viewport defect (below), so which of the two
darkened it was not measured. In the replay the overlay is drawn like any other material and stamps
its own configuration, the same as the terrain's.

The list holds 512 packets; a layer that does not fit is drawn in place (`+O in place` on the Status
line). Map units (`dBgp_c`) and the self-drawing packets do not draw through `J3DMatPacket::draw`
and are not held back.

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
  particles, the game's shadows or the held-back see-through layers.
- Draws the replay cannot recolour (grass, flowers and other self-drawing packets) render their lit
  colours. A pixel with green or blue above 0.03, or a red value outside a valid slot, takes the
  **fallback configuration**: the one the grass and flower packets drew with this frame, or
  configuration 0 if there were none.
- Red 216 (C++ `kNoFogSlot` = 8) means "leave this pixel unfogged", written by Skip Unfogged
  (below).
- Blue carries why the draw got its slot (`StampReason`, as `reason/255`), read only by the
  Replay Coverage debug view. It stays below the 0.03 that marks a pixel unstamped.
- Each stamp is a small display list (`StampList`), written the way J3D materials program the GPU,
  sent after the material's display list has loaded it. It never goes through the GX API: aurora's
  API keeps its own copy of the registers that pack several settings and rebuilds a whole register
  from that copy when one setting changes, and display lists do not update the copy
  (`docs/mod-api-notes.md`). The stamp sets every field of the packed registers it touches: all of
  `genMode` (`put_gen_mode`: counts, and the material's own cull mode, so the replay draws the same
  faces as the frame) and both stages of a TEV-order pair.
- **The replay leaves no trace.** After a stamped draw, the post-hooks on `J3DMatPacket::draw` and
  `dBgp_c::modelMaterial_c::drawSimple` re-issue the draw's own display lists
  (`restore_packet_state`; the material's shared display list for a map unit) and switch its fog off
  as the capture did, so each draw ends with the GPU as the same draw left it in the frame. The
  replay draws the frame's own lists in the frame's order, so it ends with the state the opaque
  world ended with, and nothing is reset afterwards: `J3DSys::reinitGX` would leave J3D defaults (a
  null texture in every texture slot, alpha writes off, black ambient colours) under everything the
  game draws later in the frame.
- **The viewport is restored after the offscreen pass has ended.** Aurora maps a logical viewport to
  render pixels by the ratio of the current target to the logical framebuffer
  (`map_logical_viewport`), and inside an offscreen pass it takes the target itself as the logical
  size (`logical_fb_size`), so the ratio is 1. GX calls are queued and applied when the next pass
  operation drains them, so a `GXSetViewport` issued before `resolve_pass` is applied while the
  replay's pass is still current. That leaves aurora's render viewport at the logical width, a
  fraction of the screen's, and every later draw with range-adjusted fog (the game enables it) then
  takes its per-column fog factors from a table built for that width (`build_fog_range_lut`): every
  column right of it is fogged several times over. Before 2.0 the replay did exactly that, so
  translucent effects drawn after it, such as the fake light shafts, were lost to fog on most of the
  screen.
- Some draws write nothing (`stamp_nothing`), so their pixels keep the ID of the surface whose depth
  the fog pass uses there: the barrier (below) and fog-off draws that write no depth. They use a
  blend that keeps the destination.
- If the replay cannot run, the frame uses configuration 0 everywhere. The game's fog is already off
  for that frame, so this is the closest available result. The Status line shows `replay failed`.

### Skip Unfogged

A material's fog block can have type 0 (`GX_FOG_NONE`): the game then draws it with no fog at any
distance, while the fog pass would fog it like everything else. With Skip Unfogged on (the default),
the replay writes the no-fog mark (red 216) for such a material instead of a configuration, and the
fog pass leaves the marked pixels alone. A frame with a markable fog-off draw runs the replay even
with one configuration.

- Only a material that writes its own depth is marked. A J3D material that writes none, or blends,
  is a see-through layer and is held back (above), so it keeps its own fog-off look over the fogged
  surface behind it. A map-unit material that writes no depth writes nothing in the replay, so the
  surface behind it decides the fog.
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
  Deferring it would put the dome's black fog on the castle and trees behind it. A barrier material
  that is a see-through layer is held back like any other. One that reaches the capture is
  recognised by that **exact** triple (`is_barrier_fog`) and left on the game's fog; in the replay
  it writes nothing, so its pixels take the configuration behind them. The match must stay
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

Enable Exceptions (`fogExceptions`, default on) controls this exemption. Off makes the mod take over
the senses fog anyway, for examination with the debug views. The result can then differ from the
game's own look wherever the fog pass's single depth per pixel does not match the surface.

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

The mod hooks eleven game functions, all of them required:

| Hook | Purpose |
| :-- | :-- |
| `GXSetFog`, `GFSetFog` (pre) | Capture direct fog setters |
| `J3DShape::drawFast` (pre) | Capture material fog; stamp the replay |
| `dBgp_c::modelMaterial_c::drawSimple` (pre/post) | Bracket map-unit drawing; restore a stamped replay draw's state |
| `J3DMaterial`, `J3DPatchedMaterial`, `J3DLockedMaterial` `::loadSharedDL` (post) | Capture map-unit material fog; stamp the replay |
| `dGrass_packet_c::draw`, `dFlower_packet_c::draw` (pre/post) | Record the grass and flower configuration |
| `J3DMatPacket::draw` (pre/post) | Hold back see-through layers; skip them in the replay; restore a stamped replay draw's state |
| `dComIfGd_drawXluListBG` (pre) | Push the fog pass |

`install_hooks` attempts all of them. If any fails to attach (a game build this mod was not compiled
for), the capture scope never opens, the hooks that did attach have nothing to do, and the game draws
its own fog. The log names each missing hook (and, in a diagnostics build, the Status line names
the first).

### The exported service

`dev.automata.deferred_fog` (`include/deferred_fog_service.h`) has one call, `get_state`. Its
`deferring` field is true when the frame armed the fog pass at `SCENE_AFTER_OPAQUE`. Before that
stage in a frame it holds the previous frame's value, and it stays true if the depth snapshot then
fails. It is false while the mod is off, inactive or leaving Wolf Senses to the game, in a frame
whose sky-depth snapshot failed, and in a frame with no fogged draws. A mod that composites at
`SCENE_AFTER_OPAQUE` is already under the fog without importing it, and an overlay that must sit on
top of the fog can draw at `GFX_STAGE_FRAME_AFTER_HUD`. Importing the service only matters for
ordering inside one stage: hooks on a stage run in registration order, which follows load order, and
an import makes the importer load later. Nothing imports it today.

## Limitations

What one fullscreen pass over the finished opaque image cannot reproduce exactly:

- **Held-back layers draw after all opaque geometry**, not between it. A held-back layer that
  writes depth therefore no longer hides opaque geometry the game drew behind it later in the
  frame; that geometry now shows through it.
- **See-through surfaces that are not held back** (map units, self-drawing packets, layers drawn
  without the depth test): the game fogs each fragment at its own depth before blending, the fog
  pass fogs each pixel once at the depth the depth buffer holds.
  - A see-through surface that writes depth fogs what is behind it at its own, nearer, depth.
  - A surface that writes no depth is fogged at the depth of what is behind it, and over the sky
    not at all.
  - For blend factors (sᵢ, dᵢ) the two differ by `f·F·(K − 1)` where `K = Σᵢ sᵢ·Π_{j>i} dⱼ`: an
    additive blend (destination factor 1) or `GX_BM_SUBTRACT` gives `K ≠ 1`. The `additive` counter
    shows such draws.
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
The tools below need a diagnostics build (`kShowDiagnostics = true`; see [Options](#options)).

1. Read the Status line in the view.
2. **Fog Factor**: where the fog lands per pixel. A surface that keeps its own colours through the
   haze in vanilla but is gray-white here is a fog-off or depth-ownership difference; one that is
   *more* fogged than the fog colour in vanilla points at an additive blend.
3. **Config IDs**: which configuration each pixel resolves to.
4. **Replay Coverage**: per pixel, whether the replay recorded the draw's own configuration
   (green, or orange on a see-through surface that writes depth), configuration 0 for a reason
   (yellow merged, cyan fog-off not marked, magenta no fog block), the no-fog mark (red) or nothing
   (blue).
5. **Log Diagnostics**: the configurations themselves.

| Reading | Meaning |
| :-- | :-- |
| `fog-off` > 0, `markable` > 0 | Skip Unfogged marks those draws; red in the debug views shows where. Turning it off shows whether they are the difference |
| `fog-off` > 0, `unmarkable` > 0 | Fog-off materials that write depth but cannot be marked (no alpha test of their own, or 16 TEV stages) |
| `fog-off` > 0, `no-Z` > 0 | Fog-off materials that write no depth. They write nothing in the replay; the surface behind them decides the fog |
| A surface is blue in Replay Coverage | Its depth comes from something the replay does not draw (a particle, a shadow, a directly drawn packet); it takes the fallback configuration |
| A surface the game leaves unfogged is fogged, and is not red with Skip Unfogged on | It is not a fog-off material the replay reaches: check whether it draws in the sky lists (black in the debug views) or through a path the capture does not see |
| `see-through held back` rises with a surface in view | That surface is drawn after the fog pass with its own fog. A difference in its shading (not its fog) is state it inherited from the draws before it, which differ from the game's order: see the terrain-overlay exception in [See-through layers](#see-through-layers) |
| A difference appears only with Skip Unfogged on | The replay runs in that frame; it must leave the GPU state, and aurora's viewport, as the opaque world left them (see the replay section) |
| `additive` > 0 | Additive or subtractive blends that are not held back (map units, other paths) |
| `replay failed` or `merged` | The frame fell back to configuration 0 in places |

## Changing this mod: rules

- Scene-pass pipelines are built lazily from the live layout and rebuilt on `layout.key`.
- The fog pass is pushed only from `on_xlu_list_bg_pre`. After a pin bump, confirm that
  `dComIfGd_drawXluListBG` is still an out-of-line function and is still called directly after the
  `SCENE_AFTER_OPAQUE` stage.
- Every hook is required. Keep `install_hooks` all-or-nothing.
- Keep `needs_id_buffer()` as the single test for both building and using the replay.
- Release builds keep `kShowDiagnostics` false: the pane shows Enabled, Skip Unfogged Geometry and
  Enable Exceptions only. Keep the diagnostics compiling with it set either way.
- The scope opens only with the sky-depth snapshot in hand; without it the fog pass would fog what
  the sky lists drew depth for.
- Keep `is_barrier_fog` an exact match.
- Do not leave blended draws on the game's fog in place: the fog pass would still fog their pixels.
  Hold them back (`is_see_through_layer`) instead.
- Held-back layers must be drawn in every frame they were held back: `draw_held_back_layers` runs
  from `on_xlu_list_bg_pre` whether or not the fog pass did.
- Do not hold back an overlay that lies on the surface under it with the same fog: in place it is
  exact, and drawn late it inherits other draws' state (`is_terrain_overlay`).
- The replay must leave no trace: stamp with display lists, restore each stamped draw's own
  display lists after it, and never reset GX state afterwards (no `J3DSys::reinitGX`, no GX API
  calls on packed registers).
- Restore the GX viewport and scissor only after `resolve_pass` has ended the replay's offscreen
  pass, never inside it.
- The uniform structs (`FogUniforms` 112 bytes, `MixedFogUniforms` 336 bytes, `FogRangeUniform`
  64 bytes) are mirrored in `fog.wgsl`; keep the `static_assert`s true.
- After a pin bump, re-check every `DEFINE_HOOK` target by name in the new tree (see
  `CONTRIBUTING.md` "Moving to a newer game build"). A clean compile does not prove a hook resolves.
- Before changing anything for a visual difference, get per-pixel evidence (above).
