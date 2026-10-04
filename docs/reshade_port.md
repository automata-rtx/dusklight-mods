# ReShade Port

Runs ReShade effect files (`.fx`) inside the game's frame instead of on the finished image. Each
technique runs at an insertion point you choose: before the world's transparent surfaces, before
the game's particles and post-processing, before the HUD, or after it (where standalone ReShade
runs). Effects read the game's real depth buffer, already converted to what ReShade's depth
helpers expect, so no depth settings are needed.

Status: **first iteration (0.1.0), never run in-game yet.** It compiles for all seven platforms and
passes the offline checkers below; nothing has been seen on screen. Treat the first in-game test as
a bring-up.

| | |
| :-- | :-- |
| Kind | game-linked (one hook) + GfxService + CameraService + UiService + HostService |
| Source | `mods/reshade-port/` |
| Package | `reshade_port.dusk`, mod id `dev.automata.reshade_port` |
| Config vars | `effectsActive` (bool), `depthRange` (int, game units, 0 = camera far plane) |
| Settings file | `ReShadePreset.ini` in the mod's data folder (ReShade's format) |

## Using it

1. Find the mod's data folder: the game's config folder (the folder that holds `mods/`), then
   `mod_data/dev.automata.reshade_port/`. For example on Windows
   `%APPDATA%\TwilitRealm\Dusklight\mod_data\dev.automata.reshade_port\`. The mod creates it on
   first start and shows the exact path in **Open ReShade Port > Settings > Folders**.
2. Put effects in `reshade-shaders/Shaders/` and their textures in `reshade-shaders/Textures/`
   inside it, the way ReShade's installer lays them out (one subfolder per package is fine; both
   folders are searched recursively, and `#include` finds headers such as `ReShade.fxh` in any of
   them).
3. In the Mods menu, press **Open ReShade Port** (or **Reload effects** after adding files).
   - **Techniques**: every technique, in the order they run. Select one, then use **Enabled**,
     **Insertion point**, **Move up** / **Move down**.
   - **Parameters**: one entry per effect; the effect's uniforms (sliders become a value box with
     -/+ buttons, colours a colour picker, combos a dropdown), its compiler output, and its own
     preprocessor definitions.
   - **Settings**: run effects on/off, global preprocessor definitions, depth range, status,
     folders and the list of effects with errors.

Settings save to `ReShadePreset.ini` about a second after a change. A preset written by ReShade can
be dropped in: technique order, enabled techniques, uniform values and preprocessor definitions are
read as ReShade reads them. The insertion points are kept in an extra `[DUSKLIGHT]` section that
ReShade ignores.

## Insertion points

| Point | Where in the frame (`m_Do_graphic.cpp`, `mDoGph_Painter`) | What is drawn after it |
| :-- | :-- | :-- |
| Before transparency | `GFX_STAGE_SCENE_AFTER_OPAQUE` (after `dComIfGd_drawOpaListPacket`) | translucent world lists, particles, all post-processing, HUD |
| Before particles & post-processing (default) | post-hook on `dComIfGd_drawXluListDark` | `motionBlure`, `drawDepth2` (depth of field), particles, `dComIfGd_drawIndScreen`, bloom, letterbox, fade, HUD |
| Before HUD | `GFX_STAGE_FRAME_BEFORE_HUD` | the 2D lists (HUD, menus) |
| After HUD | `GFX_STAGE_FRAME_AFTER_HUD` | nothing: the finished frame |

The game's names are its own: `motionBlure` is the blur filter's original spelling, and "Dark" in
`dComIfGd_drawXluListDark` is one of the game's draw-list classes (`japanese-naming.md`, "The
draw-list taxonomy"); `dComIfGd_drawXluListDark` is simply the last translucent world list the
frame draws, and `DUSK_NOINLINE` in the PC build, so it can be hooked.

At each point that has at least one enabled, built technique, `run_point` (`src/mod.cpp`):

1. `resolve_pass(color, depth)` snapshots the scene colour and raw depth and continues the pass.
2. `Runtime::prepare` builds a plan: the techniques at this point, in list order, plus each
   effect's uniform bytes for this frame.
