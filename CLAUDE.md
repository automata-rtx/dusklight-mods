# dusklight-mods (instructions for AI coding sessions)

Graphics mods for Dusklight, the Twilight Princess PC/mobile port, built on its mod API.

**The human documentation is the source of truth; this file is the short version plus the rules
specific to AI sessions.** Read `CONTRIBUTING.md` before changing code, and the per-mod doc before
changing a mod. If this file disagrees with the docs or the code, the code wins, then the docs; fix
whichever is wrong in the same change.

**This branch (`claude/reshade-bridge-*`) builds only the ReShade Bridge** (`mods/reshade-bridge/`,
doc `docs/reshade_bridge.md`): a game-linked Dusklight mod plus a ReShade add-on (`.addon64`, built
by the windows-amd64 CI leg and shipped in `mods-combined`), which together run an installed
ReShade's techniques at chosen points inside the frame. Works in-game (maintainer-tested); 0.2.0's
hand-over at ReShade's screen size when the internal resolution differs (doc "Resolution") is not
yet tested, and the doc's "Ideas for later" holds the agreed next steps. The released mods below
keep main's sources here, unbuilt; work on them on main.

| Released mod | Kind | Doc |
| :-- | :-- | :-- |
| `mods/vbao/` — visibility-bitmask ambient occlusion | service-only | `docs/vbao.md` |
| `mods/deferred_fog/` — the game's fog re-applied after the opaque world | game-linked | `docs/deferred_fog.md` |
| `mods/smaa/` — SMAA 1x post-process antialiasing | service-only | `docs/smaa.md` |

Four more mods (SSILVB, Realtime Sun Shadows, Celestial Orbit, Effect Remover) are **not built or
released**. Their docs are in `docs/unreleased/`, marked as possibly outdated; start at
`docs/unreleased/README.md`. Do not edit them unless asked to port one back.

Other docs: `docs/README.md` (index), `docs/editing-options.md` (defaults, hiding options),
`docs/mod-api-notes.md` (API pitfalls, crash symbolization), `docs/normal_buffer_portability.md`
and `docs/authored_normals.md` (the normal buffer), `docs/japanese-naming.md` (game identifiers).

## Working with the maintainer

- The maintainer usually does not build locally. The loop is: commit and push to the session's
  branch → CI builds all seven platforms in a few minutes (artifact `mods-combined`) → the
  maintainer installs the `.dusk` files, presses **Reload** in the mod manager, and reports back
  with screenshots and in-game observations.
- In-game testing is the only real test. A green CI build proves the C++ compiles, nothing more:
  CI does not validate shaders, hook targets, or behaviour.
- **SSILVB (unreleased): technical direction rests with Claude.** The maintainer is an amateur on
  SSAO/SSGI internals and gives taste-level feedback ("too strong", "flickers here"); translate it
  into fixes yourself rather than offering algorithm choices. Full statement:
  `docs/unreleased/ssilvb_plan.md` §0.
- **Measure before theorising.** Several fixes here were built on plausible but unmeasured
  mechanisms and failed. Prefer shipping a diagnostic (a debug view, a log line, a Status field) over
  shipping a guess. A debug view must read the same resource, under the same conditions, as the
  effect it diagnoses.

## Decisions already made

- **MXAO**: VBAO's `mod.json` description ("Inspired by iMMerse's MXAO Reshade filter") is the sole
  permitted mention of MXAO. Never reference it in code, comments or docs; the code is our own and
  a stray mention invites false plagiarism claims.
- **VBAO 1.1.1 "Normal Repair"** (branch `claude/vbao-amd-flickering-xbualq`) was a failed
  experiment and is intentionally unmerged. Do not merge or revive it.

## Deferred Fog's open issue: read before touching it

Distant landmarks (Death Mountain, the Ganon barrier) look brighter with the mod **off**. Three fixes
have shipped and failed; the maintainer has parked it. **Do not propose a fourth mechanism from the
Status-line counters alone.** Twice a per-frame counter correctly showed that a mechanism was
*present* in the view without showing it was what the view *looks like*. The next step needs
per-pixel evidence (the Fog Factor and Config IDs debug views) plus the `markable / no-Z / alpha`
breakdown that was never captured. The evidence, the failed fixes and the decision table are in
`docs/deferred_fog.md` "Known issues".

