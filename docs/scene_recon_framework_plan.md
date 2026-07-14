# Scene Reconstruction Framework — plan (design only, not implemented)

Status: **plan for review.** Nothing here is built. It proposes a small framework mod that
computes the depth-derived scene data our mods currently each reconstruct on their own, and
exposes it as a mod-provided service any mod (ours or third-party) can import.

## Why

Our screen-space mods each turn the game's depth snapshot into higher-level scene data before
doing their actual work. That reconstruction is the "prerequisite" — and today it is duplicated.
Consolidating it into one provider computed once per frame means: smaller mod codebases, one
reconstruction instead of N, and a clean data surface other mods can build on.

Dusklight already supports this: mods export services via `EXPORT_SERVICE` / `publish_service`
and import them via `IMPORT_SERVICE`, with a load-order guarantee that a provider's
`mod_initialize` runs before its importers' (`extern/dusklight/docs/modding.md` §"Advanced:
Exporting Services", `include/mods/service.hpp`, `include/mods/svc/host.h`). No Dusklight change
is required.

## Audit — what each mod reconstructs today

| Prerequisite | VBAO (`enhanced_ao`) | Realtime Sun Shadows | Deferred Fog |
|---|---|---|---|
| Position from depth | **view-space**, `inverse_projection` (`vbao.wgsl:114`) | **world-space**, `world_from_proj` (`shadow.wgsl`, `normal_smooth.wgsl`) | none (raw depth only) |
| Normal from depth | **view-space, raw**, atyuwen 5-tap (`vbao.wgsl:151`) | **world-space, smoothed** — 1px cross (`normal_gen`) then bilateral Gaussian (`normal_blur_h/v`) | none |
| Working resolution | AO chain res (can be **half-res**, jittered) | **full** render res | n/a |
| Depth MIP chain | yes (`preprocess_depth.wgsl`, Bevy SSAO) — **AO-specific** | no | no |
| Camera matrices | camera service | camera service | camera service |
| Raw depth snapshot | gfx `resolve_pass` | gfx `resolve_pass` | gfx `resolve_pass` |

Takeaways:
- **Camera matrices and the depth snapshot are already shared services** (camera + gfx). The
  framework must NOT re-expose them.
- The only expensive, genuinely common step is **depth→normal reconstruction** (multi-tap).
  Position reconstruction is one matrix-multiply in-shader — cheap enough that sharing a
  position *buffer* saves little and costs a texture fetch + bandwidth; not worth it.
- The **depth MIP chain is AO-specific** (single consumer) — it stays in VBAO, per "no
  single-mod functionality in the framework."
- **Deferred Fog consumes none of this today.** It would only become a consumer if the shelved
  underwater fog (`docs/deferred_fog_underwater_notes.md`) is built, which needs world position.

## The meaningfully-different inputs (flagged, as requested)

The two normal consumers do NOT want the same normal. Four axes differ; each is a decision:

1. **Space — view (VBAO) vs world (shadows).** They differ by the view rotation, both matrices
   available from the camera service. A shared buffer picks one canonical space; the other mod
   rotates in-shader (one mat3×vec3, negligible).
   - *Recommendation:* **world-space** canonical. It is the most reusable for third parties
     (lighting, fog, atmosphere all think in world space) and shadows uses it directly. VBAO
     rotates world→view (free).
   - *Downside:* VBAO gains a tiny per-sample rotation it doesn't have today.

2. **Smoothing — raw (VBAO) vs bilateral-smoothed (shadows).** This is the big one. AO needs the
   true geometric normal (smoothing softens contact darkening); shadows deliberately smooths to
   kill faceted bias banding, with a resolution-scaled radius that is a shadow-specific tuning.
   - *Recommendation:* framework provides the **raw geometric normal only.** Shadows keeps its
     bilateral blur but runs it **on the shared raw normals** — so it drops `normal_gen` (the
     reconstruction) and keeps only `normal_blur_h/v` (its unique need).
   - *Benefit:* the shadow-specific smoothing config stays out of the framework; VBAO gets
     exactly the raw normal it wants.
   - *Downside:* shadows still runs its 2-pass blur (unavoidable — only it needs it), so the
     framework saves shadows the *gen* pass, not the blur.
   - *If we instead put smoothing in the framework:* shadows offloads everything, but the
     framework inherits shadow-only params (radius, resolution scaling, bilateral tolerance) —
     violating "no single-mod functionality" and giving VBAO a knob it must ignore. Not
     recommended.

