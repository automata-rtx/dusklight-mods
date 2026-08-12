# Twilight Princess's fake-shading systems (what Effect Remover disables)

TP has almost no realtime lighting: the world looks "lit" because the shading is **faked** and
baked in several different ways. Our realtime mods (Realtime Sun Shadows, SSILVB) add real shading
*on top* of that fake shading, so the fakes have to be turned down or they fight the new look
(double-darkening, shadows that don't move with the sun, flat ambient that never changes).

This doc is the map of those fake systems: what each one looks like in-game, **what it's called in
the game code**, how we disable it, and which **Effect Remover** feature (and namespace in
`mods/effect_remover/src/mod.cpp`) owns it. Read this first before touching any of the removal code
in a future session — the three systems look similar on screen but are completely different in code.

Effect Remover targets **three** systems. The game has **at least seven** — the single function
`er_tsr` hooks sets up four more on its own (§4). Do not read this document as an inventory of
everything TP fakes; it is an inventory of what we currently remove.

A shade you see on the ground could be any of them, so the identification step (each feature has a
logger) matters.

---

## 1. "Moya" (靄, *mist/haze*) — a camera-facing haze veil, **not** a ground shadow

> **Correction, and it matters for how this feature is described.** This section previously
> called moya "projected particle ground shade" and the mod's UI still says so. It is not
> projected onto anything. Verified in the pinned tree:
>
> - The quads are built from the **inverse of the view rotation** — `MTXInverse(dComIfGd_getView()
>   ->viewMtxNoTrans, camMtx)` (`d_kankyo_rain.cpp:4543`, inside `drawCloudShadow` which begins at
>   `:4514`) — i.e. camera-facing billboards that follow the camera, not geometry on the ground.
> - They are drawn with the **depth test and depth write both disabled**:
>   `GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE)` (`d_kankyo_rain.cpp:4594`). A surface-projected
>   shadow cannot be depth-independent; this is an overlay.
> - **Five of the twelve modes blend additively** — modes 3, 4, 6, 10 and 11 take
>   `GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, ...)` (`d_kankyo_rain.cpp:4587`), whose
>   destination factor is `ONE`. Those modes can only ever *brighten* the frame. They are glare,
>   dust and storm haze — they are not shadows of any kind.
>
> The dappled shade you see on a forest floor is a different system entirely: it is §2, a texture
> stage baked into the terrain material. `er_tsr`'s own notes record that the moya count reads 0 on
> that terrain, which is consistent.

**What it looks like:** drifting haze, mist, dust and steam hanging in the air, plus — at
`mMoyaMode >= 50` — the full-screen heat-shimmer / wolf-senses distortion, which is the same
function's other branch.

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
  the default + UI hints): mode 5 is the only pure non-wind *slow sway*.
- **Which mode runs where is decided in code, not in map data** — every `mMoyaMode` value is
  assigned by a `kytag` actor or by the weather code, so the mapping is greppable. Two that the
  mod's UI currently gets wrong: **mode 4 is set only by `d_a_kytag02` (`:27`, `:121`)**, the
  scripted wind-gust tag, and **Hyrule Field's ambient haze is mode 7**, set by stage name in
  `d_kankyo_wether.cpp:1111` (`F_SP121`) alongside `F_SP108` (Faron Woods) and `F_SP127` (the
  fishing hole). The UI's "leave mode 4 on so Hyrule Field keeps its drifting shadows" is wrong on
  both halves — mode 4 is not Hyrule Field's, and being additive it is not a shadow.

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
  KColor register 1**'s **red channel** to `g_env_light.mFogDensity` on the
  `MA00`/`MA01`/`MA04`/`MA16` materials. **This red channel is the shadow's wash-out control.**

  **`mFogDensity` is not fog density.** The name is a decompilation reconstruction; the original
  team's own slider calls the field **雲影の濃さ — "cloud shadow density"** (`d_kankyo.cpp:5003`),
  and its only other consumer is `dKy_cloudshadow_scroll` (`d_kankyo.cpp:4511`), the function that
  scrolls this same overlay. So this feature is not fighting an incidental fog term that happens to
  land on terrain — **it is overriding the game's own cloud-shadow strength control.** See
  `docs/japanese-naming.md` §4.1.

**Key gotcha (learned in-game):** KColor 1's red behaves as a *wash-out*, **not** a darkener.
Forcing it to **0** makes the shade **darker**; **maximum** washes it out. So removal = **pin it to
255**, not zero.

**Polarity — corroborated by the game itself.** 濃さ means density/darkness, so a naive reading
predicts 255 = darker, and our in-game test found the opposite. The game settles the direction: in
the wolf's enhanced-senses state it forces `mFogDensity = -1`, which the terrain pass reads as
`255` (`d_kankyo.cpp:2427`, consumed at `:11456`) — i.e. the engine drives this value to maximum
exactly when it wants the cloud shadow gone. **255 is the engine's own "no cloud shadow" value**,
which is what `er_tsr` pins. The TEV equation in the `.bmd` is still unread and would explain
*why* 濃さ runs this way, but cannot change what 255 does. See `docs/japanese-naming.md` §4.1.

