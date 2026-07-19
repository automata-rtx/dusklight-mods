# Changing defaults / removing options yourself (no AI needed)

This guide is for making **small, safe tweaks** to the mods — changing a default value or locking an
option to a fixed value — and building the result on GitHub for all platforms, entirely by hand. You
do **not** need to install anything or build locally. You edit one text file, GitHub compiles it on
7 platforms, and you download the finished `.dusk` files.

You only ever touch two kinds of file inside `mods/<mod-name>/`:

- `src/mod.cpp` — the code. Defaults and options live here.
- `mod.json` — the mod's name, version, and store description.

**Never** edit anything in the `dusklight/` folder — that's the read-only game SDK, fetched
automatically.

---

## The loop (how a change reaches your game)

1. Edit a file (on GitHub.com in the browser is easiest — see below).
2. Commit it to the branch `claude/dusklight-platform-rebuild-rqhsaw`.
3. GitHub Actions automatically builds every mod on all 7 platforms (~6 minutes).
4. Download the built `mods-combined` artifact.
5. Drop the `.dusk` files into your game's mods folder and hit **Reload** in the mod manager.

That's it. Steps 2–4 are the same every time; only step 1 changes.

---

## Editing a file on GitHub.com (no git, no local setup)

1. Go to the repo on GitHub → make sure the branch selector (top-left of the file list) says
   **`claude/dusklight-platform-rebuild-rqhsaw`**, not `main`.
2. Navigate to the file, e.g. `mods/effect_remover/src/mod.cpp`.
3. Click the **pencil icon** (Edit this file), top-right of the file view.
4. Make your change (see the recipes below).
5. Scroll down to **Commit changes**, leave "Commit directly to the
   `claude/dusklight-platform-rebuild-rqhsaw` branch" selected, click **Commit changes**.
6. The build starts automatically. Jump to **Getting the built mods** below.

---

## Recipe A — change a default value

Defaults are set where each option is **registered**, in the mod's `mod_initialize` /
`init()` code. There are two shapes to look for.

### Shape 1: a `ConfigVarDesc` block

Search the file for the option name (the `.name = "..."` line) and change the `default_*` line just
below it. Example — **Graphics Hub**, make Deferred Fog **off** by default (it ships on):

```cpp
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "fogEnabled";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;      //  <-- change to false
```

Example — **Effect Remover**, make Unbaked Vertex Lighting default to 50 instead of 100:

```cpp
    cvarDesc.name = "vertexLight";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 100;        //  <-- change to 50
```

