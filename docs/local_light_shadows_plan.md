# Realtime Shadows for Local Lights — viability assessment

Investigation of a proposed mod that makes **local/point light sources** (a fire on a totem, wall
torches, bonfires, lanterns, glowing props) cast real-time geometry shadows — turning the warm glow
each one places in the vanilla scene into a shadow-casting light. This doc is a viability study, not
a shipped plan: it grades each subproblem and recommends a scope.

**Verdict: viable as a standalone game-linked mod for a bounded number of nearby lights, reusing
~80% of the Realtime Sun Shadows machinery — with one genuinely hard subproblem (compositing) that
forces a pragmatic "darken where occluded" approach rather than physically correct light removal.**
Two of the four subproblems are solved outright; two need care. Details below.

**Two decisions locked in (from the requester):** (1) this is a **standalone** mod, not folded into
Realtime Sun Shadows — see "Standalone mod + service export" below; the earlier hook-conflict worry
does not survive scrutiny (the replay hooks self-gate). (2) The mod additionally **exports its
per-pixel local-shadow term as a service** so the **SSILVB** GI mod can apply it to its light-input
buffer — making the bounce respect local shadows (shadowed ground near a fire spills less light).
That integration is designed against SSILVB's real code in "Integration with SSILVB" below, and it
is a clean, sanctioned fit (the same optional-service pattern SSILVB already uses for Depth to
Normal and plans for an albedo provider).

## The four subproblems, graded

| # | Subproblem | Grade | Why |
|---|---|---|---|
| 1 | Enumerate lights (pos, radius, color) | ✅ Solved | The game exposes a 100-slot point-light registry, directly readable game-linked |
| 2 | Render the shadow map(s) | 🟡 Feasible, cost-bounded | Reuse the sun mod's replay; but point lights are omnidirectional and multiply the pass count against a hard streaming budget |
| 3 | Composite (subtract light where occluded) | 🟠 Hard | The glow is baked into scene color per-object with albedo — no separate light term to mask. Forces a crude darken |
| 4 | Occluder capture / bias / filtering | ✅ Solved | Directly inherited from the sun mod (replay hooks, PCF, slope/normal bias) |

## 1. Light discovery — solved (game-linked)

Twilight Princess tracks local lights in the environment manager `dScnKy_env_light_c`, a single
global `g_env_light` reachable via `dKy_getEnvlight()` (`include/d/d_kankyo.h`). The active registry
is a fixed pointer array:

```c
LIGHT_INFLUENCE* pointlight[100];   // dScnKy_env_light_c @ 0x03F8; NULL = empty slot
```

Each entry is:

```c
struct LIGHT_INFLUENCE {
    cXyz       mPosition;      // world-space xyz
    GXColorS10 mColor;         // s16 r,g,b,a — warm glow color
    f32        mPow;           // radius / influence distance (world units)
    f32        mFluctuation;   // flicker amount
    int        mIndex;         // slot bookkeeping
};
```

So per light we get exactly the three things a shadow caster needs — **world position, a radius
(`mPow`), and a color** — with no reverse-engineering. Iterating each frame on the game thread:

```c
dScnKy_env_light_c* env = dKy_getEnvlight();
for (int i = 0; i < 100; ++i) {
    LIGHT_INFLUENCE* L = env->pointlight[i];
    if (L && L->mPow > 0.01f) { /* L->mPosition, L->mPow, L->mColor */ }
}
```

This is the same scan the game itself does in `dKy_light_influence_id` (`src/d/d_kankyo.cpp`).
Concrete registrants confirm the target set: `d_a_ep` (wall torch, RGB 175/93/0, pow ∝ flame size),
`d_a_obj_fireWood` (bonfire, RGB 188/102/66, pow 500), `d_a_coach_fire` (RGB 255/100/0, pow 700,
position updated per frame). Slots 0–49 are `dKy_plight_set`; 50–99 are `dKy_mock_light_every_set`;
a separate `efplight[5]` holds transient effect lights (bomb/sword flashes). Map-placed static
lights (up to 30) land here too.

**Consequence:** this mod must be **game-linked** (it includes `d/d_kankyo.h` and reads
`g_env_light`), exactly like Realtime Sun Shadows and Deferred Fog. It is on the ABI treadmill — a
re-platform requires re-verifying the struct offsets — but that is already the shadow mod's posture.

