# The game's names are romanized Japanese — and our mods are built on them

Twilight Princess was written by a Japanese team, and the decompilation Dusklight
is built from (`zeldaret/tp`) reproduces the original build 1:1 — **including its
symbol names**. Every identifier our game-linked mods hook, read or include is a
Japanese word written in Latin letters, an abbreviation of one, or English spelled
by ear.

That is not trivia for this repo. **Effect Remover, Realtime Sun Shadows and Graphics
Hub's Deferred Fog are entirely built out of these names** — `drawCloudShadow`,
`mMoyaMode`, `dKy_bg_MAxx_proc`, `mpVrkumoPacket`, `dKy_Indoor_check`,
`dComIfGd_drawOpaListBG`. Reading one of them as English has already put a wrong
sentence into our own documentation (§4.1).

This document is **self-contained on purpose**: a mod session attaches only
`dusklight-mods`, so it cannot assume the game tree is already present. §2.1 says how to
fetch it. **This file is the reference** — longer versions exist in the older fork
`dusklight-ao` / `aurora-ao` forks, but those repos are no longer the platform and are
historical only.

**Status.** Everything in §4 was verified against the game tree at the pinned
`DUSKLIGHT_VERSION` — re-verified line by line after the move to upstream Dusklight, so the
citations match the source the mods are actually built against. §5 says which of the
questions this document raised are now answered and which are still open.

Findings here have since driven mod changes, not just prose: the `mFogDensity` reading
corrected Effect Remover's terrain feature, "Projected Shadow Removal" was renamed **Haze
Removal** because moya is not a projected shadow, and Realtime Sun Shadows' `drawCloudShadow`
hook was removed once that name was read correctly. Where a section says something is
unresolved, that is meant literally — do not build on it without checking.

---

## 1. The rule, short

1. **A game name is a Japanese word until proven otherwise.** `kankyo` (環境) is
   *environment* — hence `dKy_`, `d_kankyo.cpp`, `kytag`. `moya` (靄) is mist/haze.
   `kumo` (雲) is cloud. `kage` (影) is shadow.
2. **Never rename or "correct" a game symbol.** The tree also contains English
   spelled by ear, and it is load-bearing: `wether` (weather), `Schejule`,
   `Sord`, `parcent`, `resorce`, `tresure` — and, in the draw-list header our shadow
   mod replays, `dComIfGd_setListInvisisble` (`d_com_inf_game.h:4671`) sitting 189
   lines above the correctly-spelled `dComIfGd_drawOpaListInvisible` (`:4860`).
   Renaming breaks the match with `zeldaret/tp` and every grep anyone runs.
3. **Search in both romanizations.** The tree mixes kunrei-shiki (`si`, `tu`, `ti`,
   `sya`, `zi`) with Hepburn (`shi`, `tsu`, `chi`, `sha`, `ji`) *for the same word*.
   Spray is `Sibuki` in the C functions (21 hits) and `shibuki` in the effect-ID
   table (69 hits, and **zero** `sibuki`). Droplet is spelled `shizuku` 29 times and
   `sizuku` 26 times **in one file**. Either spelling alone finds none of the other
   half.
4. **A header field name is not an authored name.** Function and global-data symbols
   are the original team's; struct *member* names were reconstructed by the
   decompilation. A member name is a hypothesis until an authored string agrees with
   it. This is the rule that caught our own error — see §4.1.
5. **Our code stays English.** Everything in `mods/` is ours and uses ordinary
   `snake_case`/`camelCase`. The convention describes the code we *read*.
6. **Gloss on first use** in any document here — *moya* (靄, mist) — then use it
   bare. A reader who does not know the word cannot look it up, because it is not
   English.

---

## 2. How to actually read the game's Japanese from a mods session

This is the part that changes how sessions work: where the tree is, how to search it
without getting a wrong answer, and what to search *for*.

### 2.1 Getting the game tree

The mods repo does not contain the game. `cmake/FetchDusklight.cmake` clones it
into **`dusklight/`** (git-ignored) at the pinned `DUSKLIGHT_VERSION` during a
CMake configure, so after any `cmake -B build` the whole game source is sitting
there and is greppable. If you only want to read it — no build — clone it directly:

```sh
DUSKLIGHT_VERSION=$(sed -n 's/.*set(DUSKLIGHT_VERSION "\([0-9a-f]*\)").*/\1/p' CMakeLists.txt)
mkdir -p dusklight && git -C dusklight init --quiet
git -C dusklight remote add origin https://github.com/automata-rtx/dusklight-ao.git
git -C dusklight fetch --depth=1 origin "$DUSKLIGHT_VERSION"
git -C dusklight checkout --quiet FETCH_HEAD
```

That is the same repository and the same commit `cmake/FetchDusklight.cmake` uses, so a
plain `cmake -B build` produces an equivalent tree. `DUSKLIGHT_DIR` can also point at an
existing checkout.

> **The fetched tree is `automata-rtx/dusklight-ao` at `DUSKLIGHT_VERSION`** — the
> scene-normal-buffer platform, not stock upstream, so the SHA above will not resolve
> against `TwilitRealm/dusklight` and fetching from there hard-fails. Its **game code is
> stock upstream**, though — the fork delta is renderer + SDK only — so every line citation
> in §4 stands unchanged and anything you grep here matches upstream. (CLAUDE.md's "The ABI
> pin" names the exact upstream base; it is deliberately not repeated here, so there is one
> place to update.)

### 2.2 Searching it — use ripgrep, not `grep -P`

**496 files under the game's `src/` and `include/` contain literal kana/kanji.**
They are the original team's own debug-panel labels, `OS_REPORT` strings and CSV
headers — the single most authoritative documentation of what a field means,
because it is the authors labelling their own fields.

The reason nobody on this project had ever read them is one environment variable.
This container's locale is `POSIX` (`locale` prints `LC_CTYPE="POSIX"`), and under
it the three obvious ways to search for Japanese fail in three **different** ways.
Measured on the game tree, this container:

| Command | POSIX | `LC_ALL=C.UTF-8` | Failure mode |
| :-- | --: | --: | :-- |
| `grep -rlP '\p{Han}' src include` | **0** | 427 | **silent** — exit 1, no error, reads as "not there" |
| `grep -rlP '[\x{3040}-\x{30ff}\x{4e00}-\x{9fff}]' src include` | **0** | 496 | errors to *stderr*, exit 2; stdout empty, so a pipeline reads zero |
| `grep -rlP '[ぁ-んァ-ヶ一-龥]' src include` | **507** | 496 | **worst** — matches byte ranges, 11 false positives, and *looks* like it worked |
| `rg -l '\p{Hiragana}\|\p{Katakana}\|\p{Han}' src include` | **496** | 496 | none |

**So: use ripgrep** — `rg`, and the Claude Code `Grep` tool, which is ripgrep. It is
correct under either locale, including `\p{…}` classes. If you must shell out to
`grep -P`, `export LC_ALL=C.UTF-8` first.

An empty `grep -P` for Japanese is not evidence of absence, and a non-empty one is
not evidence of presence.

Two more ways a search comes back empty or wrong, both measured on this tree:

- **`rg -r` is `--replace`, not "recursive".** `rg -rn 'bloom' src` rewrites every match
  to the letter `n` and prints that, so a real hit reads as nonsense rather than as an
  error. ripgrep already recurses by default; you never need `-r`. This misfired twice
  while auditing, once making a class called `bloom_c` look like one called `n_c`.
- **A polygon code may be compared one character at a time.** The game frequently tests
  `mat_name[3] == 'M' && mat_name[4] == 'A' && mat_name[5] == '0' && mat_name[6] == '9'`
  instead of `memcmp(&mat_name[3], "MA09", 4)`. Grepping for the string `MA09` misses
  every one of those sites. `dKy_cloudshadow_scroll` — the function the terrain-shadow
  work is built on — is matched this way (`d_kankyo.cpp:4504-4507`). When a code seems
  unhandled, also search `\[5\] == '` and `\[6\] == '`.

### 2.3 The method: find where the game labels its own field

The game ships the debug panels its developers used. `d_kankyo.cpp` alone carries
**291** `genSlider` calls, each one a machine-readable binding:

```
genSlider("<Japanese label>", &<live game field>, <min>, <max>)
```

a label written by the people who authored the game's look, the exact variable, and
the range they considered sane. **When a game name is ambiguous, find its slider.**
That is how §4.1 was settled in minutes after months of a wrong description.

```sh
rg -n 'genSlider\("|genLabel\("|genCheckBox\("' dusklight/src/d/d_kankyo.cpp | rg '<the field you care about>'
```

