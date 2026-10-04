// ReShade Bridge, the ReShade half: a ReShade add-on that runs ReShade's techniques at the points of
// Dusklight's frame where the Dusklight mod (src/mod.cpp) hands the scene over.
//
// How a hand-over works. Dusklight renders with Dawn, which turns each WebGPU command buffer into a
// Direct3D 12 command list when the game submits it. ReShade wraps that command list and reports
// every copy to its add-ons (copy_texture_region). The mod records, at each insertion point, two
// one-texel copies into "marker" textures whose size encodes the point (include/drb_protocol.hpp):
// first from its depth texture, then from its colour texture. When this add-on sees them:
//
//   depth marker   remember the depth resource (bound to ReShade's DEPTH semantic below)
//   colour marker  transition colour to render target and depth to shader resource, run every
//                  enabled technique assigned to this point (ReShade's render_technique, in
//                  ReShade's own technique order) on the colour texture, transition both back, and
//                  restore the Dawn command-list state ReShade's work disturbed (state_restore.cpp)
//
// Both marker copies are skipped (they carry no data). The mod then composites the colour texture
// back into the frame. While the mod's bridge is on, ReShade's normal end-of-frame effect pass is
// suppressed (render_effects with no target, which also updates the timer and frame-count
// uniforms), so each technique runs exactly once per frame, where the user put it.
//
// The insertion point of each technique is chosen in this add-on's overlay tab ("Dusklight") and
// saved in ReShade.ini, section DUSKLIGHT_BRIDGE, key "<technique>@<effect file>".
//
// Threads: the copy and state events run on the thread that submits Dawn's work (Dusklight's render
// worker), present and overlay events on the presenting thread. They share state under g_mutex;
// the present-side handlers only try_lock it, so they can never deadlock against a render_technique
// call in flight. g_viewMutex guards the colour-view cache, which destroy_resource (any thread)
// also touches.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "drb_protocol.hpp"
#include "state_restore.hpp"

#include <unknwn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = "Dusklight Bridge";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Runs ReShade's techniques inside Dusklight's frame, at points chosen per technique (under the HUD, "
    "before the game's post-processing, ...), with the game's depth. Needs the ReShade Bridge mod in Dusklight.";

namespace {

using drb_addon::TrackingPause;

constexpr const char* kSection = "DUSKLIGHT_BRIDGE";

const char* const kPointNames[] = {
    "Before transparency",
    "Before particles & post-processing",
    "Before HUD",
    "After HUD",
};
static_assert(sizeof(kPointNames) / sizeof(kPointNames[0]) == drb::kPointCount);

// --- Shared state with the mod ---------------------------------------------------------------------

HANDLE g_mapping = nullptr;
drb::SharedState* g_shared = nullptr;
drb::SharedState g_localState; // stands in if the shared block cannot be mapped (never read by the mod)

drb::SharedState& shared() { return g_shared != nullptr ? *g_shared : g_localState; }

void open_shared_state() {
    wchar_t name[96];
    std::swprintf(name, 96, L"%ls%lu", drb::kMappingPrefix, static_cast<unsigned long>(GetCurrentProcessId()));
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, drb::kMappingSize, name);
    if (g_mapping == nullptr) {
        return;
    }
    void* view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, drb::kMappingSize);
    if (view != nullptr && drb::claim(static_cast<drb::SharedState*>(view))) {
        g_shared = static_cast<drb::SharedState*>(view);
        return;
    }
    if (view != nullptr) {
        UnmapViewOfFile(view);
    }
    CloseHandle(g_mapping);
    g_mapping = nullptr;
}

