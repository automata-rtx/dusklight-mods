// Effect Remover — cuts down or removes built-in Twilight Princess visual features that fight new
// realtime graphical effects. It bundles three independently-toggleable removers, each targeting
// one of TP's "fake shading" systems so realtime shadows / GI can carry the look instead:
//
//   * Projected Shadow Removal — suppresses the game's "moya" projected ground shade (the kankyo
//     cloud packet: swaying forest-canopy dapple, rolling Hyrule-Field cloud shadows, drifting
//     mist), per mMoyaMode, so you keep the effects you want and drop the ones you don't.
//   * Terrain Shadow Removal — washes out the animated shadow overlay baked into the terrain
//     material itself (the MA00/MA01/MA16/MA04 cloud-shadow texture stage), per material code.
//   * Unbaked Vertex Lighting — fades out the lighting baked into geometry's vertex colors
//     (0 = flat texture-only base, 100 = vanilla), rewritten as models load.
//
// This file merges the three former standalone mods (projected_shadow_removal +
// terrain_shadow_removal + vertex_unbake) verbatim, each inside its own namespace
// (er_psr / er_tsr / er_vu) so they keep separate state; the service imports and the mod entry
// points are shared. To change a default or drop a control, edit the sub-namespace's
// init()/build_section() — see docs/self_editing_guide.md.
//
// Game-linked: hooks game functions, coupled to the pinned game build.

#include "global.h"

#include "JSystem/J3DGraphAnimator/J3DModel.h"          // J3DModel::getModelData (terrain)
#include "JSystem/J3DGraphAnimator/J3DModelData.h"      // material table + vertex data
#include "JSystem/J3DGraphBase/J3DMaterial.h"           // J3DMaterial::setTevKColor (terrain)
#include "JSystem/J3DGraphBase/J3DMatBlock.h"           // J3DGXColor (terrain)
#include "JSystem/J3DGraphLoader/J3DModelLoader.h"      // J3DModelLoaderDataBase (vertex)
#include "JSystem/JUtility/JUTNameTab.h"                // JUTNameTab::getName (terrain)
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"                                 // dKy_getEnvlight, dKy_bg_MAxx_proc
#include "d/d_kankyo_rain.h"                            // drawCloudShadow (moya)
#include "dolphin/gx/GXEnum.h"
#include "dolphin/gx/GXStruct.h"
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
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

// Shared UI helper (each sub-namespace's build_section uses it).
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

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
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

// ===========================================================================================
// SUB-FEATURE: Projected Shadow Removal   (moya projected ground shade, per mMoyaMode)
// ===========================================================================================
namespace er_psr {

DEFINE_HOOK(drawCloudShadow, CloudShadow);

constexpr int kMaxMode = 11;  // mMoyaMode > kMaxMode (incl. >= 50 shimmer/senses) is left alone

ConfigVarHandle g_cvarEnabled = 0;      // DEFAULT in init()
ConfigVarHandle g_cvarLogMode = 0;      // DEFAULT in init()
ConfigVarHandle g_cvarSuppress[kMaxMode + 1] = {};

int g_lastLoggedMode = -1000;
int g_lastLoggedCount = -1000;

UiWindowHandle g_modesWindow = 0;

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
        return HOOK_CONTINUE;  // the shimmer/senses branch — leave it
    }
    if (get_bool_option(g_cvarSuppress[mode], false)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

const char* mode_label(int mode) {
    switch (mode) {
    case 0: return "Mode 0 — default wind drift";
    case 4: return "Mode 4 — wind cloud shadows";
    case 5: return "Mode 5 — slow-sway canopy shade";
    case 11: return "Mode 11 — strong wind drift";
    default: return nullptr;
    }
}

ModResult build_modes_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;
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

void on_modes_window_closed(ModContext*, UiWindowHandle, void*) {
    g_modesWindow = 0;
}

void on_open_modes(ModContext*, void*) {
    if (g_modesWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Moya Modes";
    tabs[0].build = build_modes_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_modes_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_modesWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open moya mode toggles window");
    }
}

void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Projected Shadow Removal (moya)");
    add_toggle(panel, "Enabled", g_cvarEnabled,
        "Master switch for the per-mode moya suppression. Removes TP's fake projected ground "
        "shade (the wind/animation-driven particle field projected on the ground).");
    add_toggle(panel, "Log Active Mode", g_cvarLogMode,
        "Prints the live projected-shade mode AND count to the log whenever either changes. Walk "
        "into a spot to read off its mode; a persistent shade with count 0 is NOT this system "
        "(try Terrain Shadow Removal below).");
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Mode Toggles";
    control.on_pressed = on_open_modes;
    add_control(panel, control);
}

