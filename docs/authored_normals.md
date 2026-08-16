# Authored normals (scene normal buffer) — consuming them, and how to A/B them

**Status:** landed in the mods. Partly verified in-game — see §0. The platform side is done,
merged and published (`platform-normals-test`).

> **Cold-start readers:** read §0 first. It is the state of play — what is done, what is confirmed
> in-game, what is still open, and which debug views answer which question. §8 is the list of
> findings that each cost hours; read it before forming any theory about normals or shadows, since
> most of the obvious theories were tried and were wrong for reasons that are now written down.
> §9 answers "could a mod build this buffer instead, without the aurora change?" — including the
> one route that genuinely would work, why it is still the wrong build, and the upstream-drift
> notes (an `GfxDeviceInfo` ABI collision) that matter on the next re-platform.

The game's forward renderer can write its own **authored, interpolated vertex normals** into a
second RGB10A2 color attachment on the scene (EFB) pass, and the mod API exposes a per-frame
snapshot of it. **Graphics Hub / Depth to Normal** consumes that snapshot instead of reconstructing
normals from the depth buffer, and because every other mod reads normals through the
`dev.automata.depth_to_normal` service, all of them (VBAO, SSILVB, Realtime Sun Shadows, SMAA)
inherit it with **zero changes of their own**.

**It is off by default.** The game ships the buffer switched off — **Video → Rendering → Scene
Normal Buffer**, applied on the next launch — because it costs a render target and a write per
covered fragment and nothing reads it unless a mod does. Until it is on, every mod here behaves
exactly as it did before: the provider reconstructs from depth for every pixel and says so in its
status line. It is also unavailable in compatibility mode (the D3D11 and OpenGL ES fallbacks),
where the renderer disables it whatever the setting says.

Why it matters: a depth-gradient normal is a cross product of screen-space position deltas, i.e.
the flat face normal of each rasterized triangle — faceting is inherent to the method. Authored
normals are smooth by construction, which removes the faceting *at the source* rather than blurring
it away afterwards. See `dusklight-ao/docs/thin-gbuffer-normals.md` for the renderer-side design.

---

## 0. State of play (read first)

**Platform:** `DUSKLIGHT_VERSION = 5ded001`, `DUSKLIGHT_REPOSITORY` → `automata-rtx/dusklight-ao`,
`DUSKLIGHT_SDK_STUB_URL` → `platform-normals-test`. The user must run the `win32-msvc-x86_64` build
from that release; game build and `.dusk` files are always a matched pair, in both directions (see
§6 for rollback). **Then turn on Video → Rendering → Scene Normal Buffer and restart** — nothing in
this document is observable until that is done.

That pin is upstream `c880d46f` plus **GfxService 1.3**, over aurora `cf3ffc9` plus the normal
attachment. Note the release tag `platform-normals-test` is republished on every push to the
platform branch, so the *URL* is stable while its assets are not.

**The fork delta is now two fields wide.** Upstream shipped its own scene-target-layout API in
#2305 (GfxService 1.2 — `GfxRenderTargetLayout`, `get_scene_target_layout`,
`gfx_init_color_target_states`, `GfxDrawContext::layout`, and the `GfxAttachmentSemantic` tags), so
the hand-rolled `GfxPassTargets` vocabulary our fork used to carry was **deleted rather than
merged**. What remains fork-local is 1.3's `GfxResolveDesc::normal` → `GfxResolvedTargets::normal`.
`GfxDeviceInfo::normal_format` is gone entirely — see §7.

### What landed, in order

| Commit | What |
|---|---|
| `9cd2ca9` | Second color target on every mod pipeline recorded into the scene pass (§5). Six sites. |
| `731dd44` | Platform pinned to `platform-gbuffer-test`. |
| `83e39f1` | Provider consumes authored normals + the A/B toggle, comparison debug views, basis diagnostic (§1). |
| `c27ab29` | Shadow mod consults the provider at **every** Normal Smoothing setting (§4a). |
| `44d4e18` | VBAO's "Normals" debug view shows what the AO actually consumes (§8.3). |
| `773e034` | Shadow mod's normal gate mirrors the shader's own condition (§4a, second round). |
| `915f92a` | **Boot-scene crash fix**: null stage info before `dKy_Indoor_check` (§8.4). |
| `311aa47` | Crash-symbolization runbook in `docs/mod-api-notes.md`. |
| `db1dff2` | Authored normals no longer flipped at silhouettes (§2a). |
| `5300789` | Bias uses the geometric face normal; terminator uses the shading normal (§8.6). |
| `9af4701` | `sin`-scaled normal offset (Holbert) + **Shadow Terms** debug view (§8.7). |
| `317111b` | Map comparison faded across the terminator band (§8.7). |
| `aa2c723` | **Receiver-plane fractional-sampling bias capped** (§8.8). |
| `38386e4` | Normal Smoothing pass deleted outright (§8.9). |
| `e50a1a9` | Re-platformed onto upstream `0fc05028`, which has no normal buffer — authored normals off across the board, provider reconstructing every pixel. |
| `dcdaa23` | Re-platformed onto `platform-normals-test`, restoring authored normals; all six scene-pass pipelines moved to `get_pass_targets`, and the `GfxDrawContext::normal_format` guard deleted (§5). |
| `ca6b73a` | **Every camera-facing flip on an authored normal deleted** — provider (was 0.5) and VBAO/SSILVB (were −0.15). Re-pinned to the rebased platform, which also drops aurora's enlarged streaming buffers — harmless, upstream had already raised its own (§2a). |
| `3cbab91`–`8cc46b9` | AO occlusion hemisphere built from geometry, not the shading normal; the rejection plane made a 4-tap ±1 in both mods (§8.11, §8.11a). |
| `b426c4d` | Shadow map and `n·L` terms combined by multiplying visibilities instead of `max` — fixes the terminator glint (§8.12). |
| *(this change)* | Re-pinned to **GfxService 1.3** on rebased upstream. Our scene-layout fork deleted in favour of upstream's; `normal_format` accessor removed; `has_normal_attachment` is the new "does this build have authored normals" (§5, §7). |

### Confirmed in-game by the user

- Authored normals reach **SSILVB** and visibly change its output.
- Authored normals reach the **shadow mod** (Receiver Normal view shows them) after `773e034`.
- **Coverage is green across the whole screen** — every visible pixel has an authored normal. Any
  theory that depends on missing coverage is dead.
