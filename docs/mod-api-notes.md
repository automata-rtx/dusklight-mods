# Mod API notes — pitfalls learned while building these mods

Upstream reference: the fetched `dusklight/docs/modding.md` and `dusklight/sdk/include/mods/`
(fetched by `cmake/FetchDusklight.cmake`; also on GitHub at
`TwilitRealm/dusklight/blob/main/docs/modding.md`). These notes are the deltas that actually bit us.

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
  (`mat4_mul_col` in vbao), not game matrix code.
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

- The SDK (in the fetched `dusklight/sdk`) provides `add_mod()`, game headers
  (`dusklight_game_headers` INTERFACE target), and Dawn headers via a prebuilt package. The tree is
  fetched by `cmake/FetchDusklight.cmake` (pinned by `DUSKLIGHT_VERSION`) — nothing from the game
  compiles in this repo.
- On **Windows/macOS/Android**, a mod using `FEATURES game|webgpu` (all of ours) links against a
  per-arch stub the SDK **auto-downloads** from `DUSKLIGHT_SDK_STUB_URL` (import library on Windows,
  `bundle_loader`/`.so` stub on macOS/Android) — no manual `DUSK_GAME_EXE` needed (set it to override
  the download). **Linux needs nothing** — game symbols resolve at load (`-Wl,--allow-shlib-undefined`).
- Windows builds with plain MSVC (`cl`); no clang-cl override. The base game's `modmeta` parser
  tolerates linker padding, so `DEFINE_HOOK` records register under `cl`.
- `.dusk` = zip of {`lib/<platform>/mod.{dll,so}`, mod.json, res/}. CI builds one per platform and
  `tools/merge_mod.py` merges them into a single cross-platform bundle (the `mods-combined`
  artifact); the loader also picks up a `mods/` dir next to the app for dev builds.

## Validation workflow

- CI (the template's build + combine) compiles every mod on all seven platforms; the Linux legs are
  a fast full type-check of mod.cpp against the game headers. Shaders are validated by the game at
  pipeline-creation time — there is no separate offline WGSL validator (it was dropped in the move to
  the pure template; `git log` for `tools/wgsl_validate.cpp` if you want to reinstate it).
