# Normal-buffer portability — what happens when the platform loses the thin g-buffer

**Short version:** nothing breaks. The mods compile and run against an SDK and a game build that
have no authored-normal buffer at all. This document says why, and what to check if you are the one
doing the re-platform.

The thin g-buffer is a **fork-local** feature: `platform-gbuffer-test` has it, `platform-v2-test`
does not, and **upstream Dusklight has never had it** (see `docs/authored_normals.md` §9 for the
investigation). So "the platform no longer exposes authored normals" is a state the tree has to
survive by design, not an accident to patch later.

There are two independent failure modes, and they need two different mechanisms.

---

## 1. Runtime — the game build has no normal buffer

This is the case when you build against `platform-gbuffer-test` but *run* `platform-v2-test`, which
is a supported and tested combination (the struct-size ABI check only rejects callers built against
an **older** SDK, so a newer-built mod runs fine on an older host — see CLAUDE.md, "The ABI pin").

The host reports `GfxDeviceInfo::normal_format == WGPUTextureFormat_Undefined`, and everything
downstream follows from that single fact:

| Site | Behaviour when Undefined |
|---|---|
| Graphics Hub, init | `g_authoredAvailable = false`. |
| Graphics Hub, per frame | Never sets `GfxResolveDesc::normal`, so no snapshot is requested and no cost is paid. |
| Graphics Hub, compute | Binds `g_dummyAuthoredView` (a 1×1 texture created at init) so the bind group stays valid, and passes `use_authored = 0` — the shader takes the 5-tap depth reconstruction for **every** pixel. |
| Graphics Hub, UI | "Use Authored Normals" greys out; the status line reads *"Reconstructed — this game build has no normal buffer"*. |
| All five scene-pass composites | Declare **one** colour target instead of two, matching a scene pass that has one attachment. |
| All five draw callbacks | `normal_format(*ctx) == normal_format(g_deviceInfo)` (both Undefined), so the layout-mismatch guard does not fire and the composite records normally. |

Consumers (VBAO, SSILVB, SMAA, Realtime Sun Shadows) never see any of this: they read normals
through the `dev.automata.depth_to_normal` service, which keeps returning a valid world-space
normal either way. **The only visible difference is that normals are faceted rather than smooth.**

## 2. Build time — the SDK has no normal-buffer fields

This is the case when you re-platform onto upstream Dusklight, or any base predating the thin
g-buffer. Four struct fields simply cease to exist:

```
GfxDeviceInfo::normal_format      GfxResolveDesc::normal
GfxDrawContext::normal_format     GfxResolvedTargets::normal (and ::normal_format)
```

A direct field access is a hard compile error, and before `common/gfx_normal_compat.h` there were
~18 of them across five mods — which would have made a one-line pin bump into a source rescue.

**All access now goes through `common/gfx_normal_compat.h`**, which detects each field by member
name at compile time and degrades to the "no normal buffer" answer when it is absent:

```cpp
gfx_compat::normal_format(g_deviceInfo)   // -> Undefined  when the field is gone
gfx_compat::normal_format(*ctx)           // -> Undefined  when the field is gone
gfx_compat::request_normal(desc, want)    // -> no-op      when the field is gone
gfx_compat::resolved_normal(resolved)     // -> nullptr    when the field is gone
```

Those are exactly the values §1 already handles, so an SDK without the fields collapses onto the
tested runtime fallback path rather than into a new, untested one. No `#ifdef`s, no CMake feature
detection, no per-mod configuration.

### Verified, not assumed

This was checked end to end by deleting all four field declarations from
`dusklight/sdk/include/mods/svc/gfx.h` (and trimming the positional `GFX_*_INIT` macros to match),
then configuring and building the whole tree from scratch. All seven mods compiled, linked and
packaged. Re-run that experiment if you change the shim.

### The one rule for new code

> **Never write `.normal_format` or `.normal` on an SDK struct directly. Call the `gfx_compat`
> accessor.**

Any new mod that records a draw into the scene pass must also declare the write-masked second
colour target when `gfx_compat::normal_format(g_deviceInfo) != WGPUTextureFormat_Undefined` — see
CLAUDE.md, "Every render pipeline recorded into the scene pass must declare TWO color targets".

---

## 3. This already happened — and the shim held

The tree now pins **upstream Dusklight** (`0fc05028`), which has no normal buffer of any kind. The
move needed **zero source changes**: a wiped-tree `cmake -B build && cmake --build build` against the
upstream SDK compiled, linked and packaged all seven mods with no errors. `normal_format` does not
appear anywhere in that SDK, so every accessor above took its absent-field branch.

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

## 4. Doing the next re-platform

1. Bump `DUSKLIGHT_VERSION` to the upstream commit whose build you run, and reconfigure. The stub URL
   does not change: upstream's `sdk` release is one fixed, version-independent tag.
2. Build. It should just work; if it does not, the failure is in the shim, not in the mods.
3. Expect Graphics Hub to report *"this game build has no normal buffer"* and normals to be faceted.
   That is correct.
4. Re-verify the **game-linked** mods in-game (Realtime Sun Shadows, Deferred Fog, Effect Remover,
   Celestial Orbit) — they hook specific game functions **by symbol, resolved at load**, so a decomp
   delta can make one fail to load rather than merely misbehave. The service-only mods (VBAO, SSILVB,
   SMAA) need no re-verification.
5. Watch the shadow mod for streaming-buffer overflow: upstream's aurora is smaller than our old fork
   (Vertex 5 MB / Index 2 MB / Storage 8 MB vs 16 / 4 / 16) and the cascade replays are the heaviest
   consumer.
