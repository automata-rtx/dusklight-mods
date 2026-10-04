// ReShade Port: runs ReShade effect files (.fx) inside the game's frame.
//
// The effects come from <mod data folder>/reshade-shaders/Shaders and /Textures, laid out as a
// ReShade installation lays them out, and their settings live in <mod data folder>/ReShadePreset.ini
// in ReShade's own preset format. Each technique runs at one of four insertion points:
//
//   Before transparency           GFX_STAGE_SCENE_AFTER_OPAQUE
//   Before particles & post-FX    post-hook on dComIfGd_drawXluListDark (game_hooks.cpp)
//   Before HUD                    GFX_STAGE_FRAME_BEFORE_HUD
//   After HUD                     GFX_STAGE_FRAME_AFTER_HUD
//
// At each point that has work: resolve_pass snapshots colour and depth, the runtime builds a plan
// (fx_runtime.cpp), a compute task records the techniques into the frame encoder (copy the snapshot
// to the effects' back buffer, convert depth, run the passes), and a draw writes the result back into
// the live scene pass (RGB only: the game's alpha is left alone).
//
// Threads: stage callbacks, the game hook, mod_update and UI callbacks run on the game thread; the
// compute and draw callbacks run on the render worker and only touch the runtime's plan ring and
// immutable GPU objects. Effects compile on the runtime's own build thread.
//
// Design notes and limitations: docs/reshade_port.md.

#include "fx_images.hpp"
#include "fx_runtime.hpp"
#include "game_hooks.hpp"

#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/host.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "gfx_scene_pass.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(HostService, svc_host);

namespace {

using rsp::InsertionPoint;

std::unique_ptr<rsp::Runtime> g_runtime;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
GfxComputeTypeHandle g_computeType = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_stageHooks[3] = {};
bool g_hookInstalled = false;

// Config vars ("enabled" is reserved by the loader).
ConfigVarHandle g_cvarActive = 0;
ConfigVarHandle g_cvarDepthDistance = 0;

UiWindowHandle g_window = 0;
UiElementHandle g_panelStatus = 0;

struct ComputePayload {
    uint64_t seq;
    uint32_t slot;
    uint32_t pad;
};
struct DrawPayload {
    uint64_t seq;
    WGPUTextureView result;
    uint32_t slot;
    uint32_t pad;
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE && std::is_trivially_copyable_v<ComputePayload>);
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE && std::is_trivially_copyable_v<DrawPayload>);

// Composite pipelines, one per scene-pass layout key. Render worker only (released in shutdown).
struct Composite {
    WGPURenderPipeline pipeline = nullptr;
};
std::map<uint64_t, Composite> g_composites;

bool get_bool_option(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

int64_t get_int_option(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

std::string rml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\n': out += "<br/>"; break;
        default: out += c;
        }
    }
    return out;
}

// --- Render worker --------------------------------------------------------------------------------

void on_compute(ModContext*, const GfxComputeContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(ComputePayload) || ctx == nullptr || g_runtime == nullptr) {
        return;
    }
    ComputePayload p;
    std::memcpy(&p, payload, sizeof(p));
    g_runtime->execute(p.slot, p.seq, ctx->encoder, ctx->queue);
}

WGPURenderPipeline ensure_composite(const GfxDrawContext& ctx, const rsp::GpuShared& shared) {
    const uint64_t key = gfx_compat::scene_pass_layout_key(ctx);
    if (const auto it = g_composites.find(key); it != g_composites.end()) {
        return it->second.pipeline;
    }
    gfx_compat::ScenePassLayout layout;
    if (!gfx_compat::scene_pass_layout_for_draw(ctx, g_deviceInfo, layout)) {
        return nullptr;
    }
    // Colour only: the game's EFB alpha is left as the game wrote it.
    layout.color_targets[0].writeMask = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = shared.utility_module();
    fs.entryPoint = {"fs_copy", WGPU_STRLEN};
    fs.targetCount = layout.color_target_count;
    fs.targets = layout.color_targets;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = layout.depth_format;
    ds.depthWriteEnabled = WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.label = {"ReShade port composite", WGPU_STRLEN};
    rpd.layout = shared.single_texture_pipeline_layout();
    rpd.vertex.module = shared.utility_module();
    rpd.vertex.entryPoint = {"vs_fullscreen", WGPU_STRLEN};
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.depthStencil = layout.depth_format != WGPUTextureFormat_Undefined ? &ds : nullptr;
    rpd.multisample.count = layout.sample_count;
    rpd.fragment = &fs;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(ctx.device, &rpd);
    g_composites[key] = Composite{pipeline};
    return pipeline;
}

void on_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(DrawPayload) || ctx == nullptr || g_runtime == nullptr) {
        return;
    }
    DrawPayload p;
    std::memcpy(&p, payload, sizeof(p));
    const rsp::GpuShared* shared = g_runtime->game_shared();
    WGPURenderPipeline pipeline = shared != nullptr ? ensure_composite(*ctx, *shared) : nullptr;
    if (pipeline != nullptr && p.result != nullptr) {
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.textureView = p.result;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = shared->single_texture_layout();
        bgd.entryCount = 1;
        bgd.entries = &e;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(ctx->device, &bgd);
        const GfxColorAttachmentLayout& target = ctx->layout.color_attachments[GFX_SCENE_COLOR_ATTACHMENT_INDEX];
        wgpuRenderPassEncoderSetPipeline(ctx->pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, group, 0, nullptr);
        // The game may have left a smaller viewport (2D ports); the composite covers the target.
        if (target.width != 0 && target.height != 0) {
            wgpuRenderPassEncoderSetViewport(ctx->pass, 0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f);
            wgpuRenderPassEncoderSetScissorRect(ctx->pass, 0, 0, target.width, target.height);
        }
        wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
        wgpuBindGroupRelease(group);
    }
    g_runtime->finish_plan(p.slot, p.seq);
}

