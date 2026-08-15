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
  pinned game build. **Two normals, never interchangeable**: the *shading* normal (the provider's
  authored one) drives `n·L` / attached shadows / normal offset, the *geometric* face normal drives
  the bias — see `docs/realtime_sun_shadows.md` "Shadow term assembly" and
  `docs/authored_normals.md` §8.6. The `normalSmooth` blur pass was **deleted**; do not
  reintroduce it. Debug View 15 ("Shadow Terms") is the view that diagnoses wrongly-lit pixels.
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
  Depth to Normal service (hard dependency, now exported by **Graphics Hub**) for per-sample
  normals; composites GI additively and AO multiplicatively in a single blend draw. Since 0.10.0 it
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
  Depth to Normal service** (normal-angle + relative-depth discontinuity — catches silhouettes and
  creases where luma contrast is weak). The expensive blend-weight pass uses **CMAA2-style compute
  compaction** (Intel 2018): edge pixels in each 16×16 workgroup are packed into contiguous threads
  via a groupshared list so sparse edges run in fully-occupied warps. Composites at
  `SCENE_AFTER_OPAQUE` (before bloom/translucency, so the game's post effects operate on
  antialiased geometry). Three passes: edge-detect (compute) → compacted blend-weights (compute) →
  neighborhood blend (draw). No LUT assets — orthogonal search is linear, coverage analytic; v1
  defers diagonals/corners. **Service-only** (gfx/config/ui/resource/log + optional Depth to Normal).
  The SMAA algorithm is reimplemented from the MIT reference (iryoku/smaa) — Marty's proprietary
  iMMERSE port was studied for the optimization ideas only, never copied. Docs: `docs/smaa.md`.
- **`mods/graphics_hub/`** — "[WIP] Graphics Hub": a **combination mod** hosting the screen-space
  infrastructure other mods build on, so effects layer correctly over the game's original rendering.
  It merges two former standalone mods, each in its own namespace inside `src/mod.cpp`
  (`hub_dtn` / `hub_fog`) with its own section in the shared UI panel and independent config:
  - **Depth to Normal** (`hub_dtn`): provides a per-pixel world-space surface normal (+ raw depth)
    once per frame and **publishes it as the mod-exported service**
    `include/depth_to_normal_service.h` (service id `dev.automata.depth_to_normal`, **unchanged** so
    SSILVB/Realtime Sun Shadows/VBAO/SMAA resolve it as before — they include
    `../graphics_hub/include`). Two sources: the game's **authored vertex normals** from a renderer
    thin g-buffer (smooth — no faceting), falling back per pixel to the **depth reconstruction**
    (atyuwen's 5-tap method) wherever there is no authored normal. The current platform **does**
    provide the buffer, but **it ships switched off**: the user must turn on the game's own
    **Video → Rendering → Scene Normal Buffer** and restart. Until then every pixel takes the
    reconstruction, "Use Authored Normals" greys out, and the status line names the setting to turn
    on — correct, not a regression. Same fallback on a base without the buffer, and in compatibility
    mode (D3D11 / OpenGL ES), where the renderer refuses it whatever the setting says. Passive
    provider: no on/off. Docs: `docs/authored_normals.md`, `docs/normal_buffer_portability.md`.
  - **Deferred Fog** (`hub_fog`): suppresses the game's per-draw fog during the opaque world lists
    and re-applies it (bit-exact aurora fog math) as a fullscreen pass after every mod's
    `SCENE_AFTER_OPAQUE` composites, so AO/shadows darken surfaces under the fog instead of the fog
    itself. Mixed fog configs auto-revert to vanilla (or exact per-pixel replay). Independently
    toggleable. Special-cases the Hyrule Castle Ganon barrier (`d_a_obj_ganonwall2`, a translucent
    dome drawn in the *opaque* BG list with pure-black `endZ 250000` fog) via `is_barrier_fog`: it
    is left on vanilla forward fog, never suppressed/deferred, so its black fog isn't stamped onto
    the castle/trees inside it. Also handles TP's **near-fog + distant-scenery-fog split** (Hyrule
    Field draws distant geometry — Death Mountain, the castle — with a *wider projection far plane*
    and a separate gentle long-range fog config): the single-projection config-ID replay clips that
    far geometry, so its pixels are uncovered; the fog quad falls those back to `widest_far_index()`
    (the widest-far config = the distant fog) rather than config 0 (the aggressive near fog), and
    the barrier dome stamps that same distant index in the replay — so distant subjects keep their
    correct light fog instead of being over-fogged toward the dark near fog. Diagnostic: a
    `fogLogConfigs` toggle dumps the frame's captured fog-config table.

  **Game-linked** (Deferred Fog hooks game functions) + webgpu. Docs: `docs/deferred_fog.md`,
  **`docs/authored_normals.md`** (§0 = state of play, §8 = the findings that each cost hours —
  read both before theorising about normals or shadows), `docs/depth_to_normal_plan.md`,
  `docs/depth_to_normal_consumers.md`.

  The service returns the **true surface direction**, not a camera-facing one. Consumers wanting
  camera-facing normals apply their own guard *with a margin* (VBAO and SSILVB already do, at 0.15);
  a hard zero threshold corrupts smooth normals near every silhouette.
