# Changing defaults, hiding options, and editing descriptions

For a pass over the mods without reading the rest of the codebase. Everything here is one edit in
one file, and none of it needs the game or the renderer built — `cmake --build build` recompiles a
mod in seconds, or just push and let CI do it.

---

## Change a default

Each mod registers its options from a **table** near the bottom of `mods/<mod>/src/mod.cpp`, in
`mod_initialize`. One line per option: config name, default, handle.

```cpp
} intOptions[] = {
    {"quality",   2,   &g_cvarQuality},
    {"radius",    200, &g_cvarRadius},      // <- change the number, that is the whole edit
    {"intensity", 150, &g_cvarIntensity},
```

Find the table with:

```sh
grep -n "boolOptions\[\]\|intOptions\[\]" mods/*/src/mod.cpp
```

**The table is the only thing you have to change.** You will also see the same number repeated at
the point the option is read — `get_int_option(g_cvarRadius, 200)`. That second number is a
*fallback*, used only if the option failed to register at all, which in practice never happens. It
does not override the default and you cannot break a default by leaving it alone. Worth keeping in
step so the file does not lie to the next reader, but nothing depends on it.

Three shapes exist, so here is exactly where each mod keeps its defaults:

| Mod | Where the defaults are | Grep for |
| :-- | :-- | :-- |
| **VBAO** | two tables, `boolOptions[]` and `intOptions[]` | `intOptions\[\]` |
| **SMAA** | `intOptions[]` for the sliders; its two switches are literal arguments to `register_bool_option` | `intOptions\[\]`, `register_bool_option("` |
| **Deferred Fog** | one block per option — the default is the `default_bool` / `default_int` line inside each | `cvarDesc.name = "` |

Same idea in all three: one number, one edit. (Deliberately no line numbers here — they are wrong
the moment anyone edits the file, which is the whole point of this document.)

**Integer scaling.** Many options are stored as integers and scaled when read — `intensity` 150
means 1.5, `radius` 200 means 2.0. Look at the read site to see the divisor before picking a new
default:

```cpp
uniforms.intensity = static_cast<float>(get_int_option(g_cvarIntensity, 150)) * 0.01f;
```

**Saved settings win.** A user who has already moved a slider keeps their value; a new default only
applies to someone who has not touched it. When testing your own change, either use a clean config
or move the slider back and forth once.

---

## Hide an option from the UI (keep it working)

Delete or comment out its `add_control(...)` block in `build_controls_window` / `build_panel`. The
option stays registered, keeps its default, and anything already using it keeps working — it simply
stops being shown.

```cpp
control = UI_CONTROL_DESC_INIT;
control.kind = UI_CONTROL_SLIDER;
control.label = "Depth Bias";
...
add_control(section, control);     // <- remove this line, or the whole block
```

This is the safe way to simplify a panel for release. It cannot break anyone's saved settings,
and putting the control back later is a one-line revert.

## Remove an option completely

Only if you also want the setting gone from the config file:

1. Remove its line from the options table.
2. Remove its `add_control(...)` block.
3. Remove the `ConfigVarHandle g_cvarThing = 0;` global and any `get_*_option(g_cvarThing, ...)`
   reads — the compiler will point at every one of them, so let it.

Anyone who had that setting saved keeps a harmless orphan entry in their config file.

**Never name an option `enabled`.** The game reserves `mod.<id>.enabled` for the mod manager's own
checkbox, so registering it fails and the whole mod fails to load, silently. Prefix it —
`effectEnabled`, `fogEnabled`. `python3 tools/check_reserved_config_names.py` catches this.

---

## Edit a description

`mods/<mod>/mod.json`, the `description` field. Three constraints that are not obvious:

- **Newlines collapse.** The manager inserts the text as an RML text node and the stylesheet does
  not set `white-space: pre-wrap` (`res/rml/mods.rcss`), so `\n` renders as a single space. Write
  one flowing block; paragraph breaks will not survive.
- **The list view shows about two lines.** The first sentence is what most people read, so it has
  to stand on its own.
- It is plain text, not RML — tags will show as literal text.

Per-option help text is different: `control.help_rml` **does** take RML, so `<br/>` and `<b>` work
there.

---

## Add an icon or banner

Drop the files in and rebuild — **no `mod.json` change needed**:

```
mods/<mod>/res/icon.png
mods/<mod>/res/banner.png
```

Those exact paths are the defaults the loader looks for. To use a different name or location, set
`"icon"` / `"banner"` in `mod.json` to a path inside the bundle (e.g. `"res/art/logo.png"`); if the
path is missing or unsafe the loader logs a warning and falls back to the default path, then to
nothing. Anything under `res/` is packaged automatically.

---

## Where the long-form text lives

Front-facing descriptions stay short on purpose. Provenance, third-party licences and attribution
live in `mods/<mod>/res/licenses/`, which ships inside the `.dusk` — present and findable, but only
if someone goes looking. Do not move that material into `mod.json`.

---

## After editing

```sh
cmake --build build                      # -> build/mods/*.dusk
python3 tools/check_reserved_config_names.py
```

Shader edits additionally need `./build/wgsl_check mods/*/res/*.wgsl` — CI does **not** validate
WGSL, so a broken shader ships and only fails in-game.