Realistically only a handful of these are near the camera in any scene, so the design shadows the
**nearest N** (sort the non-null slots by distance to the camera eye, take the top N above a minimum
`mPow`), mirroring how the game itself only lights each object by its 1–2 nearest point lights.

**Vanilla already selects one dominant shadow light and switches to it — and exposes the choice.**
In the stock game, Link's projected character shadow is cast from a sun/moon approximation anchored
near the player, but when he gets close enough to a local light his shadow switches to being cast
*from that light's position*. This is a **real** source switch (not a vertex-lighting illusion): the
shadow system builds an actual light-space projection each frame in
`dDlst_shadowReal_c::setShadowRealMtx` (`src/d/d_drawlist.cpp:1245`) via `cMtx_lookAt` from the light
position `tevstr->mLightPosWorld` toward the receiver, with the direction formed as
`lightPos − receiverPos`. So the design does **not** need to invent light selection — the game
already contains the authoritative pick:

- **The switch rule** is in `dKy_light_influence_id` (`src/d/d_kankyo.cpp:888`): scan
  `g_env_light.pointlight[0..99]` and take the nearest light whose **distance-to-player is below that
  light's `mPow` radius** (`d_kankyo.cpp:920`). If one qualifies, `mLightPosWorld` chases that light's
  `mPosition` → shadow casts from the torch; if none, it falls back to the sun/moon `base_light`.
  Higher-priority overrides exist for **Link's lantern** (`shadow_mode` bit 2 → `field_0x10a0` =
  lantern flame pos, set in `exeKankyo`, `d_kankyo.cpp:4810`) and a **nearby indoor effect light**
  (`shadow_mode` bit 1, from `efplight[]`).
- **The current shadow source is directly readable each frame** — the mod does not re-derive
  anything. `dKy_plight_near_pos()` (`d_kankyo.cpp:9086`; member `g_env_light.plight_near_pos`)
  returns the world position Link's shadow is *currently* being cast from
  (`g_env_light.plight_near_pos = tevstr->mLightPosWorld` is published every frame at
  `d_kankyo.cpp:4075`). `g_env_light.mPlayerPLightIdx` (`d_kankyo.cpp:4578`) is the index into
  `pointlight[]` of the dominant local light near Link (−1 = using sun/moon), so
  `pointlight[mPlayerPLightIdx]` hands back that light's full `mPosition` + `mPow` + `mColor`.
  `shadow_mode` / `field_0x10a0` tell you when it's the lantern/effect override.

Two consequences for the design:

- **Shadowing the single nearest light is not a compromise — it reproduces vanilla behavior.** The
  N=1 default is the faithful case; N>1 is an *enhancement* beyond what vanilla ever did.
- **The mod reuses the game's own selection** by reading `dKy_plight_near_pos()` /
  `mPlayerPLightIdx` each frame, so the switch point and feel match vanilla and stay consistent with
  the character shadow. The mod's value-add is then narrow and clear: replace the low-res projected
  **blob** with a **real-geometry shadow map** cast from that same light, and (optionally) let *all*
  nearby objects cast into it, not just Link. (Vanilla even clamps the shadow light to ≥~39° above
  the horizon — `dir.y/|dir| ≥ 0.8`, `d_drawlist.cpp:1259` — a stylistic quirk the mod can keep for
  consistency or drop for a truer low-angle torch shadow.)

## 2. Shadow-map generation — feasible, but omnidirectional and cost-bounded

The caster-capture machinery transfers almost verbatim. `replay_cascade`
(`mods/realtime_sun_shadows/src/mod.cpp:1307`) already does the whole bracket: `create_pass(w,h)` →
set light view + projection (`GXSetProjectionFull` accepts an arbitrary 4×4, so a **perspective**
projection drops in where the sun uses ortho) → replay the game's own opaque draw lists
(`draw_opaque_scene_lists`, `:1201`) → `resolve_pass` yields the reversed-Z depth = the shadow map.
The anti-popping clip bypass, two-sided casters, small-caster cull, and depth-only color-write skip
all apply unchanged.

The one structural difference: **a point light is omnidirectional.** A fire on a totem casts Link's
shadow radially, so a single frustum will not cover it. Options, cheapest first:

- **Single perspective (1 pass)** — only if we cheat and shadow one hemisphere (e.g. toward the
  camera, or downward onto the ground). Cheapest; misses shadows on the far side of the light.
- **Dual-paraboloid (2 passes)** — the standard efficient omni map; mild distortion, no cube
  plumbing. **Recommended default.**
