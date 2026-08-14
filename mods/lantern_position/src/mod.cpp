// Lantern Position — moves where Link's lantern sits on his belt while it is lit but not in his
// hand (sword drawn, bow out, empty-handed).
//
// The lantern is NOT parented to a bone in the model file. daAlink_c::setItemMatrix() rebuilds
// mpKanteraModel's base matrix from one of Link's animated joint matrices every frame, choosing
// between two placements (d_a_alink.cpp, the FLG2_UNK_1 block):
//
//     in hand   joint mLeftItemJntNo (10)  trans(-2.0, -0.1, -0.7)  rot(100.0, 9.3, 183.0)
//     on belt   joint 0x10 (16)            trans(-1.0,  4.5,  9.0)  rot(-75.0, 62.0, 89.0)
//
// mEquipItem picks the branch: it is the lantern (or the oil bottle being poured into it) while
// held, anything else while stowed. This mod post-hooks setItemMatrix and rewrites the STOWED
// placement only, from config: joint index, a local translation, an XYZ rotation and a scale.
// The held placement, and every other item Link carries, are left alone.
//
// Everything downstream follows the base matrix for free. Joint 1 of the lantern model carries
// daAlink_kandelaarModelCallBack, which derives mKandelaarFlamePos from wherever the model ended
// up; that single position drives the glow billboard, the lantern's light and shadow mode
// (d_kankyo.cpp), the real-shadow entry, and the insect actors that react to lantern light. So
// there is nothing to move but the one matrix.
//
// The one subtlety is that the flame position is a damped spring with per-frame state
// (mKandelaarFlamePos + field_0x3618/3624/3630), advanced inside the joint callback that
// modelCalc() runs. The vanilla call has already advanced it once against the vanilla matrix by
// the time our post-hook gets control, so a second modelCalc() would advance it twice a frame and
// stiffen the swing. The pre-hook snapshots those four vectors and the post-hook restores them
// before recalculating, leaving exactly one advance per frame — against our matrix.
//
// Defaults are the vanilla numbers on purpose: switched on, out of the box, this mod reproduces
// the stock belt placement exactly. That is the check that the re-apply path is sound before any
// slider is touched.
//
// Game-linked: hooks a game function and drives game models, so it is coupled to the pinned build.

#include "global.h"

#include "JSystem/J3DGraphAnimator/J3DModel.h"      // setBaseTRMtx, getAnmMtx, getModelData
#include "JSystem/J3DGraphAnimator/J3DModelData.h"  // getJointNum, getJointName
#include "JSystem/JUtility/JUTNameTab.h"            // JUTNameTab::getName (joint dump)
#include "SSystem/SComponent/c_math.h"              // cM_deg2s
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"  // dItemNo_KANTERA_e
#include "m_Do/m_Do_mtx.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