- `default_bool` is `true` or `false`.
- `default_int` is a whole number (respect the control's min/max — Vertex Lighting is 0–100).

### Shape 2: a `register_bool(...)` call

Effect Remover registers most of its toggles with a helper. The **third-from-last** argument is the
default. Examples (all in `mods/effect_remover/src/mod.cpp`):

```cpp
    // Terrain Shadow Removal ships OFF; make it ON by default:
    register_bool("tsrEnabled", false, g_cvarEnabled, error);   //  false -> true

    // Projected Shadow Removal master switch (ships ON):
    register_bool("psrEnabled", true, g_cvarEnabled, error);    //  true -> false
```

Some defaults are written as a small expression instead of a plain `true`/`false`:

```cpp
    // Moya per-mode toggles: only mode 5 defaults ON. `mode == 5` is the default value.
    register_bool(name, mode == 5, g_cvarSuppress[mode], error);
    //   -> to also remove mode 4 by default: (mode == 5 || mode == 4)
    //   -> to default ALL modes on:          true
```

```cpp
    // Terrain shadow codes: all default ON when the feature is enabled.
    register_bool(name, true, g_cvarRemove[i], error);
    //   -> to default only MA04 on:
    //      register_bool(name, (kCodes[i].c5 == '0' && kCodes[i].c6 == '4'), g_cvarRemove[i], error);
```

> After changing a default, players who already have the mod keep their **saved** setting; the new
> default only applies to a fresh install / a setting they never touched. To force your value on
> everyone, use Recipe B.

---

## Recipe B — lock an option to a fixed value (hardcode)

Two levels, from easiest to most thorough.

### B1 (easy, recommended): set the default AND hide the control

Do Recipe A to set the value you want, then stop showing the control so nobody changes it. Controls
are added in a `build_section(...)` (or `build_panel` / `build_controls_tab`) function. Comment out
the one line that adds the control by putting `//` in front of it:

```cpp
    // add_toggle(panel, "Log Overlay Materials", g_cvarLog, "...");
```

The option still exists internally at your chosen default; it just no longer appears in the UI. This
is the safe way — nothing else needs to change.

### B2 (thorough): replace the option with a constant

If you want the value truly baked in (option gone entirely), find where the code **reads** the
option and replace the read with your constant. The reads look like `get_bool_option(handle, ...)`,
`get_int_option(handle, ...)`, or `svc_config->get_int(...)`.

Example — force Terrain Shadow Removal always enabled, ignoring the toggle. In
`er_tsr::update()`:

```cpp
    g_enabledCached = get_bool_option(g_cvarEnabled, false);   // reads the toggle
    //  ->  g_enabledCached = true;                            // always on
```

You can then also delete that option's registration (Recipe A block) and its control (B1) if you
want it gone completely. If you're unsure, **stick to B1** — leaving the registration in place is
harmless and can't break the build.

> Build-safety rule of thumb: changing a **value** (a number, `true`/`false`) is always safe.
> Deleting whole lines is where typos creep in — comment out (`//`) rather than delete when you can,
> and change one thing per commit so a red build is easy to trace.

---

## Recipe C — rename a mod or edit its store text

Open `mods/<mod-name>/mod.json` and edit the `"name"`, `"description"`, or bump `"version"` (any
higher number, e.g. `1.0.0` → `1.0.1`). Bumping the version isn't required but makes it obvious in
the mod manager that you installed a newer build.

---

## Getting the built mods

1. On GitHub, open the **Actions** tab.
2. Click the newest run at the top (it's named after your commit message). Wait for the green
   check (~6 minutes). A red X means the build failed — see below.
3. Scroll to the bottom of the run page to the **Artifacts** section.
4. Download **`mods-combined`** (this is the one bundle with all mods, each as a single
   cross-platform `.dusk`). Unzip it.
5. Copy the `.dusk` files you changed into your game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
6. In game: open the Mods menu and press **Reload** — no restart needed.

## If the build fails (red X)

1. Click the failed run → click the red job → scroll the log to the first line containing
   `error:`. It names the file and line number.
2. The usual cause is a typo from an edit: a missing `;`, a deleted `}`, or a stray character.
   Compare against what you changed.
3. Easiest fix: on GitHub, open the file's **History**, find your commit, and revert it (or just
   re-edit the line back). A value change (Recipe A) almost never fails to build; deletions are the
   risky part, which is why B1 (comment-out) is recommended over deleting.

## Where each option lives (quick index)

- **Graphics Hub** — `mods/graphics_hub/src/mod.cpp`
  - Depth to Normal: namespace `hub_dtn`, option `normalsDebug`.
  - Deferred Fog: namespace `hub_fog`, options `fogEnabled`, `fogMixedMode`, `fogDebug`.
- **Effect Remover** — `mods/effect_remover/src/mod.cpp`
  - Projected Shadow Removal: namespace `er_psr`, options `psrEnabled`, `psrLogMode`,
    `psrSuppress0`…`psrSuppress11`.
  - Terrain Shadow Removal: namespace `er_tsr`, options `tsrEnabled`, `tsrLog`, `tsrRemoveMA00`,
    `tsrRemoveMA01`, `tsrRemoveMA16`, `tsrRemoveMA04`.
  - Unbaked Vertex Lighting: namespace `er_vu`, option `vertexLight`.
- **VBAO** — `mods/vbao/src/mod.cpp`; **SSILVB** — `mods/ssilvb/src/mod.cpp`; **Realtime Sun
  Shadows** — `mods/realtime_sun_shadows/src/mod.cpp` (same `ConfigVarDesc` / `default_*` pattern).

In **Graphics Hub** and **Effect Remover**, the defaults are marked in the code with a `// DEFAULT`
comment next to the value — search the file for `DEFAULT` to jump straight to them. The other mods
use the same `default_bool` / `default_int` pattern; search for the option name to find its block.