3. **Reconstruction method — atyuwen 5-tap (VBAO) vs 1px side-selected cross (shadows).** The
   5-tap is strictly more accurate at depth discontinuities.
   - *Recommendation:* unify on **atyuwen 5-tap** for the shared buffer. VBAO is unchanged;
     shadows' raw normal improves slightly.
   - *Downside:* shadows' pre-blur normals change subtly vs today — needs an in-game eyeball to
     confirm no regression in the bias look (the blur will mask most of it).

4. **Resolution — working/half-res (VBAO) vs full-res (shadows).** This is the real friction.
   Shadows is always full-res. VBAO reconstructs normals at its AO chain resolution, which can
   be half-res and is **jittered** for temporal accumulation.
   - *Recommendation:* the shared buffer is **full render resolution** (what shadows needs and
     what is most broadly useful). VBAO consumes it **in full-res mode** (samples full-res
     normals, rotating to view). In **half-res mode**, VBAO keeps its own in-chain reconstruction
     (jitter-coupled) — the shared buffer doesn't fit that path cleanly, and forcing it there
     would mean adding a half-res, jittered variant to the framework = VBAO-specific, avoid.
   - *Net:* VBAO's benefit is **conditional** (full-res AO only); shadows' benefit is
     unconditional.

**Honest bottom line:** shadows is the clean beneficiary (drops its whole `normal_gen` path).
VBAO benefits only in full-res mode and gains a cheap rotation; its half-res + MIP-chain paths
stay local. Deferred Fog is a future consumer at best. The framework is still worth it — mostly
for shadows today and for making the data trivially available to *new* mods — but it is not the
across-the-board dedup that "each mod does the same thing" first suggests.

## Proposed design

### New mod: `dev.automata.scene_recon` (the framework/provider)

- `mods/scene_recon/` — `mod.json`, `src/mod.cpp`, `res/reconstruct.wgsl`.
- Public header `mods/scene_recon/include/scene_recon_service.h` — the shared service contract,
  `#include`-able by any consumer (ours or third-party). This is the only file a consumer needs.

### Service contract (`scene_recon_service.h`)

```c
#define SCENE_RECON_SERVICE_ID "dev.automata.scene_recon"
#define SCENE_RECON_SERVICE_MAJOR 1u
#define SCENE_RECON_SERVICE_MINOR 0u

typedef struct SceneReconFrame {
    uint32_t struct_size;
    WGPUTextureView world_normal;   // rgba*float: xyz = world-space geometric normal, w = raw depth
    WGPUTextureView linear_depth;   // r32float view-space linear depth (0/null if not requested)
    uint32_t width;
    uint32_t height;
} SceneReconFrame;

typedef struct SceneReconService {
    ServiceHeader header;
    // Ensure this frame's reconstruction is computed and queued into the command stream, then
    // return its resources. Idempotent per frame (first call does the work; later calls return
    // the cached result). Call from a game-thread stage callback BEFORE your own draw/compute
    // that samples the data. Views are frame-valid (gfx contract) - never cache across frames.
    ModResult (*get_frame)(ModContext* ctx, SceneReconFrame* out);
} SceneReconService;

#ifdef __cplusplus
template <> struct dusk::mods::ServiceTraits<SceneReconService> {
    static constexpr const char* id = SCENE_RECON_SERVICE_ID;
    static constexpr uint16_t major_version = SCENE_RECON_SERVICE_MAJOR;
};
#endif
```

Minor-versioned: later we can append fields (e.g. `view_normal`, a half-res variant) without a
break; consumers gate new fields with `SERVICE_HAS`.

### Ordering — lazy compute on first request (the key design choice)

The provider does NOT rely on stage-hook execution order. Instead `get_frame` is
**compute-on-first-call-per-frame**: the first consumer to call it (during that consumer's
`SCENE_AFTER_OPAQUE` game-thread callback) triggers the provider to resolve depth once, push its
reconstruction compute into the command stream, cache the resulting views for the frame, and
return them. Because the compute is pushed *before* the caller's subsequent `push_draw`, GPU
ordering is correct. Subsequent consumers the same frame get the cached views for free. A frame
counter / reset (on `SCENE_BEGIN`) invalidates the cache each frame.

This decouples the framework from load/stage order entirely, and means a consumer only pays for
reconstruction if *something* actually requests it that frame.

### What the provider computes

