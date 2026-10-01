# Contributing

This is the starting point for programmers working on the three released mods: **VBAO**,
**Deferred Fog** and **SMAA**. It covers building, testing, how a mod in this repo is put together,
and the rules that have caused silent failures before. Each mod then has its own reference doc in
`docs/`.

Four more mods live in `mods/` but are not built or released. Their documentation is in
`docs/unreleased/`, which is explicitly marked as possibly out of date. Leave them alone unless you
are porting one back (see `docs/unreleased/README.md`).

## Contents

- [What is in the repo](#what-is-in-the-repo)
- [Prerequisites](#prerequisites)
- [Build](#build)
- [Check your change](#check-your-change)
- [Test in-game](#test-in-game)
- [How a mod here works](#how-a-mod-here-works)
- [Where each mod lands in the frame](#where-each-mod-lands-in-the-frame)
- [Rules that have bitten before](#rules-that-have-bitten-before)
- [Where to start on each mod](#where-to-start-on-each-mod)
- [Releasing](#releasing)
- [Moving to a newer game build](#moving-to-a-newer-game-build)
- [Documentation map](#documentation-map)

## What is in the repo

```
CMakeLists.txt          top level: pins the game build, lists which mods build
cmake/FetchDusklight.cmake  fetches the pinned game + SDK source into ./dusklight (git-ignored)
common/                 headers shared by the mods (scene-pass layout, normal-buffer access)
mods/vbao/              ambient occlusion            (service-only)
mods/deferred_fog/      fog re-applied after effects (game-linked)
mods/smaa/              post-process antialiasing    (service-only)
mods/<others>/          unreleased, not built        (see docs/unreleased/)
tools/                  shader validator, bundle merger, doc checkers
docs/                   per-mod references and shared notes
.github/workflows/build.yml  CI: builds every mod on seven platforms, merges the bundles
```

Every mod has the same shape:

```
mods/<name>/
  CMakeLists.txt   add_mod(...) call: FEATURES, sources, mod.json, res dir
  mod.json         id, display name, version, description, icon/banner paths
  src/mod.cpp      all host code: config vars, UI, pipelines, stage hooks, game hooks
  res/*.wgsl       shaders, loaded at runtime through the resource service
  res/licenses/    third-party attribution shipped inside the bundle
```

Two kinds of mod:

- **Service-only** (VBAO, SMAA). `FEATURES webgpu` only. They use the host's services (graphics,
  camera, config, UI, resource, log) and never include game headers or call game code. They keep
  working across game updates without a rebuild, as long as the services they use keep their
  shape. Prefer this for any new screen-space effect.
- **Game-linked** (Deferred Fog). `FEATURES game webgpu`. They include game headers, call game
  functions and hook them. Hook targets are resolved **by symbol name when the mod loads**, so a
  game-linked mod is tied to the exact game build it was compiled against.

`dusklight/` is a read-only, pinned copy of the game's decompiled source, the mod SDK
(`dusklight/sdk/include/mods/`) and the renderer, aurora (`dusklight/extern/aurora`). Nothing in it
is compiled here. It is there for headers and so you can read the game and renderer code your mod
interacts with. Never edit it.

## Prerequisites

- CMake 3.26 or newer and a C++20 compiler:
  - Linux: GCC or Clang.
  - Windows: Visual Studio's MSVC (`cl`). CI uses the Ninja generator from a VS developer shell.
    No clang-cl needed.
  - macOS: Xcode command-line tools.
- Python 3, for the tools in `tools/`.
- Network access on the first configure. It fetches the pinned game source from
  `github.com/TwilitRealm/dusklight` (with the `extern/aurora` submodule), a prebuilt Dawn package for
  WebGPU headers, and on Windows/macOS/Android a link stub for the game executable.

You do **not** need the game installed to build. You do need it to test.

## Build

```sh
cmake -B build                 # first run fetches ./dusklight and the SDK dependencies
cmake --build build            # every released mod -> build/mods/<name>.dusk
cmake --build build --target vbao_package   # one mod (deferred_fog_package, smaa_package)
```

The build type defaults to `RelWithDebInfo` and should stay that way. On Windows a `Release` link
strips the hook records the loader scans for, so a game-linked mod loads and silently does nothing.
`CMakeLists.txt` sets the default before `project()` so multi-config generators get it too.

A `.dusk` file is a zip: `mod.json`, `res/`, and `lib/<platform>/mod.{so,dll,dylib}`. A local build
contains only your own platform. CI builds all seven (Linux x86_64/aarch64, macOS arm64/x86_64,
Windows amd64/arm64, Android aarch64) and `tools/merge_mod.py` merges them into one cross-platform
bundle per mod.

## Check your change

The compiler checks the C++. Nothing in the build or in CI checks the shaders, the game hook
targets or the docs, so these are on you.

**Shaders.** CI only copies the `.wgsl` files into the bundle. A WGSL error builds fine, ships, and
first appears in-game as a pipeline that fails to create, which looks like the effect being off.
Validate locally after any shader edit. `tools/wgsl_check.cpp` compiles each file through Dawn's
null backend and needs no GPU:

```sh
cmake -B build && cmake --build build      # also fetches the prebuilt Dawn the validator links
D=build/_deps/dawn_prebuilt-src
g++ -std=c++20 -I$D/include tools/wgsl_check.cpp $D/lib/libwebgpu_dawn.a -ldl -lpthread -lX11 \
    -o build/wgsl_check
./build/wgsl_check mods/vbao/res/*.wgsl mods/smaa/res/*.wgsl mods/deferred_fog/res/*.wgsl
```

**Repo checkers.** Each skips cleanly if `./dusklight` has not been fetched.

```sh
python3 tools/check_reserved_config_names.py  # a config var name the host reserves (mod fails to load)
python3 tools/check_source_citations.py       # `file.cpp:LINE` citations in docs/comments still point at what they claim
python3 tools/check_japanese_naming.py        # game symbols named in docs/japanese-naming.md still exist
```

**In-game.** For a game-linked change, a clean compile proves very little. `DEFINE_HOOK` checks the
function signature at compile time, but the symbol is looked up by name at load. Run it.

## Test in-game

1. Install the game build that matches the pin: upstream Dusklight at the tag in
   `DUSKLIGHT_VERSION` (top-level `CMakeLists.txt`, currently `v2.0.0`). A mismatched game and mod
   build can fail to load outright.
2. Copy the `.dusk` into the user mods folder, or start the game with `--mods <dir>` to point it at
   a folder of your choosing (for example `build/mods`):
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
3. In the game's Mods menu, enable the mod. After replacing a `.dusk`, the mod manager's **Reload**
   button picks up the new build without a restart.
4. Each mod's options are in its pane in the Mods menu; the larger option sets open in a separate
   controls window. Option values are saved in the game's `config.json`.
5. Log output (`svc_log->info/warn/error`) goes to the game's console, prefixed with the mod id.
   Every mod here logs its state changes, and Deferred Fog also shows a live **Status** line in its
   pane.

Each mod has **debug views** (a "Debug View" selector in its controls) that show intermediate
results: VBAO's raw AO, normals and depth; Deferred Fog's fog factor and per-pixel fog config;
SMAA's edges and blend weights. Use them before guessing. The per-mod docs list what each view
shows and what a broken one means.

Things that change what you see and are easy to forget:

- **Saved settings beat new defaults.** Changing a default in code does nothing for an option you
  already moved. Reset it in the UI, or test with a clean `config.json`.
- **MSAA disables the normal buffer.** The renderer only creates the authored-normal attachment
  with antialiasing off. VBAO then disables itself (and says so); SMAA falls back to luma-only edges.
- **The compatibility renderers** (D3D11, OpenGL ES) cannot carry the normal attachment at all.
  VBAO needs D3D12, Vulkan or Metal.
- **Wolf Senses.** Deferred Fog deliberately does nothing while Wolf Link's senses are active.

## How a mod here works

The upstream mod API reference is `dusklight/docs/modding.md` in the fetched tree, and the headers
are in `dusklight/sdk/include/mods/`. This section covers the parts these mods use.

### Lifecycle and services

`mod.cpp` imports the services it needs at file scope (`IMPORT_SERVICE(GfxService, svc_gfx)` and so
on); the loader resolves them before calling `mod_initialize`. `mod_initialize` loads shaders,
registers config vars, builds the UI, registers stage hooks and (for game-linked mods) installs
hooks. `mod_shutdown` releases everything the mod created. A mod may be unloaded and reloaded at
runtime, so shutdown has to leave nothing behind.

### Options

Options are config vars: `svc_config->register_var` with a name, a type (bool or int) and a default.
The host stores them as `mod.<id>.<name>` in `config.json`. UI controls bind to them with
`UI_BINDING_CONFIG_VAR`. Floats are not bindable, so every fractional option here is an int scaled
on read (most commonly ×0.01). `docs/editing-options.md` shows where each mod's defaults are.

### Graphics: stage hooks, compute and draw callbacks

Rendering goes through the graphics service (`dusklight/sdk/include/mods/svc/gfx.h`). Three kinds of
callback, on two threads:

- **Stage hooks** (`register_stage_hook`) run on the **game thread** at fixed points in the frame
  (next section). This is where a mod reads config and game/camera state, takes snapshots
  (`resolve_pass`), uploads per-frame data (`push_uniform`, `push_storage`) and queues GPU work
  (`push_compute`, `push_draw`).
- **Compute and draw callbacks** (`register_compute_type`, `register_draw_type`) run later on the
  **render worker thread**, with a WebGPU encoder and the small payload (at most 128 bytes) queued
  with them. Only raw `wgpu*` calls and the payload are allowed there. Anything that depends on game
  state or config has to be decided on the game thread and put in the payload.
- **`resolve_pass`** snapshots the current scene pass: colour, depth (`R32Float`, reversed-Z) and,
  if requested, the authored normal. The returned texture views are valid for **this frame only**.
  Ask again every frame and never cache them.

A draw pushed from a stage hook is encoded at that point in the frame, so the stage you push from is
the layer your output lands on.

### Uniforms

Each uniform struct exists twice: once in C++ in `mod.cpp`, once in WGSL. The two must match byte
for byte, and the size must be a multiple of 16. `static_assert`s on size and field offsets guard
this. Keep them true and do not delete them. Avoid `vec3f` in uniform structs; pack scalars instead.
If a struct is declared in several `.wgsl` files, update all of them.

### Game hooks (Deferred Fog)

`DEFINE_HOOK(&Class::method, Name)` declares a hook target; `mods::hook_add_pre<Name>` and
`hook_add_post<Name>` attach callbacks at init. A pre-hook can read and rewrite arguments
(`mods::arg`, `mods::arg_ref`). Hook callbacks run on the game thread, synchronously with the game.

The game's identifiers are the original Japanese team's names, preserved by the decompilation
(`kankyo` = environment, `moya` = haze, `kumo` = cloud). Read them with `docs/japanese-naming.md` to
hand: reading them as English has produced wrong conclusions here before. Struct *member* names were
invented by the decompilers and are guesses; function names are the original team's.

## Where each mod lands in the frame

The world camera's part of a frame, in `dusklight/src/m_Do/m_Do_graphic.cpp` (line numbers at the
current pin):

| Point | Line | What happens | Used by |
| :-- | :-- | :-- | :-- |
| sky lists | 2328 | `dComIfGd_drawOpaListSky` / `XluListSky` | |
| `GFX_STAGE_SCENE_BEGIN` | 2334 | before any world geometry | Deferred Fog opens its fog-suppression scope |
| opaque world lists | 2344+ | terrain, objects, actors; fog applied per draw by the game | Deferred Fog suppresses per-draw fog here |
| `GFX_STAGE_SCENE_AFTER_TERRAIN` | 2366 | after terrain and shadows, before the main opaque list | (nothing in the released set) |
| `GFX_STAGE_SCENE_AFTER_OPAQUE` | 2395 | all opaque world geometry is down | VBAO composites; SMAA antialiases; Deferred Fog closes its scope and arms the fog quad |
| translucent lists | 2405 | `dComIfGd_drawXluListBG` onward | Deferred Fog draws its fog quad at the first translucent J3D shape |
| particles, DOF, bloom | ... 2632 | the game's own post effects | Deferred Fog's fallback anchor is just before bloom |
| `GFX_STAGE_FRAME_BEFORE_HUD` | 2759 | after all 3D post effects | Deferred Fog's last-resort anchor |
| `GFX_STAGE_FRAME_AFTER_HUD` | 2820 | the last stage in the frame | VBAO's debug views (so nothing draws over them) |

Consequences:

- VBAO and SMAA composite at `SCENE_AFTER_OPAQUE`, before the game's translucency, bloom and depth
  of field, so those effects work on the already-occluded, already-antialiased image.
- Deferred Fog applies the fog after that, so AO darkens the surface under the fog rather than
  darkening the fog colour. That ordering comes from the stages, not from the mods knowing about
  each other. No mod in the build imports another.
- Within one stage, hooks run in registration order, which follows load order. There is no priority
  field.

## Rules that have bitten before

Each of these has caused a silent failure in this repo: the build was green and the effect simply
did not appear.

- **Scene-pass pipelines must be built from the live layout and rebuilt when it changes.** When any
  mod asks for authored normals, the scene pass gains a second colour attachment, from the *next*
  frame. A render pipeline built for the old shape is rejected by WebGPU and the draw silently
  disappears. Build scene-pass pipelines lazily in the draw callback from `GfxDrawContext::layout`,
  and rebuild them when `layout.key` changes, using `gfx_compat::scene_pass_layout_for_draw()` and
  `scene_pass_layout_key()` from `common/gfx_scene_pass.h`. Copy `ensure_composite_pipelines()`
  (VBAO), `ensure_neighborhood_pipeline()` (SMAA) or `ensure_fog_pipelines()` (Deferred Fog).
  Building them once in `mod_initialize` is a bug. Offscreen passes from `create_pass` stay
  single-target.
- **The normal snapshot latches.** The first `resolve_pass` that asks for the normal turns the
  attachment on for the next frame and returns null for this one. A null normal for a few frames
  after start-up is not "this device has no normals". VBAO and SMAA both wait
  `kNormalLatchGraceFrames` before reporting anything.
- **Reach the normal fields through `common/gfx_normal_compat.h`** (`gfx_compat::request_normal`,
  `resolved_normal`), not directly. It compiles to "no normals" on an SDK that lacks them.
- **Never name a config var `enabled`.** The host reserves `mod.<id>.enabled` for the mod manager's
  own checkbox, so registration fails with `MOD_CONFLICT` and the whole mod fails to load. Prefix it
  (`effectEnabled`, `fogEnabled`). `tools/check_reserved_config_names.py` catches this.
- **Thread rules.** Game state, config and camera are read on the game thread only. Draw and compute
  callbacks use their payload and `wgpu*` calls, nothing else.
- **Everything is reversed-Z**: depth 1 is near, 0 is far, and sky pixels have raw depth 0.
- **Do not trust a green build after the pin moves.** Read the new SDK header. Upstream once renamed
  the scene-layout API, the old `#if` guard went false, and every pipeline quietly fell back to the
  wrong shape. `gfx_scene_pass.h` now refuses to compile against an SDK it does not recognise.
- **A debug view must sample the same resource, under the same conditions, as the effect.** Several
  investigations here were sent the wrong way by a view that showed something the effect never
  used.

`docs/mod-api-notes.md` has the full list of API pitfalls, plus how to symbolize a game crash.

## Where to start on each mod

| Mod | Read first | Then | Key entry points in `src/mod.cpp` |
| :-- | :-- | :-- | :-- |
| VBAO | `docs/vbao.md` "Pipeline" | `res/vbao.wgsl` (the estimator), `res/temporal.wgsl` | the `SCENE_AFTER_OPAQUE` stage hook, `ensure_composite_pipelines()`, the option tables in `mod_initialize` |
| Deferred Fog | `docs/deferred_fog.md` "How it works" | `src/fog_math.h`, `res/fog.wgsl` | `on_scene_begin`, `on_shape_draw_pre`, `on_set_fog_pre`, `on_scene_after_opaque`, `push_fog_quad` |
| SMAA | `docs/smaa.md` "Pipeline" | `res/edge_detection.wgsl`, `res/blend_weights.wgsl` | the `SCENE_AFTER_OPAQUE` stage hook, `ensure_neighborhood_pipeline()` |

Open problems worth knowing about before you start:

- **Deferred Fog: distant landmarks (Death Mountain, the Ganon barrier) are brighter with the mod
  off.** Three fixes have failed. `docs/deferred_fog.md` "Known issues" has the evidence so far and
  what to measure next.
- **SMAA** handles orthogonal edge patterns only; diagonal search and corner rounding are not
  implemented.

## Releasing

1. Bump `version` in the mod's `mod.json`. Each mod is versioned independently; there is no shared
   version.
2. Push. CI builds every mod on seven platforms; the `mods-combined` artifact holds one
   cross-platform `.dusk` per mod, and `mods-<platform>` holds the per-platform bundles. CI runs on
   every branch.
3. Pushing a tag additionally creates a GitHub release with the combined `.dusk` files attached.

`mod.json` notes: `description` is plain text and newlines collapse to spaces in the mod manager,
which shows roughly two lines in its list. See `docs/editing-options.md`.

## Moving to a newer game build

The game build is pinned by `DUSKLIGHT_VERSION` in `CMakeLists.txt`. To move:

1. Bump `DUSKLIGHT_VERSION` and reconfigure.
2. Run `git diff --stat <old> <new>` over the game tree. If it touches no `sdk/` file and none of the
   files Deferred Fog hooks, the move is probably a no-op.
3. Read `dusklight/sdk/include/mods/svc/gfx.h` and the other service headers you use. A green build
   does not prove the API kept its shape.
4. Re-verify Deferred Fog's hook targets: take every `DEFINE_HOOK` line in
   `mods/deferred_fog/src/mod.cpp` and confirm each symbol still exists in the new tree.
5. Run `python3 tools/check_source_citations.py` and fix the line numbers it reports as drifted.
   Its guesses are advisory; confirm each one by reading the source.
6. Test in-game, with the matching game build installed.

Mods built against an older SDK can be refused by a newer game: the host rejects structs smaller
than its own, and a game-service major version bump refuses every older mod. Rebuild against the
pin that matches the game.

## Documentation map

| Doc | For |
| :-- | :-- |
| `README.md` | Users: what the mods do, how to install them |
| `CONTRIBUTING.md` | This file |
| `docs/README.md` | Index of every doc, with what each is for and whether it is current |
| `docs/vbao.md`, `docs/deferred_fog.md`, `docs/smaa.md` | Per-mod reference: pipeline, options, debug views, known issues, history |
| `docs/editing-options.md` | Changing a default, hiding an option, editing a description |
| `docs/mod-api-notes.md` | Mod API pitfalls, crash symbolization, debugging lessons |
| `docs/authored_normals.md` | How the game's authored normals reach the mods |
| `docs/japanese-naming.md` | Reading the game's Japanese identifiers |
| `docs/unreleased/` | The four unreleased mods. Possibly outdated |
| `docs/historical/` | Retired designs |
| `CLAUDE.md` | Instructions for AI coding sessions. Mostly duplicates the above in denser form |
