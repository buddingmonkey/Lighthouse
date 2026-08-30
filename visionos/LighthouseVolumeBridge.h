#pragma once

#include <simd/simd.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Every route a volume might have to the head. Scene phase: 0 background, 1 inactive, 2 active.
typedef struct {
    bool HasImmersiveSpace;
    simd_float4x4 ImmersiveFromQuad;
    bool HeadAnchored;
    simd_float4x4 QuadFromHead;
    bool WorldAnchored;
    simd_float4x4 QuadFromWorld;
    simd_float3 BoundsCenter;
    simd_float3 BoundsExtents;
    int ScenePhase;
    int SpaceOpen;
} LighthouseVolumeSample;

void LighthouseVolumeStart(void);

void LighthouseVolumeProbe(LighthouseVolumeSample sample);

#ifdef __cplusplus
}
#endif
