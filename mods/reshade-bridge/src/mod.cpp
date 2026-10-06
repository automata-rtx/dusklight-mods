// ReShade Bridge, the Dusklight half: hands the scene to an installed ReShade at four points of the
// frame and puts ReShade's result back.
//
//   Before transparency           GFX_STAGE_SCENE_AFTER_OPAQUE
//   Before particles & post-FX    post-hook on dComIfGd_drawXluListDark (game_hooks.cpp)
//   Before HUD                    GFX_STAGE_FRAME_BEFORE_HUD
//   After HUD                     GFX_STAGE_FRAME_AFTER_HUD
//
// The other half is a ReShade add-on (addon/bridge_addon.cpp). The add-on says which points have
// enabled techniques (SharedState::points_mask) and how big ReShade's screen is (screen_width/
// height); at those points this mod snapshots colour and depth (resolve_pass), and a compute task
// scales them to ReShade's screen size into textures of its own and records two one-texel
// "marker" copies (bridge_gpu.cpp). When Dawn turns the frame into a Direct3D 12 command list, the
// add-on recognises the marker copies and runs the point's techniques on the hand-over texture
// right there. A draw then scales ReShade's change back up to the game's internal resolution and
// adds it to the scene, RGB only. ReShade's effects only work on frames of their screen's size
// (drb_protocol.hpp, "Sizes"), whatever the game renders at.
//
// This mod never talks to ReShade and the add-on never touches WebGPU: if either half is missing,
// the other does nothing (the mod composites an unchanged copy only while the add-on asks for it).
//
// Threads: stage callbacks, the game hook, mod_update and UI callbacks run on the game thread; the
// compute and draw callbacks run on the render worker and only use their payload and objects that
// outlive the frame (BridgeGpu keeps replaced textures for a few frames).
//
// Design notes and limitations: docs/reshade_bridge.md.

#include "bridge_gpu.hpp"
#include "bridge_link.hpp"
#include "game_hooks.hpp"

#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);

namespace {

using rsb::BridgeGpu;

BridgeGpu g_gpu;
bool g_gpuReady = false;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
GfxComputeTypeHandle g_computeType = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_stageHooks[3] = {};
bool g_hookInstalled = false;
drb::SharedState* g_shared = nullptr;

// Config vars ("enabled" is reserved by the loader).
ConfigVarHandle g_cvarActive = 0;
ConfigVarHandle g_cvarDepthRange = 0;
ConfigVarHandle g_cvarSpread = 0;    // rsb::SpreadMode
ConfigVarHandle g_cvarDebugView = 0; // 0 off, 1 + point: that point's change layer

UiElementHandle g_panelStatus = 0;

// Per frame (game thread). g_want: bit p = run point p. Reset in mod_update.
uint32_t g_want = 0;
bool g_depthThisFrame = false;
struct CameraDepth {
    float proj_from_view[16] = {};
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    bool valid = false;
} g_camera;

// The composites queued this frame, kept for the change-layer debug view (drawn at the end of the
// frame from the same textures). Valid until the next mod_update.
struct Composite {
    rsb::CompositePayload payload;
    uint32_t screen[2];
    rsb::SpreadMode mode;
};
Composite g_composites[drb::kPointCount] = {};
uint32_t g_composited = 0; // bit p: point p composited this frame

// Diagnostics (game thread).
uint64_t g_recorded[drb::kPointCount] = {};
uint32_t g_lastFrameSize[2] = {};
uint32_t g_lastHandoverSize[2] = {};
bool g_lastDebugDrawn = false;
bool g_debugDrawn = false;
bool g_warnedFormat[drb::kPointCount] = {};

const char* const kPointNames[] = {
    "Before transparency",
    "Before particles & post-processing",
    "Before HUD",
    "After HUD",
};
static_assert(std::size(kPointNames) == drb::kPointCount);

// Option lists, in config value order.
const char* kSpreadOptions[] = {"Edge-aware", "Simple"};
const char* kDebugViewOptions[] = {
    "Off",
    "Change layer: Before transparency",
    "Change layer: Before particles & post-processing",
    "Change layer: Before HUD",
    "Change layer: After HUD",
};
static_assert(std::size(kDebugViewOptions) == 1 + drb::kPointCount);

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

// --- Render worker --------------------------------------------------------------------------------

void on_compute(ModContext*, const GfxComputeContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(rsb::RecordPayload) || ctx == nullptr) {
        return;
    }
    rsb::RecordPayload p;
    std::memcpy(&p, payload, sizeof(p));
    g_gpu.record_point(*ctx, p);
}