- **`mods/effect_remover/`** — "Effect Remover": a **combination mod** that cuts down TP's built-in
  fake-shading so it doesn't fight the realtime stack. It merges three former standalone mods, each
  in its own namespace inside `src/mod.cpp` (`er_psr` / `er_tsr` / `er_vu`) with its own UI section
  and independent config:
  - **Projected Shadow Removal** (`er_psr`): pre-hooks `drawCloudShadow` (TP's "moya"
    projected-ground-shade draw — the kankyo cloud packet) and cancels it **per `mMoyaMode`**
    (per-area, set by `kytag` actors). Per-mode toggles; default removes only mode 5 (the slow-sway
    canopy candidate) and keeps the wind-driven cloud shadows (modes 4/11). `mMoyaMode >= 50`
    (heat-shimmer / senses) always preserved. Live mode logger.
  - **Terrain Shadow Removal** (`er_tsr`): the *other* fake shadow — a drifting dapple **overlay
    baked as a second TEV texture stage inside the terrain material** (not moya — moya count reads 0
    there). `dKy_cloudshadow_scroll` scrolls **texmtx 1** of `MA00`/`MA01`/`MA16` by the `vrkumo`
    packet (the sway); `dKy_bg_MAxx_proc` sets **TEV KColor 1**'s red to `g_env_light.mFogDensity`
    on `MA00`/`MA01`/`MA04`/`MA16` — **which is not fog density**: the game's own slider labels that
    field 雲影の濃さ, *cloud shadow* density, so this feature overrides the game's own cloud-shadow
    strength control (`docs/japanese-naming.md` §4.1). That red measures as a *wash-out* control
    (in-game test: 0 = **darker**, max = washed out), so `er_tsr` **post-hooks `dKy_bg_MAxx_proc`**
    and pins KColor 1's red to **255** — white into the shadow stage, base ground (stage 0)
    untouched, so it **does not hole the floor**. The polarity is **known-effective, not yet
    known-faithful**: the label predicts the opposite sign and the TEV equation is baked in the
    `.bmd`. **`MA04` is the confirmed Faron forest-floor shade.** Per-code toggles + logger. Off by
    default (global terrain change).
  - **Unbaked Vertex Lighting** (`er_vu`): post-hooks the J3D model loader
    (`J3DModelLoaderDataBase::load`/`loadBinaryDisplayList`) and rewrites each model's CLR0/CLR1
    vertex-color arrays in place — `rgb' = mix(white, rgb, vertexLight/100)` — 100 = vanilla, 0 =
    flat; alpha untouched; all six GX color formats; applies as models load (re-enter the area).

  **Game-linked**. EXPERIMENTAL. See `docs/fake_shading_systems.md` for the three systems + code
  names.

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
the scene normal buffer and the enlarged aurora streaming buffers) and `DUSKLIGHT_SDK_STUB_URL`
(that fork's `platform-normals-test` release, which publishes the per-arch link stubs as top-level
assets). `DUSKLIGHT_AURORA_VERSION` stays unset — the recorded `extern/aurora` pin resolves on its
own. Everything else is the template unchanged, so template updates apply cleanly.

## What a change does and does not require

