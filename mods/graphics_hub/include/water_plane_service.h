// Water Plane - shared service contract.
//
// Publishes the height of the water surface at the scene's active water body, probed once per
// frame from the game (fopAcM_getWaterY at the player position) by the Water Plane provider
// (Graphics Hub, hub_water). Screen-space effect mods (AO/GI) consume it to FADE their effect
// with water depth. Pure query - the provider draws nothing and touches no GX state, so with no
// consumer (or no active effect) it has zero visual impact.
//
// Usage (consumer):
//     #include "water_plane_service.h"
//     IMPORT_OPTIONAL_SERVICE(WaterPlaneService, svc_water);
//     ...
//     WaterPlaneFrame w = WATER_PLANE_FRAME_INIT;
//     if (svc_water != nullptr && svc_water->get_frame(mod_ctx, &w) == MOD_OK && w.has_water) {
//         // fade the effect where reconstructed world Y < w.water_y, by (w.water_y - world_y)
//     }
// Call get_frame from a game-thread stage callback (e.g. SCENE_AFTER_OPAQUE).

#ifndef WATER_PLANE_SERVICE_H
#define WATER_PLANE_SERVICE_H

#include "mods/api.h"

#define WATER_PLANE_SERVICE_ID "dev.automata.water_plane"
#define WATER_PLANE_SERVICE_MAJOR 1u
#define WATER_PLANE_SERVICE_MINOR 0u

typedef struct WaterPlaneFrame {
    uint32_t struct_size;
    float water_y;   /* world-space Y of the water surface (valid only when has_water) */
    bool has_water;  /* false when there is no water at the probe this frame */
} WaterPlaneFrame;

#define WATER_PLANE_FRAME_INIT {sizeof(WaterPlaneFrame), 0.0f, false}

typedef struct WaterPlaneService {
    ServiceHeader header;
    /* Idempotent per frame: first call probes fopAcM_getWaterY at the player XZ and caches; later
     * calls return the cache. Call from a game-thread stage callback. has_water=false => no fade. */
    ModResult (*get_frame)(ModContext* ctx, WaterPlaneFrame* out);
} WaterPlaneService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct mods::ServiceTraits<WaterPlaneService> {
    static constexpr const char* id = WATER_PLANE_SERVICE_ID;
    static constexpr uint16_t major_version = WATER_PLANE_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = WATER_PLANE_SERVICE_MINOR;
};
#endif

#endif  // WATER_PLANE_SERVICE_H
