# dusklight-mods

Graphics mods for Dusklight (the Twilight Princess PC/mobile port), built on its mod API:

- **`mods/vbao/`** — "VBAO" (Visibility Bitmask Ambient Occlusion): a 32-sector
  visibility-bitmask AO estimator with temporal accumulation, edge-aware denoise, and
  depth-aware compositing. A distinct technique from Encounter's GTAO demo mod (its original
  framework). **Service-only**: it uses only mod-API services (gfx, camera, config, ui,
  resource, log) — it must NOT include game headers or call game code, which is what lets it
  survive game updates without a rebuild.
  **OPEN BUG: temporal flicker, overwhelmingly on AMD GPUs (two NVIDIA reports too).** The user's
  own reading, which the evidence supports: it is the TEMPORAL path. Disabling Temporal Accumulation
  removes the flicker; Motion Response 0-1 with accumulation on almost entirely removes it; the
  wrong-looking AO (improper angles, hard edges) is probably motion-only, i.e. it is the raw
  single-frame estimate being displayed whenever the velocity term drives the blend weight to 1.
  A first pass misread it as "30 fps, frame interpolation off"; **that was wrong** (interpolation
  is on by default in the shipped build). What is established, all read in the pinned upstream
  source: nothing in the depth/normal snapshot path, viewport, camera identity under
  interpolation, projection convention, uniform staging or reversed-Z differs per vendor. Three
  changes stand: the velocity term is ceilinged by a frame-time-aware cap
  (`kVelocityFusionFrameTime`), the **default Motion Response is 2** (the field-validated
  range; the history is reprojected, so camera motion alone never needed a full reset), and the
  noise **LUT is gone** - the Hilbert index is computed in-shader, because a game-thread texture
  upload at init on a host that does not enable Dawn's implicit device synchronization is the one
  path in the chain that genuinely can differ per driver, and a LUT reading as zero produces
  exactly "directional, hard-edged AO that changes every frame". A driver-level cause cannot be
  excluded; it also cannot be proven from here. **1.0.2 result: the flicker is gone in the field**;
  it traded for ghosting (moving-occluder trails, e.g. Link's contact shadow on ground he left),
  answered by a σ-normalised history outlier test against the local mean plus a motion-tightened
  clamp - not by bringing the velocity reset back. **Debug views 5-8 (Geo Normal, Normal Agreement,
  Raw AO, Depth MIP 3)** and the `adapter:` log line exist to localise it from an affected
  machine; `docs/vbao.md` "AMD report: status" is the protocol and the record.
- **`mods/realtime_sun_shadows/`** — "Realtime Sun Shadows": real-geometry sun/moon cascaded
  shadow maps (game draw-list replay into up to 3 nested light-space depth passes, plus an
  optional Link-only cascade) with PCF, receiver-plane + slope bias, sin-scaled normal-offset
  receiver, two-sided casters, Bend-style screen-space shadows, and indoor auto-disable.
  **Game-linked**: it includes game headers and hooks game functions, so it is coupled to the
  pinned game build. It does **not** hook `drawCloudShadow` — that is the moya haze packet, not a
  shadow, and suppressing it was the cause of the long-standing "distortion particles vanish"
  bug; moya belongs to Effect Remover's Haze Removal. **Two normals, never interchangeable**: the *shading* normal (the game's
  authored one) drives `n·L` / attached shadows / normal offset, the *geometric* face normal drives
  the bias — see `docs/realtime_sun_shadows.md` "Shadow term assembly" and
  `docs/authored_normals.md` §8.6. The `normalSmooth` blur pass was **deleted**; do not
  reintroduce it — it existed to hide reconstruction faceting, which authored normals remove at the
  source, and it flattened real curvature. The two shading problems that used to be open here (harsh
  faceting, and broken shading on back-lit Link) are **both closed and confirmed in-game**; see
  "Shading history" in `docs/realtime_sun_shadows.md`. Faceting now appears only on the
  reconstruction fallback, which now means only the compatibility renderers. Debug View
  15 ("Shadow Terms") is the view that separates a missing occluder from a misread `n·L` — they look
  identical otherwise.
  **`linkCascade` on also removes Link from the world cascades** rather than drawing him into
  both: the composite takes `max()` of the cascades, so a coarse map would otherwise override the
  crisp one.
  Derives its light direction from the time of day rather than reading `sun_pos`, so it imports
  the **Celestial Orbit** service (soft dependency) to follow a retilted sun/moon path.
