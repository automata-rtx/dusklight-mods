# dusklight-mods

Two graphics mods for Dusklight (the Twilight Princess PC/mobile port), built on its mod API:

- **`mods/enhanced_ao/`** — "Enhanced Ambient Occlusion" (**VBAO**): a 32-sector
  visibility-bitmask AO estimator with temporal accumulation, edge-aware denoise, and
  depth-aware compositing. **Service-only**: it uses only mod-API services (gfx, camera,
  config, ui, resource, log) — it must NOT include game headers or call game code, which is
  what lets it survive game updates without a rebuild.
- **`mods/realtime_sun_shadows/`** — "Realtime Sun Shadows": a real-geometry sun/moon shadow
  map (game draw-list replay into a light-space depth pass) with PCF, slope-scaled bias,
  normal-offset receiver, contact shadows, and indoor auto-disable. **Game-linked**: it
  includes game headers and hooks game functions, so it is coupled to the pinned game build.

Each mod is `src/mod.cpp` (host code: pipelines, config vars, UI panel) plus `res/*.wgsl`
(shaders). Deep documentation: `docs/vbao.md`, `docs/realtime_sun_shadows.md`, and
`docs/mod-api-notes.md` (pitfalls — read before touching uniforms or render code).

## First run

`extern/dusklight` is a submodule (the pinned game/SDK source). If that directory is empty
(a clone that didn't init submodules), run `git submodule update --init --recursive` before
building or relying on the game headers. CI already checks out submodules recursively.

## What a change does and does not require

Editing a shader or tuning a default touches ONE file here. It does **not** require building
the game, building aurora, or touching `extern/dusklight` (a read-only pinned reference).
CI compiles both mods and validates every shader in a few minutes.

The user typically does not build locally. Iteration loop:
1. Edit, commit, push (branch per the session's instructions).
2. GitHub Actions produces `enhanced_ao.dusk` + `realtime_sun_shadows.dusk`
   (artifact `dusklight-mods-win64`).
3. User downloads them into `%APPDATA%\TwilitRealm\Dusklight\mods`, then uses the in-game
   mod manager's **Reload** button — no game restart needed.

## Hard constraints

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
- **The ABI pin**: `extern/dusklight` (submodule) and `RELEASE_TAG` in
  `.github/workflows/build.yml` point at the `platform-v1` release of
  `automata-rtx/dusklight-ao` — the exact game build the user has installed. Never bump one
  without the other, and never bump either as a side effect of a mod change.

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