---

## 3. Glossary — every game name our mods touch

Glossed because these are the names in `mods/effect_remover/src/mod.cpp`,
`mods/realtime_sun_shadows/src/mod.cpp` and `mods/graphics_hub/src/mod.cpp`.

### Prefixes

| Prefix | Expansion | What it means for us |
| :-- | :-- | :-- |
| `dKy_` / `dKyw_` / `dKyr_` / `dKyd_` | **kankyo** 環境 = environment; `w` = wether (weather), `r` = the particle draw routines, `d` = data tables | the whole environment system all three game-linked mods read |
| `dScnKy_` | d **Scn** (scene) **Ky** (kankyo) | `dScnKy_env_light_c` — the environment state struct, i.e. `g_env_light` |
| `dComIfGd_` | d_com_inf **G**ame **d**raw | the draw-list façade Realtime Sun Shadows replays |
| `dComIfGp_` / `dComIfGs_` | …**p**lay process / **s**ave data | `dComIfGs_getTime` |
| `fopAcM_` | f_op **Ac**tor **M**anager | actor lifecycle |
| `J3D*`, `JUT*`, `JKR*`, `JPA*` | **JSystem**, Nintendo's shared middleware | the model/material/loader layer Effect Remover's vertex unbake patches |
| `kytag` | 環境 (kankyo) + tag | the invisible per-area actors that set `mMoyaMode` |

### Words

| Romaji | Japanese | Meaning | Where it reaches our mods |
| :-- | :-- | :-- | :-- |
| kankyo | 環境 | environment | everything `dKy_*` |
| moya | 靄 | mist, haze | `mMoyaMode` / `mMoyaCount` — Effect Remover's **Haze Removal** (`er_psr`) switches on exactly this |
| kumo | 雲 | cloud | `drawCloudShadow` is the moya packet; 雲影 *kumokage* = cloud shadow |
| vrkumo | VR box + 雲 | the drifting skybox cloud packet | `mpVrkumoPacket` — its translation is what scrolls the terrain shadow overlay `er_tsr` removes |
| kasumi | 霞 | horizon haze band | `vrbox_kasumi_*_col`; **`outer` is the near band, `inner` is the far one** — the opposite of the English |
| kage | 影 | shadow | the game's own word; 雲影 cloud shadow, リアル影 "real shadow" |
| ese | えせ | **fake, phoney** | the game's own label for its fake point lights (`d_kankyo.cpp:5185`) |
| nama | 生 | **raw, live** | the game's own label for its placed lights (`d_kankyo.cpp:5165`) |
| wether | *(English by ear)* | weather | `dKyw_wether_move` — do not "fix" |
| sibuki / shibuki | 飛沫 | spray, splash | `dKyr_drawSibuki`; the grep-trap example |
| housi / houshi | 胞子 | spore | `dKyr_housi_init` — drifting motes; the authors' own panel heading 「胞子の調整パラメータ」 sits directly above its sliders (`d_kankyo.cpp:7753`) |
| taiyou | 太陽 | sun | 「太陽の調整パラメータ」 (`d_kankyo.cpp:7742`); `setSunpos` writes `sun_pos` |
| tsuki | 月 | moon | 「月の調整パラメータ」 (`d_kankyo.cpp:7732`); `setSunpos` also writes `moon_pos` |
| vectle | *(English by ear)* | **vector** | `dKyr_get_vectle_calc` — do not "fix" |
| Schejule | *(English by ear)* | **schedule** | do not "fix" |

**Celestial Orbit's vocabulary, and one trap in it.** That mod retilts the sun and moon
path by post-hooking `dScnKy_env_light_c::setSunpos()` (`d_kankyo.cpp:1666`), which writes
both `sun_pos` and `moon_pos`. The trap: **`dKyr_drawSun` draws the moon as well as the
sun** — `d_kankyo_wether.cpp:61` calls it with the moon's own texture resource
(`&mpResMoon`) — so it is the shared celestial-billboard drawer, not a sun-only routine.
`dKyr_drawStar` is a separate function for the starfield, and `dKyr_draw_rev_moon`
(`d_kankyo_rain.cpp:2082`) is a third, distinct path that reads `moon_pos` directly.
Reading `drawSun` as "the sun's drawing code" will send you to the wrong function.

