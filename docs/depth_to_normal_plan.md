# Depth to Normal — plan (scoped input mod, not implemented)

Status: **plan for review.** Supersedes the earlier broad "scene reconstruction framework"
idea. That analysis showed the only genuinely shared, expensive prerequisite between VBAO and
Realtime Sun Shadows is depth→normal reconstruction — everything else (linear depth, position,
MIP chain, smoothing, half-res jitter) is either cheap to recompute in-shader or a
signal-specific choice that must stay local. So instead of a wide framework we scope to exactly
that one input.

## What it is

A tiny provider mod, **`dev.automata.depth_to_normal`** ("Depth to Normal"), that reconstructs a
per-pixel geometric surface normal from the game's depth snapshot once per frame and exposes it
as a mod-provided service. Any mod — ours or third-party — imports it instead of reconstructing
normals itself. It does one thing and does it well.

### The output (the exact shared input)

A single texture, full render resolution, `rgba32float`:
- **`xyz` = world-space geometric normal**, unit length, oriented toward the camera.
- **`w` = raw reversed-Z depth** (the same value the snapshot holds), carried along so consumers
  get a depth reference for bilateral weighting / rejection without a second fetch.

Reconstruction uses the **atyuwen 5-tap side-selected method** (what VBAO already uses,
`vbao.wgsl:151`) — strictly more stable across depth discontinuities than the 1-px cross shadows
uses today, so both consumers improve or stay equal.

**Attribution:** that 5-tap reconstruction comes from **Encounter's `ao_mod` demo** (which ports
it from Bevy Engine's SSAO / atyuwen's normal-reconstruction post). This mod must carry the same
credit and the Bevy/XeGTAO license files (`res/licenses/`) that VBAO already ships, since its core
shader is adapted from that demo.

### Why world-space, and why that's lossless for VBAO

VBAO works in view space, shadows in world space. The provider picks **world space** as
canonical (the most reusable for downstream effects — lighting, fog, reflections all think in
world space). VBAO rotates world→view with the view matrix's rotation (`mat3 · vec3`,
negligible), and this is **mathematically identical** to reconstructing in view space directly:
world position and view position differ by a rigid transform, so neighbor *differences* differ
only by the view rotation, the cross product differs only by that same rotation (det = 1 for a
rotation), and rotating back recovers exactly the view-space normal. No precision or quality
loss — VBAO gets the identical normal it computes today, just sourced from the shared buffer.

### Resolution: full-res, and the honest VBAO caveat