- **`mods/celestial_orbit/`** — "Celestial Orbit": raises the sun/moon travel path. TP sweeps both
  bodies around a great circle tilted so its peak is only 59° (`z = y * 48000/80000`), which caps
  how expressive realtime shadows can be; the mod post-hooks `dScnKy_env_light_c::setSunpos` and
  re-derives `z` from `y` with `ratio = cot(peak elevation)` (**capped at 80°** — at 90° the arc
  crosses the zenith and a shadow map's light-space up vector degenerates), plus an optional yaw
  of the whole orbit plane. The sweep is untouched, so no timing, palette schedule, or day/night
  transition changes. Exports the orbit as `dev.automata.celestial_orbit` with the shared
  `celestial_orbit_apply_offset()` both it and Realtime Sun Shadows apply, so they cannot drift
  apart. **Game-linked**. Docs: `docs/celestial_orbit.md`.
- **`mods/ssilvb/`** — "SSILVB" (Screen Space Indirect Lighting with Visibility Bitmask,
  Therrien et al. 2023 — the mod carries the paper's name): VBAO's bitmask sampling chain extended
  with a one-bounce indirect-diffuse accumulate; with the bounce toggled off it doubles as a
  standalone directional-AO mod. Consumes the scene-color snapshot as its light input and the
  (**not currently built** — it still imports the retired depth-to-normal service and needs the same
  resolve-based normal conversion VBAO and SMAA got) the provider's normal for per-sample normals; composites GI additively and AO multiplicatively in a single blend draw. Since 0.10.0 it
  also carries an **environment probe**: a persistent world-space ambient cube (6 axes + coverage
  confidence, 8×1 texture) measured from MIP 4 of its own colour chain in one workgroup, evaluated
  in each slice's bent direction and applied through the sectors the march found *nothing* in — so
  it fills exactly the light the bounce structurally cannot see (off-screen, beyond radius) with no
  possibility of double counting. It persists across frames per-direction, which is what stops
  light popping at the screen edge. The old sky-only ambient remains as the fallback when the probe
  is off. **Service-only**. Docs: `docs/ssilvb_plan.md` (§0 first — see the note below) and
  `docs/ssilvb_environment_light.md`.

- **`mods/smaa/`** — "SMAA" (subpixel morphological antialiasing): a spatial post-process AA mod
  (SMAA 1x). Edge detection unions the reference SMAA luma detector with **geometric edges from the
  game's own authored normals** (resolved alongside colour and depth; normal-angle + relative-depth
  discontinuity —
  catches silhouettes and creases where luma contrast is weak). Since those normals are smooth
  rather than per-triangle flat, `normalThreshold` defaults to 5% (~18°) where the reconstruction
  era needed 10% to mask facet noise. `edgeThreshold` (luma) defaults to **20%**, double the SMAA
  reference: 10% is tuned for high-contrast modern rendering and lit up ordinary texture detail on
  TP's flatter art, and the geometric detector now covers what it was compensating for. The expensive blend-weight pass uses **CMAA2-style compute
  compaction** (Intel 2018): edge pixels in each 16×16 workgroup are packed into contiguous threads
  via a groupshared list so sparse edges run in fully-occupied warps. Composites at
  `SCENE_AFTER_OPAQUE` (before bloom/translucency, so the game's post effects operate on
  antialiased geometry). Three passes: edge-detect (compute) → compacted blend-weights (compute) →
  neighborhood blend (draw). No LUT assets — orthogonal search is linear, coverage analytic; v1
  defers diagonals/corners. **Service-only** (gfx/config/ui/resource/log); depends on no other mod.
  The SMAA algorithm is reimplemented from the MIT reference (iryoku/smaa) — Marty's proprietary
  iMMERSE port was studied for the optimization ideas only, never copied. Docs: `docs/smaa.md`.
- **`mods/deferred_fog/`** — "Deferred Fog": suppresses the game's per-draw fog during the opaque
  world lists and re-applies it (bit-exact aurora fog math, `src/fog_math.h`) as one fullscreen pass
  right before the translucent lists, so AO/shadows darken surfaces *under* the fog instead of
  darkening the fog itself. Mixed fog configs take an exact per-pixel replay (default) or
  auto-revert to vanilla.
  **PORTED TO UPSTREAM 2.0 AND BACK IN THE BUILD, BUT NOT RE-VERIFIED IN-GAME.** It compiled clean
  against the new base with no source change, which proves nothing for a game-linked mod; what the
  port took was re-checking all ten hook targets by symbol, moving its four scene-pass pipelines onto
  the `layout.key` rebuild, and correcting the drifted source-line citations. **Its pipelines are the
  most latch-exposed in the repo and not through anything it does**: it never asks for normals, VBAO
  does, and the pass changes shape under it — so if the quad ever vanishes only when VBAO is also on,
  check `ensure_fog_pipelines()` first. Everything below marked confirmed in-game was confirmed on
  the retired fork platform.
  **ONE KNOWN-UNFIXED BUG, PARKED BY THE USER — AND THE FIRST THING TO READ.** Distant landmarks
  (Death Mountain, the Ganon barrier) are brighter with the mod OFF. **Three fixes have shipped for
  this and all three failed**; the user has accepted the mod as-is for now, so nobody is working it.
  The live trap is that the evidence still reads forward convincingly to fix #3: the Death Mountain
  view measures `3 fog-off, 0 additive`, which says geometry the game draws with fog switched OFF is
  being fogged by the quad and refutes the `K`-factor mechanism *for that view*, and the screenshots
  agree independently (the mountain keeps its own colours in vanilla and washes to the haze colour
  with the mod — a silhouette difference, not a fog-coloured one). That reasoning produced
  `fogSkipUnfogged`. **The user turned it on and Death Mountain did not change.** It is still in the
  build, default-off, as a diagnostic. The negative result disproves the *fix*, not mechanism 2 — the
  reading that separates those, the Status line's `markable / no-Z / alpha` breakdown, was never
  captured for that view. **Do not propose a fourth mechanism from an aggregate counter**: twice now
  a per-frame measurement has correctly identified a mechanism *present* in the view without
  identifying what the view actually looks like. That needs per-pixel evidence (the Fog Factor view).
  **`docs/deferred_fog.md` opens with a STATUS section — start there.**
  It reproduces **fog range adjustment** ("XFog"), the per-column multiplier GX applies to the fog
  term because screen-edge pixels are further from the eye than their Z says: TP enables it globally
  (`d_kankyo.cpp:1257`) and aurora implements it, so omitting it flattened a horizontal gradient
  vanilla has — small for near fog, double-digit percentage points for the narrow far-*starting*
  bands distant haze uses. `docs/deferred_fog.md` used to claim aurora ignored it; that was false.
  **Not every draw goes through `J3DShape::drawFast`.** `dBgp_c` map units (the shared, instanced
  pieces a stage is assembled from — how much of the field's distance they account for is NOT
  established) call `loadSharedDL()` and then `J3DShapeDraw::draw()` directly, so the material
  display list re-issues `J3DGDSetFog` after the packet's `GXSetFog` was suppressed — double fog,
  and unstamped geometry in the replay. A post-hook on `loadSharedDL`, **bracketed to `dBgp_c` by a
  `drawSimple` hook**, closes it; the bracket is required, because every other `loadSharedDL` caller
  sets its fog *after* the display list, so registering that fog would invent a config vanilla never
  draws with.
  **WHERE THE QUAD LANDS IS NOT A FIXED POINT IN THE FRAME.** It wants to go in right after every
  mod's `SCENE_AFTER_OPAQUE` composite and before the translucent lists (`m_Do_graphic.cpp:2404`,
  one line before `dComIfGd_drawXluListBG`) — but there is no stage hook there and every list entry
  point inlines, so the mod anchors on the first `J3DShape::drawFast` after the stage closes. A view
  with **no translucent J3D at all** would fall through to `FRAME_BEFORE_HUD` (`:2795`) — after
  every particle pass
  and **bloom** (`:2663`, on by default). Fog applied after bloom is fog the bloom never saw, so the
  bright distant subjects vanilla blooms hardest come out dimmer. (`motionBlure` at `:2483` is
  **not** motion blur — it is a previous-frame blend gated off in ordinary play — and `drawDepth2`
  is DOF, gated on auto-focus. Neither belongs in that list; reading the symbol name as a
  description is exactly what `docs/japanese-naming.md` warns about.) A pre-hook on
  `mDoGph_gInf_c::bloom_c::draw` is now the fallback; the Status line reports which anchor fired
  (`[at translucents]` / `[before bloom]` / `[AFTER BLOOM]`). **The premise that an open field view
  contains no translucent J3D is contradicted by in-game testimony** — the user reports the field
  does have translucencies — so the fallback is probably dead code there and the anchor readout is
  how you check rather than assume.
  **The `K` factor is the exact statement of what a fullscreen pass can reproduce.** Aurora fogs the
  fragment *source* inside the fragment shader (`shader.cpp:1543`) and the GX blend is a pipeline
  blend state applied after (`gx.cpp:332-338`), so for layers with GX factors `(sᵢ, dᵢ)` the two
  orders differ by `f·F·(K−1)`, `K = Σᵢ(sᵢ·Π_{j>i}dⱼ)`. An ordinary alpha blend over an opaque base
  has **K = 1 — bit-exact**; only a destination factor other than `1−src` (additive, or
  `GX_BM_SUBTRACT` → ReverseSubtract One/One) gives `K ≠ 1`. And `mix(scene,F,f)` converges to
  exactly `F` as `f→1` while vanilla converges to `K·F`, so at extreme range the quad clamps every
  pixel to the haze colour while vanilla can be a *multiple* of it — which is what "distant geometry
  overpowers the fog" means arithmetically. **A "blended draws keep vanilla fog" rule was tried and
  reverted**: it exempted the `K = 1` draws that were already exact and got them fogged twice.
  **Separately, a material's fog can simply be OFF** (`mType == 0`): `J3DFog::load()` programs a real
  `GX_FOG_NONE` and `setLightTevColorType_MAJI_sub` refuses to overwrite such a block
  (`d_kankyo.cpp:4429`), so vanilla applies literally zero fog however distant — and it is invisible
  to the `GXSetFog` hooks because `J3DGDSetFog` writes raw BP commands. `fogSkipUnfogged` (default
  **off**) marks those pixels with a config-ID sentinel the shader skips; it may only mark a material
  that **owns its depth** and whose **alpha test is trivial**, and it forces the ID replay. **This is
  fix #3 above and it did not resolve Death Mountain in-game** — the mechanism is real in the game
  source and the option is sound; it is simply not the answer to the open bug.
  **Measure before theorising**: the Status line carries per-frame `fog-off`, `additive/no-Z` and
  `shared-DL` counts plus the quad anchor, precisely because two fixes here were built on plausible
  mechanisms with no per-view measurement behind them.
  `is_barrier_fog` keeps its exemption because black fog over 1000..250000 in the config table is
  worse still: That test matches the **exact literal triple** the actor
  writes (black, `startZ 1000`, `endZ 250000`) — it used to be `black && endZ > 100000`, which also
  matched the game's own `mType = 7` black-fog sentinel (which forces the fog *colour* black over the
  room palette's range) on the water family and `MA20`, double-fogging them in any room with a
  distant palette fog. **TP has no near-fog/distant-scenery-fog split**: every
  config in a frame carries the same near/far from the one live view, so the old `widest_far_index()`
  (ranked by far plane) was a long way of writing `return 0`. What TP widens for distant scenery is
  the CPU clipper, which never touches fog. That helper is now **deleted** — both its callers have
  exact answers instead of a ranking.
  **The uncovered-pixel fallback is the config the SELF-DRAWING packets used.** Grass
  (`dGrass_packet_c`) and flowers (`dFlower_packet_c`) emit raw GX batches after replaying their own
  material lists, so the replay's flat-ID override structurally cannot reach them and every grass
  pixel lands on the fallback; a bracket hook on those two draws records the config their own fog
  setter resolved to. Ranking the fallback by widest `endZ` instead — briefly, on this branch — is
  the *weakest* fog at any depth, so grass stopped darkening with distance and read as lit right
  next to the camera. Widest-`endZ` now resolves only the barrier dome.
  Mixed-scene mode defaults to **Exact**: most outdoor scenes mix configs, and Vanilla hands those
  back to forward fog, i.e. AO on top of the fog again in exactly the scenes the mod exists for.
  Diagnostic: `fogLogConfigs` dumps the frame's captured fog-config table; the Status line reports
  how many shared-DL materials carried live fog. **Game-linked** + webgpu.
  Docs: `docs/deferred_fog.md`.

  **No other mod depends on it, and that is deliberate.** It exports `dev.automata.deferred_fog`
  (a one-call state query) because the mod API has no priority field on a stage hook — hooks run in
  registration order, registration follows `mod_initialize`, and the loader initializes in
  dependency order, so importing a service is the only lever for "init that one first". But the
  lever is rarely needed: a mod compositing at `SCENE_AFTER_OPAQUE` is already ahead of the fog quad
  (`FRAME_BEFORE_HUD`) by stage separation. VBAO briefly imported it so its debug views could sit on
  top of the fog; drawing those at `FRAME_AFTER_HUD` — the last stage in the frame — achieves the
  same thing with no coupling, which is what it does now. Reach for the import only if you need to
  interleave *within* a stage. See `mods/deferred_fog/include/deferred_fog_service.h`.

  **Graphics Hub is RETIRED.** It bundled this with a "Depth to Normal" provider that reconstructed
  a world-space normal from depth and published it as a service. GfxService 1.3's normal snapshot
  supersedes that completely — the renderer hands every mod the game's *authored* normal directly — so
  the provider had nothing left to do and the combination had no reason to exist. `docs/
  depth_to_normal_plan.md` and `docs/depth_to_normal_consumers.md` are marked historical.