// --- Game thread: insertion points ----------------------------------------------------------------

void run_point(InsertionPoint point) {
    if (g_runtime == nullptr || !g_runtime->point_has_work(point)) {
        return;
    }
    GfxResolveDesc rd = GFX_RESOLVE_DESC_INIT;
    rd.color = true;
    rd.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &rd, &resolved) != MOD_OK || resolved.color == nullptr) {
        return;
    }
    uint32_t slot = 0;
    uint64_t seq = 0;
    WGPUTextureView result = nullptr;
    if (!g_runtime->prepare(point, resolved.color, resolved.depth, resolved.width, resolved.height, slot, seq, result)) {
        return;
    }
    const ComputePayload cp{seq, slot, 0};
    if (svc_gfx->push_compute(mod_ctx, g_computeType, &cp, sizeof(cp)) != MOD_OK) {
        return;
    }
    const DrawPayload dp{seq, result, slot, 0};
    svc_gfx->push_draw(mod_ctx, g_drawType, &dp, sizeof(dp));
}

void on_after_opaque(ModContext*, const GfxStageContext* stage, void*) {
    if (g_runtime == nullptr) {
        return;
    }
    if (stage != nullptr && stage->game_view != nullptr) {
        CameraInfo camera = CAMERA_INFO_INIT;
        if (svc_camera->get_camera(mod_ctx, stage->game_view, &camera) == MOD_OK) {
            rsp::CameraDepth depth;
            std::memcpy(depth.proj_from_view, camera.proj_from_view, sizeof(depth.proj_from_view));
            depth.near_plane = camera.near_plane;
            depth.far_plane = camera.far_plane;
            g_runtime->set_camera(depth);
        }
    }
    run_point(InsertionPoint::BeforeTransparency);
}

void on_after_translucent() { run_point(InsertionPoint::BeforeParticles); }

void on_before_hud(ModContext*, const GfxStageContext*, void*) { run_point(InsertionPoint::BeforeHud); }

void on_after_hud(ModContext*, const GfxStageContext*, void*) { run_point(InsertionPoint::AfterHud); }

// --- UI -------------------------------------------------------------------------------------------

const char* const kPointLabels[] = {
    "Before transparency",
    "Before particles & post-processing",
    "Before HUD",
    "After HUD",
};
static_assert(std::size(kPointLabels) == rsp::kInsertionPointCount);

const char* const kPointHelp =
    "<b>Before transparency</b>: the opaque world only. Water, glass, particles and all of the "
    "game's post-processing are drawn over the effect. Good for ambient occlusion and other "
    "effects that read depth.<br/><br/>"
    "<b>Before particles &amp; post-processing</b> (default): the whole world, including "
    "translucent surfaces. Particles, the game's motion blur, depth of field, heat haze and bloom "
    "are applied on top of the effect, so they warp and blur it as they warp the world.<br/><br/>"
    "<b>Before HUD</b>: after all of the game's post-processing; the HUD is drawn over the "
    "effect.<br/><br/>"
    "<b>After HUD</b>: the finished frame, where standalone ReShade runs. The effect also "
    "applies to the HUD and menus.";

// Techniques tab ------------------------------------------------------------------------------------

std::string g_selectedTechnique; // unique name
UiListHandle g_techniqueList = 0;
UiElementHandle g_selectedText = 0;
uint64_t g_listVersion = 0;
std::vector<std::string> g_listLabels;
std::vector<std::string> g_listNames;

size_t selected_index() {
    if (g_runtime == nullptr) {
        return SIZE_MAX;
    }
    const auto& list = g_runtime->techniques();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].unique_name() == g_selectedTechnique) {
            return i;
        }
    }
    return SIZE_MAX;
}

uint64_t technique_key(const std::string& unique) { return std::hash<std::string>{}(unique) | 1u; }

std::string technique_state(const rsp::TechniqueEntry& t) {
    rsp::EffectState* e = g_runtime->effect(t.file);
    if (e == nullptr || e->build == nullptr) {
        return "compiling";
    }
    if (!e->build->compile_ok) {
        return "failed to compile";
    }
    if (!t.enabled) {
        return "";
    }
    if (!e->build->gpu_done) {
        return "building";
    }
    if (t.module_index >= e->build->technique_ok.size() || e->build->technique_ok[t.module_index] == 0) {
        return "cannot run";
    }
    return "";
}

void refresh_technique_list(bool force) {
    if (g_techniqueList == 0 || g_runtime == nullptr) {
        return;
    }
    if (!force && g_listVersion == g_runtime->technique_list_version() + g_runtime->effects_version()) {
        return;
    }
    g_listVersion = g_runtime->technique_list_version() + g_runtime->effects_version();
    g_listLabels.clear();
    g_listNames.clear();
    std::vector<UiListItem> items;
    for (const rsp::TechniqueEntry& t : g_runtime->techniques()) {
        if (!t.present || t.hidden) {
            continue;
        }
        std::string label = (t.enabled ? "[x] " : "[  ] ") + (t.label.empty() ? t.name : t.label);
        label += "  -  ";
        label += kPointLabels[static_cast<size_t>(t.point)];
        const std::string state = technique_state(t);
        if (!state.empty()) {
            label += "  (" + state + ")";
        }
        g_listLabels.push_back(std::move(label));
        g_listNames.push_back(t.unique_name());
    }
    for (size_t i = 0; i < g_listLabels.size(); ++i) {
        UiListItem item = UI_LIST_ITEM_INIT;
        item.key = technique_key(g_listNames[i]);
        item.label = g_listLabels[i].c_str();
        items.push_back(item);
    }
    svc_ui->list_set_items(mod_ctx, g_techniqueList, items.data(), items.size());
}

