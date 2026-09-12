#pragma once

#include <simd/simd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool HasQuad;
    simd_float4x4 ImmersiveFromQuad;
    float HalfWidth;
    float HalfHeight;
    int ScenePhase;
} LighthouseVolumeFrame;

void LighthouseVolumeStart(void* device, void* commandQueue, uint32_t width, uint32_t height);

void LighthouseVolumeSetShutdownHandler(void (*handler)(void));

void LighthouseVolumeStop(void);

void LighthouseVolumeUpdate(LighthouseVolumeFrame frame);

float LighthouseVolumeAspect(void);

void LighthouseVolumeNote(const char* text);

void LighthouseVolumeOpenMenu(void);

void LighthouseVolumePoint(float x, float y, bool pressed);

typedef struct {
    float MinX;
    float MinY;
    float MaxX;
    float MaxY;
    uint64_t Identifier;
} LighthouseVolumeHoverRect;

size_t LighthouseVolumeHoverRects(LighthouseVolumeHoverRect* out, size_t max);

void LighthouseVolumeNoteCopy(double seconds);
void LighthouseVolumeNoteCopyGpu(double seconds);

void LighthouseVolumeSetStereo(bool stereo);

void* LighthouseVolumeTexture(int eye);

#ifdef __cplusplus
}
#endif
