# Realtime Shadows for Local Lights — viability assessment

Investigation of a proposed mod that makes **local/point light sources** (a fire on a totem, wall
torches, bonfires, lanterns, glowing props) cast real-time geometry shadows — turning the warm glow
each one places in the vanilla scene into a shadow-casting light. This doc is a viability study, not
a shipped plan: it grades each subproblem and recommends a scope.

**Verdict: viable as a game-linked mod for a bounded number of nearby lights, reusing ~80% of the
Realtime Sun Shadows machinery — with one genuinely hard subproblem (compositing) that forces a
pragmatic "darken where occluded" approach rather than physically correct light removal.** Two of
the four subproblems are solved outright; two need care. Details below.

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

## Where it should live (and a hook-conflict warning)

Build it **into Realtime Sun Shadows** (or a shared internal static lib both consume) — **not** as an
independent sibling mod. The caster-replay hooks (`J3DUClipper::clip` bypass, `GXSetCullMode`
rewrite, `J3DShape::drawFast` genMode re-issue, small-caster cull) are non-trivial and already
installed by the sun mod; a second mod installing its **own** pre-hooks on the same J3D functions
would double-fire them (both hooks run per clip test / per drawFast), which is a real correctness and
performance hazard. One mod owning the replay hooks and driving both the sun cascades and the
local-light maps from a shared "render this draw-list from this camera into this map" primitive is
the clean structure. The mod's identity broadens from "Realtime Sun Shadows" to "Realtime Shadows."

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

This proves the whole pipeline at ~2 extra small replays/frame and a bounded, well-understood
composite. Scaling N and adding a cube-map quality toggle are natural follow-ups; Option B (deferred
re-light) is a separate, larger project gated on an albedo source.

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
   (torch-lit dungeons). This mod should run indoors, which is the opposite gate from the sun path —
   another reason to keep the two subsystems' enable logic separate within one mod.
5. **The distortion-particles interaction** (open bug: the sun shadow **map** disturbs heat-haze /
   steam particles) will very likely reproduce, since it stems from the offscreen replay leaving GX
   state dirty — the same replay this feature adds more of. Widening the GX save/restore around the
   replay would fix both at once and should be scoped in alongside.
6. **ABI coupling** — game-linked; `LIGHT_INFLUENCE` / `dScnKy_env_light_c` offsets must be
   re-verified after any re-platform (there is a `STATIC_ASSERT(sizeof(dScnKy_env_light_c) == 4880)`
   upstream that will catch a layout shift at compile time).
