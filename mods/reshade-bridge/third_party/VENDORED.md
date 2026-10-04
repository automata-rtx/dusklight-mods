# Vendored headers (ReShade Bridge add-on)

Only the add-on (`addon/`) uses these; the Dusklight mod (`src/`) does not.

| Directory | Source | Version | License | Changes |
| :-- | :-- | :-- | :-- | :-- |
| `reshade/include/` | https://github.com/crosire/reshade `include/` | tag `v6.8.0` (`18deaa52`) | BSD-3-Clause OR MIT (`reshade/LICENSE.md`) | none |
| `imgui/` (`imgui.h`, `imconfig.h`) | https://github.com/ocornut/imgui | 1.92.5 (`3912b3d9`) | MIT (`imgui/LICENSE.txt`) | none |

The two versions are coupled. `reshade_overlay.hpp` accepts exactly one Dear ImGui version
(`IMGUI_VERSION_NUM` 19250 for ReShade 6.8.0) and the add-on reaches ImGui through ReShade's
function table, so both must move together: take `imgui.h` and `imconfig.h` from the ImGui tag that
the new ReShade's `reshade_overlay.hpp` names. `RESHADE_API_VERSION` (20 in 6.8.0) is the minimum
ReShade the add-on loads in; newer ReShade versions load older add-ons.
