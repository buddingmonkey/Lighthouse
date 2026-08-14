#include <libultra/gbi.h>

#include "Patches.h"

// The HUD is not part of the world behind the window. The game places it a fixed distance in front
// of the camera with an ordinary perspective matrix, which the headset backend would otherwise
// replace with the off-axis frustum, and the counters and prompts would then float in the room at
// whatever depth the game chose.
//
// The mark has to travel in the display list. Everything here runs while the game builds the list,
// and the projection is substituted later, when the list is run.
extern "C" void port_xr_beginFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 1);
}

extern "C" void port_xr_endFlat(Gfx** gfx) {
    gSPXrFlatProjection((*gfx)++, 0);
}