ModResult init(ModError* error) {
    // DEFAULT: moya suppression enabled; mode logging off.
    ModResult result = register_bool("psrEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool("psrLogMode", false, g_cvarLogMode, error);
    if (result != MOD_OK) {
        return result;
    }
    for (int mode = 0; mode <= kMaxMode; ++mode) {
        char name[24];
        std::snprintf(name, sizeof(name), "psrSuppress%d", mode);
        // DEFAULT: only the slow-sway canopy candidate (mode 5) is removed; everything else
        // (including the wind cloud shadows) is kept until you opt in.
        result = register_bool(name, mode == 5, g_cvarSuppress[mode], error);
        if (result != MOD_OK) {
            return result;
        }
    }
    if (mods::hook_add_pre<CloudShadow>(svc_hook, on_cloud_shadow_pre) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook the projected-shade draw");
    }
    return MOD_OK;
}

// Runs every frame on the game thread: report the live moya mode + count when logging is on.
void update() {
    if (!get_bool_option(g_cvarLogMode, false)) {
        g_lastLoggedMode = -1000;
        g_lastLoggedCount = -1000;
        return;
    }
    const dScnKy_env_light_c* envLight = dKy_getEnvlight();
    if (envLight == nullptr) {
        return;
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
}

void shutdown() {
    g_cvarEnabled = 0;
    g_cvarLogMode = 0;
    for (auto& handle : g_cvarSuppress) {
        handle = 0;
    }
    g_lastLoggedMode = -1000;
    g_lastLoggedCount = -1000;
    g_modesWindow = 0;
}

}  // namespace er_psr

// ===========================================================================================
// SUB-FEATURE: Terrain Shadow Removal   (animated shadow overlay baked into terrain materials)
// ===========================================================================================
namespace er_tsr {

DEFINE_HOOK(dKy_bg_MAxx_proc, MAxxProc);

struct ShadowCode {
    char c5;
    char c6;
    const char* label;
    const char* help;
};

constexpr ShadowCode kCodes[] = {
    {'0', '0', "Remove MA00 (scrolling shade)",
     "The main drifting ground-shadow overlay (texture-matrix scrolled by the cloud packet). This "
     "is the usual swaying forest-floor / field-floor shade."},
    {'0', '1', "Remove MA01 (scrolling shade, alt)",
     "A second scrolling ground-shadow overlay variant used by some rooms."},
    {'1', '6', "Remove MA16 (scrolling shade, alt)",
     "Another scrolling ground-shadow overlay variant used by some rooms."},
    {'0', '4', "Remove MA04 (forest-floor shade)",
     "The Faron/forest-floor ground shadow overlay (confirmed in-game as the slowly swaying floor "
     "shade there). Same shadow-overlay family; washed out the same way."},
};
constexpr int kCodeCount = static_cast<int>(sizeof(kCodes) / sizeof(kCodes[0]));

ConfigVarHandle g_cvarEnabled = 0;      // DEFAULT in init()
ConfigVarHandle g_cvarLog = 0;          // DEFAULT in init()
ConfigVarHandle g_cvarRemove[kCodeCount] = {};

bool g_enabledCached = false;
unsigned g_removeMask = 0;

unsigned g_seenAccum = 0;
unsigned g_hitAccum = 0;
unsigned g_seenCodeAccum = 0;
unsigned g_lastLoggedSeen = 0xFFFFFFFFu;
unsigned g_lastLoggedHit = 0xFFFFFFFFu;
unsigned g_lastLoggedCodes = 0xFFFFFFFFu;

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
// KColor on the terrain materials; the shadow TEV stage treats KColor register 1's red channel as
// a WASH-OUT amount (0 = full shadow, max = washed out — the value maximum fog density produces).
// Pinning it to 255 feeds white into the shadow stage so it stops darkening the ground; the base
// ground texture (stage 0) is untouched, so this does not hole the floor.
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

    J3DGXColor wash;
    wash.r = 255;
    wash.g = 0;
    wash.b = 0;
    wash.a = 0;

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
            material->setTevKColor(1, &wash);
            ++g_hitAccum;
        }
    }
}

