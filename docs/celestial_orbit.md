# Celestial Orbit — raising the sun/moon travel path

Twilight Princess never lifts the sun above **59 degrees**. That ceiling is the single
biggest limit on what realtime sun shadows can express: the light is slanted even at high
noon, so shadows stay long and raked all day and the sun never reads as "overhead". This
mod re-derives the tilt of the orbit from a peak-elevation knob (up to 80 degrees) without
touching the sweep, so the clock and every time-of-day transition are unchanged.

Game-linked: it post-hooks `dScnKy_env_light_c::setSunpos` and rewrites `sun_pos` /
`moon_pos`, so it is coupled to the pinned game build.

## Where the cap comes from

`dScnKy_env_light_c::setSunpos()` (`src/d/d_kankyo.cpp`) places both bodies on a circle
around the camera eye:

```cpp
pos.x = sinf(DEG_TO_RAD(sun_angle)) * 80000.0f;
pos.y = cosf(DEG_TO_RAD(sun_angle)) * 80000.0f;
pos.z = cosf(DEG_TO_RAD(sun_angle)) * -48000.0f;   // <- the cap

sun_pos  = eye + (pos.x, -pos.y, pos.z);   // absolute
moon_pos =       (pos.x, -pos.y, pos.z);   // offset only, not absolute
```

`y` and `z` are both driven by `cos(a)`, so the path is a **great circle tilted off
vertical**, and with `z/xy = 48000/80000 = 0.6` its highest point is

```
peak = atan(1 / 0.6) = 59.036 degrees
```

Nothing else limits it. The angle sweep `a` covers a full circle; the tilt is the whole
story.

## The transform

Written as an offset from the eye, the vanilla position for swept angle `a` and radius
`R = 80000` is

```
x = sin a * R        y = -cos a * R        z = -cos a * R * 0.6  ==  y * 0.6
```

so **`z` carries no information the caller does not already have in `y`**. Re-deriving it
from `y` with a different ratio is exactly "retilt the orbit plane": the path stays a great
circle through the same two horizon points, swept by the same angle at the same time of
day, but its peak rises to `atan(1 / ratio)`. Inverting that gives the knob:

```
ratio = cot(peak elevation)      // 0.600006 at 59.036 -> vanilla to six decimals
```

An optional **yaw** then spins that retilted plane about world Y, moving where the bodies
rise and set without changing any elevation.

Both steps live in **one function**, `celestial_orbit_apply_offset()` in
`include/celestial_orbit_service.h`, which the provider and every consumer call — see
"Keeping the shadows in sync" below.

## What moves as a result

Everything downstream of `sun_pos` / `moon_pos`, which is the whole point:

| Consumer | Path |
| :-- | :-- |
| **Day light** | `dKy_setLight()` copies `sun_pos` → `sun_light_pos` every frame; `SetBaseLight` uses it as `base_light.mPosition` while `67.5 < daytime < 292.5` |
| **Night light** | `SetBaseLight` uses `eye + moon_pos` outside that window |
| Weather light dir | `d_kankyo_rain.cpp` — `dKyr_get_vectle_calc(eye, sun_light_pos, …)` |
| Sun billboard, lens flare | `mpSunPacket` / `mpSunLenzPacket` |
| Moon billboard, reflected moon | `dKyr_drawStar` / `dKyr_draw_rev_moon` — `eye + moon_pos` |

The visible body and the light that casts shadows track together; there is no second place
to patch, and `48000.0f` appears nowhere else in the game source.

## What does not move

**Time of day and every state transition.** `daytime`, the palette schedule, and
dawn/dusk/night selection run off `dComIfGs_getTime()` and `l_time_attribute`; none of them
reads `sun_pos`, `moon_pos`, or the orbit. `SetBaseLight` picks sun vs moon from `daytime`
alone. There is no path from this mod's value to any timing.

