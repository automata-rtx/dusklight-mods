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
`dusklight-mods`, so it cannot assume the game repo is open. The canonical, longer
reference is `dusklight-ao/docs/japanese-naming.md`; the parts of it a mod session
actually needs are restated here.

**Status: reference, not an audit.** Everything in §4 was verified against the game
tree at the pinned `DUSKLIGHT_VERSION`. §5 lists what has *not* been checked. No mod
code was changed to produce this document — two documentation sentences were, and
§4.1 says exactly which.

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
git clone --filter=blob:none https://github.com/automata-rtx/dusklight-ao.git dusklight
git -C dusklight checkout <DUSKLIGHT_VERSION from the top-level CMakeLists.txt>
```

`DUSKLIGHT_DIR` can also point at an existing checkout, which is what to do if the
session already has `dusklight-ao` attached.

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
| moya | 靄 | mist, haze | `mMoyaMode` / `mMoyaCount` — Effect Remover's Projected Shadow Removal (`er_psr`) switches on exactly this |
| kumo | 雲 | cloud | `drawCloudShadow` is the moya packet; 雲影 *kumokage* = cloud shadow |
| vrkumo | VR box + 雲 | the drifting skybox cloud packet | `mpVrkumoPacket` — its translation is what scrolls the terrain shadow overlay `er_tsr` removes |
| kasumi | 霞 | horizon haze band | `vrbox_kasumi_*_col`; **`outer` is the near band, `inner` is the far one** — the opposite of the English |
| kage | 影 | shadow | the game's own word; 雲影 cloud shadow, リアル影 "real shadow" |
| ese | えせ | **fake, phoney** | the game's own label for its fake point lights (`d_kankyo.cpp:5185`) |
| nama | 生 | **raw, live** | the game's own label for its placed lights (`d_kankyo.cpp:5165`) |
| wether | *(English by ear)* | weather | `dKyw_wether_move` — do not "fix" |
| sibuki / shibuki | 飛沫 | spray, splash | `dKyr_drawSibuki`; the grep-trap example |
| housi / houshi | 胞子 | spore | `dKyr_housi_init` — drifting motes |

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

**What it does not settle, and this matters.** The game's word 濃さ means
density/darkness, so a naive reading predicts 255 = *darker*. Our in-game test found
the opposite: 0 darker, 255 washed out, which is why `er_tsr` pins 255. Both facts
are solid and they are not yet reconciled — the TEV equation that consumes KColor1.r
lives in the `.bmd` material, not in the source tree. It is readable, and
`aurora-ao/docs/japanese-naming.md` §3 says where: aurora generates the WGSL for that
draw. Until someone reads it, "pin to 255" is **known-effective, not known-faithful**,
and the parenthetical "== max fog density, engine-faithful" in `CLAUDE.md` was
claiming more than we had.

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
J3DModelLoader.cpp:21-28   AssignMaterialNames()  under  #if TARGET_PC
                           → mat->mMaterialName is populated in EVERY PC build
J3DPacket.cpp:218-222      GXPushDebugGroup("Mat: %s")  under  #if DEBUG && TARGET_PC
                           → immediately before callDL(), i.e. the real draw
```

So the game's own semantic label for a draw — `MA00_Gake`, `MA00_Kusa`, and the
`MA06` water variants — is **available to a game-linked mod at draw time in a
release build**. Only the debug-group *push* is compiled out. This is a stronger
per-draw identity channel than anything screen-space, and nothing in this repo uses
it.

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

Found while reading §4.1's neighbourhood, and it lands on Graphics Hub's Deferred Fog.

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
selector is a material name. `hub_fog` reverts to vanilla on "mixed fog configs"; this
says the mixed case is not an edge case, it is the game's design for a whole material
class, and it is identifiable by name rather than by inspecting state.

**Not established:** the per-frame call order between `dKy_bg_MAxx_proc` and
`setLightTevColorType_MAJI_sub`, i.e. whether the value the translation reads on a given
frame is the authored one or the one the MAxx pass just wrote. Both write the same field
on the same materials. That ordering decides whether the sentinel is a static asset
property or a live per-frame override, and it should be settled before anything is built
on it.

---

## 5. Not checked — this is what the audit is for

Stated plainly so nobody reads the above as coverage. **None of these are claims;
they are the questions the lens raises about our current mods.**

- **`er_tsr` KColor polarity.** §4.1's unresolved half. Read the generated WGSL for
  an `MA04` draw and settle whether 255 is the faithful "no cloud shadow" value or
  merely the one that looks right.
- **`er_tsr` per-code toggles vs the suffix.** `MA00`/`MA01`/`MA04`/`MA16` is the
  code granularity; the game authors a descriptive suffix on the same names. Whether
  the suffix is a better handle is unmeasured. Note the game itself only acts on the
  suffix in one place (`d_a_bg.cpp:377-390`, gated to one stage), so it is a *weaker*
  signal in game code even though it is authored everywhere.
- **`er_psr` mode coverage.** `mMoyaMode` is set per area by `kytag` actors, i.e. the
  mapping is stage data, not code. Our default (remove mode 5 only) came from
  `cloud_shadow_move`'s motion table plus in-game observation. Whether the mode↔area
  mapping is complete is not established from the source tree.
- **Deferred Fog vs the game's *other* fog.** `hub_fog` defers `GXSetFog`-driven per-
  draw fog. The environment palette also carries an ambient alpha the authors labelled
  **`ウソFog`** — "fake fog" (`bg_amb_col[3].a`, `d_kankyo.cpp:5133`), alongside a
  water-surface alpha (`bg_amb_col[1].a`) and an auxiliary alpha (`bg_amb_col[2].a`).
  Whether any of that reaches a surface our deferred pass re-fogs is unchecked.
- **VBAO / SSILVB and the game's four ambient layers.** The game has one actor ambient
  (`actor_amb_col`) and **four** terrain ambient layers (`bg_amb_col[0..3]`), scaled by
  `bg_light_influence` (地形ライト影響率) and `mActorLightEffect` (影響率, 0–200). Our
  AO and GI composite over the finished image, i.e. over a blend of all five. What that
  means for tuning is unexplored.
- **Realtime Sun Shadows and light slots 2/3.** §4.2. The game states which slots the
  sun and moon occupy; our mod derives direction its own way. Whether they agree has
  not been checked.
- **Effect-name mining.** `src/d/d_particle_name.cpp` is 3,201 descriptive effect
  names covering the whole game, frequently romanized opposite to the C code. No
  session has read them.

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

- `dusklight-ao/docs/japanese-naming.md` — the canonical reference and the full
  glossary
- `aurora-ao/docs/japanese-naming.md` — what survives into GX, and how to read a
  baked TEV meaning out of a generated shader (the method §4.1 needs)
- `docs/fake_shading_systems.md` — the three fake-shading systems Effect Remover
  targets; §4.1 corrects one sentence of it
- `docs/deferred_fog.md`, `docs/realtime_sun_shadows.md` — the two mods most exposed
  to this vocabulary
