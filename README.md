# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template):

| Mod | Package | What it does |
|---|---|---|
| VBAO | `vbao.dusk` | Visibility-bitmask ambient occlusion with temporal accumulation, edge-aware denoise, and a large tuning surface |
| Realtime Sun Shadows | `realtime_sun_shadows.dusk` | Real-geometry sun/moon cascaded shadow maps with PCF, slope-scaled bias, contact (screen-space) shadows, and indoor auto-disable |
| Deferred Fog | `deferred_fog.dusk` | Re-applies the game's fog as a fullscreen pass after AO/shadows composite, so they darken surfaces *under* the fog instead of the fog itself |
| Depth to Normal | `depth_to_normal.dusk` | Reconstructs a per-pixel world-space surface normal from the depth buffer and publishes it as a service other mods consume. No settings of its own |
| SSILVB | `ssilvb.dusk` | Screen-space indirect lighting with visibility bitmask (Therrien et al. 2023): one-bounce colored light gathered through the same 32-sector bitmask VBAO uses; with the bounce disabled it acts as a standalone directional AO. Requires Depth to Normal |

VBAO, Depth to Normal, and SSILVB are **service-only** (mod-API services only, no game code, so they
survive game updates without a rebuild). Realtime Sun Shadows and Deferred Fog are **game-linked**
(they hook game functions, so they are coupled to the pinned game build). Realtime Sun Shadows
consumes the Depth to Normal service and SSILVB requires it — install Depth to Normal alongside
either. Running SSILVB and VBAO together double-darkens unless you disable one mod's AO term
(SSILVB has an "Apply AO" toggle for exactly this).

Each `.dusk` is a **single cross-platform bundle** (Windows x64/arm64, macOS arm64/x64,
Linux x64/arm64, Android arm64) produced by CI.

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

This repo is the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template),
laid out as a monorepo. The Dusklight SDK is **fetched automatically** (pinned by `DUSKLIGHT_VERSION`
in `CMakeLists.txt`) and the SDK **auto-downloads** the per-arch link stub it needs — no submodule,
no `--recursive`, no manual link libraries, no compiler override. Only the mod sources compile.

```sh
git clone <this repo>
cmake -B build          # fetches the SDK + link stub on first run
cmake --build build     # -> build/mods/*.dusk
```

That's it, on any platform — including Windows (plain MSVC) — because the base game is the template's
own base (`76b56cd8`). The only fork-specific configuration is two cache variables in `CMakeLists.txt`
that point the template at our platform: `DUSKLIGHT_REPOSITORY` (our `dusklight-ao` fork, for the
enlarged aurora buffers) and `DUSKLIGHT_SDK_STUB_URL` (our public `platform-v2-test` release).

CI (`.github/workflows/build.yml`) is the template's build + combine pipeline: it builds every mod on
all seven platforms and merges each into one cross-platform `.dusk` via `tools/merge_mod.py`
(artifact `mods-combined`).

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
