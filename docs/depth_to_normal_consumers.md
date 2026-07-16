# Depth to Normal — building on it (future consumer mods)

The **Depth to Normal** provider mod (`dev.automata.depth_to_normal`) publishes one thing: a
per-pixel **world-space geometric surface normal** (+ raw depth), reconstructed from the scene
depth buffer once per frame, as a mod-exported service. The game's forward renderer never exposes
a screen-space normal, so reconstructing one is the single missing ingredient for a whole class of
screen-space effects. This doc is a menu of what becomes buildable once that normal exists, and how
cheap each is to reach from the service.

## How a consumer taps it (the whole integration)

```cpp
#include "depth_to_normal_service.h"
IMPORT_OPTIONAL_SERVICE(DepthToNormalService, svc_n2d);   // optional so your mod still loads alone
...
// in a game-thread stage callback (e.g. SCENE_AFTER_OPAQUE), before your draw/compute:
DepthToNormalFrame f = DEPTH_TO_NORMAL_FRAME_INIT;
if (svc_n2d != nullptr && svc_n2d->get_frame(mod_ctx, &f) == MOD_OK) {
    // f.normal : rgba32float texture view, f.width x f.height
    //   .xyz = unit world-space normal (camera-facing), .w = raw reversed-Z depth
    // bind it into your pipeline; the reconstruction is already queued ahead of your pass.
}
```

Add `mods/depth_to_normal/include` to your mod's include path (CMake:
`target_include_directories(<your_mod> PRIVATE .../mods/depth_to_normal/include)`). The view is
valid for the current frame only — call `get_frame` every frame, never cache the handle. World
space is canonical; rotate into view space with the camera service's `view_from_world` if your
effect wants view-space normals.

## What becomes buildable

Roughly ordered by impact / how naturally they fall out of a normal buffer:

### Screen-space reflections (SSR)
Reflect the view ray about the surface normal and march the depth buffer to find what the surface
sees. Water, wet stone, the Temple of Time floor, polished armor. Normal + depth is exactly the
input; the rest is a ray-march the depth snapshot already supports. The single most visually
impactful thing the normal unlocks.

### Toon / ink outlines (Wind-Waker-style)
The classic outline detector is **normal discontinuity + depth discontinuity**: neighbors whose
normals or depths differ past a threshold are edges. Read the buffer, threshold, draw ink lines.
Almost the entire mod is the edge pass — a very TP-appropriate stylized look, and a great first
third-party consumer because it needs nothing but the normal and depth.

### Screen-space GI / directional occlusion (SSGI, SSDO) — **shipping as `mods/ssilvb`**
The natural evolution of the existing AO: instead of just "how occluded is this pixel," gather a
single bounce of light (SSGI) or directional occlusion (SSDO) from neighbors, weighted by their
normals and depths. Adds colored bounce light and directional contact shading. Shares the AO's
sampling machinery; the normal is what makes it *directional* rather than uniform darkening.
This is now being built as the SSILVB mod (`docs/ssilvb_plan.md`), which is also the first
**hard** (non-optional) consumer of the service — it needs the normal at every marched sample.

### Rim light / fresnel / wetness
`dot(normal, view)` gives a rim term (character silhouette glow) or a fresnel sheen (water, wet
surfaces after rain). Screen-space, so it enriches materials with no game-shader edits. Cheap —
one dot product per pixel over the normal buffer.

### Curvature / cavity shading
The spatial derivative of the normal is surface curvature. A cavity term darkens tight creases
(complements AO with a tighter, sharper crease darkening), and edge-highlight / wear looks fall out
of the same derivative. Cheap, and stacks with AO for richer contact.

### Normal-aware fog and light shafts
Deferred Fog could add a surface-orientation light-wrap term (surfaces facing the sun scatter
differently), and the shelved underwater fog could shade the lakebed by its slope. Volumetric light
shafts already use depth; the normal adds surface-aware scattering.

### Geometry-guided AA / upscaling
Normal + depth edges predicate sharper anti-aliasing (edge-directed blends) or a cleaner
half-res→full-res upsample for other screen-space passes (including AO's own half-res mode).

## Why the service form matters

Every effect above needs a per-pixel normal and none of them can get one from the game. Without a
shared provider each would re-derive reconstruction (5-tap depth taps, unprojection, orientation,
edge handling) — a chunk of fiddly, easy-to-get-subtly-wrong code. With the service it's the
three-line integration above, so a community modder's SSR or outline mod is a weekend, not a
research project. The provider is also the natural place to improve reconstruction once (e.g. a
future minor could add a curvature channel or a view-space convenience view) and have every
consumer benefit without touching their code.

## Credit

The reconstruction the provider ships is a port of the depth-to-normal function from Encounter's
`ao_mod` demo (atyuwen's 5-tap method / Bevy Engine SSAO / Intel XeGTAO). Consumers build on top of
that shared normal; the provider carries the credit and license files so consumers don't have to.