namespace {

// Hook target (emits a modmeta hook record the host resolves at load). Arg 0 is `this`, arg 1 is
// the int parameter. setItemMatrix runs once per frame from Link's matrix/draw paths.
DEFINE_HOOK(&daAlink_c::setItemMatrix, SetItemMatrix);

// Vanilla stowed placement, in the units the config vars use (offsets in tenths of a world unit,
// rotations in whole degrees). Chosen as the defaults so an untouched install is bit-identical to
// the stock game.
constexpr int kVanillaJoint = 16;  // 0x10 — hip/waist height on Link's human skeleton
constexpr int kVanillaOffsetX = -10;
constexpr int kVanillaOffsetY = 45;
constexpr int kVanillaOffsetZ = 90;
constexpr int kVanillaRotX = -75;
constexpr int kVanillaRotY = 62;
constexpr int kVanillaRotZ = 89;
constexpr int kVanillaScale = 100;

// Offsets are in tenths so the vanilla 4.5 is representable; the UI steps 5 at a time (0.5 units),
// which keeps every click on the vanilla lattice.
constexpr int kOffsetLimit = 600;  // +/- 60 world units — Link is roughly 150 tall
constexpr int kOffsetStep = 5;

// Link's model has well under this many joints; the real bound is getJointNum() at apply time.
constexpr int kMaxJointIndex = 63;

ConfigVarHandle g_cvarBeltEnabled = 0;  // DEFAULTs set in mod_initialize
ConfigVarHandle g_cvarJoint = 0;
ConfigVarHandle g_cvarOffsetX = 0;
ConfigVarHandle g_cvarOffsetY = 0;
ConfigVarHandle g_cvarOffsetZ = 0;
ConfigVarHandle g_cvarRotX = 0;
ConfigVarHandle g_cvarRotY = 0;
ConfigVarHandle g_cvarRotZ = 0;
ConfigVarHandle g_cvarScale = 0;

// Resolved once per frame on the game thread (refresh_state) and read by the hook. Starts
// inactive, so any frame before the first mod_update behaves exactly like the unmodded game.
struct BeltState {
    bool active;
    int joint;
    f32 x, y, z;     // world units, in the attach joint's local space
    f32 rx, ry, rz;  // degrees, applied X then Y then Z
    f32 scale;
};

BeltState g_state = {false, kVanillaJoint, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

// The flame spring's state, saved by the pre-hook so the post-hook can rewind the vanilla call's
// advance and redo it against our matrix. See the header comment.
struct FlameSpring {
    cXyz pos;
    cXyz prev;
    cXyz prev2;
    cXyz vel;
};

FlameSpring g_savedFlame;
bool g_haveSavedFlame = false;

// Set from the UI button, consumed by the post-hook (which is where a valid daAlink_c* exists).
bool g_dumpJoints = false;

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

// Game thread, once per frame (mod_update) and once at init.
void refresh_state() {
    BeltState state = {false, kVanillaJoint, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (get_bool_option(g_cvarBeltEnabled, true)) {
        state.active = true;
        state.joint = std::clamp(get_int_option(g_cvarJoint, kVanillaJoint), 0, kMaxJointIndex);
        state.x = static_cast<f32>(std::clamp(get_int_option(g_cvarOffsetX, kVanillaOffsetX),
                      -kOffsetLimit, kOffsetLimit)) * 0.1f;
        state.y = static_cast<f32>(std::clamp(get_int_option(g_cvarOffsetY, kVanillaOffsetY),
                      -kOffsetLimit, kOffsetLimit)) * 0.1f;
        state.z = static_cast<f32>(std::clamp(get_int_option(g_cvarOffsetZ, kVanillaOffsetZ),
                      -kOffsetLimit, kOffsetLimit)) * 0.1f;
        state.rx = static_cast<f32>(std::clamp(get_int_option(g_cvarRotX, kVanillaRotX), -180, 180));
        state.ry = static_cast<f32>(std::clamp(get_int_option(g_cvarRotY, kVanillaRotY), -180, 180));
        state.rz = static_cast<f32>(std::clamp(get_int_option(g_cvarRotZ, kVanillaRotZ), -180, 180));
        state.scale =
            static_cast<f32>(std::clamp(get_int_option(g_cvarScale, kVanillaScale), 10, 400)) *
            0.01f;
    }
    g_state = state;
}

// True when setItemMatrix's STOWED branch is the one that ran this call, so rewriting the lantern
// matrix is ours to do. Mirrors the vanilla conditions exactly (d_a_alink.cpp): the lantern is
// engaged, nothing external has claimed the matrix this frame, we are not in the treasure-open or
// get-item poses that place it in world space, and the equipped item is not the lantern itself
// (in hand) or the oil bottle being poured into it (also in hand).
bool stowed_placement_runs(daAlink_c* link, const BeltState& state) {
    if (link == nullptr || link->mpLinkModel == nullptr || link->mpKanteraModel == nullptr ||
        link->mpKanteraGlowModel == nullptr)
    {
        return false;
    }
    // Wolf form has its own skeleton and no lantern of its own; leave it entirely alone.
    if (link->checkWolf()) {
        return false;
    }
    if (!link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1)) {
        return false;
    }
    // ERFLG1_UNK_4 means someone called setKandelaarMtx this frame (Coro's shop, d_a_npc_ks) and
    // owns the matrix; vanilla skips its own placement then, and so do we.
    if (link->checkEndResetFlg1(daPy_py_c::ERFLG1_UNK_4)) {
        return false;
    }
    if (link->mProcID == daAlink_c::PROC_OPEN_TREASURE) {
        return false;
    }
    if (link->mProcID == daAlink_c::PROC_GET_ITEM && link->mProcVar4.field_0x3010 != 0) {
        return false;
    }
    if (link->mEquipItem == dItemNo_KANTERA_e || link->checkOilBottleItemNotGet(link->mEquipItem)) {
        return false;  // in hand
    }
    const J3DModelData* modelData = link->mpLinkModel->getModelData();
    if (modelData == nullptr || state.joint >= static_cast<int>(modelData->getJointNum())) {
        return false;  // a joint the model does not have — stay on vanilla rather than misplace it
    }
    return true;
}

// Rebuild the lantern's base matrix the way setItemMatrix does, from our numbers, and redo the
// two model calcs that depend on it. Called only when stowed_placement_runs() said yes.
void apply(daAlink_c* link, const BeltState& state) {
    mDoMtx_stack_c::copy(link->mpLinkModel->getAnmMtx(state.joint));
    mDoMtx_stack_c::transM(state.x, state.y, state.z);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(state.rx), cM_deg2s(state.ry), cM_deg2s(state.rz));
    if (state.scale != 1.0f) {
        mDoMtx_stack_c::scaleM(state.scale, state.scale, state.scale);
    }
    link->mpKanteraModel->setBaseTRMtx(mDoMtx_stack_c::get());

    // Recalculating is what actually moves the drawn model: J3D draws from the anim matrices this
    // builds, not from the base matrix. It also re-runs the joint-1 callback, which is what puts
    // mKandelaarFlamePos (and with it the glow, the light and the shadow) in the right place.
    link->modelCalc(link->mpKanteraModel);

    mDoMtx_stack_c::transS(link->mKandelaarFlamePos);
    link->mpKanteraGlowModel->setBaseTRMtx(mDoMtx_stack_c::get());
    link->modelCalc(link->mpKanteraGlowModel);
}

// One line per joint of whichever model Link is currently wearing, so the attach-joint number can
// be read off rather than guessed. The decomp never named Link's joints (other humanoids get a
// JNT_ enum; Link does not), but the model file carries the original names.
void dump_joint_names(daAlink_c* link) {
    if (link->mpLinkModel == nullptr) {
        svc_log->warn(mod_ctx, "joint dump: no player model yet");
        return;
    }
    J3DModelData* modelData = link->mpLinkModel->getModelData();
    if (modelData == nullptr) {
        svc_log->warn(mod_ctx, "joint dump: player model has no model data");
        return;
    }

    const u16 jointCount = modelData->getJointNum();
    JUTNameTab* names = modelData->getJointName();

    char line[192];
    std::snprintf(line, sizeof(line), "Link joint names (%s form, %u joints)%s:",
        link->checkWolf() ? "wolf" : "human", static_cast<unsigned>(jointCount),
        names == nullptr ? " - model has no name table" : "");
    svc_log->info(mod_ctx, line);

    for (u16 i = 0; i < jointCount; ++i) {
        const char* name = (names != nullptr) ? names->getName(i) : nullptr;
        std::snprintf(line, sizeof(line), "  joint %2u (0x%02X): %s", static_cast<unsigned>(i),
            static_cast<unsigned>(i), (name != nullptr) ? name : "(unnamed)");
        svc_log->info(mod_ctx, line);
    }
}

// ---------------------------------------------------------------------------------------------
// Hook callbacks — both on the game thread, inside setItemMatrix's own call.
// ---------------------------------------------------------------------------------------------
HookAction on_set_item_matrix_pre(ModContext*, void* args, void*, void*) {
    g_haveSavedFlame = false;

    const BeltState state = g_state;
    if (!state.active) {
        return HOOK_CONTINUE;
    }

    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (!stowed_placement_runs(link, state)) {
        return HOOK_CONTINUE;
    }

    // Snapshot the flame spring so the post-hook can undo the advance the vanilla call is about to
    // make against the vanilla matrix. Without this the spring steps twice a frame.
    g_savedFlame.pos = link->mKandelaarFlamePos;
    g_savedFlame.prev = link->field_0x3624;
    g_savedFlame.prev2 = link->field_0x3630;
    g_savedFlame.vel = link->field_0x3618;
    g_haveSavedFlame = true;
    return HOOK_CONTINUE;
}

void on_set_item_matrix_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    if (g_dumpJoints && link != nullptr) {
        g_dumpJoints = false;
        dump_joint_names(link);
    }