- The normal buffer itself is **smooth** — confirmed against a faceted Shadow Factor in the same
  frame, which is what proved the faceting was downstream of the normal (§8.6).
- The startup crash is gone (later builds run).
- The **normal debug view is correct** after the camera-facing flips came out (`ca6b73a`, §2a).
- **The terminator glint is fixed** (`b426c4d`, §8.12) — reported from Debug View 15 as a
  specular-looking stripe where the red map term handed over to the green `n·L` term, and confirmed
  gone. That one was found *from the debug view alone*, which is the workflow §8.12 documents.

### NOT yet verified — the open question

The back-lit-character work from `5300789` onward is still **unverified in-game**. The last user
report on it showed wrongly-lit patches on Link's boots, torso and lower tunic when back-lit. Since
then several changes landed that each plausibly address it, the strongest being the
**fractional-bias cap** (`aa2c723`, §8.8) — that one is arithmetic, not inference: the term was
contributing several hundred world units of flat bias against a ~150-unit-tall character. Note the
glint fix above is a *different* defect that happened to live in the same handover; it does not
close this item.

**Next step:** Shadow Factor + Shadow Terms on back-lit Link, same frame. Then the bias retune
(below).

### Open items

1. **Bias default retune.** `Normal Offset`'s units changed with `sin` scaling (`9af4701`) — it is
   now the grazing-incidence maximum, not a flat amount — and it is now the *only* normal-derived
   acne control since Normal Smoothing is gone. Needs a screenshot-driven pass, not guessed values.
2. If flat sunlit ground shows acne, `rpdb_max` (currently `0.02`) is the knob; the fractional cap
   `kMaxFractionalBias` (`0.001`) is deliberately tight.
3. MSAA handling if MSAA is ever enabled (§8.5). *(Normal-target precision is done — the buffer is
   `RGB10A2Unorm`, ten bits per axis, so the RGBA8 banding this item was raised against cannot
   occur.)*
4. **Everything in §0 "Confirmed in-game" was confirmed on the retired `platform-gbuffer-test`
   platform**, whose buffer was RGBA8 and whose renderer was a different fork. The findings should
   carry — the encoding, coverage rule and basis are the same — but the first run on
   `platform-normals-test` is a re-confirmation, not a regression check. Start with Coverage (is it
   still green everywhere?) and Difference (§2), in that order.
5. **SMAA's `Normal Threshold` default (10%) was tuned against faceted normals**, where a low value
   lights up every facet boundary on curved low-poly geometry. With authored normals those interior
   steps are gone, so lower values should now be usable and catch real creases this default misses.
   Worth a screenshot pass; the control is live, so no rebuild is needed to explore it.

### Debug views — which question each answers

| View | Where | Answers |
|---|---|---|
| **Coverage** | Graphics Hub → Normal Controls | Does this pixel have an authored normal at all? (green/red) |
| **Authored / Reconstructed / Difference** | Graphics Hub | What do the two sources look like, and where do they disagree? |
| **Receiver Normal** (13) | Shadow mod | The normal the shadow bias *actually* uses this frame, as bound. |
| **Shadow Terms** (15) | Shadow mod | **Which term is shadowing each pixel.** Red = shadow map, green = attached `n·L`, yellow = both, **black = neither, i.e. reported fully lit**. This is the view that separates "the map missed an occluder" from "`n·L` misread the surface" — two bugs with identical symptoms. |
| **Shadow Factor** (2) | Shadow mod | The combined result only. **Cannot** distinguish the two failure modes; do not diagnose from it alone (§8.7). |


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

Note that no camera-facing guard is applied to the authored normal (see §2a), so a *global* sign
error would show up plainly rather than being masked on front-facing surfaces.

## 2a. Never flip an authored normal toward the camera — at any threshold

**This is the single most expensive lesson in this document. It was got wrong three times, each
time by weakening the test rather than deleting it.**

The reconstruction ends with `if dot(n, pos) > 0 { n = -n }`, forcing the normal toward the camera.
That is *required* there and nowhere else: a cross product of screen-space deltas has an *arbitrary
sign*. Nothing in the depth buffer says which way the surface faces, so the sign has to be invented,
and "toward the camera" is the only sane choice.

An authored normal is not in that position. It **already has a sign** — the one the artist gave it,
transformed by the model-view matrix — and the renderer writes it per draw, so a reverse pass over
two-sided geometry supplies the normals for the side it actually draws. There is no ambiguity left
for a guard to resolve, so a guard can only ever destroy information.

### Why any threshold seams

`dot(n, view_ray)` is **not a property of the surface**. The ray direction sweeps across the screen
under perspective, so on a large surface seen at a grazing angle the product crosses any fixed
threshold *along a line*, and every pixel past that line is negated. The result is a **hard seam
across geometrically flat ground**, worst where ground planes run to the horizon.

Moving the threshold moves the seam. It does not remove it.

### The three attempts

| Threshold | Reasoning at the time | What it actually did |
|---|---|---|
| `dot > 0` | "Copy the reconstruction's guard." | Negated the whole silhouette band. A smooth normal field crosses `dot = 0` *at the visual silhouette by construction*, and on low-poly geometry travels well past perpendicular first — an 8-sided cylinder reaches ~22°, `dot ≈ 0.37`. Showed as a wrong-coloured band hugging every silhouette that *widened and narrowed with camera angle*, and as the shadow mod's `n·L` inverting on curved back-lit features (nose tip, boot and tunic edges). |
| `dot > 0.5` | "Spare the silhouette band; flip only what is *clearly* inverted." | Same bug, seam relocated. Smaller region negated, still bounded by a hard view-dependent line. |
| *(none)* | An authored normal has no sign ambiguity. | Correct. |

The consumers repeated the mistake independently: **VBAO and SSILVB each re-applied their own guard
at `dot(n, view_vec) < -0.15`**, nominally for double-sided foliage. That fires *earlier* than the
provider's own 0.5, so it planted the seam nearer the horizon — precisely where ground planes are
widest on screen. It also never fired on the reconstruction path (already camera-facing), so despite
the general-sounding comment it only ever acted on authored normals.

All of them are now deleted. The four surviving flips in the tree are all on cross-product
reconstructions, which is the one case that needs them:
`graphics_hub/reconstruct.wgsl` `reconstruct_normal`, `vbao/vbao.wgsl` and `vbao/composite.wgsl`
`reconstruct_normal`, and `realtime_sun_shadows/shadow.wgsl` `geometric_normal_at`.

