# VBGI — Visibility Bitmask Global Illumination (implementation plan)

New **service-only** mod `mods/vbgi/` (proposed id `dev.automata.vbgi`, name "VBGI"), implementing
**SSILVB** — *Screen Space Indirect Lighting with Visibility Bitmask* (Therrien, Levesque, Gilet,
The Visual Computer 2023 / arXiv 2301.11376) — on our stack. It is the natural sibling of VBAO:
the paper is the same visibility-bitmask technique VBAO's occlusion estimator already implements,
extended from "how much of the hemisphere is blocked" to "**what light arrives through the parts
that aren't**." The name keeps the family naming (VBAO → VBGI); the paper's own acronym SSILVB is
fine too if preferred — nothing below depends on the name.

## 1. What the paper does (and what we already have)

One compute pass per pixel, walking hemisphere slices exactly like VBAO. Per slice a 32-bit
visibility bitmask is carved sample by sample. The single addition (paper Algorithm 1, line 23):
when a marched sample carves sectors `bj` into the running mask `bi`, the sectors that are **newly
occluded this step** (`bj & ~bi`) are precisely the solid angle through which that sample's surface
is visible from the receiver — so the sample's radiance is accumulated with that weight *before*
the mask is updated:

```
GI += countbits(bj & ~bi)/32 · c_j · max(n_p·l_j, 0) · max(n_j·-l_j, 0)
bi  = bi | bj
AO  = 1 - countbits(bi)/32          (per slice, averaged over slices — unchanged from VBAO)
```

- `c_j` — radiance at the sample (paper: HDR direct-light buffer; us: see §3),
- `l_j` — unit direction receiver → sample,
- `n_p`, `n_j` — receiver and **sample** normals (us: Depth to Normal service),
- the `bj & ~bi` trick is what makes light correctly pass behind thin occluders and prevents
  double-counting: a surface behind an already-occluded sector contributes nothing.

Everything else in the paper — thickness-aware front/back horizon carving, bitmask centered on the
projected normal, jittered steps to hide banding, 32 sectors as the quality/perf sweet spot,
half-res + bilateral upsample, temporal distribution of directions — **VBAO already implements**,
in most cases in a stronger form (log-scaled thickness with depth-difference fade, Hilbert+R2
noise, neighborhood-clamped temporal accumulation, jittered temporal *upsampler* rather than plain
bilateral upscale). So this mod is: **VBAO's chain + three new per-sample fetches (color, sample
normal) + one accumulate line + a completely different composite.**

Paper results worth keeping in mind: sampling pass 0.9–4.0 ms at 1080p/RTX 2080 depending on
sample count and radius; half-res ≈ 4× cheaper (1.07 ms total incl. denoise); GI is
bandwidth-bound (uncorrelated color/normal fetches), which our color MIP chain (§4.2) mitigates.

Out of scope from the paper:
- **Directionally-occluded ambient lighting (paper §3.2)** weights an *ambient source* per
  hemisphere subregion. Service-only we have no separated ambient term to weight (the game's
  ambient is baked into the composited scene color), so this variant needs either a game-linked
  fetch of TP's light registers or an upstream service extension. Not in v1; noted in §9.
