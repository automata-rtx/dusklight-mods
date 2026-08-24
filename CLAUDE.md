# dusklight-mods

Graphics mods for Dusklight (the Twilight Princess PC/mobile port), built on its mod API:

- **`mods/vbao/`** — "VBAO" (Visibility Bitmask Ambient Occlusion): a 32-sector
  visibility-bitmask AO estimator with temporal accumulation, edge-aware denoise, and
  depth-aware compositing. A distinct technique from Encounter's GTAO demo mod (its original
  framework). **Service-only**: it uses only mod-API services (gfx, camera, config, ui,
  resource, log) — it must NOT include game headers or call game code, which is what lets it
  survive game updates without a rebuild.
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
  `get_scene_normals` conversion VBAO and SMAA got) the provider's normal for per-sample normals; composites GI additively and AO multiplicatively in a single blend draw. Since 0.10.0 it
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
  game's own authored normals** (`get_scene_normals`; normal-angle + relative-depth discontinuity —
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
  world lists and re-applies it (bit-exact aurora fog math, `src/fog_math.h`) as a fullscreen pass at
  `FRAME_BEFORE_HUD`, so AO/shadows darken surfaces *under* the fog instead of darkening the fog
  itself. Mixed fog configs take an exact per-pixel replay (default) or auto-revert to vanilla.
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
  mod's `SCENE_AFTER_OPAQUE` composite and before the translucent lists (`m_Do_graphic.cpp:2426`,
  one line before `dComIfGd_drawXluListBG`) — but there is no stage hook there and every list entry
  point inlines, so the mod anchors on the first `J3DShape::drawFast` after the stage closes. A view
  with **no translucent J3D at all** (open field: alpha-tested trees, self-drawing grass packets,
  JPA particles) used to fall through to `FRAME_BEFORE_HUD` (`:2795`) — after every particle pass
  and **bloom** (`:2663`, on by default). Fog applied after bloom is fog the bloom never saw, so the
  bright distant subjects vanilla blooms hardest come out dimmer. (`motionBlure` at `:2483` is
  **not** motion blur — it is a previous-frame blend gated off in ordinary play — and `drawDepth2`
  is DOF, gated on auto-focus. Neither belongs in that list; reading the symbol name as a
  description is exactly what `docs/japanese-naming.md` warns about.) A pre-hook on
  `mDoGph_gInf_c::bloom_c::draw` is now the fallback; the Status line reports which anchor fired
  (`[at translucents]` / `[before bloom]` / `[AFTER BLOOM]`).
  **The `K` factor is the exact statement of what a fullscreen pass can reproduce.** Aurora fogs the
  fragment *source* inside the fragment shader (`shader.cpp:1579`) and the GX blend is a pipeline
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
  (`d_kankyo.cpp:4434`), so vanilla applies literally zero fog however distant — and it is invisible
  to the `GXSetFog` hooks because `J3DGDSetFog` writes raw BP commands. `fogSkipUnfogged` (default
  **off**) marks those pixels with a config-ID sentinel the shader skips; it may only mark a material
  that **owns its depth** and whose **alpha test is trivial**, and it forces the ID replay.
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
  a world-space normal from depth and published it as a service. GfxService 1.3's `get_scene_normals`
  supersedes that completely — the host hands every mod the game's *authored* normal directly — so
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
    (`d_kankyo_rain.cpp:4594`), and five of its twelve modes blend additively so they can only
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
    state (`d_kankyo.cpp:2427`), where it deliberately flattens the look — so 255 is the engine's
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
(shaders). Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`,
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

## Build model (official mod template)

This repo **is** the official Dusklight mod template
(`https://github.com/TwilitRealm/mod-template`) — its `cmake/FetchDusklight.cmake`, `tools/merge_mod.py`,
`add_mod` usage, `.gitattributes`, and `build.yml` (build + combine, plain `cl`, no link-target
plumbing), just laid out as a monorepo (one `mods/<name>/` per mod). The pinned game/SDK source is
**fetched** by `FetchDusklight` into `dusklight/` (git-ignored), keyed by `DUSKLIGHT_VERSION`; a plain
`git clone` + `cmake -B build` fetches it and the per-arch link stub automatically. Two knobs point
the stock template at our platform fork: `DUSKLIGHT_REPOSITORY` (`automata-rtx/dusklight-ao`, for
the scene normal buffer) and `DUSKLIGHT_SDK_STUB_URL`
(that fork's `platform-normals-test` release, which publishes the per-arch link stubs as top-level
assets). `DUSKLIGHT_AURORA_VERSION` stays unset — the recorded `extern/aurora` pin resolves on its
own. Everything else is the template unchanged, so template updates apply cleanly.

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
  `cmake/FetchDusklight.cmake` from `DUSKLIGHT_REPOSITORY` at the pinned `DUSKLIGHT_VERSION`, and
  the SDK **auto-downloads** its per-arch link stub from `DUSKLIGHT_SDK_STUB_URL`. Attach
  `dusklight-ao` / `aurora-ao` only when changing the platform *itself* (a renderer or SDK change),
  not to build or modify mods.
