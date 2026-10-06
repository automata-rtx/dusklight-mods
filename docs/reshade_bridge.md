# ReShade Bridge

**Status: works in-game** (the maintainer's test: an ambient-occlusion effect at *Before
transparency* and other effects, seamless). Version 0.2.0 hands the frame to ReShade at ReShade's
screen size when the game renders at another internal resolution ("Resolution", below); that part
is new and untested in-game. Built only on branch `claude/reshade-bridge-dndsck`, where it is the
only mod CI builds.

ReShade Bridge lets a normally installed ReShade run its techniques *inside* Dusklight's frame
instead of over the finished image. Each technique is assigned to one of four points of the frame:
an ambient-occlusion effect can run on the opaque world only, before water, particles, the game's
own bloom and depth of field, and the HUD are drawn over it; a colour grade can run under the HUD.
Effects get the game's real depth buffer, already converted to what ReShade's depth functions
expect, with no depth-buffer guessing.

ReShade itself does all the effect work: compiling, the overlay, presets, every effect feature. The
bridge only decides *where in the frame* that work happens.

## The two parts

| File | What it is | Where it goes |
| :-- | :-- | :-- |
| `reshade_bridge.dusk` | The Dusklight mod. Hands the frame to ReShade at the chosen points and puts the result back. | Dusklight's mods folder |
| `dusklight_reshade_bridge.addon64` | A ReShade add-on. Runs the techniques when the mod hands the frame over. | Next to ReShade's DLL |

Both come from the same CI run, in the `mods-combined` artifact. Use the two files from one run
together; they agree on a private protocol (`mods/reshade-bridge/include/drb_protocol.hpp`), and
two files from builds with different protocol versions refuse each other (both sides say so).
Either one without the other does nothing: without the add-on the mod stays idle; without the mod,
ReShade runs as it always does.

## Requirements

- **Windows x64.** ReShade has no build for the other platforms Dusklight runs on. The `.dusk`
  loads everywhere and stays idle.
- **Dusklight `v2.0.0` on Direct3D 12.** Settings > Graphics backend: *D3D12* or *Auto* (Auto picks
  D3D12 first on Windows). Under Vulkan or D3D11 the add-on reports that it is not on D3D12 and
  does nothing.
- **ReShade 6.8.0 or newer, the build "with full add-on support".** ReShade's website offers it as a
  separate download next to the normal installer. The normal build refuses to load add-ons that are
  not signed by the ReShade project, and does not report the copy commands the bridge relies on.

## Installing

1. **ReShade.** Run the installer *with full add-on support*, pick `Dusklight.exe`, choose the
   *Direct3D 10/11/12* API (it installs ReShade as `dxgi.dll` next to the game). When it offers
   effect packages, pick the ones you want (or add them later).
2. **The add-on.** Copy `dusklight_reshade_bridge.addon64` into the folder that holds `Dusklight.exe`
   and ReShade's `dxgi.dll`. (If you set an *Add-on search path* in ReShade's Settings tab, put it
   there instead.)
3. **The mod.** Copy `reshade_bridge.dusk` into `%APPDATA%\TwilitRealm\Dusklight\mods` and enable
   it in the game's Mods menu.
4. Start the game and open ReShade's overlay (Home key by default).
   - **Add-ons tab:** *Dusklight Bridge* should be listed and ticked. **Untick Generic Depth.** The
     bridge supplies the depth buffer itself; Generic Depth's guessing costs time on every draw
     call and can briefly fight the bridge's binding.
   - **Home tab:** enable techniques as usual.
   - **Dusklight tab:** choose where each enabled technique runs (below).

The add-on sets ReShade's depth definitions (`RESHADE_DEPTH_INPUT_IS_REVERSED` and the rest) to the
values the bridge's depth needs, as global preprocessor definitions saved in `ReShade.ini`. Effects
recompile once when that happens. Do not change them: the depth texture is encoded for exactly
these values. If a preset overrides one, the add-on corrects the preset's copy too.

## Choosing where each technique runs

The **Dusklight** tab in ReShade's overlay lists the enabled techniques (tick *Show disabled
techniques* to set one up before enabling it). Each has a point:

| Point | The image the technique sees | Drawn over it afterwards |
| :-- | :-- | :-- |
| **Before transparency** | The opaque world only | Water, glass and other translucent surfaces, particles, the game's motion blur, depth of field, heat haze and bloom, and the HUD |
| **Before particles & post-processing** | The whole world, translucent surfaces included | Particles, the game's post-processing, the HUD |
| **Before HUD** (default) | The finished world, after the game's post-processing | The HUD |
| **After HUD** | The finished frame | Nothing (this is where ReShade normally runs) |

Rules of thumb: ambient occlusion and other effects that shade surfaces from depth belong at
**Before transparency**, so water and particles are not shaded and the game's fog, bloom and
depth of field apply on top as they do to the world. Colour grading and sharpening belong at
**Before HUD**. Only effects meant to change the HUD too belong at **After HUD**.

Techniques at the same point run in the Home tab's order. A choice is saved in `ReShade.ini`,
section `[DUSKLIGHT_BRIDGE]`, as `<technique>@<effect file>=<point number>` (0 to 3, in the table's
order).

While the bridge is on, ReShade's own end-of-frame pass is skipped, so every technique runs exactly
once per frame, at its point. Switch the bridge off in Dusklight (Mods > ReShade Bridge > *Run
ReShade in the game's frame*) and ReShade runs everything over the finished frame again, as if the
bridge were not installed.

### Depth

Effects see the game's depth from the end of the world pass, linearized over the **Depth range**
option in the mod's pane: the view distance that linear depth 1.0 stands for. `0` (default) uses
the camera's far plane, which changes from area to area. A smaller value gives nearby scenery more
of the depth range, which is what lowering `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` does in plain
ReShade (that definition itself must stay at 1000; see above).

At **Before HUD** and **After HUD** the depth is the same world depth (particles and the HUD have
none). On screens without the 3D world (menus), effects see "far" everywhere.

## Resolution

Dusklight can render at an internal resolution other than the window's (Settings > Graphics,
internal resolution: a multiple of the game's 448-line base; *Auto* is the window's size) and
scales the finished frame to the window at the very end, after the HUD (Aurora,
`resample_present_source`). Every bridge point is before that, so the frame there is at the
internal resolution.