void update_selected_text() {
    if (g_selectedText == 0 || g_runtime == nullptr) {
        return;
    }
    const size_t i = selected_index();
    std::string rml;
    if (i == SIZE_MAX) {
        rml = "Select a technique in the list.";
    } else {
        const rsp::TechniqueEntry& t = g_runtime->techniques()[i];
        rml = "<b>" + rml_escape(t.label.empty() ? t.name : t.label) + "</b> (" + rml_escape(t.name) + " in " + rml_escape(t.file) + ")";
        const std::string state = technique_state(t);
        if (!state.empty()) {
            rml += "<br/>Status: " + rml_escape(state);
        }
        if (!t.tooltip.empty()) {
            rml += "<br/>" + rml_escape(t.tooltip);
        }
    }
    svc_ui->elem_set_rml(mod_ctx, g_selectedText, rml.c_str());
}

void on_list_pressed(ModContext*, UiListHandle, uint64_t key, void*) {
    if (g_runtime == nullptr) {
        return;
    }
    for (const rsp::TechniqueEntry& t : g_runtime->techniques()) {
        if (technique_key(t.unique_name()) == key) {
            g_selectedTechnique = t.unique_name();
        }
    }
    update_selected_text();
}

bool on_list_selected(ModContext*, UiListHandle, uint64_t key, void*) {
    return !g_selectedTechnique.empty() && technique_key(g_selectedTechnique) == key;
}

bool no_selection(ModContext*, void*) { return selected_index() == SIZE_MAX; }

void get_selected_enabled(ModContext*, void*, UiControlValue* out) {
    const size_t i = selected_index();
    out->bool_value = i != SIZE_MAX && g_runtime->techniques()[i].enabled;
}

void set_selected_enabled(ModContext*, void*, const UiControlValue* v) {
    const size_t i = selected_index();
    if (i != SIZE_MAX) {
        g_runtime->set_technique_enabled(i, v->bool_value);
        refresh_technique_list(true);
        update_selected_text();
    }
}

void get_selected_point(ModContext*, void*, UiControlValue* out) {
    const size_t i = selected_index();
    out->int_value = i != SIZE_MAX ? static_cast<int64_t>(g_runtime->techniques()[i].point) : static_cast<int64_t>(rsp::kDefaultInsertionPoint);
}

void set_selected_point(ModContext*, void*, const UiControlValue* v) {
    const size_t i = selected_index();
    if (i != SIZE_MAX && v->int_value >= 0 && v->int_value < static_cast<int64_t>(rsp::kInsertionPointCount)) {
        g_runtime->set_technique_point(i, static_cast<InsertionPoint>(v->int_value));
        refresh_technique_list(true);
    }
}

void move_selected(int delta) {
    const size_t i = selected_index();
    if (i != SIZE_MAX) {
        g_runtime->move_technique(i, delta);
        refresh_technique_list(true);
    }
}

void on_move_up(ModContext*, void*) { move_selected(-1); }
void on_move_down(ModContext*, void*) { move_selected(+1); }

void add_control(UiElementHandle pane, const UiControlDesc& desc, UiElementHandle* out = nullptr) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, out);
}

ModResult build_techniques_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    g_techniqueList = 0;
    g_selectedText = 0;
    svc_ui->pane_add_section(mod_ctx, left, "Techniques, in the order they run");
    UiListDesc list = UI_LIST_DESC_INIT;
    list.on_pressed = on_list_pressed;
    list.is_selected = on_list_selected;
    svc_ui->pane_add_list(mod_ctx, left, &list, &g_techniqueList);
    refresh_technique_list(true);

    svc_ui->pane_add_section(mod_ctx, left, "Selected technique");
    svc_ui->pane_add_rml(mod_ctx, left, "", &g_selectedText);
    update_selected_text();

    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = "Enabled";
    c.get = get_selected_enabled;
    c.set = set_selected_enabled;
    c.is_disabled = no_selection;
    c.help_rml = "Runs the technique. Effects are built the first time one of their techniques is enabled.";
    add_control(left, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_DROPDOWN;
    c.label = "Insertion point";
    c.options = kPointLabels;
    c.option_count = rsp::kInsertionPointCount;
    c.get = get_selected_point;
    c.set = set_selected_point;
    c.is_disabled = no_selection;
    c.help_rml = kPointHelp;
    add_control(left, c);

    UiRowDesc row = UI_ROW_DESC_INIT;
    UiElementHandle buttons = 0;
    svc_ui->pane_add_row(mod_ctx, left, &row, &buttons);
    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Move up";
    c.on_pressed = on_move_up;
    c.is_disabled = no_selection;
    add_control(buttons != 0 ? buttons : left, c);
    c.label = "Move down";
    c.on_pressed = on_move_down;
    add_control(buttons != 0 ? buttons : left, c);

    svc_ui->pane_add_rml(mod_ctx, right,
        "Techniques run in list order. At each insertion point, the techniques assigned to it run "
        "in that order, exactly as ReShade runs its list.<br/><br/>",
        nullptr);
    svc_ui->pane_add_rml(mod_ctx, right, kPointHelp, nullptr);
    return MOD_OK;
}

ModResult update_techniques_tab(ModContext*, void*, ModError*) {
    refresh_technique_list(false);
    return MOD_OK;
}

