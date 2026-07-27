# SSILVB — Environment Light (the probe)

Added in SSILVB 0.10.0. This documents the environment-probe ambient only; the bounce itself is
covered by `docs/ssilvb_plan.md`.

## What problem it solves

SSILVB's bounce gathers light by marching the depth buffer. That means it can only find light
that is (a) currently on screen and (b) inside the march radius. Everything else contributes
nothing. In practice that produces the two complaints screen-space GI always attracts:

- **Dark areas stay dark.** A room lit by a torch just out of frame receives nothing.
- **Light pops at the screen edge.** Pan the camera and a coloured surface's contribution appears
  and disappears as it crosses the frame boundary.

The environment probe fixes both without tracing anything new.

## How it works

### Representation — a world-space ambient cube

Six radiance values, one per world axis (`+X, +Y, +Z, -X, -Y, -Z`), each with a coverage
confidence, plus a frame-wide mean as a fallback. Stored in an 8×1 `rgba32float` texture,
ping-ponged per frame.

Evaluation in a direction `d` is:

```
L(d) = Σ axis[i] · max(dot(d, axis_i), 0)²
```

The three facing axes' squared cosines sum to exactly 1 (because `d.x² + d.y² + d.z² = 1`), so
this is an **exact partition of unity** — no normalization, never negative, and the ambient can
neither gain nor lose energy as a surface turns.

SH9 was considered and rejected: a single camera frustum covers a narrow, partial slice of the
sphere each frame, and SH9 rings badly on partial angular coverage. Ringing shows up as dark
haloes and oversaturated blotches. The ambient cube cannot ring.

### Capture — from the frame, not from re-rendered geometry

`accumulate_probe` (in `res/preprocess_color.wgsl`) runs as **one workgroup** reading **MIP 4 of
the colour chain the mod already builds** — roughly 2 000 pre-averaged texels covering the whole
frame. For each texel it:

1. Reconstructs the world-space ray direction through that screen position.
2. Weights it by the solid angle a screen-uniform texel actually subtends (`cos³θ`, so frame
   corners do not count as much as the centre).
3. Applies the same firefly luminance ceiling the march uses, so one boosted emissive texel
   cannot swing the whole estimate.
4. Projects it onto the six axes.

A tree reduction over 64 threads collapses this to the six buckets. **No geometry is re-rendered
and no game function is hooked** — the mod stays service-only.

### Persistence — why off-screen light survives

Each axis is blended toward the current frame's measurement **in proportion to how much that
frame constrained it**:

```
axis_alpha = alpha · coverage_of_this_axis_this_frame
```

Directions the camera is not pointing at have coverage ≈ 0, so they keep their history intact.
Walk past a torch and turn away: the torch's warmth stays in whichever axis it occupied until it
decays. That persistence is the entire mechanism by which off-screen light reaches a surface.

Confidence tracks coverage slowly in both directions. Where confidence is low — a direction the
camera has genuinely never looked at — evaluation falls back to the frame-wide mean rather than
to a stale or arbitrary value.

`+Y` (straight up) is the one direction players rarely look at and every floor faces, so the
existing sky estimate fills it to the extent direct coverage is missing. That is what the
**Sky Light** toggle now controls when the probe is on.

### Application — through the visibility the sampler already computes

Inside the slice loop, after the march, the still-visible sectors get:

```
slice_gi += eval_probe(bent_direction_world) · (visible_sectors / 32)
```

This is the same slot the old sky-only term occupied. Two consequences worth stating plainly:

- **No double counting is possible.** A sector is either occluded — in which case the march
  already accumulated that surface's bounce — or visible, in which case the probe fills it.
  Never both.
- **Spatial gating is free.** A pixel deep in a tight corridor has few visible sectors and so
  receives little ambient, even though the probe itself has no spatial variation. The visibility
  bitmask does that work.

## Interaction with the existing bounce

| Setting | Effect |
|---|---|
| Environment Light **on** (default) | probe supplies ambient; Sky Light fills the up axis |
| Environment Light **off** | the old sky-only ambient path runs unchanged — previous look, exactly |
| Bounce Intensity 0 | ambient-only mode: directional AO + environment light, no screen-space bounce |

The last row required a fix: the bounce, sky and probe share one channel that the composite
multiplies by `gi_intensity`, so zeroing the bounce slider previously zeroed the sky light too.
The composite now floors that multiply at 0.01 and the host pre-divides the sky/probe sliders by
the same floored value, making the three sliders genuinely independent. At the floor the bounce
contributes 1 % of its raw value, which is not visible.

## Cost

- **One extra compute dispatch of one workgroup** reading a ~2 000-texel mip. Effectively free.
- The colour mip reduction chain (4 small dispatches) now runs when the probe is on, where
  before it ran only for the bounce.
- **VRAM: two 8×1 textures.** Kilobytes.
- Per-pixel: six texture loads from an 8×1 texture (always resident) hoisted out of the slice
  loop, then ~12 ALU per slice.

There is no meaningful frame-time cost. The expensive part of this mod remains the march.

## Known limits

- **No spatial variation.** One probe covers the whole scene. A corridor and the room it opens
  into share an ambient value. The visibility gating hides most of this, but a large space with
  strongly contrasted halves will be wrong in one of them.
- **Directions never observed are guesses.** They fall back to the frame mean. Walking backwards
  into an unseen area gives it ambient measured from where you came from.
- **The environment is treated as infinitely distant.** Distance falloff comes only from the
  visibility term, not from how far away the lit surface actually is.
- **Room transitions lag** by the adaptation time. Large brightness changes accelerate the blend,
  but a transition between two areas of similar overall brightness but different colour will
  cross-fade at the base rate (~0.3 s by default).

The upgrade path, if these bite: replace the capture with periodic cubemap renders via
draw-list replay (as the shadow mod does), or move to a sparse probe grid. Both would keep this
representation and this application path unchanged — only `accumulate_probe` would be replaced.
That would make the mod game-linked, which is why it was not done first.

## Tuning

| Setting | Default | Notes |
|---|---|---|
| Environment Light | on | master toggle; off restores the sky-only path |
| Environment Intensity | 100 % | applies the measured surroundings as-is |
| Environment Saturation | 100 % | lower if an area's dominant colour washes the scene |
| Environment Response | 100 % | ~0.3 s settle; raise if it lags cave entrances |
| Sky Light | on | with the probe on, this is the up-axis fill only |

**Debug View → Environment** shows the ambient each surface's own normal sees, occlusion ignored.
It is flat and blotchy by design — it is a six-direction estimate, not an image. Turn to face a
torch and watch the value warm; that confirms the capture and the persistence are working.