- **`mods/effect_remover/`** — "Effect Remover": a **combination mod** that cuts down TP's built-in
  fake-shading so it doesn't fight the realtime stack. It merges three former standalone mods, each
  in its own namespace inside `src/mod.cpp` (`er_psr` / `er_tsr` / `er_vu`) with its own UI section
  and independent config:
  - **Haze Removal** (`er_psr` — internal name and `psr*` config keys kept so saved settings
    survive the rename): pre-hooks `drawCloudShadow` and cancels it **per
    `mMoyaMode`**. **Despite the feature's name, moya (靄, mist/haze) is not a projected ground
    shade**: it is camera-facing haze billboards drawn with the depth test disabled
    (`d_kankyo_rain.cpp:4748`), and five of its twelve modes blend additively so they can only
    brighten (`:4587`). The dappled forest floor is a *different* system — the terrain TEV stage
    `er_tsr` targets. Mode assignment is in **code**, not map data: mode 4 comes only from
    `d_a_kytag02`, and **Hyrule Field's haze is mode 7** (`d_kankyo_wether.cpp:1111`), so the UI's
    "keep mode 4 for Hyrule Field's shadows" advice is wrong on both halves. Default removes only
    mode 5. `mMoyaMode >= 50` (heat-shimmer / senses) always preserved. Live mode logger.
    See `docs/fake_shading_systems.md` §1.
  - **Terrain Shadow Removal** (`er_tsr`): the *other* fake shadow — a drifting dapple **overlay
    baked as a second TEV texture stage inside the terrain material** (not moya — moya count reads 0
    there). `dKy_cloudshadow_scroll` scrolls **texmtx 1** of `MA00`/`MA01`/`MA16` by the `vrkumo`
    packet (the sway); `dKy_bg_MAxx_proc` sets **TEV KColor 1**'s red to `g_env_light.mFogDensity`
    on `MA00`/`MA01`/`MA04`/`MA16` — **which is not fog density**: the game's own slider labels that
    field 雲影の濃さ, *cloud shadow* density, so this feature overrides the game's own cloud-shadow
    strength control (`docs/japanese-naming.md` §4.1). That red measures as a *wash-out* control
    (in-game test: 0 = **darker**, max = washed out), so `er_tsr` **post-hooks `dKy_bg_MAxx_proc`**
    and pins KColor 1's red to **255** — white into the shadow stage, base ground (stage 0)
    untouched, so it **does not hole the floor**. The polarity is **corroborated by the engine's
    own usage**: the game forces `mFogDensity = -1` (read as 255) in the wolf's enhanced-senses
    state (`d_kankyo.cpp:2423`), where it deliberately flattens the look — so 255 is the engine's
    own "no cloud shadow" value. The TEV equation in the `.bmd` is still unread but cannot change
    what 255 does. **`MA04` is the confirmed Faron forest-floor shade.** Note the hook fires on
    **seven** actors, not just room terrain (two of them water) — see `docs/fake_shading_systems.md`
    §2. Per-code toggles + logger. Off by default (global terrain change).
  - **Unbaked Vertex Lighting** (`er_vu`): post-hooks the J3D model loader
    (`J3DModelLoaderDataBase::load`/`loadBinaryDisplayList`) and rewrites each model's CLR0/CLR1
    vertex-color arrays in place — `rgb' = mix(white, rgb, vertexLight/100)` — 100 = vanilla, 0 =
    flat; alpha untouched; all six GX color formats; applies as models load (re-enter the area).

  **Game-linked**. EXPERIMENTAL. See `docs/fake_shading_systems.md` for the three systems Effect
  Remover targets, the **four more** the same `dKy_bg_MAxx_proc` sets up that we do not (§4), and
  the code names.

  **Working mode (user's explicit standing instruction): the technical direction of SSILVB rests
  with Claude.** The user is an amateur on SSAO/SSGI internals and cannot provide technical
  direction on the algorithm, math, or rendering architecture — never block on them for such
  decisions or offer them implementation options to pick from. They provide in-game testing,
  screenshots, and taste-level feedback ("too strong", "flickers here"); translate that feedback
  into fixes yourself. Full statement: `docs/ssilvb_plan.md` §0.

Each mod is `src/mod.cpp` (host code: pipelines, config vars, UI panel) plus `res/*.wgsl`
(shaders). **For tuning rather than building — changing a default, hiding a control, editing a
front-facing description, adding an icon or banner — `docs/editing-options.md` is the short
answer to all of them, written for someone who does not want to read the rest of the codebase.** Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`,
`docs/deferred_fog.md`, `docs/celestial_orbit.md`, and `docs/mod-api-notes.md` (pitfalls — read
before touching uniforms or render code).

## The game's code is named in Japanese, and our mods are built on those names

**Every game identifier our game-linked mods hook, read or include is the original
Japanese team's name, preserved 1:1 by the decompilation** — romaji, abbreviated
Japanese, and English spelled by ear. `drawCloudShadow`, `mMoyaMode`, `dKy_bg_MAxx_proc`,
`mpVrkumoPacket`, `dComIfGd_drawOpaListBG` are all of that kind. Read as English they
produce confident, wrong answers, and one already reached our own documentation.

`kankyo` (環境) is *environment* — hence `dKy_`. `moya` (靄) is mist/haze. `kumo` (雲) is
cloud. `dKyw_wether_move` is the **weather** system and `wether` is not a typo to fix.

**`docs/japanese-naming.md` is the reference for this repo** — self-contained, because a
mod session attaches only `dusklight-mods`. Four things to internalise now:

- **Search Japanese with ripgrep, not `grep -P`.** 496 files in the game tree contain
  literal kana/kanji — the original team's own debug-panel labels, which are the most
  authoritative documentation of what any field means. This container's locale is
  `POSIX`, and under it `grep -P '\p{Han}'` silently matches **nothing** while a raw
  kana/kanji character class silently matches **too much**. `rg` — and the Claude Code
  `Grep` tool, which is ripgrep — is correct either way. Measured table in
  `docs/japanese-naming.md` §2.2. The game tree itself is at `dusklight/` after any
  CMake configure (git-ignored, fetched at `DUSKLIGHT_VERSION`).
- **Search in both romanizations.** The tree mixes kunrei-shiki (`si`, `tu`, `ti`, `sya`)
  with Hepburn (`shi`, `tsu`, `chi`, `sha`) *for the same word*. Either spelling alone
  finds none of the other half. An empty search is not evidence of absence.
- **A header field name is not an authored name.** Function and global-data symbols are
  the original team's; struct *member* names were reconstructed by the decompilation, so
  a member name is a hypothesis until an authored string agrees with it. This is what
  caught `mFogDensity` (it is *cloud-shadow* density) — see `er_tsr` above.
- **Never rename or "correct" a game symbol**, and gloss one on first use in any document
  here. Our own code in `mods/` stays ordinary English; the convention describes the code
  we *read*.

Run `python3 tools/check_japanese_naming.py` after editing that document — it verifies
every game symbol it names still exists in the fetched tree, and skips cleanly when the
tree is not present.

**And run `python3 tools/check_source_citations.py` after any pin bump.** Our documentation
argues from the game's and aurora's source and cites line numbers as evidence — ``d_kankyo.cpp:1257``
— and a re-platform invalidates them silently. The move to upstream 2.0 broke **21 of 153**,
including several this file leaned on. The checker guesses what each citation is evidence for
(the nearest backticked identifier) and reports `DRIFT` with the nearest real line. That guess is
**advisory**: check each before editing — on this pin it wrongly flagged `d_kankyo.cpp:1257`, which
was correct all along, because 9459 is where `GXSetFogRangeAdj` is *called* and 1257 is where the
flag is *set*. Citations confirmed by reading the source go in
`tools/source_citations_verified.txt` **with the pin they were checked against**, and the checker
re-reports them as `STALE` once the pin moves rather than treating them as permanent exemptions.

## Build model (official mod template)

This repo **is** the official Dusklight mod template
(`https://github.com/TwilitRealm/mod-template`) — its `cmake/FetchDusklight.cmake`, `tools/merge_mod.py`,
`add_mod` usage, `.gitattributes`, and `build.yml` (build + combine, plain `cl`, no link-target
plumbing), just laid out as a monorepo (one `mods/<name>/` per mod). The pinned game/SDK source is
**fetched** by `FetchDusklight` into `dusklight/` (git-ignored), keyed by `DUSKLIGHT_VERSION`; a plain
`git clone` + `cmake -B build` fetches it and the per-arch link stub automatically. **There are no
fork knobs any more.** `DUSKLIGHT_REPOSITORY` is upstream `TwilitRealm/dusklight`; there is no
`DUSKLIGHT_SDK_STUB_URL` override because upstream's stub release is version-INDEPENDENT (one fixed
`sdk` tag) and the SDK's own default already points at it (`dusklight/cmake/ModSDK.cmake:5`); and
`DUSKLIGHT_AURORA_VERSION` is gone, because it only ever existed to work around a force-pushed fork
branch leaving the recorded `extern/aurora` submodule pin dangling. The tree is now the stock
template plus `mods/` and `common/`, so template updates apply cleanly and a fresh clone needs no
local overrides at all.