void close_shared_state() {
    if (g_shared != nullptr) {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_mapping != nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

// --- Add-on state ----------------------------------------------------------------------------------

std::recursive_mutex g_mutex;
effect_runtime* g_runtime = nullptr; // the D3D12 effect runtime the bridge drives
uint64_t g_presents = 0;             // presents of g_runtime so far
uint64_t g_effectsUpdatedAt = UINT64_MAX; // value of g_presents when render_effects(no target) last ran
uint64_t g_renderedAt = UINT64_MAX;       // value of g_presents when a technique last ran here

// Technique key ("<technique>@<effect file>") -> insertion point; filled from ReShade.ini on demand.
std::unordered_map<std::string, uint32_t> g_points;
uint32_t g_pointTechniqueCount[drb::kPointCount] = {}; // enabled techniques per point, last present

// The depth texture bound to ReShade's DEPTH semantic. The add-on holds a reference on the resource
// while ReShade's descriptor tables may point at it, so it cannot be destroyed under ReShade.
struct DepthBinding {
    device* dev = nullptr;
    resource res = {0};
    resource_view srv = {0};
};
DepthBinding g_depth;
bool g_depthBound = false;     // the binding is applied in the runtime
bool g_rebindInEffects = false; // re-apply it in reshade_begin_effects (Generic Depth is on)
struct RetiredDepth {
    DepthBinding binding;
    uint64_t release_at; // g_presents value
};
std::vector<RetiredDepth> g_retiredDepth;

// The latest depth marker: the command list it came in and its depth resource.
command_list* g_pendingDepthList = nullptr;
resource g_pendingDepth = {0};

// Render target views of the mod's colour textures, by resource.
struct ColorViews {
    device* dev = nullptr;
    resource_view rtv = {0};
    resource_view rtv_srgb = {0};
};
std::mutex g_viewMutex;
std::unordered_map<uint64_t, ColorViews> g_colorViews;

// Settings checks, refreshed now and then on the presenting thread.
bool g_genericDepth = false;
bool g_definitionsOk = false;
std::string g_definitionsNote;
bool g_loggedPoint[drb::kPointCount] = {};

thread_local bool t_inBridge = false;

// --- Helpers ---------------------------------------------------------------------------------------

std::string technique_key(effect_runtime* rt, effect_technique t) {
    char name[256] = "";
    char effect[256] = "";
    size_t n = sizeof(name);
    rt->get_technique_name(t, name, &n);
    n = sizeof(effect);
    rt->get_technique_effect_name(t, effect, &n);
    return std::string(name) + "@" + effect;
}

uint32_t point_of(effect_runtime* rt, const std::string& key) {
    if (const auto it = g_points.find(key); it != g_points.end()) {
        return it->second;
    }
    int value = static_cast<int>(drb::kDefaultPoint);
    if (!reshade::get_config_value(rt, kSection, key.c_str(), value) || value < 0 || value >= static_cast<int>(drb::kPointCount)) {
        value = static_cast<int>(drb::kDefaultPoint);
    }
    g_points[key] = static_cast<uint32_t>(value);
    return static_cast<uint32_t>(value);
}

void set_point(effect_runtime* rt, const std::string& key, uint32_t point) {
    g_points[key] = point;
    reshade::set_config_value(rt, kSection, key.c_str(), static_cast<int>(point));
}

// Enabled techniques at `point`, in ReShade's order (empty while effects are loading).
std::vector<effect_technique> techniques_at(effect_runtime* rt, uint32_t point) {
    struct Ctx {
        uint32_t point;
        std::vector<effect_technique> out;
    } ctx{point, {}};
    rt->enumerate_techniques(nullptr, [](effect_runtime* r, effect_technique t, void* user) {
        auto& c = *static_cast<Ctx*>(user);
        if (r->get_technique_state(t) && point_of(r, technique_key(r, t)) == c.point) {
            c.out.push_back(t);
        }
    }, &ctx);
    return std::move(ctx.out);
}

bool engaged() {
    const drb::SharedState& s = shared();
    return g_runtime != nullptr && s.mod_attached.load() != 0 && s.bridge_active.load() != 0;
}

void retire_depth(DepthBinding& b) {
    if (b.res.handle != 0) {
        g_retiredDepth.push_back({b, g_presents + 4});
    }
    b = DepthBinding{};
}

void release_depth(DepthBinding& b) {
    if (b.srv.handle != 0 && b.dev != nullptr) {
        b.dev->destroy_resource_view(b.srv);
    }
    if (b.res.handle != 0) {
        reinterpret_cast<IUnknown*>(b.res.handle)->Release();
    }
    b = DepthBinding{};
}

void unbind_depth() {
    if (g_runtime != nullptr && g_depthBound) {
        g_runtime->update_texture_bindings("DEPTH", resource_view{0}, resource_view{0});
    }
    g_depthBound = false;
    retire_depth(g_depth);
}

// Makes `res` the bound DEPTH texture (creating its view on first use).
void bind_depth(device* dev, resource res) {
    if (res.handle != g_depth.res.handle) {
        const resource_desc desc = dev->get_resource_desc(res);
        format f = desc.texture.format;
        if (format_to_typeless(f) == f) {
            f = format_to_default_typed(f);
        }
        resource_view srv = {0};
        if (!dev->create_resource_view(res, resource_usage::shader_resource, resource_view_desc(f), &srv)) {
            return;
        }
        reinterpret_cast<IUnknown*>(res.handle)->AddRef();
        retire_depth(g_depth);
        g_depth = DepthBinding{dev, res, srv};
        g_depthBound = false;
    }
    if (!g_depthBound && g_depth.srv.handle != 0) {
        g_runtime->update_texture_bindings("DEPTH", g_depth.srv, g_depth.srv);
        g_depthBound = true;
    }
}

bool color_views(device* dev, resource res, ColorViews& out) {
    std::lock_guard lock(g_viewMutex);
    if (const auto it = g_colorViews.find(res.handle); it != g_colorViews.end()) {
        out = it->second;
        return true;
    }
    const resource_desc desc = dev->get_resource_desc(res);
    if (desc.type != resource_type::texture_2d || desc.texture.samples != 1) {
        return false;
    }
    format linear = desc.texture.format;
    format srgb = linear;
    if (format_to_typeless(linear) == linear) {
        linear = format_to_default_typed(desc.texture.format, 0);
        srgb = format_to_default_typed(desc.texture.format, 1);
    }
    ColorViews v;
    v.dev = dev;
    if (!dev->create_resource_view(res, resource_usage::render_target, resource_view_desc(linear), &v.rtv)) {
        return false;
    }
    if (srgb == linear || !dev->create_resource_view(res, resource_usage::render_target, resource_view_desc(srgb), &v.rtv_srgb)) {
        v.rtv_srgb = v.rtv;
    }
    g_colorViews[res.handle] = v;
    out = v;
    return true;
}

void destroy_color_views(const ColorViews& v) {
    if (v.dev == nullptr) {
        return;
    }
    if (v.rtv_srgb.handle != 0 && v.rtv_srgb.handle != v.rtv.handle) {
        v.dev->destroy_resource_view(v.rtv_srgb);
    }
    if (v.rtv.handle != 0) {
        v.dev->destroy_resource_view(v.rtv);
    }
}

void clear_color_views(device* dev) {
    std::lock_guard lock(g_viewMutex);
    for (auto it = g_colorViews.begin(); it != g_colorViews.end();) {
        if (dev == nullptr || it->second.dev == dev) {
            destroy_color_views(it->second);
            it = g_colorViews.erase(it);
        } else {
            ++it;
        }
    }
}

// The RESHADE_DEPTH_* definitions the mod's depth encoding needs, set at global scope (saved in
// ReShade.ini, every preset) and, where the current preset overrides one, in the preset too.
void apply_depth_definitions(effect_runtime* rt) {
    bool ok = true;
    std::string note;
    for (const drb::DepthDefinition& d : drb::kDepthDefinitions) {
        char value[64] = "";
        size_t size = sizeof(value);
        if (!rt->get_preprocessor_definition_for_effect("GLOBAL", d.name, value, &size) || std::strcmp(value, d.value) != 0) {
            rt->set_preprocessor_definition_for_effect("GLOBAL", d.name, d.value);
        }
        size = sizeof(value);
        value[0] = '\0';
        if (rt->get_preprocessor_definition(d.name, value, &size) && std::strcmp(value, d.value) != 0) {
            rt->set_preprocessor_definition(d.name, d.value);
            size = sizeof(value);
            value[0] = '\0';
            if (rt->get_preprocessor_definition(d.name, value, &size) && std::strcmp(value, d.value) != 0) {
                ok = false;
                note += std::string(d.name) + " is " + value + " (needs " + d.value + ")\n";
            }
        }
    }
    g_definitionsOk = ok;
    g_definitionsNote = note;
    shared().depth_definitions_ok.store(ok ? 1u : 0u);
}

bool generic_depth_enabled() {
    char list[2048] = {};
    size_t size = sizeof(list);
    if (!reshade::get_config_value(nullptr, "ADDON", "DisabledAddons", list, &size)) {
        return true; // nothing disabled
    }
    // '\0'-separated list
    for (size_t i = 0; i < size && i < sizeof(list);) {
        const char* item = list + i;
        const size_t len = strnlen(item, sizeof(list) - i);
        if (std::strcmp(item, "Generic Depth") == 0) {
            return false;
        }
        i += len + 1;
    }
    return true;
}

void log_info(const std::string& message) { reshade::log::message(reshade::log::level::info, message.c_str()); }

// --- The hand-over (Dawn's submitting thread) ------------------------------------------------------

void run_point(command_list* cmd_list, uint32_t point, resource color) {
    effect_runtime* rt = g_runtime;
    device* dev = cmd_list->get_device();
    if (rt == nullptr || dev != rt->get_device() || !rt->get_effects_state()) {
        return;
    }
    const std::vector<effect_technique> techniques = techniques_at(rt, point);
    if (techniques.empty()) {
        return;
    }
    ColorViews views;
    if (!color_views(dev, color, views)) {
        return;
    }
    // Depth only from a depth marker just before this colour marker in the same command list: then
    // Dawn has just left it in the copy-source state.
    const bool haveDepth = g_pendingDepthList == cmd_list && g_pendingDepth.handle != 0;
    if (haveDepth) {
        bind_depth(dev, g_pendingDepth);
    }
    g_pendingDepthList = nullptr;
    g_pendingDepth = {0};

    // Once per frame: ReShade's per-frame uniform updates (timer, frame count, ...), and the flag
    // that skips ReShade's own effect pass at present.
    if (g_effectsUpdatedAt != g_presents) {
        rt->render_effects(cmd_list, resource_view{0}, resource_view{0});
        g_effectsUpdatedAt = g_presents;
    }

    const resource resources[2] = {color, g_depth.res};
    const resource_usage before[2] = {resource_usage::copy_source, resource_usage::copy_source};
    const resource_usage during[2] = {resource_usage::render_target, resource_usage::shader_resource};
    const uint32_t barrierCount = haveDepth && g_depth.res.handle != 0 ? 2u : 1u;

    TrackingPause pause;
    t_inBridge = true;
    g_rebindInEffects = g_genericDepth && g_depth.srv.handle != 0;
    cmd_list->barrier(barrierCount, resources, before, during);
    for (const effect_technique t : techniques) {
        rt->render_technique(t, cmd_list, views.rtv, views.rtv_srgb);
    }
    cmd_list->barrier(barrierCount, resources, during, before);
    t_inBridge = false;
    drb_addon::restore_state(cmd_list);

    shared().techniques_run[point].fetch_add(techniques.size(), std::memory_order_relaxed);
    g_renderedAt = g_presents;
    if (!g_loggedPoint[point]) {
        g_loggedPoint[point] = true;
        log_info(std::string("Dusklight bridge: first techniques run at '") + kPointNames[point] + "'");
    }
}

// --- Events ----------------------------------------------------------------------------------------

bool on_copy_texture_region(command_list* cmd_list, resource source, uint32_t, const subresource_box*, resource dest,
    uint32_t, const subresource_box*, filter_mode) {
    if (t_inBridge || g_runtime == nullptr || source.handle == 0 || dest.handle == 0) {
        return false;
    }
    device* dev = cmd_list->get_device();
    if (dev->get_api() != device_api::d3d12) {
        return false;
    }
    const resource_desc desc = dev->get_resource_desc(dest);
    if (desc.type != resource_type::texture_2d || desc.texture.width != drb::kMarkerWidth) {
        return false;
    }
    const uint32_t h = desc.texture.height;
    const bool isColor = h >= drb::kColorMarkerHeightBase && h < drb::kColorMarkerHeightBase + drb::kPointCount;
    const bool isDepth = h >= drb::kDepthMarkerHeightBase && h < drb::kDepthMarkerHeightBase + drb::kPointCount;
    if (!isColor && !isDepth) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (isDepth) {
        const uint32_t point = h - drb::kDepthMarkerHeightBase;
        shared().depth_markers_seen[point].fetch_add(1, std::memory_order_relaxed);
        g_pendingDepthList = cmd_list;
        g_pendingDepth = source;
        return true;
    }
    const uint32_t point = h - drb::kColorMarkerHeightBase;
    shared().markers_seen[point].fetch_add(1, std::memory_order_relaxed);
    run_point(cmd_list, point, source);
    return true;
}

// ReShade calls this inside every render_technique. With Generic Depth on, it may have just bound
// its own guess at the game's depth; put the mod's depth back for the bridge's techniques.
void on_begin_effects(effect_runtime* rt, command_list*, resource_view, resource_view) {
    if (t_inBridge && g_rebindInEffects && rt == g_runtime && g_depth.srv.handle != 0) {
        rt->update_texture_bindings("DEPTH", g_depth.srv, g_depth.srv);
    }
}

// Before ReShade's own effect pass: while the bridge is on, skip it (the techniques have run, or
// will run, at their insertion points).
void on_present(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock || !engaged() || g_effectsUpdatedAt == g_presents) {
        return;
    }
    g_runtime->render_effects(g_runtime->get_command_queue()->get_immediate_command_list(), resource_view{0}, resource_view{0});
    g_effectsUpdatedAt = g_presents;
}

// After ReShade's present: publish state for the mod, release old depth textures, refresh checks.
void on_reshade_present(effect_runtime* rt) {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock || rt != g_runtime) {
        return;
    }
    drb::SharedState& s = shared();
    // No technique ran here this frame: stop lending ReShade the mod's depth, so the end-of-frame
    // pass never samples a texture the mod may release.
    if (g_renderedAt != g_presents && g_depth.res.handle != 0) {
        unbind_depth();
    }
    ++g_presents;
    s.present_count.store(g_presents, std::memory_order_relaxed);
    s.effects_enabled.store(rt->get_effects_state() ? 1u : 0u);

    uint32_t mask = 0;
    for (uint32_t p = 0; p < drb::kPointCount; ++p) {
        g_pointTechniqueCount[p] = static_cast<uint32_t>(techniques_at(rt, p).size());
        mask |= g_pointTechniqueCount[p] != 0 ? (1u << p) : 0u;
    }
    s.points_mask.store(mask);

    for (auto it = g_retiredDepth.begin(); it != g_retiredDepth.end();) {
        if (it->release_at <= g_presents) {
            release_depth(it->binding);
            it = g_retiredDepth.erase(it);
        } else {
            ++it;
        }
    }
    if (g_presents % 300 == 1) {
        g_genericDepth = generic_depth_enabled();
        s.generic_depth_enabled.store(g_genericDepth ? 1u : 0u);
    }
}

