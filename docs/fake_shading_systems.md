# Twilight Princess's fake-shading systems (what Effect Remover disables)

TP has almost no realtime lighting: the world looks "lit" because the shading is **faked** and
baked in several different ways. Our realtime mods (Realtime Sun Shadows, SSILVB) add real shading
*on top* of that fake shading, so the fakes have to be turned down or they fight the new look
(double-darkening, shadows that don't move with the sun, flat ambient that never changes).

This doc is the map of those fake systems: what each one looks like in-game, **what it's called in
the game code**, how we disable it, and which **Effect Remover** feature (and namespace in
`mods/effect_remover/src/mod.cpp`) owns it. Read this first before touching any of the removal code
in a future session — the three systems look similar on screen but are completely different in code.

There are **three independent systems**. A shade you see on the ground could be any of them, so the
identification step (each feature has a logger) matters.

---

## 1. "Moya" — projected particle ground shade

**What it looks like:** soft, drifting patches of shade projected onto the ground — the slowly
swaying dappled shadow under a forest canopy (Faron/Ordon), the big rolling cloud shadows drifting
across Hyrule Field, and drifting mist/dust/steam. It is a *particle field* projected downward, not
attached to any single surface.

**Code names:**
- `drawCloudShadow(Mtx, u8**)` in `d/d_kankyo_rain.h` — the draw call for the whole moya packet.
- `dScnKy_env_light_c::mMoyaMode` (`u8`, offset `0x0EB5`) — **which** moya effect is active. Set
  **per area** by the map's `kytag` actors, so the number-to-effect mapping is map data, not fixed
  in code. `mMoyaMode >= 50` is a different effect entirely (framebuffer heat-shimmer /
  wolf-senses distortion) and must never be touched.
- `dScnKy_env_light_c::mMoyaCount` (`int`, offset `0x0EB8`) — density; `0` means moya isn't drawing
  here (so if a shade persists with count 0, it is **not** this system — check system 2).
- `dKy_getEnvlight()` returns `&g_env_light` (linkable) — how we read the two fields above.
- Behaviour reference: `cloud_shadow_move` in `d_kankyo_rain.cpp` (mode → motion mapping used for
  the default + UI hints): mode 5 is the only pure non-wind *slow sway* (canopy candidate); modes
  4/11 are wind-driven drift (the rolling field shadows).

**How Effect Remover disables it (`er_psr`):** a **pre-hook on `drawCloudShadow`** that returns
`HOOK_SKIP_ORIGINAL` only when the current `mMoyaMode`'s per-mode toggle is on. Skipping a
self-contained immediate-mode draw is safe. Default removes **only mode 5** and keeps the rest, so
Hyrule Field keeps its rolling shadows. A live logger prints `mMoyaMode`/`mMoyaCount` on change so
any spot's mode can be read off in-game.

---

## 2. Terrain shadow overlay — animated texture stage inside the ground material

**What it looks like:** a drifting dappled shade *painted onto the terrain itself* that sways gently
back and forth even when you stand still. The confirmed case is the **Faron forest floor**. It
survives moya removal (moya count reads 0 there) because it is a completely different system: the
shadow is a **second TEV texture stage baked into the terrain material**, over the base ground
texture (the "two textures interacting" the ground shows).

**Code names:**
- Terrain materials are named `"??MAcc.."` — the 4-char code at name positions `[3..6]` is the
  discriminator (`MA00`, `MA01`, `MA04`, `MA16`, …). The room terrain actor is `d_a_bg`.
- `dKy_cloudshadow_scroll(J3DModelData*, dKy_tevstr_c*, int)` in `d_kankyo.cpp` — scrolls **texture
  matrix 1** of the `MA00`/`MA01`/`MA16` materials by the drifting cloud (`vrkumo`) packet
  translation (`g_env_light.mpVrkumoPacket->field_0x1150/0x1154`). **This scroll is the sway.**
- `dKy_bg_MAxx_proc(void* bg_model)` in `d/d_kankyo.h` (public symbol) — per frame, sets **TEV
  KColor register 1**'s **red channel** to the environment fog density (`g_env_light.mFogDensity`)
  on the `MA00`/`MA01`/`MA04`/`MA16` materials. **This red channel is the shadow's wash-out
  control.**

**Key gotcha (learned in-game):** KColor 1's red is a *wash-out*, **not** a darkener. Forcing it to
**0** makes the shade **darker**; **maximum** fog density washes it out. So removal = **pin it to
255**, not zero.

**How Effect Remover disables it (`er_tsr`):** a **post-hook on `dKy_bg_MAxx_proc`** (runs right
after the game sets the register, so our value is the one that draws) that, for the enabled material
codes, rewrites **TEV KColor 1 to `{255,0,0,0}`** — the red channel swizzles to white in the shadow
stage, so the overlay stops darkening while the **base ground texture (stage 0) is untouched** (no
holes; an earlier "skip the whole shape" approach holed the floor and was abandoned). Per-code
toggles (`MA00`/`MA01`/`MA16`/`MA04`) + a logger of which codes a room uses (the Faron spot logs
~72 `MA04` materials). **Off by default** — it's a global terrain change; verify per area.
Waterfalls/water are separate actors (`d_a_obj_waterfall`, `d_a_obj_lv3Water`) with their own
object-archive BTKs and are never touched.

---

## 3. Baked vertex lighting — colors painted into the geometry

**What it looks like:** the general "lit" feel of vanilla scenes — crevice darkening, interior
gradients, painted pools of torchlight. It is hand-authored **per-vertex color** that rasterizes
into the color channel and multiplies the textures. It's what makes the world look shaded even
though nothing is casting a real shadow, and it's the flat ambient our realtime GI is trying to
replace.

**Code names:**
- `J3DModelLoaderDataBase::load` / `loadBinaryDisplayList` in
  `JSystem/J3DGraphLoader/J3DModelLoader.h` — the single funnel every BMD/BDL model passes through
  on load.
- The vertex colors live in the model's `CLR0`/`CLR1` arrays (`J3DVertexData::getVtxColorArray`,
  `getVtxArrNum`, `getVtxArrStride`, `getVtxAttrFmtList`), in one of six GX color formats
  (`GX_RGBA8`, `GX_RGBX8`, `GX_RGB8`, `GX_RGB565`, `GX_RGBA4`, `GX_RGBA6`).

