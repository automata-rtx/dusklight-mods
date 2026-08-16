// Celestial Orbit — raises (or lowers, or spins) the path the sun and the moon travel across the
// sky, so realtime lighting has something to work with at midday.
//
// Twilight Princess sweeps both bodies around a great circle that is tilted well off vertical.
// dScnKy_env_light_c::setSunpos() builds the offset from the camera eye as
//
//     x = sin a * 80000        y = -cos a * 80000        z = -cos a * 48000
//
// for a swept angle `a` remapped from the time of day. Because y and z are both driven by cos(a),
// z is just y * (48000 / 80000) = y * 0.6, and the highest the body ever gets is
// atan(1 / 0.6) = 59.036 degrees. That is the whole cap: nothing else limits the elevation.
//
// This mod post-hooks setSunpos and re-derives z from y with a different ratio, so the peak
// elevation becomes a knob (ratio = cot(peak)). The sweep itself is untouched — same angle at the
// same time of day, same two horizon crossings — so sunrise/sunset barely move and NOTHING about
// timing changes: the clock, the palette schedule, and dawn/dusk/night selection all run off
// dComIfGs_getTime() and l_time_attribute and never read sun_pos, moon_pos, or the orbit. An
// optional yaw then spins the whole path about world Y, moving where the bodies rise and set.
//
// Everything downstream of sun_pos / moon_pos follows for free: the day/night base light
// (SetBaseLight), the weather light direction, the sun billboard, the lens flare and the moon.
// The one consumer that does NOT follow is a mod that recomputes the direction from the time of
// day itself — Realtime Sun Shadows does, so its debug time-of-day override can move the light —
// which is why the modded orbit is also published as the "dev.automata.celestial_orbit" service
// (include/celestial_orbit_service.h). Consumers push their own vanilla offset through the shared
// celestial_orbit_apply_offset() and land on exactly the position written here.
//
// Peak elevation is capped at 80 degrees on purpose. At 90 the arc passes through the zenith, the
// azimuth flips instantaneously at noon, and any light-space view matrix (a shadow map's) has its
// up vector degenerate as the direction approaches vertical — shadows snap around. 80 keeps a
// comfortable margin.
//
// Game-linked: hooks a game function and writes game state, so it is coupled to the pinned build.

#include "global.h"

#include "celestial_orbit_service.h"

#include "d/d_com_inf_game.h"   // dComIfGp_getCamera, dComIfGp_getStartStageName
#include "d/d_kankyo.h"         // dScnKy_env_light_c::setSunpos
#include "f_op/f_op_camera_mng.h"
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

