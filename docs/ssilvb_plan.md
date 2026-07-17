# SSILVB — Screen Space Indirect Lighting with Visibility Bitmask (implementation plan)

New **service-only** mod `mods/ssilvb/` (id `dev.automata.ssilvb`, name "SSILVB"), implementing
the paper of the same name — *Screen Space Indirect Lighting with Visibility Bitmask* (Therrien,
Levesque, Gilet, The Visual Computer 2023 / arXiv 2301.11376) — on our stack. It is the natural
sibling of VBAO: the paper is the same visibility-bitmask technique VBAO's occlusion estimator
already implements, extended from "how much of the hemisphere is blocked" to "**what light arrives
through the parts that aren't**." The mod deliberately carries the paper's name rather than a
"VBGI" coinage: with the bounce toggled off it is a standalone directional-AO method, not only a
GI mod, so naming it after the technique (which covers both uses) is the honest label.

## 0. Working mode for this mod (read first — applies to every session)

The user has been explicit, for the record and for future Claude Code sessions: **the technical
direction of SSILVB rests with Claude.** The user describes themselves as an amateur on
SSAO/SSGI internals, finds the concepts here confusing, and cannot provide technical direction on
the algorithm, the math, or the rendering architecture. Concretely:

- Do **not** block on the user for technical decisions (sampling math, buffer formats, pass
  structure, blend states, temporal design, defaults). Decide, document the reasoning in this
  file or `docs/ssilvb.md`, and proceed.
- What the user **does** provide: in-game testing and visual feedback ("this looks wrong /
  washed out / flickers here"), screenshots, and taste-level calls (too strong, too subtle,
  prefer this default). Frame any questions to them in those terms — looks and preferences,
  never implementation choices.
- When feedback arrives ("it glows in dark rooms"), translate it into the technical fix yourself;
  don't present the user with implementation options to choose from.
- The debug views (§6) and tunables exist precisely so the user can express feedback by eye and
  by slider rather than by concept.

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
mods/ssilvb/
  CMakeLists.txt          # add_mod(ssilvb FEATURES webgpu SOURCES src/mod.cpp MOD_JSON mod.json RES_DIR res)
                          # + target_include_directories(... ../depth_to_normal/include)
  mod.json                # id dev.automata.ssilvb, name "SSILVB", 0.x until in-game sign-off
  src/mod.cpp             # host: pipelines, targets, temporal state, config vars, UI panel
  res/preprocess_depth.wgsl   # copied from VBAO (identical MIP-chain prefilter)
  res/preprocess_color.wgsl   # NEW: small box-filtered MIP chain of the scene-color snapshot
  res/ssilvb.wgsl               # vbao.wgsl + the GI accumulate (the heart of the mod)
  res/denoise.wgsl            # VBAO's edge-aware filter widened to rgba (GI.rgb + AO)
  res/temporal.wgsl           # VBAO's accumulation/upsampler widened to rgba + split depth plane
  res/composite.wgsl          # NEW composite: outputs (GI_add.rgb, AO) — see §5
  res/licenses/               # carried over (Bevy SSAO / XeGTAO framework heritage)
