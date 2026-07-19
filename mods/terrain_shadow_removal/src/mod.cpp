// Terrain Shadow Removal — removes Twilight Princess's animated ground-shadow OVERLAY (the
// slowly drifting dappled shade on forest/field floors that is NOT the "moya" particle system).
//
// THE MECHANISM (found in the game source, not guessed):
// TP darkens the ground with a second texture stage baked into the terrain material itself. The
// room's terrain materials are named "??MAcc.." and the environment code drives them every frame:
//   - dKy_cloudshadow_scroll() scrolls TEXTURE MATRIX 1 of the MA00/MA01/MA16 materials by the
//     drifting cloud ("vrkumo") packet translation  -> this is the SWAY,
//   - dKy_bg_MAxx_proc() then sets TEV KColor register 1's RED channel to the environment fog
//     density on the MA00/MA01/MA04/MA16 materials  -> this is the shadow STRENGTH.
// The shadow TEV stage multiplies the scrolled shadow texture into the base ground by that KColor1
// strength. Set the fog-density red channel to 0 and the overlay stage contributes nothing, while
// the base ground texture (stage 0) is untouched.
//
// WHY THIS (and not the previous shape-skip): the shadow is a stage INSIDE the ground material, so
// skipping the whole shape holed the ground. Zeroing the overlay's strength register removes only
// the shadow and leaves the ground intact — no holes.
//
// HOW IT TARGETS ONLY THE SHADOW: it acts solely on materials whose name matches the cloud-shadow
// codes (MA00/MA01/MA04/MA16), and only rewrites TEV KColor 1 — the register the game itself uses
// for the shadow strength on exactly these materials. It runs as a post-hook on dKy_bg_MAxx_proc,
// which the game calls right after it sets that register, so our value is the one that draws.
//
// SAME SYSTEM, DIFFERENT AREAS: this overlay appears in several areas (a forest floor, a field
// ground scroll, etc.). Because the material code is the discriminator, removal is offered per
// code with a live logger, so an area whose overlay you want kept can leave that code enabled.
// The big rolling Hyrule-Field cloud shadows are the *moya* system instead — those are untouched
// here (see the projected_shadow_removal mod).
//
// Game-linked: hooks a game function, coupled to the pinned game build.

#include "global.h"

#include "JSystem/J3DGraphAnimator/J3DModel.h"         // J3DModel::getModelData
#include "JSystem/J3DGraphAnimator/J3DModelData.h"     // material table accessors
#include "JSystem/J3DGraphBase/J3DMaterial.h"          // J3DMaterial::setTevKColor
#include "JSystem/J3DGraphBase/J3DMatBlock.h"          // J3DGXColor
#include "JSystem/JUtility/JUTNameTab.h"               // JUTNameTab::getName
#include "d/d_kankyo.h"                                // dKy_bg_MAxx_proc
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

// The per-frame terrain-material environment setup (sets the shadow-strength KColor). Free function
// declared in d/d_kankyo.h. We POST-hook it so our KColor override is the final value before draw.
DEFINE_HOOK(dKy_bg_MAxx_proc, MAxxProc);