### Terrain material vocabulary — the `MAnn` codes

The game calls these **`ポリゴンコード`, "polygon codes"** (`d_kankyo.cpp:4972-4973`).
We have been calling them "material codes"; both are fine, but the game's term is
the one to grep for.

A terrain material name is `xxxMAnn[_Suffix]`, matched at name offsets `[3..6]`. The
suffix after the code is **descriptive romaji**:

| Suffix | Japanese | Meaning |
| :-- | :-- | :-- |
| `_Gake` | 崖 | cliff |
| `_Kusa` | 草 | grass |
| `_Enkei_…` | 遠景 | distant scenery |
| `_Nami` | 波 | wave |
| `_Nigori…` | 濁り | turbidity, murk |
| `_Mizugiwa` | 水際 | water's edge, shoreline |
| `_Kasan` | 加算 | **addition** — an additively blended pass |
| `_Mera` | めら | shimmer |

**The granularity trap that matters for `er_tsr`:** a code alone is coarser than the
name. `MA06` covers the waves, the shoreline **and** the murk. Our per-code toggles
(`MA00`/`MA01`/`MA04`/`MA16`) are therefore blunt by construction — see §5.

When adding any classifier over these names, prefer matching `_Word` and `Word` (the
convention lowercases after the code and capitalises inside a compound) over a bare
substring, so `minami` is not read as `nami` — and make "unrecognised" mean **leave
it alone**.

---

## 4. What the lens has already found here

Verified against the game tree at the pinned `DUSKLIGHT_VERSION`. Each item gives
the citation so it can be re-checked cheaply rather than believed.

### 4.1 `mFogDensity` is not fog density — it is **cloud-shadow** density — **DOC CORRECTED**

Effect Remover's Terrain Shadow Removal (`er_tsr`) post-hooks `dKy_bg_MAxx_proc` and
overwrites TEV KColor register 1's red channel. Our documentation described the value
the game puts there as "**env fog density**" (`docs/fake_shading_systems.md` §2, and
the `er_tsr` bullet in `CLAUDE.md`).

The game says otherwise, three independent ways:

```
d_kankyo.cpp:5003    genSlider("雲影の濃さ ", &g_env_light.mFogDensity, 0, 0xff);
                                 ^ "cloud shadow density"      ^ the decomp called it mFogDensity
d_kankyo.cpp:4511    k_color.r = g_env_light.mFogDensity & 0xFF;   // dKy_cloudshadow_scroll, MA00 only
d_kankyo.cpp:11456   sp5C.r = (u8)g_env_light.mFogDensity;         // dKy_bg_MAxx_proc, MA00/01/04/16
```

The authors' own slider calls it cloud-shadow density; one of its two consumers is a
function the game itself named `dKy_cloudshadow_scroll`; the other is the terrain
material pass. Nothing in either path is fog. **`mFogDensity` is a decompilation-
assigned member name, and it is wrong** — which is exactly rule 4 in §1.

**What this changes:** it makes `er_tsr` legible. The feature is not fighting an
incidental fog term that happens to land on the terrain — **it is overriding the
game's own cloud-shadow strength control**, the same value the swaying overlay is
scrolled by. The mod and the game agree about what the system is; only our
description of it did not.

**The polarity, and how the game settles it.** 濃さ means density/darkness, so a naive
reading predicts 255 = *darker*, while our in-game test found the opposite — 0 darker,
255 washed out — which is why `er_tsr` pins 255. The game's own use of the value decides
between them:

```
d_stage.h:150        u8 cloud_shadow_density;      // the palette column mFogDensity loads from
d_kankyo.cpp:2423    mFogDensity = kankyo_color_ratio_set(..., cloud_shadow_density, ...)
d_kankyo.cpp:2427    if (daPy_py_c::checkNowWolfPowerUp()) { mFogDensity = -1; }
```

Two things follow. First, the value is loaded straight out of a palette column the
decompilation independently named `cloud_shadow_density` — a *second reconstruction*
agreeing with the authored slider. (It is a member name, so by rule 4 it is corroboration,
not proof; the authored 雲影の濃さ slider remains the actual evidence.) Second, and more
useful: in the wolf's enhanced-senses state the game **forces the value to `-1`, which the
terrain pass reads as `255`** (`sp5C.r = (u8)mFogDensity`, `:11456`) — and it does that in
the same routine that flattens the rest of the look for that mode. The game drives this
value to 255 precisely when it wants the cloud shadow gone. That is the same direction our
in-game test measured, and it means **255 is the engine's own "no cloud shadow" value**, not
merely a setting that happens to look right.