```

Root `CMakeLists.txt` gets one `add_subdirectory(mods/ssilvb)`; CI needs **zero changes** (the
combine loop iterates every `.dusk` the platform legs produce). Docs: this file + a `docs/ssilvb.md`
tunables/architecture doc once it lands, plus one line each in `CLAUDE.md`/`README.md` and a
check-mark in `depth_to_normal_consumers.md` (SSGI row becomes "shipped: ssilvb").

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
- **Emissive effects (torch fire, fairy glow, lanterns) are NOT in the opaque snapshot** — they
  are particle/effect draws in the translucent phase, after `SCENE_AFTER_OPAQUE`. First in-game
  testing flagged this immediately (fire contributed zero bounce). The fix (v0.9.1): a second
  `resolve_pass` at `FRAME_BEFORE_HUD` captures the late scene (aurora's resolve snapshots at
  the call point — verified in `extern/aurora/lib/gfx/common.cpp`, no per-frame caching), a
  small compute extracts `max(late − opaque − threshold, 0)` in linear light (everything the
  translucent phase *added*: emissive particles and their bloom; the threshold keeps our own
  composite and Deferred Fog's re-apply from feeding back), and the next frame's color
  prefilter reprojects that delta into light-input MIP 0 with the temporal reprojection matrix.
  Emissive light then rides the normal MIP chain — the march is unchanged. One-frame latency,
  invisible in practice. Controls: `emissiveBounce` (on), `emissiveBoost` (300%), and
  `emissiveThreshold` (10%). Known approximation: the emitter cosine uses the opaque surface
  *behind* the particle (a flame is a volumetric emitter with no normal of its own).

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
4. ● `ssilvb.wgsl` — VBAO's slice/step walk with the accumulate from §1 spliced into
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

**Interaction with VBAO**: running both mods double-darkens (two AO multiplies). SSILVB's AO term is
the *same estimator*, so the guidance is: run SSILVB alone (its `aoApply` covers AO duty), or run
both with SSILVB's `aoApply` off (pure light bounce on top of VBAO's occlusion — also exactly the
"directional AO only" configuration the user described, inverted). Both composites are
`SCENE_AFTER_OPAQUE` pushes, so Deferred Fog still layers correctly over whichever combination.

### 5.1 Receiver albedo proxy (`chromaLift`)

`GI` is irradiance; physically it should be multiplied by the receiver's **albedo**. No albedo
buffer exists anywhere in this game's frame — Dusklight is a forward GC-era renderer whose TEV
combiners write finished color straight to the framebuffer; there is no G-buffer and no pass where
albedo sits isolated. (It can't be exposed by a service extension either — there is nothing
upstream to point at. Creating one is possible but game-linked: see §8.) So the proxy works from
the receiver's snapshot color `s`, and the design leans on a property of this specific game:

**Vanilla TP's lighting is nearly flat.** There is no real-time direct sun on world geometry
(only actors cast shadows); the world is lit by a mostly uniform — though often strongly tinted —
ambient plus low-contrast baked vertex shading. Under flat lighting `s ≈ albedo × ambient` with
ambient locally constant, i.e. **the scene color IS an albedo buffer up to a global scale** that
`giIntensity` absorbs — the raw scene color becomes a nearly exact proxy. The ambiguity the proxy
exists to fight (is this pixel dark because of material or because of lighting?) is reintroduced
by *our own mods*: Realtime Sun Shadows darkens shadowed receivers and VBAO darkens crevices, and
the snapshot may include their composites (same-stage ordering across mods is not controllable).
Chroma-normalizing (dividing luminance out) defends against exactly that — at the cost of
discarding albedo *brightness*, which under vanilla-flat lighting is real material information.

Since the right answer sits on a spectrum between "raw scene color" (faithful in vanilla) and
"full chroma normalization" (robust with RSS/VBAO installed), the control is one continuous knob
instead of a mode select:

```
proxy = clamp(s / pow(max(luma(s), 0.08), chromaLift), 0, 1)   // per channel
```

- `chromaLift = 0` → proxy = `s`: raw scene color. Near-exact under vanilla's flat lighting;
  shadowed receivers kill their own bounce when shadow mods are installed.
- `chromaLift = 1` → full chroma normalization: hue/saturation kept, brightness discarded; the
  standard backbuffer-GI compromise, right when RSS/VBAO darken the snapshot.
- **Default 50** (×0.01 int var), tuned in-game. The 0.08 luma floor bounds noise amplification
  in dark scenes (division by near-zero luma).
- A separate `bounceWhite` debug-ish toggle forces proxy = 1 (raw irradiance — chalky but
  unambiguous, useful for judging the light transport itself).

Two smaller flat-lighting consequences, both acceptable: TP's ambient is strongly *tinted*
(dusk orange, Lakebed blue), and the proxy carries that tint into the multiply, mildly
double-tinting the bounce (low-frequency, stylistically benign). And since the *source* side
`c_j ≈ albedo × ambient` everywhere, vanilla SSILVB reads as soft color bleeding / directional
ambient transfer rather than the paper's hard "sunlit wall lights the alley" — with RSS installed,
sunlit-vs-shadowed sources diverge and true direct-bounce behavior emerges. The mods compound.

## 6. Config vars / UI (all fixed-point ints ×0.01 unless noted, VBAO conventions)

**Effect** — `effectEnabled` (on) · `giEnabled` (on; **off = directional-AO-only mode**, the
sampling loop skips all color/normal fetches) · `giIntensity` (200, 0–800) · `giBlendMode`
(select Screen/Add, default Screen) · `chromaLift` (50 of 0–100, §5.1) · `bounceWhite` (off) ·
`aoApply` (on) · `aoIntensity` (150) · `contrast` (150) · `blackPoint` (3).

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

Uniform struct: start from `AoUniforms`, add `gi_intensity`, `chroma_lift`, and flag bits for
blend mode / GI enable / AO apply / white proxy, drop the VBAO-only debug fields that don't apply;
keep the C ↔ WGSL mirror rule and the `%16 == 0` static_assert (mod-api-notes).

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

Additions from the post-v0.9.1 roadmap review (2026-07-16), roughly in recommended order:

- **Sky light injection (directional sky fill)** — **SHIPPED in v0.9.2** as the
  directionally-occluded ambient (paper §3.2 with the sky as the ambient source). The sky
  radiance is measured live from the on-screen skybox pixels (raw depth 0), which the game has
  already painted with its time-of-day palette — sunset gradients and weather included; the
  ambient tint the user asked about is thereby captured without any game-state access. A
  per-workgroup reduction in `prefilter_color` plus a 1×1 `reduce_sky` collapse produce a
  temporally smoothed (rgb = radiance, a = confidence) estimate that is HELD while no sky is
  on screen and fades over seconds indoors. In the sampler, each slice's still-visible sector
  count (the same cosine-weighted openness integral the AO uses) is weighted by the visible
  arc's bent direction: the arc midpoint (first/last visible sector, unwarped through the exact
  inverse of the cosine-lobe smoothstep) becomes an angle, and `smoothstep(-0.15, 0.5,
  dot(bent, world_up_in_view))` gates sky-facingness — light comes in sideways through windows
  but not up through ceilings. Works with the bounce off (directional AO + sky ambient mode);
  `skyLight` (on) + `skyIntensity` (100 = measured tint as-is, host pre-divides by
  gi_intensity so the sliders are independent). The per-slice bent direction computed here is
  also the stepping stone to the bent-normal service export below.
- **Per-sample radiance luma clamp (firefly guard)** — **SHIPPED in v0.9.2**: `c_j` luminance
  capped at 3.0 linear in `sample_bounce`.
- **v0.9.3 tuning from the first sky/emissive in-game round** (user feedback: desert got a blue
  cast under overhangs and on far mountains; Hyrule Field lost contrast; bounce/emissive weak
  even at maxed sliders): (1) the v0.9.2 firefly cap was FIXED at 3.0 luma, silently
  neutralizing `emissiveBoost` past ~100% — it now scales as `max(4, 2 x boost)`; (2) sky
  sampling is horizon-weighted (`w = 0.15 + screen_y^2`) so the warm horizon haze drives the
  tint instead of the zenith blue; (3) new `skySaturation` (65) pulls the sky tint toward
  neutral luminance, and sky default intensity dropped to 75; (4) new `giSaturation` (120)
  restores chroma the MIP prefilter + temporal average wash out; (5) slider ceilings raised —
  `giIntensity` to 1600 (default 250), `emissiveBoost` to 2000 (default 400) — the LDR source
  legitimately needs large multipliers, and Screen blend attenuates on bright receivers
  (suggest A/B with Add mode when judging strength).
- **Un-bake / Relight companion mod (game-linked; user-requested investigation, verified
  feasible)**: TP's world lighting is two baked layers, both confirmed in the Dusklight source:
  per-vertex colors in the room models (J3D materials carry `J3DColorChanInfo`; BG geometry
  rasterizes vertex color x channel light into the TEV), and the kankyo time-of-day palette
  (`d_kankyo.cpp`: `bg_amb_col[0..3]` + actor ambient, applied per draw via
  `settingTevStruct`/tevstr into TEV registers). A game-linked mod could neutralize both - a
  load-time patch of the material channel config (force material source REG/white instead of
  VTX, the deferred_fog per-draw-override pattern applied to channels) plus a tevstr hook for
  the ambient registers - with a 0-100% strength lerp rather than a binary switch. That would
  hand almost all shading to RSS + SSILVB (and as a bonus make the snapshot markedly closer to
  true albedo, improving the §5.1 proxy). Big look change; prototype as its own mod.
  directional visibility distribution; exporting (bent normal, AO) as a service (the
  `depth_to_normal` pattern) would let the shadow mod shape its ambient term and any future SSR
  mod cheapen its occlusion — SSILVB becomes a provider, not just a consumer.
- **Resolution scale (quarter-res chain)**: the Android headroom lever noted in §7, subsuming
  the halfRes toggle into a scale factor.
- **Two-scale gather**: a second, coarse far-field pass (quarter res, large radius) added to the
  near-field one — large-hall light transfer without paying full price at the contact scale.
- **Local-light synergy (no work needed here)**: if Realtime Sun Shadows grows local light
  sources, their lit surfaces land in the opaque snapshot and SSILVB bounces them automatically
  — worth an explicit in-game check when that lands, not code.

Known accepted limitation (documented, not scheduled): translucent surfaces (water) neither
receive bounce nor occlude it — the composite runs before the translucent phase and the depth
buffer is opaque-only. Revisiting means a fundamentally different composite point.

- **Multi-bounce** (`multiBounce` toggle + `bounceGain` ≤ 80): feed the *previous frame's*
  `history_color` GI, reprojected with the existing temporal matrix, into `c_j` at each sample
  (`c_j += giGain × prevGI(sample_uv')`). No feedback through the scene snapshot happens by
  itself (each frame re-renders the opaque pass, our composite is not in next frame's snapshot),
  so gain stays fully under our control — but it still needs the paper's energy-balance care and
  in-game A/B time, hence v2.
- **Albedo provider mod** (real receiver albedo, replacing the §5.1 proxy when present): a
  separate **game-linked** mod that replays the opaque draw lists into an offscreen color pass
  with lighting flattened to white — the same draw-list-replay machinery Realtime Sun Shadows
  already proves out (for depth) and the same per-draw state-override pattern Deferred Fog uses
  (for fog) — and publishes the result as a service, exactly the `depth_to_normal` pattern.
  SSILVB would take it as `IMPORT_OPTIONAL_SERVICE`: real albedo when installed, `chromaLift`
  proxy otherwise, so SSILVB itself stays service-only and update-proof while the ABI coupling
  and the extra geometry pass live in an opt-in mod. This is the `CLAUDE.md`-sanctioned case for
  game-linking ("per-object albedo that SSGI might want"). It would also sharpen the *source*
  side (bounce from un-lit surface color, closer to the paper's G-buffer setup). Build it only
  if the proxy's limits actually bother the user in play.
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

1. Scaffold `mods/ssilvb/` (CMake, mod.json, VBAO shaders copied, `dusk`-loadable no-op composite).
2. Color snapshot + `preprocess_color.wgsl` + debug view 4 (proves light input + gamma).
3. `ssilvb.wgsl` GI accumulate, denoise widened, composite in Add mode, temporal OFF path — first
   visible bounce (noisy, full-res).
4. Composite finalization: Screen mode, albedo proxies, AO term + `aoApply`, black point/contrast.
5. Temporal: split-plane history, per-channel clamp, half-res jittered upsampler. Default-on tune.
6. Docs (`docs/ssilvb.md`, README/CLAUDE/consumers), version 1.0.0 after in-game sign-off.

CI validates every step on all 7 platforms automatically; nothing in the build system changes
beyond the one `add_subdirectory`.
