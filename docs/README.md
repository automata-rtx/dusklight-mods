# Documentation index

New here? Read `CONTRIBUTING.md` at the repo root first. It covers building, testing, the mod API as
these mods use it, and the rules that have caused silent failures.

## Current: the released mods and shared infrastructure

| Doc | What it covers | Read it when |
| :-- | :-- | :-- |
| `vbao.md` | VBAO: per-frame pipeline, every option, debug views, the temporal design and its field history | working on AO |
| `deferred_fog.md` | Deferred Fog: how fog is suppressed and re-applied, hooks, options, debug views, the open Death Mountain problem, Wolf Senses | working on fog |
| `deferred_fog_underwater_notes.md` | A designed but **unbuilt** Deferred Fog feature: fading AO on submerged terrain | picking that feature up |
| `smaa.md` | SMAA: pipeline, options, debug views, scope and limits | working on antialiasing |
| `reshade_port.md` | ReShade Port (first iteration, untested in-game): running ReShade `.fx` effects at chosen points of the frame; the WGSL compiler, validation device, depth conversion, limitations, offline checkers | working on the ReShade port or testing it |
| `editing-options.md` | Changing a default, hiding an option, editing a mod description, adding an icon | making a small tuning change |
| `self_editing_guide.md` | The same tasks done entirely in the GitHub web editor, no local build | you have no build environment |
| `mod-api-notes.md` | Mod API pitfalls, crash symbolization, debugging lessons | before touching uniforms, threads or render code; when something crashes |
| `authored_normals.md` | How the game's authored normals reach the mods, and the long investigation behind it | working on anything that reads normals |
| `normal_buffer_portability.md` | Which devices and SDKs carry the normal buffer, and how the mods degrade without it | a mod reports no normals |
| `japanese-naming.md` | Reading the game's Japanese identifiers; searching the game tree correctly | reading or hooking game code |

## Not current

| Folder | Status |
| :-- | :-- |
| `unreleased/` | Docs for the four mods that are not built or released (SSILVB, Realtime Sun Shadows, Celestial Orbit, Effect Remover). **Possibly out of date.** Start at `unreleased/README.md`. |
| `historical/` | Retired designs (the Depth to Normal provider). Kept for their reasoning. |

## Elsewhere

- `../CONTRIBUTING.md`: contributor guide.
- `../README.md`: user-facing overview and install steps.
- `../CLAUDE.md`: instructions for AI coding sessions. It overlaps heavily with the docs above. If it
  disagrees with them, fix whichever is wrong.
- `../dusklight/docs/modding.md` (after the first CMake configure): the upstream mod API reference.