**How Effect Remover disables it (`er_tsr`):** a **post-hook on `dKy_bg_MAxx_proc`** (runs right
after the game sets the register, so our value is the one that draws) that, for the enabled material
codes, rewrites **TEV KColor 1 to `{255,0,0,0}`** — the red channel swizzles to white in the shadow
stage, so the overlay stops darkening while the **base ground texture (stage 0) is untouched** (no
holes; an earlier "skip the whole shape" approach holed the floor and was abandoned). Per-code
toggles (`MA00`/`MA01`/`MA16`/`MA04`) + a logger of which codes a room uses (the Faron spot logs
~72 `MA04` materials). **Off by default** — it's a global terrain change; verify per area.
**The hook's reach is wider than "room terrain".** `er_tsr` post-hooks `dKy_bg_MAxx_proc`, and
seven actors call that function, not one — verified in the pinned tree:

| Caller | What it is |
| :-- | :-- |
| `d_a_bg.cpp:339` | room terrain (the intended target) |
| `d_a_obj_groundwater.cpp:268-269` | large ground-water bodies |
| `d_a_obj_onsen.cpp:92, :96` | 温泉 *onsen*, hot spring |
| `d_a_bg_obj.cpp:1303` | moving background objects |
| `d_a_obj_bubblePilar.cpp:192` | bubble pillar |
| `d_a_obj_gb.cpp:24` | — |
| `d_a_demo00.cpp:1529` | cutscene object |

So the previous claim here that water "is never touched" was too broad. It is true of the two
actors it named — `d_a_obj_waterfall` and `d_a_obj_lv3Water` genuinely never call
`dKy_bg_MAxx_proc` — but two *other* water actors do. Whether the wash actually changes their
appearance depends on whether their materials carry an `MA00`/`MA01`/`MA04`/`MA16` code, which
lives in `.bmd` asset data and cannot be settled from source; use the feature's own material
logger in-game to find out.

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

## 4. Fake shading we do **not** remove (found in the same function `er_tsr` already hooks)

These are listed so nobody reads §1–3 as an inventory of everything TP fakes. All are set up
inside `dKy_bg_MAxx_proc`, keyed on polygon code, and none is currently exposed by Effect Remover.
Verified in the pinned tree; none has been evaluated in game.

| Code(s) | What the game does | Where |
| :-- | :-- | :-- |
| `MA02`, `MA10` | Builds a **camera-projected texture matrix** (`C_MTXLightPerspective`) and routes the material to the Invisisble list — a screen-locked projected overlay | `d_kankyo.cpp:11424-11450` |
| `MA11` | In the Twilight (`dKy_darkworld_check`) re-routes to `setListDarkBG` and forces a purple TEV colour — the twilight mist tint | `d_kankyo.cpp:11479-11490` |
| `MA20` | Forces fog type 7 (black), takes its colour from the `ウソFog` layer, and builds a `cMtx_lookAt` projection anchored to **the player's position** — a mask that follows Link | `d_kankyo.cpp:11582-11620` |
| `MA13`, `MA14`, `MA16` | Written the authors' own **`ウソFog`** ("fake fog") ambient term as a TEV constant; `MA14` additionally receives the real fog colour | `d_kankyo.cpp:11622-11652` |

The last row matters for Graphics Hub's Deferred Fog: because `ウソFog` is a TEV constant baked
into the material rather than a `GXSetFog` call, **Deferred Fog can neither suppress nor defer
it**. On those materials a fog-coloured tint is part of the surface before any mod composites over
it. See `docs/japanese-naming.md` §5.

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

Line numbers below were re-verified against the pinned tree (upstream `TwilitRealm/dusklight` at
`DUSKLIGHT_VERSION`); re-check them after any re-platform.

- `src/d/d_kankyo_rain.cpp` — `drawCloudShadow` (4514), `cloud_shadow_move` (1585),
  `vrkumo_move` (1845); the moya mode branch at 4587 and its depth-off state at 4594.
- `src/d/d_kankyo.cpp` — `dKy_cloudshadow_scroll` (4491), `dKy_bg_MAxx_proc` (11344),
  `dKy_murky_set` (11236); `mFogDensity` loaded from the palette at 2423 and forced to `-1` at 2427.
- `src/d/actor/d_a_bg.cpp` — room terrain load (`model.btk`/`model.brk`); the ordered pair
  `setLightTevColorType_MAJI` (338) then `dKy_bg_MAxx_proc` (339); the one suffix-matching block,
  gated to the fishing-hole stages, at 378-383.
- The moya mode assignments: `d_a_kytag00.cpp`, `d_a_kytag02.cpp:27, :121` (mode 4),
  `d_a_kytag06.cpp` (modes 10/11), `d_kankyo_wether.cpp:1111` (mode 7).

> A previous revision listed "the `MA08` special texmtx path (~587)" in `d_a_bg.cpp`. **There is no
> `MA08` anywhere in `src/` or `include/`** — that landmark pointed at code that does not exist and
> has been removed.
- `include/d/d_kankyo.h` — `dKy_getEnvlight`, `dKy_bg_MAxx_proc`; `dScnKy_env_light_c` layout
  (`mMoyaMode`, `mMoyaCount`).