- **Default to service-only.** A new screen-space effect (e.g. SSDO, 1-bounce SSGI, SSR,
  outlines) should follow the VBAO / SSILVB pattern: consume depth + the world-space
  normal from **GfxService** (`get_scene_normals` — the game's own authored normal, snapshotted by
  the host once per frame) + the scene color, all via mod-API services — **no game headers, no
  hooks**, and no dependency on another mod. That keeps it off the ABI treadmill: it survives game updates and needs no platform
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
- **Every render pipeline recorded into the scene pass must take its attachment layout from
  `gfx_compat::scene_pass_layout` (`common/gfx_scene_pass.h`)**, never from `GfxDeviceInfo`. A host
  normal buffer adds a second attachment to the EFB pass, and WebGPU rejects any pipeline whose
  target count does not match the pass. The helper wraps GfxService 1.2's `get_scene_target_layout`
  and the SDK's inline `gfx_init_color_target_states`, which write-masks off every attachment the
  mod does not own. Every stage that pushes draws lands in that pass; there is no exempt stage.
  Offscreen `create_pass` targets stay single-target. See `docs/authored_normals.md` §5.
- **Never touch an SDK normal-buffer field directly** — `GfxResolveDesc::normal`,
  `GfxResolvedTargets::normal`. Go through `common/gfx_normal_compat.h` (`gfx_compat::request_normal`
  / `resolved_normal`), which detects each field at compile time and degrades to "no normal buffer"
  when it is absent. Those two fields are **fork-local** (GfxService 1.3) and are now the *entire*
  fork delta — upstream Dusklight has neither — so a direct access compiles today and breaks the
  whole tree on the next re-platform. Verified by forcing a full rebuild against a stripped SDK; see
  `docs/normal_buffer_portability.md`.
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
  fetched from `DUSKLIGHT_REPOSITORY`. It currently points at **`5ded001`** in
  **`automata-rtx/dusklight-ao`** (branch `claude/dusklight-thin-gbuffer-normals-l4l9dc`, published
  as the **`platform-normals-test`** prerelease) — upstream Dusklight `c880d46f` plus **GfxService
  1.3**, over aurora `cf3ffc9` plus the optional normal attachment (upstream-sized streaming
  buffers — see below). `DUSKLIGHT_SDK_STUB_URL` tracks it to that same release, whose tag is
  **republished on every push** to the platform branch, so the URL is stable while its assets are
  not. `DUSKLIGHT_AURORA_VERSION` stays unset. **`DUSKLIGHT_VERSION` must match the game build
  actually being run.**
  - **The fork delta is now two fields.** Upstream shipped its own scene-target-layout API (#2305,
    GfxService 1.2), so our hand-rolled `GfxPassTargets` version was **deleted rather than merged**
    and `GfxDeviceInfo::normal_format` is gone entirely. What is left fork-local is 1.3's
    `GfxResolveDesc::normal` → `GfxResolvedTargets::normal`. Placement is deliberate: `normal` sits
    in `GfxResolveDesc`'s existing **tail padding**, so `sizeof` is unchanged and `struct_size`
    cannot see it; the host honours it only when `GfxResolvedTargets` is large enough to carry the
    result back, so a 1.2 mod's uninitialised padding can never request a snapshot it cannot
    receive. Copy that pattern if the fork ever grows a third field.
  - **Why a fork at all.** Upstream bumped the **game service major version**, so mods built against
    the older `0fc05028` SDK are refused outright by that host (every mod but SMAA failed to load);
    and 1.3's resolve pair is what hands mods the authored normals. The offset collision that sank
    the *first* fork cannot recur — the colliding field no longer exists in any form.
  - **Match the pin to the running build.** Game-linked mods resolve hook targets **by symbol at load**,
    so a mod built against a different base can fail to load outright rather than merely misbehave.
  - **Struct-size ABI is one-directional.** The host rejects callers whose
    `struct_size < sizeof(host struct)`, so mods built against an OLDER SDK are refused by a newer
    game (symptom: every webgpu mod dies at init with `failed to query device info`, and the service
    consumers then fail on the missing `depth_to_normal` import). A LARGER `struct_size` passes an
    older host's check. **When in doubt build against the SDK matching the game.**
  - **Appended fields are not a safe assumption — offsets can collide.** Our old fork appended
    `GfxDeviceInfo::normal_format` at offset 40; upstream independently appended `WGPUInstance
    instance` at *the same offset*. A fork-built mod therefore read a live pointer as a texture
    format, concluded the thin g-buffer existed, declared a second colour target against a
    one-attachment scene pass, and had **every composite rejected** — the mods loaded and did
    nothing. Two vendors appending to the same struct is not forward compatibility.
  - **Two color attachments.** When the host normal buffer is on the scene pass has two color targets,
    and *every* pipeline recorded into it must declare two or WebGPU rejects the draw. All six
    scene-pass pipelines (vbao, smaa, deferred_fog, and — once ported — ssilvb and
    realtime_sun_shadows) take their layout from `gfx_compat::scene_pass_layout`, so they follow the pass
    whichever shape it has. Any new scene-pass mod must do the same.
  - **Authored normals are back — but off until the user switches them on.** The platform provides
    the buffer; the game ships it disabled (**Video → Rendering → Scene Normal Buffer**, applies on
    always, with no setting and no restart; only the compatibility renderers (D3D11 / OpenGL ES)
    cannot carry the attachment, and a mod that needs normals disables itself there and says so. `common/gfx_normal_compat.h` makes a base *without* the buffer a
    compile-time non-event; see `docs/normal_buffer_portability.md`.
  - **Link stubs come from the same release as the game build.** `DUSKLIGHT_SDK_STUB_URL` points at
    `automata-rtx/dusklight-ao` `releases/download/platform-normals-test`, which publishes every
    asset our CI matrix needs (`windows-amd64.lib`, `windows-arm64.lib`, `stub-macos-arm64`,
    `stub-macos-x86_64`, `stub-android-aarch64.so`) as top-level assets. Linux needs no stub at all.
    Unlike upstream's version-independent `sdk` tag, **a fork release's stubs are per-build** — move
    `DUSKLIGHT_SDK_STUB_URL` whenever you move `DUSKLIGHT_VERSION`. Bump either **only** when
    deliberately re-platforming, never as a side effect of a mod change.
  - **Aurora's streaming buffers are UPSTREAM-SIZED** (Vertex 5 MB / Index 2 MB / Storage 8 MB), and
    that is **fine — the old overflow risk is closed, not merely tolerated.** The fork once carried
    enlarged 16/4/16 buffers for Realtime Sun Shadows' cascade replays, sized against aurora's
    *then* 3 MB/1 MB. Upstream has since raised both itself — Vertex 3→5 MB (`b979ff6`,
    2026-07-07) and Index 1→2 MB (`1b484d4` "Bump IndexBufferSize", 2026-07-19) — so the index
    budget is double what the v1.6.0/1.6.1 crash happened on, and the mod separately gained three
    mitigations that did not exist then (`cascadeCull`, `casterMinTexels`, a 2-cascade default).
    Dropping the fork's buffers is therefore not a regression. **Earlier revisions of this file and
    three docs called it "the one real regression on this pin"; that was wrong and is corrected.**
    Cascade count and coverage are framerate choices now, not stability ones — see
    `docs/realtime_sun_shadows.md`.

## Re-platforming (moving to a newer base game)

1. Bump **`DUSKLIGHT_VERSION`**, and **`DUSKLIGHT_REPOSITORY` / `DUSKLIGHT_SDK_STUB_URL` with it** if
   the new base lives in a different repo or release. The stubs must come from the same build as the
   game: upstream's `sdk` tag is version-independent, a fork release's stubs are not.
2. Install the matching game build and fresh `.dusk` files **as a pair**. The pin and the running
   build must agree: game-linked mods resolve hook targets by symbol at load, so a mismatch can make
   a mod fail to load outright (this is exactly how Celestial Orbit failed when fork-built `.dusk`
   files were run on an upstream build). The **game service major version** is a second, blunter
   version of the same trap — a bump there refuses every mod built against the older SDK regardless
   of hooks.
3. **Re-verify the game-linked mods in-game** — Deferred Fog, Realtime Sun Shadows,
   Effect Remover, Celestial Orbit. They hook specific game functions and a decomp delta can move or
   rename what they hook. The service-only mods (VBAO, SSILVB, SMAA) need no re-verification.
4. The shadow mod's cascade replays are still the heaviest consumer of aurora's per-frame
   streaming buffers, so they are the thing to watch if a *new* base ever shrinks them. At the
   current upstream sizes (Vertex 5 MB / Index 2 MB / Storage 8 MB) this is a framerate
   consideration, not a crash risk.
5. If the new base has no scene normal buffer, expect the normal consumers to disable themselves and
   say so. That is correct, and needs no source change.

## Where the platform is going (state this plainly — it is not a secret)

**Today we run our own fork, deliberately.** `automata-rtx/dusklight-ao` is the platform because it
is the only build that hands mods the game's authored vertex normals, and everything in this repo
that reads a normal depends on that. This is a considered position, not an accident or a stopgap we
are embarrassed about.

**The intended endgame is to upstream the delta and then move the pin to upstream.** The delta is
now two fields (`GfxResolveDesc::normal` → `GfxResolvedTargets::normal`, GfxService 1.3) plus
aurora's optional normal attachment — small, additive, off by default, and useful to any aurora
consumer. `docs/authored_normals.md` §9.5 is the concrete PR shape. Once upstream carries a
compatible equivalent, moving is a **pin bump and nothing else**: `common/gfx_normal_compat.h`
detects the fields by member name and `common/gfx_scene_pass.h` reads the real scene layout, so no
mod source changes whichever base provides them. That is the whole reason those two shims exist.

Until that lands, expect the fork. Do not "clean up" the fork knobs in `CMakeLists.txt`, and do not
write documentation that describes upstream as the current platform — main did exactly that during
a temporary retreat and it took a full merge to unpick.

> **Note on scope:** this repo's documentation is ours and is *not* part of any upstream PR, so it
> says all of the above directly. What would be offered upstream is the platform change itself, not
> these notes.

**Forking the SDK is still not free.** Appending fields to SDK structs is what caused the
`normal_format` / `WGPUInstance` offset collision documented under The ABI pin — which is precisely
why upstreaming is the goal rather than growing the delta. **That argument has already been paid off
once in practice:** upstream shipped its own scene-target-layout API, our hand-rolled equivalent was
deleted rather than merged, and the fork shrank from "a layout API + a device field + two resolve
fields" to just the two resolve fields. The one that stayed is the one placed most carefully — in
existing tail padding, gated on the *reply* struct's size.

## Related repos

- `automata-rtx/dusklight-ao` — **our Dusklight fork, and the current platform.**
  `DUSKLIGHT_VERSION` pins a commit here and `DUSKLIGHT_SDK_STUB_URL` a release here.
  - Branch `claude/dusklight-thin-gbuffer-normals-l4l9dc` / `platform-normals-test` (`5ded001`) =
    **the live platform**: upstream `c880d46f` + **GfxService 1.3**'s normal snapshot
    (`b752155`), the Video setting (`071ee88`) and the ao_mod reference consumer (`6086e9e`).
    `dusklight-ao/docs/thin-gbuffer-normals.md` is the renderer-side design. The two
    `TEST SCAFFOLDING` commits on the tip (aurora submodule pin + release job, Aurora gfx tests)
    are meant to be dropped before any upstream PR — dropping them would move the pin, so re-pin if
    that happens. The branch is **force-pushed** on each rebase onto newer upstream, so `git fetch`
    before assuming a local copy is current.
    **This is the mod-facing contract:** authored normals are snapshotted through
    `GfxResolveDesc::normal`, and the buffer's presence is detected by finding a
    `GFX_ATTACHMENT_NORMAL` semantic in `get_scene_target_layout`. `mods/ao_mod/` on this branch is
    the reference consumer — read it before changing ours.
  - Branch `claude/thin-gbuffer-authored-normals-wgqupt` / `platform-gbuffer-test` (`b96bf5ec01`) =
    the **retired** first g-buffer platform (RGBA8, colliding `GfxDeviceInfo` offset). Historical.
  - Branch `claude/dusklight-platform-rebuild-rqhsaw` / `platform-v2-test` (`9361fbd9ea`) = the
    superseded pre-g-buffer platform.
  - Branch `claude/standalone-final` + the `standalone-final` release = the pre-mod-API aurora-fork
    build; that build is the ONLY way the graphics features run on iOS (code mods cannot run there —
    dlopen restriction), so never delete it. (`mod-platform` / `platform-v1` are the superseded
    first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork, the renderer under the platform above. Branch
  `claude/dusklight-thin-gbuffer-normals-l4l9dc` (`49d644e`) = upstream aurora `cf3ffc9` (which
  carries upstream's own RenderTargetLayout refactor) + the optional normal attachment and nothing
  else — **the enlarged streaming buffers are no longer on this branch**; this is the
  `extern/aurora` submodule pin `dusklight-ao` records. Also force-pushed on rebase. Other branches remain the frozen fork the `standalone-final` build uses.
- `TwilitRealm/dusklight` — upstream. Our fork tracks it; a mod session does not need it attached.