## Hard constraints

Each of these has caused a silent failure (green build, effect missing in-game). Details:
`CONTRIBUTING.md` "Rules that have bitten before" and `docs/normal_buffer_portability.md`.

- **VBAO and SMAA stay service-only.** No game headers, no hooks. If a feature needs game code it
  belongs in a game-linked mod or needs an upstream service extension.
- **Scene-pass render pipelines are built lazily from the live `GfxDrawContext::layout` and rebuilt
  when `layout.key` changes** (`gfx_compat::scene_pass_layout_for_draw` /
  `scene_pass_layout_key`, `common/gfx_scene_pass.h`). The pass gains a normal attachment at
  runtime, the frame after any mod requests normals. Building at init is a bug. Copy
  `ensure_composite_pipelines()` (VBAO), `ensure_neighborhood_pipeline()` (SMAA) or
  `ensure_fog_pipelines()` (Deferred Fog).
- **The normal snapshot latches**: the first request returns null and enables it for the next
  frame. Do not report the first nulls as "no normals" (`kNormalLatchGraceFrames` in VBAO and SMAA).
- **Never touch `GfxResolveDesc::normal` / `GfxResolvedTargets::normal` directly**; use
  `common/gfx_normal_compat.h`. **`normal_format` does not exist on any SDK struct**; ask
  `ScenePassLayout::has_normal_attachment`. Degrade-to-absent is safe for a value you *read*, never
  for one you *compare* against a live value.
- **A renamed API looks like an absent one.** `gfx_scene_pass.h` therefore `#error`s on an SDK it
  does not recognise. After any pin bump, read the new SDK header; a green build proves nothing.
- **Never name a config var `enabled`** (the host reserves it; the mod fails to load).
  `tools/check_reserved_config_names.py` does not see names in VBAO's and SMAA's option tables.
- **Uniform structs are mirrored C++ ↔ WGSL**, byte for byte, size a multiple of 16; keep the
  `static_assert`s. Every shader that declares the struct must be updated.
- **Threads**: stage hooks and game hooks run on the game thread; draw/compute callbacks run on the
  render worker with only their payload (≤ 128 bytes) and `wgpu*` calls. Never touch game state,
  config or the log service from the render worker.
- **Resolved views are valid for the current frame only**; everything the mod creates is released
  in `mod_shutdown`.
- **Reversed-Z everywhere** (1 = near, sky raw depth 0).
- **Build `RelWithDebInfo`.** On Windows a `Release` link strips the `DEFINE_HOOK` records and
  hooks never register. Windows uses plain MSVC (`cl`); no clang-cl override anywhere.
- **CI does not validate WGSL.** Run `build/wgsl_check` on changed shaders before pushing (recipe in
  `CONTRIBUTING.md` "Check your change").
- **Game-linked code: a clean compile is not verification.** `DEFINE_HOOK` checks the signature at
  compile time; the symbol is resolved by name at load. Check every hook target exists in the
  fetched tree, then test in-game.

## The game's code is named in Japanese

Every game identifier our game-linked code hooks, reads or includes is the original Japanese team's
name, preserved by the decompilation: romaji, abbreviated Japanese, or English spelled by ear.
`kankyo` (環境) is *environment* (hence `dKy_`), `moya` (靄) is haze, `kumo` (雲) is cloud, and
`wether` in `dKyw_wether_move` is *weather* (not a typo). Read as English, these names have already
produced wrong conclusions in this repo's docs. `docs/japanese-naming.md` is the reference.

- **Search Japanese with ripgrep** (`rg`, or the Grep tool), not `grep -P`. This container's locale
  is `POSIX`: `grep -P '\p{Han}'` silently matches nothing, and a raw kana/kanji character class
  matches too much. 496 files in the game tree contain the original team's Japanese debug labels,
  the most authoritative documentation of what a field means.
- **Search both romanizations** (kunrei `si`/`tu`/`ti`/`sya` and Hepburn `shi`/`tsu`/`chi`/`sha`).
  An empty search is not evidence of absence.