void on_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(rsb::CompositePayload) || ctx == nullptr) {
        return;
    }
    rsb::CompositePayload p;
    std::memcpy(&p, payload, sizeof(p));
    g_gpu.composite(*ctx, p);
}

// --- Game thread: insertion points ----------------------------------------------------------------

// The depth parameters for this frame's camera, pushed to the frame's uniform buffer.
bool push_depth_params(uint32_t& offset) {
    rsb::DepthParams dp{};
    if (g_camera.valid) {
        const float* m = g_camera.proj_from_view;
        dp.a = m[10];
        dp.b = m[14];
        dp.c = m[11];
        dp.d = m[15];
        dp.near_plane = g_camera.near_plane;
        const int64_t range = std::max<int64_t>(0, get_int_option(g_cvarDepthRange, 0));
        dp.range = range > 0 ? static_cast<float>(range) : g_camera.far_plane;
        dp.valid = 1.0f;
    }
    GfxRange r{};
    if (svc_gfx->push_uniform(mod_ctx, &dp, sizeof(dp), &r) != MOD_OK) {
        return false;
    }
    offset = static_cast<uint32_t>(r.offset);
    return true;
}

// ReShade's screen size as the add-on reports it; the frame's own size until it does.
void screen_size(uint32_t frameW, uint32_t frameH, uint32_t& w, uint32_t& h) {
    w = g_shared != nullptr ? g_shared->screen_width.load(std::memory_order_relaxed) : 0u;
    h = g_shared != nullptr ? g_shared->screen_height.load(std::memory_order_relaxed) : 0u;
    if (w == 0 || h == 0) {
        w = frameW;
        h = frameH;
    }
}

rsb::SpreadMode spread_option() {
    return get_int_option(g_cvarSpread, rsb::kSpreadEdgeAware) == rsb::kSpreadSimple ? rsb::kSpreadSimple : rsb::kSpreadEdgeAware;
}

// The frame's place on the screen, pushed to the frame's uniform buffer.
bool push_map_params(uint32_t screenW, uint32_t screenH, uint32_t frameW, uint32_t frameH, rsb::SpreadMode mode,
    bool debug, uint32_t& offset) {
    const rsb::MapParams mp = rsb::map_params(screenW, screenH, frameW, frameH, mode, debug);
    GfxRange r{};
    if (svc_gfx->push_uniform(mod_ctx, &mp, sizeof(mp), &r) != MOD_OK) {
        return false;
    }
    offset = static_cast<uint32_t>(r.offset);
    return true;
}

