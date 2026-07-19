# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template):

| Mod | Package | What it does |
|---|---|---|
| VBAO | `vbao.dusk` | Visibility-bitmask ambient occlusion with temporal accumulation, edge-aware denoise, and a large tuning surface |
| Realtime Sun Shadows | `realtime_sun_shadows.dusk` | Real-geometry sun/moon cascaded shadow maps with PCF, slope-scaled bias, contact (screen-space) shadows, and indoor auto-disable |
| SSILVB | `ssilvb.dusk` | Screen-space indirect lighting with visibility bitmask (Therrien et al. 2023): one-bounce colored light gathered through the same 32-sector bitmask VBAO uses; with the bounce disabled it acts as a standalone directional AO. Requires Graphics Hub |
| [WIP] Graphics Hub | `graphics_hub.dusk` | Hosts the screen-space infrastructure other graphics mods build on, so effects layer correctly with the game's original rendering instead of over-applying. Two independently-toggleable features: **Depth to Normal** (reconstructs a world-space surface normal buffer that AO/GI/shadow mods consume — no settings, keep enabled for SSILVB & Realtime Sun Shadows) and **Deferred Fog** (re-applies the game's fog after screen-space effects so they darken the world under the fog, not the fog itself). Experimental |
| Effect Remover | `effect_remover.dusk` | Cuts down TP's built-in fake-shading so it doesn't fight new realtime effects. Three independently-toggleable removers: **Projected Shadow Removal** (the "moya" fake ground shade — swaying canopy dapple vs. rolling cloud shadows are per-mode toggles), **Terrain Shadow Removal** (the animated shadow overlay baked into terrain materials, per material code), and **Unbaked Vertex Lighting** (fades the lighting painted into vertex colors, 0 = flat, 100 = vanilla). Experimental |

VBAO and SSILVB are **service-only** (mod-API services only, no game code, so they survive game
updates without a rebuild). Realtime Sun Shadows, Graphics Hub, and Effect Remover are
**game-linked** (they hook game functions, so they are coupled to the pinned game build). Graphics
Hub exports the *depth-to-normal* service that Realtime Sun Shadows consumes and SSILVB requires —
keep **Graphics Hub installed and enabled** alongside either. Running SSILVB and VBAO together
double-darkens unless you disable one mod's AO term (SSILVB has an "Apply AO" toggle for exactly
this).

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
- `docs/self_editing_guide.md` — **how to change defaults / hardcode options and build without AI**
- `docs/fake_shading_systems.md` — TP's fake-shading systems (moya, terrain overlay, vertex
  lighting), their in-code names, and which Effect Remover feature handles each
- `docs/vbao.md` — AO algorithm, every tunable, defaults rationale
- `docs/realtime_sun_shadows.md` — shadow architecture, known issues and their fixes, tuning
- `docs/deferred_fog.md` — deferred fog design, mixed-config handling, caveats (now the Deferred
  Fog feature of Graphics Hub)
- `docs/depth_to_normal_plan.md`, `docs/depth_to_normal_consumers.md` — the normal-reconstruction
  provider (now the Depth to Normal feature of Graphics Hub) and how other mods tap its service
- `docs/mod-api-notes.md` — mod-API pitfalls learned the hard way
- Upstream mod API reference: <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>

## iOS note

Code mods cannot run on iOS (dlopen restriction). The pre-mod-API standalone build (release
`standalone-final` on automata-rtx/dusklight-ao) is the only build with these graphics features
on iPhone.
