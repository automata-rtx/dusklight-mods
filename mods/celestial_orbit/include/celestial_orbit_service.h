// Celestial Orbit - shared service contract.
//
// The Celestial Orbit mod retilts the path the sun and the moon travel across the sky (and can
// spin that path about world Y), so realtime lighting gets a high midday sun instead of the
// 59-degree ceiling Twilight Princess hard-codes. It does that by rewriting sun_pos / moon_pos
// after the game computes them, which moves the visible body AND every consumer of those
// positions inside the game (base light, weather light direction, billboards, lens flare).
//
// A mod that derives its own light direction from the time of day instead of reading sun_pos -
// Realtime Sun Shadows does exactly that, so its debug time-of-day override keeps working - would
// otherwise disagree with the sky. Such a mod imports this service and pushes its own vanilla
// offset through celestial_orbit_apply_offset(), which is the same function the provider uses, so
// the two cannot drift apart.
//
// Usage (consumer):
//     #include "celestial_orbit_service.h"
//     IMPORT_OPTIONAL_SERVICE(CelestialOrbitService, svc_orbit);   // soft dependency
//     ...
//     CelestialOrbitState orbit = CELESTIAL_ORBIT_STATE_INIT;   // vanilla until proven otherwise
//     if (svc_orbit != nullptr) {
//         svc_orbit->get_state(mod_ctx, &orbit);
//     }
//     if (orbit.active) {
//         celestial_orbit_apply_offset(
//             orbit.sun_z_ratio, orbit.yaw_sin, orbit.yaw_cos, &x, &y, &z);
//     }
//
// Call get_state from the game thread. The state is plain cached scalars refreshed once per
// frame, so unlike a frame-valid texture view it is safe to copy and hold for the frame.

#ifndef CELESTIAL_ORBIT_SERVICE_H
#define CELESTIAL_ORBIT_SERVICE_H

#include "mods/api.h"

#define CELESTIAL_ORBIT_SERVICE_ID "dev.automata.celestial_orbit"
#define CELESTIAL_ORBIT_SERVICE_MAJOR 1u
#define CELESTIAL_ORBIT_SERVICE_MINOR 0u

/* Vanilla tilt: setSunpos() builds the body offset with a horizontal radius of 80000 and a z lean
 * of 48000, i.e. z = y * 0.6, whose peak elevation is atan(1 / 0.6) = 59.036 degrees. */
#define CELESTIAL_ORBIT_VANILLA_Z_RATIO 0.6f

typedef struct CelestialOrbitState {
    uint32_t struct_size;
    uint32_t active;   /* 0 = the mod is writing nothing this frame; treat the orbit as vanilla
                        * and do NOT call celestial_orbit_apply_offset (the remaining fields are
                        * still filled with vanilla-equivalent values, so applying it anyway is
                        * harmless, just pointless). */
    float sun_z_ratio;  /* cot(peak sun elevation); CELESTIAL_ORBIT_VANILLA_Z_RATIO = vanilla */
    float moon_z_ratio; /* cot(peak moon elevation) */
    float yaw_sin;      /* sin/cos of the orbit-plane rotation about world Y (0 / 1 = vanilla), */
    float yaw_cos;      /* pre-resolved so consumers never re-derive the angle convention */
} CelestialOrbitState;

#define CELESTIAL_ORBIT_STATE_INIT                                                                 \
    {sizeof(CelestialOrbitState), 0u, CELESTIAL_ORBIT_VANILLA_Z_RATIO,                             \
        CELESTIAL_ORBIT_VANILLA_Z_RATIO, 0.0f, 1.0f}

/*
 * Rewrite one vanilla body offset - the (x, y, z) that dScnKy_env_light_c::setSunpos() builds,
 * relative to the camera eye - into the modded orbit, in place. THE one implementation: the
 * provider and every consumer call this, so a change here moves both at once.
 *
 * Vanilla, for the swept angle a and radius R = 80000:
 *
 *     x = sin a * R          y = -cos a * R          z = -cos a * R * 0.6  ==  y * 0.6
 *
 * so z carries no information the caller does not already have in y. Re-deriving it from y with a
 * different ratio is exactly "retilt the orbit plane": the path stays a great circle through the
 * same two horizon points, swept by the same angle at the same time of day, but its highest point
 * rises to atan(1 / z_ratio). Nothing about the sweep - and therefore nothing about the time of
 * day, palette schedule, or day/night transitions - changes.
 *
 * The yaw then spins that retilted plane about world Y, which moves where the body rises and sets
 * without touching any elevation. Positive yaw turns the path from +Z toward +X.
 */
static inline void celestial_orbit_apply_offset(
    float z_ratio, float yaw_sin, float yaw_cos, float* x, float* y, float* z) {
    const float leaned_z = *y * z_ratio;
    const float rotated_x = (*x * yaw_cos) + (leaned_z * yaw_sin);
    const float rotated_z = (leaned_z * yaw_cos) - (*x * yaw_sin);
    *x = rotated_x;
    *z = rotated_z;
}

typedef struct CelestialOrbitService {
    ServiceHeader header;

    /*
     * Copy this frame's orbit state into *out. Always succeeds (MOD_OK) once the provider is
     * loaded; *out is left vanilla-equivalent with active = 0 while the feature is switched off.
     * Game thread.
     */
    ModResult (*get_state)(ModContext* ctx, CelestialOrbitState* out);
} CelestialOrbitService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct mods::ServiceTraits<CelestialOrbitService> {
    static constexpr const char* id = CELESTIAL_ORBIT_SERVICE_ID;
    static constexpr uint16_t major_version = CELESTIAL_ORBIT_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = CELESTIAL_ORBIT_SERVICE_MINOR;
};
#endif

#endif  // CELESTIAL_ORBIT_SERVICE_H