3. A compute task (`Runtime::execute`) records into the frame encoder: copy the colour snapshot to
   the effects' back buffer, convert depth (below), then every technique exactly as ReShade's
   runtime records it.
4. A draw (`on_draw`) writes the back buffer into the live scene pass, RGB only, so the game's own
   alpha (which later game passes may read) is untouched.

Each point that runs costs one colour+depth snapshot, one depth conversion and one fullscreen
composite on top of the effects themselves. Points with nothing enabled cost nothing.

## How it works

### Compiling

`third_party/reshadefx` is ReShade's own compiler front end (v6.8.0, `VENDORED.md`). It drives a
code generator; ReShade ships HLSL, GLSL and SPIR-V generators, and this mod adds a fourth,
`src/codegen_wgsl.cpp`, because the game's device accepts only WGSL (Aurora disables SPIR-V input).
Each entry point becomes its own WGSL module:

- `@group(0) @binding(0)`: the effect's uniform block, laid out exactly as ReShade's GLSL back end
  lays it out (arrays and matrix rows on 16-byte strides), so preset values upload unchanged.
- `@group(1)`: each sampler as a texture at `2b` and a sampler at `2b + 1`.
- `@group(2)`: storage textures.

The generator follows D3D semantics where WGSL differs: matrices are transposed (HLSL `floatRxC`
becomes WGSL `matRxC` and `mul(a, b)` becomes `b * a`), out-of-range fetches return zero,
`tex2Dgather` with four offsets fetches each texel, and functions that take samplers or storage
textures are specialised per call site (WGSL cannot pass them as parameters).

### Formats the game's device lacks

Aurora requests no optional WebGPU features, so some ReShade texture formats cannot be used as
ReShade uses them. `src/fx_formats.hpp` stores each in a format that holds the same values, and the
generated code makes up the difference:

- 16-bit unorm textures are stored as 32-bit float (values are not rounded to 16 bits).
- R8, RG8, R16F, RG16F and RGB10A2 textures used as storage are widened to a storage-capable format;
  reads are swizzled back to what the narrow format returns.
- RG11B10F is stored as RGBA16F.
- 32-bit float and integer textures cannot use filtering samplers, so the generator filters them
  itself (bilinear, trilinear with derivatives, border colour, LOD bias) with `textureLoad`.
- A 32-bit float texture that a pass blends into is stored as 16-bit float (the device cannot blend
  32-bit float); the effect still runs with less precision, and the compiler output says so.
- Textures with the same name in different effects are shared, as in ReShade, and their usage is
  merged over all effects before code generation so every effect agrees on the storage format.

### Depth

ReShade effects read depth through `ReShade.fxh`, configured by the `RESHADE_DEPTH_*` definitions.
The mod fixes those definitions (reversed, not upside down, not logarithmic, far plane 1000) and
converts the game's raw reversed-Z depth into exactly the encoding they expect
(`kUtilityWgsl` `fs_depth` in `src/fx_gpu.cpp`): with linear depth `L = (z - near) / (range - near)`
for view distance `z`, it writes `1 - L * F / (1 + L * (F - 1))`, `F = 1000`, which
`ReShade::GetLinearizedDepth` maps back to `L`. `z` comes from `CameraInfo::proj_from_view` of the
frame's camera (captured at `GFX_STAGE_SCENE_AFTER_OPAQUE`). `range` is the camera's far plane, or
**Depth range** when set; a smaller range spreads `0..1` over nearby scenery, as lowering
`RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` does in ReShade. Users cannot override the fixed
definitions (the first definition of a name wins and these go first).

### Building and validation

