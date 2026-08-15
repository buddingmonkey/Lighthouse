#include <libultra/gbi.h>

#include "Patches.h"

// The mark has to travel in the display list: the list is built here and the projection is
// substituted later, when it is run.
extern "C" void port_xr_beginFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 1);
}

extern "C" void port_xr_endFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 0);
}
