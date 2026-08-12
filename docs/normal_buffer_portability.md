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

## 3. Doing the re-platform

1. Bump `DUSKLIGHT_VERSION` (and `DUSKLIGHT_SDK_STUB_URL` if the release tag changes) per CLAUDE.md,
   "Re-platforming".
2. Build. It should just work; if it does not, the failure is in the shim, not in the mods.
3. `DUSKLIGHT_AURORA_VERSION` can be dropped if the new base's `extern/aurora` pin resolves — it is
   already a no-op against `b96bf5ec01`, which records `3ba95790`.
4. Expect Graphics Hub to report *"this game build has no normal buffer"* and normals to go back to
   faceted. That is correct, not a regression.
5. Re-verify the **game-linked** mods in-game (Realtime Sun Shadows, Deferred Fog, Effect Remover,
   Celestial Orbit) — they hook specific game functions and a game-code delta can shift what they
   hook. The service-only mods (VBAO, SSILVB, SMAA) need no re-verification.