void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Terrain Shadow Removal");
    add_toggle(panel, "Enabled", g_cvarEnabled,
        "Removes the animated ground-shadow overlay TP bakes into the terrain material as a second "
        "texture stage (the swaying floor shade the moya removal cannot reach). Washes only the "
        "shadow stage; the base ground is left intact, so it does not hole the floor. "
        "EXPERIMENTAL - global terrain change, so verify per area.");
    add_toggle(panel, "Log Overlay Materials", g_cvarLog,
        "Prints how many cloud-shadow overlay materials each area has and which codes "
        "(MA00/MA01/MA16/MA04) they use, plus how many are removed. If a swaying shade persists "
        "but the seen count stays 0, it is not this system.");
    for (int i = 0; i < kCodeCount; ++i) {
        add_toggle(panel, kCodes[i].label, g_cvarRemove[i], kCodes[i].help);
    }
}

ModResult init(ModError* error) {
    // DEFAULT: off (opt-in; global terrain change). Logging off.
    ModResult result = register_bool("tsrEnabled", false, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool("tsrLog", false, g_cvarLog, error);
    if (result != MOD_OK) {
        return result;
    }
    for (int i = 0; i < kCodeCount; ++i) {
        char name[24];
        std::snprintf(name, sizeof(name), "tsrRemoveMA%c%c", kCodes[i].c5, kCodes[i].c6);
        // DEFAULT: every shadow-overlay code on when the feature is enabled (MA04 is the confirmed
        // forest-floor shade); untick any code whose overlay you want to keep in another area.
        result = register_bool(name, true, g_cvarRemove[i], error);
        if (result != MOD_OK) {
            return result;
        }
    }
    if (mods::hook_add_post<MAxxProc>(svc_hook, on_maxx_post) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook dKy_bg_MAxx_proc");
    }
    return MOD_OK;
}

// Game thread, once per frame. Caches config for the hook and reports the overlay tally.
void update() {
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
}

void shutdown() {
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
}

}  // namespace er_tsr

