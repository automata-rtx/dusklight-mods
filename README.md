# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template):

| Mod | Package | What it does |
|---|---|---|
| VBAO | `vbao.dusk` | Visibility-bitmask ambient occlusion with temporal accumulation, edge-aware denoise, and a large tuning surface |
| Realtime Sun Shadows | `realtime_sun_shadows.dusk` | Real-geometry sun/moon cascaded shadow maps with PCF, slope-scaled bias, contact (screen-space) shadows, and indoor auto-disable |
| Deferred Fog | `deferred_fog.dusk` | Re-applies the game's fog as a fullscreen pass after AO/shadows composite, so they darken surfaces *under* the fog instead of the fog itself |
| Depth to Normal | `depth_to_normal.dusk` | Reconstructs a per-pixel world-space surface normal from the depth buffer and publishes it as a service other mods consume. No settings of its own |

VBAO and Depth to Normal are **service-only** (mod-API services only, no game code, so they survive
game updates without a rebuild). Realtime Sun Shadows and Deferred Fog are **game-linked** (they hook
game functions, so they are coupled to the pinned game build). Realtime Sun Shadows also consumes the
Depth to Normal service — install both together.

Each `.dusk` is a **single cross-platform bundle** (Windows x64, macOS arm64/x64, Linux x64/arm64,
Android arm64) produced by CI. (Windows-on-ARM is not built yet.)

## Installing

1. Install the matching game build once: the **`platform-v2-test`** release of
   [automata-rtx/dusklight-ao](https://github.com/automata-rtx/dusklight-ao/releases) (pick the zip
   for your OS/arch; on Windows unzip `dusklight-*-win32-msvc-x86_64.zip` and run `dusklight.exe`).
   The `.dusk` files must match the game build they were built against.
2. Download the latest `mods-combined` artifact from this repo's Actions page.
3. Copy the `.dusk` files into the game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
4. In game: Mods menu → enable them. Settings live in each mod's detail pane.

After replacing a `.dusk` with a newer build, the in-game **Reload** button picks it up without
restarting.

## Building

The Dusklight SDK is **fetched automatically** (pinned by `DUSKLIGHT_VERSION` in `CMakeLists.txt`) —
no submodule, no `--recursive`. Only the mod sources compile; the game and its renderer are never
built here.

```sh
git clone <this repo>
# Linux — builds out of the box (game symbols resolve at load):
cmake -B build -G Ninja -DMODS_BUILD_TOOLS=ON
cmake --build build                        # -> build/mods/*.dusk
```

On **Windows/macOS/Android**, a game-linked mod additionally needs a per-arch link target via
`-DDUSK_GAME_EXE=<path>` — the `sdk/` stub inside the matching `platform-v2-test` game zip
(`sdk/windows-amd64.lib`, `sdk/stub-macos-arm64`, `sdk/stub-android-aarch64.so`, …). CI fetches these
automatically; locally, extract the one you need. **Windows also requires clang-cl**:

```sh
cmake -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
  -DDUSK_GAME_EXE=<path to sdk/windows-amd64.lib>
cmake --build build
```

Plain MSVC (`cl`) mangles the mod SDK's `modmeta`/`DEFINE_HOOK` records so hooks never register —
clang-cl is mandatory on this platform, and CI's `hook-repro` job guards against regressing it.
`-DMODS_BUILD_TOOLS=ON` also builds `tools/wgsl_validate.cpp`, an offline WGSL shader validator.

CI (`.github/workflows/build.yml`) builds every mod on all six platforms and merges each into one
cross-platform `.dusk` via `tools/merge_mod.py` (artifact `mods-combined`).

## Docs

- `CLAUDE.md` — repo overview, build model, hard constraints, and the platform/ABI pin (read first)
- `docs/vbao.md` — AO algorithm, every tunable, defaults rationale
- `docs/realtime_sun_shadows.md` — shadow architecture, known issues and their fixes, tuning
- `docs/deferred_fog.md` — deferred fog design, mixed-config handling, caveats
- `docs/depth_to_normal_plan.md`, `docs/depth_to_normal_consumers.md` — the normal-reconstruction
  provider and how other mods tap it
- `docs/mod-api-notes.md` — mod-API pitfalls learned the hard way
- Upstream mod API reference: <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>

## iOS note

Code mods cannot run on iOS (dlopen restriction). The pre-mod-API standalone build (release
`standalone-final` on automata-rtx/dusklight-ao) is the only build with these graphics features
on iPhone.
