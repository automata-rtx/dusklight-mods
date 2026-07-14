// Depth to Normal - shared service contract.
//
// Include this header in any mod that wants the per-pixel world-space surface normal that the
// Depth to Normal provider mod reconstructs from the scene depth buffer once per frame. See
// docs/depth_to_normal_plan.md and docs/depth_to_normal_consumers.md.
//
// Usage (consumer):
//     #include "depth_to_normal_service.h"
//     IMPORT_OPTIONAL_SERVICE(DepthToNormalService, svc_n2d);   // or IMPORT_SERVICE (hard dep)
//     ...
//     DepthToNormalFrame f = DEPTH_TO_NORMAL_FRAME_INIT;
//     if (svc_n2d != nullptr && svc_n2d->get_frame(mod_ctx, &f) == MOD_OK) {
//         // bind f.normal (rgba32float: xyz = world normal, w = raw depth), sized f.width x f.height
//     }
//
// Call get_frame from a game-thread stage callback (e.g. SCENE_AFTER_OPAQUE) BEFORE the draw or
// compute that samples the normal - it queues the reconstruction into the current command stream
// and returns the frame's normal view. The view is valid for the current frame only; never cache
// it across frames.

#ifndef DEPTH_TO_NORMAL_SERVICE_H
#define DEPTH_TO_NORMAL_SERVICE_H

#include "mods/api.h"

#include <webgpu/webgpu.h>

#define DEPTH_TO_NORMAL_SERVICE_ID "dev.automata.depth_to_normal"
#define DEPTH_TO_NORMAL_SERVICE_MAJOR 1u
#define DEPTH_TO_NORMAL_SERVICE_MINOR 0u

typedef struct DepthToNormalFrame {
    uint32_t struct_size;
    WGPUTextureView normal; /* rgba32float: xyz = world-space geometric normal (unit,
                             * camera-facing), w = raw reversed-Z depth. Frame-valid. */
    uint32_t width;
    uint32_t height;
} DepthToNormalFrame;

#define DEPTH_TO_NORMAL_FRAME_INIT {sizeof(DepthToNormalFrame), NULL, 0u, 0u}

typedef struct DepthToNormalService {
    ServiceHeader header;

    /*
     * Ensure this frame's world-space normal buffer is computed and queued into the command
     * stream, then return its view + dimensions in *out. Idempotent per frame: the first call of
     * a frame does the work; later calls return the cached result. Call from a game-thread stage
     * callback before the draw/compute that samples it. Returns MOD_UNAVAILABLE (out->normal NULL)
     * if there is no populated scene / camera this frame. The view is frame-valid; do not cache.
     */
    ModResult (*get_frame)(ModContext* ctx, DepthToNormalFrame* out);
} DepthToNormalService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct dusk::mods::ServiceTraits<DepthToNormalService> {
    static constexpr const char* id = DEPTH_TO_NORMAL_SERVICE_ID;
    static constexpr uint16_t major_version = DEPTH_TO_NORMAL_SERVICE_MAJOR;
};
#endif

#endif  // DEPTH_TO_NORMAL_SERVICE_H