### What about genuinely back-facing geometry?

The case the guards were written for largely does not arise: TP draws two-sided geometry as a
reverse pass, and that pass writes the normals for the side it draws, so a visible back face already
has a normal facing the viewer. Where single-pass two-sided geometry does leave a normal pointing
away, AO integrates over a hemisphere facing away from the screen on those pixels. That is the
accepted trade — it is local and rare, where the seam was global and on the most visible surface in
the game. SSILVB's *emitter* term shows the right way to handle it without a sign change: it
`clamp`s `dot(n, -l)` to `[0,1]`, so a back-facing emitter simply contributes nothing.

**Rule for the service:** it returns the surface direction with the game's own sign. Consumers take
it as given. A consumer that "corrects" it against the view re-creates this bug.

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

1. **Prereq.** Install the `platform-normals-test` game build **and** fresh `.dusk` files as a
   matched pair. Start with **Video → Rendering → Scene Normal Buffer OFF** — the shipping default.
   Everything must look exactly as it did before and the log must be clean; Graphics Hub's status
   line should tell you to turn the buffer on. This step alone confirms the re-platform, since the
   symptom it replaces was mods failing to load outright.
2. **Buffer on.** Turn on the Video setting, **restart**, and confirm "Use Authored Normals" is no
   longer greyed out. Leave it **off** for one more pass: everything must still look unchanged. Any
   missing or corrupted composite *here* — with the pass now carrying two attachments — means a mod
   pipeline is not following `get_pass_targets` (see §5). This is the step that catches it.
3. **Basis.** Turn the toggle on, Show Normals on, Debug View = Difference. Expect dark with bright
   creases; see §2 if not.
4. **Coverage.** Debug View = Coverage. Confirm the fallback regions are the expected ones.
5. **The payoff.** Overlay off, VBAO/SSILVB on a low-poly rock face or a character: the faceting
   the reconstruction produced should be gone, with no blur pass involved.
6. **Shadows.** Check the Receiver Normal debug view, then compare shadow quality against the
   buffer-off run.
7. **Everything at once.** VBAO/SSILVB + Realtime Sun Shadows + Deferred Fog + SMAA together — each
   pushes a draw into the scene pass, so this is where a missed pipeline surfaces.
8. **Perf.** Frame time with the buffer off vs on — the same install, so it is a clean A/B. Expect
   slightly more in the scene pass (one extra RGB10A2 target and a write per covered fragment), and
   less in the provider, which drops eight depth taps and two unprojections per pixel wherever an
   authored normal covers it.

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

- ~~Delete `realtime_sun_shadows/res/normal_smooth.wgsl`~~ — **done.** The shader, both blur
  pipelines, the two full-res `rgba32float` ping-pong targets and their retire scheme, the compute
  payload and type, and the `normalSmooth` config var and UI control are all gone. The shadow mod
  binds the provider's normal directly. Its own header said it existed solely to smooth
  reconstruction faceting; authored normals have none, so it was pure cost — a dense 32-tap
  separable blur over the full screen — and it flattened the real curvature the bias could use.
- Retune the shadow mod's slope-scaled bias and normal-offset magnitudes. They were tuned against
  *blurred* normals; authored normals are smooth but not blurred, so they keep real curvature the
  blur was flattening. Expect both to want to come down.

## 5. The prerequisite that touches every mod

**A WebGPU render pass with two color attachments requires every pipeline drawing into it to
declare two color targets.** With the normal buffer on, the scene pass has two, or Dawn rejects the
draw on attachment count. Every stage that pushes draws (`SCENE_BEGIN`, `SCENE_AFTER_TERRAIN`,
`SCENE_AFTER_OPAQUE`, `FRAME_BEFORE_HUD`, `FRAME_AFTER_HUD`) lands inside that pass; there is no
exempt stage.

**Ask the service for the layout; do not rebuild it.** `get_scene_target_layout(…)` returns a
`GfxRenderTargetLayout` — one entry per attachment, each tagged with a `GfxAttachmentSemantic` — and
the SDK's inline `gfx_init_color_target_states` turns that into a `WGPUColorTargetState[]` with
every attachment the mod does not own already write-masked off. All six sites go through
`gfx_compat::scene_pass_layout` (see `docs/normal_buffer_portability.md` §3): VBAO and SSILVB
composites (blend + debug), SMAA neighborhood blend, Graphics Hub's normal debug overlay and
deferred fog fullscreen, Realtime Sun Shadows composite. The WGSL is unchanged — a fragment shader
returning a single `@location(0)` value is valid against a pipeline whose other targets are masked.

Two earlier versions of this section are worth remembering, because each shipped a silent failure.
The first told you to assemble the layout by hand from `g_deviceInfo.normal_format`, which is a copy
of the renderer's own logic and wrong the moment the pass changes shape; it came paired with a
per-draw guard comparing `GfxDrawContext::normal_format` against the device's, on a field the SDK
had stopped carrying, so the guard fired unconditionally and disabled every composite. The second
named upstream-1.2-era symbols (`get_pass_targets`, `GfxPassTargets`, `GFX_MAX_COLOR_TARGETS`) that
upstream then **renamed**; the compat header's `#if` went false, the whole tree compiled cleanly,
and every pipeline reverted to one colour target. Both are `normal_buffer_portability.md` §2 now,
and the second is why that header fails the build instead of guessing.

Offscreen passes from `create_pass` (shadow-map replays, the fog config-ID replay) stay
single-target: they render with the game's own pipelines, which the renderer builds from the
current pass.

**Any new mod pipeline recorded into the scene pass must call `scene_pass_layout` too.**

## 6. Platform pin and rollback

`CMakeLists.txt` pins `DUSKLIGHT_VERSION = 5ded001…` (tip of
`claude/dusklight-thin-gbuffer-normals-l4l9dc` in `dusklight-ao`), `DUSKLIGHT_REPOSITORY` at that
fork and `DUSKLIGHT_SDK_STUB_URL` at the `platform-normals-test` release. The base game code is
upstream Dusklight `c880d46f`; the renderer change and the SDK header additions sit on top of it.