Compiling runs on the runtime's own build thread (`Runtime::Builder`): every `.fx` file is parsed
in parallel, texture usage is merged, then WGSL is generated. GPU objects are built only for effects
with an enabled technique, and only after the effect has been **validated on a private Dawn
null-backend device**: Aurora's release build turns Dawn's validation off (`skip_validation`) and
treats any WebGPU error it does see as fatal, so an effect the device cannot run must never reach
the game's device. The validation device is created on its own instance with the game device's
feature level and limits (never looser), without `allow_unsafe_apis`, which also makes Dawn lower
each pipeline's shaders to Tint IR as the D3D12, Vulkan and Metal back ends do. Each technique is
built and recorded there; one that fails is reported and skipped, the rest of the effect runs. Only
then is the effect built on the game's device, inside error scopes.

A change of scene resolution, of preprocessor definitions, or **Reload effects** starts a new
*generation*: everything is recompiled for the new `BUFFER_WIDTH`/`BUFFER_HEIGHT`, as ReShade does.
Effects disappear until their rebuild finishes. Objects of the old generation are released when the
last plan that uses them has been recorded.

### What follows ReShade's runtime exactly

Technique and pass order; the copy of the back buffer to `COLOR` before a technique's first pass
and after every pass that renders to the back buffer; `ClearRenderTargets`; viewport = render target
size; stencil only for back-buffer-sized passes, cleared at a technique's first stencil pass;
`DispatchSizeX/Y/Z` as workgroup counts; mipmap regeneration after a pass (`GenerateMipMaps`) and
after loading an image; blend and write-mask state (MIN/MAX blend ignore their factors, as in
D3D); sRGB reads and writes on RGBA8; unknown texture semantics read as an empty texture
(`0, 0, 0, 1`); image loading (PNG, JPEG, BMP, TGA, DDS, .cube LUTs; resized as ReShade resizes);
special uniforms `frametime`, `framecount`, `random`, `pingpong`, `date`, `timer`; preset reading
and writing. A texture that a pass both samples and writes reads as zero there (ReShade leaves that
undefined on D3D12; WebGPU forbids it).

## Limitations for testing

What to expect in this first iteration, roughly in order of how likely you are to notice:

1. **Untested in-game.** Expect bring-up problems: the composite or a snapshot may be wrong
   (black, flipped, stretched), an insertion point may not run. Start with one simple effect at
   **After HUD**, then move it to the other points. `DisplayDepth.fx` from ReShade's standard
   shaders is the quickest depth check.
2. **Compute shaders over the device's limits are skipped.** The game's device has WebGPU's default
   limits: 256 threads per workgroup and 16 KB of workgroup memory (D3D allows 1024 and 32 KB).
   Effects that need more are listed under errors; in the test corpus that is 4 of 149 effects
   (CMAA 2, ReVeil and two iMMERSE effects).
3. **No keyboard or mouse input.** `source = "key"`, `"mousepoint"`, `"mousedelta"`,
   `"mousebutton"` and `"mousewheel"` uniforms stay at zero, technique toggle keys do nothing, and
   `overlay_open` is always false.
4. **Parameters UI.** The host UI has integer steppers only, so float parameters are a text box plus
   -/+ buttons (step = `ui_step`, or 1/100 of the range); matrices and arrays are not editable.
   Per-effect preprocessor definitions are a text line (`NAME=VALUE, ...`); the effect's tested
   definitions and their current values are listed in its help text.
5. **Resolution changes recompile everything** (effects vanish for the duration, as in ReShade).
   All effects in the folder are compiled at start-up on a background thread; a large shader
   collection takes a while before the Techniques list fills.
6. **First enable of an effect** builds its pipelines on the game's device from the background
   thread; on D3D12 this may cause a short hitch.
7. **Mirror mode**: at **Before HUD** and **After HUD** the image is already mirrored but depth is
   not, so depth-based effects misalign there. The two earlier points are unaffected.
8. **Depth outside gameplay**: with no 3D camera this frame (title screen, some menus) the last
   camera is used; before any camera exists depth reads as far everywhere.
9. **Precision differences** from D3D12 ReShade, by design: 16-bit unorm textures stored as 32-bit
   float, blended 32-bit float targets stored as 16-bit float, software filtering of float/integer
   textures (bilinear in the shader instead of hardware).
