# Authored normals (thin g-buffer) — consuming them, and how to A/B them

**Status:** landed in the mods, **pending in-game verification**. The platform side is done,
merged and published (`platform-gbuffer-test`).

The game's forward renderer now writes its own **authored, interpolated vertex normals** into a
second RGBA8 color attachment on the scene (EFB) pass, and the mod API exposes a per-frame snapshot
of it. **Graphics Hub / Depth to Normal** consumes that snapshot instead of reconstructing normals
from the depth buffer, and because every other mod reads normals through the
`dev.automata.depth_to_normal` service, all of them (VBAO, SSILVB, Realtime Sun Shadows, SMAA)
inherit it with **zero changes of their own**.

Why it matters: a depth-gradient normal is a cross product of screen-space position deltas, i.e.
the flat face normal of each rasterized triangle — faceting is inherent to the method. Authored
normals are smooth by construction, which removes the faceting *at the source* rather than blurring
it away afterwards. See `dusklight-ao/docs/thin-gbuffer-normals.md` for the renderer-side design.

---

## 1. The A/B, in one switch

Everything below is live at runtime — no rebuild, no restart, no game-build swap.

**Mods panel → Graphics Hub → Depth to Normal:**

| Control | What it does |
|---|---|
| **Use Authored Normals** (default **on**) | The whole A/B. On = authored normals with per-pixel reconstruction fallback; off = the old depth reconstruction for every pixel, bit-for-bit the path that shipped before. Greyed out on a game build without the normal buffer. |
| **Normal Source** (read-only) | What actually fed the buffer on the last drawn frame. |
| **Show Normals** | The fullscreen diagnostic overlay, drawn at `FRAME_BEFORE_HUD` so nothing composites over it. |
| **Open Normal Controls** | Window with the debug-view and basis selectors (both are SELECT controls, which the host only renders in a window tab). |

Because the switch lives in the provider, flipping it changes what *every* consumer sees at once —
that is the intended lever. There is deliberately no per-mod normal source: a second source per
consumer would be four copies of the same fallback logic, and A/B-ing one effect at a time is done
by toggling that effect, not its normal input.

**Open Normal Controls → Debug View** (needs Show Normals on):

| Mode | Shows |
|---|---|
| **Service Output** | Exactly what consumers get this frame. The old behaviour of this overlay. |
| **Authored** | The thin g-buffer normal. Black where the pixel has none. |
| **Reconstructed** | The 5-tap depth-gradient normal. Always available. |
| **Difference** | Angle between the two, 0–45° as black → blue → green → yellow → red. |
| **Coverage** | Green where an authored normal exists, red where it falls back. |

Modes 1–4 render **both** normals in the same frame, so they compare the two sources without
touching the Use Authored Normals switch. That costs a second full-size `rgba32float` target and a
second normal computation per pixel; it is allocated only while such a mode is selected and
released as soon as it is not. Mode 0 costs nothing extra, and with authored normals off *and* no
comparison view up, the provider does not even request the normal snapshot — so the A/B is an
honest frame-time comparison too.

## 2. Reading the Difference view (the one real risk)

The authored normal arrives rotated by GX's concatenated **model-view** matrix (`nrm_mtx`), while
the reconstruction derives its view-space normal from the camera service's `view_from_proj`. Both
are then rotated to world space by `world_from_view`. These *should* be the same view basis, but
that has never been checked on a GPU.

- **Dark with bright creases along triangle edges** — correct. That is exactly the faceting the
  authored normal removes: the two agree on flat spans and disagree at facet boundaries.
- **Broad yellow/red across whole surfaces** — a basis mismatch, not a smoothness difference. Fix
  it with **Authored Basis** (As-is / Flip Y / Flip Z / Flip Y and Z) and report which one landed;
  the winning flip then gets folded into the shader as the default.

Note the camera-facing guard (`dot(n, pos) > 0 → negate`) is applied to the authored normal too,
so a *global* sign error is largely self-correcting on front-facing surfaces — a Y flip or an axis
swap is the failure mode that would actually show.

## 3. Coverage and the fallback

The renderer gates the normal write on **depth-writing draws** (`depthCompare && depthUpdate`), not
on opaque-only. That deliberately matches the depth buffer: the same set of pixels the depth
reconstruction already covered, depth-writing water included. Additive/blended effects that do not
write depth have no authored normal — and they were garbage in the reconstruction too.

`a < 0.5` in the snapshot's alpha means "no authored normal", and those pixels reconstruct
individually. The Coverage view is the direct readout: expect green over world geometry and
characters, red on sky (which is invalid anyway and drawn black) and on effects. The fallback
should be seamless, not a visible seam.

## 4. Verification order

1. **Prereq.** Install the `platform-gbuffer-test` game build **and** fresh `.dusk` files as a
   matched pair, with **Use Authored Normals off**. Everything must look exactly as it did before
   and the log must be clean. Any missing or corrupted composite here means a mod pipeline is still
   declaring one color target (see §5).
2. **Basis.** Turn the toggle on, Show Normals on, Debug View = Difference. Expect dark with bright
   creases; see §2 if not.