**Rolling back to upstream is not free any more.** The base carries an upstream **game-service
major-version bump**, so a mod built against the older `0fc05028` SDK is refused by this host and a
mod built against this SDK is refused by that one. Rollback therefore means moving the pin *and*
rebuilding *and* installing the matching game build — the same matched-pair rule as always, but with
no overlap window where one set of `.dusk` files works on both. No **source** change is needed
either way: every authored-normal path degrades through `common/gfx_normal_compat.h`, and the
"is there a normal buffer" question is answered by the scene layout's semantic tags, which simply
list no `GFX_ATTACHMENT_NORMAL` on a base without one.

Note also that turning the buffer off in Video settings is a much cheaper A/B than rolling the
platform back, and it is the one to reach for first when deciding whether authored normals are the
cause of something.

## 7. API surface used

Two groups, and the distinction matters when re-platforming: **upstream** (GfxService 1.2, present
on any current base) and **fork-local** (GfxService 1.3, the entire remaining delta).

```c
/* upstream 1.2 — scene pass layout, for building pipelines */
GfxService::get_scene_target_layout   /* -> GfxRenderTargetLayout */
GfxRenderTargetLayout::color_attachments[i].semantic   /* GFX_ATTACHMENT_NORMAL lives here */
gfx_init_color_target_states          /* inline SDK helper; write-masks off what you don't own */
GfxDrawContext::layout                /* the same layout, inside a draw callback */

/* fork-local 1.3 — the normal snapshot */
GfxResolveDesc::normal                /* request the per-frame normal snapshot */
GfxResolvedTargets::normal            /* single-sample snapshot, frame-valid, may be NULL */
```

**To ask whether this build has authored normals, scan the layout for a `GFX_ATTACHMENT_NORMAL`
semantic** — `gfx_compat::ScenePassLayout::has_normal_attachment` does exactly that. There is no
`normal_format` field on any struct any more. Two platforms had one and each produced a distinct
silent failure: an offset collision with upstream's `WGPUInstance`, and a per-draw guard that
compared a compile-time-absent field against a live value and disabled every composite. See
`docs/normal_buffer_portability.md` §2.1.

`GfxResolveDesc::normal` is the one field `struct_size` cannot police: it landed in the struct's
existing tail padding, so `sizeof(GfxResolveDesc)` is **unchanged** between 1.2 and 1.3 and the flag
is invisible to the usual check. The host therefore honours it only for callers that also pass a
1.3-sized `GfxResolvedTargets` to receive the view in — so a 1.2 mod's uninitialised padding can
never request a snapshot it has nowhere to put. Initialising both from their `GFX_*_INIT` macros, as
the provider does, satisfies that automatically.

Snapshot contents: `rgb` = `normalize(mv_nrm) * 0.5 + 0.5`, the **view-space** authored normal;
`a` = 1.0 when the draw supplied a normal attribute, 0.0 otherwise. Ten bits per axis, which is what
keeps low-curvature surfaces from banding; the two alpha bits only ever carry the flag. Decode with
`normalize(texel.xyz * 2.0 - 1.0)` — **always renormalize**, since interpolation, quantization and
any MSAA resolve all denormalize it. The attachment is cleared to `(0,0,0,0)` on the frame's first
EFB pass and loaded on resumed segments, so a snapshot taken at `SCENE_AFTER_OPAQUE` holds every
opaque draw so far.

Coverage is exactly the depth buffer's: a draw writes a normal if and only if it writes depth. So a
normal snapshot and a depth snapshot always describe the same surface, and effects that only blend
over the scene (particle billboards, the game's projected shadow quads) cannot contaminate it.

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
  decode to a direction belonging to no real surface; at the time the camera-facing guard then
  flipped many of them as well, compounding it — that guard is gone, §2a, but the blend it was
  reacting to is what this section is about and the length test is what fixes it);
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

---

## 8. Findings that cost the most time

Each of these blocked progress for a long stretch. They are recorded with the **wrong theory** as
well as the right one, because the wrong theories were plausible and will re-suggest themselves.

### 8.1 A debug view is only trustworthy if it reads the buffer the effect reads

Hit **three separate times** in this work. Every time, a view showed one thing while the effect
consumed another, and the mismatch sent the investigation in the wrong direction for rounds:

1. The shadow mod's Receiver Normal view bound the provider normal *because the view asked for it*,
   while the shadow term itself had no buffer bound and silently used an inline reconstruction.
2. VBAO's "Normals" view always reconstructed from depth, while the AO pass consumed the provider —
   and its help text claimed it showed "the normals the occlusion pass consumes".
3. The Shadow Factor view shows `max(map, attached)` only. Two opposite bugs — the map missing an
   occluder, and `n·L` misreading a surface — produce *identical* bright pixels in it. Rounds were
   spent alternating between those two explanations before **Shadow Terms** (view 15) was built to
   separate them.

**Rule:** before diagnosing from a debug view, verify it samples the same resource the effect does,
under the same gate. If it forces a resource on that the effect wouldn't have, it is lying. And if a
view shows a *combined* result, it cannot diagnose which input failed — build the view that splits
them before theorising, not after.

### 8.2 Host-side "do I need to bind X" gates that duplicate shader-side "do I use X" conditions

`shadow.wgsl` reads the receiver normal when
`rpdb_enabled || slope_bias || normal_offset || attached_shadows`. The host bound the buffer for a
*narrower* condition, and `world_normal_at` has a silent fallback — so the mismatch produced no
error, just quietly wrong shading. This happened twice in a row:

- First the gate required `Normal Smoothing > 0`, so at 0 the provider was never consulted at all.
- Then the corrected gate still omitted **Receiver-Plane Bias** and **Attached Shadows**, which are
  *both on by default* and both read the normal. With Slope Bias and Normal Offset at 0 — the
  natural setup once Receiver-Plane Bias replaces Slope Bias — the shadows ran on facet normals.

Both conditions now carry a `KEEP MIRRORED` comment pointing at each other. **Adding a normal
consumer to the shader means adding it to `normalConsumers` in `mod.cpp`.**

Note the *other* capability gates in the shadow mod (`map_enabled`, `link_enabled`,
`contact_enabled`) are safe by construction: the host sets each uniform flag from the same variable
that chose what to bind, so there is one source of truth and nothing to drift. The receiver normal
was the only one where the shader re-derived the condition independently.

### 8.3 Symbolize crashes immediately; do not infer them from source