// Runs one insertion point: `color` hands the scene to the add-on's techniques, `convertDepth`
// refreshes the shared depth textures from this point's depth.
void run_point(uint32_t point, bool color, bool convertDepth) {
    if (!g_gpuReady || (!color && !convertDepth)) {
        return;
    }
    GfxResolveDesc rd = GFX_RESOLVE_DESC_INIT;
    rd.color = true;
    rd.depth = convertDepth;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &rd, &resolved) != MOD_OK || resolved.width == 0 || resolved.height == 0) {
        return;
    }
    uint32_t screenW = 0, screenH = 0;
    screen_size(resolved.width, resolved.height, screenW, screenH);

    rsb::RecordPayload p{};
    if (convertDepth) {
        rsb::DepthTarget* depth = resolved.depth != nullptr
                                      ? g_gpu.ensure_depth(resolved.width, resolved.height, screenW, screenH)
                                      : nullptr;
        uint32_t offset = 0;
        if (depth != nullptr && push_depth_params(offset)) {
            p.flags |= rsb::kRecordConvertDepth;
            p.src_depth = resolved.depth;
            p.depth_uniform_offset = offset;
            g_depthThisFrame = true;
        } else if (!g_depthThisFrame) {
            // No depth to give this frame: "far" everywhere rather than an old frame's depth.
            p.flags |= rsb::kRecordClearDepth;
        }
    }
    // Depth made for another screen size (the window changed and no point has converted since)
    // would reach ReShade at the wrong size: not used.
    rsb::DepthTarget* depth = g_gpu.depth();
    if (depth != nullptr && (depth->screen_width != screenW || depth->screen_height != screenH)) {
        depth = nullptr;
    }
    if (depth != nullptr) {
        p.depth_screen = depth->screen;
        p.depth_full_view = depth->full_view;
        p.depth_screen_view = depth->screen_view;
    }

    rsb::PointTargets* t = nullptr;
    if (color && resolved.color != nullptr) {
        t = g_gpu.ensure_point(point, screenW, screenH, resolved.color_format);
        if (t == nullptr && !g_warnedFormat[point]) {
            g_warnedFormat[point] = true;
            const std::string m = std::string("ReShade Bridge: cannot hand over the frame at '") + kPointNames[point] +
                                  "' (colour format " + std::to_string(static_cast<uint32_t>(resolved.color_format)) +
                                  ", " + std::to_string(screenW) + "x" + std::to_string(screenH) + ")";
            svc_log->warn(mod_ctx, m.c_str());
        }
    }
    if (t != nullptr) {
        p.flags |= rsb::kRecordColor;
        p.src_color = resolved.color;
        p.color = t->color;
        p.color_view = t->color_view;
        p.input = t->input;
        p.color_marker = t->color_marker;
        p.depth_marker = t->depth_marker;
        p.color_format = t->format;
        g_lastFrameSize[0] = resolved.width;
        g_lastFrameSize[1] = resolved.height;
        g_lastHandoverSize[0] = t->width;
        g_lastHandoverSize[1] = t->height;
    }
    // The edge-aware scale-up needs this frame's depth; without it, the simple one.
    const bool depthThisFrame = depth != nullptr && g_depthThisFrame;
    const rsb::SpreadMode mode = depthThisFrame ? spread_option() : rsb::kSpreadSimple;
    uint32_t mapOffset = 0;
    if (p.flags == 0 || !push_map_params(screenW, screenH, resolved.width, resolved.height, mode, false, mapOffset)) {
        return;
    }
    p.map_uniform_offset = mapOffset;
    if (svc_gfx->push_compute(mod_ctx, g_computeType, &p, sizeof(p)) != MOD_OK || t == nullptr) {
        return;
    }
    ++g_recorded[point];
    if (g_shared != nullptr) {
        g_shared->markers_recorded[point].fetch_add(1, std::memory_order_relaxed);
    }
    rsb::CompositePayload cp{};
    cp.frame = resolved.color;
    cp.result = t->color_view;
    cp.input = t->input_view;
    cp.depth_full = depthThisFrame ? depth->full_view : nullptr;
    cp.depth_screen = depthThisFrame ? depth->screen_view : nullptr;
    cp.uniform_offset = mapOffset;
    if (svc_gfx->push_draw(mod_ctx, g_drawType, &cp, sizeof(cp)) == MOD_OK) {
        g_composites[point] = Composite{cp, {screenW, screenH}, mode};
        g_composited |= 1u << point;
    }
}