- **Cube map (6 passes)** — most correct, 6× the per-light cost. Reserve for a quality toggle.

### The binding constraint: aurora's fixed per-frame streaming budget

This is the same wall the sun mod hit: aurora streams all replayed geometry into fixed, non-growable
per-frame buffers whose overflow is an unconditional `abort()` (see realtime_sun_shadows.md — 3 sun
cascades already risk it on dense scenes). Every extra replay adds to that budget, so N lights ×
faces is the real cost question.

**The reframe that makes it workable: local lights have small radius.** A fire's `mPow` is ~500–700
world units; a sun *far* cascade spans 8000–16000 and streams the entire visible world. With a
**sphere cull** (adapt the existing light-column cull to reject shapes whose bounds fall outside the
light's `mPow` sphere), each local face streams only geometry within a few hundred units — a tiny
fraction of one sun cascade. So a dual-paraboloid pair for one tightly-culled fire can stream *less*
total geometry than a single sun cascade, even though it is two passes. The budget danger is not one
fire; it is **many overlapping lights** (a torch-lined corridor) or an unusually large-radius light.

Two costs do not shrink with radius and must be respected:
- **Fixed per-pass overhead** — `create_pass`/`resolve_pass`, GX state save/restore, pipeline
  binding. 4–12 passes/frame is real overhead independent of geometry. Bound N hard.
- **Map resolution** is free of the streaming budget (Map Size never touches it), and local lights
  affect a small screen area, so 512–1024 per face is plenty — keep them small.

**Practical envelope:** v1 shadows the **single nearest** qualifying light with a dual-paraboloid
pair at 512–1024, sphere-culled — 2 extra small replays/frame, comfortably inside budget. A config
`localLightCount` (1–4) scales it, carrying the same "dense scenes can exceed the geometry budget —
back off in those areas" caveat the 3-cascade sun path already documents.

## 3. Compositing — the genuinely hard part

This is where local-light shadows differ fundamentally from sun shadows, and it caps the achievable
quality.

**The sun mod dodges the hard problem.** Its composite outputs a flat `value = 1 - strength *
occlusion` and multiply-blends (`res/shadow.wgsl:486`). It never removes a *specific* light term —
it just darkens occluded pixels, and that reads as shadow because the sun is the dominant light.

**A local light cannot be dodged the same way for free, because its contribution is already baked
into scene color — entangled with albedo, per object.** The investigation confirmed TP computes the
warm glow on the **CPU per object**: `settingTevStruct_plightcol_plus` picks the nearest registered
`pointlight[]` and folds its color into the object's diffuse/ambient *before* texturing. There is no
GX channel per torch and **no separate light buffer** — the same limitation the VBGI plan flags
("the game's ambient is baked into the composited scene color… needs a game-linked fetch of TP's
light registers or an upstream service extension"). So we cannot cleanly isolate "light i's
contribution to this pixel" and mask just that.

Two ways forward:

### Option A — modulated darken (recommended for v1)

Per pixel, for shadowing light *i*: reconstruct world position (scene depth) and normal (from the
**Depth to Normal service** — three-line integration, `docs/depth_to_normal_consumers.md`), then
estimate how much light *i* *should* reach this pixel:

```
reach_i = attenuation(dist, L.mPow) * max(dot(n, dir_to_light), 0)   // 0..1
```

Where the light's shadow map says the pixel is occluded, darken by that estimated reach:

```
scene *= 1 - strength * occlusion_i * reach_i * tint(L.mColor)
```

Pixels the fire barely lights (far, back-facing) barely darken; brightly-lit pixels near the fire
darken most — so the shadow appears only where the fire's light actually falls, which is visually
"the fire's light is blocked here." It is the sun mod's trick, *modulated by the local light's
estimated reach* instead of a global strength, and it composes as another `SCENE_AFTER_OPAQUE`
multiply (so Deferred Fog still layers correctly). **Caveat:** it darkens the pixel's *total* color
(ambient + albedo), not only the point-light term, so a shadow in a fire-lit area is slightly too
dark and slightly desaturates. In practice, near a bright fire at night the point light dominates
locally, so it reads acceptably — the same reason the sun's uniform darken works. This is the
pragmatic, shippable path.

### Option B — suppress + deferred re-light (physically cleaner, not recommended for v1)

Follow the Deferred Fog pattern: hook `settingTevStruct_plightcol_plus` to **suppress** light *i*'s
CPU color add during the opaque lists (so the scene renders without it), then **re-add** it as a
screen-space deferred pass that includes the shadow term. This would correctly remove only the point
term. **Why it's hard:** re-adding it correctly needs the receiver's **albedo** to modulate — which
we do not have (scene color is already lit) — and TP adds the glow to *material color before TEV*,
not as a separable per-pixel Lambert, so a screen-space re-light won't match the original look. The
result is that *every* fire's appearance would change even with shadows off. Viable only if paired
with an albedo buffer (an upstream service extension or a game-linked albedo fetch), which is a
larger project. Note it as the "correct but expensive" upgrade path, not v1.

Either option needs the shadow map from §2; they differ only in the composite.

## 4. Occluders, bias, filtering — inherited

Everything else is already solved in the sun mod and transfers: the J3D replay hooks (clip bypass so
off-frustum casters still render, `GXSetCullMode` rewrite + `drawFast` genMode re-issue for
two-sided casters, small-caster cull), reversed-Z depth, PCF via `textureGather`, slope-scaled +
normal-offset bias. Point-light shadows want **distance-based bias** (bias scales with the
perspective map's depth non-linearity) — an adaptation of the existing per-cascade normalized bias,
not new machinery. The Bend screen-space-shadow pass is directional (traces toward one light) and
does not generalize cheaply to N omnidirectional lights; leave it sun-only.

## Standalone mod + service export

Build it as its **own** game-linked mod (`mods/local_light_shadows/`, proposed id
`dev.automata.local_light_shadows`), not folded into Realtime Sun Shadows. Two reasons the requester
prefers this — dissatisfaction with the current sun mod, and local-light shadows being a smaller,
more self-contained problem (small radii, no cascades, most relevant *indoors* where the sun mod
auto-disables) — and one reason the earlier "merge it in" recommendation was wrong:

**The hook-conflict worry does not survive scrutiny — the replay hooks self-gate.** The sun mod's
pre-hooks (`J3DUClipper::clip` bypass, `GXSetCullMode` rewrite, `J3DShape::drawFast` genMode
re-issue, `GXCopyTex` skip) are every one of them guarded by a per-frame flag that is only live
*inside that mod's own replay bracket* — `on_frustum_clip_pre` returns `HOOK_CONTINUE` unless
`g_replayingSceneLists` is set (`mod.cpp:1084`), which the `replay_scope` RAII toggles on only around
the replay (`mod.cpp:380`). Two independently-installed mods each hooking the same J3D functions
therefore coexist cleanly: during the sun mod's replay its hooks act and the local mod's are inert
no-ops (one bool check), and vice versa; during normal game rendering both are inert. The hook
framework chains multiple pre-hooks on one target, so registration does not clash. The only real
cost is *duplicated hook code to maintain* in two mods — which a shared internal header
(`replay_primitive.hpp`: "render these draw-lists from this camera into this offscreen map") factors
out without coupling the mods at runtime. Do keep both mods' replay brackets on the game thread and
non-overlapping (each is a synchronous `create_pass → replay → resolve_pass`), which they naturally
are as stage callbacks.

**The mod exports a service.** Beyond drawing its own visible shadows, Local Light Shadows publishes
a screen-space **local-light occlusion** buffer as a mod-exported service — the exact
`depth_to_normal` pattern (`mods/depth_to_normal/include/depth_to_normal_service.h`):

```c
// include/local_shadow_service.h  (consumer-facing)
struct LocalShadowFrame { WGPUTextureView occlusion; uint32_t width, height; /* r8/r16f: 1=lit, 0=shadowed */ };
// provider: EXPORT_SERVICE(LocalShadowService, ...); get_frame(ctx, &out) returns a frame-valid view
```

The same per-pixel occlusion term drives both the mod's own composite (the visible shadow) and any
consumer (SSILVB). Publishing it as a service — rather than relying on scene-color darkening being
picked up implicitly — is what makes the SSILVB integration correct and order-independent (next
section).

## Integration with SSILVB (making the GI bounce respect local shadows)

SSILVB (`mods/ssilvb/` on `claude/dusklight-platform-rebuild-rqhsaw`; docs `ssilvb_plan.md`) is the
shipped visibility-bitmask GI mod. Its indirect light is gathered by marching the depth buffer and,
at each marched sample, reading that sample's **outgoing radiance** `c_j` from a MIP chain of the
opaque scene-color snapshot (`res/preprocess_color.wgsl`, linear-decoded at
`preprocess_color.wgsl:116`; sampled in `res/ssilvb.wgsl:219` `load_sample_radiance`). The requester
wants Local Light Shadows to feed its shadow into that light input so the bounce is more accurate:
the patch of ground a torch *should* light but which is shadowed by an object should spill **less**
warm bounce. This is a strong, clean synergy — for three concrete reasons grounded in SSILVB's code.

**1. The effect the user wants is source-side, and source-side is exactly `c_j`.** The shadowed
ground's radiance *is* the `c_j` a marched sample reads. Attenuating the light-input MIP chain at
shadowed pixels reduces `c_j` there → the shadowed ground bounces less light. Nothing else in the
march changes.

**2. It survives `chromaLift` — the two do not collide.** SSILVB already has a knob (`chromaLift`,
`composite.wgsl:166` `albedo_proxy`) that exists *because* shadow mods darken the color it samples
and muddy the albedo estimate. But `chromaLift` is **receiver-side** (it normalizes the *receiver's*
albedo proxy at composite). The local-shadow term multiplies into the **source** radiance
(`preprocess_color`), a cleanly separate stage `chromaLift` never touches. So the shadow "sticks" in
the bounce instead of being normalized away — unlike scene-color darkening picked up implicitly.

**3. SSILVB already captures the fire's *glow* separately, so Local Shadows only handles surface
shadowing.** Emissive particles (torch fire, fairy glow) are not in the opaque snapshot; SSILVB
captures them via a late-frame delta reprojected into light-input MIP 0 (`emissiveBounce`,
`preprocess_color.wgsl:127–140`). Local Shadows must therefore attenuate the **opaque** radiance
*before* that emissive add, so the fire's own emission is **not** self-shadowed (an object's shadow
doesn't fall on the flame). Concretely, the multiply goes between the decode at
`preprocess_color.wgsl:116` and the emissive `radiance +=` at `:140`.

### The wiring (against real SSILVB code)

Use the **service export**, not implicit scene-color pickup. SSILVB resolves its color snapshot at
the very top of its `SCENE_AFTER_OPAQUE` hook (`mod.cpp:968`), live at the call point; whether a
Local Shadows composite has darkened the scene by then is **uncontrollable same-stage ordering** (the
SSILVB plan flags this explicitly in §5.1), and even if it landed it would double-hit the receiver
proxy. The service makes it deterministic and one-directional:

- **Provider (Local Light Shadows):** publish the screen-space `occlusion` view via
  `LocalShadowService::get_frame` (r8/r16f, 1 = lit, 0 = locally shadowed), full render resolution.
- **Consumer (SSILVB), a ~10-line change:** `IMPORT_OPTIONAL_SERVICE(LocalShadowService, ...)`; fetch
  the view in `on_scene_after_opaque` next to the Depth-to-Normal fetch; thread one `WGPUTextureView`
  through `ComputePayload` (a spare slot — the payload has room under its 128-byte `static_assert`);
  bind it at the free `@binding(10)` on `g_colorMip0Layout` (currently uses 0,1,2,8,9); and in
  `preprocess_color.wgsl` multiply `radiance *= textureLoad(local_shadow, coord, 0).r` on the opaque
  radiance (before the emissive add), gated by a new flag bit that is 0 when the service is absent —
  so SSILVB stays fully functional standalone. This is the same optional-service shape SSILVB already
  uses for Depth to Normal and explicitly plans for a future albedo provider (`ssilvb_plan.md` §8).

### Caveats to set expectations

- **The bounce shadow is soft, not crisp.** The light-input MIP chain box-averages MIP 0 into MIPs
  1–4, and the chain runs **half-res** by default with a 4-phase jitter, so distant samples read a
  blurred shadow. Fine for diffuse GI (and consistent with how all of SSILVB's light input behaves),
  but do not expect sharp shadow lines *in the bounce* — the crisp shadow is the mod's own direct
  composite; SSILVB only needs the low-frequency "this area is darker" signal.
- **Ghosting on moving lights.** SSILVB's GI output is temporally accumulated (~8-frame tail), so a
  moving torch's bounce shadow trails slightly — the same tradeoff as all SSILVB output, not
  specific to this integration.
- **No double-darkening by construction.** The service term is the *single* channel by which local
  shadows enter SSILVB's light input; SSILVB must not *also* sample a post-Local-Shadows-composite
  scene color for that purpose. Since SSILVB's opaque snapshot is taken before the translucent phase
  and the service is applied once in `preprocess_color`, this holds as long as Local Shadows' own
  visible composite is not double-counted into the light input — which the service design guarantees.

This integration is optional and independent: Local Light Shadows ships and draws its own shadows
with or without SSILVB; SSILVB runs with or without Local Light Shadows. Installed together, the
bounce gains local-shadow awareness for a ~10-line consumer change and one exported view.

## Recommended v1 scope

1. Read the game's own current shadow source: `dKy_plight_near_pos()` for the position, and
   `g_env_light.mPlayerPLightIdx` to fetch the driving `pointlight[idx]` (`mPosition` + `mPow` +
   `mColor`); consult `shadow_mode`/`field_0x10a0` for the lantern/effect override. That single light
   is the N=1 case (faithful to vanilla). For N>1, additionally enumerate
   `g_env_light.pointlight[0..99]`, filter `mPow > min`, sort by camera distance, take the next
   nearest (config `localLightCount` 0–4, default 1; 0 = off).
2. Render a **dual-paraboloid** pair per shadowed light at 512–1024, **sphere-culled** to `mPow`,
   reusing `replay_cascade`'s bracket with a perspective projection.
3. Composite with **Option A** (modulated darken) at `SCENE_AFTER_OPAQUE`, using Depth-to-Normal for
   the receiver normal and per-light attenuation for reach.
4. Distance-based bias adapted from the existing bias controls; PCF inherited.
5. UI group "Local Light Shadows": enable, count, map size, radius scale, strength, bias, cull knobs
   — mirroring the sun panel's structure and inert-when-off convention.
6. Export the per-pixel `occlusion` buffer as `LocalShadowService` (the `depth_to_normal` pattern) —
   the same term the composite already computed, published for SSILVB. Land the ~10-line SSILVB
   consumer change (optional import + `@binding(10)` in `preprocess_color`) as a paired follow-up.

This proves the whole pipeline at ~2 extra small replays/frame and a bounded, well-understood
composite. Scaling N and adding a cube-map quality toggle are natural follow-ups; Option B (deferred
re-light) is a separate, larger project gated on an albedo source. Build order suggestion: get the
mod's own visible shadows right first (steps 1–5), then add the service export + SSILVB hook (6) once
the occlusion term is stable — the service is a thin publish of an already-computed buffer.

## Risks / open questions

1. **Streaming budget under many lights** — the dominant risk. Mitigated by sphere-cull + small N +
   the same "back off in dense areas" guidance as 3-cascade sun. Needs in-game validation in a
   torch-dense interior.
2. **Composite realism (Option A)** — darkens total color, not only the point term; validate it
   reads right near bright fires and doesn't crush ambient elsewhere. `strength` and the reach
   attenuation curve are the levers.
3. **Flicker** — fires carry `mFluctuation`; drive the shadow geometry from the stable `mPosition`,
   not a flickered value, so shadow edges don't jitter (the *brightness* may still flicker via the
   composite, which is correct).
4. **Indoors** — unlike the sun (auto-disabled indoors), local lights are *most* relevant indoors
   (torch-lit dungeons). This mod should run indoors — the opposite gate from the sun path, and a
   further reason it is cleaner as a standalone mod than merged into the indoor-disabled sun mod.
5. **The distortion-particles interaction** (open bug: the sun shadow **map** disturbs heat-haze /
   steam particles) will very likely reproduce, since it stems from the offscreen replay leaving GX
   state dirty — the same replay this feature adds more of. Widening the GX save/restore around the
   replay would fix both at once and should be scoped in alongside.
6. **ABI coupling** — game-linked; `LIGHT_INFLUENCE` / `dScnKy_env_light_c` offsets must be
   re-verified after any re-platform (there is a `STATIC_ASSERT(sizeof(dScnKy_env_light_c) == 4880)`
   upstream that will catch a layout shift at compile time).
7. **SSILVB integration is a soft, low-frequency signal, by design** — the bounce shadow is
   MIP-averaged and half-res (§Integration), so it reads as "this area bounces less," not as crisp
   shadow lines in the GI. That is the correct expectation to set; the crisp shadow is the mod's own
   direct composite. The consumer change lives in SSILVB (on the platform-rebuild branch), so it is
   versioned with SSILVB, not this mod — coordinate the `LocalShadowService` header/version across
   both. The service ABI is a mod-to-mod contract (a texture view + dims), independent of the game
   ABI, so it is stable across game re-platforms.
