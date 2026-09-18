# Normal-buffer portability — what happens when the platform loses the scene normal buffer

**Short version:** nothing breaks. The mods compile and run against an SDK and a game build that
have no authored-normal buffer at all. This document says why, and what to check if you are the one
doing the re-platform.

**The scene normal buffer is UPSTREAM now.** This document was written when it was fork-local to
`platform-normals-test` and upstream Dusklight had nothing; upstream Dusklight 2.0 ships its own
(GfxService 1.3's `GfxResolveDesc::normal` → `GfxResolvedTargets::normal`) with the attachment in
upstream aurora, and our fork is retired. That changes *which* SDKs lack the feature — anything
older than 1.3 — but changes nothing about why this document exists.

It is still **optional at runtime on a platform that has it**, and there are now three distinct
runtime ways to have no normals:

| Why | Permanent? | What the user can do |
|---|---|---|
| **Not yet** — the snapshot latches: the first `resolve_pass` that asks enables the attachment for the *next* frame and returns `NULL` | No, resolves itself in one frame | Nothing; a consumer must not report this |
| **MSAA is on** — aurora refuses to create the buffer unless `msaaSamples == 1`, and does not even record the request | Yes, while MSAA is on | **Set antialiasing to none** |
| **No WebGPU core features** — the D3D11 / OpenGL ES compatibility renderers | Yes | Nothing |

The fork's user-facing **Video → Rendering → Scene Normal Buffer** switch is **gone** — upstream has
no such setting, and the buffer is created on demand instead. Anywhere below that tells you to flip
it is describing the fork.

So "the platform is not giving me authored normals" is a state the tree has to survive by design,
and on a cold start it is briefly true on *every* platform. There are two independent failure modes,
and they need two different mechanisms.

---

## 1. Runtime — the running game is not writing a normal buffer

Four situations land here. Three are indistinguishable to the mod — the device is in
**compatibility mode**, the game build **predates the feature**, or the SDK does (§2) — and all
three report the same thing. The fourth, **MSAA**, is distinguishable and must be distinguished:
`GfxDeviceInfo::sample_count` reports the live scene-pass sample count, so a consumer that sees a
null normal view with `sample_count > 1` can say *"MSAA is on and the normal buffer requires MSAA
off"* instead of blaming the user's GPU. VBAO and SMAA both do. A fifth, the **latch**, is not a
failure at all and must not be reported — see the table above.

The question is asked of the **scene pass**, not of the device: the attachment exists only when the
renderer built the pass with it. `get_scene_target_layout` tags every attachment with a
`GfxAttachmentSemantic`, so the presence of a `GFX_ATTACHMENT_NORMAL` entry is the direct answer,
surfaced as `gfx_compat::ScenePassLayout::has_normal_attachment`. Everything downstream follows
from that single fact:

| Site | Behaviour when there is no normal attachment |
|---|---|
| A normal consumer, per frame | `resolve_pass` returns a `NULL` normal view; the mod takes its no-normals path. It keeps *asking* — that is what arms the latch — but an absent attachment costs nothing to request. |
| A normal consumer, compute | Binds a 1×1 dummy view so the bind group stays valid, and takes whatever fallback it has — for VBAO and SMAA that means the normal-driven half of the effect disables itself and the UI says so. |
| Every scene-pass pipeline | The layout reports one colour target, so they declare one. |

> **NAMING NOTE.** The rows above used to name **Graphics Hub**, the provider mod that took the
> snapshot, reconstructed normals from depth where it had none, and republished them as
> `dev.automata.depth_to_normal`. That mod and that service are retired: GfxService 1.3 hands the
> authored normals to each mod through its own `resolve_pass`, so there is no provider and no
> depth-reconstruction fallback to fall back *to*. Anything else in this document that names
> Graphics Hub is describing that era.

This used to read `GfxDeviceInfo::normal_format == WGPUTextureFormat_Undefined`. **That field no
longer exists in any SDK** — see §2.1.

> **The paragraph that was here is HISTORICAL and was the opposite of today's behaviour.** It said
> consumers never see any of this, because the provider service kept returning a valid world-space
> normal either way and the only visible difference was faceting. With the provider retired there is
> no reconstruction anywhere in the tree: **a consumer that needs normals and has none disables its
> normal-driven half and says so.** VBAO disables AO entirely; SMAA keeps running on luma edges. A
> faceted-vs-smooth difference is no longer a thing any current mod can show.

Note the asymmetry with the build-time case below: the game build being *older* than the SDK is
only safe in this one direction. The struct-size ABI check rejects callers built against an
**older** SDK, so a newer-built mod runs on an older host but not the reverse — see CLAUDE.md, "The
ABI pin".

## 2. Build time — the SDK has no normal-buffer fields

This is the case when you re-platform onto upstream Dusklight, or any base predating the scene
normal buffer. **Exactly two struct fields cease to exist** (GfxService 1.3):

```
GfxResolveDesc::normal      GfxResolvedTargets::normal
```

That pair is the whole normal-buffer API, and the whole remaining fork delta. Everything else the
mods touch — the scene-target-layout query, the attachment semantics, `GfxDrawContext::layout` — is
upstream GfxService 1.2.

A direct field access is a hard compile error, and before `common/gfx_normal_compat.h` there were
~18 of them across five mods — which would have made a one-line pin bump into a source rescue.

**All access now goes through `common/gfx_normal_compat.h`**, which detects each field by member
name at compile time and degrades to the "no normal buffer" answer when it is absent:

```cpp
gfx_compat::request_normal(desc, want)    // -> no-op      when the field is gone
gfx_compat::resolved_normal(resolved)     // -> nullptr    when the field is gone
```

Those are exactly the values §1 already handles, so an SDK without the fields collapses onto the
tested runtime fallback path rather than into a new, untested one. No `#ifdef`s, no CMake feature
detection, no per-mod configuration.

### 2.1 `normal_format` is gone from every struct — and so are the accessors for it

Two platforms carried a `normal_format` field and each produced its own bug. The retired fork put
it on `GfxDeviceInfo` at an offset upstream independently claimed for `WGPUInstance` (§4). It also
put one on `GfxDrawContext`, and every scene-pass draw callback compared that against the device's
to catch a pass that had changed shape mid-session:

```cpp
if (gfx_compat::normal_format(*ctx) != gfx_compat::normal_format(g_deviceInfo)) { return; }
```

When the field vanished from `GfxDrawContext`, that guard became a trap rather than a safeguard.
`normal_format(*ctx)` compiled to a constant `Undefined` while the device reported a real format,
so the condition was *always true the moment the user switched the buffer on* — every composite
silently returned without drawing. Degrading to "absent" is only safe for a value being **read**,
never for one **compared against a live one**.

Both the guard and the accessor are deleted. The current SDK has no `normal_format` anywhere; ask
`ScenePassLayout::has_normal_attachment` instead, which reads the real pass's semantic tags.

### 2.2 A renamed API is not an absent one — and this bit us

**This is the trap to internalise, because it is silent and the tree looked healthy.**

`gfx_scene_pass.h` used to guard on `#if defined(GFX_MAX_COLOR_TARGETS)` and fall back to a
hand-assembled single-target layout when that was absent. Upstream then shipped its own version of
the same feature under different names — `get_pass_targets` → `get_scene_target_layout`,
`GfxPassTargets` → `GfxRenderTargetLayout`, `GFX_MAX_COLOR_TARGETS` → `GFX_MAX_COLOR_ATTACHMENTS`.

On the pin bump, the guard went false. **All seven mods compiled, linked and packaged with zero
errors and zero warnings**, and every scene-pass pipeline quietly reverted to declaring one colour
target — which WebGPU rejects against a two-attachment pass. Six composites would have drawn
nothing, in-game, with no build-time signal at all. Identical symptom to the offset collision,
reached from a third direction.

The lesson generalises past this one header: **"degrade to absent" cannot distinguish a feature
that is gone from a feature that was renamed**, and it silently picks the wrong answer for the
second. So the two shims now differ deliberately, by how bad being wrong is:

| Shim | Missing vocabulary means | Why |
|---|---|---|
| `gfx_normal_compat.h` | quietly take the reconstruction path | Genuinely optional. Being wrong costs smoothness, and §1 handles it every session already. |
| `gfx_scene_pass.h` | **`#error`, with the fix named** | Being wrong makes six mods draw nothing with no other symptom. The legacy path still exists, behind `GFX_COMPAT_ALLOW_LEGACY_SCENE_LAYOUT`, so a genuine pre-1.2 base is one define away. |

The rule: reach for degrade-to-absent when the feature is optional and its absence is *observable
at runtime*. When silently guessing wrong produces no diagnostic, fail the build instead.

## 3. Pipelines — `common/gfx_scene_pass.h`

A pipeline recorded into the scene pass must declare that pass's attachments exactly. Mods used to
assemble that layout by hand from `GfxDeviceInfo`. GfxService 1.2 answers it directly with
`get_scene_target_layout`, and `gfx_compat::scene_pass_layout(...)` is the one call site for it:

```cpp
gfx_compat::ScenePassLayout layout;
if (!gfx_compat::scene_pass_layout(mod_ctx, svc_gfx, g_deviceInfo, layout)) {
    return false;
}
layout.color_targets[0].blend = &myBlendState;   // blend state is the caller's; the rest is the pass's
fragment.targetCount = layout.color_target_count;
fragment.targets = layout.color_targets;
depthStencil.format = layout.depth_format;
pipelineDesc.multisample.count = layout.sample_count;
```

Internally it calls `get_scene_target_layout` and hands the result to the SDK's own inline
`gfx_init_color_target_states`, which write-masks off every attachment the mod does not own — so
the six sites (vbao, ssilvb, smaa, realtime_sun_shadows, and both of graphics_hub's) stay correct
whether the pass has one attachment or several, and a composite that only reads the scene can never
clobber the game's authored normals. `color_targets[0]` is the scene colour and the only entry the
caller may modify. Offscreen `create_pass` targets are single-target and do not use this.

### Verified, not assumed — and it has caught two real bugs

This is checked end to end by reducing the fetched SDK header to the previous era — deleting the
field declarations and trimming the positional `GFX_*_INIT` macros to match — then forcing a full
recompile of the whole tree. All seven mods compile, link and package; the probe additionally
asserts that the layout query stays engaged while `has_normal<GfxResolveDesc>` and
`has_normal<GfxResolvedTargets>` both go false. Re-run it if you change either shim, and **check
that objects actually rebuilt** — a cached "Built target" proves nothing.

It is not a formality; it has now caught two bugs that would each have shipped.

1. `scene_pass_layout` was first written with `if constexpr` on a member-detection trait, mirroring
   `gfx_normal_compat.h`, and the stripped build rejected it outright: **a discarded `if constexpr`
   branch is still parsed and name-looked-up**, so naming a type the SDK lacks is a hard error, not
   a quietly unused branch. Member detection works for a *member of an existing type*; it cannot
   help when the type itself is absent. Hence the `#if`.
2. The rename in §2.2 — which the *build* could not catch by construction, because compiling
   cleanly was the symptom. What caught it was reading the new SDK header before trusting the
   green build.

### The one rule for new code

> **Never write `.normal` on an SDK struct directly. Call the `gfx_compat` accessor. Never
> hand-assemble a scene-pass pipeline's colour targets — take them from the live
> `GfxDrawContext::layout` via `gfx_compat::scene_pass_layout_for_draw`, and **rebuild the pipeline
> when `gfx_compat::scene_pass_layout_key` changes**, because the attachment count changes at
> runtime. And never ask `GfxDeviceInfo` whether the normal buffer exists; ask the scene layout.**

---

## 4. Three moves travelled — the shim held each time, and the third found its limit

The tree went to **upstream Dusklight** (`0fc05028`, no normal buffer of any kind), back to a fork
SDK that has one (`platform-normals-test`), and then on to **upstream Dusklight 2.0** (`c83ce89`),
which has its own. All three moves needed **zero changes to the normal-reading code**: a wiped-tree
`cmake -B build && cmake --build build` compiled, linked and packaged every mod in the build set
each time. Going out, every accessor took its absent-field branch; coming back, they took the
present-field branch and authored normals reappeared on their own.

**The third move is the interesting one, because upstream's API is a different shape from the
fork's** — `normal` is an appended `uint32_t` growing `sizeof(GfxResolveDesc)` 8 → 12, where the
fork had a `bool` in tail padding. `gfx_normal_compat.h` detects by member *name*, so it never
noticed, and the pin bump really was free.

**What no compile-time shim could absorb was a BEHAVIOUR change.** Upstream's snapshot *latches*:
the first resolve that asks enables the attachment for the next frame and returns null. Two things
had to change in every consumer, and neither is visible to a detection shim:

- **Don't report the first null.** Treating it as "this device cannot" fires a
  compatibility-renderer warning on every cold start. VBAO waits `kNormalLatchGraceFrames`.
- **Don't build scene-pass pipelines at init.** The pass gains a second colour attachment when the
  latch fires, and a pipeline built against the one-attachment pass is rejected from then on —
  **silently**, as a composite that simply stops appearing. Build lazily, keyed on `layout.key`.

The rule that follows: **a compatibility layer protects you from a changed declaration, not from
changed behaviour.** After any pin bump, read the new SDK header and the renderer code behind it; a
green build proves only that the names still line up.

What did *not* survive the round trip was the one place a compat accessor was used for a
**comparison** rather than a read — the `GfxDrawContext::normal_format` guard described in §2. It
compiled silently in both directions and would have disabled every composite on the way back. Reads
degrade safely; comparisons against a live value do not.

### The failure this prevented, and the one it didn't

Our old fork appended `GfxDeviceInfo::normal_format` at **offset 40**. Upstream independently
appended `WGPUInstance instance` at **the same offset**, and grew the struct from 48 to 56 bytes.
Fork-built `.dusk` files run on an upstream build therefore:

- failed the host's `caller->struct_size < sizeof(host struct)` check (48 < 56), and
- read a live `WGPUInstance` pointer as a texture format — non-zero, so `!= Undefined`, so every
  scene-pass composite declared **two** colour targets against a **one**-attachment pass and was
  rejected outright.

Result: the mods loaded and did nothing. Rebuilding against the upstream SDK fixes both, and the
shim is what made that a pin bump rather than an edit across five mods.

**What the shim does not cover:** appending a field to an SDK struct is not forward compatible when
two vendors do it independently. The shim protects against a field *disappearing*; nothing protects
against two different fields landing at the same offset. That is an argument for upstreaming a
feature rather than forking the SDK for it — see `docs/authored_normals.md` §9.5.

The current pin cannot repeat that mistake, because `normal_format` is **gone entirely** rather
than relocated. The surviving fork delta is two fields, and 1.3 places them carefully:
`GfxResolveDesc::normal` lands in the struct's existing **tail padding**, so `sizeof` is unchanged
and `struct_size` cannot distinguish a 1.2 caller from a 1.3 one. The host therefore reads it only
when `GfxResolvedTargets` is large enough to carry the result back — a 1.2 mod's uninitialised
padding can never accidentally request a snapshot it has nowhere to receive. That is the pattern to
copy if the fork ever has to grow a third field.

### Upstreaming shrank the delta, exactly as predicted

A fork pin once carried the whole scene-layout mechanism. Upstream shipped its own (#2305), and our
hand-rolled version — five types, a service entry point, and the compat branch that chose between
them — was **deleted rather than merged**. The delta went from "a layout API plus a device field
plus two resolve fields" to "two resolve fields", and then to **nothing**: upstream shipped its own
normal snapshot too, and the fork was retired. Every re-platform along the way was cheaper for the
deletion. The §9.5 argument was not theoretical — though note how it actually resolved: upstream
solved the same problem *its own way* rather than taking ours, which is the more likely outcome in
general and the reason to keep the delta small enough to throw away.

## 5. Doing the next re-platform

1. Bump `DUSKLIGHT_VERSION` to the commit whose build you run, and reconfigure. On upstream that is
   normally the whole change — the SDK downloads its stubs from a **version-independent** release,
   so there is nothing to move with the pin. **If a new base is ever a different repository,
   `DUSKLIGHT_REPOSITORY` moves with it, and if that repository is a fork, `DUSKLIGHT_SDK_STUB_URL`
   does too** — a fork's stubs are per-release and must come from the same build as the game.
2. Build. It should just work; if it does not, the failure is in the shim, not in the mods.
2a. **Then read the new SDK header and the renderer behind it.** §4's lesson: the shim proves the
   names still line up, not that the behaviour did. Ask specifically whether anything the mods
   depend on is now created on demand, latched, or otherwise different *at runtime*.
3. If the new base has no normal buffer, expect the normal consumers (VBAO, SMAA) to disable their
   normal-driven half and say so in their status lines.
   That is correct.
4. Re-verify the **game-linked** mods in-game (Realtime Sun Shadows, Deferred Fog, Effect Remover,
   Celestial Orbit) — they hook specific game functions **by symbol, resolved at load**, so a decomp
   delta can make one fail to load rather than merely misbehave. The service-only mods (VBAO, SSILVB,
   SMAA) need no re-verification.
5. The shadow mod's cascade replays are the heaviest consumer of aurora's per-frame streaming
   buffers, so check them if a new base ever *shrinks* those. The current pin uses upstream's sizes
   (Vertex 5 MB / Index 2 MB / Storage 8 MB); the enlarged 16 / 4 / 16 buffers the fork once
   carried are gone and are not needed, because upstream raised both the vertex and index buffers
   itself. See `docs/realtime_sun_shadows.md` for the dates.
