/*
 * Deferred Fog service, "dev.automata.deferred_fog".
 *
 * Reports whether Deferred Fog is applying this frame's fog itself. It provides no other data.
 *
 * Ordering needs no import in the usual cases. Deferred Fog draws its fog pass from a pre-hook on
 * dComIfGd_drawXluListBG, which the game calls directly after GFX_STAGE_SCENE_AFTER_OPAQUE, so a
 * mod compositing at SCENE_AFTER_OPAQUE is always under the fog. To draw on top of the fog, draw at
 * GFX_STAGE_FRAME_AFTER_HUD, as VBAO's debug views do.
 *
 * Importing the service matters only for ordering within one stage. Stage hooks have no priority
 * field: within a stage they run in slot order, which follows registration order, and registration
 * happens in mod_initialize, which the loader runs in dependency order (dusklight/docs/modding.md,
 * "Dependencies between mods"). For example, a SCENE_AFTER_OPAQUE hook that wants this frame's
 * `deferring` value must run after Deferred Fog's, so its mod must import this service.
 *
 * Import it with IMPORT_OPTIONAL_SERVICE: Deferred Fog is a separate install, and without it the
 * game uses its own fog.
 */

#ifndef DEFERRED_FOG_SERVICE_H
#define DEFERRED_FOG_SERVICE_H

#include "mods/api.h"

#define DEFERRED_FOG_SERVICE_ID "dev.automata.deferred_fog"
#define DEFERRED_FOG_SERVICE_MAJOR 1u
#define DEFERRED_FOG_SERVICE_MINOR 0u

typedef struct DeferredFogState {
    uint32_t struct_size;
    /* True when this frame's fog pass was armed at SCENE_AFTER_OPAQUE. False while the mod is off,
     * inactive (a required game hook is missing) or leaving Wolf Senses to the game, and in a
     * frame with no fogged draws, so it can change from frame to frame. Updated at
     * SCENE_AFTER_OPAQUE; earlier in the frame it holds the previous frame's value. It stays true
     * if the fog pass then fails to draw (a failed depth snapshot). */
    bool deferring;
} DeferredFogState;

#define DEFERRED_FOG_STATE_INIT {sizeof(DeferredFogState), false}

typedef struct DeferredFogService {
    ServiceHeader header;
    /* Returns MOD_INVALID_ARGUMENT if out_state is null or its struct_size is too small; otherwise
     * fills it and returns MOD_OK. `deferring` is false until the first frame is processed. */
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