**Tracked at mod-template `c79deaa`.** What we take from it and what we deliberately do not:

| Template change | Us |
| :-- | :-- |
| `CMAKE_BUILD_TYPE` defaulted to RelWithDebInfo **before `project()`** | **Taken.** This is the Windows one: a Release link strips the `DEFINE_HOOK` records the game's modmeta parser scans for, so hooks never register and the mod loads and does nothing. The SDK's own fallback covers single-config generators only — it does nothing under Visual Studio / Ninja Multi-Config, where `cmake --build` with no `--config` gives you Debug. |
| CI passes `-DCMAKE_BUILD_TYPE=RelWithDebInfo` explicitly | **Taken**, same reason — no reliance on any fallback. |
| `DUSK_VERSION_OVERRIDE` dropped from `FetchDusklight.cmake` | **Taken.** Nothing in the fetched tree ever read it. `cmake/FetchDusklight.cmake` is now byte-identical to the template again. |
| `step-security/msvc-dev-cmd` replacing `ilammy/msvc-dev-cmd`, plus the action version bumps | **Taken.** Only exercised in CI, so a green local build proves nothing about them. |
| `mod.json.in` + `configure_file`, version from `project(VERSION)` | **NOT taken, deliberately.** It gives a single-mod repo one source of truth for its version. We are a monorepo and our mods version *independently* — VBAO was 1.6.0 while SMAA was 1.1.0 before the 1.0.0 reset — and there is one top-level `project()`, so adopting it would force every mod to share a version. Each `mods/<name>/mod.json` stays a hand-edited literal. |
| `DUSKLIGHT_VERSION` default of `v2.0.0` | **Matched, by a separate decision.** The default itself is N/A — it only applies when the variable is unset, and we always pin explicitly — but we did then move the pin to that same tag, as a re-platform rather than as template tracking. See The ABI pin. |
| `ios-arm64` in the CI matrix | **NOT taken.** Code mods cannot run on iOS (dlopen restriction) — see the `standalone-final` note under Related repos. |