// Parameters tab ------------------------------------------------------------------------------------

struct ParamBinding {
    std::string file;
    std::string uniform;
    uint32_t component = 0;
    float step = 0.01f;
    float min = 0.0f;
    float max = 0.0f;
    bool has_range = false;
    int decimals = 3;
    std::string text; // getter result storage
    std::vector<std::string> items;
    std::vector<const char*> item_ptrs;
};
std::deque<ParamBinding> g_params;
std::string g_paramEffect;
std::string g_paramDefinitionsText;

const reshadefx::uniform* find_uniform(const ParamBinding& b, rsp::EffectState*& state) {
    state = g_runtime != nullptr ? g_runtime->effect(b.file) : nullptr;
    if (state == nullptr || state->build == nullptr || state->build->compiled == nullptr) {
        return nullptr;
    }
    for (const reshadefx::uniform& u : state->build->compiled->module().uniforms) {
        if (u.name == b.uniform) {
            return &u;
        }
    }
    return nullptr;
}

template <class T>
bool read_component(ParamBinding& b, T& out) {
    rsp::EffectState* state = nullptr;
    const reshadefx::uniform* u = find_uniform(b, state);
    if (u == nullptr) {
        return false;
    }
    T values[16] = {};
    state->uniforms.get(*u, values, u->type.components());
    out = values[std::min<uint32_t>(b.component, 15u)];
    return true;
}

template <class T>
void write_component(ParamBinding& b, T value) {
    rsp::EffectState* state = nullptr;
    const reshadefx::uniform* u = find_uniform(b, state);
    if (u == nullptr) {
        return;
    }
    T values[16] = {};
    state->uniforms.get(*u, values, u->type.components());
    values[std::min<uint32_t>(b.component, 15u)] = value;
    state->uniforms.set(*u, values, u->type.components());
    g_runtime->uniform_changed(b.file, *u);
}

void param_get_bool(ModContext*, void* ud, UiControlValue* out) {
    bool v = false;
    read_component(*static_cast<ParamBinding*>(ud), v);
    out->bool_value = v;
}
void param_set_bool(ModContext*, void* ud, const UiControlValue* v) { write_component(*static_cast<ParamBinding*>(ud), v->bool_value); }

void param_get_int(ModContext*, void* ud, UiControlValue* out) {
    int32_t v = 0;
    read_component(*static_cast<ParamBinding*>(ud), v);
    out->int_value = v;
}
void param_set_int(ModContext*, void* ud, const UiControlValue* v) {
    write_component(*static_cast<ParamBinding*>(ud), static_cast<int32_t>(v->int_value));
}

std::string format_value(float v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    return buf;
}

void param_get_float_text(ModContext*, void* ud, UiControlValue* out) {
    auto& b = *static_cast<ParamBinding*>(ud);
    float v = 0.0f;
    read_component(b, v);
    b.text = format_value(v, b.decimals);
    out->string_value = b.text.c_str();
}

void param_set_float_text(ModContext*, void* ud, const UiControlValue* v) {
    auto& b = *static_cast<ParamBinding*>(ud);
    if (v->string_value == nullptr) {
        return;
    }
    char* end = nullptr;
    float f = std::strtof(v->string_value, &end);
    if (end == v->string_value || !std::isfinite(f)) {
        return;
    }
    if (b.has_range) {
        f = std::clamp(f, b.min, b.max);
    }
    write_component(b, f);
}

void param_step(ParamBinding& b, float direction) {
    float v = 0.0f;
    if (!read_component(b, v)) {
        return;
    }
    v += direction * b.step;
    // Snap to the step grid so repeated presses do not accumulate rounding error.
    const float origin = b.has_range ? b.min : 0.0f;
    v = origin + std::round((v - origin) / b.step) * b.step;
    if (b.has_range) {
        v = std::clamp(v, b.min, b.max);
    }
    write_component(b, v);
}
void param_decrement(ModContext*, void* ud) { param_step(*static_cast<ParamBinding*>(ud), -1.0f); }
void param_increment(ModContext*, void* ud) { param_step(*static_cast<ParamBinding*>(ud), +1.0f); }

void param_get_color(ModContext*, void* ud, UiControlValue* out) {
    auto& b = *static_cast<ParamBinding*>(ud);
    rsp::EffectState* state = nullptr;
    const reshadefx::uniform* u = find_uniform(b, state);
    float c[16] = {0, 0, 0, 1};
    if (u != nullptr) {
        state->uniforms.get(*u, c, u->type.components());
    }
    const uint32_t n = u != nullptr ? std::min<uint32_t>(u->type.components(), 4u) : 3u;
    char buf[16];
    const auto byte = [](float f) { return static_cast<unsigned>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    if (n >= 4) {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
    } else {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]));
    }
    b.text = buf;
    out->string_value = b.text.c_str();
}

void param_set_color(ModContext*, void* ud, const UiControlValue* v) {
    auto& b = *static_cast<ParamBinding*>(ud);
    rsp::EffectState* state = nullptr;
    const reshadefx::uniform* u = find_uniform(b, state);
    if (u == nullptr || v->string_value == nullptr) {
        return;
    }
    std::string s = v->string_value;
    if (!s.empty() && s[0] == '#') {
        s.erase(0, 1);
    }
    if (s.size() != 6 && s.size() != 8) {
        return;
    }
    float c[16] = {};
    state->uniforms.get(*u, c, u->type.components());
    for (size_t i = 0; i * 2 < s.size() && i < u->type.components(); ++i) {
        c[i] = static_cast<float>(std::strtoul(s.substr(i * 2, 2).c_str(), nullptr, 16)) / 255.0f;
    }
    state->uniforms.set(*u, c, u->type.components());
    g_runtime->uniform_changed(b.file, *u);
}