A startup crash cost two wrong "fixes" before the binary was consulted. The platform release ships
`debug.7z` containing `dusklight.pdb`; `llvm-symbolizer --inlines` turned the backtrace into an
exact inline chain in one step. Full runbook: `docs/mod-api-notes.md` → "Symbolizing a game crash".

The first wrong fix guarded `indoor_blocked()` with `draw_lists_ready()` on the assumption that it
meant "a scene exists". **It does not** — the draw lists are already populated on the logo scene
while the stage info is still null. Two independent pieces of state; the guard was inert.

### 8.4 Game accessors that are unguarded before a stage loads

`dKy_Indoor_check()` → `dStage_stagInfo_GetSTType(getStagInfo())` is
`return (pstag->field_0x0c >> 16) & 7;` with **no null check** (`d_stage.h`).
`dComIfGp_getStage()->getStagInfo()` is null until a stage loads, so it faults at address `0xc` —
matching `field_0x0c` exactly. The game never trips this because nothing of its own asks whether it
is indoors that early; a mod stage hook does, on the first frame the window appears.

**A fault address under ~0x100 is a null dereference at that struct offset.** Match it against the
header's field offsets to identify the object immediately.

### 8.5 MSAA is OFF — do not build theories on it

A silhouette rim artifact was attributed to hardware MSAA resolve averaging normals. **Wrong.**
Dusklight never assigns `AuroraConfig::msaa`, and `aurora-ao/lib/aurora.cpp:116` defaults `0` → `1`.
`g_normalBufferResolved` is therefore never created. The length-based rejection added for it
(`reconstruct.wgsl`, threshold 0.92) is **inert** and kept only as a cheap robustness guard should
MSAA ever be enabled.

The distinguishing evidence was the user's: the artifact's **extent changed with camera position and
aim**. An MSAA resolve artifact would be a fixed ~1px rim. A view-dependent band is the signature of
a `dot(n, view) > 0` region — which led to 8.6.

### 8.6 The two normals do different jobs — never feed one to both

This is the single most important structural finding in the shadow mod.

- **Shading normal** (authored, smooth) — the `n·L` terminator, Attached Shadows, and the
  normal-offset receiver. Lighting quantities.
- **Geometric face normal** (from the depth buffer, `geometric_normal_at`) — Receiver-Plane Bias and
  the slope-bias `tan_t`. These solve the receiver's depth gradient *in light space*, a property of
  the triangle the pixel physically sits on.

Feeding the smooth normal to the bias tilts the comparison plane away from the real triangle by the
shading-vs-face angle, so every facet gets a systematically wrong bias — **per-facet patches in the
Shadow Factor with a perfectly smooth normal buffer**. That contradiction (smooth normals, faceted
shadows, same frame) is what proved it.

Authored normals *exposed* this rather than causing it: a depth-reconstructed normal **is** the face
normal, so the two uses coincided and the bug could not show. `normalSmooth` was an earlier partial
workaround from the same confusion — it traded bias banding at facet edges for bias error inside
facets, which is why no value of it ever looked right.

**Related:** the camera-facing guard was the same shape of error, twice over — a test that is right
for a face normal and wrong for an authored one, then "fixed" by retuning its threshold instead of
removing it. It is gone entirely (§2a).

### 8.7 Acne and light leaks are the same failure with opposite sign

A shadow-map comparison is only trustworthy where the surface faces the light. As it turns edge-on,
one shadow texel spans an ever-larger depth range, so the result is decided by bias error rather
than geometry. Too little bias there → acne; too much → leaks. **No setting of Bias, Slope Bias,
Normal Offset or Normal Smoothing can fix one without producing the other**, because they are two
ends of one broken input. This is why the tuning knobs felt like they were going in circles: they
were.

Fixes applied:
- `occlusion = max(map_occlusion * light_facing, 1 - light_facing)` — the map fades out exactly
  where it cannot be trusted, and `n·L` already dominates there, so nothing visible is lost. Cast
  shadows on lit surfaces (`light_facing = 1`) are completely unchanged. Monotone, so no seam.
- The normal offset was missing half of Holbert's method: it must scale by `sin(angle to light)`.
  A constant offset is simultaneously too small at grazing (acne) and too large head-on
  (peter-panning). **This changed what the Normal Offset number means** — it is now the
  grazing-incidence maximum.

### 8.8 The receiver-plane fractional-sampling term was larger than the geometry

The bug that best matches the long-running "improperly lit patches":

```wgsl
let cap = rpdb_max * map_size;                       // 0.02 * 4096 = 81.92
bias_uv = clamp(bias_uv, -cap, cap);
base_bias += (|bias_uv.x| + |bias_uv.y|) * inv_map_size;   // → up to 0.04
```

The term is derived from the **already-clamped** gradient. At grazing incidence the gradient sits
*at* the cap, so this added up to `2 × rpdb_max` = **4% of the cascade's light-space depth range as
a flat margin** — reintroducing as a constant precisely the explosion the clamp exists to prevent.
On a cascade covering ~25000 world units that is several hundred world units, against a character
roughly 150 units tall. **The bias was larger than the geometry it was biasing**, so a character's
own self-shadowing leaked straight through.

Why it evaded everything: it is scaled by `light_facing`, so it only fires on surfaces facing the
light — the up-facing boot tops, belt and tunic folds of a back-lit character, exactly where `n·L`
correctly abstains and the map works alone. And the constant `Bias` default is **2 world units**,
four orders of magnitude smaller, so moving `Bias` across its whole range did nothing.

Now taken over a half-texel and hard-capped at `kMaxFractionalBias = 0.001` (0.1% of the cascade
depth range).

### 8.9 Normal Smoothing is gone

Deleted entirely in `38386e4` — shader, both blur pipelines, the two full-res `rgba32float`
ping-pong targets and their retire scheme, the compute payload and type, the config var and the UI
control. It existed solely to hide depth-reconstruction faceting; authored normals have none. It was
a dense 32-tap separable blur over the full screen, and after 8.6 it was actively harmful, flattening
the curvature the shading normal carries.

### 8.10 Process notes

- **Do not ship a fix built on an unverified premise.** Two rounds were lost this way (the
  `draw_lists_ready` guard, the MSAA length check). If a premise is checkable in the source or the
  binary, check it before writing the fix — both of those were.
- **Verify the claim, not the plausibility.** `AuroraConfig::msaa` took one grep to disprove.
- **Read the user's observation literally.** "The affected area changes with camera position"
  discriminated between two theories instantly; it was in hand before the wrong fix was written.
