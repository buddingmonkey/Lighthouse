#include <libultra/gbi.h>

#include "Patches.h"

#ifdef ENABLE_XR_WINDOW
#include <cmath>
#include <fast/backends/gfx_xr_view.h>

extern "C" {
bool player_is_present(void);
void player_getPosition(f32 dst[3]);
void viewport_getPosition_vec3f(f32 arg0[3]);
}
#endif

#ifndef ENABLE_XR_WINDOW

extern "C" void port_xr_beginFlat(Gfx** gfx) {
}

extern "C" void port_xr_endFlat(Gfx** gfx) {
}

extern "C" void port_xr_beginParticlePass(Gfx** gfx) {
}

extern "C" void port_xr_endParticlePass(Gfx** gfx) {
}

extern "C" void port_xr_setSubjectDistance(void) {
}

#else

extern "C" void port_xr_beginFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 1);
}

extern "C" void port_xr_endFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 0);
}

extern "C" void port_xr_beginParticlePass(Gfx** gfx) {
    gSPXrSceneDepth((*gfx)++, 1);
    gSPTextureBatch((*gfx)++, 1);
}

extern "C" void port_xr_endParticlePass(Gfx** gfx) {
    gSPTextureBatch((*gfx)++, 0);
    gSPXrSceneDepth((*gfx)++, 0);
}

extern "C" void port_xr_setSubjectDistance(void) {
    float distance = 0.0f;
    if (player_is_present()) {
        f32 camera[3];
        f32 player[3];
        viewport_getPosition_vec3f(camera);
        player_getPosition(player);
        distance = sqrtf((player[0] - camera[0]) * (player[0] - camera[0]) +
                         (player[1] - camera[1]) * (player[1] - camera[1]) +
                         (player[2] - camera[2]) * (player[2] - camera[2]));
    }
    Fast::SetXrSubjectDistance(distance);
}

#endif
