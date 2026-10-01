# Normal-buffer portability

How the mods get the game's authored surface normals, what happens when they are missing, and the
two shared headers that keep the mods compiling and drawing across SDK versions. For how the
normals are produced in the renderer and the history behind them, see `docs/authored_normals.md`.

## The API

GfxService 1.3 (Dusklight `v2.0.0`) adds two fields:

```
GfxResolveDesc::normal      (request)     GfxResolvedTargets::normal   (result)
```

A mod sets `normal` in the descriptor it passes to `resolve_pass` and gets back a view of the
authored normal: `RGB10A2Unorm`, view space, encoded `n*0.5+0.5`, alpha 1 where a valid normal was
written and 0 elsewhere (sky, billboards, draws without a normal attribute). The renderer writes it
into a second colour attachment of the scene pass, which it adds only once some mod asks.

In this repo, never touch those fields directly. Use `common/gfx_normal_compat.h`:

```cpp
gfx_compat::request_normal(desc, true);              // no-op if the SDK has no such field
WGPUTextureView n = gfx_compat::resolved_normal(out); // nullptr if the SDK has no such field
```

It detects each field by member name at compile time, so an SDK without them compiles to "no
normals", which every consumer already handles at runtime.

## When there are no normals

| Cause | Lasts | What the mods do |
| :-- | :-- | :-- |
| **The latch.** The first resolve that asks enables the attachment for the *next* frame and returns null this frame | One frame or so | Nothing visible. VBAO and SMAA each wait `kNormalLatchGraceFrames` (8) before reporting anything |
| **Compatibility renderer** (D3D11, OpenGL ES): no WebGPU core features, so no attachment | Permanent | VBAO disables itself and logs one warning. SMAA logs one INFO line and runs on luma edges only |
| **MSAA on**: the renderer only creates the attachment at sample count 1 | While MSAA is on | Same as above, and the log line names MSAA. The `v2.0.0` game build never enables MSAA, so this cannot currently happen |
| **SDK older than GfxService 1.3** | Until rebuilt | `gfx_normal_compat.h` compiles to "no normals"; same runtime path |

Consumers keep requesting normals every frame even when none come back: the request is what arms
the latch, and asking for an absent attachment costs nothing. Nothing in the tree reconstructs a
normal from depth any more (VBAO's depth-derived face normal is a rejection plane, not a
replacement; see `docs/vbao.md`).

To ask whether the current scene pass carries the attachment, use
`gfx_compat::ScenePassLayout::has_normal_attachment`, which scans the real pass layout for a
`GFX_ATTACHMENT_NORMAL` entry. Do not ask `GfxDeviceInfo`; it has no normal field.

## Scene-pass pipelines: `common/gfx_scene_pass.h`

A render pipeline recorded into the scene pass must declare exactly that pass's colour attachments,
and the pass gains the normal attachment at runtime, a frame after the first request. So:

```cpp
// In the draw callback (render worker):
const uint64_t key = gfx_compat::scene_pass_layout_key(*ctx);
if (key != g_cachedKey) {
    gfx_compat::ScenePassLayout layout;
    gfx_compat::scene_pass_layout_for_draw(*ctx, g_deviceInfo, layout);
    layout.color_targets[0].blend = &myBlend;        // target 0 is the scene colour; yours to set
    fragment.targetCount = layout.color_target_count;
    fragment.targets = layout.color_targets;
    depthStencil.format = layout.depth_format;
    pipelineDesc.multisample.count = layout.sample_count;
    // ...create the pipeline, remember the key
}
```

The helper feeds the SDK's `gfx_init_color_target_states`, which write-masks every attachment the
mod does not own, so a composite can never overwrite the game's normals. VBAO
(`ensure_composite_pipelines`), SMAA (`ensure_neighborhood_pipeline`) and Deferred Fog
(`ensure_fog_pipelines`) all follow this pattern. Building a scene-pass pipeline once at init is a
bug: it is silently rejected from the frame the attachment appears, and the effect just stops
drawing. Offscreen `create_pass` targets are single-target and do not use this.

`scene_pass_layout(mod_ctx, svc_gfx, ...)`, the variant that queries the service directly, is
still in the header for the unreleased mods. Do not use it for a cached pipeline.

## Why the two headers behave differently on an unknown SDK

- `gfx_normal_compat.h` **degrades quietly**: on an SDK without the normal fields it compiles to
  "no normals". Normals are optional, and their absence is visible at runtime (log lines, debug
  views).
- `gfx_scene_pass.h` **fails the build** with `#error` when it does not recognise the SDK's
  scene-layout API (override with `GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT` for a genuine pre-1.2
  base). Guessing wrong there produces no diagnostic at all.

That split comes from a real incident. Upstream renamed the scene-layout API (`get_pass_targets` →
`get_scene_target_layout`, `GFX_MAX_COLOR_TARGETS` → `GFX_MAX_COLOR_ATTACHMENTS`). The old `#if`
guard went false, every mod compiled cleanly, and every scene-pass pipeline quietly fell back to a
single colour target, which would have drawn nothing in-game. **"Degrade to absent" cannot tell a
removed API from a renamed one.** Use it only when the feature is optional and its absence shows
up at runtime; otherwise fail the build.

Two more rules that came from incidents:

- **Degrading is safe for a value you read, not for one you compare against a live value.** A guard
  once compared a compile-time-detected field (constant "undefined" on the new SDK) against the
  device's real format; it was always true, and every composite silently returned.
- **A compatibility layer protects against changed declarations, not changed behaviour.** Moving to
  upstream 2.0 cost nothing at compile time, but its normal snapshot latches where the retired fork
  snapshotted every frame, and that needed the lazy pipeline rebuild and the latch grace counter in
  every consumer. After any pin bump, read the new SDK header and the renderer behind it.

## Checking the headers against an older SDK

If you change either header, check both directions: reduce the fetched SDK header to the previous
version (delete the `normal` fields and trim the positional `GFX_*_INIT` macros to match), force a
full rebuild of every mod, and confirm the objects actually rebuilt (a cached "Built target" proves
nothing). One thing this caught: a discarded `if constexpr` branch is still parsed, so naming a
type the SDK lacks is a hard error. Member detection works for a missing *member* of an existing
type; a missing *type* needs `#if`.
