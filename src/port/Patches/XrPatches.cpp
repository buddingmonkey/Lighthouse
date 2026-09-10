#include <libultra/gbi.h>

#include "Patches.h"

#ifndef ENABLE_XR_WINDOW

extern "C" void port_xr_beginFlat(Gfx** gfx) {
}

extern "C" void port_xr_endFlat(Gfx** gfx) {
}

extern "C" void port_xr_beginParticlePass(Gfx** gfx) {
}

extern "C" void port_xr_endParticlePass(Gfx** gfx) {
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

#endif
