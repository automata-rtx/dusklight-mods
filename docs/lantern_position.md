# Lantern Position

Moves where Link's lantern hangs on his belt while it is **lit but not in his hands** — the
placement you see the moment he draws his sword, pulls out the bow, or stands empty-handed with the
lantern still burning. The in-hand placement is deliberately left alone.

**Game-linked**: it pre/post-hooks a game function and drives game models, so it is coupled to the
pinned build (`DUSKLIGHT_VERSION`). Source: `mods/lantern_position/src/mod.cpp`.

## How the game attaches the lantern

The lantern is **not parented to a bone in the model file**, and there is no attachment table to
edit. `mpKanteraModel` (loaded from `al_kantera.bmd`) is created once when Link is created and
lives as long as he does; where it appears is decided in C++ **every frame**.

`daAlink_c::setItemMatrix()` — `src/d/actor/d_a_alink.cpp`, the `FLG2_UNK_1` block — rebuilds the
lantern's base matrix from one of Link's *animated joint matrices*, picking between two placements:

| | joint | translation | rotation (X, Y, Z degrees) |
|---|---|---|---|
| in hand | `mLeftItemJntNo` = 10 | `-2.0, -0.1, -0.7` | `100.0, 9.3, 183.0` |
| on the belt | `0x10` = 16 | `-1.0, 4.5, 9.0` | `-75.0, 62.0, 89.0` |

```cpp
if (checkNoResetFlg2(FLG2_UNK_1) || checkEndResetFlg1(ERFLG1_UNK_4)) {   // lantern engaged
    if (mProcID != PROC_OPEN_TREASURE && !checkEndResetFlg1(ERFLG1_UNK_4) && ...) {
        if (mEquipItem == dItemNo_KANTERA_e || checkOilBottleItemNotGet(mEquipItem)) {
            /* in hand  */ mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));  ...
        } else {
            /* on belt  */ mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(0x10));            ...
        }
        mpKanteraModel->setBaseTRMtx(mDoMtx_stack_c::get());
    }
    modelCalc(mpKanteraModel);
    mDoMtx_stack_c::transS(mKandelaarFlamePos);
    mpKanteraGlowModel->setBaseTRMtx(mDoMtx_stack_c::get());
    modelCalc(mpKanteraGlowModel);
}
```

Four things follow from this, and they are what make the mod small:

- **`mEquipItem` selects the branch.** Draw the sword and `mEquipItem` stops being
  `dItemNo_KANTERA_e`, so the belt branch runs. `FLG2_UNK_1` — set by `setKandelaarModel()`, cleared
  by `offKandelaarModel()` — is the "lantern engaged" flag, which is why it stays lit on his belt
  rather than disappearing.
- **Moving the matrix moves everything.** Joint 1 of the lantern model carries
  `daAlink_kandelaarModelCallBack` (`d_a_alink_kandelaar.inc`), which derives `mKandelaarFlamePos`
  from wherever the model ended up. That one position feeds the glow billboard, the lantern's light
  and shadow mode (`d_kankyo.cpp` → `g_env_light`, `dKy_shadow_mode_set(2)`), the real-shadow entry
  (`dComIfGd_addRealShadow`), and the insect/critter actors that check
  `getKandelaarFlamePos()`. There is nothing to move but the matrix.
- **`ERFLG1_UNK_4` means "someone else owns the matrix this frame."** It is set by the virtual
  `setKandelaarMtx()`, which only Coro's shop uses (`d_a_npc_ks.cpp`). Vanilla skips its own
  placement then, and so does this mod.
- **The treasure-open pose places the lantern in world space**, not on a joint
  (`d_a_alink_demo.inc`), guarded by `PROC_OPEN_TREASURE`. Same guard here.

### Link's joint numbers

Assigned per form in `d_a_alink_wolf.inc`. Human Link: left hand 9 / **left item point 10**, right
hand 14 / right item point 15, **back-attach joint 5** (sheath, back-slung sword, stowed shield),
head 4, backbone 1, heavy-boot legs 19–21 and 24–26. **Joint 16** — the belt lantern's — sits
between the right-arm chain and the legs, i.e. hip height, which is consistent with the tiny offset
the game applies to it.

The decomp never reconstructed a `JNT_` enum for Link (other humanoids have one, e.g.
`d_a_npc_jagar.h`), so these are raw indices. The model file still carries the original names:
**Dump Joint Names** in the mod's panel prints index → name for every joint of whichever model Link
is currently wearing, via `J3DModelData::getJointName()`.

## What the mod does

Pre/post-hooks `daAlink_c::setItemMatrix(int)`, lets the vanilla call run, and then rebuilds the
**stowed** placement from config — attach joint, local translation, XYZ rotation, uniform scale —
followed by the two `modelCalc()`s that depend on it. Recalculating is what actually moves the drawn
model: J3D draws from the anim matrices `calc()` builds, not from the base matrix.

A pre-hook is not enough on its own (the original overwrites whatever it sets) and a replace-hook
would mean reimplementing sword, shield, hat, face, boots and stirrups too. `setBaseTRMtx` is inline
and has no symbol, so it cannot be hooked directly.

### The flame spring, and why there is a pre-hook

`mKandelaarFlamePos` is a **damped spring with per-frame state** (`field_0x3618` velocity,
`field_0x3624` / `field_0x3630` history), advanced inside the joint-1 callback that `modelCalc()`
runs — it is what gives the flame its lag and sway. By the time the post-hook gets control the
vanilla call has already advanced it once, against the vanilla matrix, so a second `modelCalc()`
would step it **twice per frame** and stiffen the swing.

The pre-hook snapshots those four vectors and the post-hook restores them before recalculating, so
exactly one advance happens per frame — against our matrix. The pre-hook does nothing else.

## Settings

Defaults are the game's own numbers, so a fresh install reproduces the stock belt placement exactly.
That is deliberate: it is the check that the re-apply path is sound before any slider moves.

| Control | Default | Notes |
|---|---|---|
| Enabled | on | Off = the game's placement runs untouched |
| Attach Joint | 16 | 5 puts it on his back, 10 is the left-hand item point |
| Offset X / Y / Z | -10 / 45 / 90 | Tenths of a world unit, in the **joint's local space** |
| Rotation X / Y / Z | -75 / 62 / 89 | Degrees, applied X then Y then Z (the game's order) |
| Scale | 100% | Flame, glow and light scale with it |
| Reset to Vanilla | — | Restores all eight numbers |
| Dump Joint Names | — | Prints Link's joint table to the log |

The offset axes belong to the joint and rotate with it as Link animates, so which way each points
depends on the joint chosen — nudge one and look. The rotations are not zero at rest: the lantern
model does not hang straight down, so start from the defaults and adjust rather than zeroing them.

An attach-joint index the current model does not have leaves the lantern exactly where the game put
it, rather than misplacing it.

## Notes and limits

- **Stowed only.** The in-hand placement is untouched. Adding a second set of knobs for it is a
  handful of lines in the same hook (the branch is right above the one this mod rewrites), but it
  was not asked for and doubles the tuning surface.
- **Wolf form is skipped entirely.** It has a different skeleton and no lantern of its own.
- **No animation changes.** This moves a static offset from a joint; it does not add a hanging or
  swinging animation. The flame's own sway still works, since it is derived from the model's
  position.
- The mod holds no resources and touches no game state outside the lantern's two models, so it is
  safe to enable, disable and reload freely.