void param_reset(ModContext*, void* ud) {
    auto& b = *static_cast<ParamBinding*>(ud);
    rsp::EffectState* state = nullptr;
    if (const reshadefx::uniform* u = find_uniform(b, state)) {
        g_runtime->reset_uniform(b.file, *u);
    }
}

void get_effect_definitions(ModContext*, void*, UiControlValue* out) {
    g_paramDefinitionsText = g_runtime != nullptr ? g_runtime->effect_definitions(g_paramEffect) : std::string();
    out->string_value = g_paramDefinitionsText.c_str();
}
void set_effect_definitions(ModContext*, void*, const UiControlValue* v) {
    if (g_runtime != nullptr && v->string_value != nullptr) {
        g_runtime->set_effect_definitions(g_paramEffect, v->string_value);
    }
}

void add_float_row(UiElementHandle pane, ParamBinding& b, const std::string& label, const char* help) {
    UiRowDesc rd = UI_ROW_DESC_INIT;
    UiElementHandle row = 0;
    if (svc_ui->pane_add_row(mod_ctx, pane, &rd, &row) != MOD_OK) {
        row = pane;
    }
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_STRING;
    c.label = label.c_str();
    c.help_rml = help;
    c.get = param_get_float_text;
    c.set = param_set_float_text;
    c.user_data = &b;
    c.max_length = 24;
    add_control(row, c);
    for (int dir = -1; dir <= 1; dir += 2) {
        UiControlDesc s = UI_CONTROL_DESC_INIT;
        s.kind = UI_CONTROL_ICON_BUTTON;
        s.label = dir < 0 ? "Decrease" : "Increase";
        s.icon = dir < 0 ? "remove" : "add";
        s.on_pressed = dir < 0 ? param_decrement : param_increment;
        s.user_data = &b;
        if (svc_ui->pane_add_control(mod_ctx, row, &s, nullptr) != MOD_OK) {
            s.kind = UI_CONTROL_BUTTON;
            s.label = dir < 0 ? "-" : "+";
            s.icon = nullptr;
            add_control(row, s);
        }
    }
}