namespace {

// Hook target (emits a modmeta hook record the host resolves at load). setSunpos is a member
// function with no parameters, so the post callback's arg 0 is `this`.
DEFINE_HOOK(&dScnKy_env_light_c::setSunpos, SetSunpos);

// Peak-elevation range offered in the UI, in degrees.
//   * 80 is the safe ceiling (zenith azimuth flip / degenerate light-space up vector — see above).
//   * 15 is the floor: the peak is the highest the body gets all day, and Realtime Sun Shadows
//     already fades its shadows out below ~11 degrees, so a lower peak would mean a day with
//     essentially no realtime shadows in it.
constexpr int kMinElevation = 15;
constexpr int kMaxElevation = 80;

// Vanilla is 59.036 degrees. The whole-degree knob cannot land on it exactly, so bit-exact vanilla
// is what the Enabled toggle is for: switched off, this mod writes nothing at all.
constexpr int kDefaultElevation = 75;

constexpr float kPi = 3.14159265358979323846f;

ConfigVarHandle g_cvarEnabled = 0;        // DEFAULT in init()
ConfigVarHandle g_cvarSunElevation = 0;   // DEFAULT in init()
ConfigVarHandle g_cvarMoonElevation = 0;  // DEFAULT in init()
ConfigVarHandle g_cvarYaw = 0;            // DEFAULT in init()

// Refreshed once per frame on the game thread (refresh_state), read by the hook and handed to
// consumers through the service. Starts vanilla/inactive so the first frame — and any frame before
// mod_update has run — behaves exactly like the unmodded game.
CelestialOrbitState g_state = CELESTIAL_ORBIT_STATE_INIT;

// The positions this mod last wrote. setSunpos leaves sun_pos / moon_pos untouched on the frames
// it bails out of (no camera, or the F_SP200 stage), and re-tilting our own output would be fine
// for the z lean (it is derived from y, which we never change) but would compound the yaw. If the
// values are bit-identical to what we wrote, nothing fresh was written, so there is nothing to do.
// A frame where the game DID write cannot be falsely skipped: for the game's vanilla write to
// equal our previous output the transform would have to be a no-op on that value anyway.
cXyz g_lastSunPos(0.0f, 0.0f, 0.0f);
cXyz g_lastMoonPos(0.0f, 0.0f, 0.0f);
bool g_haveLastPos = false;

bool same_pos(const cXyz& a, const cXyz& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

int get_int_option(ConfigVarHandle handle, int fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

// Peak elevation (degrees) -> the z/y lean the orbit needs to reach it: ratio = cot(elevation),
// the inverse of the vanilla peak = atan(1 / ratio). Clamped to the offered range, so a config
// file edited by hand cannot ask for the zenith.
float z_ratio_for_elevation(int degrees) {
    const float radians =
        static_cast<float>(std::clamp(degrees, kMinElevation, kMaxElevation)) * (kPi / 180.0f);
    const float sinE = std::sin(radians);
    if (!(sinE > 0.0f)) {
        return CELESTIAL_ORBIT_VANILLA_Z_RATIO;
    }
    return std::cos(radians) / sinE;
}

// Game thread, once per frame (mod_update) and once at init.
void refresh_state() {
    CelestialOrbitState state = CELESTIAL_ORBIT_STATE_INIT;
    if (get_bool_option(g_cvarEnabled, true)) {
        const float yaw = static_cast<float>(std::clamp(get_int_option(g_cvarYaw, 0), -180, 180)) *
            (kPi / 180.0f);
        state.active = 1u;
        state.sun_z_ratio =
            z_ratio_for_elevation(get_int_option(g_cvarSunElevation, kDefaultElevation));
        state.moon_z_ratio =
            z_ratio_for_elevation(get_int_option(g_cvarMoonElevation, kDefaultElevation));
        state.yaw_sin = std::sin(yaw);
        state.yaw_cos = std::cos(yaw);
    }
    g_state = state;
}

// Post-hook on dScnKy_env_light_c::setSunpos(). The game has just placed both bodies on the
// vanilla orbit; retilt them before anything reads the result (drawKankyo calls setSunpos ->
// SetBaseLight -> setLight, so the base light picks up the new position the same frame).
void on_setsunpos_post(ModContext*, void* args, void*, void*) {
    const CelestialOrbitState state = g_state;
    if (state.active == 0u) {
        return;  // switched off — bit-exact vanilla, we never touch the positions
    }

    dScnKy_env_light_c* envLight = mods::arg<dScnKy_env_light_c*>(args, 0);
    if (envLight == nullptr) {
        return;
    }

    // Mirror setSunpos's own guard. On these frames the game writes nothing, so there is no fresh
    // vanilla offset to work from (and the eye it would be measured against has moved on).
    camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return;
    }
    const char* startStage = dComIfGp_getStartStageName();
    if (startStage != nullptr && std::strcmp(startStage, "F_SP200") == 0) {
        return;
    }
    if (g_haveLastPos && same_pos(envLight->sun_pos, g_lastSunPos) &&
        same_pos(envLight->moon_pos, g_lastMoonPos))
    {
        return;  // nothing was written this frame
    }

    // sun_pos is absolute (eye + offset); moon_pos is the bare offset, added to the eye by its
    // consumers. Both carry the same vanilla orbit, so both go through the same transform.
    const cXyz& eye = camera->view.lookat.eye;
    float x = envLight->sun_pos.x - eye.x;
    float y = envLight->sun_pos.y - eye.y;
    float z = envLight->sun_pos.z - eye.z;
    celestial_orbit_apply_offset(state.sun_z_ratio, state.yaw_sin, state.yaw_cos, &x, &y, &z);
    envLight->sun_pos.x = eye.x + x;
    envLight->sun_pos.y = eye.y + y;
    envLight->sun_pos.z = eye.z + z;

    x = envLight->moon_pos.x;
    y = envLight->moon_pos.y;
    z = envLight->moon_pos.z;
    celestial_orbit_apply_offset(state.moon_z_ratio, state.yaw_sin, state.yaw_cos, &x, &y, &z);
    envLight->moon_pos.x = x;
    envLight->moon_pos.y = y;
    envLight->moon_pos.z = z;

    g_lastSunPos = envLight->sun_pos;
    g_lastMoonPos = envLight->moon_pos;
    g_haveLastPos = true;
}

// ---------------------------------------------------------------------------------------------
// Exported service — see include/celestial_orbit_service.h. Kept at global-ish scope so
// EXPORT_SERVICE's generated meta record is a simple token.
// ---------------------------------------------------------------------------------------------
ModResult get_state(ModContext*, CelestialOrbitState* out) {
    if (out == nullptr || out->struct_size < sizeof(CelestialOrbitState)) {
        return MOD_INVALID_ARGUMENT;
    }
    const CelestialOrbitState state = g_state;
    out->active = state.active;
    out->sun_z_ratio = state.sun_z_ratio;
    out->moon_z_ratio = state.moon_z_ratio;
    out->yaw_sin = state.yaw_sin;
    out->yaw_cos = state.yaw_cos;
    return MOD_OK;
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------
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

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Sun and Moon Trajectory");
    add_toggle(panel, "Enabled", g_cvarEnabled,
        "Retilts the path the sun and moon travel across the sky. Switched off, the game's own "
        "positions are left completely untouched (bit-exact vanilla), which also tells Realtime "
        "Sun Shadows to go back to the vanilla light direction.");
    add_number(panel, "Sun Peak Elevation", g_cvarSunElevation, kMinElevation, kMaxElevation, 1,
        " deg",
        "How high the sun climbs at noon. Vanilla is 59 degrees, which is what keeps midday "
        "shadows long and slanted all day; raising it gives realtime shadows a proper overhead "
        "sun. Sunrise and sunset elevations barely move, so those transitions still look the "
        "same, and the time of day is not touched at all.<br/>Capped at 80 degrees on purpose: at "
        "90 the sun passes straight through the zenith and shadows snap around as its direction "
        "flips at noon.");
    add_number(panel, "Moon Peak Elevation", g_cvarMoonElevation, kMinElevation, kMaxElevation, 1,
        " deg",
        "The same knob for the moon, which is the light that casts night shadows. Set it to match "
        "the sun unless you deliberately want the night sky to read differently.");
    add_number(panel, "Orbit Yaw", g_cvarYaw, -180, 180, 5, " deg",
        "Spins the whole path about the vertical axis, moving where the sun and moon rise and set "
        "without changing how high they get. 0 is vanilla. The painted sky and cloud art do not "
        "rotate with it, so large values can put the sun somewhere the backdrop does not expect.");
    return MOD_OK;
}

ModResult register_int(const char* name, int64_t def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register option");
    }
    return MOD_OK;
}

}  // namespace