void on_init_effect_runtime(effect_runtime* rt) {
    std::lock_guard lock(g_mutex);
    if (g_runtime != nullptr) {
        return;
    }
    if (rt->get_device()->get_api() != device_api::d3d12) {
        shared().blocked.store(drb::kNotD3D12);
        log_info("Dusklight bridge: this ReShade instance is not on Direct3D 12; the bridge stays idle");
        return;
    }
    g_runtime = rt;
    g_presents = 0;
    g_effectsUpdatedAt = g_renderedAt = UINT64_MAX;
    g_points.clear();
    g_genericDepth = generic_depth_enabled();
    shared().generic_depth_enabled.store(g_genericDepth ? 1u : 0u);
    shared().blocked.store(drb::kNotBlocked);
    apply_depth_definitions(rt);
    shared().runtime_ready.store(1);
    log_info("Dusklight bridge: attached to the Direct3D 12 effect runtime");
}

void on_destroy_effect_runtime(effect_runtime* rt) {
    std::lock_guard lock(g_mutex);
    if (rt != g_runtime) {
        return;
    }
    shared().runtime_ready.store(0);
    shared().points_mask.store(0);
    g_depthBound = false; // the runtime's tables go with it
    if (g_depth.res.handle != 0) {
        g_retiredDepth.push_back({g_depth, 0});
        g_depth = DepthBinding{};
    }
    for (RetiredDepth& r : g_retiredDepth) {
        release_depth(r.binding);
    }
    g_retiredDepth.clear();
    g_pendingDepthList = nullptr;
    g_pendingDepth = {0};
    g_runtime = nullptr;
}

