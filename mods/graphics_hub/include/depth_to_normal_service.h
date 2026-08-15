// Depth to Normal - shared service contract.
//
// Include this header in any mod that wants the per-pixel world-space surface normal the Depth to
// Normal provider (Graphics Hub) publishes once per frame. See docs/depth_to_normal_plan.md and
// docs/depth_to_normal_consumers.md.
//
// WHICH NORMAL THIS IS. Two different things get called "the normal", they are not
// interchangeable, and picking the wrong one is a bug that looks like a tuning problem:
//
//   * SHADING normal - the game's authored, interpolated vertex normal. Smooth across a facet by
//     design, and by design NOT perpendicular to the triangle it sits on. Right for n.L, attached
//     shadows, normal offset, and cosine weighting.
//   * GEOMETRIC normal - the face normal of the rasterized triangle, i.e. the plane the depth
//     samples actually lie in. Right for anything asking "is this direction above or below the
//     surface": shadow-map bias, and the occlusion hemisphere of an AO march.
//
// **This service returns the SHADING normal wherever the game supplied one**, falling back to the
// depth reconstruction (which is geometric) only where it did not. So the meaning of the vector
// CHANGES per pixel and with the user's "Use Authored Normals" switch. A consumer that needs the
// geometric plane must derive it from depth itself and must not assume this vector is it -
// vbao/ssilvb do exactly that for their sample rejection, and the shadow mod for its bias.
//
// Getting this wrong is what made AO appear on flat ground the moment authored normals were turned
// on: a visibility hemisphere centred on a shading normal that tilts off the geometry swallows the
// very plane its samples lie in. See docs/authored_normals.md 8.6 and 8.11.
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
    WGPUTextureView normal; /* rgba32float: xyz = world-space surface normal (unit), w = raw
                             * reversed-Z depth. Frame-valid.
                             *
                             * WORLD SPACE, always - the authored normal arrives from the renderer
                             * in view space and the provider rotates it out with the camera
                             * service's world_from_view, so consumers get one canonical basis
                             * whichever source fed the pixel. Rotate it into your own space if you
                             * need to (vbao/ssilvb rotate straight back to view, which is a round
                             * trip through exact inverses - lossless, and the price of the shared
                             * buffer).
                             *
                             * ORIENTATION: the surface direction with the GAME'S OWN SIGN. Do NOT
                             * flip it toward the camera, at any threshold. dot(n, view_ray) is not
                             * a property of the surface - the ray sweeps across the screen - so any
                             * such test negates everything past a line and seams flat ground. This
                             * was got wrong three times; see docs/authored_normals.md 2a. The only
                             * legitimate camera-facing flips are on cross products you build
                             * yourself, whose sign genuinely is arbitrary. */
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
struct mods::ServiceTraits<DepthToNormalService> {
    static constexpr const char* id = DEPTH_TO_NORMAL_SERVICE_ID;
    static constexpr uint16_t major_version = DEPTH_TO_NORMAL_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = DEPTH_TO_NORMAL_SERVICE_MINOR;
};
#endif

#endif  // DEPTH_TO_NORMAL_SERVICE_H
