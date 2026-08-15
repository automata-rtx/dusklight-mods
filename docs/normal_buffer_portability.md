# Normal-buffer portability — what happens when the platform loses the scene normal buffer

**Short version:** nothing breaks. The mods compile and run against an SDK and a game build that
have no authored-normal buffer at all. This document says why, and what to check if you are the one
doing the re-platform.

The scene normal buffer is a **fork-local** feature: `platform-normals-test` has it, upstream
Dusklight does not (see `docs/authored_normals.md` §9 for the investigation). It is also *optional
at runtime on a platform that has it* — off by default, switched on in **Video → Rendering → Scene
Normal Buffer**, applied on the next launch, and unavailable altogether in compatibility mode (the
D3D11 and OpenGL ES fallbacks). So "the platform is not giving me authored normals" is a state the
tree has to survive by design, and it is the **default** state even on the current pin.

There are two independent failure modes, and they need two different mechanisms.

---

## 1. Runtime — the running game is not writing a normal buffer

Three different situations land here, and the mod cannot tell them apart: the buffer is **off in
Video settings** (the default), the device is in **compatibility mode**, or the game build
**predates the feature**. All three report the same thing.

The host reports `GfxDeviceInfo::normal_format == WGPUTextureFormat_Undefined`, and everything
downstream follows from that single fact:

| Site | Behaviour when Undefined |
|---|---|
| Graphics Hub, init | `g_authoredAvailable = false`. |
| Graphics Hub, per frame | Never sets `GfxResolveDesc::normal`, so no snapshot is requested and no cost is paid. |
| Graphics Hub, compute | Binds `g_dummyAuthoredView` (a 1×1 texture created at init) so the bind group stays valid, and passes `use_authored = 0` — the shader takes the 5-tap depth reconstruction for **every** pixel. |
| Graphics Hub, UI | "Use Authored Normals" greys out; the status line names the fix — *"Reconstructed — turn on Video > Rendering > Scene Normal Buffer and restart (unavailable in compatibility mode)"*. |
| All six scene-pass pipelines | `get_pass_targets` reports one colour target, so they declare one. |

Consumers (VBAO, SSILVB, SMAA, Realtime Sun Shadows) never see any of this: they read normals
through the `dev.automata.depth_to_normal` service, which keeps returning a valid world-space
normal either way. **The only visible difference is that normals are faceted rather than smooth.**

Note the asymmetry with the build-time case below: the game build being *older* than the SDK is
only safe in this one direction. The struct-size ABI check rejects callers built against an
**older** SDK, so a newer-built mod runs on an older host but not the reverse — see CLAUDE.md, "The
ABI pin".

## 2. Build time — the SDK has no normal-buffer fields

This is the case when you re-platform onto upstream Dusklight, or any base predating the scene
normal buffer. Three struct fields simply cease to exist:

```
GfxDeviceInfo::normal_format      GfxResolveDesc::normal
GfxResolvedTargets::normal (and ::normal_format)
```

A direct field access is a hard compile error, and before `common/gfx_normal_compat.h` there were
~18 of them across five mods — which would have made a one-line pin bump into a source rescue.

**All access now goes through `common/gfx_normal_compat.h`**, which detects each field by member
name at compile time and degrades to the "no normal buffer" answer when it is absent:

```cpp
gfx_compat::normal_format(g_deviceInfo)   // -> Undefined  when the field is gone
gfx_compat::request_normal(desc, want)    // -> no-op      when the field is gone
gfx_compat::resolved_normal(resolved)     // -> nullptr    when the field is gone
```

Those are exactly the values §1 already handles, so an SDK without the fields collapses onto the
tested runtime fallback path rather than into a new, untested one. No `#ifdef`s, no CMake feature
detection, no per-mod configuration.

### `GfxDrawContext::normal_format` is gone — and so is the guard that read it

The retired fork put `normal_format` on `GfxDrawContext` as well, and every scene-pass draw
callback compared it against the device's to catch a pass that had changed shape mid-session:

```cpp
if (gfx_compat::normal_format(*ctx) != gfx_compat::normal_format(g_deviceInfo)) { return; }
```

**The current SDK has no such field**, and that turns the guard into a trap rather than a
safeguard. `normal_format(*ctx)` compiles to a constant `Undefined` while the device reports a real
format, so the condition is *always true the moment the user switches the buffer on* — every
composite silently returns without drawing. That is the same "mods load but do nothing" symptom the
offset collision produced, reached from the opposite direction, and it is precisely the failure the
compat shim is meant to prevent: degrading to "absent" is only safe for a value being **read**, not
for one being **compared against a live one**.