- **CI is ~5 minutes for all 7 platforms.** A build that adds a diagnostic is cheap; a round trip
  through the user's testing is not. Prefer shipping the view that answers the question over
  shipping another guess.

### 8.11 A shading normal cannot define an AO hemisphere

**Symptom:** with the normal debug view correct and the seam gone, toggling *Use Authored Normals*
on still made SSILVB (bounce and probe off, so pure AO) *worse in one specific way* — shading got
smoother as expected, but **AO appeared on flat ground that has no occluder anywhere near it**.

**It is not a basis problem, and this is worth stating plainly because it looks like one.** The
provider's output really is world space: the authored normal arrives in view space and is rotated
out with the camera service's `world_from_view`; VBAO and SSILVB rotate it straight back with the
same service's `view_from_world`. Those are exact inverses from the same struct in the same frame,
so the round trip is lossless to ~1e-7 — waste (two 3×3 rotations per pixel), never a visible
artifact.

**The cause is that the two normals do different jobs**, the same lesson as §8.6 but on the AO side:

- `ssilvb.wgsl:218` (`hh = (fbang + n)/PI + 0.5`) and VBAO's `carve_sample` map each sample's
  horizon angles into a 32-sector mask **centred on the normal angle `n`**. The normal therefore
  *defines the hemisphere* — it decides which directions are above the surface at all.
- A depth-reconstructed normal is perpendicular to the plane its own samples lie in **by
  construction**. Coplanar ground samples land exactly on the horizon and carve nothing. Flat ground
  is self-consistently unoccluded, for free.
- An authored normal is *deliberately not* perpendicular to its triangle — that is the entire point
  of a smoothed vertex normal. Tilt it by θ and the hemisphere tilts with it, swallowing the very
  plane the samples lie in. On TP's heavily smoothed low-poly terrain θ is easily 10–30°, which is
  θ/π of the slice: **3–5 of 32 sectors carved on a perfectly flat surface**.

**Fix:** keep the shading normal for the hemisphere and the cosine lobe — that is what buys the
smoothness — and reject samples that lie below the *geometric* plane, which is what actually
occludes:

```wgsl
if sp.w > 0.0 && dot(sp.xyz - pixel_position, geo_n) > 0.0 { ... }
```

`geo_n` is the face normal from depth: a **4-tap `geometric_normal_view`, character-identical in
both mods** — `vbao.wgsl:253` and `ssilvb.wgsl`. Keep it that way; see 8.11a for why the two copies
diverging is not a cosmetic difference. It stays **near-inert on the reconstruction path**, where
the shading normal is already a plane from depth and only the tap pattern differs, so the A/B across
*Use Authored Normals* changes the shading normal and essentially nothing else.

Three traps, all hit while writing that one line:

- `geo_n` must fall back to the **shading normal** when the cross product is degenerate, never to a
  zero vector. `dot(delta, 0) > 0` is false, so a zero `geo_n` rejects every sample and switches AO
  off entirely on those pixels — the opposite of "pass everything".
- The same applies to **NaN** from a degenerate normalize: every comparison against NaN is false, so
  it is the same silent AO-off. The guard is written `if !(len > 1.0e-12)`, negated so that NaN
  takes the fallback rather than sailing through.
- `geo_n` must be built from the **unbiased** centre position. SSILVB applied
  `pixel_position *= 1.0 - depth_bias` *before* calling `geometric_normal_view`, whose four taps are
  unbiased — leaving a spurious `depth_bias · P` offset along the view ray of 0.0040·z against a
  true one-pixel gradient of 0.0011·z. **3.7× the signal**, so the cross product measured the bias,
  not the surface. Build `geo_n` first, bias after.

### 8.11a The rejection plane must be the ±1 tap, not atyuwen's 5-tap

**Symptom:** with both mods at half res and slices, steps, black point and every other visible
setting matched 1:1, SSILVB held extremely thin mid-distance coverage that VBAO broke up — an area
reading solid in SSILVB looked *"almost half-res"* in VBAO, despite both being in half-res mode.

Everything else in the AO path is shared and was ruled out by direct comparison: MIP selection
during the march (`clamp(log2(max(dist, 1.0)) - 3.3, 0.0, 4.0)`), `load_sample_position`,
`calculate_neighboring_depth_differences` and its edge packing, `preprocess_depth.wgsl`, the
noise/jitter, the sector math, the denoise kernel, the composite shaping. Byte-identical or
algebraically identical.

