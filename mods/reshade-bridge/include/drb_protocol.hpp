// The contract between the two halves of the ReShade bridge, both built from this repository in the
// same CI run:
//   reshade_bridge.dusk                Dusklight mod. Snapshots the scene at each insertion point,
//                                      records "marker" copies into the game's WebGPU frame and
//                                      composites the result back.
//   dusklight_reshade_bridge.addon64   ReShade add-on. Recognises the marker copies when Dawn writes
//                                      them into its Direct3D 12 command list and runs the ReShade
//                                      techniques assigned to that point right there.
//
// The two never exchange GPU handles: the mod only has WebGPU objects and the add-on only D3D12
// ones. A marker is a one-texel copy from the mod's colour (or depth) texture into a small texture
// whose size encodes the insertion point; the add-on reads both resources from the copy itself.
//
// Sizes. ReShade sizes its effects' own textures for its screen (the swapchain's back buffer) and
// shares them, by name, with the copies of an effect it compiles for other sizes, so an effect only
// works on a frame of exactly that size. The add-on therefore publishes the screen size
// (SharedState::screen_width/height) and the mod hands every frame over at that size, placed the
// way Dusklight's own present places it, whatever the game's internal resolution.
// Everything else goes through SharedState, a small block of named shared memory that either side
// creates and both map, so neither depends on the other staying loaded (Dusklight reloads mods;
// ReShade unloads add-ons with its last device).

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace drb {

constexpr uint32_t kProtocolVersion = 2;

// Insertion points, in frame order. The add-on persists them by value; append only.
constexpr uint32_t kPointCount = 4;
enum Point : uint32_t {
    kBeforeTransparency = 0, // opaque world drawn; water, glass, particles and post-processing follow
    kBeforeParticles = 1,    // all world geometry; particles, motion blur, DOF, heat haze, bloom follow
    kBeforeHud = 2,          // the game's post-processing is done; the HUD follows
    kAfterHud = 3,           // the finished frame
};
// Where a technique runs until the user picks a point for it: under the HUD, after the game's own
// post-processing, which is the closest to where ReShade normally runs without touching the HUD.
constexpr Point kDefaultPoint = kBeforeHud;

// Marker texture sizes. 1597 is prime and far from any size the game uses. Colour markers have the
// colour texture's format, depth markers R32Float.
constexpr uint32_t kMarkerWidth = 1597;
constexpr uint32_t kColorMarkerHeightBase = 11; // height = base + point
constexpr uint32_t kDepthMarkerHeightBase = 23; // height = base + point

// The depth texture the mod hands over holds this encoding: for linear depth L in [0, 1],
// 1 - L * F / (1 + L * (F - 1)) with F = kDepthFarPlane, which ReShade.fxh's GetLinearizedDepth
// turns back into L when the RESHADE_DEPTH_* definitions below are in effect. The add-on sets them.
constexpr float kDepthFarPlane = 1000.0f;
struct DepthDefinition {
    const char* name;
    const char* value;
};
constexpr DepthDefinition kDepthDefinitions[] = {
    {"RESHADE_DEPTH_INPUT_IS_REVERSED", "1"},
    {"RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0"},
    {"RESHADE_DEPTH_INPUT_IS_MIRRORED", "0"},
    {"RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0"},
    {"RESHADE_DEPTH_MULTIPLIER", "1"},
    {"RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", "1000.0"},
    {"RESHADE_DEPTH_INPUT_X_SCALE", "1"},
    {"RESHADE_DEPTH_INPUT_Y_SCALE", "1"},
    {"RESHADE_DEPTH_INPUT_X_OFFSET", "0"},
    {"RESHADE_DEPTH_INPUT_Y_OFFSET", "0"},
    {"RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0"},
    {"RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0"},
};

// Why the add-on refuses to run techniques (SharedState::blocked).
enum BlockReason : uint32_t {
    kNotBlocked = 0,
    kNotD3D12 = 1, // the effect runtime is not on Direct3D 12
};

// The shared block. It lives in zero-filled shared memory, so every field starts at 0 and there are
// no constructors; `magic` is set by whichever side maps it first. Lock-free atomics only (they are
// address-free, so they work across two mappings of the same memory).
constexpr uint32_t kMagic = 0x44524200u | kProtocolVersion; // "DRB" + protocol
struct SharedState {
    std::atomic<uint32_t> magic;

    // Written by the add-on.
    std::atomic<uint32_t> addon_loaded;          // the add-on is registered with ReShade
    std::atomic<uint32_t> runtime_ready;         // an effect runtime exists on a D3D12 device
    std::atomic<uint32_t> effects_enabled;       // ReShade's global effects switch
    std::atomic<uint32_t> blocked;               // BlockReason
    std::atomic<uint32_t> points_mask;           // bit p: an enabled technique runs at point p
    std::atomic<uint32_t> depth_definitions_ok;  // the RESHADE_DEPTH_* definitions are in effect
    std::atomic<uint32_t> generic_depth_enabled; // ReShade's built-in Generic Depth add-on is on
    std::atomic<uint64_t> present_count;
    std::atomic<uint64_t> markers_seen[kPointCount];       // colour markers recognised
    std::atomic<uint64_t> depth_markers_seen[kPointCount]; // depth markers recognised
    std::atomic<uint64_t> techniques_run[kPointCount];     // render_technique calls made
    std::atomic<uint32_t> screen_width;  // ReShade's screen (back buffer) size: every hand-over is
    std::atomic<uint32_t> screen_height; // made at this size; 0 until an effect runtime exists

    // Written by the mod.
    std::atomic<uint32_t> mod_attached;  // the mod is loaded
    std::atomic<uint32_t> bridge_active; // its "Run ReShade in the game's frame" option is on; while
                                         // it is, ReShade's own end-of-frame pass is suppressed
    std::atomic<uint64_t> markers_recorded[kPointCount];
};
static_assert(std::is_standard_layout_v<SharedState>);
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

// Size of the mapping: generous, so a later protocol can grow the struct without a new name.
constexpr uint32_t kMappingSize = 4096;
static_assert(sizeof(SharedState) <= kMappingSize);

// Name of the mapping, per process: kMappingPrefix followed by the decimal process id.
constexpr const wchar_t* kMappingPrefix = L"Local\\DusklightReShadeBridge.";

// Claims a freshly mapped block or checks an existing one. False: another protocol owns it.
inline bool claim(SharedState* state) {
    uint32_t expected = 0;
    return state->magic.compare_exchange_strong(expected, kMagic) || expected == kMagic;
}

constexpr const char* kAddonFileName = "dusklight_reshade_bridge.addon64";

} // namespace drb
