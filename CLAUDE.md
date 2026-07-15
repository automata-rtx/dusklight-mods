# dusklight-mods

Graphics mods for Dusklight (the Twilight Princess PC/mobile port), built on its mod API:

- **`mods/enhanced_ao/`** — "Enhanced Ambient Occlusion" (**VBAO**): a 32-sector
  visibility-bitmask AO estimator with temporal accumulation, edge-aware denoise, and
  depth-aware compositing. **Service-only**: it uses only mod-API services (gfx, camera,
  config, ui, resource, log) — it must NOT include game headers or call game code, which is
  what lets it survive game updates without a rebuild.
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

Each mod is `src/mod.cpp` (host code: pipelines, config vars, UI panel) plus `res/*.wgsl`
(shaders). Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`,
`docs/deferred_fog.md`, and `docs/mod-api-notes.md` (pitfalls — read before touching
uniforms or render code).

## First run

`extern/dusklight` is a submodule (the pinned game/SDK source). If that directory is empty
(a clone that didn't init submodules), run `git submodule update --init --recursive` before
building or relying on the game headers. CI already checks out submodules recursively.

## What a change does and does not require

Editing a shader or tuning a default touches ONE file here. It does **not** require building
the game, building aurora, or touching `extern/dusklight` (a read-only pinned reference).
CI compiles all mods and validates every shader in a few minutes.

The user typically does not build locally. Iteration loop:
1. Edit, commit, push (branch per the session's instructions).
2. GitHub Actions produces one `.dusk` package per mod
   (artifact `dusklight-mods-win64`).
3. User downloads them into `%APPDATA%\TwilitRealm\Dusklight\mods`, then uses the in-game
   mod manager's **Reload** button — no game restart needed.

## Hard constraints

- **Windows mods MUST be built with clang-cl, not plain MSVC (`cl`).** The mod SDK places its
  `modmeta` records via `__declspec(allocate("modmeta$d"))`, and the `DEFINE_HOOK` records are
  template statics. `cl` mishandles this: it strips the hook records under `/OPT:REF` (Release)
  and emits a malformed, over-padded section otherwise, so the game reports
  *"tried to hook undeclared target … hook targets must be declared with DEFINE_HOOK"* and the
  game-linked mods fail to load. The SDK only grants the retention (`used`) attribute under
  `__clang__`. Configure with `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`
  (or the `windows-clang` CMake preset). CI's Windows job and the `hook-repro` regression guard
  (`tools/hook_repro/`) both enforce this. Service-only mods (VBAO, Depth-to-Normal) have no
  hooks and are unaffected.
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
  mod or needs an upstream service extension — don't add game includes to enhanced_ao.
- **The ABI pin**: `extern/dusklight` (submodule) is pinned to a **mainline Dusklight** commit
  carrying the #2215 mod SDK (static `modmeta` metadata, `MOD_ABI_VERSION 1`, headers under
  `sdk/include/mods`, `add_mod FEATURES`). The mods must be built against the SDK matching the
  game build the user runs. The Linux CI job compile-checks against this SDK with no game lib;
  the Windows `.dusk` job needs the game's import library and is gated on the `PLATFORM_RELEASE_TAG`
  repo variable (unset = skipped; build locally with `-DDUSK_GAME_EXE=<sdk/windows-<arch>.lib>`).
  Never bump the submodule as a side effect of a mod change.

## Re-platforming (moving to a newer base game)

Only when explicitly asked: build/tag a new `platform-vN` release in
`automata-rtx/dusklight-ao` (branch `mod-platform`; its release workflow packages the game,
`dusklight.lib`, and `dusklight.symdb`), then in this repo bump the `extern/dusklight`
submodule and `RELEASE_TAG` together, rebuild, and have the user install the new game build
and fresh `.dusk` files as a pair. The shadow mod is the ABI-sensitive one; VBAO usually
just works.

## Related repos (context only — not needed for mod work)

- `automata-rtx/dusklight-ao` — our Dusklight fork. Branch `mod-platform` = the pinned game
  platform (upstream + mod-API PR #2193, no bundled mods). Branch `claude/standalone-final`
  + the `standalone-final` release = the pre-mod-API aurora-fork build; that build is the
  ONLY way the graphics features run on iOS (code mods cannot run there — dlopen restriction),
  so never delete it.
- `automata-rtx/aurora-ao` — frozen aurora fork used only by that standalone build.
