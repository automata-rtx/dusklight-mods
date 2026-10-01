# Unreleased mods (potentially outdated)

> **Read this first.** Everything in `docs/unreleased/` describes mods that are **not built, not
> released, and not maintained**. They are commented out of the top-level `CMakeLists.txt`. Their
> designs were last exercised on the **retired fork platform** (`automata-rtx/dusklight-ao`), not on
> the current upstream Dusklight `v2.0.0` pin, and the documents were not re-audited when the
> released mods' docs were. Treat every claim here as a lead to verify against the code and the
> fetched game tree, not as a fact. Where these documents disagree with `CONTRIBUTING.md`,
> `CLAUDE.md` or the released mods' docs, those win.

The released mods are VBAO, Deferred Fog and SMAA. Start from `CONTRIBUTING.md` for those.

## Status at a glance

| Mod | Source | Last `mod.json` version | Kind | What blocks it from the build |
| :-- | :-- | :-- | :-- | :-- |
| SSILVB | `mods/ssilvb` | 0.10.0 | service-only | Still imports the retired `dev.automata.depth_to_normal` provider service, which no longer exists. Needs the same conversion VBAO got: read normals through `gfx_compat::request_normal` / `resolved_normal` (`common/gfx_normal_compat.h`) and rebuild its scene-pass pipelines on `GfxDrawContext::layout.key`. It may be retired instead; this has not been decided. |
| Realtime Sun Shadows | `mods/realtime_sun_shadows` | 1.15.0 | game-linked | The same normal conversion and layout-key rebuild, plus it needs **two** normals (authored for `n·L`, a depth-derived face normal for the bias, see `docs/authored_normals.md` §8.6). Its hook targets also need re-verifying against the pin. |
| Celestial Orbit | `mods/celestial_orbit` | 1.0.0 | game-linked | Hook targets not re-verified against the pin (see "Re-platforming" in `CONTRIBUTING.md`). No normal use. |
| Effect Remover | `mods/effect_remover` | 1.0.0 | game-linked, experimental | Hook targets not re-verified against the pin. Its line citations were updated with everyone else's. |

To bring one back: port it, uncomment its `add_subdirectory` line in `CMakeLists.txt`, build, test
in-game, then move its documentation out of this folder and give it the same audit the released
mods' docs had. Re-enable one mod at a time.

## Documents in this folder

| File | About | Notes |
| :-- | :-- | :-- |
| `ssilvb_plan.md` | SSILVB design, algorithm and working model | §0 records the owner's standing instruction that SSILVB's technical direction rests with the AI assistant |
| `ssilvb_environment_light.md` | SSILVB's environment probe (0.10.0) | |
| `realtime_sun_shadows.md` | Realtime Sun Shadows design, tunables, shading history, streaming-buffer budget | The budget section predates upstream raising aurora's buffers to Vertex 5 MB / Index 2 MB / Storage 8 MB |
| `celestial_orbit.md` | Celestial Orbit | |
| `fake_shading_systems.md` | The game's built-in fake-shading systems Effect Remover targets, and four more it does not | Also useful background for anyone working on fog or AO, because it explains what the game already draws |

`docs/historical/` holds the design documents for the retired Depth to Normal provider. Those are
history, not a guide.

## Per-mod summaries

These were moved out of `CLAUDE.md` and `README.md` when the docs were split. They describe the
mods as they stood when they were last built.

### SSILVB