Sunrise/sunset elevations barely move either, so those transitions look the same
(measured against this mod's own transform, hours on the game clock):

| Peak elevation | 06:00 | 08:00 | 12:00 | 16:00 | 18:00 |
| :-- | :-: | :-: | :-: | :-: | :-: |
| 59.036 (vanilla) | 14.8 | 36.9 | **59.0** | 36.9 | 14.8 |
| 59 (the knob's lowest vanilla-ish setting) | 14.8 | 36.8 | **59.0** | 36.8 | 14.8 |
| 75 (default) | 15.0 | 39.3 | **75.0** | 39.3 | 15.0 |
| 80 (ceiling) | 15.0 | 39.7 | **80.0** | 39.7 | 15.0 |

## Why 80 degrees is the ceiling

At exactly 90 the arc passes through the zenith: the azimuth flips instantaneously at noon
and shadows snap around. Anything that derives a stable azimuth from the light direction
degenerates as that direction approaches vertical — which a shadow map's view matrix does,
since its up vector is undefined for a straight-down light. 80 keeps a comfortable margin
and still reads as a proper overhead sun.

The floor is 15: the peak is the highest the body gets *all day*, and Realtime Sun Shadows
already fades shadows out below ~11 degrees of elevation, so a lower peak would mean a day
with essentially no realtime shadows in it.

## Keeping the shadows in sync

Realtime Sun Shadows does **not** read `sun_pos`. It mirrors `setSunpos`'s math itself
(`sun_moon_offset` / `sun_moon_angle`) so its debug time-of-day override moves the light
directly, and because the packet positions can be stale when it runs. That means rewriting
`sun_pos` alone would leave the shadows pointing at the vanilla sun while the visible sun
sits somewhere else.

So the modded orbit is published as a service:

- **`dev.automata.celestial_orbit`** (`include/celestial_orbit_service.h`) — `get_state()`
  returns `{active, sun_z_ratio, moon_z_ratio, yaw_sin, yaw_cos}`.
- Realtime Sun Shadows imports it with `IMPORT_OPTIONAL_SERVICE` (**soft** dependency: no
  provider, or the feature switched off, leaves the vanilla orbit) and pushes its own
  vanilla offset through the same `celestial_orbit_apply_offset()` the provider uses. One
  implementation, in the shared header, so the two cannot drift apart.

Any future mod that derives a light direction from the time of day should do the same. A
mod that simply reads `sun_pos` / `moon_pos` needs no changes at all.

## Idempotency (why the hook guards)

`setSunpos` bails out without writing when there is no camera, or on the `F_SP200` stage
(where the moon is placed absolutely instead). Re-tilting our own output on such a frame
would be harmless for the `z` lean — it is derived from `y`, which the transform never
changes — but the yaw would compound, spinning the sun a little further every frame. The
post-hook therefore:

1. mirrors `setSunpos`'s own camera / `F_SP200` guard, and
2. remembers the exact positions it last wrote and skips when the current values are
   bit-identical, which catches any other path that skips the write.

A frame where the game *did* write cannot be falsely skipped: for a vanilla write to equal
the previous transformed output, the transform would have to be a no-op on that value.

## Options

| Option | Default | Notes |
| :-- | :-- | :-- |
| `orbitEnabled` | on | Off = the positions are never touched, bit-exact vanilla (and the shadow mod falls back to the vanilla orbit). This is the only way to get the *exact* 59.036 vanilla tilt back, since the knob steps in whole degrees. **Not named `enabled`** — that key is reserved by the host for the mod manager's own per-mod checkbox, and using it made this mod fail to load entirely; see `docs/mod-api-notes.md` "Config/UI". The two differ in kind: the manager's checkbox unloads the mod, this one keeps it loaded and exporting its service while reporting the vanilla orbit. |
| `sunElevation` | 75 | Peak sun elevation, 15–80 degrees. |
| `moonElevation` | 75 | Peak moon elevation — the light that casts night shadows. |
| `orbitYaw` | 0 | Rotates the whole path about world Y, ±180 degrees. The painted sky and cloud art do not rotate with it, so large values can put the sun somewhere the backdrop does not expect. |