ModResult build_effect_params(ModContext*, UiElementHandle pane, void* ud, ModError*) {
    g_params.clear();
    g_paramEffect = static_cast<const char*>(ud);
    rsp::EffectState* state = g_runtime != nullptr ? g_runtime->effect(g_paramEffect) : nullptr;
    svc_ui->pane_add_section(mod_ctx, pane, g_paramEffect.c_str());
    if (state == nullptr || state->build == nullptr) {
        svc_ui->pane_add_text(mod_ctx, pane, "This effect is still compiling.", nullptr);
        return MOD_OK;
    }
    if (!state->build->messages.empty()) {
        svc_ui->pane_add_rml(mod_ctx, pane, ("<b>Compiler output</b><br/>" + rml_escape(state->build->messages)).c_str(), nullptr);
    }
    if (state->build->compiled == nullptr) {
        return MOD_OK;
    }
    const reshadefx::effect_module& module = state->build->compiled->module();
    std::string category;
    for (const reshadefx::uniform& u : module.uniforms) {
        if (rsp::special_of(u) != rsp::SpecialUniform::None || rsp::annotation_int(u.annotations, "hidden") != 0 ||
            u.type.is_matrix() || u.type.is_array()) {
            continue;
        }
        const std::string cat = rsp::annotation_string(u.annotations, "ui_category");
        if (cat != category && !cat.empty()) {
            svc_ui->pane_add_section(mod_ctx, pane, cat.c_str());
        }
        category = cat;
        std::string label = rsp::annotation_string(u.annotations, "ui_label");
        if (label.empty()) {
            label = u.name;
        }
        const std::string tooltip = rsp::annotation_string(u.annotations, "ui_tooltip");
        const std::string uiType = rsp::annotation_string(u.annotations, "ui_type");
        // help_rml must outlive the control; keep it in the binding.
        const auto make = [&](uint32_t component) -> ParamBinding& {
            ParamBinding& b = g_params.emplace_back();
            b.file = g_paramEffect;
            b.uniform = u.name;
            b.component = component;
            b.text = rml_escape(tooltip);
            return b;
        };
        const unsigned n = u.type.components();
        if (u.type.is_boolean()) {
            for (unsigned k = 0; k < n; ++k) {
                ParamBinding& b = make(k);
                const std::string l = n > 1 ? label + " [" + std::to_string(k) + "]" : label;
                b.items = {l, b.text};
                UiControlDesc c = UI_CONTROL_DESC_INIT;
                c.kind = UI_CONTROL_TOGGLE;
                c.label = b.items[0].c_str();
                c.help_rml = b.items[1].c_str();
                c.get = param_get_bool;
                c.set = param_set_bool;
                c.user_data = &b;
                add_control(pane, c);
            }
        } else if (u.type.is_integral()) {
            const std::string itemsText = rsp::annotation_string(u.annotations, "ui_items");
            for (unsigned k = 0; k < n; ++k) {
                ParamBinding& b = make(k);
                const std::string l = n > 1 ? label + " [" + std::to_string(k) + "]" : label;
                b.items = {l, b.text};
                UiControlDesc c = UI_CONTROL_DESC_INIT;
                c.label = b.items[0].c_str();
                c.help_rml = b.items[1].c_str();
                c.get = param_get_int;
                c.set = param_set_int;
                c.user_data = &b;
                if (!itemsText.empty() && (uiType == "combo" || uiType == "list" || uiType == "radio")) {
                    size_t start = 0;
                    std::vector<std::string> options;
                    while (start < itemsText.size()) {
                        const size_t end = itemsText.find('\0', start);
                        options.push_back(itemsText.substr(start, end == std::string::npos ? std::string::npos : end - start));
                        if (end == std::string::npos) {
                            break;
                        }
                        start = end + 1;
                    }
                    b.items.insert(b.items.end(), options.begin(), options.end());
                    for (size_t i = 2; i < b.items.size(); ++i) {
                        b.item_ptrs.push_back(b.items[i].c_str());
                    }
                    c.label = b.items[0].c_str();
                    c.help_rml = b.items[1].c_str();
                    c.kind = UI_CONTROL_DROPDOWN;
                    c.options = b.item_ptrs.data();
                    c.option_count = b.item_ptrs.size();
                } else {
                    c.kind = UI_CONTROL_NUMBER;
                    const int lo = rsp::annotation_int(u.annotations, "ui_min", 0, u.type.is_signed() ? INT32_MIN / 2 : 0);
                    const int hi = rsp::annotation_int(u.annotations, "ui_max", 0, INT32_MAX / 2);
                    c.min = lo;
                    c.max = hi > lo ? hi : lo + 1;
                    c.step = std::max(1, rsp::annotation_int(u.annotations, "ui_step", 0, 1));
                }
                if (c.kind != UI_CONTROL_DROPDOWN || c.option_count != 0) {
                    add_control(pane, c);
                }
            }
        } else if (u.type.is_floating_point()) {
            if (uiType == "color" && (n == 3 || n == 4)) {
                ParamBinding& b = make(0);
                b.items = {label, b.text};
                UiControlDesc c = UI_CONTROL_DESC_INIT;
                c.kind = UI_CONTROL_COLOR;
                c.label = b.items[0].c_str();
                c.help_rml = b.items[1].c_str();
                c.color_alpha = n == 4;
                c.get = param_get_color;
                c.set = param_set_color;
                c.user_data = &b;
                add_control(pane, c);
                continue;
            }
            for (unsigned k = 0; k < n; ++k) {
                ParamBinding& b = make(k);
                const float lo = rsp::annotation_float(u.annotations, "ui_min", 0, 0.0f);
                const float hi = rsp::annotation_float(u.annotations, "ui_max", 0, 0.0f);
                b.has_range = rsp::find_annotation(u.annotations, "ui_min") != nullptr &&
                              rsp::find_annotation(u.annotations, "ui_max") != nullptr && hi > lo;
                b.min = lo;
                b.max = hi;
                float step = rsp::annotation_float(u.annotations, "ui_step", 0, 0.0f);
                if (!(step > 0.0f)) {
                    step = b.has_range ? (hi - lo) / 100.0f : 0.01f;
                }
                b.step = step;
                b.decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-4)) + 1, 1, 6);
                const std::string l = n > 1 ? label + " [" + std::string(1, "xyzw"[k]) + "]" : label;
                b.items = {l, b.text};
                add_float_row(pane, b, b.items[0], b.items[1].c_str());
            }
        }
    }

    svc_ui->pane_add_section(mod_ctx, pane, "Effect preprocessor definitions");
    std::string defsHelp = "NAME=VALUE pairs separated by commas, for this effect only. Changing them recompiles all effects.";
    if (!state->build->compiled->used_definitions.empty()) {
        defsHelp += "<br/><br/>Definitions this effect reads (current values):<br/>";
        for (const auto& [name, value] : state->build->compiled->used_definitions) {
            defsHelp += rml_escape(name) + " = " + rml_escape(value) + "<br/>";
        }
    }
    ParamBinding& holder = g_params.emplace_back();
    holder.text = defsHelp;
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_STRING;
    c.label = "Definitions";
    c.help_rml = holder.text.c_str();
    c.get = get_effect_definitions;
    c.set = set_effect_definitions;
    add_control(pane, c);
    return MOD_OK;
}

std::vector<std::string> g_effectNames; // storage for group user_data

ModResult build_parameters_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    g_effectNames.clear();
    if (g_runtime == nullptr) {
        return MOD_OK;
    }
    std::vector<std::string> enabled;
    std::vector<std::string> others;
    for (const auto& [file, state] : g_runtime->effects()) {
        bool on = false;
        for (const rsp::TechniqueEntry& t : g_runtime->techniques()) {
            on = on || (t.file == file && t.enabled && t.present);
        }
        (on ? enabled : others).push_back(file);
    }
    g_effectNames.reserve(enabled.size() + others.size());
    const auto add_groups = [&](const char* title, const std::vector<std::string>& files) {
        if (files.empty()) {
            return;
        }
        svc_ui->pane_add_section(mod_ctx, left, title);
        for (const std::string& f : files) {
            g_effectNames.push_back(f);
            UiGroupDesc g = UI_GROUP_DESC_INIT;
            g.label = g_effectNames.back().c_str();
            g.build = build_effect_params;
            g.user_data = const_cast<char*>(g_effectNames.back().c_str());
            svc_ui->pane_add_group(mod_ctx, left, right, &g, nullptr);
        }
    };
    add_groups("Effects with enabled techniques", enabled);
    add_groups("Other effects", others);
    if (enabled.empty() && others.empty()) {
        svc_ui->pane_add_text(mod_ctx, left, "No effects found yet. See the Settings tab for the folder.", nullptr);
    }
    return MOD_OK;
}

// Settings tab --------------------------------------------------------------------------------------

UiElementHandle g_settingsStatus = 0;
std::string g_globalDefinitionsText;