ReShade's effects only work on frames of ReShade's screen size (the window's). ReShade compiles an
extra copy of an effect for every other size it is handed (`BUFFER_WIDTH`/`BUFFER_HEIGHT` follow),
but the effect's own textures are shared by name with the screen-size copy compiled at start-up
and keep the screen's size (`create_effect` in ReShade 6.8's `source/runtime.cpp`). A frame of
another size is processed into textures of the wrong size: before 0.2.0, with the internal
resolution above the window's, debug views showed the top-left part of the image stretched over the
screen. `ReShade.log` names the affected effects ("already created a texture with the same name
but different dimensions").

So the bridge hands every frame over at the screen's size:

1. **Down.** The frame is scaled to the screen with an area average (each frame pixel weighted by
   how much of the screen pixel it covers; Dusklight's *Area* resampler), placed where Dusklight's
   present puts it (black bars when the aspect ratios differ). Depth takes the frame pixel under
   each screen pixel's centre, never an average: an average across a silhouette is a surface that
   is not there. ReShade sees what a game running natively at the window's size would give it, and
   effects tuned in screen pixels (sample radii, sharpening) look as their authors intended.
2. **ReShade runs** at the screen's size.
3. **Up.** Only what ReShade *changed* (its result minus what it was handed) is scaled back up and
   added to the full-resolution frame. Where an effect changed nothing, the frame keeps its full
   internal-resolution detail; the game then draws the rest of the frame (water, particles, the
   HUD) at full resolution over it, and Dusklight's own scaling at the end brings it to the window.

**Scale-up** (mod option): how the change is spread over the frame's pixels.

- **Edge-aware** (default). Each frame pixel compares its own depth with the depths of the four
  nearest screen pixels. If they agree (within 5%), it takes their bilinear blend; if one does not
  (a silhouette runs between them), it takes the change of the screen pixel whose depth is closest
  to its own. Ambient occlusion computed for a shoulder stays on the shoulder instead of darkening a
  fringe of the wall behind it. Points without this frame's depth fall back to Simple.
- **Simple.** Plain bilinear, for comparison: along silhouettes the change bleeds up to about one
  screen pixel onto the other side.

**Debug view** (mod option): *Change layer* at one point draws only what ReShade changed there,
scaled up exactly as it is applied (same textures, same Scale-up), over the whole finished frame:
mid-grey where nothing changed, darker where ReShade darkened, brighter where it brightened. It is
drawn at the end of the frame so that nothing drawn after the point (water, the game's bloom and
depth of field, the HUD) hides it. Compare the two Scale-up settings along silhouettes here.

What is not exact: an effect's change is computed at the screen's size, and the trip up to the
internal resolution and back down through Dusklight's scaling softens changes that are themselves
pixel-sized (sharpening, post-process antialiasing) when the internal resolution is not an exact
multiple of the window's (with the 448-line base it rarely is). Smooth changes (ambient occlusion,
colour grading, fog, bloom) come through essentially exactly. When the frame is the screen's size
(*Auto*), the change added back is exactly ReShade's.

## Status and diagnostics

Three places report what the bridge is doing:

- **Dusklight, Mods > ReShade Bridge:** one status line (add-on found? ReShade started? effects on?
  the game's resolution and the size ReShade gets the frame at), then, per point in use: *frames
  sent* (by the mod), *received* (by the add-on) and *technique runs*. Warnings for Generic Depth
  being on, depth definitions not applied, the hook for *Before particles* having failed, and the
  two files coming from different builds. With a debug view on, whether it is shown.
- **ReShade, Dusklight tab > Hand-over statistics:** the hand-over size (ReShade's screen), the same
  counters from the add-on's side, and the number of techniques at each point.
- **`ReShade.log`:** lines starting `Dusklight bridge:` (attachment to the effect runtime, the first
  technique run at each point).

Reading the counters:

| Symptom | Meaning |
| :-- | :-- |
| No ReShade overlay at all | ReShade did not load (see "How ReShade gets loaded" below); check that `dxgi.dll` and `ReShade.log` are next to `Dusklight.exe` and what the log says |
| Status: *ReShade add-on not loaded* | The `.addon64` is not next to ReShade's DLL, or ReShade is the normal build (no full add-on support), or it is older than 6.8.0 |
| *sent* rises, *received* stays 0 | ReShade loaded the add-on but does not report Dawn's copies to it. Not expected with a full add-on build; please report it |
| *received* rises, *technique runs* stays 0 | Effects are still compiling, or ReShade's effects are toggled off |
| Everything rises, nothing visible | Check the technique is enabled and at the point you expect; try **After HUD**, where it behaves like plain ReShade |

## How it works

### How ReShade gets loaded

Dawn loads `d3d12.dll` and `dxgi.dll` from System32 only (`DynamicLib::OpenSystemLibrary`), so it
never picks up ReShade's `dxgi.dll` by itself. SDL does: its Windows video start-up loads
`DXGI.DLL` by plain name (`SDL_windowsvideo.c`, `WIN_CreateDevice`), which finds ReShade's copy in
the game folder before the game creates its graphics device. Once a module named `dxgi.dll` is
loaded, Windows resolves Dawn's later request to that module, and ReShade hooks `d3d12.dll` itself.
If a future SDL stops loading DXGI at start-up, ReShade would not load at all (no overlay); nothing
would break.

### The hand-over

Dusklight renders with WebGPU through Dawn. Dawn turns each command buffer into a Direct3D 12
command list when the game submits it, and ReShade (with full add-on support) wraps that command
list and reports every texture copy to its add-ons. The mod and the add-on never share a GPU
handle; they meet in that copy event.

At each point with work (`src/mod.cpp` `run_point`, `src/bridge_gpu.cpp` `record_point`), the mod:

1. takes GfxService snapshots of the scene colour and depth (`resolve_pass`);
2. scales the colour to ReShade's screen size into a hand-over texture of its own (one per point)
   and copies that to an *input* texture; converts the depth into a frame-size R32Float texture
   (encoding below) and samples it to a screen-size one, shared by all points ("Resolution");
3. records two one-texel copies: screen-size depth to a *depth marker*, then hand-over texture to a
   *colour marker*. A marker is 1597 texels wide; its height encodes the point (11 + point for
   colour, 23 + point for depth);
4. draws ReShade's change (hand-over minus input) scaled up over the scene, RGB only (the game's
   alpha is kept).

When Dawn records those copies, the add-on (`addon/bridge_addon.cpp`) recognises the markers by
their size. On the depth marker it notes the depth resource. On the colour marker it transitions
the colour texture to render target and the depth texture to shader resource, binds the depth to
ReShade's `DEPTH` semantic, calls ReShade's `render_technique` for every enabled technique assigned
to that point, transitions both back, and restores Dawn's command-list state. Both marker copies
are skipped: they carry no data.

Which points have work, and the screen size, flow back the other way: the add-on publishes a bit
mask of points with enabled techniques and ReShade's screen size in a small named shared-memory
block (`Local\DusklightReShadeBridge.<pid>`, `include/drb_protocol.hpp`), and the mod only snapshots
and composites at those points. Either side creates the block; neither depends on the other staying
loaded.

### Restoring Dawn's state

ReShade's `render_technique` changes the command list's descriptor heaps, root signatures and root
arguments and, on Direct3D 12, restores none of them. Dawn re-sets everything else at the start of
each pass, but these three it tracks across the passes of a command buffer and does not set again
while they look unchanged (`CommandBufferD3D12.cpp`: `DescriptorHeapState`, and
`BindGroupStateTracker::AreLayoutsCompatible`). The add-on therefore tracks them from ReShade's
command-list events while Dawn records (`addon/state_restore.cpp`) and re-applies them after every
hand-over: the heaps through ReShade's own command list (which remembers the application's pair),
the root signatures, and every root argument (descriptor tables, root constants, root CBV/SRV/UAV
addresses) natively.

This is the most fragile part of the design. It was derived from reading Dawn's D3D12 backend at
the pinned aurora's Dawn; a Dawn update that caches more state across passes would need another
look here. A wrong restore shows as garbage or flicker after the hand-over point, or a device-lost
crash.

### Uniforms and ReShade's end-of-frame pass

Once per frame, before the first technique runs, the add-on calls ReShade's `render_effects` with no
target. That updates the per-frame uniforms (timer, frame count, key and mouse state) and marks the
frame's effects as rendered, so ReShade skips its own pass at present. On frames without a
hand-over, while the bridge is on, the add-on makes the same call at present.

### Depth encoding

For view distance `z`, linear depth is `L = (z - near) / (range - near)`, clamped to [0, 1], and the
texture stores `1 - L * F / (1 + L * (F - 1))` with `F = 1000`. ReShade's linearization, with
`RESHADE_DEPTH_INPUT_IS_REVERSED=1`, `..._IS_LOGARITHMIC=0` and
`RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000`, turns that back into exactly `L`. The view distance
comes from the game's reversed-Z depth through the camera's projection (`fs_depth` in
`src/bridge_gpu.cpp`).

## Limitations

- **The scaling of 0.2.0 is untested in-game** (the rest works). The offline check runs its shaders
  for real and checks their output, but only on test patterns.
- **Windows x64, Direct3D 12, ReShade with full add-on support** only.
- **State restore** is the main risk (above). If the frame corrupts after a hand-over point, note
  which point and which techniques.
- **Effect size.** Effects always run at ReShade's screen size ("Resolution"). ReShade still compiles
  a second copy of each effect for the bridge's hand-over (it differs from the screen's back buffer
  in colour space); a short pause the first time.