**How Effect Remover disables it (`er_vu`):** a **post-hook on both loader entry points** that
rewrites each loaded model's `CLR0`/`CLR1` arrays **in place**: `rgb' = mix(white, rgb,
vertexLight/100)`. `vertexLight` 100 = vanilla (no change), 0 = fully flat white (texture-only base
for the realtime stack); alpha is never touched. Because it patches at load, the change applies to
models loaded **after** the setting changes — re-enter the area (or reload the save) to see a new
value. Scope note: the hook sees *every* J3D model (rooms, props, characters), so decorative vertex
tinting on props flattens too.

---

## Which system is which? (in-game triage)

1. Turn on **Projected Shadow Removal → Log Active Mode**. Walk to the shade.
   - If the log shows a moya **mode with count > 0**, it's **system 1** — toggle that mode off.
   - If the log shows **count 0**, moya isn't drawing it → go to step 2.
2. Turn on **Terrain Shadow Removal → Log Overlay Materials** and **Enabled**.
   - If the "seen" count rises (e.g. `seen 72 (MA04)`), it's **system 2** — it'll wash out.
3. If neither logger reacts and the whole scene just looks flatly pre-shaded (not a discrete
   moving shadow), that's **system 3** — lower **Vertex Lighting**.

## Related game-source landmarks (in the fetched, read-only `dusklight/` tree)

- `src/d/d_kankyo_rain.cpp` — `drawCloudShadow` (~4506), `cloud_shadow_move` (~1583), `vrkumo_move`.
- `src/d/d_kankyo.cpp` — `dKy_cloudshadow_scroll` (~4490), `dKy_bg_MAxx_proc` (~11347),
  `dKy_murky_set` (~11236).
- `src/d/actor/d_a_bg.cpp` — room terrain load (`model.btk`/`model.brk`), `dKy_bg_MAxx_proc` call
  site (~339), the `MA08` special texmtx path (~587).
- `include/d/d_kankyo.h` — `dKy_getEnvlight`, `dKy_bg_MAxx_proc`; `dScnKy_env_light_c` layout
  (`mMoyaMode`, `mMoyaCount`).
