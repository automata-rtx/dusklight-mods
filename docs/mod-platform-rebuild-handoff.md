# Handoff: rebuild the Dusklight-AO mod platform (mainline hook fixes + our buffer sizes)

**For a fresh Claude Code session that has `automata-rtx/dusklight-ao` attached.**
Paste the "PROMPT TO PASTE" block at the bottom as your first message, then follow this
spec. Confirm the buffer values with the user before pushing anything.

---

## Goal

Publish a new base-game **platform release** on `automata-rtx/dusklight-ao` that equals
**latest mainline Dusklight (with Encounter's Windows hook fix) + our enlarged aurora
streaming buffers**, then have `automata-rtx/dusklight-mods` build its mods against it. This
**replaces the current `platform-v2-test`** release.

## Why (background)

- Our four graphics mods failed to load with **"tried to hook undeclared target."** Root
  cause was a **Dusklight bug** in Windows hook thunk re-resolution, fixed upstream in
  `TwilitRealm/dusklight` commit **`adfb830b`** ("hook: Fix thunk re-resolution on Windows").
  It is **not** in our current platform base (mainline `30def245`). Also want **`0f2a00cd`**
  ("Mods: Embed symdb in linked executable"). We need a platform that includes both.
- Separately, mods **must be compiled with clang-cl** (plain MSVC mangles the mod-metadata
  section so hooks never register). That is already handled in the dusklight-mods CI — do
  **not** regress it, but there's nothing to change here for it.

## Repos

| Repo | Role |
|---|---|
| `TwilitRealm/dusklight` | Public mainline. Read-only reference; source of the fixes. |
| `automata-rtx/dusklight-ao` | **Our fork (attached).** The platform we rebuild + release. |
| `encounter/aurora` | Aurora (the GX→WebGPU layer), a submodule at `extern/aurora`. Needs a fork to carry the buffer change. |
| `automata-rtx/dusklight-mods` | The consumer. Update after (branch `claude/dusklight-mods-graphics-fixes-7s0qs1`). |

## How the current platform is built (verified facts)

- The `platform-v2-test` release is produced by `.github/workflows/build.yml` on branch
  **`claude/dusklight-platform-update-ps81cg`**. The `release:` job is gated
  `if: endsWith(github.ref, 'claude/dusklight-platform-update-ps81cg')` and fires on **any
  push** to that branch. It builds Linux/Apple/Android/Windows, then **re-creates the
  `platform-v2-test` release idempotently** (`gh release delete platform-v2-test --cleanup-tag
  || true` then `gh release create platform-v2-test ...`), attaching **`windows-amd64.lib`**
  (curated import library the mods link against) and **`dusklight.symdb`**, plus per-platform
  game zips.
- That branch = pristine mainline `30def245` **plus only the added `release:` job** in
  `build.yml`. No game-code changes, no buffer changes.

## The buffer change (in aurora)

- File: `extern/aurora/lib/gfx/common.hpp` (around lines **176-179**) — the per-frame
  streaming buffer size constants. Their sum is `StagingBufferSize` in
  `lib/gfx/common.cpp` (~57-60), allocated **×5** (`StagingBufferCount = FrameSlotCount(2)+3`).
- Change these (**CONFIRM the exact numbers with the user first** — these are the recommended
  values, and should match what the user set in their own local game build):

  | Buffer | Current | New |
  |---|---|---|
  | Index | 1 MB | **4 MB** |
  | Vertex | 5 MB | **16 MB** |
  | Storage | 8 MB | **16 MB** |
  | Uniform | 24 MB | leave (24) |
  | TextureUpload | 24 MB | leave (24) |

- Effect: staging VRAM ≈ 315 MB → ≈ 600 MB. Fine on any modern GPU. These buffers `abort()`
  on overflow, which is why our multi-pass shadow-map replays need them raised.

## Plan

**Step 1 — Sync the platform branch to latest mainline.**
- `git remote add upstream https://github.com/TwilitRealm/dusklight.git && git fetch upstream`
- Rebuild branch `claude/dusklight-platform-update-ps81cg` on top of **`upstream/main`**
  (latest; includes `adfb830b` + `0f2a00cd`). Verify:
  `git log --oneline -20 | grep -E 'adfb830|0f2a00c'` returns both.
- Re-apply **only** the `release:` job onto mainline's current `.github/workflows/build.yml`.
  Mainline's build.yml may have changed since `30def245`, so take mainline's build.yml as the
  base and graft the `release:` job block (copy it verbatim from the old branch tip
  `e61d5bd`'s build.yml). Keep everything else pristine.

**Step 2 — Aurora buffer fork.**
- After checkout, note the aurora commit mainline pins: `git -C extern/aurora rev-parse HEAD`.
- **Fork `encounter/aurora` → `automata-rtx/aurora`** (do **NOT** reuse `automata-rtx/aurora-ao`
  — that is a frozen, older, pre-mod-API fork and will not match this mainline). Create a
  branch at the pinned commit, edit `lib/gfx/common.hpp` per the buffer table, commit, push.
  - If the user already pushed their own modified aurora, point at that fork/commit instead.
- In dusklight-ao: set `.gitmodules` `extern/aurora` url to the fork, `git submodule sync`,
  check out the buffered commit inside `extern/aurora`, then `git add extern/aurora .gitmodules`.

**Step 3 — Publish.**
- Commit and push branch `claude/dusklight-platform-update-ps81cg`. This triggers the full
  build and **re-publishes `platform-v2-test`** (same tag; job is idempotent). Update the
  release title/notes to say it is based on the new mainline commit and includes the Windows
  hook fix + enlarged buffers.
- (Optional) For a distinct tag, change the two `platform-v2-test` strings in the `release:`
  job to a new tag — but then also update `PLATFORM_RELEASE_TAG` in dusklight-mods (Step 4).
  Recommended: **keep `platform-v2-test`** so nothing downstream needs re-pointing.
- Watch CI; confirm the release re-publishes `windows-amd64.lib`, `dusklight.symdb`, and the
  `dusklight-*-win32-msvc-x86_64.zip` game build.

**Step 4 — Point the mods at it** (in `automata-rtx/dusklight-mods`, branch
`claude/dusklight-mods-graphics-fixes-7s0qs1`; add that repo to the session if needed).
- Bump the `extern/dusklight` submodule to the new platform branch commit.
- Leave `RELEASE_TAG` = `platform-v2-test` (unchanged if you kept the tag). **Keep the clang-cl
  configure flags** in `.github/workflows/build.yml`'s `build-windows` job.
- Push → the `build-windows` job downloads the new import lib and produces fresh `.dusk` in the
  `dusklight-mods-win64` artifact. That's what the user installs.

## Then (user, after CI is green)

Install the newly built game (the user is rebuilding their own game with these same fixes +
buffers; the platform's `win32-msvc-x86_64` zip is the matching reference build) **and** the
fresh `.dusk` files from the `dusklight-mods-win64` artifact. Reload in the mod manager. With
`adfb830b` in the game, the hooks register and the "undeclared target" errors are gone.

## Guardrails

- Change **no game code** beyond grafting the `release:` job and repointing the aurora
  submodule — keep the base pristine so the import library matches upstream exactly.
- **Do not** regress the clang-cl requirement in dusklight-mods.
- **Confirm the buffer values with the user before pushing.**
- The platform build is heavy (full game, all platforms, vcpkg/sccache) but it is the existing
  working workflow — `platform-v2-test` was produced this way. If a non-Windows target flakes,
  the mods only need the `win32-msvc-x86_64` output, but the release job `needs:` all of them —
  re-run failed jobs rather than removing targets unless the user agrees.

---

## PROMPT TO PASTE into the fresh session

> You have `automata-rtx/dusklight-ao` attached. We're rebuilding our mod platform. Read
> `docs/mod-platform-rebuild-handoff.md` in the `automata-rtx/dusklight-mods` repo (add that
> repo if it isn't attached) and execute it end to end. Before you push anything to
> dusklight-ao, show me the exact buffer values you're about to set and wait for my OK. The
> short version: sync the `claude/dusklight-platform-update-ps81cg` branch to the latest
> `TwilitRealm/dusklight` main (it now has the `adfb830b` "hook: Fix thunk re-resolution on
> Windows" fix), point `extern/aurora` at a fork with our enlarged streaming buffers (Index
> 1→4 MB, Vertex 5→16 MB, Storage 8→16 MB in `lib/gfx/common.hpp`), push to re-publish the
> `platform-v2-test` release, then bump the dusklight-mods `extern/dusklight` submodule to the
> new commit so CI builds fresh `.dusk` files.
