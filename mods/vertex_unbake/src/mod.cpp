// Unbaked Vertex Lighting — fades out the lighting baked into J3D vertex colors.
//
// Twilight Princess bakes most of its world lighting offline: room models carry hand-authored
// per-vertex colors (crevice darkening, interior gradients, painted pools of torchlight) that
// rasterize into the color channel and multiply the textures. That baked layer is what makes
// vanilla scenes look "lit" — and what fights realtime replacements: Realtime Sun Shadows and
// SSILVB shade on top of shading that is already painted in.
//
// This mod intercepts every J3D model load (J3DModelLoaderDataBase::load /
// loadBinaryDisplayList — the single funnel all BMD/BDL models pass through) and rewrites the
// model's CLR0/CLR1 vertex-color arrays IN PLACE:
//
//     color.rgb' = mix(white, color.rgb, vertexLight / 100)
//
// 100 keeps the vanilla data untouched, 0 flattens every vertex color to white (texture-only
// base for the realtime stack), and anything between is an exact linear blend. Alpha is never
// touched — vertex alpha carries transparency/effect data, not lighting.
//
// WHY THE LOAD HOOK: on this platform aurora streams vertex arrays from these buffers every
// frame, so a one-time patch at load covers every subsequent frame at zero per-frame cost, and
// the loader funnel guarantees exactly one patch per model (no registry, no double-patching,
// no dangling pointers when rooms unload). The tradeoff: changing the setting affects models
// loaded AFTER the change — re-enter the area (or reload the save) to see a new value.
//
// The arrays sit in the model's resource memory in one of six GX color formats; the PC loader
// records per-attribute stride/count (TARGET_PC), and the format comes from the model's vertex
// attribute format list. All six formats are handled; a mismatch between the recorded stride
// and the format's expected stride skips the array (logged once) rather than corrupting it.
//
// Scope note: the hook sees EVERY J3D model — rooms, props, characters. Baked vertex shading
// on props/characters flattens too, which is consistent with "remove vertex lighting" but can
// wash decorative vertex tinting on a few models. EXPERIMENTAL.
//
// Design + rationale: docs/ssilvb_plan.md §8 (un-bake / relight entry).

#include "global.h"

#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphLoader/J3DModelLoader.h"
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

DEFINE_HOOK(&J3DModelLoaderDataBase::load, LoadBmd);
DEFINE_HOOK(&J3DModelLoaderDataBase::loadBinaryDisplayList, LoadBdl);

namespace {

ConfigVarHandle g_cvarVertexLight = 0;

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

// mix(maxValue, v, t), rounded — the exact "lift toward white" for an n-bit channel.
inline uint32_t lift(uint32_t v, uint32_t maxValue, float t) {
    const float lifted = static_cast<float>(maxValue) * (1.0f - t) + static_cast<float>(v) * t;
    return std::min(static_cast<uint32_t>(lifted + 0.5f), maxValue);
}

// The color-attribute component type from the model's vertex attribute format list (terminated
// by GX_VA_NULL). Returns false if the attribute isn't listed.
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

// Rewrite one color array in place. Byte layouts follow the PC loader's endian fixup
// (J3DModelLoader::readVertexData): 1-byte-component formats keep the file's byte order,
// 2-byte formats were swapped to native u16.
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
    case GX_RGBX8: // bytes [R, G, B, A/X]; alpha (or padding) untouched
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
    case GX_RGB8: // bytes [R, G, B]
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
    case GX_RGB565: // native u16: RRRRRGGG GGGBBBBB
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
    case GX_RGBA4: // native u16: RRRRGGGG BBBBAAAA; alpha untouched
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
    case GX_RGBA6: // 3 bytes, big-endian 24-bit RRRRRRGG GGGGBBBB BBAAAAAA; alpha untouched
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
        break; // not a color format we know; leave the data alone
    }
}

// Game thread, immediately after a model's data finished loading: lift its vertex colors
// toward white by the current setting. Runs exactly once per model.
void patch_model(J3DModelData* modelData) {
    if (modelData == nullptr) {
        return;
    }
    const float t = current_t();
    if (t >= 0.995f) {
        return; // vanilla — leave the resource untouched
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

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
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
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
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

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);

    svc_log->info(mod_ctx, "vertex_unbake ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // Hooks are removed by the host on unload. Already-patched models keep their flattened
    // colors until their area reloads — the load funnel repatches (or leaves vanilla) then.
    g_cvarVertexLight = 0;
    g_patchedModels = 0;
    g_patchedColors = 0;
    g_loggedFirstPatch = false;
    g_warnedStride = false;
    return MOD_OK;
}
}
