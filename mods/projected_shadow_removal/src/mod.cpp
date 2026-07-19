// Projected Shadow Removal — selectively suppresses Twilight Princess's fake projected
// ground-shade effects, per effect type.
//
// TP has no real-time environment shadow casting; it fakes atmospheric ground shade with the
// "moya" system. drawCloudShadow (the kankyo cloud packet's draw) renders a field of soft
// wind/animation-driven particles projected onto the ground. The SAME function draws every
// variant, distinguished by dScnKy_env_light_c::mMoyaMode:
//   - the slowly swaying dappled canopy shade on forest floors (Faron/Ordon),
//   - the rolling cloud shadows drifting across Hyrule Field,
//   - drifting mist / dust / steam variants,
// each with a per-stage texture and per-mode motion. The mode is set per area by the map's
// "kytag" actors, so which NUMBER is the forest canopy vs the field clouds is map data, not
// something the code fixes — hence this mod suppresses BY MODE and ships a live mode logger so
// the exact number for any spot can be read off in-game.
//
// Behavioral notes from the game's cloud_shadow_move (used for the default + UI hints):
//   - mode 5 is the only pure non-wind SLOW SWAY → the forest-canopy candidate (default: removed),
//   - modes 4 and 11 are WIND-DRIVEN drift → the rolling-cloud candidates (default: kept),
//   - modes 6/8/10/11 add vertical rise → drifting mist/steam (default: kept).
// mMoyaMode >= 50 is a different effect entirely (framebuffer heat-shimmer / wolf-senses
// distortion) and is always left alone.
//
// MECHANISM: a pre-hook on drawCloudShadow that cancels the call only when the current mode's
// suppression toggle is on (the Deferred Fog suppression tactic). Skipping a self-contained
// immediate-mode draw is safe — the next draw sets its own GX state.
//
// Game-linked: hooks a game function, coupled to the pinned game build.

#include "global.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"       // dKy_getEnvlight, dScnKy_env_light_c::mMoyaMode
#include "d/d_kankyo_rain.h"  // drawCloudShadow
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <cstdio>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

// The projected-ground-shade draw (kankyo cloud packet). Free function in d/d_kankyo_rain.h.
DEFINE_HOOK(drawCloudShadow, CloudShadow);