3. **Coverage.** Debug View = Coverage. Confirm the fallback regions are the expected ones.
4. **The payoff.** Overlay off, VBAO/SSILVB on a low-poly rock face or a character: the faceting
   the reconstruction produced should be gone, with no blur pass involved.
5. **Shadows.** With the shadow mod's Normal Smoothing still enabled, confirm shadows are unchanged
   or better.
6. **Everything at once.** VBAO/SSILVB + Realtime Sun Shadows + Deferred Fog + SMAA together — each
   pushes a draw into the scene pass, so this is where a missed pipeline surfaces.
7. **Perf.** Frame time against the `platform-v2-test` baseline. Expect slightly more in the scene
   pass (one extra RGBA8 target), and considerably less once the normal-smoothing pass goes.

**Still to do, after verification** (deliberately not in the same push, so each reverts alone):

- Delete `realtime_sun_shadows/res/normal_smooth.wgsl` and its host code (a dense 32-tap separable
  bilateral — the single most expensive thing in the shadow chain). Its own header says it exists
  solely to smooth reconstruction faceting, and there is none left to smooth.
- Retune the shadow mod's slope-scaled bias and normal-offset magnitudes. They were tuned against
  *blurred* normals; authored normals are smooth but not blurred, so they keep real curvature the
  blur was flattening. Expect both to want to come down.

## 5. The prerequisite that touches every mod

**A WebGPU render pass with two color attachments requires every pipeline drawing into it to
declare two color targets.** With the normal buffer on, the scene pass has two — so every pipeline
handed to `push_draw` declares a second target in `g_deviceInfo.normal_format` with
`writeMask = None`, or Dawn rejects the draw on attachment count. Every stage that pushes draws
(`SCENE_BEGIN`, `SCENE_AFTER_TERRAIN`, `SCENE_AFTER_OPAQUE`, `FRAME_BEFORE_HUD`,
`FRAME_AFTER_HUD`) lands inside that pass; there is no exempt stage.

Done at all six sites: VBAO and SSILVB composites (blend + debug), SMAA neighborhood blend,
Graphics Hub's normal debug overlay and deferred fog fullscreen, Realtime Sun Shadows composite.
The WGSL is unchanged — a fragment shader returning a single `@location(0)` value is valid against
a pipeline whose second target is masked.

Offscreen passes from `create_pass` (shadow-map replays, the fog config-ID replay) stay
single-target: they render with the game's own pipelines, which the renderer builds from the
current pass.

**Any new mod pipeline recorded into the scene pass must do the same.** Keep the
`!= WGPUTextureFormat_Undefined` guard rather than hardcoding two targets — that is what lets the
same binary run on both platforms.

## 6. Platform pin and rollback

`CMakeLists.txt` pins `DUSKLIGHT_VERSION = b96bf5ec01…` (tip of
`claude/thin-gbuffer-authored-normals-wgqupt` in `dusklight-ao`) and `DUSKLIGHT_SDK_STUB_URL` at the
`platform-gbuffer-test` release. The base game code is identical to `platform-v2-test` (both are
pristine upstream Dusklight `76b56cd8`); only the renderer change and the SDK header additions
differ, so the game-linked mods hook the same functions.

To roll back: set both knobs back to `9361fbd9ea…` / `platform-v2-test`, let CI rebuild, and
install those `.dusk` files together with that game build. The stable release is untouched and
still published. **No source change is needed** — every authored-normal path is guarded on
`GfxDeviceInfo::normal_format`, which that platform reports as `Undefined`, so the provider simply
reconstructs exactly as before and the second color target is never declared. The game build and
the `.dusk` files are always a matched pair, in either direction.

## 7. API surface used

All fields are appended to existing structs and `struct_size`-guarded.

```c
GfxDeviceInfo::normal_format      /* RGBA8Unorm, or Undefined when the buffer is off */
GfxResolveDesc::normal            /* request the per-frame normal snapshot */
GfxResolvedTargets::normal        /* single-sample snapshot, frame-valid, may be NULL */
GfxResolvedTargets::normal_format
GfxDrawContext::normal_format     /* 2nd target format of the pass a draw lands in */
```

Snapshot contents: `rgb` = `normalize(mv_nrm) * 0.5 + 0.5`, the **view-space** authored normal;
`a` = 1.0 when the draw supplied a normal attribute, 0.0 otherwise. Decode with
`normalize(texel.xyz * 2.0 - 1.0)` — **always renormalize**, since interpolation, 8-bit
quantization and any MSAA resolve all denormalize it. The attachment is cleared to `(0,0,0,0)` on
the frame's first EFB pass and loaded on resumed segments, so a snapshot taken at
`SCENE_AFTER_OPAQUE` holds every opaque draw so far.

Two renderer paths remain untested (both flagged in `dusklight-ao/docs/thin-gbuffer-normals.md`
§12): **MSAA** (the normal target resolves like the color target; renormalizing on read covers most
of it, slight silhouette error is expected) and **precision** (if RGBA8 banding shows on smooth
surfaces under strong AO, the fix is `NormalBufferFormat` → `RGB10A2Unorm`, which keeps the
validity channel — that is an `aurora-ao` change, so report it rather than working around it here).
