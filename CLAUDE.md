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
- **`mods/deferred_fog/`** — "Deferred Fog": suppresses the game's per-draw fog during the
  opaque world lists and re-applies it (bit-exact aurora fog math) as a fullscreen pass after
  every mod's `SCENE_AFTER_OPAQUE` composites, so AO/shadows darken surfaces under the fog
  instead of the fog itself. Standalone: other mods need no changes to benefit. Scenes with
  mixed fog configurations (twilight/wolf-senses special fog) auto-revert to vanilla fog.
  **Game-linked**.
- **`mods/depth_to_normal/`** — "Depth to Normal": reconstructs a per-pixel world-space surface
  normal (+ raw depth) from the scene depth buffer once per frame (atyuwen's 5-tap method) and
  publishes it as a mod-exported service other mods consume via `include/depth_to_normal_service.h`.
  Has no settings of its own. **Service-only**. Docs: `docs/depth_to_normal_plan.md`,
  `docs/depth_to_normal_consumers.md`.
- **`mods/ssilvb/`** — "SSILVB" (Screen Space Indirect Lighting with Visibility Bitmask,
  Therrien et al. 2023 — the mod carries the paper's name): VBAO's bitmask sampling chain extended
  with a one-bounce indirect-diffuse accumulate; with the bounce toggled off it doubles as a
  standalone directional-AO mod. Consumes the scene-color snapshot as its light input and the
  Depth to Normal service (hard dependency) for per-sample normals; composites GI additively and
  AO multiplicatively in a single blend draw. **Service-only**. Docs: `docs/ssilvb_plan.md`
  (§0 first — see the note below), then `docs/ssilvb.md` once written.

  **Working mode (user's explicit standing instruction): the technical direction of SSILVB rests
  with Claude.** The user is an amateur on SSAO/SSGI internals and cannot provide technical
  direction on the algorithm, math, or rendering architecture — never block on them for such
  decisions or offer them implementation options to pick from. They provide in-game testing,
  screenshots, and taste-level feedback ("too strong", "flickers here"); translate that feedback
  into fixes yourself. Full statement: `docs/ssilvb_plan.md` §0.

Each mod is `src/mod.cpp` (host code: pipelines, config vars, UI panel) plus `res/*.wgsl`
(shaders). Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`,
`docs/deferred_fog.md`, and `docs/mod-api-notes.md` (pitfalls — read before touching
uniforms or render code).

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
  outlines) should follow the VBAO / `depth_to_normal` pattern: consume depth + the world-space
  normal from the **Depth to Normal service** (`include/depth_to_normal_service.h`) + the scene
  color, all via mod-API services — **no game headers, no hooks**. That keeps it off the ABI
  treadmill: it survives game updates and needs no platform rebuild. `docs/depth_to_normal_consumers.md`
  is the menu of exactly these effects plus the consumer integration boilerplate — read it first.
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
  commit `9361fbd9ea` on branch `claude/dusklight-platform-rebuild-rqhsaw` — **pristine upstream
  Dusklight `76b56cd8`** (the base the official mod template pins) plus exactly two deltas: the
  release-publishing job and `extern/aurora` repointed to our enlarged-buffer aurora fork. That
  platform is published as the **`platform-v2-test`** release. The mods must be built against the SDK
  matching the game build the user runs. Linux links with `--allow-shlib-undefined` (no game lib);
  Windows/macOS/Android **auto-download** their per-arch link stub from `DUSKLIGHT_SDK_STUB_URL` (our
  `platform-v2-test` release, which publishes `windows-<arch>.lib` and `stub-<platform>` as top-level
  assets). Bump `DUSKLIGHT_VERSION` **only** when deliberately re-platforming, never as a side effect
  of a mod change.

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
  - Branch `claude/dusklight-platform-rebuild-rqhsaw` = **the current mod platform** (pristine upstream
    Dusklight `76b56cd8` + the release job + the aurora-ao submodule repoint), published as
    `platform-v2-test`. This is what `DUSKLIGHT_VERSION` pins (commit `9361fbd9ea`).
  - Branch `claude/standalone-final` + the `standalone-final` release = the pre-mod-API aurora-fork
    build; that build is the ONLY way the graphics features run on iOS (code mods cannot run there —
    dlopen restriction), so never delete it. (`mod-platform` / `platform-v1` are the superseded
    first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork. Branch `claude/dusklight-platform-rebuild-rqhsaw` =
  the platform's aurora (mainline-pinned + enlarged buffers, above). Other branches remain the
  frozen fork the `standalone-final` build uses.
