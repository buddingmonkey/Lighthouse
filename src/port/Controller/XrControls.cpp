#include "XrControls.h"

#ifndef ENABLE_OPENXR

extern "C" void XrControls_MergeInto(void*) {
}

#else

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <fast/backends/gfx_xr_view.h>
#include <libultraship/libultra/controller.h>
#include <ship/Context.h>
#include <ship/controller/controldeck/ControlDeck.h>

namespace {
// N64 sticks read a little past 80 at the octagon corners; 80 is the safe full-range value.
constexpr float kStickRange = 80.0f;
// A trigger or a grip is analogue and the N64 button is not, so it latches at two thirds.
constexpr float kPullPoint = 0.66f;
// How far the right stick goes over before it counts as a C button held.
constexpr float kCameraPoint = 0.5f;
} // namespace

extern "C" void XrControls_MergeInto(void* contPad) {
    Fast::XrPadState xr;
    if (contPad == nullptr || !Fast::GetXrPad(&xr)) {
        return;
    }
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetControlDeck() == nullptr || ctx->GetControlDeck()->GamepadGameInputBlocked()) {
        return;
    }

    uint32_t buttons = 0;
    if (xr.buttons & Fast::XR_PAD_A) {
        buttons |= BTN_A;
    }
    if (xr.buttons & Fast::XR_PAD_B) {
        buttons |= BTN_B;
    }
    if (xr.buttons & Fast::XR_PAD_MENU) {
        buttons |= BTN_START;
    }
    // Both index triggers give Z, which is held through most of what the game asks for it.
    if (xr.trigger[0] >= kPullPoint || xr.trigger[1] >= kPullPoint) {
        buttons |= BTN_Z;
    }
    if (xr.squeeze[0] >= kPullPoint) {
        buttons |= BTN_L;
    }
    if (xr.squeeze[1] >= kPullPoint) {
        buttons |= BTN_R;
    }
    // The C buttons are the camera, and the right stick is where a hand expects to find it.
    if (xr.stick[1][0] <= -kCameraPoint) {
        buttons |= BTN_CLEFT;
    }
    if (xr.stick[1][0] >= kCameraPoint) {
        buttons |= BTN_CRIGHT;
    }
    if (xr.stick[1][1] >= kCameraPoint) {
        buttons |= BTN_CUP;
    }
    if (xr.stick[1][1] <= -kCameraPoint) {
        buttons |= BTN_CDOWN;
    }
    // X and Y stand in for the D-pad, which the game uses in its menus alone.
    if (xr.buttons & Fast::XR_PAD_X) {
        buttons |= BTN_DLEFT;
    }
    if (xr.buttons & Fast::XR_PAD_Y) {
        buttons |= BTN_DUP;
    }

    OSContPad* pad = static_cast<OSContPad*>(contPad);
    pad->button |= buttons;
    // Leave a gamepad in charge when it is being used.
    if (pad->stick_x == 0 && pad->stick_y == 0) {
        const float x = std::clamp(xr.stick[0][0], -1.0f, 1.0f) * kStickRange;
        const float y = std::clamp(xr.stick[0][1], -1.0f, 1.0f) * kStickRange;
        pad->stick_x = (int8_t)std::lround(x);
        pad->stick_y = (int8_t)std::lround(y);
    }
    if (pad->right_stick_x == 0 && pad->right_stick_y == 0) {
        const float x = std::clamp(xr.stick[1][0], -1.0f, 1.0f) * kStickRange;
        const float y = std::clamp(xr.stick[1][1], -1.0f, 1.0f) * kStickRange;
        pad->right_stick_x = (int8_t)std::lround(x);
        pad->right_stick_y = (int8_t)std::lround(y);
    }
}

#endif