Our `build.yml` also keeps `branches: ["**"]` (GitHub's `*` does not match `/`, so the template's
glob skips our `claude/...` branches), the `mods-*` artifact names, and the per-mod merge loop with
`--expect-platforms`, which is our own addition to `tools/merge_mod.py` and not template drift.

## What a change does and does not require

Editing a shader or tuning a default touches ONE file here. It does **not** require building
the game, building aurora, or editing the fetched `dusklight/` tree (a read-only pinned
reference). CI compiles all mods on every platform in a few minutes.

**CI does NOT validate shaders** — it only packages the `.wgsl` files, so a WGSL error ships and
first appears in-game as a pipeline that fails to create. Validate locally before pushing a shader
change; `tools/wgsl_check.cpp` compiles every shader through Dawn's null backend and needs no GPU:

```sh
cmake -B build && cmake --build build          # fetches the prebuilt Dawn the validator links
D=build/_deps/dawn_prebuilt-src
g++ -std=c++20 -I$D/include tools/wgsl_check.cpp $D/lib/libwebgpu_dawn.a -ldl -lpthread -lX11 \
    -o build/wgsl_check && ./build/wgsl_check mods/*/res/*.wgsl
```

The user typically does not build locally. Iteration loop:
1. Edit, commit, push (branch per the session's instructions).
2. GitHub Actions builds each mod on all 7 platforms (Linux x64/arm64, macOS arm64/x64, Windows
   x64/arm64, Android arm64) and merges them into one **cross-platform `.dusk` per mod** (artifact
   `mods-combined`; per-platform artifacts are `mods-<platform>`).
3. User downloads them into `%APPDATA%\TwilitRealm\Dusklight\mods` (or the platform equivalent),
   then uses the in-game mod manager's **Reload** button — no game restart needed.

## Building a new mod (session setup + which pattern)

- **Which repos to attach to the session:** **only `automata-rtx/dusklight-mods`** — for any mod
  work *and* for re-platforming. The game SDK is **fetched over the network** by
  `cmake/FetchDusklight.cmake` from `DUSKLIGHT_REPOSITORY` (upstream `TwilitRealm/dusklight`) at the
  pinned `DUSKLIGHT_VERSION`, and the SDK **auto-downloads** its per-arch link stub. Attaching
  `dusklight-ao` / `aurora-ao` buys nothing now that the fork is retired. Attach
  `TwilitRealm/dusklight` itself only to read upstream *history* or its `mods/ao_mod` reference
  consumer — the fetched `dusklight/` tree is a **depth-1 shallow checkout** at the pin, so its
  sources are complete but `git log` there shows exactly one commit.
- **Default to service-only.** A new screen-space effect (e.g. SSDO, 1-bounce SSGI, SSR,
  outlines) should follow the VBAO / SSILVB pattern: consume depth + the world-space
  normal from **GfxService** (`GfxResolveDesc::normal` → `GfxResolvedTargets::normal`, the game's
  own authored view-space normal, resolved alongside depth) + the scene color, all via mod-API
  services — **no game headers, no hooks**, and no dependency on another mod. Reach the two normal
  fields through `common/gfx_normal_compat.h`, and **build scene-pass pipelines lazily, keyed on
  `GfxDrawContext::layout.key`** — see the normal-buffer constraint below. That keeps it off the ABI treadmill: it survives game updates and needs no platform
  rebuild. `docs/depth_to_normal_consumers.md` is the menu of exactly these effects plus the
  consumer integration boilerplate — read it first.
- Make a mod **game-linked** only if it genuinely needs a game buffer the gfx service does not
  expose (e.g. pre-tonemap HDR lighting or per-object albedo that SSGI might want). That couples
  it to the pinned build like the shadow/fog mods. Prefer service-only whenever the service
  surface (depth + normal + scene color) is enough.

## Hard constraints

- **Windows builds with plain MSVC (`cl`), like the stock template.** The base game's `modmeta`
  parser skips linker padding and the SDK defaults to RelWithDebInfo (no `/OPT:REF` stripping), so
  the `DEFINE_HOOK` records survive under `cl` and hooks register. (This is why we re-platformed onto
  the template's base — the *old* `0f2a00cd` base needed clang-cl and a `hook-repro` guard; both are
  gone.) No compiler override is needed anywhere.
- **Uniform structs are mirrored C ↔ WGSL.** Any change must keep the byte layouts identical
  on both sides and the total size a multiple of 16 (there are `static_assert`s — keep them
  true, don't delete them). Scalars are packed to avoid vec3 16-byte alignment traps.
- **Thread rules**: `GfxStageFn` callbacks run on the game thread; `GfxDrawFn`/`GfxComputeFn`
  run later on the render worker with only their context handles + raw `wgpu*` calls.
  Never touch game state from a draw/compute callback.
- **All WGPU handles from the gfx service are borrowed**; resolved views are valid for the
  current frame only. Objects the mod creates are released in `mod_shutdown`.
- **Reversed-Z everywhere** (1 = near). Sky pixels have raw depth 0.
- **Every render pipeline recorded into the scene pass must take its attachment layout from the
  LIVE `GfxDrawContext::layout`, and must be REBUILT whenever `layout.key` changes** —
  `gfx_compat::scene_pass_layout_for_draw()` + `gfx_compat::scene_pass_layout_key()`
  (`common/gfx_scene_pass.h`), never `GfxDeviceInfo`. A host normal buffer adds a second attachment
  to the EFB pass, and WebGPU rejects any pipeline whose target count does not match the pass. The
  helper reads the layout the host is about to record into and feeds the SDK's inline
  `gfx_init_color_target_states`, which write-masks off every attachment the mod does not own. Every
  stage that pushes draws lands in that pass; there is no exempt stage. Offscreen `create_pass`
  targets stay single-target.
  **Building a scene-pass pipeline once in `mod_initialize` is now a BUG, not a shortcut.** The
  normal attachment latches on at runtime (next bullet), so the pass shape a mod saw at init is not
  the shape it will be recorded into a frame later — and the symptom is silent: the composite simply
  stops appearing. Copy `ensure_composite_pipelines()` in `mods/vbao/src/mod.cpp` or
  `ensure_neighborhood_pipeline()` in `mods/smaa/src/mod.cpp`; both follow upstream `mods/ao_mod`'s
  own `ensure_pipelines(ctx->layout)` pattern. See `docs/authored_normals.md` §5.
- **Never touch an SDK normal-buffer field directly** — `GfxResolveDesc::normal`,
  `GfxResolvedTargets::normal`. Go through `common/gfx_normal_compat.h` (`gfx_compat::request_normal`
  / `resolved_normal`), which detects each field at compile time and degrades to "no normal buffer"
  when it is absent. **Both fields are UPSTREAM now** (GfxService 1.3); this header was written when
  they were fork-local, and that is precisely why the move to upstream cost nothing but a pin bump.
  It detects by member **name**, so it never noticed that upstream declares `normal` as a `uint32_t`
  *appended* to `GfxResolveDesc` where our fork had a `bool` tucked into the struct's tail padding.
  Keep using it: an SDK older than 1.3 still has neither field, and this is the seam that absorbs
  the next such difference. Verified by forcing a full rebuild against a stripped SDK; see
  `docs/normal_buffer_portability.md`.
- **The normal snapshot LATCHES, so the first frame's view is null and that is NOT a failure.** The
  first `resolve_pass` that sets `normal` turns the attachment on for the **next** frame and hands
  back `nullptr` for this one — upstream's `mods/ao_mod` says so in as many words and simply
  returns. The retired fork had no latch (it snapshotted unconditionally, with the attachment
  decided before any mod initialized), so this is genuinely new behaviour to honour on this
  platform. A consumer that reads the first null as "this device has no normal buffer" announces a
  compatibility-renderer warning on every cold start; VBAO's `kNormalLatchGraceFrames` is the fix,
  and SMAA needs none because it silently falls back to luma-only edge detection.
- **`normal_format` does not exist on any SDK struct. Do not reintroduce an accessor for it.** To
  ask whether this build carries authored normals, use
  `gfx_compat::ScenePassLayout::has_normal_attachment`, which scans the real scene layout for a
  `GFX_ATTACHMENT_NORMAL` semantic. Two platforms carried a `normal_format` field and each produced
  a distinct silent failure — an offset collision with upstream's `WGPUInstance`, and the
  compare-against-live guard below.
- **Degrade-to-absent is safe for a READ, not for a COMPARISON.** The draw callbacks used to guard
  on `gfx_compat::normal_format(*ctx) != gfx_compat::normal_format(g_deviceInfo)`. `GfxDrawContext`
  had no `normal_format`, so that shim call was a constant `Undefined` while the device reported a
  real format — the guard fired on every draw and silently disabled all six composites the moment
  the user enabled the buffer. It is deleted. Do not reintroduce a guard that compares a
  compile-time-detected field against a live value.
- **A RENAMED API is not an ABSENT one, and degrade-to-absent cannot tell them apart.** Upstream
  renamed the scene-layout vocabulary (`get_pass_targets` → `get_scene_target_layout`,
  `GFX_MAX_COLOR_TARGETS` → `GFX_MAX_COLOR_ATTACHMENTS`). Our `#if` guard went false, **the whole
  tree compiled with zero errors**, and every scene-pass pipeline quietly reverted to one colour
  target — six composites that would have drawn nothing in-game with no build signal. So
  `gfx_scene_pass.h` now **`#error`s** when it does not recognise the SDK (override with
  `GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT` for a genuine pre-1.2 base), while
  `gfx_normal_compat.h` still degrades quietly. The rule: degrade silently only when the feature is
  optional *and* its absence is observable at runtime; when guessing wrong produces no diagnostic,
  fail the build. **After any pin bump, read the new SDK header — a green build proves nothing.**
- **Never name a config var `enabled`** — the host reserves `mod.<escaped id>.enabled` for the mod
  manager's own per-mod checkbox, created at discovery before any mod initializes, so
  `register_var("enabled")` returns `MOD_CONFLICT` and the mod fails to load. This is silent: the
  tree builds, the mod packages, and the only symptom is a runtime line naming the mod's own option.
  It kept Celestial Orbit from loading at all. Prefix it (`orbitEnabled`); the UI label can still
  say "Enabled". `python3 tools/check_reserved_config_names.py` scans every mod and re-derives the
  reserved list from the fetched game tree. See `docs/mod-api-notes.md` "Config/UI".
- **VBAO stays service-only.** If a feature seems to need game code, it belongs in the shadow
  mod or needs an upstream service extension — don't add game includes to `vbao`.
- **The ABI pin**: the platform is pinned by **`DUSKLIGHT_VERSION` in the top-level `CMakeLists.txt`**,
  fetched from `DUSKLIGHT_REPOSITORY`. **Both point at UPSTREAM now** — `TwilitRealm/dusklight` at
  the **`v2.0.0`** release tag (`e9b12054`, 2026-09-18) — which is **GameService 2.0 / GfxService
  1.3** over upstream aurora `7d4484a` (the recorded `extern/aurora` pin; the submodule URL is
  `encounter/aurora`). **Pinning a TAG rather than a SHA is deliberate**: a tag is a build users can
  actually download, and the matched-pair rule below is only checkable if the pin names the same
  thing the user installs. There is
  no `DUSKLIGHT_SDK_STUB_URL` override because upstream's stub release is version-independent and
  the SDK already defaults to it. **`DUSKLIGHT_VERSION` must match the game build actually being
  run.**
  - **The fork is RETIRED, and this is the endgame earlier revisions of this file predicted.**
    `automata-rtx/dusklight-ao` existed for exactly one reason — it was the only build that handed
    mods the game's authored vertex normals — and upstream now ships that itself, renderer included.
    Nothing here needs the fork. Do not re-add the fork knobs; do not write documentation that
    describes the fork as the current platform.
  - **Upstream's normal API is NOT the one our fork had, and the difference is BINARY.** Same
    service version, different shape:

    | | fork (retired) | upstream (now) |
    |---|---|---|
    | how a mod asks | `svc_gfx->get_scene_normals(ctx, &GfxSceneNormals)`, its own vtable call | `GfxResolveDesc::normal` → `GfxResolvedTargets::normal`, through `resolve_pass` |
    | when it snapshots | host-side every frame, whether or not anyone asked | on request, and it **latches**: the first ask enables it for the *next* frame |
    | `sizeof(GfxResolveDesc)` | **unchanged** — a `bool` in the struct's tail padding | **8 → 12** — a `uint32_t` appended, *"not a bool to avoid using the previous padding"* (upstream's own comment) |

    **There is no `get_scene_normals` in the upstream vtable at all** — the vtable ends at
    `get_scene_target_layout`. And that size growth is why this was a real re-platform rather than a
    cosmetic one: the host reads `desc->struct_size >= sizeof(GfxResolveDesc) ? desc->normal : 0`,
    so a mod built against the fork's SDK does **not** fail loudly on upstream — it silently gets no
    normals and then disables itself. **Rebuild every normal consumer against this pin.**
  - **Match the pin to the running build.** Game-linked mods resolve hook targets **by symbol at load**,
    so a mod built against a different base can fail to load outright rather than merely misbehave.
  - **Struct-size ABI is one-directional.** The host rejects callers whose
    `struct_size < sizeof(host struct)`, so mods built against an OLDER SDK are refused by a newer
    game (symptom: every webgpu mod dies at init with `failed to query device info`). A LARGER
    `struct_size` passes an older host's check. **When in doubt build against the SDK matching the
    game.** GameService is **2.0** on this pin, which is the blunter version of the same trap: a
    major bump refuses every mod built against the older SDK regardless of hooks or struct sizes.
  - **Appended fields are not a safe assumption — offsets can collide.** Historical now, but it is
    the reason we stopped forking. Our old fork appended `GfxDeviceInfo::normal_format` at offset
    40; upstream independently appended `WGPUInstance instance` at *the same offset*. A fork-built
    mod therefore read a live pointer as a texture format, concluded the thin g-buffer existed,
    declared a second colour target against a one-attachment scene pass, and had **every composite
    rejected** — the mods loaded and did nothing. Two vendors appending to the same struct is not
    forward compatibility. That field exists in no SDK today.
  - **Two color attachments — and the count CHANGES MID-SESSION.** When the normal buffer is on the
    scene pass has two colour targets, and *every* pipeline recorded into it must declare two or
    WebGPU rejects the draw. Because the attachment **latches on at runtime**, "how many targets" is
    not a question that can be answered once at init: take the layout from `GfxDrawContext::layout`
    and rebuild the pipeline when `layout.key` changes. `mods/vbao` and `mods/smaa` do; `ssilvb` and
    `realtime_sun_shadows` must before they go back in the build. See the scene-pass constraint
    under Hard constraints.
  - **Whether a frame carries normals is a runtime question with three answers.** The SDK may not
    have the fields at all (older than 1.3 — `gfx_normal_compat.h` makes that a compile-time
    non-event); the renderer may be unable to carry the attachment (the compatibility renderers,
    D3D11 / OpenGL ES); or it may simply not have latched on yet. Only the middle one is permanent.
    A mod that needs normals disables itself and says so; SMAA falls back to luma-only edges. See
    `docs/normal_buffer_portability.md`.
  - **Aurora's streaming buffers are UPSTREAM-SIZED** (Vertex 5 MB / Index 2 MB / Storage 8 MB), and
    that is **fine — the old overflow risk is closed, not merely tolerated.** The retired fork once
    carried enlarged 16/4/16 buffers for Realtime Sun Shadows' cascade replays, sized against
    aurora's *then* 3 MB/1 MB. Upstream has since raised both itself — Vertex 3→5 MB (`b979ff6`,
    2026-07-07) and Index 1→2 MB (`1b484d4` "Bump IndexBufferSize", 2026-07-19) — so the index
    budget is double what the v1.6.0/1.6.1 crash happened on, and the mod separately gained three
    mitigations that did not exist then (`cascadeCull`, `casterMinTexels`, a 2-cascade default).
    Losing the fork's buffers is therefore not a regression. **Earlier revisions of this file and
    three docs called it "the one real regression on this pin"; that was wrong and is corrected.**
    Cascade count and coverage are framerate choices now, not stability ones — see
    `docs/realtime_sun_shadows.md`.
  - **Which mods actually build on this pin is a SHORT LIST — see the comment block in
    `CMakeLists.txt`.** Today it is `mods/vbao`, `mods/smaa` and `mods/deferred_fog`. The four
    others are commented out with a per-mod reason: `ssilvb` and `realtime_sun_shadows` still need
    the resolve-based normal conversion plus the layout-key rebuild, and `celestial_orbit` /
    `effect_remover` need their hook symbols re-verified against this game build (§ Re-platforming
    step 3 — Deferred Fog is the worked example of what that takes). Re-enable them one at a time.

