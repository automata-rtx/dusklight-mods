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
  optional Link-only cascade) with PCF, slope-scaled bias, normal-offset receiver, two-sided
  casters, Bend-style screen-space shadows, and indoor auto-disable. **Game-linked**: it
  includes game headers and hooks game functions, so it is coupled to the pinned game build.
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
  - **Depth to Normal** (`hub_dtn`): reconstructs a per-pixel world-space surface normal (+ raw
    depth) from the scene depth buffer once per frame (atyuwen's 5-tap method) and **publishes it as
    the mod-exported service** `include/depth_to_normal_service.h` (service id
    `dev.automata.depth_to_normal`, **unchanged** so SSILVB/Realtime Sun Shadows/VBAO resolve it as
    before — they include `../graphics_hub/include`). Passive provider: no on/off, just a debug view.
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
  `docs/depth_to_normal_plan.md`, `docs/depth_to_normal_consumers.md`.
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
`git clone` + `cmake -B build` fetches it and the per-arch link stub automatically. The **only**
fork-specific bits are two supported template cache knobs in the top-level `CMakeLists.txt`:
`DUSKLIGHT_REPOSITORY` (→ our `dusklight-ao` fork, for the enlarged aurora buffers) and
`DUSKLIGHT_SDK_STUB_URL` (→ our public `platform-v2-test` release, since upstream's default stub URL
is private). Everything else tracks the template, so template updates apply cleanly.

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

- **Which repos to attach to the session:** for building or tuning ANY mod (including a
  brand-new one), you need **only `automata-rtx/dusklight-mods`**. The game SDK you compile
  against is **fetched over the network** by `cmake/FetchDusklight.cmake` (from
  `automata-rtx/dusklight-ao` at the pinned `DUSKLIGHT_VERSION`), and the SDK **auto-downloads** the
  per-arch link stub from `DUSKLIGHT_SDK_STUB_URL` (our `platform-v2-test` release) — so neither
  `dusklight-ao` nor `aurora-ao` needs to be attached to the session. Attach them **only when
  re-platforming** (see that section below), never for authoring a mod on the current platform.
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
- **VBAO stays service-only.** If a feature seems to need game code, it belongs in the shadow
  mod or needs an upstream service extension — don't add game includes to `vbao`.
- **The ABI pin**: the platform is pinned by **`DUSKLIGHT_VERSION` in the top-level `CMakeLists.txt`**
  (fetched from `DUSKLIGHT_REPOSITORY`, our `automata-rtx/dusklight-ao` fork). It currently points at
  commit `b96bf5ec01` on branch `claude/thin-gbuffer-authored-normals-wgqupt`, published as the
  **`platform-gbuffer-test`** release — that is `platform-v2-test` (pristine upstream Dusklight
  `76b56cd8` + the release job + the enlarged-buffer aurora fork) **plus the thin g-buffer**: aurora
  writes the game's authored view-space normals to a second RGBA8 target, exposed to mods as
  `GfxResolveDesc::normal` / `GfxResolvedTargets::normal`. `DUSKLIGHT_SDK_STUB_URL` must always point
  at the release matching this pin.
  - **Struct-size ABI is one-directional.** The host rejects callers whose
    `struct_size < sizeof(host struct)`, so mods built against an OLDER SDK are refused outright by a
    newer game (symptom: every webgpu mod dies at init with `failed to query device info`, and the
    service consumers then fail on the missing `depth_to_normal` import). The reverse is fine — a
    LARGER `struct_size` passes an older host's check and the appended fields keep their `GFX_*_INIT`
    defaults — so mods built here also run on `platform-v2-test`. **When in doubt build against the
    newest platform.**
  - **Two color attachments.** When the host's normal buffer is enabled the scene pass has two color
    targets, and *every* pipeline recorded into it must declare two or WebGPU rejects the draw. All
    five scene-pass composites (ssilvb, vbao, realtime_sun_shadows, graphics_hub's deferred fog,
    smaa) declare a write-masked second target when `GfxDeviceInfo::normal_format` is set, and skip
    the draw if `GfxDrawContext::normal_format` ever disagrees at record time. **Any new mod that
    draws into the scene pass must do the same.**
  - `DUSKLIGHT_AURORA_VERSION` (third fork knob) overrides the `extern/aurora` submodule pin, which
    the thin-gbuffer commit left dangling after an aurora-ao force-push. Drop it once that pin is
    repaired upstream.
  - Linux links with `--allow-shlib-undefined` (no game lib); Windows/macOS/Android **auto-download**
    their per-arch link stub from `DUSKLIGHT_SDK_STUB_URL`. Bump `DUSKLIGHT_VERSION` **only** when
    deliberately re-platforming, never as a side effect of a mod change.

## Re-platforming (moving to a newer base game)

Only when explicitly asked. The platform is built from **two** repos, both on branch
`claude/dusklight-platform-rebuild-rqhsaw`, each **pristine upstream + a tiny delta**:

1. **`automata-rtx/aurora-ao`** carries our only aurora change: the enlarged per-frame streaming
   buffers in `lib/gfx/common.hpp` (Index 4 MB, Vertex 16 MB, Storage 16 MB; Uniform/TextureUpload
   24 MB). It is otherwise pristine upstream `encounter/aurora` at the exact commit the new Dusklight
   base pins (currently `81f12f31`). To re-platform: `git rebase --onto <new-aurora> <old-aurora>` (or
   reset + re-apply the one buffer commit), then push.
2. **`automata-rtx/dusklight-ao`** is pristine upstream Dusklight (currently `76b56cd8`) with only two
   deltas: the release-publishing job grafted into `.github/workflows/build.yml` (gated to this
   branch), and `extern/aurora` repointed to the aurora-ao fork above. To re-platform:
   `git reset --hard <new-upstream>`, re-apply the aurora repoint (`.gitmodules` url +
   `git update-index --cacheinfo 160000,<aurora-ao-sha>,extern/aurora`), re-graft the release job,
   then force-push. Since the upstream build jobs rarely change, the release job usually re-applies
   verbatim. The release publishes the per-platform game zips **and** the SDK link stubs as top-level
   assets (`windows-<arch>.lib`, `stub-macos-<arch>`, `stub-android-aarch64.so`) that the mod SDK
   auto-downloads.

Then in this repo bump **`DUSKLIGHT_VERSION`** to the new `dusklight-ao` commit (and
`DUSKLIGHT_SDK_STUB_URL` only if the release tag changes). Have the user install the new game build
(the `win32-msvc-x86_64` zip) and fresh `.dusk` files as a pair. **VBAO and Depth-to-Normal are
service-only and just work; the game-linked shadow/fog mods hook specific functions, so re-verify them
in-game against the new base** (a game-code delta can shift what they hook). Keep both base repos
pristine apart from the deltas above so upstream's SDK (auto-stub-download, `cl` support) keeps working.

## Related repos (context only — not needed for day-to-day mod work)

- `automata-rtx/dusklight-ao` — our Dusklight fork.
  - Branch `claude/thin-gbuffer-authored-normals-wgqupt` = **the current mod platform**
    (`platform-v2-test` + the thin g-buffer authored-normal target), published as
    `platform-gbuffer-test`. This is what `DUSKLIGHT_VERSION` pins (commit `b96bf5ec01`). Its
    `extern/aurora` pin is dangling — see `DUSKLIGHT_AURORA_VERSION` above.
  - Branch `main` = the stable platform (pristine upstream Dusklight `76b56cd8` + the release job +
    the aurora-ao submodule repoint), published as `platform-v2-test`. Mods built against the
    g-buffer SDK still run on it (struct-size ABI is one-directional — see The ABI pin).
    Note `claude/dusklight-platform-rebuild-rqhsaw` (commit `9361fbd9ea`) was the previous pin.
  - Branch `claude/standalone-final` + the `standalone-final` release = the pre-mod-API aurora-fork
    build; that build is the ONLY way the graphics features run on iOS (code mods cannot run there —
    dlopen restriction), so never delete it. (`mod-platform` / `platform-v1` are the superseded
    first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork. Branch `claude/dusklight-platform-rebuild-rqhsaw` =
  the platform's aurora (mainline-pinned + enlarged buffers, above). Other branches remain the
  frozen fork the `standalone-final` build uses.
