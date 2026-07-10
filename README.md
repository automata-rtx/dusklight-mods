# Dusklight Graphics Mods

Two graphics mods for [Dusklight](https://github.com/TwilitRealm/dusklight), built on its
mod API (gfx service):

| Mod | Package | What it does |
|---|---|---|
| Enhanced Ambient Occlusion (VBAO) | `enhanced_ao.dusk` | Visibility-bitmask AO with temporal accumulation and a large tuning surface |
| Realtime Sun Shadows | `realtime_sun_shadows.dusk` | Real-geometry sun/moon shadow maps with PCF, contact shadows, and indoor auto-disable |

## Installing

1. Install the matching game build once: the **platform-v1** release of
   [automata-rtx/dusklight-ao](https://github.com/automata-rtx/dusklight-ao/releases)
   (Windows: unzip, run `dusklight.exe`).
2. Download the latest `dusklight-mods-win64` artifact from this repo's Actions page
   (or a release, when one exists).
3. Copy both `.dusk` files into `%APPDATA%\TwilitRealm\Dusklight\mods`
   (Linux: `~/.local/share/TwilitRealm/Dusklight/mods`).
4. In game: Mods menu → enable both. Settings live in each mod's detail pane.

After replacing a `.dusk` with a newer build, the in-game **Reload** button picks it up
without restarting.

## Building

```
git clone --recursive <this repo>
cmake -B build -G Ninja        # + -DDUSK_GAME_IMPLIB=<dusklight.lib> on Windows
cmake --build build            # -> build/mods/*.dusk
```

Only the mod sources compile — the game and its renderer are never built here. On Windows,
`dusklight.lib` comes from the platform-v1 release assets. `-DMODS_BUILD_TOOLS=ON` also
builds `tools/wgsl_validate.cpp`, an offline WGSL shader validator.

## Docs

- `docs/vbao.md` — AO algorithm, every tunable, defaults rationale
- `docs/realtime_sun_shadows.md` — shadow architecture, known issues and their fixes, tuning
- `docs/mod-api-notes.md` — mod-API pitfalls learned the hard way
- `extern/dusklight/docs/modding.md` — the upstream mod API reference

## iOS note

Code mods cannot run on iOS. The pre-mod-API standalone build (release `standalone-final`
on automata-rtx/dusklight-ao) is the only build with these graphics features on iPhone.