The one real difference was the rejection plane introduced by 8.11: VBAO used `reconstruct_normal`
(atyuwen's 5-tap, taps at ±1 **and ±2** pixels) where SSILVB used the new 4-tap (±1 only).

**Why the tap radius matters here and not for shading.** At mid distance a thin feature is 1–2 chain
pixels wide, and the chain is half-res, so coarser again. A ±2 tap lands **both** far taps on the
background; atyuwen's side test then picks whichever side extrapolates most smoothly, which is the
background. `geo_n` becomes the *background's* plane, `dot(delta, geo_n) > 0` discards samples that
legitimately occlude the thin feature, and its occlusion breaks up per pixel — which through the
half-res upscale reads as lower resolution. A ±1 tap at least has a chance of straddling the
feature.

`reconstruct_normal` is not wrong; it is being asked the wrong question. Its wide taps are exactly
what make it **silhouette-robust for shading** — it is picking a stable plane across a
discontinuity. A per-pixel rejection plane wants the opposite: the plane of *this* pixel, however
small the feature it belongs to. It stays as VBAO's normal fallback, and nowhere else.

**Rule:** the service returns a *shading* normal wherever the game supplied one. Anything asking
"is this direction above the surface" — an AO hemisphere, a shadow-map bias — must build its own
geometric normal from depth. The service header says so now.

### 8.12 `max` is not how you combine two independent visibilities

**Confirmed fixed in-game.**

**Symptom:** in Realtime Sun Shadows, a bright stripe like a **specular glint** running along the
light/shadow boundary on curved surfaces — reported from Debug View 15 as sitting exactly where the
red (map) region hands over to the green (`n·L`) region. Not at every shadow edge, which is what
made it look exotic.

The combine in `shadow.wgsl` was:

```wgsl
occlusion = max(map_occlusion * light_facing, 1.0 - light_facing);
```

Write it as `max(m·f, 1 − f)` and evaluate it for a pixel **fully inside a cast shadow**, `m = 1`:

| `f` (light_facing) | 1 | 0.75 | **0.5** | 0.25 | 0 |
|---|---|---|---|---|---|
| `max(m·f, 1 − f)` | 1 | 0.75 | **0.5** | 0.75 | 1 |

A **V-shaped dip to half darkness through the middle of the terminator band, inside a shadow**. At
the default Terminator Softness the band is ±0.1 in `n·L` — a few degrees of surface orientation —
so on anything curved the dip is a thin bright stripe following the terminator through a dark
region. It needs a cast shadow to *overlap* the terminator band, which is a much smaller set of
pixels than "every shadow edge": hence rare-but-common-enough.

**The fix is one line**, because the two terms are independent reasons not to receive sun —
something is in the way, or the surface is turned away — so the light that arrives is their
**product** and the darkening is one minus it:

```wgsl
occlusion = saturate(map_occlusion * light_facing + (1.0 - light_facing));  // = 1 - (1 - m) * f
```

Same endpoints (`f = 1` → `m`, `f = 0` → 1), no dip, and genuinely monotone in both inputs
(`∂/∂m = f ≥ 0`, `∂/∂f = m − 1 ≤ 0`) where `max` is not monotone in `f` at all.

**What made this survive so long** is the comment that defended it: *"at light_facing 0.5 the result
is 0.5 either way, so the fade is invisible."* Both halves of that are true — the terms really do
both equal 0.5 there, and the handover really is seamless — and the conclusion is still wrong,
because the *correct* value for a pixel that is both shadowed and facing away is 1.0. Two terms
agreeing on a number is not evidence that the number is right. The comment also claimed the
expression was "monotone in both inputs", which is false and checkable in one line of algebra.

**Rule:** when combining independent visibility/occlusion terms, multiply the visibilities. `max`
of occlusions is only correct when the terms are alternative *estimates of the same quantity*
(which is why `max` remains right for the Link cascade, and for folding in the Bend SSS term — both
are other measurements of "is something in the way").

## 9. Could a mod produce this buffer without the aurora change?

Investigated against **upstream `TwilitRealm/dusklight` HEAD `4504e5009`** (28 commits past our base
`76b56cd8`, which is a clean ancestor) — `docs/modding.md`, the whole `sdk/include/mods/` tree, and
aurora's GX code generator. Question: can a mod-API-only provider mod produce the authored-normal
buffer we currently get from the renderer, so the aurora/SDK delta could be dropped?

### 9.0 Verdict

| | |
|---|---|
| Does the mod API expose per-draw authored normals, MRT, or the game's geometry? | **No.** Nothing in GfxService 1.0 or 1.1 reaches a draw's vertex attributes or adds an attachment to a pass. |
| Is there *any* mod-only route to true authored normals? | **Yes, exactly one** — hijack GX's per-vertex lighting so the game's own shaders rasterize the normal as colour, into a replayed offscreen pass (§9.2). |
| Should we do it? | **No.** It is strictly worse on every axis that matters here (§9.3) and it does not remove the aurora fork (§9.4). |
| Is there a way to stop carrying the delta? | **Yes — upstream it** (§9.5). That is the real answer to the question behind the question. |

### 9.1 What the mod API actually offers

`GfxService` gives a mod four ways to touch rendering, and none of them can see a draw's normals:

- **`push_draw`** records a *mod-authored* pipeline into the current pass. Its vertex data comes from
  the mod's own `push_verts`/`push_indices`. `GfxDrawContext` does hand over `vertex_buffer` /
  `index_buffer`, but those are aurora's shared per-frame streaming buffers — the mod has no map of
  what the game put in them, in what layout, at what offset. Aurora decides that per draw while
  decoding GX display lists.
- **`create_pass` + `resolve_pass`** open an offscreen colour+depth pass (surface format, cleared to
  `(0,0,0,0)`) and snapshot it. This is how Realtime Sun Shadows renders its cascades.
- **Stage hooks** (`SCENE_BEGIN` … `FRAME_AFTER_HUD`) are timing points, not attachment control.
- **Present targets** (new in GfxService **1.1**, upstream commit `7305ef09b`) render a mod's own
  content into an *extra window or surface* alongside WindowService. Despite the name "external
  rendering" this has nothing to do with extra targets in the scene pass.

`GfxDeviceInfo` gained `instance` / `adapter` in 1.1; `GfxResolveDesc` / `GfxResolvedTargets` are
unchanged. **Upstream still has no normal buffer of any kind.**

The value we need — `mv_nrm` — exists only inside the *generated* GX vertex shader
(`aurora/lib/gx/shader.cpp:1022-1025`). Routing it anywhere is a shader-generator change, and the
shader generator is aurora.

### 9.2 The one route that does work: hijack GX lighting during a replay

GX computes per-vertex lighting from the authored normal, and aurora implements that faithfully
(`lib/gx/shader.cpp`, `build_light_source`):

```wgsl
lighting = amb;
for each enabled light {
    ldir = normalize(light.pos - mv_pos);   // aurora treats every light as positional
    attn = 1.0;                             // GX_AF_NONE
    diff = dot(ldir, mv_nrm);               // GX_DF_SIGN — signed, unclamped
    lighting += attn * diff * light.color;
}
out.cc0 = mat * clamp(lighting, 0, 1);
```

Set three lights very far along the view-space axes (`GXInitLightPos` takes view-space coordinates,
so `ldir` is the axis to within `|mv_pos| / D`), give them colours `(k,0,0)`, `(0,k,0)`, `(0,0,k)`,
set the ambient register to `(a,a,a)` and the material register to white, with
`GXSetChanCtrl(GX_COLOR0A0, enable, GX_SRC_REG, GX_SRC_REG, lightMask, GX_DF_SIGN, GX_AF_NONE)`:

```
out.cc0 = a + k * mv_nrm
```

With `k = 127/255`, `a = 128/255` that is **the same encoding the g-buffer writes**, in the same
space, at the same 8-bit precision, and it never trips the `clamp`. Interpolation is equivalent:
encode is affine, so interpolating the encoded value equals encoding the interpolated normal, and
consumers renormalize on read anyway. Point the last TEV stage's colour output at `RASC`
(`GXSetTevColorIn(stage, ZERO, ZERO, ZERO, RASC)` + `GXSetTevColorOp(..., GX_TEVPREV)`) and the
framebuffer receives the encoded normal.

The plumbing all exists on our side already:

- `DEFINE_HOOK(GXCopyTex, …)` and `DEFINE_HOOK(GXSetCullMode, …)` in Realtime Sun Shadows prove GX
  entry points are hookable **and** callable from a mod (`dolphin/gx/GXLighting.h` declares the
  whole lighting API, and aurora implements it in `lib/dolphin/gx/GXLighting.cpp`).
- The injection point is a **pre-hook on `J3DShape::drawFast`** — after the material has loaded its
  GX state, before the shape streams vertices. Graphics Hub already holds that hook for Deferred Fog
  and Realtime Sun Shadows already holds `J3DMatPacket::draw`.
- The replay itself is the shadow mod's existing mechanism: `create_pass(w, h)` at render resolution
  with the main camera, then `dComIfGd_drawOpaListBG/DarkBG/Middle/…()`, then `resolve_pass(color)`.
- Validity needs no extra channel: a unit normal can never encode to `(0,0,0)` (that would be
  `n = (-1,-1,-1)`, length √3), and the offscreen pass clears to `(0,0,0,0)`, so "decoded length ≉ 1"
  *is* the validity test — the same length test §8.5 left inert would become the live one.

So the idea is sound, not hand-waving. It is still the wrong thing to build.

### 9.3 Why it is worse in practice

1. **It costs a second full opaque replay every frame.** Not a cheap one: the normal buffer must
   cover every visible surface at full resolution, so none of the shadow mod's levers apply — no
   light-column culling, no `casterMinTexels`, no staggering, no reduced map size. It is ~1× the
   main scene's geometry walk and vertex streaming, added to the **game thread**.
2. **It runs straight into the per-frame streaming budget**, the one whose overflow is an
   unconditional `abort()` (`ByteBuffer::resize`) — the v1.6.0 instant-crash-to-desktop. See
   `realtime_sun_shadows.md`. The renderer-side buffer streams *zero* extra vertices: it writes one
   more 4-byte value per fragment already being rasterized.
3. **Alpha-tested cutouts.** Forcing the colour path is safe; the alpha path is not. Leaves, grass,
   chains and ladders need their texture alpha test intact, and TP materials vary in which stage and
   map supply it. Get it wrong and foliage becomes solid quads in the normal buffer — wrong normals
   over exactly the geometry AO and shadows care most about.
4. **Coverage is strictly worse.** The renderer gates the normal write on `depthCompare &&
   depthUpdate`, so coverage matches the depth buffer *by construction*, including depth-writing
   transparencies. A replay only covers what the replayed lists contain; direct GX drawers (the
   grass/flower packet list is called out in the shadow docs) and anything outside those lists write
   whatever their own TEV state produces — garbage in the normal target unless individually handled.
5. **State leakage.** The shadow mod already has an open item to widen GX save/restore around its
   replays. This proposal hijacks channel control, eight light objects, and TEV stage state per
   shape, and must restore all of it. That is a much bigger blast radius on the same known-fragile
   seam.
6. **The provider becomes deeply game-linked.** Depth to Normal's normal path is service-only today.
   This would couple *the thing every other mod depends on* to J3D internals and TP's material
   conventions — the exact ABI treadmill CLAUDE.md tells us to keep the provider off.
7. **More code, in the hardest place.** The renderer delta is ~90 lines of additive aurora code plus
   five `struct_size`-guarded SDK fields. The mod version is a per-material state machine over TEV,
   lighting and alpha, plus a replay, plus a budget story.

### 9.4 It would not remove the aurora fork

`aurora-ao` historically carried **two** deltas, and the thin g-buffer was only one of them. The
other was the enlarged per-frame streaming buffers (Index 4 MB, Vertex 16 MB, Storage 16 MB), which
existed because the shadow mod's replays overflowed the aurora sizes **of the time** (Vertex 3 MB,
Index 1 MB); upstream has since raised both itself, so that delta is retired. Adding a second full-scene
replay would need *more* headroom, not less, so the trade bought nothing on the axis it was proposed
for.

