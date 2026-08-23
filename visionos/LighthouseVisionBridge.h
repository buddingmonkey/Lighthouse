#pragma once

#import <CompositorServices/CompositorServices.h>

#ifdef __cplusplus
extern "C" {
#endif

void LighthouseVisionRun(cp_layer_renderer_t renderer);

// Phase: 0 active, 1 ended, 2 cancelled. The ray is the system's gaze and pinch selection, in
// world coordinates, and it arrives only while a pinch is held.
void LighthouseVisionSpatialEvent(int phase, float originX, float originY, float originZ, float directionX,
                                  float directionY, float directionZ);

#ifdef __cplusplus
}
#endif