What is still unread is the TEV equation itself, which lives in the `.bmd` material rather
than in the source tree. Reading it would explain *why* 濃さ runs this direction; it would
not change what 255 does. So `er_tsr` pinning 255 is **corroborated by the engine's own
usage** — a materially stronger position than the "known-effective, not known-faithful"
this document used to record.

**Corrected: two documentation sentences** (`docs/fake_shading_systems.md` §2 and the
`er_tsr` bullet in `CLAUDE.md`). **No mod code changed.** `er_tsr` behaves exactly as
it did; whether it *should* is a question for the audit, not a doc edit. Documented is
not done.

### 4.2 The game names its own fakery, and it is the distinction our mods exist on

The original team's light panel labels its light slots in Japanese
(`src/d/d_kankyo.cpp`):

| Line | Label | Reading |
| --: | :-- | :-- |
| 5165 | `● 生ライト` | **nama** light — *raw / live*: the placed lights |
| 5185 | `（ライト０）―えせポイントライト` | **ese** point light — *fake / phoney* |
| 5186 | `（ライト１）―エフェクトライト` | effect light |
| 5191 | `※太陽が存在する場合、設定は上書きされます` | "overwritten if a sun exists" → slot **2 is the sun** |
| 5204 | `※月が存在する場合、設定が上書きされます` | "overwritten if a moon exists" → slot **3 is the moon** |
| 5359 | `えせライト地形反映特別版` | the "terrain-reflecting **fake** light" |

Two things follow. First, **light slots 2 and 3 are the sun and the moon, stated by
the game** — directly relevant to Realtime Sun Shadows, which derives its light
direction independently. Second, the vocabulary our whole mod suite is organised
around (fake shading vs real shading) is the game's own vocabulary, and using its
words makes claims checkable instead of aspirational.

Similarly for shadows: `d_bg_s.cpp:48,57` calls the projected geometry shadows
**リアル影** ("real *kage*"), and many actors carry a `real_shadow_size` slider, while
`d_s_play.cpp:361-368` exposes 影表示 / 影濃さ / 影イメージ表示 / 影ポリゴン表示
(shadow display / density / image / polygon) and `d_kankyo.cpp:7574` gives the
projected shadow a 通常α / 接近ＭＡＸα pair — a base alpha and a close-up alpha.
**"Blob shadow" is a coinage from elsewhere in this project, not the game's word**;
the game's simple shadow class has no Japanese name anywhere in the tree.

### 4.3 The material name is live in release, at the draw site

Relevant to any future mod that wants per-draw intent instead of a texture hash:

```
J3DModelLoader.cpp:21       AssignMaterialNames(), guarded by  #if TARGET_PC
J3DModelLoader.cpp:134      called from J3DModelLoader::load()      — the .bmd path ONLY
J3DModelLoader.cpp:175      J3DModelLoader::loadBinaryDisplayList() — .bdl: never calls it
J3DPacket.cpp:218, :245     the two readers, both guarded `if (mMaterialName != nullptr)`
```

So the game's own semantic label for a draw — `MA00_Gake`, `MA00_Kusa`, and the
`MA06` water variants — is available to a game-linked mod at draw time in a release
build **on the `.bmd` load path**. This is a stronger per-draw identity channel than
anything screen-space, and nothing in this repo uses it.

> ⚠️ **Do not read `J3DMaterial::mMaterialName` unguarded.** `AssignMaterialNames` is
> called from `J3DModelLoader::load()` and from nowhere else;
> `J3DModelLoader::loadBinaryDisplayList()` (the `.bdl` path) never calls it, and
> `J3DMaterial` never initialises the field, so on that path it is *indeterminate* rather
> than null. The game's own two readers both null-check it before use, and a mod must do
> at least the same. An earlier revision of this section claimed the field was "populated
> in EVERY PC build"; that was wrong and would have led a mod into dereferencing an
> uninitialised pointer.
>
> **The safe route is the model data's name table**, which is what the game itself uses
> in `dKy_bg_MAxx_proc`: `modelData->getMaterialName()->getName(i)`
> (`d_kankyo.cpp:11359-11360`). It is populated on both load paths.