std::string status_text() {
    if (g_runtime == nullptr) {
        return "Not running.";
    }
    const rsp::RuntimeStatus s = g_runtime->status();
    std::string t;
    if (s.compile_total != 0 && s.compile_done < s.compile_total) {
        t = "Compiling effects: " + std::to_string(s.compile_done) + " / " + std::to_string(s.compile_total);
    } else {
        t = std::to_string(s.effects_compiled) + " effects compiled";
        if (s.effects_failed != 0) {
            t += ", " + std::to_string(s.effects_failed) + " failed";
        }
    }
    if (s.width != 0) {
        t += " (" + std::to_string(s.width) + "x" + std::to_string(s.height) + ")";
    }
    size_t active = 0;
    for (const rsp::TechniqueEntry& tech : g_runtime->techniques()) {
        active += tech.enabled && tech.present ? 1 : 0;
    }
    t += ", " + std::to_string(active) + " techniques enabled";
    if (!s.validation_device) {
        t += ". Effects are not validated before use!";
    }
    if (!s.last_error.empty()) {
        t += "\n" + s.last_error;
    }
    return t;
}

void get_global_definitions(ModContext*, void*, UiControlValue* out) {
    g_globalDefinitionsText = g_runtime != nullptr ? g_runtime->global_definitions() : std::string();
    out->string_value = g_globalDefinitionsText.c_str();
}
void set_global_definitions(ModContext*, void*, const UiControlValue* v) {
    if (g_runtime != nullptr && v->string_value != nullptr) {
        g_runtime->set_global_definitions(v->string_value);
    }
}

void on_reload(ModContext*, void*) {
    if (g_runtime != nullptr) {
        g_runtime->reload();
    }
}

