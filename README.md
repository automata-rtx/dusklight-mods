# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template):

| Mod | Package | What it does |
|---|---|---|
| VBAO | `vbao.dusk` | Visibility-bitmask ambient occlusion with temporal accumulation, edge-aware denoise, and a large tuning surface. Reads the game's authored surface normals from the graphics service |
| Deferred Fog | `deferred_fog.dusk` | Re-applies the game's fog after screen-space effects, so AO darkens the world *under* the fog instead of darkening the fog itself. Install alongside VBAO. Not currently built — awaiting hook re-verification against the new game build |
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

> **Two mods are built right now: VBAO and SMAA.** The platform moved to **upstream Dusklight 2.0**,
> which supplies surface normals through GfxService 1.3's resolve pair (`GfxResolveDesc::normal` →
> `GfxResolvedTargets::normal`, resolved alongside depth); these two are ported to it. A test drop is
> exactly these rather than a mix of mods at different stages. The rest are still in the tree and come
> back a mod at a time — the four game-linked mods need their hook symbols re-verified against the new
> game build first, and SSILVB and Realtime Sun Shadows need the same normal-API port. See the note in
> `CMakeLists.txt`. Graphics Hub is retired — its Depth to Normal half is obsolete now the service
> provides normals directly, and its Deferred Fog half is the standalone mod above.

## Installing

1. Install the matching game build: **upstream Dusklight** at the commit pinned as
   `DUSKLIGHT_VERSION` in `CMakeLists.txt` (currently `c83ce89`, which is GameService 2.0). Our
   fork is retired — these are built against stock upstream now. See the matched-pair note below.
2. Download the latest `mods-combined` artifact from this repo's Actions page.
3. Copy the `.dusk` files into the game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
4. In game: Mods menu → enable them. Settings live in each mod's detail pane.
5. Nothing to enable for surface normals: the graphics service creates the normal buffer the first
   time a mod asks for it (one frame later — the first request always comes back empty, which is
   normal and not reported). Two things prevent it entirely:
   - **MSAA.** The renderer will not create the normal buffer unless antialiasing is off. If VBAO
     says AO is disabled and names MSAA, that is the fix — set antialiasing to none.
   - **The compatibility renderers** (D3D11 / OpenGL ES), which cannot carry the extra buffer at
     all. VBAO disables itself and says so in the log; it needs a D3D12 / Vulkan / Metal device.
     SMAA keeps working either way, on luma edges only.

A `.dusk` and the game build it was compiled against are a matched pair, for two reasons: the
game-linked mods resolve their hook targets **by symbol at load**, and the host refuses any mod built
against an older **game service major version** outright — which this pin bumped to 2.0. If a mod
fails to load, or loads and does nothing, that pin and your game build have diverged.

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

That's it, on any platform — including Windows (plain MSVC). **No local overrides are needed.**
`DUSKLIGHT_REPOSITORY` is upstream `TwilitRealm/dusklight`, and the SDK downloads its per-arch link
stubs from upstream's own **version-independent** `sdk` release, so there is no stub URL to keep in
sync with the pin. (The fork-era `DUSKLIGHT_SDK_STUB_URL` and `DUSKLIGHT_AURORA_VERSION` knobs are
gone; if a future base is ever a fork again, remember a fork release's stubs *are* per-build and the
URL has to move with `DUSKLIGHT_VERSION`.)

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