(Full material names such as `cc_MA06_NigoriWater_v_x` live in `.bmd` asset data, not
in the source tree, so the checker in §6 cannot verify them and this document does
not assert any particular one. `MA00_Gake` and `MA00_Kusa` are the exceptions — those
two are string literals in `d_a_bg.cpp` and are checked.)

### 4.4 The draw-list taxonomy is a game-authored classification

Realtime Sun Shadows replays `dComIfGd_drawOpaList`, `…ListBG`, `…ListDark`,
`…ListDarkBG`, `…ListMiddle` and reads `dComIfGd_getXluListBG`. Those lists are not
arbitrary buckets — `dKy_bg_MAxx_proc` **sorts terrain materials into them by polygon
code** (`d_kankyo.cpp:11367-11376`). Of the water family it matches:

| Code | Routed to |
| :-- | :-- |
| `MA03`, `MA09` | `dComIfGd_setListDarkBG` |
| `MA19` | `dComIfGd_setListInvisisble` *(the game's spelling)* |
| `MA17` | neither — left in whatever list it was in |

The full family also includes `…ListSky`, `…ListFilter`, `…ListInvisible` and
`…ListZxlu`. Which lists a shadow-map replay includes therefore decides which
*material classes* cast — a question that reads as a rendering detail and is actually
a content classification the game already made.

### 4.5 The game already runs a per-material fog override, keyed on the same codes

Found while reading §4.1's neighbourhood, and it lands on Deferred Fog (a standalone mod —
Graphics Hub, which used to host it, is retired).

`setLightTevColorType_MAJI_sub` (`d_kankyo.cpp:4231`) is the per-material light/TEV/fog
setup every BG material goes through. Its fog block (`:4432-4487`) does not simply apply
the environment fog: at `:4466-4479` it reads the material's **authored**
`J3DFogInfo::mType` and treats two values as sentinels —

| authored `mType` | what the game does |
| :-- | :-- |
| `7` | rewrites the type to `2` and forces the fog colour to **black** |
| `6` | forces the fog colour to **white** |
| anything else | uses `tevstr_p->FogCol`, the environment fog colour |

— and `dKy_bg_MAxx_proc` **writes exactly those two values**, by polygon code
(`d_kankyo.cpp:11376-11408`): `MA09` gets `mType = 6`, the rest of the water family gets
`mType = 7`. Start/end Z come from the tevstr and near/far from the view, so the *range*
is shared while the *colour* is not.

So "the game's fog" is not one configuration. **Water surfaces are deliberately fogged
to black or white while everything else is fogged to the palette colour**, and the
selector is a material name. That black is applied to the water's *own* colour before it is
blended over the riverbed — which is what makes deep water darken with distance, and why a
deferred fullscreen pass cannot reproduce it. See `docs/deferred_fog.md`, "Blended draws inside
the opaque lists". Deferred Fog reverts to vanilla on "mixed fog configs"; this
says the mixed case is not an edge case, it is the game's design for a whole material
class, and it is identifiable by name rather than by inspecting state.

**Settled — the sentinel is a live per-frame override, not an asset property.** The call
order is visible at the room-terrain draw site, two adjacent lines:

```
d_a_bg.cpp:338   g_env_light.setLightTevColorType_MAJI(bg_model, bgPart->tevstr);  // translates
d_a_bg.cpp:339   dKy_bg_MAxx_proc(bg_model);                                       // re-stamps
```

The translation runs **first** and the polygon-code pass runs **second**, so the type the
translation writes is immediately overwritten. The same order holds at the other call
sites (e.g. `d_a_obj_groundwater.cpp:265-269`).

Two consequences worth having straight:

- The `7 → 2` rewrite at `d_kankyo.cpp:4467` **never reaches the GPU for these materials**.
  Its colour side-effect (black) persists, but the type is re-stamped to `7` before the
  draw, so water is drawn with GX fog type 7 (and `MA09` with 6), not type 2. Any reasoning
  that starts "type 7 becomes linear fog" is wrong for the terrain water family.
- Because the codes are re-stamped every frame, the black/white water fog is keyed purely
  on material name at run time. That is why a view containing water always carries more
  than one fog configuration — see `docs/deferred_fog.md`.