**As of the current pin that second delta is gone** — the rebased branch is upstream aurora plus the
normal target and nothing else, so the buffers are back to 5 / 2 / 8. That does not revive this
proposal (it makes the headroom argument worse, not better), but it does mean the shadow mod's
cascade replays are running on stock sizes again; see CLAUDE.md's ABI pin.

### 9.5 The actual way to stop carrying the delta

Upstream it. The change is small, additive, off by default, and useful to any aurora consumer:

- **aurora** (`encounter/aurora`): `AuroraConfig::enableNormalBuffer` → optional second colour target
  + the `@location(1)` write. Documented end to end in `dusklight/docs/thin-gbuffer-normals.md`.
- **Dusklight** (`TwilitRealm/dusklight`): the five appended `struct_size`-guarded SDK fields, which
  is exactly the shape of change GfxService 1.1 already made for present targets — it would land as
  GfxService **1.2**.

That removes the fork properly and leaves every consumer mod unchanged, whereas §9.2 removes nothing
and makes the provider fragile.

### 9.6 Upstream drift since our base (rebase notes)

Checked while investigating; relevant whenever we re-platform.

- `76b56cd8` (our base) is an ancestor of upstream HEAD `4504e5009` — **28 commits**, so a rebase is
  clean in principle.
- **ABI collision to watch.** Upstream appended `WGPUInstance instance; WGPUAdapter adapter;` to
  `GfxDeviceInfo` — the same slot where our fork appended `WGPUTextureFormat normal_format;`. On a
  rebase, ours must be re-appended **after** theirs and the service minor bumped to 1.2. Re-applying
  our diff blindly would silently mis-map the struct. `GfxResolveDesc` / `GfxResolvedTargets` are
  untouched upstream, so those merge cleanly.
- **SDK source renames that touch our three game-linked mods** (`7305ef09b`):
  `mods/hook.hpp` → `mods/svc/hook.hpp`, and `mods::hook_add_pre/add_post/replace(svc_hook, fn)` →
  `mods::hook::add_pre/add_post/replace(fn)` (the service argument is now an optional overload).
  We already use the `mods::` namespace, so `dusk::mods::` → `mods::` costs us nothing.
- **New and free if we want it:** an `fmt` feature with `mods/svc/log.hpp` formatted logging, UI
  toasts (`push_toast`), WindowService, and GfxService present targets.