// The change-layer debug view: the chosen point's change, scaled up exactly as its composite did
// (same textures, same spread), drawn over the finished frame on mid-grey so nothing drawn after
// the point (water, the game's post-processing, the HUD) hides it.
void draw_debug_view() {
    const int64_t view = get_int_option(g_cvarDebugView, 0);
    if (view < 1 || view > static_cast<int64_t>(drb::kPointCount)) {
        return;
    }
    const uint32_t point = static_cast<uint32_t>(view - 1);
    if (((g_composited >> point) & 1u) == 0) {
        return;
    }
    GfxResolveDesc rd = GFX_RESOLVE_DESC_INIT;
    rd.color = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &rd, &resolved) != MOD_OK || resolved.color == nullptr) {
        return;
    }
    const Composite& c = g_composites[point];
    uint32_t offset = 0;
    if (!push_map_params(c.screen[0], c.screen[1], resolved.width, resolved.height, c.mode, true, offset)) {
        return;
    }
    rsb::CompositePayload cp = c.payload;
    cp.frame = resolved.color;
    cp.uniform_offset = offset;
    if (svc_gfx->push_draw(mod_ctx, g_drawType, &cp, sizeof(cp)) == MOD_OK) {
        g_debugDrawn = true;
    }
}

bool want(uint32_t point) { return (g_want >> point) & 1u; }

void on_after_opaque(ModContext*, const GfxStageContext* stage, void*) {
    if (stage != nullptr && stage->game_view != nullptr) {
        CameraInfo camera = CAMERA_INFO_INIT;
        if (svc_camera->get_camera(mod_ctx, stage->game_view, &camera) == MOD_OK) {
            std::memcpy(g_camera.proj_from_view, camera.proj_from_view, sizeof(g_camera.proj_from_view));
            g_camera.near_plane = camera.near_plane;
            g_camera.far_plane = camera.far_plane;
            g_camera.valid = true;
        }
    }
    run_point(drb::kBeforeTransparency, want(drb::kBeforeTransparency), want(drb::kBeforeTransparency));
}

// The world is complete here: later points reuse this depth rather than the post-processed frame's.
void on_after_translucent() {
    const bool later = (g_want & ~3u) != 0;
    run_point(drb::kBeforeParticles, want(drb::kBeforeParticles), want(drb::kBeforeParticles) || later);
}

void on_before_hud(ModContext*, const GfxStageContext*, void*) {
    run_point(drb::kBeforeHud, want(drb::kBeforeHud), want(drb::kBeforeHud) && !g_depthThisFrame);
}

void on_after_hud(ModContext*, const GfxStageContext*, void*) {
    run_point(drb::kAfterHud, want(drb::kAfterHud), want(drb::kAfterHud) && !g_depthThisFrame);
    draw_debug_view();
}

// --- UI -------------------------------------------------------------------------------------------