ModResult build_settings_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "Effects");
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = "Run effects";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarActive;
    c.help_rml = "Turns all ReShade effects on or off without changing which techniques are enabled.";
    add_control(left, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Reload effects";
    c.on_pressed = on_reload;
    c.help_rml = "Scans the shader folder again and recompiles every effect.";
    add_control(left, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_STRING;
    c.label = "Global preprocessor definitions";
    c.get = get_global_definitions;
    c.set = set_global_definitions;
    c.help_rml = "NAME=VALUE pairs separated by commas, applied to every effect (as ReShade's global "
                 "preprocessor definitions). Changing them recompiles all effects. The depth "
                 "definitions (RESHADE_DEPTH_INPUT_IS_REVERSED and the rest) are fixed by this mod "
                 "and cannot be overridden: the depth buffer is converted to match them.";
    add_control(left, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = "Depth range";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarDepthDistance;
    c.min = 0;
    c.max = 1000000;
    c.step = 500;
    c.suffix = " units";
    c.help_rml = "The view distance that linearized depth (ReShade.fxh ReShade::GetLinearizedDepth) "
                 "reports as 1.0. 0 uses the camera's far plane, which varies by area. Smaller values "
                 "spread the depth range over nearby scenery, as lowering "
                 "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE does in ReShade.";
    add_control(left, c);

    svc_ui->pane_add_section(mod_ctx, left, "Status");
    g_settingsStatus = 0;
    svc_ui->pane_add_text(mod_ctx, left, status_text().c_str(), &g_settingsStatus);

    svc_ui->pane_add_section(mod_ctx, left, "Folders");
    if (g_runtime != nullptr) {
        const std::string folders = "Shaders: " + rsp::path_utf8(g_runtime->shaders_dir()) +
                                    "\nTextures: " + rsp::path_utf8(g_runtime->textures_dir()) +
                                    "\nPreset: " + rsp::path_utf8(g_runtime->preset_path());
        svc_ui->pane_add_rml(mod_ctx, left, rml_escape(folders).c_str(), nullptr);
    }

    // Effects with problems.
    if (g_runtime != nullptr) {
        std::string problems;
        for (const auto& [file, state] : g_runtime->effects()) {
            if (state.build == nullptr) {
                continue;
            }
            const bool rejected = state.build->gpu_done &&
                std::find(state.build->technique_ok.begin(), state.build->technique_ok.end(), 0) != state.build->technique_ok.end();
            if (!state.build->compile_ok || rejected) {
                std::string m = state.build->messages;
                if (m.size() > 600) {
                    m.resize(600);
                    m += "...";
                }
                problems += "<b>" + rml_escape(file) + "</b><br/>" + rml_escape(m) + "<br/>";
            }
        }
        if (!problems.empty()) {
            svc_ui->pane_add_section(mod_ctx, left, "Effects with errors");
            svc_ui->pane_add_rml(mod_ctx, left, problems.c_str(), nullptr);
        }
    }

    svc_ui->pane_add_rml(mod_ctx, right,
        "Put ReShade effect packages in the Shaders and Textures folders listed on the left, as "
        "ReShade's installer would (subfolders are searched). Effects that need features the game's "
        "graphics device does not provide are listed under errors and skipped; they never stop "
        "the game.",
        nullptr);
    return MOD_OK;
}

ModResult update_settings_tab(ModContext*, void*, ModError*) {
    if (g_settingsStatus != 0) {
        svc_ui->elem_set_text(mod_ctx, g_settingsStatus, status_text().c_str());
    }
    return MOD_OK;
}

// Window and panel ----------------------------------------------------------------------------------

void on_window_closed(ModContext*, UiWindowHandle, void*) {
    g_window = 0;
    g_techniqueList = 0;
    g_selectedText = 0;
    g_settingsStatus = 0;
    g_params.clear();
}

void on_open_window(ModContext*, void*) {
    if (g_window != 0) {
        return;
    }
    UiTabDesc tabs[3] = {UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT};
    tabs[0].title = "Techniques";
    tabs[0].build = build_techniques_tab;
    tabs[0].update = update_techniques_tab;
    tabs[1].title = "Parameters";
    tabs[1].build = build_parameters_tab;
    tabs[2].title = "Settings";
    tabs[2].build = build_settings_tab;
    tabs[2].update = update_settings_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 3;
    desc.on_closed = on_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_window) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open the ReShade Port window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = "Run effects";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarActive;
    add_control(panel, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Open ReShade Port";
    c.on_pressed = on_open_window;
    add_control(panel, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Reload effects";
    c.on_pressed = on_reload;
    add_control(panel, c);

    g_panelStatus = 0;
    svc_ui->pane_add_text(mod_ctx, panel, status_text().c_str(), &g_panelStatus);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_panelStatus != 0) {
        svc_ui->elem_set_text(mod_ctx, g_panelStatus, status_text().c_str());
    }
    return MOD_OK;
}

ModResult register_option(const char* name, ConfigVarType type, bool defaultBool, int64_t defaultInt,
    ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = type;
    d.default_bool = defaultBool;
    d.default_int = defaultInt;
    if (svc_config->register_var(mod_ctx, &d, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register a ReShade Port option");
    }
    return MOD_OK;
}

} // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    if (register_option("effectsActive", CONFIG_VAR_BOOL, true, 0, g_cvarActive, error) != MOD_OK ||
        register_option("depthRange", CONFIG_VAR_INT, false, 0, g_cvarDepthDistance, error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK || g_deviceInfo.instance == nullptr) {
        return mods::set_error(error, MOD_ERROR, "failed to query the graphics device");
    }
    const char* dataDir = nullptr;
    if (svc_host->data_dir(mod_ctx, &dataDir) != MOD_OK || dataDir == nullptr) {
        return mods::set_error(error, MOD_ERROR, "no data folder for the ReShade Port");
    }

    g_runtime = std::make_unique<rsp::Runtime>();
    std::string errors;
    const rsp::GpuApi game{g_deviceInfo.instance, g_deviceInfo.device, g_deviceInfo.queue};
    if (!g_runtime->init(game, rsp::utf8_path(dataDir), errors)) {
        g_runtime.reset();
        return mods::set_error(error, MOD_ERROR, "failed to start the ReShade Port runtime");
    }

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "ReShade port techniques";
    computeDesc.callback = on_compute;
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "ReShade port composite";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_computeType) != MOD_OK ||
        svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register GfxService callbacks");
    }
    const struct {
        GfxStage stage;
        GfxStageFn fn;
    } stages[] = {
        {GFX_STAGE_SCENE_AFTER_OPAQUE, on_after_opaque},
        {GFX_STAGE_FRAME_BEFORE_HUD, on_before_hud},
        {GFX_STAGE_FRAME_AFTER_HUD, on_after_hud},
    };
    for (size_t i = 0; i < std::size(stages); ++i) {
        GfxStageHookDesc d = GFX_STAGE_HOOK_DESC_INIT;
        d.callback = stages[i].fn;
        if (svc_gfx->register_stage_hook(mod_ctx, stages[i].stage, &d, &g_stageHooks[i]) != MOD_OK) {
            return mods::set_error(error, MOD_ERROR, "failed to register a GfxService stage hook");
        }
    }
    g_hookInstalled = rsp::install_after_translucent_hook(on_after_translucent);
    if (!g_hookInstalled) {
        svc_log->warn(mod_ctx,
            "could not hook dComIfGd_drawXluListDark (game build mismatch?): the 'Before particles & "
            "post-processing' insertion point will not run");
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    const std::string ready = "ReShade Port ready; effects folder: " + rsp::path_utf8(g_runtime->shaders_dir());
    svc_log->info(mod_ctx, ready.c_str());
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (g_runtime == nullptr) {
        return MOD_OK;
    }
    uint32_t width = 0, height = 0;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    GfxRenderTargetLayout layout = GFX_RENDER_TARGET_LAYOUT_INIT;
    if (svc_gfx->get_scene_target_layout(mod_ctx, &layout) == MOD_OK && layout.color_attachment_count > 0) {
        const GfxColorAttachmentLayout& color = layout.color_attachments[GFX_SCENE_COLOR_ATTACHMENT_INDEX];
        width = color.width;
        height = color.height;
        format = color.format;
    }
    g_runtime->set_effects_active(get_bool_option(g_cvarActive, true));
    g_runtime->set_depth_distance(static_cast<float>(std::max<int64_t>(0, get_int_option(g_cvarDepthDistance, 0))));
    g_runtime->update(width, height, format);
    for (const std::string& line : g_runtime->take_log()) {
        const bool isError = line.find("error") != std::string::npos;
        (isError ? svc_log->warn : svc_log->info)(mod_ctx, line.c_str());
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_runtime != nullptr) {
        g_runtime->shutdown();
        g_runtime.reset();
    }
    for (auto& [key, c] : g_composites) {
        if (c.pipeline != nullptr) {
            wgpuRenderPipelineRelease(c.pipeline);
        }
    }
    g_composites.clear();
    g_computeType = 0;
    g_drawType = 0;
    for (auto& h : g_stageHooks) {
        h = 0;
    }
    g_hookInstalled = false;
    g_cvarActive = g_cvarDepthDistance = 0;
    g_window = 0;
    g_techniqueList = 0;
    g_selectedText = 0;
    g_settingsStatus = 0;
    g_panelStatus = 0;
    g_params.clear();
    g_selectedTechnique.clear();
    return MOD_OK;
}
}