The guard is deleted. It defended against a mid-session toggle that cannot happen — the buffer is
an initialization-time setting requiring a restart — and the layout question it was asking is now
answered properly at pipeline build time by `get_pass_targets`.

## 3. Pipelines — `common/gfx_scene_pass.h`

A pipeline recorded into the scene pass must declare that pass's attachments exactly. Mods used to
assemble that layout by hand from `GfxDeviceInfo` plus a second write-masked target when
`normal_format` was set. GfxService 1.2 answers it directly, and
`gfx_compat::scene_pass_layout(...)` is the one call site for it:

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

It prefers `get_pass_targets` and falls back to the hand-assembled layout on an SDK that predates
it, detected the same way as the struct fields — so the six sites (vbao, ssilvb, smaa,
realtime_sun_shadows, and both of graphics_hub's) stay correct on a base with the buffer, without
it, and on an SDK that has never heard of the query. Offscreen `create_pass` targets are
single-target and do not use this.

### Verified, not assumed — and it caught a real bug

This is checked end to end by reducing the fetched SDK header to a 1.1-era one — deleting the field
declarations, `GfxPassTargets`, `GfxPass`, `GFX_MAX_COLOR_TARGETS` and `get_pass_targets`, and
trimming the positional `GFX_*_INIT` macros to match — then configuring and building the whole tree
from scratch. All seven mods compile, link and package. Re-run it if you change either shim.

It is not a formality. `scene_pass_layout` was first written with `if constexpr` on a
member-detection trait, mirroring `gfx_normal_compat.h`, and this build rejected it outright: **a
discarded `if constexpr` branch is still parsed and name-looked-up**, so naming `GfxPassTargets` in
it is a hard error on an SDK that lacks the type. Member detection works for a *member of an
existing type*; it cannot help when the type itself is absent. Hence the `#if` — only the
preprocessor removes code from the translation unit. Had the experiment been skipped, the tree would
have compiled fine today and broken the next re-platform, which is precisely what these shims exist
to prevent.

### The one rule for new code

> **Never write `.normal_format` or `.normal` on an SDK struct directly. Call the `gfx_compat`
> accessor. Never hand-assemble a scene-pass pipeline's colour targets — call
> `gfx_compat::scene_pass_layout`.**

---

## 4. Both directions have now been travelled — and the shim held each way

The tree went to **upstream Dusklight** (`0fc05028`, no normal buffer of any kind) and back to a
fork SDK that has one (`platform-normals-test`). Both moves needed **zero changes to the
normal-reading code**: a wiped-tree `cmake -B build && cmake --build build` compiled, linked and
packaged all seven mods each time. Going out, every accessor took its absent-field branch; coming
back, they took the present-field branch and authored normals reappeared on their own.

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

The current pin does not repeat that mistake: `normal_format` is appended *after* upstream's own
`instance`/`adapter`, on top of upstream rather than beside it, so no slot is claimed twice.

## 5. Doing the next re-platform

1. Bump `DUSKLIGHT_VERSION` to the commit whose build you run, and reconfigure. **If the new base is
   a different repository, `DUSKLIGHT_REPOSITORY` and `DUSKLIGHT_SDK_STUB_URL` move with it** — the
   stubs must come from the same build as the game. (Upstream's `sdk` release is one fixed,
   version-independent tag; a fork's is per-release.)
2. Build. It should just work; if it does not, the failure is in the shim, not in the mods.
3. If the new base has no normal buffer, expect Graphics Hub to say so and normals to be faceted.
   That is correct.
4. Re-verify the **game-linked** mods in-game (Realtime Sun Shadows, Deferred Fog, Effect Remover,
   Celestial Orbit) — they hook specific game functions **by symbol, resolved at load**, so a decomp
   delta can make one fail to load rather than merely misbehave. The service-only mods (VBAO, SSILVB,
   SMAA) need no re-verification.
5. Watch the shadow mod for streaming-buffer overflow. The current pin's aurora carries the enlarged
   buffers (Vertex 16 MB / Index 4 MB / Storage 16 MB); a base on upstream's sizes (5 / 2 / 8) is
   tighter and the cascade replays are the heaviest consumer.