namespace {

// The cloud-shadow-overlay material codes. name[3..6] == "MAcc". MA00/MA01/MA16 get the scrolling
// (swaying) overlay; MA04 is the same shadow family without the scroll. Each is independently
// toggleable so an overlay you want kept (in some other area) can stay.
struct ShadowCode {
    char c5;
    char c6;
    const char* label;
    const char* help;
};

constexpr ShadowCode kCodes[] = {
    {'0', '0', "Remove MA00 (scrolling shade)",
     "The main drifting ground-shadow overlay (texture-matrix scrolled by the cloud packet). This "
     "is the usual swaying forest-floor / field-floor shade. Removing it zeroes the overlay's "
     "strength; the base ground stays."},
    {'0', '1', "Remove MA01 (scrolling shade, alt)",
     "A second scrolling ground-shadow overlay variant. Some rooms use MA01 instead of / alongside "
     "MA00 for the swaying floor shade."},
    {'1', '6', "Remove MA16 (scrolling shade, alt)",
     "Another scrolling ground-shadow overlay variant used by some rooms."},
    {'0', '4', "Remove MA04 (static shade)",
     "The same shadow-overlay family but without the scroll (a static ground darkening). Off by "
     "default because it does not sway; enable it if a still ground shade remains."},
};
constexpr int kCodeCount = static_cast<int>(sizeof(kCodes) / sizeof(kCodes[0]));

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarLog = 0;
ConfigVarHandle g_cvarRemove[kCodeCount] = {};

// Cached once per frame (mod_update, game thread) so the per-material hook doesn't hit the config
// service. g_removeMask bit i == remove code i.
bool g_enabledCached = false;
unsigned g_removeMask = 0;

// Live identification counters, updated in the hook and reported by mod_update on change. "seen"
// counts materials whose code matches ANY shadow code (whether or not its removal is enabled);
// "hit" counts materials actually neutralized this scan.
unsigned g_seenAccum = 0;
unsigned g_hitAccum = 0;
unsigned g_seenCodeAccum = 0;  // bitmask of which codes were present
unsigned g_lastLoggedSeen = 0xFFFFFFFFu;
unsigned g_lastLoggedHit = 0xFFFFFFFFu;
unsigned g_lastLoggedCodes = 0xFFFFFFFFu;

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

// Match a material name against the shadow codes. Returns the code index, or -1. The short-circuit
// && chain is safe on short names: a name shorter than 7 chars hits a '\0' before name[5]/name[6].
int shadow_code_index(const char* name) {
    if (name == nullptr || name[3] != 'M' || name[4] != 'A') {
        return -1;
    }
    for (int i = 0; i < kCodeCount; ++i) {
        if (name[5] == kCodes[i].c5 && name[6] == kCodes[i].c6) {
            return i;
        }
    }
    return -1;
}

// Post-hook on dKy_bg_MAxx_proc(void* bg_model_p). The game has just written the shadow-strength
// KColor on the terrain materials; we zero KColor register 1 on the enabled shadow-overlay
// materials so the overlay TEV stage contributes nothing (the base ground stage is untouched).
void on_maxx_post(ModContext*, void* args, void*, void*) {
    if (!g_enabledCached) {
        return;
    }
    J3DModel* model = mods::arg<J3DModel*>(args, 0);
    if (model == nullptr) {
        return;
    }
    J3DModelData* modelData = model->getModelData();
    if (modelData == nullptr) {
        return;
    }
    JUTNameTab* nametab = modelData->getMaterialName();
    if (nametab == nullptr) {
        return;
    }

    J3DGXColor zero;
    zero.r = 0;
    zero.g = 0;
    zero.b = 0;
    zero.a = 0;

    const u16 count = modelData->getMaterialNum();
    for (u16 i = 0; i < count; ++i) {
        const int code = shadow_code_index(nametab->getName(i));
        if (code < 0) {
            continue;
        }
        ++g_seenAccum;
        g_seenCodeAccum |= (1u << code);
        if ((g_removeMask & (1u << code)) == 0) {
            continue;
        }
        J3DMaterial* material = modelData->getMaterialNodePointer(i);
        if (material != nullptr) {
            // KColor register 1's red channel is the environment fog-density shadow strength for
            // these materials; zeroing it removes the overlay's contribution.
            material->setTevKColor(1, &zero);
            ++g_hitAccum;
        }
    }
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

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "General");
    add_toggle(left, "Enabled", g_cvarEnabled,
        "Master switch for the per-code removal below. Removes the animated ground-shadow overlay "
        "that TP bakes into the terrain material as a second texture stage. This is the swaying "
        "floor shade that the moya removal cannot reach (moya count reads 0 there). The base "
        "ground is left intact - only the overlay's strength is zeroed, so this does not hole the "
        "floor.");
    add_toggle(left, "Log Overlay Materials", g_cvarLog,
        "Prints, as areas load, how many cloud-shadow overlay materials are present and which "
        "codes (MA00/MA01/MA16/MA04) they use, plus how many are being removed. Use it to confirm "
        "an area's swaying shade is this overlay: if a shade persists but the seen count stays 0, "
        "it is not this system.");