Editing a shader or tuning a default touches ONE file here. It does **not** require building
the game, building aurora, or editing the fetched `dusklight/` tree (a read-only pinned
reference). CI compiles all mods on every platform and validates every shader in a few minutes.

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
  normal from the **Depth to Normal service** (`mods/graphics_hub/include/depth_to_normal_service.h`,
  exported by Graphics Hub) + the scene color, all via mod-API services — **no game headers, no
  hooks**. That keeps it off the ABI treadmill: it survives game updates and needs no platform
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
  target count does not match the pass. The helper wraps GfxService 1.2's `get_pass_targets`, which
  returns the pass's real targets with renderer-owned ones already write-masked off, and falls back
  to the hand-assembled layout on an SDK too old to have the query. Every stage that pushes draws
  lands in that pass; there is no exempt stage. Offscreen `create_pass` targets stay single-target.
  See `docs/authored_normals.md` §5.
- **Never touch an SDK normal-buffer field directly** — `GfxDeviceInfo::normal_format`,
  `GfxResolveDesc::normal`, `GfxResolvedTargets::normal`. Go through `common/gfx_normal_compat.h`
  (`gfx_compat::normal_format` / `request_normal` / `resolved_normal`), which detects each field at
  compile time and degrades to "no normal buffer" when it is absent. Those fields are **fork-local**
  — upstream Dusklight has none — so a direct access compiles today and breaks the whole tree on the
  next re-platform. Verified by building against a stripped SDK; see
  `docs/normal_buffer_portability.md`.
- **Degrade-to-absent is safe for a READ, not for a COMPARISON.** The draw callbacks used to guard
  on `gfx_compat::normal_format(*ctx) != gfx_compat::normal_format(g_deviceInfo)`. `GfxDrawContext`
  has no `normal_format` in this SDK, so that shim call is a constant `Undefined` while the device
  reports a real format — the guard fired on every draw and silently disabled all six composites the
  moment the user enabled the buffer. It is deleted. Do not reintroduce a guard that compares a
  compile-time-detected field against a live value.
- **VBAO stays service-only.** If a feature seems to need game code, it belongs in the shadow
  mod or needs an upstream service extension — don't add game includes to `vbao`.
- **The ABI pin**: the platform is pinned by **`DUSKLIGHT_VERSION` in the top-level `CMakeLists.txt`**,
  fetched from `DUSKLIGHT_REPOSITORY`. It currently points at **`0474043c`** in
  **`automata-rtx/dusklight-ao`** (branch `claude/dusklight-thin-gbuffer-normals-l4l9dc`, published
  as the **`platform-normals-test`** prerelease) — upstream Dusklight `008a18c1` plus the GfxService
  1.2 scene-normal-buffer additions, over an aurora with the optional normal target and the enlarged
  streaming buffers. `DUSKLIGHT_SDK_STUB_URL` tracks it to that same release;
  `DUSKLIGHT_AURORA_VERSION` stays unset. **`DUSKLIGHT_VERSION` must match the game build actually
  being run.**
  - **Why a fork again, and why this one is safe.** Two reasons: upstream `008a18c1` bumped the
    **game service major version**, so mods built against the older `0fc05028` SDK are refused
    outright by that host (every mod but SMAA failed to load); and GfxService 1.2 is what hands mods
    the authored normals. The offset collision that sank the *previous* fork is not repeated here —
    `normal_format` is appended **after** upstream's own `instance`/`adapter`, on top of upstream
    rather than beside it, so no slot is claimed twice.
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
    scene-pass pipelines (ssilvb, vbao, realtime_sun_shadows, smaa, and graphics_hub's deferred fog +
    normal overlay) take their layout from `gfx_compat::scene_pass_layout`, so they follow the pass
    whichever shape it has. Any new scene-pass mod must do the same.
  - **Authored normals are back — but off until the user switches them on.** The platform provides
    the buffer; the game ships it disabled (**Video → Rendering → Scene Normal Buffer**, applies on
    the next launch) and cannot provide it at all in compatibility mode. Until then Graphics Hub
    reconstructs from depth for every pixel and its status line says which setting to turn on. That
    is correct, not a regression. `common/gfx_normal_compat.h` makes a base *without* the buffer a
    compile-time non-event; see `docs/normal_buffer_portability.md`.
  - **Link stubs come from the same release as the game build.** `DUSKLIGHT_SDK_STUB_URL` points at
    `automata-rtx/dusklight-ao` `releases/download/platform-normals-test`, which publishes every
    asset our CI matrix needs (`windows-amd64.lib`, `windows-arm64.lib`, `stub-macos-arm64`,
    `stub-macos-x86_64`, `stub-android-aarch64.so`) as top-level assets. Linux needs no stub at all.
    Unlike upstream's version-independent `sdk` tag, **a fork release's stubs are per-build** — move
    `DUSKLIGHT_SDK_STUB_URL` whenever you move `DUSKLIGHT_VERSION`. Bump either **only** when
    deliberately re-platforming, never as a side effect of a mod change.
  - **Aurora's streaming buffers are the enlarged ones again** (Vertex 16 MB / Index 4 MB / Storage
    16 MB, vs upstream's 5 / 2 / 8). That removes the overflow risk the upstream pin carried for
    Realtime Sun Shadows' cascade replays — but it is also a reason a mod tuned here can overrun a
    stock-upstream base later. If the shadow mod aborts or drops geometry, reduce cascade count,
    shadow resolution or draw distance first — see `docs/realtime_sun_shadows.md`.

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
3. **Re-verify the game-linked mods in-game** — Realtime Sun Shadows, Graphics Hub's Deferred Fog,
   Effect Remover, Celestial Orbit. They hook specific game functions and a decomp delta can move or
   rename what they hook. The service-only mods (VBAO, SSILVB, SMAA) need no re-verification.