// ===========================================================================================
// SUB-FEATURE: Unbaked Vertex Lighting   (fade the lighting baked into vertex colors)
// ===========================================================================================
namespace er_vu {

DEFINE_HOOK(&J3DModelLoaderDataBase::load, LoadBmd);
DEFINE_HOOK(&J3DModelLoaderDataBase::loadBinaryDisplayList, LoadBdl);

ConfigVarHandle g_cvarVertexLight = 0;   // DEFAULT in init()

uint32_t g_patchedModels = 0;
uint32_t g_patchedColors = 0;
bool g_loggedFirstPatch = false;
bool g_warnedStride = false;

// Blend factor t in [0,1]: 1 = vanilla vertex colors, 0 = flattened to white.
float current_t() {
    int64_t value = 100;
    if (g_cvarVertexLight == 0 ||
        svc_config->get_int(mod_ctx, g_cvarVertexLight, &value) != MOD_OK)
    {
        return 1.0f;
    }
    return static_cast<float>(std::clamp<int64_t>(value, 0, 100)) / 100.0f;
}

inline uint32_t lift(uint32_t v, uint32_t maxValue, float t) {
    const float lifted = static_cast<float>(maxValue) * (1.0f - t) + static_cast<float>(v) * t;
    return std::min(static_cast<uint32_t>(lifted + 0.5f), maxValue);
}

bool find_color_format(GXVtxAttrFmtList* list, GXAttr attr, GXCompType& outType) {
    if (list == nullptr) {
        return false;
    }
    for (; list->attr != GX_VA_NULL; ++list) {
        if (list->attr == attr) {
            outType = list->type;
            return true;
        }
    }
    return false;
}

void patch_color_array(uint8_t* data, uint32_t count, uint32_t stride, GXCompType type, float t) {
    const auto strideMatches = [&](uint32_t expected) {
        if (stride == expected) {
            return true;
        }
        if (!g_warnedStride) {
            g_warnedStride = true;
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                "unexpected color stride %u for format %d; array skipped", stride,
                static_cast<int>(type));
            svc_log->warn(mod_ctx, msg);
        }
        return false;
    };

    switch (type) {
    case GX_RGBA8:
    case GX_RGBX8:  // bytes [R, G, B, A/X]; alpha (or padding) untouched
        if (!strideMatches(4)) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* c = data + i * 4;
            c[0] = static_cast<uint8_t>(lift(c[0], 255, t));
            c[1] = static_cast<uint8_t>(lift(c[1], 255, t));
            c[2] = static_cast<uint8_t>(lift(c[2], 255, t));
        }
        break;
    case GX_RGB8:  // bytes [R, G, B]
        if (!strideMatches(3)) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* c = data + i * 3;
            c[0] = static_cast<uint8_t>(lift(c[0], 255, t));
            c[1] = static_cast<uint8_t>(lift(c[1], 255, t));
            c[2] = static_cast<uint8_t>(lift(c[2], 255, t));
        }
        break;
    case GX_RGB565:  // native u16: RRRRRGGG GGGBBBBB
        if (!strideMatches(2)) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t* c = reinterpret_cast<uint16_t*>(data + i * 2);
            const uint32_t v = *c;
            const uint32_t r = lift((v >> 11) & 0x1F, 31, t);
            const uint32_t g = lift((v >> 5) & 0x3F, 63, t);
            const uint32_t b = lift(v & 0x1F, 31, t);
            *c = static_cast<uint16_t>((r << 11) | (g << 5) | b);
        }
        break;
    case GX_RGBA4:  // native u16: RRRRGGGG BBBBAAAA; alpha untouched
        if (!strideMatches(2)) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t* c = reinterpret_cast<uint16_t*>(data + i * 2);
            const uint32_t v = *c;
            const uint32_t r = lift((v >> 12) & 0xF, 15, t);
            const uint32_t g = lift((v >> 8) & 0xF, 15, t);
            const uint32_t b = lift((v >> 4) & 0xF, 15, t);
            *c = static_cast<uint16_t>((r << 12) | (g << 8) | (b << 4) | (v & 0xF));
        }
        break;
    case GX_RGBA6:  // 3 bytes, big-endian 24-bit RRRRRRGG GGGGBBBB BBAAAAAA; alpha untouched
        if (!strideMatches(3)) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* c = data + i * 3;
            const uint32_t v = (static_cast<uint32_t>(c[0]) << 16) |
                               (static_cast<uint32_t>(c[1]) << 8) | c[2];
            const uint32_t r = lift((v >> 18) & 0x3F, 63, t);
            const uint32_t g = lift((v >> 12) & 0x3F, 63, t);
            const uint32_t b = lift((v >> 6) & 0x3F, 63, t);
            const uint32_t packed = (r << 18) | (g << 12) | (b << 6) | (v & 0x3F);
            c[0] = static_cast<uint8_t>(packed >> 16);
            c[1] = static_cast<uint8_t>(packed >> 8);
            c[2] = static_cast<uint8_t>(packed);
        }
        break;
    default:
        break;  // not a color format we know; leave the data alone
    }
}