10. **Not implemented**: texture atomics (`InterlockedAdd` and friends on storage textures; the
    effect fails to compile with a clear message), uniform arrays of matrices, technique
    `timeout`, screenshots, performance mode, `pooled` textures shared between different names,
    texture semantics other than `COLOR` and `DEPTH`, ReShade add-ons, multiple presets / preset
    switching.
11. **The "Before particles & post-processing" point needs the game hook.** On a game build other
    than the pin it may not resolve; the mod logs a warning and that point does nothing.
12. **Compatibility renderers** (D3D11, OpenGL ES) are untested; effects they cannot run are
    rejected by validation.

When reporting: the log lines from `dev.automata.reshade_port` (compile errors and rejected
techniques are logged once per build), the effect files involved, and a screenshot per insertion
point.

## Offline checkers

Two tools run the mod's own code on Dawn's null backend (no GPU) with a device configured like the
game's. CI does not run them.

- `tools/fx_check.cpp`: compiles effect files, creates every shader module, and with `--gpu`
  builds every texture, pipeline and bind group, records every technique and submits.
- `tools/runtime_check.cpp`: drives the whole `Runtime` like the game would: a base folder with
  `reshade-shaders`, the build thread, the validation device, every technique enabled and spread
  over the four insertion points, frames with fake snapshots, the composite draw, and the preset
  file. Any error on the "game" device fails it.

Build (Linux, after `cmake -B build` has fetched the prebuilt Dawn):

```sh
D=build/_deps/dawn_prebuilt-src
M=mods/reshade-port
SRC="$M/src/codegen_wgsl.cpp $M/src/fx_compile.cpp $M/src/fx_gpu.cpp $M/src/fx_images.cpp \
     $M/src/fx_preset.cpp $M/src/fx_runtime.cpp $M/src/fx_uniforms.cpp $M/third_party/reshadefx/*.cpp"
FLAGS="-std=c++20 -O1 -I$M/src -I$M/third_party/reshadefx -I$M/third_party/stb -I$D/include"
LIBS="$D/lib/libwebgpu_dawn.a -ldl -lpthread -lX11"
g++ $FLAGS $M/tools/fx_check.cpp $SRC $LIBS -o build/fx_check
g++ $FLAGS $M/tools/runtime_check.cpp $SRC $LIBS -o build/runtime_check

./build/fx_check --gpu --size 1920x1080 --include <Shaders folder> <file.fx>...
./build/runtime_check <folder that contains reshade-shaders> [frames]
```

Last results (149 effects from the standard, SweetFX, prod80, qUINT, iMMERSE, AstrayFX, fubax,
Insane and legacy collections, at 1920x1080 and 1280x720): all compile and pass validation except
the four over the device limits above; `runtime_check` ran 160 techniques across all four points
with no error on the game device. What they cannot show: whether the images are right.

## Files

| File | Role |
| :-- | :-- |
| `src/mod.cpp` | services, insertion points, composite draw, UI |
| `src/game_hooks.cpp` | the one game hook (`dComIfGd_drawXluListDark`) |
| `src/fx_runtime.{hpp,cpp}` | effect discovery, build thread, validation device, generations, technique list, preset, frame plans |
| `src/fx_gpu.{hpp,cpp}` | textures, pipelines, pass recording; utility shaders (back-buffer copy, depth conversion, mipmaps) |
| `src/fx_compile.{hpp,cpp}` | preprocessor setup (ReShade's predefined macros, fixed depth definitions), parse and assemble |
| `src/codegen_wgsl.{hpp,cpp}` | the WGSL code generator |
| `src/fx_formats.hpp` | how each ReShade format is stored on the device |
| `src/fx_uniforms.{hpp,cpp}` | uniform storage, typed access, special sources |
| `src/fx_images.{hpp,cpp}` | image loading for `source` textures |
| `src/fx_preset.{hpp,cpp}` | ReShade's INI dialect |
| `third_party/reshadefx`, `third_party/stb` | vendored, see each `VENDORED.md` |
| `tools/fx_check.cpp`, `tools/runtime_check.cpp` | offline checkers (not built by CMake) |