**Still not established:** whether the same ordering holds for the non-terrain callers of
`dKy_bg_MAxx_proc` under every actor's own draw sequence. The two checked agree; the
remaining five were not traced.

---

### 4.6 `XFog` is the fog **range adjustment**, and it is on everywhere — **DOC CORRECTED**

Found while auditing Deferred Fog for vanilla accuracy.

`GxXFog_set` (`d_kankyo.cpp:9463`), `dKyd_xfog_table_set` (`d_kankyo_data.cpp:775`) and
`S_xfog_table_data` (`:766`) are authored names, and the *X* is the **screen x axis** — this
is GX's fog range adjustment, the per-column multiplier the hardware applies to the fog term
because a pixel at the screen edge is genuinely further from the eye than a centre pixel at the
same Z. Read as "extra fog" or "cross fade" it disappears; read correctly it is one function
call away from `GXSetFogRangeAdj`, which is the only place it can be.

What the reading found:

- `envcolor_init` (`d_kankyo.cpp:1240`) turns it on at environment init — `:1257-1260`:
  `mFogAdjEnable = true`, table type `0`, centre `0x140` = 320, the middle column of the
  640-wide logical framebuffer (corroborated by `:1110`, which seeds the debug mirror with the
  literal `320`) — and **nothing ever clears `mFogAdjEnable`**. The 2D/ortho passes call
  `GXSetFogRangeAdj(GX_DISABLE, ...)` directly, but they carry no fog anyway, and every world
  fog set re-arms it.
- Every path that sets fog re-arms it. `GxXFog_set()` runs immediately after each of the three
  direct setters (`:9416`, `:9438`, `:9460`), and `setLightTevColorType_MAJI_sub` copies the
  same globals into every BG material's `J3DFogInfo` (`:4481-4484`) so `J3DFog::load()` re-issues
  it per material.
- Aurora implements it: its fog-range LUT builder bakes one multiplier per target column and
  the generated fragment shader applies it to `a / (b − z)` *before* subtracting `c`.

`docs/deferred_fog.md` asserted the opposite — that the game sets range adjustment but aurora
ignores it, "so the deferred pass correctly ignores it too". That was false on this pin, and it
is why the deferred pass flattened a horizontal gradient vanilla has for as long as it did. The
mod now reproduces it.

### 4.7 `dBgp_c` is "bg **parts**" — map units, and they do not draw like anything else

Also from the Deferred Fog audit. `dBgS`/`dBgW` are the **collision** system; `dBgp_c`
(`d_bg_parts.cpp`) is unrelated to them despite the shared prefix — it is the shared, instanced
**map units** a stage is assembled from, which in the field is most of the distant scenery.

The reason it matters to us is a draw-path fact, not a naming one, but the name is what leads
you there: `dBgp_c::modelMaterial_c::drawSimple` (`:20`) calls `mpMaterial->loadSharedDL()` and
then walks the shape's matrix groups calling `J3DShapeDraw::draw()` **directly** — never
`J3DShape::drawFast`. Any mod that intercepts J3D drawing at `drawFast` silently misses it. See
`docs/deferred_fog.md`, "Draw paths that do not go through `J3DShape::drawFast`".

---

## 5. The questions this document raised, and where each one stands

This section was written as a list of things the naming lens raised but nobody had
checked. Most have since been checked. Each entry says plainly whether it is **answered**
or **still open**, so nothing here reads as coverage it does not have.

**Answered.**

- **`er_tsr` KColor polarity.** 255 is the engine's own "no cloud shadow" value: the game
  forces `mFogDensity = -1` (read as `255`) in the wolf's enhanced-senses state, where it
  flattens the look deliberately. See §4.1. The TEV equation in the `.bmd` is still
  unread, but it cannot change what 255 *does*.
- **`er_psr` mode coverage — and this entry's own premise was wrong.** It claimed the
  mode↔area mapping is stage data and so not derivable from source. It is derivable:
  every `mMoyaMode` value is assigned in code. Two consequences for the mod's UI, both
  verified: **mode 4 is set only by `d_a_kytag02` (`:27`, `:121`)**, the scripted
  wind-gust tag — not by anything ambient — and **Hyrule Field's haze is mode 7**, set by
  stage name in `d_kankyo_wether.cpp:1111` (`F_SP121`, which the port's own map table
  names Hyrule Field, `src/dusk/map_loader_definitions.h:68`), alongside `F_SP108` Faron
  Woods and `F_SP127` the fishing hole. So the mod's advice to leave mode 4 on "so Hyrule
  Field keeps its drifting shadows" is wrong twice over.