4. Watch for **streaming-buffer overflow** in the shadow mod if the new base uses upstream's aurora
   sizes (Vertex 5 MB / Index 2 MB / Storage 8 MB vs the fork's 16 / 4 / 16) — the cascade replays
   are the heaviest consumer.
5. If the new base has no scene normal buffer, expect Graphics Hub to say so and normals to be
   faceted. That is correct, and needs no source change.

**Forking the SDK is not free.** Appending fields to SDK structs is what caused the
`normal_format` / `WGPUInstance` offset collision documented under The ABI pin, and the current pin
avoids it only by appending *after* upstream's own fields. Prefer upstreaming a platform feature
over carrying a delta — see `docs/authored_normals.md` §9.5.

## Related repos

- `automata-rtx/dusklight-ao` — **our Dusklight fork, and the current platform.**
  `DUSKLIGHT_VERSION` pins a commit here and `DUSKLIGHT_SDK_STUB_URL` a release here.
  - Branch `claude/dusklight-thin-gbuffer-normals-l4l9dc` / `platform-normals-test` (`0474043c`) =
    **the live platform**: upstream `008a18c1` + GfxService 1.2's scene normal buffer.
    `dusklight-ao/docs/thin-gbuffer-normals.md` is the renderer-side design. The three
    `TEST SCAFFOLDING` commits on the tip (aurora submodule pin, release job, an unrelated debug
    toggle removal) are meant to be dropped before any upstream PR — dropping them would move the
    pin, so re-pin if that happens.
  - Branch `claude/thin-gbuffer-authored-normals-wgqupt` / `platform-gbuffer-test` (`b96bf5ec01`) =
    the **retired** first g-buffer platform (RGBA8, colliding `GfxDeviceInfo` offset). Historical.
  - Branch `claude/dusklight-platform-rebuild-rqhsaw` / `platform-v2-test` (`9361fbd9ea`) = the
    superseded pre-g-buffer platform.
  - Branch `claude/standalone-final` + the `standalone-final` release = the pre-mod-API aurora-fork
    build; that build is the ONLY way the graphics features run on iOS (code mods cannot run there —
    dlopen restriction), so never delete it. (`mod-platform` / `platform-v1` are the superseded
    first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork, the renderer under the platform above. Branch
  `claude/dusklight-thin-gbuffer-normals-l4l9dc` (`f8d8c035`) = upstream aurora `59c2b97` + the
  optional normal target + the enlarged streaming buffers; this is the `extern/aurora` submodule pin
  `dusklight-ao` records. Other branches remain the frozen fork the `standalone-final` build uses.
- `TwilitRealm/dusklight` — upstream. Our fork tracks it; a mod session does not need it attached.