    svc_ui->pane_add_section(mod_ctx, left, "Remove by Material Code");
    for (int i = 0; i < kCodeCount; ++i) {
        add_toggle(left, kCodes[i].label, g_cvarRemove[i], kCodes[i].help);
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
        svc_log->error(mod_ctx, "failed to open Terrain Shadow Removal controls window");
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
    // Off by default: this changes terrain rendering globally, so it is opt-in / experimental.
    ModResult result = register_bool("effectEnabled", false, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool("logOverlays", false, g_cvarLog, error);
    if (result != MOD_OK) {
        return result;
    }
    for (int i = 0; i < kCodeCount; ++i) {
        char name[24];
        std::snprintf(name, sizeof(name), "removeMA%c%c", kCodes[i].c5, kCodes[i].c6);
        // The three scrolling (swaying) codes default on when the mod is enabled; the static MA04
        // defaults off (it does not sway and is less likely to be an unwanted shadow).
        const bool def = !(kCodes[i].c5 == '0' && kCodes[i].c6 == '4');
        result = register_bool(name, def, g_cvarRemove[i], error);
        if (result != MOD_OK) {
            return result;
        }
    }

    if (mods::hook_add_post<MAxxProc>(svc_hook, on_maxx_post) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook dKy_bg_MAxx_proc");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "terrain_shadow_removal ready");
    return MOD_OK;
}

// Game thread, once per frame. Caches config for the hook and reports the overlay-material tally
// when logging is on (snapshotting and clearing the per-frame accumulators).
MOD_EXPORT ModResult mod_update(ModError*) {
    g_enabledCached = get_bool_option(g_cvarEnabled, false);
    unsigned mask = 0;
    for (int i = 0; i < kCodeCount; ++i) {
        if (get_bool_option(g_cvarRemove[i], false)) {
            mask |= (1u << i);
        }
    }
    g_removeMask = mask;

    const unsigned seen = g_seenAccum;
    const unsigned hit = g_hitAccum;
    const unsigned codes = g_seenCodeAccum;
    g_seenAccum = 0;
    g_hitAccum = 0;
    g_seenCodeAccum = 0;

    if (get_bool_option(g_cvarLog, false)) {
        if (seen != g_lastLoggedSeen || hit != g_lastLoggedHit || codes != g_lastLoggedCodes) {
            g_lastLoggedSeen = seen;
            g_lastLoggedHit = hit;
            g_lastLoggedCodes = codes;
            char which[32];
            int n = 0;
            for (int i = 0; i < kCodeCount; ++i) {
                if (codes & (1u << i)) {
                    n += std::snprintf(which + n, sizeof(which) - n, "%sMA%c%c",
                                       n ? "," : "", kCodes[i].c5, kCodes[i].c6);
                    if (n >= static_cast<int>(sizeof(which))) {
                        break;
                    }
                }
            }
            if (n == 0) {
                std::snprintf(which, sizeof(which), "none");
            }
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "terrain shadow overlays: seen %u (%s), removed %u", seen, which, hit);
            svc_log->info(mod_ctx, msg);
        }
    } else {
        g_lastLoggedSeen = 0xFFFFFFFFu;
        g_lastLoggedHit = 0xFFFFFFFFu;
        g_lastLoggedCodes = 0xFFFFFFFFu;
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarEnabled = 0;
    g_cvarLog = 0;
    for (auto& handle : g_cvarRemove) {
        handle = 0;
    }
    g_enabledCached = false;
    g_removeMask = 0;
    g_seenAccum = 0;
    g_hitAccum = 0;
    g_seenCodeAccum = 0;
    g_lastLoggedSeen = 0xFFFFFFFFu;
    g_lastLoggedHit = 0xFFFFFFFFu;
    g_lastLoggedCodes = 0xFFFFFFFFu;
    g_controlsWindow = 0;
    return MOD_OK;
}
}
