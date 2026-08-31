#pragma once

#include <simd/simd.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the shell knows each scene update. Scene phase: 0 background, 1 inactive, 2 active.
typedef struct {
    bool HasQuad; ///< The quad's transform in the immersive space is known this update.
    simd_float4x4 ImmersiveFromQuad;
    float HalfWidth; ///< The picture on the quad, in meters.
    float HalfHeight;
    int ScenePhase;
} LighthouseVolumeFrame;

// Metal objects are Objective-C objects, so the shell and the bridge agree on void* rather than on
// Metal headers.
void LighthouseVolumeStart(void* device, void* commandQueue, uint32_t width, uint32_t height);

// Once per scene update, on the main thread. It asks ARKit where the head is, publishes the pose
// for the render thread, and lets the game draw one frame.
void LighthouseVolumeUpdate(LighthouseVolumeFrame frame);

// Where a drag meets the picture, in game texture pixels. One gesture carries the tap, the drag
// and the release, because a slider needs all three.
void LighthouseVolumePoint(float x, float y, bool pressed);

// The game texture the last finished frame left, or null when there is nothing new to copy.
void* LighthouseVolumeTakeTexture(int eye);

#ifdef __cplusplus
}
#endif
