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
  mod.json         id, display name, version, description, optional icon/banner paths
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

- `git`, CMake 3.26 or newer, and a C++20 compiler:
  - Linux: GCC or Clang.
  - Windows: Visual Studio's MSVC (`cl`), plus Ninja. No clang-cl needed.
  - macOS: Xcode command-line tools.
- Python 3.9 or newer for the tools in `tools/`, and `rg` (ripgrep) for
  `check_japanese_naming.py`.
- On Linux, the libX11 development package if you want to build the shader validator.
- Network access on the first configure, roughly 250 MB. It fetches the pinned game source from
  `github.com/TwilitRealm/dusklight` (with its `extern/aurora` submodule from `encounter/aurora`)
  and a prebuilt Dawn package for the WebGPU headers. On Windows, macOS and Android it also
  downloads a link stub for the game executable; Linux resolves game symbols when the mod loads and
  needs no stub.

You do **not** need the game installed to build. You do need it to test.

## Build

```sh
cmake -B build                 # first run fetches ./dusklight and the SDK dependencies
cmake --build build            # every released mod -> build/mods/<name>.dusk
cmake --build build --target vbao_package   # one mod (deferred_fog_package, smaa_package)
```

Build `RelWithDebInfo`. On Windows a `Release` link strips the hook records the game's loader scans
for, so a game-linked mod loads and silently does nothing; Debug is not what CI ships either.

- **Linux and macOS**: the commands above build `RelWithDebInfo` by default. For an Intel Mac build
  on Apple silicon add `-DCMAKE_OSX_ARCHITECTURES=x86_64` (universal binaries are not supported).
- **Windows**: configure from a Visual Studio *Developer* prompt with Ninja, as CI does:
  `cmake -B build -G Ninja`. The default Visual Studio generator is multi-config and builds Debug
  unless you pass `cmake --build build --config RelWithDebInfo`.
- **Android**: see the `android-aarch64` job in `.github/workflows/build.yml` for the NDK version
  and toolchain arguments.

A `.dusk` file is a zip: `mod.json`, `res/`, and `lib/<platform>/mod.so` (`mod.dll` on Windows).
A local build contains only your own platform. CI builds all seven (Linux x86_64/aarch64, macOS
arm64/x86_64, Windows amd64/arm64, Android aarch64) and `tools/merge_mod.py` merges them into one
cross-platform bundle per mod.

## Check your change

The compiler checks the C++. Nothing in the build or in CI checks the shaders, the game hook
targets or the docs, so these are on you.

**Shaders.** CI only copies the `.wgsl` files into the bundle. A WGSL error builds fine, ships, and
first appears in-game as a pipeline that fails to create, which looks like the effect being off.
Validate locally after any shader edit. `tools/wgsl_check.cpp` compiles each file through Dawn's
null backend and needs no GPU. The recipe below is for Linux; it links the prebuilt Dawn that the
configure step already fetched:

```sh
cmake -B build                             # fetches the prebuilt Dawn, among other things
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

Their limits: `check_reserved_config_names.py` only sees names written as `cvarDesc.name = "..."`
or `register_bool_option("...")`, so it does **not** check the option tables VBAO and SMAA use
(check those by eye). `check_source_citations.py` only checks citations written as `file.ext:LINE`,
not the short `:LINE` form, and its "drift" guesses are advisory.

**In-game.** For a game-linked change, a clean compile proves very little. `DEFINE_HOOK` checks the
function signature at compile time, but the symbol is looked up by name at load. Run it.

## Test in-game

1. Install the game build that matches the pin: upstream Dusklight at the tag in
   `DUSKLIGHT_VERSION` (top-level `CMakeLists.txt`, currently `v2.0.0`). A mismatched game and mod
   build can fail to load outright.
2. Copy the `.dusk` into the user mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

   Alternatively, start the game with `--mods <dir>` (for example `--mods build/mods`) to use a
   folder of your choosing instead of the user folder. A `mods/` folder next to the game executable
   is also searched.
3. In the game's Mods menu, enable the mod. After replacing a `.dusk`, the mod manager's **Reload**
   button picks up the new build without a restart.
4. Each mod's options are in its pane in the Mods menu; the larger option sets open in a separate
   controls window. Option values are saved in the game's `config.json`.
5. Log output (`svc_log->info/warn/error`) goes to the game's console, prefixed with the mod id.
   Every mod here logs its state changes. Deferred Fog's diagnostics (a live **Status** line and its
   detailed log lines) are compiled out of the released UI; see below.

Each mod has **debug views** (a "Debug View" selector in its controls) that show intermediate
results: VBAO's raw AO, normals and depth; Deferred Fog's fog factor and per-pixel fog config;
SMAA's edges and blend weights. Deferred Fog's are hidden in release builds: set `kShowDiagnostics`
to `true` in `mods/deferred_fog/src/mod.cpp` to get them back. Use them before guessing. The per-mod
docs list what each view shows and what a broken one means.

Things that change what you see and are easy to forget:

- **Saved settings beat new defaults.** Changing a default in code does nothing for an option you
  already moved. Reset it in the UI, or test with a clean `config.json`.
- **The compatibility renderers** (D3D11, OpenGL ES) cannot carry the authored-normal attachment.
  VBAO then disables itself and says so in the log (it needs D3D12, Vulkan or Metal); SMAA falls
  back to luma-only edges. The renderer would also refuse the attachment with MSAA on, but the
  current game build never enables MSAA, so that case cannot occur today.
- **Wolf Senses.** Deferred Fog deliberately does nothing while Wolf Link's senses are active
  (unless its Enable Exceptions option is off).

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

`DEFINE_HOOK(&Class::method, Name)` declares a hook target; `mods::hook::add_pre<Name>` and
`mods::hook::add_post<Name>` from `<mods/svc/hook.hpp>` attach callbacks at init. A pre-hook can
read and rewrite arguments (`mods::arg`, `mods::arg_ref`). Hook callbacks run on the game thread,
synchronously with the game. Deferred Fog requires every one of its hooks: if any fails to attach,
it stays inactive and the game draws its own fog (`install_hooks`).

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
| `GFX_STAGE_SCENE_BEGIN` | 2334 | before any world geometry | Deferred Fog snapshots the sky's depth and opens its fog-suppression scope |
| opaque world lists | 2344–2390 | terrain, objects, actors, grass; also some particles (`Pri0_B`) and the game's own shadows. Fog applied per draw by the game | Deferred Fog suppresses per-draw fog here, and holds back see-through J3D layers |
| `GFX_STAGE_SCENE_AFTER_TERRAIN` | 2366 | after terrain and shadows, before the main opaque list | (nothing in the released set) |
| `GFX_STAGE_SCENE_AFTER_OPAQUE` | 2395 | all opaque world geometry is down | VBAO composites; SMAA antialiases; Deferred Fog closes its scope and arms the fog pass |
| translucent lists | 2405 | `dComIfGd_drawXluListBG` onward | Deferred Fog draws its fog pass from a pre-hook on `dComIfGd_drawXluListBG`, then the see-through layers it held back |
| particles, depth of field, framebuffer copies, 2D-screen filters, bloom | to 2632 | the game's own post effects. They read or redraw the frame; bloom (2632) works from the last framebuffer copy | (they all see the fogged frame) |
| `GFX_STAGE_FRAME_BEFORE_HUD` | 2759 | after all 3D post effects | (nothing in the released set) |
| `GFX_STAGE_FRAME_AFTER_HUD` | 2820 | the last stage in the frame | VBAO's debug views (so nothing draws over them) |

Consequences:

- VBAO and SMAA composite at `SCENE_AFTER_OPAQUE`, before the game's translucency, bloom and depth
  of field, so those effects work on the already-occluded, already-antialiased image.
- Deferred Fog applies the fog after that and before everything the game draws or copies later, so
  AO darkens the surface under the fog rather than darkening the fog colour, and the game's own
  post effects see the fogged image. That ordering comes from the frame, not from the mods knowing
  about each other. No mod in the build imports another.
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
  (`effectEnabled`, `fogEnabled`). `tools/check_reserved_config_names.py` catches this, except
  in VBAO's and SMAA's option tables.
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
| VBAO | `docs/vbao.md` "How it works" | `res/vbao.wgsl` (the estimator), `res/temporal.wgsl` | the `SCENE_AFTER_OPAQUE` stage hook, `ensure_composite_pipelines()`, the option tables in `mod_initialize` |
| Deferred Fog | `docs/deferred_fog.md` "How it works" | `src/fog_math.h`, `res/fog.wgsl` | `on_scene_begin`, `on_shape_draw_pre`, `on_set_fog_pre`, `on_scene_after_opaque`, `on_xlu_list_bg_pre`, `push_fog_quad`, `install_hooks` |
| SMAA | `docs/smaa.md` "How it works" | `res/edge_detection.wgsl`, `res/blend_weights.wgsl` | the `SCENE_AFTER_OPAQUE` stage hook, `ensure_neighborhood_pipeline()` |

Open problems worth knowing about before you start:

- **Deferred Fog** is a single fullscreen pass over the finished opaque image, so a few cases cannot
  match the game exactly (one depth per pixel, additive blends, materials with fog off).
  `docs/deferred_fog.md` "Limitations" lists them, and "Diagnosing a difference from vanilla" says
  how to tell them apart in a view.
- **SMAA** handles orthogonal edge patterns only: edges at or near 45° get little or no smoothing,
  and diagonal search and corner rounding are not implemented.
- **VBAO** has a short list of minor known issues in `docs/vbao.md` "Known issues".

## Releasing

1. Bump `version` in the mod's `mod.json`. Each mod is versioned independently; there is no shared
   version. The game parses it as `MAJOR.MINOR.PATCH`, optionally followed by `-prerelease` and/or
   `+build` (for example `2.0.0-a`). Anything else, such as `2.0.0a`, makes the mod manager refuse
   to install the package, and a parseable older copy of the same mod wins over it.
2. Push. CI builds every mod on seven platforms; the `mods-combined` artifact holds one
   cross-platform `.dusk` per mod, and `mods-<platform>` holds the per-platform bundles. CI runs on
   every branch and takes a few minutes.
3. Pushing a tag additionally runs a step that creates a GitHub release with the combined `.dusk`
   files attached. That step has never been exercised: the repo has no GitHub releases yet, and the
   combine job does not request `contents: write` permission (the upstream template now does), so
   check it the first time.

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
   `mods/deferred_fog/src/mod.cpp` and confirm each symbol still exists in the new tree. Also
   confirm that `dComIfGd_drawXluListBG` is still an out-of-line function and that `mDoGph_Painter`
   still calls it directly after the `SCENE_AFTER_OPAQUE` stage: the fog pass is drawn from it.
5. Run `python3 tools/check_source_citations.py` and fix the line numbers it reports as drifted.
   Its guesses are advisory; confirm each one by reading the source.
6. Test in-game, with the matching game build installed.

Compatibility between a mod build and a game build:

- **Game-linked mods** (Deferred Fog) must match the game build: hooks resolve by symbol name at
  load, and the host refuses a mod built against an older GameService major version.
- **Service-only mods** (VBAO, SMAA) are looser. `IMPORT_SERVICE` asks for the minor version of each
  service in the SDK they were built with, so they need a game at least that new. Some service calls
  also check struct sizes, so build against the SDK that matches the oldest game you support.

The pin is `v2.0.0`. Upstream has since tagged newer 2.0.x releases. 2.0.3 raises GfxService to 1.4
(texture handles appended to `GfxResolvedTargets`) and adds an interpolation service, so moving the
pin needs the steps above rather than just a version bump.

## Documentation map

| Doc | For |
| :-- | :-- |
| `README.md` | Users: what the mods do, how to install them |
| `CONTRIBUTING.md` | This file |
| `docs/README.md` | Index of every doc, with what each is for and whether it is current |
| `docs/vbao.md`, `docs/deferred_fog.md`, `docs/smaa.md` | Per-mod reference: pipeline, options, debug views, limitations and known issues |
| `docs/editing-options.md` | Changing a default, hiding an option, editing a description |
| `docs/mod-api-notes.md` | Mod API pitfalls, crash symbolization, debugging lessons |
| `docs/normal_buffer_portability.md` | The normal-buffer API, when normals are missing, the two `common/` headers |
| `docs/authored_normals.md` | How the authored normals came to be, and the lessons from it (mostly history) |
| `docs/japanese-naming.md` | Reading the game's Japanese identifiers |
| `docs/unreleased/` | The four unreleased mods. Possibly outdated |
| `docs/historical/` | Retired designs |
| `CLAUDE.md` | Short instructions for AI coding sessions; points back to these docs |
