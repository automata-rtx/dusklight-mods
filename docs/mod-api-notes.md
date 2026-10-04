# Mod API notes: pitfalls, crashes, debugging

The upstream reference is `dusklight/docs/modding.md` and the headers in
`dusklight/sdk/include/mods/`, both in the tree that the first CMake configure fetches (pinned, so
they match the game build; the GitHub `main` branch may be newer). These notes are the parts that
actually caused problems here. `CONTRIBUTING.md` has the short version.

## Uniform buffers

- Every uniform struct exists twice: a C++ mirror in `src/mod.cpp` and a WGSL struct in each shader
  that binds it. They must match byte for byte, and the size must be a multiple of 16.
  `static_assert`s on size and offsets guard this; pad with `float _padN` fields.
- Avoid `vec3f` in uniform structs (16-byte alignment); pack scalars instead.
- When a mod's shaders share one uniform struct (VBAO's five, SMAA's three), every shader file
  declares it. Adding a field means updating **every** `.wgsl` that declares the struct, not just
  the one that reads the field.

## Threading

- **Stage hooks** (`register_stage_hook`) run on the **game thread** during frame recording. Read
  game state, the camera and config here, and push work.
- **Draw and compute callbacks** (`register_draw_type`, `register_compute_type`) run later on the
  **render worker** with a live encoder. Use only the payload (128 bytes at most) and `wgpu*` calls.
  Anything that depends on game state has to be decided on the game thread and put in the payload.
  The render worker cannot use the log service, so failures there are silent unless the game
  thread can observe them.
- Mirror GPU-side choices on the CPU exactly. VBAO's denoiser ping-pongs two textures, so which one
  the composite reads (`passes % 2`) is computed in `mod.cpp` and must match the compute chain.

## Graphics service

- **`resolve_pass`** returns single-sample snapshots of the current scene pass: colour, depth
  (`R32Float`, reversed-Z) and, if requested via `GfxResolveDesc::normal`, the authored normal
  (`RGB10A2Unorm`, view space). Each call ends the current render pass, copies what was asked for
  at that moment, and continues in a new pass. The views are frame-pooled and valid for this frame
  only: resolve again every frame, never cache them.
- **The normal latches.** The first resolve that asks for the normal enables the attachment from the
  next frame and returns null this frame. Go through `common/gfx_normal_compat.h`.