- **Pixel-sized changes at in-frame points** (sharpening, post-process antialiasing) are slightly
  softened when the internal resolution is not the window's ("Resolution", what is not exact).
- **ReShade's per-effect GPU timings** in its overlay only cover techniques that happen to run at the
  back buffer's exact size and format.
- **ReShade's screenshot key** captures the presented frame, so it includes the effects (and their
  points); its "before effects" screenshot option has nothing to capture.
- **Generic Depth** should be off (Installing, step 4).
- **Changing the Dusklight graphics backend** needs a restart, as always; the add-on only attaches to
  a Direct3D 12 effect runtime.

## Ideas for later

Noted from the design discussion; none is built.

- **An exact round trip.** Dusklight's final scaling is fixed, known arithmetic (the viewport and
  the Bilinear or Area resampler). The composite could replicate it to check its own work before
  the game draws on: scale the change up, scale the frame down the way Dusklight will, compare
  with ReShade's result, and add the scaled-up difference, two or three times. The frame would
  then scale down to exactly what ReShade produced wherever nothing is drawn over it, pixel-sized
  changes included (except where the frame is already clipped at black or white). It needs the
  player's Resampler setting, and a *residual* debug view (ReShade's result against what the frame
  will scale down to) to prove it.
- **After HUD on the real back buffer.** Run the *After HUD* techniques at present, on the
  swapchain's image after Dusklight's own scaling, where plain ReShade runs: no conversion at all
  for effects meant to touch the HUD.
