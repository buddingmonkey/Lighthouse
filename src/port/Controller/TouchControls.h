#ifndef TOUCH_CONTROLS_H
#define TOUCH_CONTROLS_H

// On-screen gamepad for touch devices. Enabled only where a touchscreen is the primary
// input (iOS); everything here compiles to nothing elsewhere.

#ifdef __cplusplus
extern "C" {
#endif

// Reads the current fingers and updates the virtual pad. Call once per event pump,
// on the thread that pumps SDL events.
void TouchControls_Poll(void);

// ORs the virtual pad into an OSContPad. Call after the control deck has written it.
void TouchControls_MergeInto(void* contPad);

#ifdef __cplusplus
}

#include <libultraship/libultraship.h>

// Overlay host. Draws nothing off touch platforms, so it can be registered unconditionally.
class TouchControlsWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override{};
    void UpdateElement() override{};
    void Draw() override;
};

namespace Lighthouse {
// Draws the overlay. Called from the gui pass.
void TouchControls_Draw();
// True when the overlay should exist at all (touch-first platform, user hasn't disabled it).
bool TouchControls_Active();
} // namespace Lighthouse
#endif

#endif // TOUCH_CONTROLS_H