void on_reloaded_effects(effect_runtime* rt) {
    std::lock_guard lock(g_mutex);
    if (rt != g_runtime) {
        return;
    }
    g_depthBound = false; // new effects, new descriptor tables
    apply_depth_definitions(rt);
}

void on_destroy_resource(device*, resource res) {
    std::lock_guard lock(g_viewMutex);
    if (const auto it = g_colorViews.find(res.handle); it != g_colorViews.end()) {
        destroy_color_views(it->second);
        g_colorViews.erase(it);
    }
}

void on_destroy_device(device* dev) { clear_color_views(dev); }

// --- Overlay ---------------------------------------------------------------------------------------

bool g_showDisabled = false;

void draw_status(effect_runtime* rt) {
    const drb::SharedState& s = shared();
    const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
    if (rt != g_runtime) {
        ImGui::TextWrapped("This ReShade instance is not the one the bridge drives (it needs Direct3D 12).");
        return;
    }
    if (s.mod_attached.load() == 0) {
        ImGui::TextColored(warn, "The ReShade Bridge mod is not running in Dusklight.");
        ImGui::TextWrapped("Install reshade_bridge.dusk in Dusklight's mod manager and enable it. Until then "
                           "ReShade runs as usual, over the finished frame.");
    } else if (s.bridge_active.load() == 0) {
        ImGui::TextWrapped("The bridge is switched off in Dusklight (Mods > ReShade Bridge). ReShade runs as "
                           "usual, over the finished frame.");
    } else {
        ImGui::TextWrapped("Connected to Dusklight. Each technique runs at the point chosen below instead of "
                           "over the finished frame.");
    }
    if (g_genericDepth) {
        ImGui::TextColored(warn, "Turn off the Generic Depth add-on (Add-ons tab).");
        ImGui::TextWrapped("The bridge gives effects the game's depth itself; Generic Depth's guess is not "
                           "needed and costs time on every draw call.");
    }
    if (!g_definitionsOk) {
        ImGui::TextColored(warn, "Depth settings could not be applied:");
        ImGui::TextWrapped("%s", g_definitionsNote.c_str());
    }
    if (ImGui::CollapsingHeader("Hand-over statistics", ImGuiTreeNodeFlags_None)) {
        for (uint32_t p = 0; p < drb::kPointCount; ++p) {
            ImGui::Text("%s: %u technique(s); %llu frames sent, %llu received, %llu technique runs", kPointNames[p],
                g_pointTechniqueCount[p], static_cast<unsigned long long>(s.markers_recorded[p].load()),
                static_cast<unsigned long long>(s.markers_seen[p].load()),
                static_cast<unsigned long long>(s.techniques_run[p].load()));
        }
        ImGui::TextWrapped("Sent but not received: ReShade is not reporting Dusklight's copies (a ReShade build "
                           "without full add-on support cannot load this add-on at all).");
    }
}