std::string status_text() {
#ifndef _WIN32
    return "The bridge needs ReShade, which runs only on Windows (Direct3D 12).";
#else
    if (g_shared == nullptr) {
        return rsb::shared_state_mismatch()
                   ? std::string("The ReShade add-on is from a different build of the bridge. Use ") +
                         drb::kAddonFileName + " and reshade_bridge.dusk from the same download."
                   : "Could not open the bridge's shared memory; the ReShade add-on cannot be reached.";
    }
    const drb::SharedState& s = *g_shared;
    if (s.addon_loaded.load() == 0) {
        return std::string("ReShade add-on not loaded. Install ReShade with full add-on support for "
                           "Direct3D 12 and put ") + drb::kAddonFileName + " next to ReShade's DLL.";
    }
    if (s.blocked.load() == drb::kNotD3D12) {
        return "ReShade is not running on Direct3D 12. Set the graphics backend to D3D12 (or Auto).";
    }
    if (s.runtime_ready.load() == 0) {
        return "ReShade add-on loaded; waiting for ReShade to start.";
    }
    std::string t;
    if (!get_bool_option(g_cvarActive, true)) {
        t = "Bridge off.";
    } else if (s.effects_enabled.load() == 0) {
        t = "ReShade's effects are toggled off.";
    } else if (s.points_mask.load() == 0) {
        t = "No ReShade technique is enabled.";
    } else {
        t = "Running.";
        const auto size = [](const uint32_t s[2]) { return std::to_string(s[0]) + "x" + std::to_string(s[1]); };
        const bool scaled = g_lastFrameSize[0] != g_lastHandoverSize[0] || g_lastFrameSize[1] != g_lastHandoverSize[1];
        if (g_lastHandoverSize[0] != 0 && !scaled) {
            t += " The game renders at ReShade's screen size (" + size(g_lastHandoverSize) + "): no scaling.";
        } else if (g_lastHandoverSize[0] != 0) {
            t += " The game renders at " + size(g_lastFrameSize) + "; ReShade gets the frame at its screen size, " +
                 size(g_lastHandoverSize) + ", and its change is scaled back up (" +
                 kSpreadOptions[spread_option()] + ").";
        }
        if (s.screen_width.load() == 0) {
            t += " ReShade has not reported its screen size yet.";
        }
    }
    // Per point: frames recorded here / seen by the add-on / techniques the add-on ran. Recorded but
    // never seen means ReShade does not report the copies (not a build with full add-on support?).
    const uint32_t mask = s.points_mask.load();
    for (uint32_t p = 0; p < drb::kPointCount; ++p) {
        if (((mask >> p) & 1u) == 0 && g_recorded[p] == 0) {
            continue;
        }
        t += "\n";
        t += kPointNames[p];
        t += ": " + std::to_string(g_recorded[p]) + " sent, " + std::to_string(s.markers_seen[p].load()) +
             " received, " + std::to_string(s.techniques_run[p].load()) + " technique runs";
    }
    const int64_t view = get_int_option(g_cvarDebugView, 0);
    if (view >= 1 && view <= static_cast<int64_t>(drb::kPointCount)) {
        t += std::string("\nDebug view: ReShade's change at '") + kPointNames[view - 1] +
             (g_lastDebugDrawn ? "' (mid-grey: unchanged; darker or brighter: what ReShade changed)."
                               : "' is not shown: nothing was handed over there last frame.");
    }
    if (!g_hookInstalled) {
        t += "\nThe 'Before particles & post-processing' point is unavailable (hook failed).";
    }
    if (s.generic_depth_enabled.load() != 0) {
        t += "\nReShade's Generic Depth add-on is on; turn it off in ReShade's Add-ons tab.";
    }
    if (s.depth_definitions_ok.load() == 0) {
        t += "\nReShade's depth settings are not the bridge's yet (see the Dusklight tab in ReShade).";
    }
    return t;
#endif
}