- **World-space geometric normal + raw depth**, full-res, atyuwen 5-tap, `rgba32float`
  (matching shadows' current precision; see the rgba16 finding in the shadows work — half-float
  perturbs a downstream bilateral by up to ~1.8°, so keep 32-bit for the shared buffer unless
  a consumer opts into a packed variant later).
- **Optional linear depth** (`r32float`), only if a consumer requests it (a flag on a future
  `get_frame_ex`, or always-on if cheap). Universal primitive; low cost.

### Non-goals (explicitly out of the framework)

- Camera matrices (camera service already provides them).
- The raw depth snapshot / pass management / draw dispatch (gfx service).
- VBAO's depth MIP chain, half-res jitter, temporal reprojection (AO-specific).
- Shadows' bilateral normal smoothing (shadow-specific).
- Any per-mod tuning parameter.

## Consumer migration

- **Shadows:** delete `normal_gen` and its pipeline; `IMPORT_OPTIONAL_SERVICE(SceneReconService)`;
  in the composite path call `get_frame`, feed `world_normal` into `normal_blur_h/v` (its blur is
  unchanged). Keep a fallback to the current inline reconstruction when the service is absent, so
  the mod still runs standalone. Net: removes the gen shader + its dispatch wiring.
- **VBAO:** in **full-res** mode, `IMPORT_OPTIONAL_SERVICE`, call `get_frame`, sample
  `world_normal`, rotate to view space, skip its own `reconstruct_normal`. In **half-res** mode,
  keep the existing in-chain reconstruction (no change). Guard on service presence + resolution.
- **Deferred Fog:** no change now. If underwater fog is built later, it imports the service for
  world position (reconstructed from `linear_depth` or re-derived from depth) instead of adding
  its own reconstruction.

## Third-party access (a first-class goal)

Any mod, ours or not, integrates in three steps:
1. `#include "scene_recon_service.h"` (ship it in the SDK/samples, or vendor it).
2. `IMPORT_OPTIONAL_SERVICE(SceneReconService, svc_recon);` at file scope.
3. In a game-thread stage callback: `SceneReconFrame f{sizeof(f)}; if (svc_recon &&
   svc_recon->get_frame(mod_ctx, &f) == MOD_OK) { /* bind f.world_normal ... */ }`.

Optional import + null check means their mod still loads if the framework isn't installed. No
coupling to our mods beyond the shared header.

## Load order, lifecycle, fallback

- **Load order:** an `IMPORT_SERVICE` is a dependency declaration; the loader runs the provider's
  `mod_initialize` first (`api.h:78-87`). Use `IMPORT_OPTIONAL_SERVICE` so consumers still load
  (with fallback) when the framework is absent; required-import would harden the dependency at
  the cost of standalone use.
- **Lifecycle:** the provider hands out only frame-valid texture views and holds **no per-consumer
  state**, so it needs no `watch_mod_lifecycle` / detach handling. Consumers drop the service
  pointer on their own shutdown.
- **Fallback:** each consumer keeps its current reconstruction behind the null check, so removing
  or failing the framework degrades to today's behavior rather than breaking the mod.

## Benefits / costs summary

Benefits:
- One depth→normal reconstruction per frame instead of two (shadows + full-res VBAO), one depth
  resolve shared.
- Shadows loses its `normal_gen` shader and wiring; VBAO loses its full-res normal recon.
- A documented, third-party-friendly G-buffer-lite surface for future mods (shadowed fog,
  volumetrics, SSR-lite, etc.).

Costs / risks:
- A new mod to install and version; optional-import consumers keep fallback code (so codebases
  shrink less than a hard cutover would).
- VBAO's benefit is conditional (full-res only); half-res AO is unchanged.
- Unifying the reconstruction method changes shadows' pre-blur normals slightly — needs in-game
  validation.
- Everything here touches per-frame GPU handles and load ordering, so it must be verified
  in-game, not just offline.

## Open decisions (need your call before building)

1. **Canonical space:** world (recommended, most reusable) vs view (VBAO-native).
2. **Smoothing:** framework provides raw only + shadows keeps its blur (recommended) vs framework
   also provides a smoothed variant (couples shadow params into the framework).
3. **VBAO half-res:** leave VBAO's half-res recon local (recommended) vs add a half-res/jittered
   variant to the framework (VBAO-specific, not recommended).
4. **Import strength:** optional + fallback (robust, recommended) vs required (simpler consumers,
   harder dependency).
5. **Scope of first version:** normals only, or normals + linear depth from day one.