"Screen Space Indirect Lighting with Visibility Bitmask" (Therrien et al. 2023; the mod carries the
paper's name). It extends VBAO's bitmask sampling chain with a one-bounce indirect-diffuse
accumulate. With the bounce switched off it doubles as a standalone directional-AO mod. It consumes
the scene-colour snapshot as its light input and per-sample normals for the bounce, and composites
GI additively and AO multiplicatively in a single blend draw.

Since 0.10.0 it also carries an **environment probe**: a persistent world-space ambient cube (6 axes
plus coverage confidence, an 8×1 texture) measured from MIP 4 of its own colour chain in one
workgroup. It is evaluated in each slice's bent direction and applied through the sectors the march
found *nothing* in, so it fills exactly the light the bounce cannot see (off-screen, beyond radius)
without double counting. It persists across frames per direction, which stops light popping at the
screen edge. The older sky-only ambient remains as the fallback when the probe is off.

Service-only. Docs: `ssilvb_plan.md` (read §0 first) and `ssilvb_environment_light.md`.

**Interaction with Deferred Fog:** Deferred Fog stops deferring while Wolf Senses is active, on the
grounds that black fog commutes with *multiplicative* composites. SSILVB's bounce is *additive*, so
in senses it would add light on top of the black fog instead of under it. Revisit that if SSILVB
returns.

### Realtime Sun Shadows

Real-geometry sun/moon cascaded shadow maps: the game's draw lists are replayed into up to three
nested light-space depth passes, plus an optional Link-only cascade. PCF, receiver-plane and slope
bias, a sin-scaled normal-offset receiver, two-sided casters, Bend-style screen-space shadows, and
indoor auto-disable. Game-linked.

Points that cost time to learn:

- It does **not** hook `drawCloudShadow`. Despite the name, that is the moya (靄, haze) packet, not a
  shadow. Suppressing it was the cause of the long-standing "distortion particles vanish" bug. Moya
  belongs to Effect Remover's Haze Removal.
- **Two normals, never interchangeable.** The *shading* normal (the game's authored one) drives
  `n·L`, attached shadows and the normal offset. The *geometric* face normal drives the bias. See
  `realtime_sun_shadows.md` "Shadow term assembly" and `docs/authored_normals.md` §8.6.
- The `normalSmooth` blur pass was deleted. Do not reintroduce it. It existed to hide reconstruction
  faceting, which authored normals remove at the source, and it flattened real curvature.
- The two shading problems that were once open (harsh faceting, broken shading on back-lit Link)
  were both closed and confirmed in-game on the fork platform; see "Shading history" in
  `realtime_sun_shadows.md`. Faceting then appeared only on the reconstruction fallback.
- Debug View 15 ("Shadow Terms") separates a missing occluder from a misread `n·L`, which look
  identical otherwise.
- `linkCascade` on also removes Link from the world cascades instead of drawing him into both: the
  composite takes `max()` of the cascades, so a coarse map would otherwise override the crisp one.
- It derives its light direction from the time of day rather than reading `sun_pos`, so it imports
  the Celestial Orbit service (a soft dependency) to follow a retilted sun/moon path.
- Its cascade replays are the heaviest consumer of aurora's per-frame streaming buffers. At
  upstream's current sizes that is a framerate concern, not a crash risk.

### Celestial Orbit

Raises the sun/moon travel path. TP sweeps both bodies around a great circle tilted so its peak is
only 59° (`z = y * 48000/80000`), which limits how expressive realtime shadows can be. The mod
post-hooks `dScnKy_env_light_c::setSunpos` and re-derives `z` from `y` with
`ratio = cot(peak elevation)`, capped at 80° (at 90° the arc crosses the zenith and a shadow map's
light-space up vector degenerates), plus an optional yaw of the whole orbit plane. The sweep itself
is untouched, so timing, the palette schedule and day/night transitions do not change.

It exports the orbit as `dev.automata.celestial_orbit` with a shared
`celestial_orbit_apply_offset()` that both it and Realtime Sun Shadows apply, so they cannot drift
apart. Its on/off option is `orbitEnabled`: an option named `enabled` collides with the mod
manager's own checkbox and kept this mod from loading at all (see `docs/mod-api-notes.md`).
Game-linked. Docs: `celestial_orbit.md`.

### Effect Remover

A combination mod that cuts down TP's built-in fake shading so it does not fight the realtime
stack. It merges three former standalone mods, each in its own namespace inside `src/mod.cpp`
(`er_psr`, `er_tsr`, `er_vu`) with its own UI section and independent config. Game-linked,
experimental. `fake_shading_systems.md` describes the three systems it targets, four more that the
same `dKy_bg_MAxx_proc` sets up that it does not, and the code names.

- **Haze Removal** (`er_psr`; the internal name and `psr*` config keys were kept so saved settings
  survive the rename). Pre-hooks `drawCloudShadow` and cancels it per `mMoyaMode`. Moya is
  camera-facing haze billboards drawn with the depth test disabled, and five of its twelve modes
  blend additively, so they can only brighten. The dappled forest floor is a different system (the
  terrain TEV stage `er_tsr` targets). Mode assignment is in code, not map data: mode 4 comes only
  from `d_a_kytag02`, and Hyrule Field's haze is mode 7 (`d_kankyo_wether.cpp:1111`), so the UI's
  "keep mode 4 for Hyrule Field's shadows" advice is wrong on both counts. The default removes only
  mode 5. `mMoyaMode >= 50` (heat shimmer and Wolf Senses) is always preserved. There is a live mode
  logger.
- **Terrain Shadow Removal** (`er_tsr`). The other fake shadow: a drifting dapple overlay baked as a
  second TEV texture stage inside the terrain material. `dKy_cloudshadow_scroll` scrolls texmtx 1
  of `MA00`/`MA01`/`MA16` with the `vrkumo` packet (the sway). `dKy_bg_MAxx_proc` sets TEV KColor 1's
  red to `g_env_light.mFogDensity` on `MA00`/`MA01`/`MA04`/`MA16`. That field is **not** fog density:
  the game's own debug slider labels it 雲影の濃さ, cloud-shadow density (`docs/japanese-naming.md`
  §4.1). In-game, 0 is darker and the maximum is washed out, so `er_tsr` post-hooks
  `dKy_bg_MAxx_proc` and pins KColor 1's red to 255. That is white into the shadow stage with the
  base ground stage untouched, so it does not hole the floor. The engine agrees: it forces
  `mFogDensity = -1` (read as 255) while Wolf Senses is active (`d_kankyo.cpp:2423`), where it
  deliberately flattens the look. `MA04` is the confirmed Faron forest-floor shade. The hook fires on
  seven actors, not just room terrain (two of them water). Per-code toggles plus a logger. Off by
  default because it is a global terrain change.
- **Unbaked Vertex Lighting** (`er_vu`). Post-hooks the J3D model loader
  (`J3DModelLoaderDataBase::load` / `loadBinaryDisplayList`) and rewrites each model's CLR0/CLR1
  vertex-colour arrays in place: `rgb' = mix(white, rgb, vertexLight/100)`, so 100 is vanilla and 0
  is flat. Alpha is untouched; all six GX colour formats are handled. It applies as models load, so
  re-enter the area after changing it.

Effect Remover's config var names, for anyone editing defaults: Haze Removal `psrEnabled`,
`psrLogMode`, `psrSuppress0` to `psrSuppress11`; Terrain Shadow Removal `tsrEnabled`, `tsrLog`,
`tsrRemoveMA00`, `tsrRemoveMA01`, `tsrRemoveMA16`, `tsrRemoveMA04`; Unbaked Vertex Lighting
`vertexLight`. Most toggles go through a local `register_bool(name, default, handle, error)` helper,
so the default is the second argument.
