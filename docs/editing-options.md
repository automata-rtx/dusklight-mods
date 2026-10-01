# Changing defaults, hiding options, and editing descriptions

Small tuning changes without reading the rest of the codebase. Each is one edit in one file, and
none needs the game built. `cmake --build build` recompiles a mod in seconds, or push and let CI
build it (see `CONTRIBUTING.md`, or `self_editing_guide.md` to do it all in the GitHub web editor).

---

## Change a default

| Mod | Where the defaults are | Grep for |
| :-- | :-- | :-- |
| **VBAO** | Two tables in `mod_initialize`: `boolOptions[]` and `intOptions[]`, one line per option | `intOptions\[\]` |
| **SMAA** | `intOptions[]` for the numbers; its two switches are literal arguments to `register_bool_option(...)` | `intOptions\[\]`, `register_bool_option("` |
| **Deferred Fog** | One block per option in `init()`; the default is the `default_bool` / `default_int` line in it | `cvarDesc.name = "` |

A table line looks like this:

```cpp
} intOptions[] = {
    {"quality", 2,   &g_cvarQuality},
    {"radius",  200, &g_cvarRadius},     // <- change the number; that is the whole edit
```

**The table (or block) is the only thing that sets the default.** You will also see the number
repeated where the option is read, for example `get_int_option(g_cvarRadius, 200)`. That second
number is a fallback used only if registration failed, which in practice never happens. Keep it in
step so the code does not mislead the next reader, but nothing depends on it. (VBAO currently has
four that are out of step: `halfRes`, `blackPoint`, `depthBias` and `temporalFrames`.)

**Check the range and the scale at the read site** before picking a value. Options are stored as
integers and converted when read, usually with a clamp:

```cpp
uniforms.intensity = percent(g_cvarIntensity, 150, 0, 500);   // 150 -> 1.5, clamped to 0..500
uniforms.effect_radius = ... clamp(get_int_option(g_cvarRadius, 200), 25, 800)) / 1000.0f;  // 200 -> 0.2
```

Most options are ×0.01 (percent). Some VBAO options are ×0.001 (`radius`, `radiusFar`,
`thickDist`, `depthBias`) and some are plain world units (`radiusRampStart`, `motionRange`,
`fadeStart`, and so on). A default outside the clamp range is silently clamped. The per-mod docs
(`vbao.md`, `smaa.md`, `deferred_fog.md`) list every option with its range and scale.

**Saved settings win.** Anyone who already moved a control keeps their value; a new default only
reaches people who never touched it. When testing your own change, reset the option in the UI or
use a clean `config.json`.

---

## Hide an option from the UI (keep it working)

Remove the line that adds its control. The option stays registered, keeps its default, and keeps
working; it just stops being shown. This cannot break anyone's saved settings, and putting the
control back is a one-line revert.

- **VBAO and SMAA**: each control is one call in `build_controls_tab`. Comment it out:

  ```cpp
  // add_number(left, "Depth Bias", g_cvarDepthBias, "...", 0, 20, 1, nullptr);
  ```

- **Deferred Fog**: each control is a short block in `build_controls_tab` ending in
  `add_control(left, control);`. Comment out that last line, or the whole block.

The **Enabled** toggle appears twice in each mod: in the mod's pane (`build_section` or
`build_panel`) and in the controls window. Hide both if you hide one.

## Remove an option completely

Only if you also want the setting gone from the config file:

1. Remove its line from the option table (or its registration block).
2. Remove its UI control.
3. Remove the `ConfigVarHandle g_cvarThing` global and every `get_*_option(g_cvarThing, ...)` read.
   The compiler will point at each one.

Anyone who had it saved keeps a harmless orphan entry in their config file.

**Never name an option `enabled`.** The game reserves `mod.<id>.enabled` for the mod manager's own
checkbox, so registering it fails and the whole mod fails to load, silently. Prefix it
(`effectEnabled`, `fogEnabled`). `python3 tools/check_reserved_config_names.py` catches this for
Deferred Fog's style of registration, but **not** for names in VBAO's and SMAA's tables; check those
by eye.

---

## Edit a description

`mods/<mod>/mod.json`, the `description` field. Three constraints that are not obvious:

- **Newlines collapse.** The mod manager does not preserve line breaks, so `\n` renders as a space.
  Write one flowing paragraph.
- **The list view shows about two lines.** The first sentence is what most people read, so it has
  to stand on its own.
- It is plain text, not RML; tags show up literally.

Per-option help text is different: `help_rml` strings **do** take RML, so `<br/>` and `<b>` work
there.

---

## Add an icon or banner

Drop the files in and rebuild; no `mod.json` change is needed:

```
mods/<mod>/res/icon.png
mods/<mod>/res/banner.png
```

Those are the paths the loader looks for by default. To use other names, set `"icon"` / `"banner"`
in `mod.json` to a path inside the bundle. VBAO and Deferred Fog do this; SMAA ships
`res/SMAA Logo.png` but sets no `icon` key, so it currently shows no icon. Anything under `res/` is
packaged automatically.

---

## Where the long-form text lives

Descriptions stay short on purpose. Provenance, third-party licences and attribution live in
`mods/<mod>/res/licenses/`, which ships inside the `.dusk`. Do not move that material into
`mod.json`.

---

## After editing

```sh
cmake --build build                      # -> build/mods/*.dusk
python3 tools/check_reserved_config_names.py
```

Shader edits also need the shader validator, which CI does **not** run; `CONTRIBUTING.md` "Check
your change" shows how to build and run `build/wgsl_check`.
