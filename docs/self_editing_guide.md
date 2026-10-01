# Changing defaults or hiding options in the GitHub web editor

For **small, safe tweaks** (changing a default value, or fixing an option to one value and hiding
it) with no local setup at all. You edit one file on GitHub.com, GitHub Actions builds every mod for
all seven platforms, and you download the finished `.dusk` files.

This covers the three released mods: **VBAO**, **Deferred Fog** and **SMAA**. The other mods in
`mods/` are not built, so editing them produces nothing.

You only ever touch two files inside `mods/<mod-name>/`:

- `src/mod.cpp`: the code. Defaults and options live here.
- `mod.json`: the mod's name, version and description.

Never edit anything in a `dusklight/` folder. That is the game's source, fetched automatically
during a build, and it is not part of this repository.

---

## The loop

1. Edit a file on GitHub.com (below).
2. Commit it to a branch. Actions builds every mod automatically on every branch (a few minutes).
3. Download the `mods-combined` artifact from that run.
4. Copy the `.dusk` files into your game's mods folder and press **Reload** in the mod manager.

---

## Editing a file on GitHub.com

1. Open the repository on GitHub and pick the branch to start from (usually `main`) in the branch
   selector above the file list.
2. Open the file, for example `mods/vbao/src/mod.cpp`.
3. Click the **pencil icon** (Edit this file).
4. Make your change (recipes below).
5. Click **Commit changes**. Choosing **Create a new branch for this commit** keeps `main` untouched
   until you are happy with the result.
6. Go to **Getting the built mods**.

Use your browser's find (Ctrl+F / Cmd+F) inside the editor to jump to the text a recipe mentions.

---

## Recipe A: change a default value

Each mod keeps its defaults in one place. Find the option by its config name; the per-mod docs
(`docs/vbao.md`, `docs/smaa.md`, `docs/deferred_fog.md`) list every option with its range and what
the number means.

### VBAO and SMAA: a table line

Search for `intOptions[]` (numbers) or, in VBAO, `boolOptions[]` (on/off switches). One line per
option: name, default, handle.

```cpp
{"intensity", 150, &g_cvarIntensity},     //  <-- change 150
{"halfRes", true, &g_cvarHalfRes},         //  <-- true or false
```

SMAA's two switches are not in a table. Search for `register_bool_option("`:

```cpp
register_bool_option("useNormalEdges", true, g_cvarUseNormalEdges, error)   // the true -> false
```

### Deferred Fog: a registration block

Search for the option name, for example `"fogEnabled"`, and change the `default_*` line under it:

```cpp
    cvarDesc.name = "fogEnabled";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;      //  <-- change to false
```

Each block has a `// DEFAULT` comment just above it, so searching for `DEFAULT` jumps between them.

### Rules

- `true` / `false` for switches; whole numbers for everything else.
- Stay inside the option's range. The range is the pair of numbers in the `std::clamp` (or the
  `percent(...)` call) where the option is read; a value outside it is silently clamped.
- Many numbers are scaled: VBAO's `intensity` 150 means 1.5, `radius` 200 means 0.2. The per-mod
  docs give the scale for each.

> Players who already have the mod keep their **saved** setting; a new default only reaches people
> who never touched that option. To force a value on everyone, use Recipe B.

---

## Recipe B: fix an option to one value

### B1 (recommended): set the default and hide the control

Do Recipe A, then stop showing the control so nobody changes it. Controls are added in the function
`build_controls_tab`. Put `//` at the start of the line that adds the control:

```cpp
    // add_number(left, "Depth Bias", g_cvarDepthBias, ...
```

In VBAO and SMAA one control is one call, which may continue over the next few lines; comment out
every line of that call, up to its closing `);`. In Deferred Fog each control is a short block ending
in `add_control(left, control);`; commenting out that last line is enough.

The **Enabled** switch appears twice per mod: in the controls window and in the mod's own pane
(`build_panel`, or `build_section` in Deferred Fog). Hide both if you hide one.

### B2: replace the option with a constant

Only if you want the setting gone entirely. Find where the code reads the option
(`get_bool_option(g_cvarSomething, ...)` or `get_int_option(...)`) and replace the read with your
value. Then you may also remove its registration and its control. If unsure, use B1: leaving the
registration in place is harmless and cannot break the build.

> Changing a **value** is always safe. Deleting lines is where typos creep in. Comment out (`//`)
> rather than delete, and change one thing per commit so a failed build is easy to trace.

---

## Recipe C: edit the name or description

Open `mods/<mod-name>/mod.json` and edit `"name"` or `"description"`, or bump `"version"` (for
example `1.0.2` → `1.0.3`) so the mod manager shows that you installed a newer build. Keep the
description to one paragraph; line breaks are not displayed.

---

## Getting the built mods

1. Open the **Actions** tab on GitHub.
2. Click the newest run (named after your commit message) and wait for the green check. A red X
   means the build failed; see below.
3. At the bottom of the run page, under **Artifacts**, download **`mods-combined`** and unzip it.
4. Copy the `.dusk` files you changed into the game's mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
5. In the game, open the Mods menu and press **Reload**.

## If the build fails

1. Click the failed run, then the red job, and scroll the log to the first line containing
   `error:`. It names the file and line.
2. The usual cause is a typo: a missing `;`, a deleted `}`, a half-commented call.
3. Undo it: on GitHub, open the file's **History**, find your commit and revert it, or edit the line
   back.

## Option names

- **VBAO** (`mods/vbao/src/mod.cpp`): see the table in `docs/vbao.md`.
- **SMAA** (`mods/smaa/src/mod.cpp`): `effectEnabled`, `blendStrength`, `edgeThreshold`,
  `localContrast`, `useNormalEdges`, `normalThreshold`, `depthThreshold`, `maxSearchSteps`,
  `debugMode`.
- **Deferred Fog** (`mods/deferred_fog/src/mod.cpp`): `fogEnabled`, `fogMixedMode`,
  `fogSkipUnfogged`, `fogDeferInSenses`, `fogDebug`, `fogLogConfigs`.