- **`push_compute`** also splits the scene pass; compute work is recorded between the two halves.
- **`create_pass(w, h)`** opens an offscreen pass that subsequent GX draws render into (Deferred
  Fog's config-ID replay uses it); `resolve_pass` then closes it and returns its targets.
- **Scene-pass pipelines** must take their layout from `GfxDrawContext::layout` and be rebuilt when
  `layout.key` changes. See `CONTRIBUTING.md` "Rules that have bitten before".
- Everything here assumes reversed-Z (1 = near, sky = 0). `GfxDeviceInfo::uses_reversed_z` reports
  it.
- **Stage order versus the game's post effects.** The game draws translucency, particles, depth of
  field and bloom between `GFX_STAGE_SCENE_AFTER_OPAQUE` and `GFX_STAGE_FRAME_BEFORE_HUD` (see
  `m_Do_graphic.cpp`). An effect that should sit under those (AO, AA) composites at
  `SCENE_AFTER_OPAQUE`; a draw pushed at `FRAME_BEFORE_HUD` lands on top of all of them. `push_draw`
  encodes at the point you push it, so the stage you push from is the layer you get.
- Pipelines with automatic layout (`layout` left unset, bind-group layout taken from the pipeline)
  drop bindings their entry point never uses. A bind group that supplies such a binding then fails
  to create.

## Camera service

- Matrices are column-major `float[16]` for column vectors: the transpose of the game's row-major
  `Mtx`. CPU-side multiplies must use a column-major helper (`mat4_mul_col` in VBAO), not the game's
  matrix code.
- WebGPU clip to UV: `uv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)`. A missed Y flip here once
  produced mirrored shadow sampling that a sign change elsewhere hid for a long time.
- `get_camera` returns `MOD_UNAVAILABLE` before the first in-game frame; skip the frame.

## Hooks (game-linked mods)

- `DEFINE_HOOK(&Class::method, Alias)` declares a target from a member-function pointer: the
  compiler checks the signature, and the loader resolves the symbol by name when the mod loads.
  Attach callbacks with `mods::hook::add_pre<Alias>(fn)` / `add_post` from `<mods/svc/hook.hpp>`
  (Deferred Fog passes the service explicitly: `mods::hook::add_pre<Alias>(svc_hook, fn)`). A hook
  is installed only when a callback is first attached, so its failure shows up as the `add_pre` /
  `add_post` result.
- `DEFINE_HOOK_SYMBOL("name", signature, Alias)` hooks by symbol name instead, and needs the game's
  symbol manifest; without one the hook service returns `MOD_UNSUPPORTED`. Prefer `DEFINE_HOOK`.
- On Windows only functions and `DUSK_GAME_DATA`-annotated data are reachable through the import
  library; an un-annotated data reference fails at link time.
- **Changing GX state on top of a display list.** Aurora's GX API keeps its own copy of the
  registers that pack several settings (`genMode`: texture-coordinate, channel, TEV-stage and
  indirect-stage counts and the cull mode; TEV orders, stored in stage pairs; the alpha-combiner
  register, which also holds the swap selection). A setter changes one field of that copy and writes
  the whole register. Display lists (every J3D material) write these registers through the FIFO,
  and the decoder updates the render state but not the API's copy. So after a material has loaded,
  a single `GXSetNumTevStages`, `GXSetNumChans` or `GXSetTevOrder` puts back stale values for the
  register's other fields: a wrong cull mode, or a texture-coordinate count or stage order that no
  longer matches what the stages sample, which aurora rejects as a fatal error (`unhandled tcg src
  21`). Set every field of such a register; Deferred Fog's `set_gen_mode` and
  `stamp_no_fog_through_alpha` show how.

## Config and UI

- **Never name a config var `enabled`.** The loader gives every mod a bool at
  `mod.<escaped id>.enabled` for the mod manager's own on/off checkbox, created before any mod
  initializes. `register_var` puts a mod's own vars in the same namespace, so `"enabled"` collides
  and registration returns `MOD_CONFLICT`. If that is the mod's first registration, the mod fails
  to initialize and never loads. Nothing at build time catches it, and the log line names the
  mod's own option, so it reads like a mod bug. (Celestial Orbit shipped that way and never loaded.)
  Prefix the name (`orbitEnabled`); the UI label can still say "Enabled".

  The two toggles are different anyway: the manager's checkbox unloads the mod, while a mod's own
  toggle keeps it loaded (and any service it exports available) and just stops it acting.

  `python3 tools/check_reserved_config_names.py` scans for this, deriving the reserved list from
  the fetched game tree, but it only recognises names written as `cvarDesc.name = "..."` or
  `register_bool_option("...")`. Names in VBAO's and SMAA's option tables are not checked.
- `UI_BINDING_CONFIG_VAR` needs matching types: TOGGLE = bool, NUMBER and SELECT = int. Floats are
  not bindable, so fractional options are stored as ints and scaled on read (×0.01 for most, ×0.001
  for some VBAO options, plain world units for distances).
- Values from `config.json` are applied at `register_var` without firing change callbacks. Read the
  value after registering it for the starting state.
- **A mod's own error string can hide the host's reason.** `mods::set_error(error, MOD_ERROR, ...)`
  replaces the service's result with a generic `MOD_ERROR`, and the loader prints *that* code next
  to *your* message. When a service call fails unexpectedly, log the actual `ModResult` or read the
  host's implementation before theorising.

## Build system

- The SDK in the fetched `dusklight/sdk` provides `add_mod()`, game headers for `FEATURES game`
  mods, and WebGPU headers via a prebuilt Dawn package for `FEATURES webgpu`. Nothing from the game
  compiles in this repo.
- On Windows, macOS and Android a mod with `FEATURES game` or `webgpu` links against a per-arch stub
  of the game executable, which the SDK downloads automatically (`DUSKLIGHT_SDK_STUB_URL`, default
  in `dusklight/cmake/ModSDK.cmake`; set `DUSK_GAME_EXE` to use a real game binary instead). Linux
  needs no stub: game symbols resolve at load (`-Wl,--allow-shlib-undefined`).
- Windows builds use plain MSVC (`cl`). Build `RelWithDebInfo`: a `Release` link strips the hook
  records the loader scans for.
- A `.dusk` is a zip of `lib/<platform>/mod.{so,dll}`, `mod.json` and `res/`. CI builds one per
  platform and `tools/merge_mod.py` merges them into one cross-platform bundle.

## Validation

- CI compiles every mod on all seven platforms. The Linux jobs are a quick full type-check of
  `mod.cpp` against the game headers.
- CI does **not** validate WGSL. `tools/wgsl_check.cpp` does, offline and without a GPU; see
  `CONTRIBUTING.md` "Check your change" for the build command. Run it after every shader edit.

## Symbolizing a game crash

A Dusklight crash dump gives module-relative addresses and no symbols. Resolve them; do not try to
infer the faulting code from the mod's source. The upstream Windows build ships its PDB:

1. From the upstream release (or CI run) that matches `DUSKLIGHT_VERSION`, get the Windows artifact
   (`dusklight-<version>-win32-msvc-<arch>`). It contains the `.exe` and `debug.7z`.
2. Extract `dusklight.pdb` from `debug.7z` (`7z x`, or `py7zr` in Python), next to the `.exe`.
3. The image base is `0x140000000`, so the address is base + RVA:

   ```sh
   llvm-symbolizer --obj=dusklight.exe --functions=linkage --demangle --inlines 0x1403c2828
   ```

`--inlines` matters: the outermost entry is the real function and the inner ones are the inlined
accessor chain that actually faulted. That is how one start-up crash was pinned to
`dKy_Indoor_check -> dStage_stagInfo_GetSTType -> BE<u32>::swap` in one step, after two rounds of
wrong guesses from reading mod source.

- Symbolize every frame, not just the crash address. The game-side frames name the stage dispatch
  (`dusk::mods::gfx_run_stage`) and the game loop, which tells you which of your callbacks was
  running.
- The `.exe` is stripped, so the PDB is required.
- Mod-side frames need the matching `mod.dll` from the CI per-platform artifact.
- A fault address under about `0x100` is a null dereference at that struct offset; match it to the
  field offset in the header to identify the object.

### Game state that does not exist yet on the boot screens

Stage hooks also fire on 2D screens, from the very first frame. Several game accessors are
unguarded there, and the game never notices because nothing of its own asks that early:

- `dKy_Indoor_check()` → `dStage_stagInfo_GetSTType(getStagInfo())` dereferences the stage info
  without a null check, and `dComIfGp_getStage()->getStagInfo()` is null until a stage loads.
- `dKy_getEnvlight()` returns null there.

`draw_lists_ready()` (in Deferred Fog) is not a general "a scene exists" test: the draw lists are
populated on the logo screen while the stage info is still null. Check each piece of game state for
availability on its own.

## Debugging lessons

These are from investigations that cost days; `docs/authored_normals.md` §8 has the full case
studies.

- **A debug view is only trustworthy if it samples the same resource the effect does, under the same
  conditions.** Three views in this repo showed one thing while the effect used another, and each
  sent an investigation the wrong way.
- **A view that shows a combined result cannot tell you which input failed.** Build the view that
  separates the terms before theorising.
- **Check a fix's premise before shipping it.** If it can be checked in the source or a binary, check
  it: one `grep` disproved a whole theory, but only after a fix for it had shipped.
- **Read the tester's observation literally.** "The affected area changes with camera position and
  aim" separated two theories at once; it was available before the wrong fix was written.
- **A host-side "do I need to bind X" test that duplicates a shader-side "do I use X" test will
  drift silently** when the shader has a fallback. Prefer one source of truth: the host sets a
  "this is bound" uniform flag that the shader trusts.
- **CI takes a few minutes for all seven platforms**, while a round trip through in-game testing
  takes much longer. When unsure, ship the build that adds the diagnostic, not another guess.