void add_control(UiElementHandle pane, const UiControlDesc& desc, UiElementHandle* out = nullptr) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, out);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = "Run ReShade in the game's frame";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarActive;
    c.help_rml = "Hands the frame to ReShade at the points chosen in ReShade's own overlay "
                 "(Dusklight tab). Off: ReShade runs as it normally does, over the finished frame.";
    add_control(panel, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = "Depth range";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarDepthRange;
    c.min = 0;
    c.max = 1000000;
    c.step = 500;
    c.suffix = " units";
    c.help_rml = "The view distance that ReShade's linearized depth reports as 1.0 (the far "
                 "plane). 0 uses the camera's far plane, which varies by area. Smaller values give "
                 "nearby scenery more of the depth range, as lowering "
                 "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE does in plain ReShade.";
    add_control(panel, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_SELECT;
    c.label = "Scale-up";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarSpread;
    c.options = kSpreadOptions;
    c.option_count = std::size(kSpreadOptions);
    c.help_rml = "When the game renders at another resolution than the screen, ReShade gets the frame "
                 "at the screen's size and what it changed is scaled back up to the game's resolution. "
                 "Edge-aware: along silhouettes each pixel takes the change from the side whose depth "
                 "matches its own, so a change on one object does not bleed onto what is behind it. "
                 "Simple: plain bilinear, for comparison.";
    add_control(panel, c);

    c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_SELECT;
    c.label = "Debug view";
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = g_cvarDebugView;
    c.options = kDebugViewOptions;
    c.option_count = std::size(kDebugViewOptions);
    c.help_rml = "Shows only what ReShade changed at one point, scaled up exactly as it is applied to "
                 "the frame, over the whole screen: mid-grey where nothing changed, darker where ReShade "
                 "darkened, brighter where it brightened. Compare the two Scale-up settings along "
                 "silhouettes here.";
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
        return mods::set_error(error, MOD_ERROR, "failed to register a ReShade Bridge option");
    }
    return MOD_OK;
}

} // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    if (register_option("bridgeActive", CONFIG_VAR_BOOL, true, 0, g_cvarActive, error) != MOD_OK ||
        register_option("depthRange", CONFIG_VAR_INT, false, 0, g_cvarDepthRange, error) != MOD_OK ||
        register_option("scaleUp", CONFIG_VAR_INT, false, rsb::kSpreadEdgeAware, g_cvarSpread, error) != MOD_OK ||
        register_option("debugView", CONFIG_VAR_INT, false, 0, g_cvarDebugView, error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK || g_deviceInfo.device == nullptr) {
        return mods::set_error(error, MOD_ERROR, "failed to query the graphics device");
    }
    std::string gpuError;
    if (!g_gpu.init(g_deviceInfo, gpuError)) {
        g_gpu.release();
        return mods::set_error(error, MOD_ERROR, "failed to create the ReShade Bridge's GPU objects");
    }
    g_gpuReady = true;

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "ReShade Bridge hand-over";
    computeDesc.callback = on_compute;
    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "ReShade Bridge composite";
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
    g_hookInstalled = rsb::install_after_translucent_hook(on_after_translucent);
    if (!g_hookInstalled) {
        svc_log->warn(mod_ctx,
            "could not hook dComIfGd_drawXluListDark (game build mismatch?): the 'Before particles & "
            "post-processing' point will not run");
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    g_shared = rsb::open_shared_state();
    if (g_shared != nullptr) {
        g_shared->mod_attached.store(1);
        svc_log->info(mod_ctx, g_shared->addon_loaded.load() != 0
                                   ? "ReShade Bridge ready; the ReShade add-on is loaded"
                                   : "ReShade Bridge ready; waiting for the ReShade add-on");
    } else {
#ifdef _WIN32
        svc_log->warn(mod_ctx, "ReShade Bridge: could not open the shared memory the add-on uses");
#else
        svc_log->info(mod_ctx, "ReShade Bridge: ReShade only runs on Windows; the bridge stays idle");
#endif
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    g_gpu.tick_retired();
    g_composited = 0;
    g_lastDebugDrawn = g_debugDrawn;
    g_debugDrawn = false;
    g_depthThisFrame = false;
    g_camera.valid = false;
    g_want = 0;
    const bool active = get_bool_option(g_cvarActive, true);
    if (g_shared != nullptr) {
        g_shared->bridge_active.store(active ? 1u : 0u);
    }
    if (g_shared == nullptr || !active) {
        return MOD_OK;
    }
    const drb::SharedState& s = *g_shared;
    if (s.addon_loaded.load() != 0 && s.runtime_ready.load() != 0 && s.effects_enabled.load() != 0 &&
        s.blocked.load() == drb::kNotBlocked) {
        g_want = s.points_mask.load() & ((1u << drb::kPointCount) - 1u);
    }
    if (!g_hookInstalled) {
        g_want &= ~(1u << drb::kBeforeParticles);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_shared != nullptr) {
        g_shared->bridge_active.store(0);
        g_shared->mod_attached.store(0);
    }
    rsb::close_shared_state();
    g_shared = nullptr;
    g_gpu.release();
    g_gpuReady = false;
    g_computeType = 0;
    g_drawType = 0;
    for (auto& h : g_stageHooks) {
        h = 0;
    }
    g_hookInstalled = false;
    g_cvarActive = g_cvarDepthRange = g_cvarSpread = g_cvarDebugView = 0;
    g_composited = 0;
    g_debugDrawn = g_lastDebugDrawn = false;
    g_panelStatus = 0;
    g_want = 0;
    std::fill(std::begin(g_recorded), std::end(g_recorded), 0);
    std::fill(std::begin(g_warnedFormat), std::end(g_warnedFormat), false);
    return MOD_OK;
}
}