void patch_model(J3DModelData* modelData) {
    if (modelData == nullptr) {
        return;
    }
    const float t = current_t();
    if (t >= 0.995f) {
        return;  // vanilla — leave the resource untouched
    }

    J3DVertexData& vertexData = modelData->getVertexData();
    bool patched = false;
    for (uint8_t idx = 0; idx < 2; ++idx) {
        uint8_t* array = reinterpret_cast<uint8_t*>(vertexData.getVtxColorArray(idx));
        if (array == nullptr) {
            continue;
        }
        const GXAttr attr = idx == 0 ? GX_VA_CLR0 : GX_VA_CLR1;
        const uint32_t count = vertexData.getVtxArrNum(attr);
        const uint32_t stride = vertexData.getVtxArrStride(attr);
        if (count == 0 || stride == 0) {
            continue;
        }
        GXCompType type;
        if (!find_color_format(vertexData.getVtxAttrFmtList(), attr, type)) {
            continue;
        }
        patch_color_array(array, count, stride, type, t);
        patched = true;
        ++g_patchedColors;
    }

    if (patched) {
        ++g_patchedModels;
        if (!g_loggedFirstPatch) {
            g_loggedFirstPatch = true;
            svc_log->info(mod_ctx, "vertex colors are being unbaked as models load");
        }
    }
}

void on_model_loaded(ModContext*, void*, void* retval, void*) {
    if (retval == nullptr) {
        return;
    }
    patch_model(*static_cast<J3DModelData**>(retval));
}

void build_section(UiElementHandle panel) {
    svc_ui->pane_add_section(mod_ctx, panel, "Unbaked Vertex Lighting");
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = "Vertex Lighting";
    control.help_rml =
        "How much of the baked-in vertex lighting to keep. 100 is the untouched vanilla look; "
        "0 removes it entirely, leaving a flat texture-only base for realtime shadow/GI mods "
        "to light. Applies to models as they LOAD - after changing this, re-enter the area "
        "(or reload the save) to see the new value.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarVertexLight;
    control.min = 0;
    control.max = 100;
    control.step = 5;
    control.suffix = "%";
    add_control(panel, control);
}

ModResult init(ModError* error) {
    // DEFAULT: 100 = vanilla baked vertex lighting kept.
    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "vertexLight";
    cvarDesc.type = CONFIG_VAR_INT;
    cvarDesc.default_int = 100;
    if (svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarVertexLight) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register vertexLight option");
    }
    if (mods::hook_add_post<LoadBmd>(svc_hook, on_model_loaded) != MOD_OK ||
        mods::hook_add_post<LoadBdl>(svc_hook, on_model_loaded) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to hook the J3D model loader");
    }
    return MOD_OK;
}

void shutdown() {
    g_cvarVertexLight = 0;
    g_patchedModels = 0;
    g_patchedColors = 0;
    g_loggedFirstPatch = false;
    g_warnedStride = false;
}

}  // namespace er_vu

// ===========================================================================================
// Combined mod entry points + shared UI panel
// ===========================================================================================
namespace {

ModResult build_combined_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    er_psr::build_section(panel);
    er_tsr::build_section(panel);
    er_vu::build_section(panel);
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = er_psr::init(error);
    if (result != MOD_OK) {
        return result;
    }
    result = er_tsr::init(error);
    if (result != MOD_OK) {
        return result;
    }
    result = er_vu::init(error);
    if (result != MOD_OK) {
        return result;
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_combined_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "effect_remover ready (moya + terrain shadow + vertex unbake)");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    er_psr::update();
    er_tsr::update();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    er_vu::shutdown();
    er_tsr::shutdown();
    er_psr::shutdown();
    return MOD_OK;
}
}
