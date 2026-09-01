#pragma once

#include <simd/simd.h>
#include <stdbool.h>
#include <stddef.h>
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

// The shell must take the immersive space down before the process leaves, so the handler is what
// the game calls when it is finished rather than leaving on its own. It arrives on the main queue.
void LighthouseVolumeSetShutdownHandler(void (*handler)(void));

// Stop world tracking and let the game thread go. Safe to call more than once.
void LighthouseVolumeStop(void);

// Once per scene update, on the main thread. It asks ARKit where the head is, publishes the pose
// for the render thread, and lets the game draw one frame.
void LighthouseVolumeUpdate(LighthouseVolumeFrame frame);

// The shape of the picture the game draws, width over height. Zero until there is a picture, and
// the shell then keeps the shape it opened with.
float LighthouseVolumeAspect(void);

// Where a drag meets the picture, in game texture pixels. One gesture carries the tap, the drag
// and the release, because a slider needs all three.
void LighthouseVolumePoint(float x, float y, bool pressed);

// One rectangle the system can highlight, in game texture pixels. An identifier of zero is a
// window, which takes no highlight of its own and only hides what is behind it.
typedef struct {
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
    uint64_t Identifier;
} LighthouseVolumeHoverRect;

// The menu rectangles of the last finished frame, back to front. visionOS never says where the
// wearer looks, so the shell offers these to the system and the system draws the highlight itself.
size_t LighthouseVolumeHoverRects(LighthouseVolumeHoverRect* out, size_t max);

// How long the shell spent copying the finished picture, in seconds.
void LighthouseVolumeNoteCopy(double seconds);

// Whether the shell can show an eye each. Set it before the game starts.
void LighthouseVolumeSetStereo(bool stereo);

// True once for each frame the game finishes, and then the eye textures to copy.
bool LighthouseVolumeTakeFrame(void);
void* LighthouseVolumeTexture(int eye);

#ifdef __cplusplus
}
#endif
