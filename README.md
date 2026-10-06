# Dusklight Graphics Mods

Graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight), the Twilight Princess
PC/mobile port. Built on the official [Dusklight mod template](https://github.com/TwilitRealm/mod-template).

| Mod | Package | What it does |
| :-- | :-- | :-- |
| **VBAO** | `vbao.dusk` | Ambient occlusion using a per-slice visibility bitmask (Therrien et al. 2023), so gaps between occluders and thin geometry such as grass do not over-darken the way horizon-based AO does. Temporal accumulation, half-resolution upsampling, an edge-aware denoiser, and a large set of options. Uses the game's own surface normals. Works across game updates without a rebuild |
| **Deferred Fog** | `deferred_fog.dusk` | Removes the game's fog from the opaque world while it draws and re-applies the same fog afterwards in one pass. AO then darkens the world *under* the fog instead of darkening the fog itself. Install it alongside VBAO. Hooks game code, so it must match the game build |
| **SMAA** | `smaa.dusk` | Post-process antialiasing (SMAA 1x). Detects edges from brightness and from the game's surface normals and depth, so it also catches silhouettes and creases with little brightness contrast. Runs before the game's bloom and translucency. Works across game updates without a rebuild |

**On this branch** (`claude/reshade-bridge-*`) CI builds only **ReShade Bridge**: `reshade_bridge.dusk`
plus a ReShade add-on, `dusklight_reshade_bridge.addon64`, which together run an installed ReShade's
effects inside the game's frame (under the HUD, before the game's own post-processing, with the
game's depth), at any internal resolution. Windows, Direct3D 12 and ReShade with full add-on
support only. Setup: `docs/reshade_bridge.md`. The mods in the table are built on main.

Each `.dusk` is a single cross-platform bundle for Windows (x64, arm64), macOS (arm64, x64), Linux
(x64, arm64) and Android (arm64).

## Installing

1. Install **Dusklight `v2.0.0`**, the game build these mods are built against (the pin is
   `DUSKLIGHT_VERSION` in `CMakeLists.txt`).
2. Get the `.dusk` files: from a published release, or as the `mods-combined` artifact of the
   latest successful run on this repo's **Actions** page (unzip it).
3. Copy the `.dusk` files into the game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
4. In the game, open the Mods menu and enable them. Each mod's settings are in its pane, with a
   button that opens the full set of controls.

After replacing a `.dusk` with a newer build, the mod manager's **Reload** button picks it up
without restarting the game.

**Requirements.** VBAO needs the game's surface-normal buffer, which only the D3D12, Vulkan and
Metal renderers provide. On the compatibility renderers (D3D11, OpenGL ES) VBAO turns itself off and
says so in the log; SMAA keeps working using brightness edges only. Nothing needs to be enabled for
the normals; the first frame or two after start-up simply run without them.

**Matching the game build.** Deferred Fog hooks game functions by name when it loads, so it must be
built for the exact game version you run; a mismatch can stop it loading. VBAO and SMAA only use
the mod API, so they keep working on newer game builds that still provide the same API.

## Known issues

- **Deferred Fog: some distant landmarks look brighter with the mod off** (Death Mountain, the
  Hyrule Castle barrier). Not yet fixed; see `docs/deferred_fog.md`.
- Deferred Fog deliberately does nothing while Wolf Link's senses are active and leaves that view
  to the game. This is intended and has no visible cost.

## For developers

- `CONTRIBUTING.md`: building, testing in-game, how the mods are structured, and the rules that
  have caused silent failures. Start here.
- `docs/README.md`: index of all documentation.
- `docs/editing-options.md`: changing a default or hiding an option without learning the codebase.

Four more mods live in `mods/` but are **not built or released**: Realtime Sun Shadows, SSILVB,
Effect Remover and Celestial Orbit. Their documentation is in `docs/unreleased/` and may be out of
date.

## License

GPL-3.0; see `LICENSE`. Third-party notices for adapted code ship inside each mod under
`res/licenses/`.