- **Deferred Fog vs the game's other fog.** `ウソFog` does reach terrain, but **not as
  fog**: it is written as a TEV constant into `MA13`/`MA14`/`MA16`/`MA20`
  (`d_kankyo.cpp:11588-11652`). Deferred Fog intercepts `GXSetFog`, `GFSetFog` and the J3D
  material fog block, none of which this is, so it can neither suppress nor defer this term — on
  those materials a fog-coloured tint is baked into the surface before any mod composites over it.
  It is also, for the same reason, **not** a candidate explanation for a deferred-vs-vanilla
  difference: a TEV constant is applied before the fog stage in both paths and cancels. `bg_amb_col[1].a` (水面α) and
  `bg_amb_col[2]` (補佐) are likewise consumed by the water polygon codes in
  `dKy_bg_MAxx_proc`, not applied globally.
- **VBAO / SSILVB and the ambient layers — the framing was wrong.** These are not four
  stacked layers blended over all terrain. `bg_amb_col[0]` is the general BG ambient
  (`NewAmbColGet`, `d_kankyo.cpp:9969`); `[1]`, `[2]` and `[3]` are special-purpose terms
  consumed by specific polygon codes, as above. **None of them is an occlusion term**, so
  our AO is not double-applying one. Interesting, but not currently actionable for tuning.
- **Realtime Sun Shadows and light slots 2/3.** They agree. Light slot 2's position is
  `sun_pos` (`d_kankyo.cpp:8574`), and the shadow mod's own derivation mirrors
  `setSunpos()`. Since Celestial Orbit landed, they agree *by construction*: that mod
  rewrites `sun_pos`/`moon_pos` and publishes the same orbit as a service, which the
  shadow mod imports — see `docs/celestial_orbit.md`.

**Still open.**

- **`er_tsr` per-code toggles vs the suffix.** `MA00`/`MA01`/`MA04`/`MA16` is the code
  granularity; the game authors a descriptive suffix on the same names. Whether the suffix
  is a better handle is still unmeasured. The game itself acts on a suffix in only one
  place (`d_a_bg.cpp:378-383`, gated to the fishing-hole stages `F_SP127`/`R_SP127`), so it
  is a *weaker* signal in game code even though it is authored everywhere.
- **Effect-name mining.** `src/d/d_particle_name.cpp` carries thousands of descriptive
  effect names, frequently romanized opposite to the C code. Only spot-read so far — enough
  to find the `kagerou` (陽炎, heat haze) family including `ZI_S_screenKagerou01`, and to
  identify `d_a_ep` as the torch stand from its own event strings (`SHOKUDAI`, 燭台), with
  `ep_hahen_s` = 破片, fragment. A full pass has never been done.

> **This section predates Celestial Orbit**, which was written after it and is built
> entirely on the game's sun/moon vocabulary. Re-read §5 against all seven current mods
> rather than assuming it covers them.

---

## 6. Checks

```sh
python3 tools/check_japanese_naming.py
```

Verifies that every game symbol this document names still exists in the fetched
`dusklight/` tree, so the glossary cannot rot silently as the platform pin moves. It
**skips cleanly** when `dusklight/` is absent (a plain checkout with no CMake
configure) rather than failing — but it will not catch anything either, so run it
after a configure, or point `DUSKLIGHT_DIR` at a checkout.

---

## See also

- `docs/fake_shading_systems.md` — the three fake-shading systems Effect Remover targets,
  plus (§4) four more the same game function sets up that we do not
- `docs/celestial_orbit.md` — the sun/moon vocabulary in §3, in use
- **Historical only, in the older fork branches:** `dusklight-ao/docs/japanese-naming.md` and
  `aurora-ao/docs/japanese-naming.md`. Neither repo is the platform any more. The aurora
  one describes reading a baked TEV meaning out of a generated shader — still the right
  method for the one thing §4.1 leaves unread, but aurora is now vendored inside the
  fetched game tree at `dusklight/extern/aurora`, so go there rather than to the fork.
- `docs/deferred_fog.md`, `docs/realtime_sun_shadows.md` — the two mods most exposed
  to this vocabulary
