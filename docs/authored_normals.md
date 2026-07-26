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
5. **Shadows.** Set Normal Smoothing to **0** (see §4a — that is what binds the authored normal
   unblurred) and check the Receiver Normal debug view, then compare shadow quality against the
   old default of 4.
6. **Everything at once.** VBAO/SSILVB + Realtime Sun Shadows + Deferred Fog + SMAA together — each
   pushes a draw into the scene pass, so this is where a missed pipeline surfaces.
7. **Perf.** Frame time against the `platform-v2-test` baseline. Expect slightly more in the scene
   pass (one extra RGBA8 target), and considerably less once the normal-smoothing pass goes.

## 4a. Realtime Sun Shadows had a second normal path

Found during the first in-game test: the shadow mod appeared not to use authored normals at all —
its Receiver Normal debug view still showed reconstructed normals and the Use Authored Normals
toggle made no visible difference, while SSILVB responded correctly.

The cause was **`Normal Smoothing` gating whether the provider was consulted at all**, not just
whether its output was blurred:

```cpp
const bool normalsWanted = smoothing > 0 && (...);   // ← the bug
```

At `Normal Smoothing = 0` the mod never called `get_frame`, bound no normal buffer, and
`world_normal_at` in `shadow.wgsl` fell through to its **inline 1px cross reconstruction** — a
second depth→normal path living inside the shadow composite that the provider work never touched.
And at the default of 4, the dense 32-tap bilateral blur flattens most of the authored-vs-
reconstructed difference anyway, so the toggle looks inert there too. Both readings of the symptom
had the same root cause.

Now the provider is consulted at **every** setting and Normal Smoothing is only post-processing:

| Normal Smoothing | Receiver normal |
|---|---|
| **0** | The provider's buffer bound directly — authored, unblurred, zero extra cost. |
| **> 0** | The same buffer, blurred by `normal_smooth.wgsl`. |
| *(provider unavailable)* | Inline 1px cross — now a genuine last-resort fallback only. |

Every failure path inside the normal setup degrades to the raw provider normal rather than to the
inline reconstruction, and the ping-pong blur targets are released when smoothing is 0.

**So: to see authored normals in the shadow mod, set Normal Smoothing to 0.** That is also the A/B
for whether the blur can be deleted outright — which is the next item.

### …and the gate was still incomplete (second round)

After the above, the Receiver Normal view showed authored normals but the **Shadow Factor** view
still did not react to the toggle at all. Two defects, both variants of the same mistake:

1. **The host gate did not mirror the shader's own condition.** `shadow.wgsl` reads the normal when
   `rpdb_enabled || slope_bias || normal_offset || attached_shadows`. The host only bound the
   buffer for `slopeBias > 0 || normalOffset > 0` — omitting **Receiver-Plane Bias** and
   **Attached Shadows**, which are *both on by default* and both read `n`. With Slope Bias and
   Normal Offset turned down to 0 — the natural setup once Receiver-Plane Bias replaces Slope
   Bias — the shader read `n`, found nothing bound, and fell through to the inline cross. The
   shadows ran on facet normals with no indication anywhere.
2. **The debug view forced the buffer on.** The gate began `debugMode == 13 || …`, so the Receiver
   Normal view bound the provider normal *even when the shadow path would not*. That is why it
   looked correct while the shadow term was unaffected — the view was reporting a normal the
   shadows were not using.

Both are fixed: the gate is now `normalConsumers && (mapReady || debugMode == 13)` with
`normalConsumers` mirroring the shader guard exactly, and the debug view no longer forces
anything. If every normal consumer is genuinely off, the view now honestly shows the inline
fallback.

**The rule this keeps violating, stated once:** a host-side "do I need to bind X" gate that
duplicates a shader-side "do I use X" condition will drift, and the failure is silent because the
shader has a fallback. When the two must be mirrored, say so at both ends — and never let a debug
view widen the gate, or it stops describing the thing it is supposed to diagnose.

### The same trap in VBAO's debug view

Swept for the same class of bug afterwards and found one more, in the debug view rather than in the
effect: **VBAO's "Normals" view always reconstructed from depth**, even while the AO pass itself was
consuming the provider's authored normal — its help text claimed it showed "the normals the
occlusion pass consumes", which was no longer true. It now makes the same choice `vbao.wgsl` makes
(provider normal when flags bit 3 is set, inline reconstruction otherwise), so it cannot claim a
source the AO is not using.

Checked and clean: SSILVB (hard service dependency, no inline reconstruction anywhere) and SMAA
(no normal debug view; its normal is an optional edge-detection input that stands in with the
colour snapshot). The only remaining inline depth→normal reconstructions in the repo are VBAO's
fallback for when the provider is absent, the shadow composite's last-resort cross, and the
provider's own — which is the point.

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

### MSAA silhouettes — resolved by rejecting blended texels

The renderer resolves the normal target with a **hardware MSAA resolve** (`g_normalBufferResolved`,
`aurora-ao/lib/webgpu/gpu.cpp:1116`, wired at `common.cpp:473`), which *averages* samples. For an
encoded normal that is not a harmless approximation — it produces directions that correspond to no
surface at all:

| case | decoded value | length |
|---|---|---|
| one surface, full coverage | `n` | 1 |
| partial coverage vs cleared samples (fraction `k`) | `k·n + (k−1)` | far below 1 |
| silhouette shared by two surfaces | `(n1 + n2) / 2` | `cos(½ angle)` |

The design note in §12 of the renderer doc predicted "slight silhouette error… acceptable". In
practice it was neither slight nor confined to appearance:

- a **wrong-coloured rim** on every silhouette in the normal debug view (partial-coverage texels
  get flipped by the camera-facing guard, landing on a roughly camera-facing direction);
- the shadow mod's **attached-shadow term failing** on thin back-lit features — a nose tip, boot
  and tunic edges — because `n·L` computed from a blended normal can read as sun-facing, switching
  the term off exactly where it is needed.

**The validity alpha does not catch this.** It averages to `k`, so any pixel over half covered
still reads valid, and a two-surface pixel reads a fully confident `1.0`. That is why Coverage
showed green across the whole screen while the normals were wrong.

`reconstruct.wgsl` now uses the **decoded length as a confidence signal**: a texel written by one
surface decodes to unit length (8-bit quantization moves it under 0.01), so anything below **0.92**
is a resolve average and is handed to the depth reconstruction instead, which is built for
silhouettes. Genuine curvature is untouched — adjacent samples a few degrees apart still measure
~0.999. With MSAA off every covered texel measures 1.0, so the check is inert.

After this change, silhouette rims show **red in the Coverage view** — that is the fallback working,
not a regression. The Depth to Normal status line reports the sample count so the MSAA state is
visible without leaving the game.

If the remaining reconstructed rim is objectionable, the renderer-side fix is to skip the hardware
resolve for normals and take a single sample (or the sample whose depth matches the resolved
depth) instead of averaging. That is an `aurora-ao` change.

**Precision** remains untested: if RGBA8 banding shows on smooth surfaces under strong AO, the fix
is `NormalBufferFormat` → `RGB10A2Unorm`, which keeps the validity channel — also an `aurora-ao`
change, so report it rather than working around it here.
