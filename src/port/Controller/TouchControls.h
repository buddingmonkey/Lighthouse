#ifndef TOUCH_CONTROLS_H
#define TOUCH_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

void TouchControls_Poll(void);

void TouchControls_MergeInto(void* contPad);

void TouchControls_OpenMenu(void);

#ifdef __cplusplus
}

#include <libultraship/libultraship.h>

class TouchControlsWindow final : public Ship::GuiWindow {
public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override{};
    void UpdateElement() override{};
    void Draw() override;
};

namespace Lighthouse {
void TouchControls_Draw();
bool TouchControls_Active();
} // namespace Lighthouse
#endif

#endif // TOUCH_CONTROLS_H