    if (!g_haveSavedFlame) {
        return;  // not our frame: held in hand, wolf, externally placed, or switched off
    }
    g_haveSavedFlame = false;
    if (link == nullptr) {
        return;
    }

    link->mKandelaarFlamePos = g_savedFlame.pos;
    link->field_0x3624 = g_savedFlame.prev;
    link->field_0x3630 = g_savedFlame.prev2;
    link->field_0x3618 = g_savedFlame.vel;

    apply(link, g_state);
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

void on_reset_vanilla(ModContext*, void*) {
    svc_config->set_int(mod_ctx, g_cvarJoint, kVanillaJoint);
    svc_config->set_int(mod_ctx, g_cvarOffsetX, kVanillaOffsetX);
    svc_config->set_int(mod_ctx, g_cvarOffsetY, kVanillaOffsetY);
    svc_config->set_int(mod_ctx, g_cvarOffsetZ, kVanillaOffsetZ);
    svc_config->set_int(mod_ctx, g_cvarRotX, kVanillaRotX);
    svc_config->set_int(mod_ctx, g_cvarRotY, kVanillaRotY);
    svc_config->set_int(mod_ctx, g_cvarRotZ, kVanillaRotZ);
    svc_config->set_int(mod_ctx, g_cvarScale, kVanillaScale);
    // No refresh_state() here: the next mod_update picks the values up on the game thread, so the
    // hook never reads a state this callback is halfway through writing.
}

void on_dump_joints(ModContext*, void*) {
    g_dumpJoints = true;
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Lantern on the Belt");
    add_toggle(panel, "Enabled", g_cvarBeltEnabled,
        "Moves the lit lantern where it hangs on Link's belt - the placement you see whenever the "
        "lantern is on but something else is in his hands (sword drawn, bow out, empty-handed). "
        "The in-hand placement is never touched. Switched off, the game's own placement runs "
        "untouched. The defaults below ARE the game's own numbers, so a fresh install looks "
        "exactly like vanilla until you change one.");
    add_number(panel, "Attach Joint", g_cvarJoint, 0, kMaxJointIndex, 1, nullptr,
        "Which of Link's animated joints the lantern hangs from. 16 is the game's choice - "
        "hip/waist height, so the lantern swings with his hips. 5 is the joint his sheath, "
        "back-slung sword and shield use, which puts the lantern up on his back; 10 is his left "
        "hand's item point. Use Dump Joint Names to see the model's own name for every joint, "
        "then set the number here. An index the model does not have leaves the lantern where the "
        "game put it.");
    add_number(panel, "Offset X", g_cvarOffsetX, -kOffsetLimit, kOffsetLimit, kOffsetStep,
        " (0.1 units)",
        "Position along the attach joint's own X axis, in tenths of a world unit (Link is roughly "
        "1500 tall in these units, so 10 = 1 unit is a small nudge and 100 = 10 units is a "
        "visible shift). The axes belong to the joint and rotate with it as he animates, so which "
        "way each one points depends on the joint you picked - nudge one and look. Vanilla is "
        "-10, 45, 90.");
    add_number(panel, "Offset Y", g_cvarOffsetY, -kOffsetLimit, kOffsetLimit, kOffsetStep,
        " (0.1 units)",
        "Position along the attach joint's own Y axis, in tenths of a world unit. See Offset X.");
    add_number(panel, "Offset Z", g_cvarOffsetZ, -kOffsetLimit, kOffsetLimit, kOffsetStep,
        " (0.1 units)",
        "Position along the attach joint's own Z axis, in tenths of a world unit. See Offset X.");
    add_number(panel, "Rotation X", g_cvarRotX, -180, 180, 1, " deg",
        "How the lantern hangs, as a rotation about the joint's X axis. Applied X, then Y, then "
        "Z, matching the game's own order. Vanilla is -75, 62, 89 - the lantern model does not "
        "hang straight down at zero, so start from the defaults and adjust rather than zeroing "
        "them.");
    add_number(panel, "Rotation Y", g_cvarRotY, -180, 180, 1, " deg",
        "Rotation about the joint's Y axis. See Rotation X.");
    add_number(panel, "Rotation Z", g_cvarRotZ, -180, 180, 1, " deg",
        "Rotation about the joint's Z axis. See Rotation X.");
    add_number(panel, "Scale", g_cvarScale, 10, 400, 5, "%",
        "Size of the stowed lantern. 100 is the game's size. The flame, its glow and the light it "
        "casts scale with it, since they are all measured from the same matrix.");

    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Reset to Vanilla";
    control.on_pressed = on_reset_vanilla;
    add_control(panel, control);

    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Dump Joint Names";
    control.on_pressed = on_dump_joints;
    add_control(panel, control);
    svc_ui->pane_add_text(mod_ctx, panel,
        "Dump Joint Names prints every joint of Link's current model, with its index and the "
        "name the model file gives it, to the log. Press it in human form.",
        nullptr);
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

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    // NOT "enabled": that name fragment is reserved by the loader, which owns the per-mod
    // enable/disable toggle and persists it under the same key. register_var rejects it and the
    // whole mod fails to load. See ConfigVarDesc in mods/svc/config.h.
    ConfigVarDesc enabledDesc = CONFIG_VAR_DESC_INIT;
    enabledDesc.name = "beltEnabled";
    enabledDesc.type = CONFIG_VAR_BOOL;
    enabledDesc.default_bool = true;
    if (svc_config->register_var(mod_ctx, &enabledDesc, &g_cvarBeltEnabled) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register beltEnabled option");
    }

    // DEFAULT: the game's own stowed placement, so an untouched install matches vanilla.
    struct {
        const char* name;
        int64_t def;
        ConfigVarHandle* out;
    } const vars[] = {
        {"beltJoint", kVanillaJoint, &g_cvarJoint},
        {"beltOffsetX", kVanillaOffsetX, &g_cvarOffsetX},
        {"beltOffsetY", kVanillaOffsetY, &g_cvarOffsetY},
        {"beltOffsetZ", kVanillaOffsetZ, &g_cvarOffsetZ},
        {"beltRotX", kVanillaRotX, &g_cvarRotX},
        {"beltRotY", kVanillaRotY, &g_cvarRotY},
        {"beltRotZ", kVanillaRotZ, &g_cvarRotZ},
        {"beltScale", kVanillaScale, &g_cvarScale},
    };
    for (const auto& var : vars) {
        const ModResult result = register_int(var.name, var.def, *var.out, error);
        if (result != MOD_OK) {
            return result;
        }
    }

    // The pre-hook only snapshots the flame spring; the post-hook does the work. Both are needed:
    // see the header comment on double-advancing the spring.
    if (mods::hook::add_pre<SetItemMatrix>(svc_hook, on_set_item_matrix_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook setItemMatrix (pre)");
    }
    if (mods::hook::add_post<SetItemMatrix>(svc_hook, on_set_item_matrix_post) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook setItemMatrix (post)");
    }

    refresh_state();

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "lantern_position ready (stowed lantern placement)");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    refresh_state();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_cvarBeltEnabled = 0;
    g_cvarJoint = 0;
    g_cvarOffsetX = 0;
    g_cvarOffsetY = 0;
    g_cvarOffsetZ = 0;
    g_cvarRotX = 0;
    g_cvarRotY = 0;
    g_cvarRotZ = 0;
    g_cvarScale = 0;
    g_state.active = false;
    g_haveSavedFlame = false;
    g_dumpJoints = false;
    return MOD_OK;
}
}