## Re-platforming (moving to a newer base game)

1. Bump **`DUSKLIGHT_VERSION`**, and reconfigure. That is normally the whole change: the SDK
   downloads its link stubs from a **version-independent** upstream release, so there is no stub URL
   to move with it. Only move `DUSKLIGHT_REPOSITORY` if the new base genuinely lives elsewhere — and
   if it is ever a fork again, remember a fork release's stubs *are* per-build and
   `DUSKLIGHT_SDK_STUB_URL` has to move with the pin.
2. Install the matching game build and fresh `.dusk` files **as a pair**. The pin and the running
   build must agree: game-linked mods resolve hook targets by symbol at load, so a mismatch can make
   a mod fail to load outright (this is exactly how Celestial Orbit failed when fork-built `.dusk`
   files were run on an upstream build). The **game service major version** is a second, blunter
   version of the same trap — a bump there refuses every mod built against the older SDK regardless
   of hooks.
3. **Re-verify the game-linked mods** — Deferred Fog, Realtime Sun Shadows, Effect Remover,
   Celestial Orbit. They hook specific game functions and a decomp delta can move or rename what
   they hook. The service-only mods (VBAO, SSILVB, SMAA) need no re-verification.
   **A clean compile is not the check.** Deferred Fog built against upstream 2.0 with zero source
   changes, and that said nothing: `DEFINE_HOOK` takes a member-function pointer, so the compiler
   verifies the *signature* while the symbol is resolved by name at **load**. Pull the hook list out
   of the mod's `DEFINE_HOOK` lines and grep the tree for each one, then still run it in-game.
   On **this** pin, Deferred Fog has had the grep pass and is back in the build (not yet run
   in-game); Celestial Orbit and Effect Remover have had neither and stay out. The pass was redone
   at the `c83ce89` → `v2.0.0` bump — all ten targets still present — which took about a minute and
   is the cost of this step in the ordinary case.
   **Check the diffstat first; it tells you how much work this is.** `git diff --stat <old> <new>`
   over the game tree showed that bump touched no `sdk/` file at all and no file Deferred Fog
   hooks, which correctly predicted a no-op re-platform. A range that *does* touch `sdk/` or the
   hooked files is the one to slow down for.
   `tools/check_japanese_naming.py` is a cheap first pass over the symbols our *docs* name, but it
   does **not** cover hook targets.