- **The HUD drawn after scaling (a Dusklight change).** If Dusklight scaled the 3D frame to the
  window *before* the HUD and drew the HUD at the window's resolution, as many PC games do,
  *Before HUD* would simply be the scaled, pre-HUD image and need no trip at all; the HUD would
  also be cheaper to draw. A mod cannot do this: GfxService's `create_pass` can redirect the
  HUD's drawing into a window-size target, but Dusklight only presents its full-resolution frame,
  so the result would have to be scaled up into it and back down, HUD included.

## Offline check

`mods/reshade-bridge/tools/bridge_check.cpp` drives the mod's GPU half on Dawn's null backend: every
point in frame order, both scale-up modes and the debug view, a resize, both scene colour formats,
a multisampled scene pass with a normal attachment, frames larger than, equal to, smaller than and
of another aspect ratio than the screen, and a compatibility-mode device. Any WebGPU error fails it
(it would be fatal in Aurora). It also checks the depth encoding against ReShade's decode and the
frame's placement against Aurora's present. With a Vulkan adapter (on Linux, Mesa's lavapipe:
`apt-get install mesa-vulkan-drivers`) it runs the scaling shaders for real on test patterns: the
scale-down and screen depth against a CPU reference, an exact round trip at the screen's size, and
a silhouette where edge-aware keeps the change on its side and simple bleeds. It cannot check
anything on the Direct3D 12 side.

```sh
cmake -B build                              # fetches the prebuilt Dawn
D=build/_deps/dawn_prebuilt-src
g++ -std=c++20 -DDUSK_MOD_FEATURE_WEBGPU=1 -I$D/include -Idusklight/sdk/include -Icommon \
    -Imods/reshade-bridge/include -Imods/reshade-bridge/src \
    mods/reshade-bridge/tools/bridge_check.cpp mods/reshade-bridge/src/bridge_gpu.cpp \
    $D/lib/libwebgpu_dawn.a -ldl -lpthread -lX11 -o build/bridge_check
./build/bridge_check
```

The add-on only builds with MSVC (CI's windows-amd64 leg). For a quick syntax check on Linux,
clang with MinGW-w64's headers works: `--target=x86_64-w64-windows-gnu -fms-extensions
-fsyntax-only`, plus a `Windows.h` that includes `windows.h` (MinGW's is lower-case).

## Files

| Path | Role |
| :-- | :-- |
| `mods/reshade-bridge/include/drb_protocol.hpp` | The contract: points, marker sizes, depth definitions, the shared-memory block |
| `mods/reshade-bridge/src/mod.cpp` | The mod: insertion points, options, status |
| `mods/reshade-bridge/src/bridge_gpu.cpp` | The mod's GPU work: copies, depth conversion, markers, composite |
| `mods/reshade-bridge/src/bridge_link.cpp` | The mod's end of the shared memory (Windows; a stub elsewhere) |
| `mods/reshade-bridge/src/game_hooks.cpp` | The hook on `dComIfGd_drawXluListDark` for *Before particles* |
| `mods/reshade-bridge/addon/bridge_addon.cpp` | The add-on: marker handling, technique runs, depth binding, overlay tab |
| `mods/reshade-bridge/addon/state_restore.cpp` | The add-on's tracking and restoring of Dawn's command-list state |
| `mods/reshade-bridge/third_party/` | ReShade's add-on headers and Dear ImGui's headers (`VENDORED.md`) |
| `mods/reshade-bridge/tools/bridge_check.cpp` | The offline check |
