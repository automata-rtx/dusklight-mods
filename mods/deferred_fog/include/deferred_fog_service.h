/*
 * Deferred Fog service — "dev.automata.deferred_fog".
 *
 * WHAT THIS IS FOR, AND WHY IT IS SO SMALL. Deferred Fog produces no data another mod needs; it
 * changes WHEN the game's fog is applied. The reason to import it is ORDERING.
 *
 * The mod API has no priority field on a stage hook: GfxService runs the hooks registered for a
 * stage in registration order, and registration happens during mod_initialize, which the loader
 * runs in dependency order. So importing a mod's service is how you say "initialize that one
 * first" — see docs/modding.md, "Dependencies between mods".
 *
 * Deferred Fog draws its fog quad at GFX_STAGE_FRAME_BEFORE_HUD (its SCENE_AFTER_OPAQUE hook only
 * arms it). That means:
 *
 *   - A mod that composites at SCENE_AFTER_OPAQUE is ALREADY ordered before the fog by stage
 *     separation, and needs no import for that. This is the main path, and it is why AO ends up
 *     under the fog rather than on top of it.
 *   - A mod that also draws at FRAME_BEFORE_HUD and wants to be ON TOP of the fog — a debug
 *     overlay, say — must register its hook AFTER Deferred Fog's, so it must initialize after it,
 *     so it must import this. Drawing at FRAME_AFTER_HUD instead is simpler and needs no import at
 *     all, which is what VBAO's debug views switched to; nothing in this repo imports this service
 *     for ordering today.
 *
 * Import it OPTIONALLY (IMPORT_OPTIONAL_SERVICE). Deferred Fog is a separate install, and a
 * consumer must run correctly without it — the fog is simply the game's own forward fog then, and
 * the ordering question does not arise.
 */

#ifndef DEFERRED_FOG_SERVICE_H
#define DEFERRED_FOG_SERVICE_H

#include "mods/api.h"

#define DEFERRED_FOG_SERVICE_ID "dev.automata.deferred_fog"
#define DEFERRED_FOG_SERVICE_MAJOR 1u
#define DEFERRED_FOG_SERVICE_MINOR 0u

typedef struct DeferredFogState {
    uint32_t struct_size;
    /* True when the mod is enabled AND deferring this frame. False means the frame ran on the
     * game's own forward fog — the mod auto-reverts on mixed fog configurations it cannot replay
     * faithfully, so this can change from frame to frame within one area. */
    bool deferring;
} DeferredFogState;

#define DEFERRED_FOG_STATE_INIT {sizeof(DeferredFogState), false}

typedef struct DeferredFogService {
    ServiceHeader header;
    /* Never fails; reports `deferring = false` if asked before the first frame completes. */
    ModResult (*get_state)(ModContext* ctx, DeferredFogState* out_state);
} DeferredFogService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct mods::ServiceTraits<DeferredFogService> {
    static constexpr const char* id = DEFERRED_FOG_SERVICE_ID;
    static constexpr uint16_t major_version = DEFERRED_FOG_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = DEFERRED_FOG_SERVICE_MINOR;
};
#endif

#endif  // DEFERRED_FOG_SERVICE_H