4. **Read the new SDK header. A green build proves nothing.** Two separate silent failures came out
   of assuming otherwise: a renamed scene-layout API that left `#if`-guarded code compiling to the
   wrong thing, and an appended field whose offset collided with someone else's. `gfx_scene_pass.h`
   now `#error`s rather than degrade for exactly this reason; `gfx_normal_compat.h` still degrades
   quietly, because its absence is observable at runtime.
5. Re-check anything that *changed shape* rather than merely moved. The move to upstream 2.0 is the
   worked example: same GfxService version number, completely different normal API, and a
   `GfxResolveDesc` that grew from 8 to 12 bytes — see The ABI pin.
6. The shadow mod's cascade replays are still the heaviest consumer of aurora's per-frame
   streaming buffers, so they are the thing to watch if a *new* base ever shrinks them. At the
   current upstream sizes (Vertex 5 MB / Index 2 MB / Storage 8 MB) this is a framerate
   consideration, not a crash risk.
7. If the new base has no scene normal buffer, expect the normal consumers to disable themselves and
   say so. That is correct, and needs no source change.

## Where the platform is going (state this plainly — it is not a secret)

**We are on upstream, and the fork is retired.** `DUSKLIGHT_REPOSITORY` is `TwilitRealm/dusklight`
and there is nothing fork-local left in the tree. For months this section said the opposite —
*"Today we run our own fork, deliberately"* — and that was true then: `automata-rtx/dusklight-ao`
was the only build that handed mods the game's authored vertex normals, and everything here that
reads a normal depended on it. **The stated endgame was to upstream the delta and then move the pin
to upstream. That is what happened**, though not via our PR: upstream shipped its own equivalent
(GfxService 1.3's `GfxResolveDesc::normal` → `GfxResolvedTargets::normal`) alongside the GameService
2.0 bump, with the matching attachment in upstream aurora.

**Moving cost a pin bump and two mod-source changes, and that split is the lesson.** The pin bump
was free because `common/gfx_normal_compat.h` detects the two fields by *member name* and
`common/gfx_scene_pass.h` reads the real scene layout — neither cared that upstream's `normal` is a
`uint32_t` appended to the struct where ours was a `bool` in tail padding. That is the entire reason
those two shims exist, and they earned it. What the shims could *not* absorb was a **behavioural**
difference: upstream's snapshot latches (§ The ABI pin), so a pipeline built at init against a
one-attachment pass is silently wrong a frame later. **A compile-time compatibility layer buys you
nothing against a runtime behaviour change** — that is the thing to remember next time.

**Forking the SDK was never free, and the bill came due twice.** Appending fields to SDK structs is
what caused the `normal_format` / `WGPUInstance` offset collision documented under The ABI pin, and
the shape mismatch above is the second instalment: our fork and upstream solved the same problem
differently, and every fork-built `.dusk` silently lost its normals on the new host. Both are
arguments for being *on* upstream rather than near it. If a future feature ever seems to need a fork
again, the bar is that high.

> **Note on scope:** this repo's documentation is ours and was never part of any upstream PR, which
> is why it says all of the above directly.

## Related repos

- `TwilitRealm/dusklight` — **upstream, and the platform.** `DUSKLIGHT_VERSION` pins a commit here
  (the `v2.0.0` tag = `e9b12054`, 2026-09-18) and `cmake/FetchDusklight.cmake` fetches it into `dusklight/` as a
  **depth-1 shallow checkout**, so the sources are complete but there is no history to search there.
  A mod session does **not** need it attached; attach it only to read upstream history or to look at
  `mods/ao_mod`.
  - **`mods/ao_mod` is the reference consumer for authored normals — read it before changing ours.**
    It is upstream's own demo AO mod, and it demonstrates both halves of the current contract: ask
    via `GfxResolveDesc::normal` and read `GfxResolvedTargets::normal` (its comment states the latch
    outright — *"The first request enables normals next frame; unsupported devices keep returning
    null"*), and `ensure_pipelines(ctx->layout)`, which rebuilds when `layout.key` changes.
  - `extern/aurora` → `encounter/aurora` at `7d4484a` — upstream aurora, which carries the optional
    normal attachment itself. Nothing in this repo builds it; it is named here so the renderer side
    of a normal question has an address.
- `automata-rtx/dusklight-ao` — **our Dusklight fork. RETIRED as of this pin — historical only.**
  Kept because one branch on it cannot be reproduced, not because anything here still points at it.
  - Branch `claude/dusklight-thin-gbuffer-normals-l4l9dc` / `platform-normals-test` (`5ded001`) =
    the last fork platform: upstream `c880d46f` + a **differently-shaped** GfxService 1.3 normal
    snapshot (`get_scene_normals`, plus `normal` in `GfxResolveDesc`'s tail padding). Superseded by
    upstream's own API, which is **not binary-compatible** with it — see The ABI pin. Do not consult
    it for how a mod should read normals; that answer is now `mods/ao_mod` upstream.
    `dusklight-ao/docs/thin-gbuffer-normals.md` is still the clearest renderer-side write-up of
    *why* the attachment is shaped the way it is.
  - Branches `claude/thin-gbuffer-authored-normals-wgqupt` / `platform-gbuffer-test` (`b96bf5ec01`,
    the RGBA8 first attempt with the colliding `GfxDeviceInfo` offset) and
    `claude/dusklight-platform-rebuild-rqhsaw` / `platform-v2-test` (`9361fbd9ea`, pre-g-buffer) are
    superseded likewise.
  - **Branch `claude/standalone-final` + the `standalone-final` release — NEVER DELETE.** This is
    the pre-mod-API aurora-fork build, and it is the ONLY way the graphics features run on iOS (code
    mods cannot run there — dlopen restriction). It is unaffected by any of the above.
    (`mod-platform` / `platform-v1` are the superseded first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork, the renderer under the retired platform above.
  Branch `claude/dusklight-thin-gbuffer-normals-l4l9dc` (`49d644e`) = upstream aurora `cf3ffc9` +
  the optional normal attachment. Also historical now that upstream aurora carries its own. Other
  branches remain the frozen fork the `standalone-final` build uses — those stay.
