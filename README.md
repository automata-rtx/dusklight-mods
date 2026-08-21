# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template):

| Mod | Package | What it does |
|---|---|---|
| VBAO | `vbao.dusk` | Visibility-bitmask ambient occlusion with temporal accumulation, edge-aware denoise, and a large tuning surface. Reads the game's authored surface normals from the graphics service |
| Deferred Fog | `deferred_fog.dusk` | Re-applies the game's fog after screen-space effects, so AO darkens the world *under* the fog instead of darkening the fog itself. Install alongside VBAO |
| SMAA | `smaa.dusk` | Subpixel morphological antialiasing (SMAA 1x). Luma edges unioned with geometric edges from the authored normals + depth |
| Realtime Sun Shadows | `realtime_sun_shadows.dusk` | Real-geometry sun/moon cascaded shadow maps with PCF, slope-scaled bias, contact (screen-space) shadows, and indoor auto-disable |
| SSILVB | `ssilvb.dusk` | Screen-space indirect lighting with visibility bitmask (Therrien et al. 2023): one-bounce colored light gathered through the same 32-sector bitmask VBAO uses; with the bounce disabled it acts as a standalone directional AO. Not currently built — awaiting the normal-service port |
| Effect Remover | `effect_remover.dusk` | Cuts down TP's built-in fake-shading so it doesn't fight new realtime effects. Three independently-toggleable removers: **Projected Shadow Removal** (the "moya" fake ground shade — swaying canopy dapple vs. rolling cloud shadows are per-mode toggles), **Terrain Shadow Removal** (the animated shadow overlay baked into terrain materials, per material code), and **Unbaked Vertex Lighting** (fades the lighting painted into vertex colors, 0 = flat, 100 = vanilla). Experimental |

VBAO, SMAA and SSILVB are **service-only** (mod-API services only, no game code, so they survive
game updates without a rebuild). Deferred Fog, Realtime Sun Shadows and Effect Remover are
**game-linked** (they hook game functions, so they are coupled to the pinned game build).

Surface normals come from the graphics service itself, so no mod provides them for another and the
normal consumers install standalone. **Install Deferred Fog alongside VBAO**: without it the AO
multiplies over already-fogged pixels and distant shading reads as grime on the haze. Running SSILVB
and VBAO together double-darkens unless you disable one mod's AO term (SSILVB has an "Apply AO"
toggle for exactly this).

Each `.dusk` is a **single cross-platform bundle** (Windows x64/arm64, macOS arm64/x64,
Linux x64/arm64, Android arm64) produced by CI.

> **Three mods are built right now: VBAO, Deferred Fog and SMAA.** The graphics service changed how
> mods get surface normals (GfxService 1.3 `get_scene_normals`) and these are the ones ported to it,
> so a test drop is exactly these three rather than a mix of mods at different stages. The rest are
> still in the tree and come back a mod at a time; see the note in `CMakeLists.txt`. Graphics Hub is
> retired — its Depth to Normal half is obsolete now the service provides normals directly, and its
> Deferred Fog half is the standalone mod above.

## Installing

1. Install the matching game build: the `win32-msvc-x86_64` archive from the
   **`platform-normals-test`** release of [`automata-rtx/dusklight-ao`](https://github.com/automata-rtx/dusklight-ao/releases/tag/platform-normals-test).
   The mods are built against that build, not stock upstream Dusklight — see the matched-pair note
   below.
2. Download the latest `mods-combined` artifact from this repo's Actions page.
3. Copy the `.dusk` files into the game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
4. In game: Mods menu → enable them. Settings live in each mod's detail pane.
5. Nothing to enable for surface normals: the platform always carries them and the graphics
   service hands them to any mod that asks. They are unavailable only on the **compatibility
   renderers** (D3D11 / OpenGL ES), where VBAO disables itself and says so in the log — it
   needs a D3D12 / Vulkan / Metal device.

The game-linked mods resolve their hook targets by symbol at load, so a `.dusk` and the game build
it was compiled against are a matched pair. These are built against `automata-rtx/dusklight-ao` at
the commit pinned as `DUSKLIGHT_VERSION` in `CMakeLists.txt` — the scene-normal-buffer platform, not
stock upstream; if a mod fails to load or loads and does nothing, that pin and your game build have
diverged.

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

That's it, on any platform — including Windows (plain MSVC). Two knobs point the stock template at
our platform: `DUSKLIGHT_REPOSITORY` (`automata-rtx/dusklight-ao`, which carries the scene normal
buffer) and `DUSKLIGHT_SDK_STUB_URL` (that fork's `platform-normals-test` release, which publishes
the per-arch link stubs). `DUSKLIGHT_AURORA_VERSION` stays unset — the recorded `extern/aurora` pin
resolves on its own. Note a fork release's stubs are **per-build**, unlike upstream's
version-independent `sdk` tag, so the stub URL moves whenever `DUSKLIGHT_VERSION` does.

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
  Fog mod)
- `docs/depth_to_normal_plan.md`, `docs/depth_to_normal_consumers.md` — the normal-reconstruction
  *(historical — the graphics service now provides normals directly; see `docs/authored_normals.md`)*
- `docs/mod-api-notes.md` — mod-API pitfalls learned the hard way
- Upstream mod API reference: <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>