- **Multi-bounce** (inject GI into next frame's light input): deferred to a v2 experiment (§8) —
  the paper itself warns it needs careful energy balancing to avoid feedback.

## 2. Mod shape

Service-only, exactly the VBAO pattern: gfx + camera + config + ui + resource + log. **Hard
dependency on the Depth to Normal provider** (`IMPORT_SERVICE(DepthToNormalService, ...)`), unlike
VBAO's optional import: the GI accumulate needs `n_j` at every marched sample, and reconstructing
normals per sample with the 5-tap method would quintuple the depth fetches. The provider is
already the suite's shared base (shadows consume it too); "install both" is one line in the
readme, and the loader reports the missing dependency cleanly.

```
mods/vbgi/
  CMakeLists.txt          # add_mod(vbgi FEATURES webgpu SOURCES src/mod.cpp MOD_JSON mod.json RES_DIR res)
                          # + target_include_directories(... ../depth_to_normal/include)
  mod.json                # id dev.automata.vbgi, name "VBGI", 0.x until in-game sign-off
  src/mod.cpp             # host: pipelines, targets, temporal state, config vars, UI panel
  res/preprocess_depth.wgsl   # copied from VBAO (identical MIP-chain prefilter)
  res/preprocess_color.wgsl   # NEW: small box-filtered MIP chain of the scene-color snapshot
  res/vbgi.wgsl               # vbao.wgsl + the GI accumulate (the heart of the mod)
  res/denoise.wgsl            # VBAO's edge-aware filter widened to rgba (GI.rgb + AO)
  res/temporal.wgsl           # VBAO's accumulation/upsampler widened to rgba + split depth plane
  res/composite.wgsl          # NEW composite: outputs (GI_add.rgb, AO) — see §5
  res/licenses/               # carried over (Bevy SSAO / XeGTAO framework heritage)
```

Root `CMakeLists.txt` gets one `add_subdirectory(mods/vbgi)`; CI needs **zero changes** (the
combine loop iterates every `.dusk` the platform legs produce). Docs: this file + a `docs/vbgi.md`
tunables/architecture doc once it lands, plus one line each in `CLAUDE.md`/`README.md` and a
check-mark in `depth_to_normal_consumers.md` (SSGI row becomes "shipped: vbgi").

## 3. Light input — what stands in for the paper's HDR direct-light buffer

We are service-only: the only radiance available is the **opaque scene-color snapshot**
(`resolve_pass` with `color = true`, same call VBAO already makes for depth-only). At
`SCENE_AFTER_OPAQUE` that snapshot is the lit opaque world — *before* bloom, translucency and HUD.
Notes:

- The snapshot is outgoing radiance (albedo × direct+ambient), which for a bounce is actually the
  *right* quantity — the paper multiplies its light buffer by the sample's material implicitly via
  its G-Buffer; sampling final surface color gives colored bounce (red rug → red spill) for free.
- It is **LDR** (TP's GC-era pipeline has no HDR scene target). Bounce energy from clipped
  highlights is bounded; the intensity control compensates, and the Screen blend mode (§5) keeps
  additions from clipping ugly.
- **Gamma**: the color target is gamma-encoded; accumulating light in gamma space over-brightens.
  The sampler decodes `c_j` with a fixed ≈2.2 power to linear, all accumulation and temporal
  filtering happen in linear, and the composite re-encodes. Internal detail, not a user option.
- **Deferred Fog synergy**: with Deferred Fog installed, per-draw fog is suppressed during the
  opaque lists, so our snapshot is *un-fogged* (no bouncing fog-colored light) and the fog is
  re-applied over our composite afterwards (GI correctly sits under fog). Without it, distant
  samples bounce slightly fog-tinted light and the GI sits under the game's baked-in fog — both
  acceptable, no special-casing. Document the pairing as recommended.
- The paper's known limitation carries over unchanged: light that leaves the screen stops
  bouncing. Temporal accumulation softens the pop; nothing else to do in screen space.

## 4. The frame chain (per frame, `GFX_STAGE_SCENE_AFTER_OPAQUE`)

Same skeleton as VBAO — one `push_compute` payload chains every dispatch on the render worker,
then one `push_draw` composites. New/changed steps marked ●.

1. `resolve_pass` snapshots **color + depth** (VBAO: depth only). ●
2. `preprocess_depth.wgsl` — 5-level weighted depth MIP chain (verbatim from VBAO).
3. ● `preprocess_color.wgsl` — 5-level box MIP chain of the color snapshot, rgba16float (linear —
   the decode from §3 happens here, once, instead of per sample). Distant marched samples read
   matching color/depth MIP levels: kills the uncorrelated-fetch bandwidth problem the paper
   flags, and the pre-averaging is a *feature* for diffuse GI (it pre-integrates the radiance a
   wide sector would see). MIP level selection mirrors the depth sampler's
   `clamp(log2(dist)-3.3, 0, 4)`.
4. ● `vbgi.wgsl` — VBAO's slice/step walk with the accumulate from §1 spliced into
   `carve_sample`'s call site (carve returns the *pre-OR mask delta*; sample fetches add: color MIP
   at the step UV, provider normal at the step UV rotated to view space). Output rgba16float:
   `(GI.rgb, AO)`. Slice weighting (`proj_n_len`), thickness model, radius ramp, jitter, sky
   early-out all inherited untouched. With `giEnabled` off the color/normal fetches and the
   accumulate are skipped entirely (uniform flag; the loop degenerates to exactly VBAO's cost).
5. `denoise.wgsl` — same 3×3 edge-aware filter and `depth_differences` edge weights, widened to
   rgba so GI and AO denoise together (0–3 ping-pong passes; GI is noisier than AO, default 2).
6. `temporal.wgsl` — VBAO's accumulation, with the history split into two ping-ponged planes
   because rgba has no room left for depth: **history_color** rgba16float `(GI.rgb, AO)` +
   **history_depth** r16float (normalized view depth). Reprojection, expected-prev-depth
   disocclusion, mean±k·σ neighborhood clamp (applied per-channel; σ from the same 3×3 window),
   velocity/content response, and the 4-phase **half-res jittered temporal upsampler** all carry
   over structurally unchanged — the math is per-texel and format-agnostic.
7. ● `composite.wgsl` — one fullscreen triangle, §5.

Targets live in an `Targets` struct with the same retire-for-N-frames lifecycle VBAO uses (views
may be in flight on the render worker). Payloads stay under the 128-byte inline cap — VBAO's
current payload has ~5 spare view slots after its packing trick; we add `sceneColor`,
`colorMips` (single all-mip view + per-mip storage views for the prefilter), `historyDepthIn/Out`,
and drop nothing, so it fits; if a future field busts the cap, the overflow goes in a
`push_storage` block the payload points at.

## 5. Compositing — the part that cannot be VBAO's multiply

VBAO never reads the scene: it multiplies via blend state (`src=Dst, dst=Zero`). GI *adds* light,
so that is out — but the fix is still a single draw, no scene copy needed. The fragment shader
outputs `rgb = GI_add` (albedo-modulated, intensity-scaled, §5.1) and `a = AO_mul` (black-point/
contrast-shaped occlusion, or 1.0 when the AO term is disabled), and the blend state does both
operations at once:

- **Add mode**: `srcFactor = One, dstFactor = SrcAlpha` → `out = GI_add + scene × AO_mul`
- **Screen mode** (default): `srcFactor = OneMinusDst, dstFactor = SrcAlpha` →
  `out = GI_add·(1-scene) + scene × AO_mul` — energy-conserving-ish on an LDR target, never
  clips, reads as softer light bleed. One extra pipeline, selected at push time.
- Alpha channel: `src=Zero, dst=One` (preserve target alpha), same as VBAO.
- Depth state mirrors VBAO's composite (format matched, write off, compare Always); MSAA count
  from `GfxDeviceInfo`.

The composite reads the AO source exactly like VBAO (full-res history 1:1 with temporal on, else
depth-aware 4-tap upscale of the chain output) — same code, rgba instead of r.

**Interaction with VBAO**: running both mods double-darkens (two AO multiplies). VBGI's AO term is
the *same estimator*, so the guidance is: run VBGI alone (its `aoApply` covers AO duty), or run
both with VBGI's `aoApply` off (pure light bounce on top of VBAO's occlusion — also exactly the
"directional AO only" configuration the user described, inverted). Both composites are
`SCENE_AFTER_OPAQUE` pushes, so Deferred Fog still layers correctly over whichever combination.

### 5.1 Receiver albedo proxy (`bounceTint`)

`GI` is irradiance; physically it should be multiplied by the receiver's albedo, which we don't
have. Three shader modes on the receiver's snapshot color `s`:

| mode | proxy | character |
|---|---|---|
| 0 White | `1` | raw light, most visible, can look chalky |
| 1 Scene color | `s` | correct hue, but shadowed receivers (where GI matters most) kill their own bounce |
| 2 **Chroma (default)** | `s / max(luma(s), 0.08)` clamped to ≤1 per channel | keeps the receiver's hue/saturation, independent of how lit it currently is — the standard backbuffer-GI compromise |

## 6. Config vars / UI (all fixed-point ints ×0.01 unless noted, VBAO conventions)

**Effect** — `effectEnabled` (on) · `giEnabled` (on; **off = directional-AO-only mode**, the
sampling loop skips all color/normal fetches) · `giIntensity` (200, 0–800) · `giBlendMode`
(select Screen/Add, default Screen) · `bounceTint` (select, §5.1) · `aoApply` (on) · `aoIntensity`
(150) · `contrast` (150) · `blackPoint` (3).

**Sampling** — `quality` select mapping slices × steps/side: Low 2×3, Medium 3×4, **High 4×6
(default)**, Ultra 6×8, Custom (`customSlices` 1–16, `customSteps` 1–8). GI wants more steps per
slice than AO (each step is a potential light source, and misses read as missing light, not just
missing shadow) and tolerates fewer slices (radiance is lower-frequency than visibility) — hence
the different shape from VBAO's 7×3. · `radius`/`radiusFar`/`radiusRampStart`/`radiusRampEnd`/
`radiusMax` (VBAO semantics; defaults start at VBAO's 200/800/0/10000/40 and get retuned in-game —
the paper favors *large* radii for GI, which the bitmask uniquely tolerates) · `thickness`/
`thickFade`/`thickDist`/`depthBias` (VBAO defaults).

**Temporal** — `temporal` (on) · `temporalFrames` (8 — GI noise needs a longer tail than AO's 5) ·
`temporalClamp` (250 — looser: GI varies more per frame than AO, over-tight clamping re-adds
shimmer) · `motionResponse` (10) · `contentThresh` (100) · `disoccTol` (0).

**Filtering / resolution** — `denoisePasses` (2 of 0–3) · `denoiseStrength` (60) · `halfRes`
(**on** by default: the paper's own headline config is half-res, our jittered temporal upsampler
beats their bilateral, and GI's cost profile is ~2.5× VBAO's per sample) · `distanceFade`/
`fadeStart`/`fadeEnd` (off; VBAO semantics, applied to both GI and AO).

**Debug** — `debugMode`: 0 off · 1 GI only (replace-blend, over mid-gray) · 2 AO only · 3 albedo
proxy · 4 sampled light MIP (what the bounce actually sees) · 5 provider normals. Drawn at
`FRAME_BEFORE_HUD` via the same pending-payload handoff VBAO uses, for the same reason (fog/bloom
must not repaint them).

Uniform struct: start from `AoUniforms`, add `gi_intensity`, `bounce_tint`, `blend_mode`,
`gi_flags`, drop the VBAO-only debug fields that don't apply; keep the C ↔ WGSL mirror rule and
the `%16 == 0` static_assert (mod-api-notes).

## 7. Performance expectations & mobile posture

Reference points: paper 1080p/RTX 2080 — 16 samples radius 4: 2.6 ms full res, **1.07 ms half res
incl. denoise**. Our default (High 4×6 = 48 directional samples… note the paper counts *steps*,
our 4 slices × 6 steps × 2 sides = 48 fetch pairs ≈ their 32–48 bracket) lands in the same
regime; the color MIP chain trades one cheap prefilter dispatch for much better cache behavior in
the march. TP's scenes are geometrically gentle; the risk platform is Android — defaults
half-res + Medium there would be nice, but config defaults are global, so instead: half-res is
the global default and the doc recommends Medium on handhelds. If in-game numbers demand it, a v1.1
can add a "resolution scale" (quarter-res chain) rather than per-platform defaults.

## 8. v2 candidates (explicitly not in the first build)

- **Multi-bounce** (`multiBounce` toggle + `bounceGain` ≤ 80): feed the *previous frame's*
  `history_color` GI, reprojected with the existing temporal matrix, into `c_j` at each sample
  (`c_j += giGain × prevGI(sample_uv')`). No feedback through the scene snapshot happens by
  itself (each frame re-renders the opaque pass, our composite is not in next frame's snapshot),
  so gain stays fully under our control — but it still needs the paper's energy-balance care and
  in-game A/B time, hence v2.
- **Specular/rough SSR lobe** along the reflected view vector reusing the same bitmask — the
  consumers doc's SSR row, cheaper than a real ray-march for rough surfaces.
- **Directional ambient re-weighting** (paper §3.2) if/when an ambient term becomes reachable
  (upstream service extension exposing TP's light registers, or a game-linked sibling mod).

## 9. Risks / open questions (tracked for in-game validation)

1. **LDR bounce energy** — spill from bright-but-clipped areas may need `giIntensity` > 200 to
   read; if the look demands it, add a highlight-boost curve on `c_j` (pow on decoded luma).
2. **Gamma decode constant** — the 2.2 assumption vs the game's actual transfer; validate with
   debug view 4 against a known scene.
3. **Sample-normal quality at silhouettes** — the provider normal at a marched sample near a depth
   edge can belong to the far surface; the `n_j·-l_j` clamp bounds the damage, and the color MIP
   pre-average blurs the radiance anyway. Watch for rim-fire artifacts in-game.
4. **Temporal clamp vs GI ghosting** — colored light trails behind moving characters is the
   classic SSGI failure; our per-channel clamp + content response should hold given VBAO's record,
   but `temporalClamp`/`temporalFrames` defaults will need in-game iteration.
5. **Payload budget** — §4's view count fits the 128-byte cap by packing sizes like VBAO does;
   re-verify with `static_assert` the moment the payload is written.

## 10. Build order (each step compiles + runs in-game)

1. Scaffold `mods/vbgi/` (CMake, mod.json, VBAO shaders copied, `dusk`-loadable no-op composite).
2. Color snapshot + `preprocess_color.wgsl` + debug view 4 (proves light input + gamma).
3. `vbgi.wgsl` GI accumulate, denoise widened, composite in Add mode, temporal OFF path — first
   visible bounce (noisy, full-res).
4. Composite finalization: Screen mode, albedo proxies, AO term + `aoApply`, black point/contrast.
5. Temporal: split-plane history, per-channel clamp, half-res jittered upsampler. Default-on tune.
6. Docs (`docs/vbgi.md`, README/CLAUDE/consumers), version 1.0.0 after in-game sign-off.

CI validates every step on all 7 platforms automatically; nothing in the build system changes
beyond the one `add_subdirectory`.
