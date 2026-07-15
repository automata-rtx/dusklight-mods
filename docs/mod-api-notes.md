# Mod API notes — pitfalls learned while building these mods

Upstream reference: `extern/dusklight/docs/modding.md` and `extern/dusklight/include/mods/`.
These notes are the deltas that actually bit us.

## Uniform buffers

- Every uniform struct exists twice: a C mirror in `src/mod.cpp` and a WGSL struct in each
  shader that binds it. They must match byte-for-byte, and the total size must be a
  multiple of 16 (`static_assert(sizeof(X) % 16 == 0)` guards this — a 340-byte struct
  once failed exactly there; pad with `float _padN`).
- Avoid `vec3f` in uniform structs (16-byte alignment surprises); pack scalars instead
  (e.g. `light_dir_world[3]` + a pad float).
- All shaders of one mod share one Uniforms struct definition per shader file — when adding
  a field, update EVERY `res/*.wgsl` that declares the struct, not just the one using it.

## Threading

- Stage callbacks (`register_stage_hook`) run on the **game thread** during frame recording.
  This is where you read game state, camera service, config vars, and push work.
- Draw/compute callbacks (`push_draw`, `register_compute_type`) run on the **render worker**
  with the live encoder. Only use the payload (≤128 bytes) and `wgpu*` calls there. Any
  decision that needs game state must be baked into the payload on the game thread.
- Mirror GPU-side choices on the CPU: e.g. the denoiser ping-pongs textures, so the
  composite's input (`passes % 2 ? A : B`) is computed identically in mod.cpp and must stay
  in sync with the compute chain.

## Gfx service

- `resolve_pass` gives single-sample color + R32Float depth snapshots, frame-pooled: views
  are valid this frame only — re-resolve every frame, never cache.
- `create_pass(w, h)` opens an offscreen pass that subsequent GX draws render into (that's
  how the shadow map re-renders the world); `resolve_pass` then yields its depth.
- Check `get_device_info` for `uses_reversed_z` and formats rather than assuming — but note
  the whole codebase currently assumes reversed-Z (1 = near, sky raw depth = 0).
- **Stage order vs the game's post-processing**: the game draws bloom *mid-scene*, between
  `GFX_STAGE_SCENE_AFTER_OPAQUE` and `GFX_STAGE_FRAME_BEFORE_HUD` (see `m_Do_graphic.cpp`).
  A screen-space effect that should sit under bloom (AO, shadows) must push its composite at
  `SCENE_AFTER_OPAQUE`; a draw pushed at `FRAME_BEFORE_HUD` lands on top of bloom (and DOF,
  motion blur, and all translucency). `push_draw` encodes at the push point in the command
  stream, so the stage you push from is the layer you get.

## Camera service

- Matrices are column-major float[16], matrix × column-vector convention — the TRANSPOSE of
  the game's row-major `Mtx`. CPU-side multiplies must use the column-major helper
  (`mat4_mul_col` in enhanced_ao), not game matrix code.
- WebGPU clip conventions: `uv = (ndc.x*0.5+0.5, 0.5 - ndc.y*0.5)`. The single most
  expensive bug of the aurora era was a missed Y flip here (shadows sampled mirrored, which
  a sun-direction negation silently "fixed" — see realtime_sun_shadows.md issue 2).
- `get_camera` returns `MOD_UNAVAILABLE` before the first real in-game frame — handle it by
  skipping the frame.

## Hooks

- Typed hooks (`hook_add_pre<&Class::method>`) resolve through the linked symbol — they work
  without the symbol manifest. By-NAME hooks (`NamedHook`, `resolve`) need the game's symbol
  manifest, which is embedded inside `dusklight.exe` on the `platform-v2-test` base (upstream
  #2216; earlier bases shipped it as a standalone `dusklight.symdb` next to the exe). Prefer
  typed hooks.
- On Windows/MSVC, only functions and `DUSK_GAME_DATA`-annotated data are reachable through
  the import library; un-annotated data references fail at link time.

## Config/UI

- `UI_BINDING_CONFIG_VAR` requires matching types: TOGGLE=bool, NUMBER/SELECT=int. Floats
  aren't bindable — register ints and scale (×0.01 convention throughout these mods).
- Values from config.json apply at `register_var` without firing change callbacks — read
  the value after registration for the starting state.

## Build system

- The SDK (`extern/dusklight/sdk`) provides `add_mod()`, game headers
  (`dusklight_game_headers` INTERFACE target), and Dawn headers via a prebuilt package.
  Nothing from the game compiles in this repo.
- Windows: `DUSK_GAME_EXE` (the `windows-amd64.lib` import library) is REQUIRED for any mod
  (even service-only ones — the SDK refuses to configure without it). It must come from the
  exact game build being targeted.
- `.dusk` = zip of {platform-arch lib, mod.json, res/}. CI's artifact contains every mod's
  package; the loader also picks up a `mods/` dir next to the app for dev builds.

## Validation workflow

- `tools/wgsl_validate.cpp` (Dawn Null backend) catches WGSL errors offline; CI runs it on
  every shader. Build with `-DMODS_BUILD_TOOLS=ON`.
- The Linux CI job compiles every mod with GCC — a full type-check of mod.cpp against the
  game headers without needing Windows.
