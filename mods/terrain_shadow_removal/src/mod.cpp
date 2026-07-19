// Terrain Shadow Removal — removes Twilight Princess's animated-texture ground shadow.
//
// Beyond the "moya" particle shadows (handled by projected_shadow_removal), TP fakes some ground
// shade by scrolling a SHADOW TEXTURE across the terrain itself: the room's terrain model
// (loaded by d_a_bg) carries a `model.btk` texture-SRT animation that drifts a shadow/dapple
// overlay over the ground. The forest-floor shade in Faron that slowly sways is this — it is not
// moya (the moya count reads 0 there), it is the terrain's own animated material.
//
// The terrain material is base-ground + shadow-overlay combined by the hardware texture stages.
// This mod skips drawing the animated overlay so the realtime stack can shade the ground for
// real. Two cases (see the mod description):
//   - if the overlay is its OWN material/shape, skipping it removes the shadow cleanly;
//   - if it is a second stage inside the base ground material, skipping the shape takes the
//     ground with it (the ground looks wrong) — that is why this is EXPERIMENTAL and off by
//     default, tested per area.
//
// HOW IT TARGETS ONLY THE TERRAIN SHADOW: daBg_btkAnm_c::create tells us exactly which materials
// the room BTK animates. We record those material pointers; at draw time we skip a shape only if
// its material is in that recorded set. So the membership test IS the scope — characters, and
// water/waterfalls (which are separate actors with their own object-archive BTKs, verified in
// d_a_obj_waterfall / d_a_obj_lv3Water), can never match. The getMaterialAnm() pre-check means we
// only ever look up materials that are actually animated, keeping the per-shape cost negligible
// and guarding against a stale (freed then reused) pointer that is no longer an animated material.
//
// Game-linked: hooks game functions, coupled to the pinned game build.

#include "global.h"

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"    // J3DAnmTextureSRTKey
#include "JSystem/J3DGraphAnimator/J3DModelData.h"    // J3DModelData::getMaterialNodePointer
#include "JSystem/J3DGraphBase/J3DMaterial.h"         // J3DMaterial::getMaterialAnm
#include "JSystem/J3DGraphBase/J3DShape.h"            // J3DShape::getMaterial
#include "d/actor/d_a_bg.h"                           // daBg_btkAnm_c
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <cstdio>
#include <unordered_set>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

// The room terrain's BTK setup (records which materials it animates), and the per-shape draw.
DEFINE_HOOK(&daBg_btkAnm_c::create, BtkCreate);
DEFINE_HOOK(&J3DShape::drawFast, ShapeDrawFast);

namespace {

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarLog = 0;

// Materials the room terrain's BTK animates (the drifting shadow overlays). Accumulates across
// rooms; a stale (freed) pointer is harmless because the draw hook also checks the material is
// still animated (getMaterialAnm() != NULL) before skipping it.
std::unordered_set<const void*> g_animatedTerrainMaterials;

// Cached once per frame from config so the per-shape draw hook doesn't hit the config service
// thousands of times a frame.
bool g_enabledCached = false;
uint32_t g_capturedTotal = 0;
uint32_t g_lastLoggedTotal = 0;

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

// Pre-hook on daBg_btkAnm_c::create(J3DModelData*, J3DAnmTextureSRTKey*, int). args: 0=this,
// 1=modelData, 2=btk, 3=anmPlay. Record the materials this BTK animates — the terrain shadow
// overlays — so the draw hook can skip exactly them.
HookAction on_btk_create_pre(ModContext*, void* args, void*, void*) {
    J3DModelData* modelData = mods::arg<J3DModelData*>(args, 1);
    J3DAnmTextureSRTKey* btk = mods::arg<J3DAnmTextureSRTKey*>(args, 2);
    if (modelData == nullptr || btk == nullptr) {
        return HOOK_CONTINUE;
    }
    const u16 count = btk->getUpdateMaterialNum();
    for (u16 i = 0; i < count; ++i) {
        const u16 id = btk->getUpdateMaterialID(i);
        if (id == 0xFFFFu) {
            continue;
        }
        J3DMaterial* material = modelData->getMaterialNodePointer(id);
        if (material != nullptr && g_animatedTerrainMaterials.insert(material).second) {
            ++g_capturedTotal;
        }
    }
    return HOOK_CONTINUE;
}

// Pre-hook on J3DShape::drawFast. Cancel the draw of a shape whose material is a recorded terrain
// shadow overlay. The getMaterialAnm() guard means only animated materials are ever looked up
// (cheap), and only terrain materials the BTK animates are in the set — nothing else is touched.
HookAction on_shape_draw_pre(ModContext*, void* args, void*, void*) {
    if (!g_enabledCached) {
        return HOOK_CONTINUE;
    }
    const J3DShape* shape = mods::arg<const J3DShape*>(args, 0);
    J3DMaterial* material = shape != nullptr ? shape->getMaterial() : nullptr;
    if (material == nullptr || material->getMaterialAnm() == nullptr) {
        return HOOK_CONTINUE;
    }
    if (g_animatedTerrainMaterials.count(material) != 0) {
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

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    add_toggle(panel, "Remove Terrain Shadow", g_cvarEnabled,
        "Skips drawing the room terrain's animated shadow-overlay material (the drifting "
        "dappled shade on forest floors). EXPERIMENTAL: if a room's shadow shares the base "
        "ground material, this can make the ground itself look wrong there - test per area and "
        "turn it off if a floor breaks. Waterfalls and water are unaffected.");
    add_toggle(panel, "Log Captured Count", g_cvarLog,
        "Logs how many terrain shadow-overlay materials have been captured as areas load. If a "
        "swaying terrain shadow persists but this count never rises in that area, the shadow "
        "isn't a texture-scroll overlay and this mod can't reach it.");
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
    // Off by default: this one can visibly break a floor if the shadow shares the ground
    // material, so it is opt-in.
    ModResult result = register_bool("effectEnabled", false, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool("logCount", false, g_cvarLog, error);
    if (result != MOD_OK) {
        return result;
    }

    if (mods::hook_add_pre<BtkCreate>(svc_hook, on_btk_create_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook the terrain BTK setup");
    }
    if (mods::hook_add_pre<ShapeDrawFast>(svc_hook, on_shape_draw_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook J3DShape::drawFast");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "terrain_shadow_removal ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    g_enabledCached = get_bool_option(g_cvarEnabled, false);
    if (get_bool_option(g_cvarLog, false) && g_capturedTotal != g_lastLoggedTotal) {
        g_lastLoggedTotal = g_capturedTotal;
        char msg[80];
        std::snprintf(msg, sizeof(msg), "terrain shadow overlays captured: %u", g_capturedTotal);
        svc_log->info(mod_ctx, msg);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarEnabled = 0;
    g_cvarLog = 0;
    g_animatedTerrainMaterials.clear();
    g_enabledCached = false;
    g_capturedTotal = 0;
    g_lastLoggedTotal = 0;
    return MOD_OK;
}
}
