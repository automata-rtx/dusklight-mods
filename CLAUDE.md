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

Each mod is `src/mod.cpp` (host code: pipelines, config vars, UI panel) plus `res/*.wgsl`
(shaders). Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`,
`docs/deferred_fog.md`, and `docs/mod-api-notes.md` (pitfalls — read before touching
uniforms or render code).

## Build model (official mod template)

This repo is built on the **official Dusklight mod template**
(`https://github.com/TwilitRealm/mod-template`): the pinned game/SDK source is **fetched** by
`cmake/FetchDusklight.cmake` (not vendored as a submodule) into `dusklight/` (git-ignored), keyed
by `DUSKLIGHT_VERSION` in the top-level `CMakeLists.txt`. There is **no submodule** — a plain
`git clone` is enough; the first `cmake -B build` fetches the SDK automatically. We deviate from
the stock template only where our pinned platform lags the newer upstream SDK it targets (link
targets + clang-cl — see below); those bridges are marked to be deleted once we re-platform.

## What a change does and does not require

Editing a shader or tuning a default touches ONE file here. It does **not** require building
the game, building aurora, or editing the fetched `dusklight/` tree (a read-only pinned
reference). CI compiles all mods on every platform and validates every shader in a few minutes.

The user typically does not build locally. Iteration loop:
1. Edit, commit, push (branch per the session's instructions).
2. GitHub Actions builds each mod on all 6 platforms and merges them into one **cross-platform
   `.dusk` per mod** (artifact `mods-combined`; per-platform artifacts are `mods-<platform>`).
3. User downloads them into `%APPDATA%\TwilitRealm\Dusklight\mods` (or the platform equivalent),
   then uses the in-game mod manager's **Reload** button — no game restart needed.

## Building a new mod (session setup + which pattern)

- **Which repos to attach to the session:** for building or tuning ANY mod (including a
  brand-new one), you need **only `automata-rtx/dusklight-mods`**. The game SDK you compile
  against is **fetched over the network** by `cmake/FetchDusklight.cmake` (from
  `automata-rtx/dusklight-ao` at the pinned `DUSKLIGHT_VERSION`), and the per-platform link
  targets are downloaded from the `platform-v2-test` release by CI — so neither `dusklight-ao`
  nor `aurora-ao` needs to be attached to the session. Attach them **only when re-platforming**
  (see that section below), never for authoring a mod on the current platform.
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

- **Windows mods MUST be built with clang-cl, not plain MSVC (`cl`).** The mod SDK places its
  `modmeta` records via `__declspec(allocate("modmeta$d"))`, and the `DEFINE_HOOK` records are
  template statics. `cl` mishandles this: it strips the hook records under `/OPT:REF` (Release)
  and emits a malformed, over-padded section otherwise, so the game reports
  *"tried to hook undeclared target … hook targets must be declared with DEFINE_HOOK"* and the
  game-linked mods fail to load. The SDK only grants the retention (`used`) attribute under
  `__clang__`. Configure with `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`
  (or the `windows-clang` CMake preset). The CI `build windows-amd64` leg forces clang-cl and the
  `hook-repro` guard (`tools/hook_repro/`) both enforce this — the guard is essential because a
  `cl`-built mod *builds* fine and only fails hook registration at runtime in-game, which CI can't
  otherwise catch. (The stock template uses plain `cl`; its newer SDK grants record retention under
  MSVC too. Our pinned platform doesn't yet, so we keep clang-cl until we re-platform.) Service-only
  mods (VBAO, Depth-to-Normal) have no hooks and are unaffected.
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
  (fetched by `cmake/FetchDusklight.cmake` from `DUSKLIGHT_REPOSITORY`, default
  `automata-rtx/dusklight-ao`). It currently points at commit `c18dc4223f` on branch
  `claude/dusklight-platform-rebuild-rqhsaw` — upstream mainline Dusklight `0f2a00cd` (the #2215 mod
  SDK: static `modmeta` metadata, `MOD_ABI_VERSION 1`, headers under `sdk/include/mods`,
  `add_mod FEATURES game|webgpu`) plus the Windows hook fix `adfb830b` and embedded symdb #2216, with
  its `extern/aurora` repointed to our enlarged-buffer aurora fork. That platform is published as the
  **`platform-v2-test`** release. The mods must be built against the SDK matching the game build the
  user runs. Linux links with `--allow-shlib-undefined` (no game lib); Windows/macOS/Android need a
  per-arch link target (`DUSK_GAME_EXE`) that CI pulls from `platform-v2-test` — the `sdk/` stub
  inside each game zip, or `windows-amd64.lib` (`PLATFORM_RELEASE_TAG`/`PLATFORM_REPO` repo vars
  override tag/repo). Bump `DUSKLIGHT_VERSION` **only** when deliberately re-platforming, never as a
  side effect of a mod change.

## Re-platforming (moving to a newer base game)

Only when explicitly asked. The platform is built from **two** repos, both on branch
`claude/dusklight-platform-rebuild-rqhsaw`:

1. **`automata-rtx/aurora-ao`** carries our only aurora change: the enlarged per-frame streaming
   buffers in `lib/gfx/common.hpp` (Index 4 MB, Vertex 16 MB, Storage 16 MB; Uniform/TextureUpload
   24 MB). It is otherwise pristine upstream `encounter/aurora`, reset to the exact commit mainline
   Dusklight pins (so the ABI matches). To re-platform, reset it to the new mainline-pinned aurora
   commit and re-apply the buffer edit.
2. **`automata-rtx/dusklight-ao`** is pristine upstream Dusklight with only two deltas: a
   release-publishing job grafted into `.github/workflows/build.yml` (gated to this branch), and
   its `extern/aurora` submodule repointed at the aurora-ao fork above. Pushing it (re)publishes the
   **`platform-v2-test`** release idempotently, attaching the per-platform game zips and the
   standalone `windows-amd64.lib`. Each game zip also carries the mod **link target** for that
   platform under `sdk/` (`sdk/stub-macos-arm64`, `sdk/stub-macos-x86_64`, `sdk/stub-android-aarch64.so`,
   `sdk/windows-amd64.lib`, `sdk/windows-arm64.lib`) — this is what the mod CI feeds to
   `DUSK_GAME_EXE`. (The symbol manifest is embedded in `dusklight.exe` as of upstream #2216 — no
   standalone `.symdb`. Windows-arm64 mods are not built yet; the `sdk/windows-arm64.lib` inside the
   win32-msvc-arm64 zip is all that's needed to add that leg.)

Then in this repo bump **`DUSKLIGHT_VERSION`** in the top-level `CMakeLists.txt` to the new platform
commit (leaving `PLATFORM_RELEASE_TAG`/`PLATFORM_REPO` at `platform-v2-test`/`dusklight-ao` unless you
cut a new tag), rebuild, and have the user install the new game build (the `win32-msvc-x86_64` zip) and
fresh `.dusk` files as a pair. The shadow mod is the ABI-sensitive one; VBAO usually just works. Keep
both base repos pristine apart from the two deltas above so the link targets match upstream exactly.
If the new base's SDK auto-downloads link targets and grants MSVC record retention (as the newer
upstream template SDK does), the CI's link-target steps and the clang-cl override can be dropped.

## Related repos (context only — not needed for day-to-day mod work)

- `automata-rtx/dusklight-ao` — our Dusklight fork.
  - Branch `claude/dusklight-platform-rebuild-rqhsaw` = **the current mod platform** (upstream
    `0f2a00cd` + the release job + the aurora-ao submodule repoint), published as `platform-v2-test`.
    This is what `DUSKLIGHT_VERSION` pins (commit `c18dc4223f`).
  - Branch `claude/standalone-final` + the `standalone-final` release = the pre-mod-API aurora-fork
    build; that build is the ONLY way the graphics features run on iOS (code mods cannot run there —
    dlopen restriction), so never delete it. (`mod-platform` / `platform-v1` are the superseded
    first-generation platform — historical only.)
- `automata-rtx/aurora-ao` — our aurora fork. Branch `claude/dusklight-platform-rebuild-rqhsaw` =
  the platform's aurora (mainline-pinned + enlarged buffers, above). Other branches remain the
  frozen fork the `standalone-final` build uses.
