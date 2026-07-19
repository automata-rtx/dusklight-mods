// Projected Shadow Removal — suppresses Twilight Princess's fake projected ground shadows.
//
// TP has no real-time shadow casting for the environment; instead the "moya" system projects a
// scrolling texture onto the ground to fake it. The SAME function, drawCloudShadow (the kankyo
// cloud packet's draw), renders every variant of this:
//   - the slowly swaying dappled canopy shadows on forest floors (Faron, Ordon),
//   - the rolling cloud shadows drifting across Hyrule Field,
//   - a few other drifting-shade moya modes,
// each with a per-stage texture and an animated (rotating/scrolling) projection matrix — which
// is exactly why they read as "swaying tree shadows" in one place and "rolling clouds" in
// another. They are the game's stand-in for the shadows our Realtime Sun Shadows + SSILVB stack
// now provides for real, so this mod removes them.
//
// MECHANISM: a pre-hook on drawCloudShadow that cancels the original call (the same tactic
// Deferred Fog uses to suppress the game's fog draw). Skipping a self-contained immediate-mode
// draw is safe — the next draw sets its own GX state — so nothing downstream is disturbed.
//
// PRESERVED: drawCloudShadow's mMoyaMode >= 50 branch is a different effect entirely (it samples
// the framebuffer to project a heat-shimmer/refraction, e.g. Death Mountain). The hook only
// cancels the projected-shadow branch (mMoyaMode < 50), so the shimmer still works.
//
// Game-linked: hooks a game function, so it is coupled to the pinned game build.

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

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

// The projected-ground-shadow draw (kankyo cloud packet). Free function declared in
// d/d_kankyo_rain.h.
DEFINE_HOOK(drawCloudShadow, CloudShadow);

namespace {

ConfigVarHandle g_cvarEnabled = 0;

// mMoyaMode >= this is the framebuffer-based heat-shimmer branch, not a ground shadow; leave it.
constexpr int kShimmerModeFloor = 50;

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

// Game thread, in drawCloudShadow's own frame: cancel the projected-shadow draw. Reads the live
// moya mode (dKy_getEnvlight() is the same accessor the game uses inside drawCloudShadow) so the
// heat-shimmer branch is never touched.
HookAction on_cloud_shadow_pre(ModContext*, void*, void*, void*) {
    if (!get_bool_option(g_cvarEnabled, true)) {
        return HOOK_CONTINUE;
    }
    const dScnKy_env_light_c* envLight = dKy_getEnvlight();
    if (envLight != nullptr && envLight->mMoyaMode >= kShimmerModeFloor) {
        return HOOK_CONTINUE; // heat shimmer / refraction — keep it
    }
    return HOOK_SKIP_ORIGINAL; // projected cloud / canopy shadow — remove it
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Remove Projected Shadows";
    control.help_rml =
        "Removes the game's fake projected ground shadows: the swaying dappled shade under "
        "forest canopies and the rolling cloud shadows over Hyrule Field. Leave on so the "
        "realtime shadow/GI mods carry the shading instead. Heat-shimmer effects (Death "
        "Mountain) are unaffected.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarEnabled;
    add_control(panel, control);
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "effectEnabled";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarEnabled) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register effectEnabled option");
    }

    if (mods::hook_add_pre<CloudShadow>(svc_hook, on_cloud_shadow_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook the projected-shadow draw");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "projected_shadow_removal ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarEnabled = 0;
    return MOD_OK;
}
}