- **A struct member name is the decompilers' guess**; function and global names are original. Treat
  a member name as a hypothesis until an authored string agrees (`mFogDensity` is really *cloud
  shadow* density).
- **Never rename or "correct" a game symbol.** Gloss it on first use in docs. Our own code stays in
  plain English.

Run `python3 tools/check_japanese_naming.py` after editing `docs/japanese-naming.md`.

**Source-line citations rot.** Docs cite game and aurora source as `file.cpp:LINE`. After any pin
bump run `python3 tools/check_source_citations.py`; its DRIFT guesses are advisory (it once flagged
a correct citation), it does not check the short `:LINE` form, and it accepts any matching name
within ±40 lines, so an OK verdict can still be wrong. Prefer naming functions over line numbers. Verified citations go in
`tools/source_citations_verified.txt` with the pin they were checked against.

## Build model and the pin

- This repo is the official mod template (`TwilitRealm/mod-template`) laid out as a monorepo.
  `cmake/FetchDusklight.cmake` fetches the pinned game + SDK into `dusklight/` (git-ignored,
  depth-1, read-only reference; never edit it). The upstream mod API reference is
  `dusklight/docs/modding.md`; upstream's reference consumer for authored normals is
  `dusklight/mods/ao_mod`.
- **The pin**: `DUSKLIGHT_VERSION` in `CMakeLists.txt` = `v2.0.0` (`e9b12054`) from
  `DUSKLIGHT_REPOSITORY` = upstream `TwilitRealm/dusklight`. GameService 2.0, GfxService 1.3,
  aurora `7d4484a`. Pinning a tag is deliberate: it names a build users can install.
- **Which mods build** is the `add_subdirectory` list in `CMakeLists.txt`: on main vbao, smaa,
  deferred_fog; on this branch reshade-bridge only.
- **Compatibility**: game-linked mods (Deferred Fog, ReShade Bridge) must match the game build (symbol resolution at
  load; GameService major version). Service-only mods need a host whose services are at least the
  minor versions they were built against. Build against the SDK that matches the game.
- **Upstream has moved on**: `v2.0.1`–`v2.0.3` exist; 2.0.3 raises GfxService to 1.4 (texture
  handles appended to `GfxResolvedTargets`) and adds an interpolation service. Moving the pin is a
  re-platform (`CONTRIBUTING.md` "Moving to a newer game build"), not a version bump; do it only
  when asked.
- **The fork is retired.** `automata-rtx/dusklight-ao` existed only to provide authored normals;
  upstream now ships its own (`GfxResolveDesc::normal` → `GfxResolvedTargets::normal`), with a
  different, binary-incompatible shape from the fork's (`get_scene_normals`, a `bool` in tail
  padding). Do not re-add fork knobs (`DUSKLIGHT_SDK_STUB_URL`, `DUSKLIGHT_AURORA_VERSION`) and do
  not describe the fork as current. The lesson from the move: a compile-time compatibility layer
  absorbs changed *declarations*, not changed *behaviour* (the latch).
- **Template tracking**: tracked at mod-template `c79deaa`. Deliberately not taken: `mod.json.in`
  with a shared `project(VERSION)` (our mods version independently) and the `ios-arm64` CI leg (code
  mods cannot run on iOS). Kept from our side: `branches: ["**"]` in `build.yml` (GitHub's `*` does
  not match `/`), the `mods-*` artifact names, and `merge_mod.py --expect-platforms`. The template
  has since added `permissions: contents: write` on the combine job, a newer `SYMGEN_VERSION`, and
  a `v2.0.2` default; none is taken yet.

## Related repos

- `TwilitRealm/dusklight`: upstream and the platform. Its source at the pin is already in
  `dusklight/` (including `mods/ao_mod`); attach it only to read upstream history.
- `automata-rtx/dusklight-ao` and `automata-rtx/aurora-ao`: the retired fork. Historical, except:
  **never delete branch `claude/standalone-final` or the `standalone-final` release** — the
  pre-mod-API build, and the only way these graphics features run on iOS (code mods cannot load
  there). The fork's `docs/thin-gbuffer-normals.md` is still the clearest renderer-side write-up of
  the normal attachment.
