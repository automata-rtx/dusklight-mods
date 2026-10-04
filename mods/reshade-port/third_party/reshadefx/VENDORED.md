# Vendored: ReShade FX compiler

ReShade's own effect compiler (preprocessor, lexer, parser, symbol table, the code-generator
interface), unmodified except for the patches below. The mod adds its own WGSL code generator
(`src/codegen_wgsl.cpp`) behind the same `reshadefx::codegen` interface that ReShade's HLSL, GLSL
and SPIR-V generators implement; those generators are not vendored.

- Upstream: https://github.com/crosire/reshade, `source/effect_*`
- Version: tag `v6.8.0`, commit `18deaa52de0c425a78b329e9cb3c497281cd00ec`
- License: BSD-3-Clause, `LICENSE.md` (copied from upstream's `LICENSE.md`; the binary notice is
  `res/licenses/RESHADE-BSD-3.txt`)

## Patches (all marked `dusklight-mods patch` in the source)

| File | Change | Why |
| :-- | :-- | :-- |
| `effect_lexer.cpp` | On Apple, parse float literals with `std::strtod` instead of `std::from_chars` | `from_chars(float)` needs macOS 13.3 |
| `effect_preprocessor.cpp` | `#include <share.h>` on Windows | `SH_DENYWR` is used there without the include |
| `effect_preprocessor.cpp` | `normalize_include_path`: `\` to `/` in `#include` and `__has_include` paths, except on Windows | effects written on Windows use backslashes; elsewhere they are part of the file name |
| `effect_symbol_table.cpp` | `<alloca.h>` instead of `<malloc.h>` except on Windows | macOS has no `<malloc.h>`; `alloca` lives in `<alloca.h>` on every non-Windows platform |

## Updating

Copy `source/effect_*.{hpp,cpp,inl}` (except `effect_codegen_hlsl.cpp`, `effect_codegen_glsl.cpp`,
`effect_codegen_spirv.cpp`) from the new tag, re-apply the patches, and update the version above.
Then check `src/codegen_wgsl.cpp` against any change to `effect_codegen.hpp` (new virtual
functions, changed intrinsic list in `effect_symbol_table_intrinsics.inl`), and run both offline
checkers over an effect corpus (docs/reshade_port.md, "Offline checkers").
