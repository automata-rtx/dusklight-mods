# Historical documents

These describe designs that no longer exist in the build. Keep them for the reasoning they record,
not as instructions.

| File | What it was |
| :-- | :-- |
| `depth_to_normal_plan.md` | The plan for the "Depth to Normal" provider: a mod that reconstructed a world-space normal from depth and published it to other mods as the `dev.automata.depth_to_normal` service. It shipped inside the combined "Graphics Hub" mod. Both are retired. |
| `depth_to_normal_consumers.md` | The menu of screen-space effects that could consume that provider, with integration boilerplate. The effect ideas still apply; the integration code does not. |

**What replaced them.** The host's graphics service (GfxService 1.3, Dusklight `v2.0.0`) hands every
mod the game's own *authored* view-space normal. A mod asks for it with `GfxResolveDesc::normal` and
reads `GfxResolvedTargets::normal` from `resolve_pass`. In this repo, go through
`common/gfx_normal_compat.h` rather than touching those fields directly. VBAO and SMAA are the
working examples, and upstream's `mods/ao_mod` (in the fetched `dusklight/` tree) is the reference
consumer. `docs/authored_normals.md` explains how the authored normals reach the mods.