The buffer is **full render resolution** — required by shadows (a resolution-capped normal was
tried and rejected: it "blurs fine geometry away and needs a lossy upscale", `normal_smooth.wgsl`
failed-approach #3) and the most broadly useful default. VBAO consumes it directly in **full-res
mode**. In **half-res mode**, VBAO keeps its own in-chain, jitter-coupled reconstruction — that
path is bound to its temporal accumulation and can't read a full-res buffer cleanly, and adding a
half-res/jittered variant here would be VBAO-specific (out of scope for a single-purpose input).
So VBAO's benefit is: full-res AO sources normals from the service; half-res AO is unchanged.

## Service contract (`depth_to_normal_service.h`)

```c
#define DEPTH_TO_NORMAL_SERVICE_ID "dev.automata.depth_to_normal"
#define DEPTH_TO_NORMAL_SERVICE_MAJOR 1u
#define DEPTH_TO_NORMAL_SERVICE_MINOR 0u

typedef struct DepthToNormalFrame {
    uint32_t struct_size;
    WGPUTextureView normal;  // rgba32float: xyz = world-space geometric normal, w = raw depth
    uint32_t width;
    uint32_t height;
} DepthToNormalFrame;

typedef struct DepthToNormalService {
    ServiceHeader header;
    // Ensure this frame's normal buffer is computed and queued into the command stream, then
    // return its view. Idempotent per frame (first call does the work; later calls return the
    // cached result). Call from a game-thread stage callback BEFORE the draw/compute that
    // samples it. The view is frame-valid (gfx contract) - never cache it across frames.
    ModResult (*get_frame)(ModContext* ctx, DepthToNormalFrame* out);
} DepthToNormalService;

#ifdef __cplusplus
template <> struct dusk::mods::ServiceTraits<DepthToNormalService> {
    static constexpr const char* id = DEPTH_TO_NORMAL_SERVICE_ID;
    static constexpr uint16_t major_version = DEPTH_TO_NORMAL_SERVICE_MAJOR;
};
#endif
```

Minor-versioned: a later minor could append (e.g. a `view_normal` convenience view, or a curvature
buffer) without breaking existing consumers, gated by `SERVICE_HAS`.

## Ordering — lazy compute on first request

`get_frame` is **compute-on-first-call-per-frame**: the first consumer to call it (during its own
game-thread stage callback) makes the provider resolve depth once, push the reconstruction compute
into the command stream, cache the view for the frame, and return it. Because the compute is
queued before the caller's subsequent draw, GPU ordering is correct; later callers reuse the
cached view for free. A per-frame reset (on `SCENE_BEGIN`) invalidates the cache. This decouples
the provider from stage-hook order entirely and means the reconstruction only runs if something
actually asks for it that frame.

## Implementation shape (when built)

- `mods/depth_to_normal/` — `mod.json`, `src/mod.cpp`, `res/reconstruct.wgsl`, and the public
  header `include/depth_to_normal_service.h`.
- Host: import gfx + camera services; on first `get_frame` of a frame, `resolve_pass(depth)`,
  build the world-unproject matrix from the camera service, push one compute dispatch
  (`reconstruct.wgsl`) writing the normal buffer, cache + return the view. Holds no per-consumer
  state, so no lifecycle/detach handling needed.
- Shader: the atyuwen 5-tap reconstruction (lifted from VBAO's `reconstruct_normal`, unprojected
  to world space), one `@compute` entry.

## Consumer migration (separate, in-game-verified step)

- **Shadows**: `IMPORT_OPTIONAL_SERVICE`; delete `normal_gen` + its pipeline; feed the service's
  normal into its existing bilateral blur (`normal_blur_h/v`) unchanged; keep the current inline
  reconstruction behind the null check so it still runs standalone.
- **VBAO (full-res)**: `IMPORT_OPTIONAL_SERVICE`; sample the service normal, rotate to view, skip
  its own `reconstruct_normal`; half-res path unchanged; fallback behind the null check.
- **Deferred Fog**: no change (uses only raw depth today).

Because these change two shipping mods' behavior and touch per-frame GPU handles + load order,
they must be validated in-game, not just offline — so migration follows the provider, it doesn't
ship with it.

## Third-party access (a first-class goal)

Three lines, no coupling to our mods beyond the shared header:
1. `#include "depth_to_normal_service.h"`
2. `IMPORT_OPTIONAL_SERVICE(DepthToNormalService, svc_n2d);` at file scope.
3. In a game-thread stage callback: `DepthToNormalFrame f{sizeof(f)}; if (svc_n2d &&
   svc_n2d->get_frame(mod_ctx, &f) == MOD_OK) { /* bind f.normal ... */ }`.
Optional import + null check means their mod still loads if Depth to Normal isn't installed.

## Non-goals

Camera matrices (camera service), the depth snapshot / pass management (gfx service), linear
depth / position buffers (cheap to recompute in-shader), VBAO's MIP chain and half-res jitter,
shadows' bilateral smoothing, any per-consumer tuning. One input, nothing else.

## Future potential — what a per-pixel normal unlocks

The game's forward renderer never exposes a screen-space normal; reconstructing one is the single
missing ingredient for a whole class of effects. With Depth to Normal published, each becomes a
small consumer instead of re-deriving geometry:

- **Screen-space reflections (water, wet stone, polished Temple-of-Time floors, armor).** SSR
  reflects the view ray about the surface normal and marches depth — normal + depth is exactly
  the input. The most visually impactful thing this enables.
- **Toon / ink outlines (Wind-Waker-style).** The classic outline detector is normal
  discontinuity + depth discontinuity. A stylized-outline mod is almost entirely "read
  Depth to Normal, threshold the edges, draw ink." A very TP-appropriate look.
- **Screen-space GI / directional occlusion (SSGI, SSDO).** The natural evolution of the AO you
  already have: gather a single bounce of light (or directional occlusion) from neighbors using
  their normals and depths. Adds colored bounce light and directional contact shading.
- **Rim light / fresnel / wetness.** A screen-space rim highlight on characters, or a fresnel
  sheen on water and wet surfaces after rain, from `dot(normal, view)` — material richness with
  no game-shader edits.
- **Curvature / cavity shading.** The spatial derivative of the normal is curvature; a cavity term
  darkens tight creases (complements AO), and edge-highlight/wear effects fall out of the same
  data.
- **Normal-aware fog & light shafts.** Deferred Fog could add a surface-orientation light-wrap
  term (surfaces facing the sun scatter differently), and the shelved underwater fog could shade
  the lakebed by its slope.
- **Geometry-guided AA / upsampling.** Normal+depth edges predicate sharper anti-aliasing or a
  cleaner half-res→full-res upsample for other screen-space passes.

The ecosystem angle: because it's a mod-exported service, none of these need to re-solve
reconstruction — any community modder gets a correct, consistent world-space normal for three
lines of glue, which is the difference between "an SSR mod is a research project" and "an SSR mod
is a weekend."