void draw_techniques(effect_runtime* rt) {
    ImGui::Checkbox("Show disabled techniques", &g_showDisabled);
    struct Row {
        effect_technique t;
        std::string key;
        bool enabled;
    };
    struct Ctx {
        std::vector<Row> rows;
    } ctx;
    rt->enumerate_techniques(nullptr, [](effect_runtime* r, effect_technique t, void* user) {
        static_cast<Ctx*>(user)->rows.push_back({t, technique_key(r, t), r->get_technique_state(t)});
    }, &ctx);
    if (ctx.rows.empty()) {
        ImGui::TextDisabled("No techniques loaded yet.");
        return;
    }
    if (!ImGui::BeginTable("dusklight_points", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0), 0.0f)) {
        return;
    }
    ImGui::TableSetupColumn("Technique", ImGuiTableColumnFlags_WidthStretch, 0.55f, 0);
    ImGui::TableSetupColumn("Runs at", ImGuiTableColumnFlags_WidthStretch, 0.45f, 0);
    ImGui::TableHeadersRow();
    for (const Row& row : ctx.rows) {
        if (!row.enabled && !g_showDisabled) {
            continue;
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 0.0f);
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (row.enabled) {
            ImGui::TextUnformatted(row.key.c_str());
        } else {
            ImGui::TextDisabled("%s", row.key.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        int point = static_cast<int>(point_of(rt, row.key));
        ImGui::PushID(row.key.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##point", &point, kPointNames, static_cast<int>(drb::kPointCount), -1)) {
            set_point(rt, row.key, static_cast<uint32_t>(point));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_overlay(effect_runtime* rt) {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock) {
        ImGui::TextDisabled("Busy...");
        return;
    }
    draw_status(rt);
    if (rt != g_runtime) {
        return;
    }
    ImGui::Separator();
    ImGui::TextWrapped("Enable techniques on the Home tab as usual; at each point they run in the Home tab's order.");
    ImGui::TextWrapped("Before transparency: the opaque world only (water, glass, particles and the game's "
                       "post-processing come later) - the place for ambient occlusion. Before particles & "
                       "post-processing: the whole world. Before HUD: after the game's own post-processing. "
                       "After HUD: the finished frame.");
    ImGui::Separator();
    draw_techniques(rt);
}

void register_events() {
    drb_addon::register_state_tracking();
    reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_overlay("Dusklight", draw_overlay);
}

void unregister_events() {
    reshade::unregister_overlay("Dusklight", draw_overlay);
    reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::unregister_event<reshade::addon_event::present>(on_present);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    drb_addon::unregister_state_tracking();
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) {
            return FALSE;
        }
        open_shared_state();
        register_events();
        shared().addon_loaded.store(1);
        break;
    case DLL_PROCESS_DETACH:
        unregister_events();
        reshade::unregister_addon(module);
        shared().runtime_ready.store(0);
        shared().points_mask.store(0);
        shared().addon_loaded.store(0);
        close_shared_state();
        break;
    }
    return TRUE;
}