namespace {

// Suppressible moya modes: 0..kMaxMode. Mode 0 IS a valid drawn effect (kytag00 can set mode 0
// with a positive count — a default wind-drift shade), so it gets a toggle too. mMoyaMode >= 50
// is the framebuffer shimmer/senses branch and is always preserved.
constexpr int kMaxMode = 11;  // mMoyaMode > kMaxMode (incl. >= 50 shimmer/senses) is left alone

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarLogMode = 0;
ConfigVarHandle g_cvarSuppress[kMaxMode + 1] = {};  // index by mode 0..11

// Logging state (mod_update, game thread): report the live moya mode+count so an area's effect
// can be identified even when drawCloudShadow itself isn't being called (no active cloud packet).
int g_lastLoggedMode = -1000;
int g_lastLoggedCount = -1000;

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

// Game thread, in drawCloudShadow's own frame. Reads the live moya mode (dKy_getEnvlight() is the
// same accessor the game uses inside drawCloudShadow) and cancels the draw only for a mode whose
// suppression toggle is on.
HookAction on_cloud_shadow_pre(ModContext*, void*, void*, void*) {
    if (!get_bool_option(g_cvarEnabled, true)) {
        return HOOK_CONTINUE;
    }
    const dScnKy_env_light_c* envLight = dKy_getEnvlight();
    if (envLight == nullptr) {
        return HOOK_CONTINUE;
    }
    const int mode = static_cast<int>(envLight->mMoyaMode);
    if (mode < 0 || mode > kMaxMode) {
        return HOOK_CONTINUE; // the shimmer/senses branch — leave it
    }
    if (get_bool_option(g_cvarSuppress[mode], false)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

// Per-mode labels + hints. nullptr => generic drifting-shade mode (labelled "Mode N").
const char* mode_label(int mode) {
    switch (mode) {
    case 0: return "Mode 0 — default wind drift";
    case 4: return "Mode 4 — wind cloud shadows";
    case 5: return "Mode 5 — slow-sway canopy shade";
    case 11: return "Mode 11 — strong wind drift";
    default: return nullptr; // filled generically below
    }
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "General");
    add_toggle(left, "Enabled", g_cvarEnabled,
        "Master switch for the per-mode suppression below.");
    add_toggle(left, "Log Active Mode", g_cvarLogMode,
        "Prints the live projected-shade mode AND count to the log every time either changes "
        "(runs continuously, not only while the effect draws). Turn it on and it reports the "
        "current mode immediately; walk into a spot (under forest canopy, then out into Hyrule "
        "Field) to read off each effect's mode. If an on-screen shade persists but the log "
        "shows count 0, that shade is NOT this projected-shade system - it's a different "
        "technique (e.g. an animated texture on the terrain).");

    svc_ui->pane_add_section(mod_ctx, left, "Suppress by Mode");
    static char kGenericLabels[kMaxMode + 1][16];
    for (int mode = 0; mode <= kMaxMode; ++mode) {
        const char* label = mode_label(mode);
        if (label == nullptr) {
            std::snprintf(kGenericLabels[mode], sizeof(kGenericLabels[mode]), "Mode %d", mode);
            label = kGenericLabels[mode];
        }
        add_toggle(left, label, g_cvarSuppress[mode],
            "Removes this projected-shade effect. Use Log Active Mode to find which number an "
            "on-screen effect uses. Mode 5 (default on) is the slow-swaying canopy shade; the "
            "wind-driven cloud shadows are usually a different mode, left on so Hyrule Field "
            "keeps its drifting shadows.");
    }
    return MOD_OK;
}

UiWindowHandle g_controlsWindow = 0;

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Controls";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open Projected Shadow Removal controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarEnabled;
    add_control(panel, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool(const char* name, bool def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = name;
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = def;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = register_bool("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool("logMode", false, g_cvarLogMode, error);
    if (result != MOD_OK) {
        return result;
    }
    for (int mode = 0; mode <= kMaxMode; ++mode) {
        char name[24];
        std::snprintf(name, sizeof(name), "suppressMode%d", mode);
        // Default: only the slow-sway canopy candidate (mode 5) is removed; everything else
        // (including the wind cloud shadows) is kept until the user opts in.
        result = register_bool(name, mode == 5, g_cvarSuppress[mode], error);
        if (result != MOD_OK) {
            return result;
        }
    }

    if (mods::hook_add_pre<CloudShadow>(svc_hook, on_cloud_shadow_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook the projected-shade draw");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "projected_shadow_removal ready");
    return MOD_OK;
}

// Runs every frame on the game thread. Reports the live moya mode + count when logging is on,
// regardless of whether the projected-shade draw is happening this frame — so an effect can be
// identified even where drawCloudShadow isn't being called, and a count of 0 tells the user the
// shade they see is NOT this system.
MOD_EXPORT ModResult mod_update(ModError*) {
    if (!get_bool_option(g_cvarLogMode, false)) {
        g_lastLoggedMode = -1000;
        g_lastLoggedCount = -1000;
        return MOD_OK;
    }
    const dScnKy_env_light_c* envLight = dKy_getEnvlight();
    if (envLight == nullptr) {
        return MOD_OK;
    }
    const int mode = static_cast<int>(envLight->mMoyaMode);
    const int count = envLight->mMoyaCount;
    if (mode != g_lastLoggedMode || count != g_lastLoggedCount) {
        g_lastLoggedMode = mode;
        g_lastLoggedCount = count;
        char msg[80];
        std::snprintf(msg, sizeof(msg), "projected-shade: moya mode = %d, count = %d", mode, count);
        svc_log->info(mod_ctx, msg);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarEnabled = 0;
    g_cvarLogMode = 0;
    for (auto& handle : g_cvarSuppress) {
        handle = 0;
    }
    g_lastLoggedMode = -1000;
    g_lastLoggedCount = -1000;
    g_controlsWindow = 0;
    return MOD_OK;
}
}