constexpr CelestialOrbitService g_orbitService{
    .header = SERVICE_HEADER(
        CelestialOrbitService, CELESTIAL_ORBIT_SERVICE_MAJOR, CELESTIAL_ORBIT_SERVICE_MINOR),
    .get_state = get_state,
};
EXPORT_SERVICE(g_orbitService);

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    // DEFAULT: on, with both bodies peaking at 75 degrees (well clear of the 80-degree ceiling)
    // and no yaw. Untick Enabled for bit-exact vanilla.
    //
    // The config key is "orbitEnabled", NOT "enabled". **"enabled" is RESERVED BY THE HOST** for
    // every mod: the loader gives each discovered mod a `mod.<escaped id>.enabled` bool for the mod
    // manager's own on/off checkbox (`mod_enabled_cvar_name`, loader.cpp), created at discovery,
    // before any mod initializes. A mod var named "enabled" formats to the identical key, so
    // register_var returns MOD_CONFLICT and the mod dies at its first registration. That is what
    // kept this mod from loading at all. Ours is a different question anyway - the manager's
    // checkbox unloads the mod outright, while this one keeps it loaded (and its service exported,
    // reporting vanilla to Realtime Sun Shadows) and only stops it writing.
    ConfigVarDesc enabledDesc = CONFIG_VAR_DESC_INIT;
    enabledDesc.name = "orbitEnabled";
    enabledDesc.type = CONFIG_VAR_BOOL;
    enabledDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &enabledDesc, &g_cvarEnabled) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register orbitEnabled option");
    }

    ModResult result = register_int("sunElevation", kDefaultElevation, g_cvarSunElevation, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int("moonElevation", kDefaultElevation, g_cvarMoonElevation, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int("orbitYaw", 0, g_cvarYaw, error);
    if (result != MOD_OK) {
        return result;
    }

    if (mods::hook_add_post<SetSunpos>(svc_hook, on_setsunpos_post) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook setSunpos");
    }

    refresh_state();

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "celestial_orbit ready (sun/moon trajectory)");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    refresh_state();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarEnabled = 0;
    g_cvarSunElevation = 0;
    g_cvarMoonElevation = 0;
    g_cvarYaw = 0;
    g_state = CELESTIAL_ORBIT_STATE_INIT;
    g_haveLastPos = false;
    return MOD_OK;
}
}
