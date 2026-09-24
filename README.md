# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight) (the Twilight Princess
PC/mobile port), built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template).

Three mods are released and maintained for users:

| Mod | Package | What it does |
|---|---|---|
| **VBAO** | `vbao.dusk` | Visibility-bitmask ambient occlusion — a 32-sector visibility bitmask per hemisphere slice (Therrien et al. 2022), so separated occluders, gaps and thin geometry such as grass are handled correctly where a horizon tracker over-darkens. Temporal accumulation with camera reprojection, a two-candidate history that keeps characters clean without per-object motion vectors, an edge-aware denoiser, depth-aware compositing, and a large tuning surface. Reads the game's own authored surface normals from the graphics service. **Service-only**: no game code, so it survives game updates without a rebuild |
| **Deferred Fog** | `deferred_fog.dusk` | Suppresses the game's per-draw fog during the opaque world and re-applies it, bit-exact, as one fullscreen pass right before the translucent lists — so AO darkens the world *under* the fog instead of darkening the fog itself. **Install alongside VBAO.** Game-linked (hooks game functions), so it is coupled to the pinned game build |
| **SMAA** | `smaa.dusk` | Subpixel morphological antialiasing (SMAA 1x). Luma edges are unioned with geometric edges from the authored normals and depth, which catches silhouettes and creases where luma contrast is weak; the blend-weight pass uses CMAA2-style compute compaction. Composites before bloom and translucency so the game's own post effects operate on antialiased geometry. **Service-only** |

**Install Deferred Fog alongside VBAO**: without it the AO multiplies over already-fogged pixels and
distant shading reads as grime on the haze. Surface normals come from the graphics service itself,
so nothing extra has to be installed for them.

Each `.dusk` is a **single cross-platform bundle** (Windows x64/arm64, macOS arm64/x64,
Linux x64/arm64, Android arm64) produced by CI.

## Installing

1. Install the matching game build: **upstream Dusklight** at the tag pinned as `DUSKLIGHT_VERSION`
   in `CMakeLists.txt` (currently the **`v2.0.0`** release, which is GameService 2.0). See the
   matched-pair note below.
2. Download the latest `mods-combined` artifact from this repo's Actions page, or the `.dusk` files
   from a release.
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

### VBAO 1.1.0 notes

The temporal accumulation was reworked after field reports of flicker in motion. The history now
keeps two candidates per pixel (camera-reprojected, and static at the pixel's own position) scored
on depth and the stored normal, which keeps Link free of trails without per-object motion vectors;
the disocclusion test is relative to depth rather than the far plane; the content reject is a
σ-normalised outlier test; and the motion response fades with view distance (**Motion Response
Range**), so characters stay full and responsive while distant, broad AO keeps its accumulation.
The defaults are the field-validated ones. 1.1.1 adds an experimental, off-by-default **Normal Repair**
toggle for machines that show stray AO on open surfaces at certain angles: where the game's surface
normal contradicts the geometry, the AO uses the geometry's face normal instead. Debug views 5–8 (Geo Normal, Normal Agreement, Raw AO,
Depth MIP 3) exist for reporting a problem: `docs/vbao.md` has the protocol.

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

**Changing a default, hiding an option, or editing a description?**
`docs/editing-options.md` — each is a one-line edit, and it covers the three non-obvious traps
(newlines collapse in mod descriptions, the list view shows two lines, and an option named
`enabled` silently kills the mod).

CI does not validate shaders — it only packages the `.wgsl` files — so validate a shader change
locally before pushing; `tools/wgsl_check.cpp` compiles every shader through Dawn's null backend and
needs no GPU (the recipe is in `CLAUDE.md`). Three checkers guard things a build cannot catch, all
skipping cleanly when the game tree is absent:

```sh
python3 tools/check_reserved_config_names.py   # a config var the host reserves (silent load failure)
python3 tools/check_japanese_naming.py         # game symbols our docs name still exist
python3 tools/check_source_citations.py        # `file.cpp:LINE` citations still point where claimed
```

That's it, on any platform — including Windows (plain MSVC). **No local overrides are needed.**
`DUSKLIGHT_REPOSITORY` is upstream `TwilitRealm/dusklight`, and the SDK downloads its per-arch link
stubs from upstream's own **version-independent** `sdk` release, so there is no stub URL to keep in
sync with the pin.

CI (`.github/workflows/build.yml`) is the template's build + combine pipeline: it builds every mod on
all seven platforms and merges each into one cross-platform `.dusk` via `tools/merge_mod.py`
(artifact `mods-combined`).

## Docs

- `CLAUDE.md` — repo overview, build model, hard constraints, and the platform/ABI pin (read first)
- `docs/vbao.md` — AO algorithm, every tunable, defaults rationale, the temporal accumulation
  design and its field history
- `docs/deferred_fog.md` — deferred fog design, mixed-config handling, status and caveats
- `docs/smaa.md` — the SMAA implementation
- `docs/editing-options.md` and `docs/self_editing_guide.md` — how to change defaults or hardcode
  options and build without AI
- `docs/authored_normals.md` — how the authored normals reach the mods, and what was learned
- `docs/mod-api-notes.md` — mod-API pitfalls learned the hard way
- Upstream mod API reference: <https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md>

## Unreleased mods in the tree

Three more mods live in `mods/` but are **not built or released**; they are on the back burner
until they are ported to this platform and re-verified in-game (see the note in `CMakeLists.txt`):

- **Realtime Sun Shadows** — real-geometry sun/moon cascaded shadow maps (game-linked).
- **SSILVB** — screen-space indirect lighting with the same visibility bitmask VBAO uses.
- **Effect Remover** — cuts down the game's built-in fake shading (haze, terrain shadow overlay,
  baked vertex lighting) so it does not fight realtime effects (game-linked, experimental).
- **Celestial Orbit** — raises the sun/moon travel path for more expressive shadows (game-linked).

Their docs (`docs/realtime_sun_shadows.md`, `docs/ssilvb_plan.md`, `docs/fake_shading_systems.md`,
`docs/celestial_orbit.md`) are kept current for when they return.
