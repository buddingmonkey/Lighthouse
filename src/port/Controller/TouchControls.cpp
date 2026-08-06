#include "TouchControls.h"

void TouchControlsWindow::Draw() {
    Lighthouse::TouchControls_Draw();
}

#ifndef __IOS__

extern "C" void TouchControls_Poll(void) {
}
extern "C" void TouchControls_MergeInto(void*) {
}

namespace Lighthouse {
void TouchControls_Draw() {
}
bool TouchControls_Active() {
    return false;
}
} // namespace Lighthouse

#else

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

#include <SDL2/SDL.h>
#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <ship/controller/controldeck/ControlDeck.h>

#include "port/UI/cvar_prefixes.h"

#define CVAR_TOUCH(var) CVAR_SETTING("TouchControls." var)

namespace {

// None of the state in this file is locked, because all of it belongs to the window thread:
// Game.cpp's main loop calls TouchControls_Poll() and then OS_SiService() (which reaches
// TouchControls_MergeInto under the SI latch mutex, on that same thread), and the gui pass
// that reaches TouchControls_Draw() runs from ServiceRcp()/RenderGuiFrame() in the same loop.
// Moving input polling onto a thread of its own means guarding everything below.

// Layout lives in "height units": the y axis spans 0..1 and x spans 0..aspect, so a
// circle is a circle and one set of constants suits both phone and tablet aspect ratios.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

enum PadControl {
    CTRL_A,
    CTRL_B,
    CTRL_Z,
    CTRL_L,
    CTRL_R,
    CTRL_START,
    CTRL_CUP,
    CTRL_CDOWN,
    CTRL_CLEFT,
    CTRL_CRIGHT,
    CTRL_DUP,
    CTRL_DDOWN,
    CTRL_DLEFT,
    CTRL_DRIGHT,
    CTRL_COUNT,
};

struct Button {
    Vec2 center;
    float radius = 0.0f;
    // Pills (shoulders, Start) are drawn as rounded rects; radius stays the hit radius.
    Vec2 halfExtent;
    bool pill = false;
    uint16_t mask = 0;
    const char* label = "";
    bool enabled = true;
};

struct Layout {
    float aspect = 16.0f / 9.0f;
    std::array<Button, CTRL_COUNT> buttons{};
    Vec2 stickHome;
    float stickBase = 0.0f;
    float stickKnob = 0.0f;
    // Anything inside this rect that isn't a button grabs the stick.
    Vec2 stickZoneMin;
    Vec2 stickZoneMax;
    Vec2 menuCenter;
    Vec2 menuHalfExtent;
};

// A finger keeps whatever it first grabbed until it lifts, so sliding off a button
// doesn't drop the input mid-jump.
constexpr int kTargetNone = -1;
constexpr int kTargetStick = -2;
constexpr int kTargetMenu = -3;

struct Finger {
    SDL_FingerID id = 0;
    int target = kTargetNone;
    Vec2 pos;
};

struct State {
    uint16_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    bool stickActive = false;
    Vec2 stickBase;
    Vec2 stickKnob;
    std::array<bool, CTRL_COUNT> pressed{};
    bool menuPressed = false;
};

State sState;
std::vector<Finger> sFingers;
Layout sLayout;
bool sLayoutValid = false;
bool sMenuLatch = false;
// Refreshed once per poll: PadActive() is also called from the SI service, which holds the
// pad mutex the game thread waits on, so it must not walk SDL's joystick list.
bool sGamepadPresent = false;
// The finger currently driving the stick, and where it first landed.
bool sStickHeld = false;
SDL_FingerID sStickFinger = 0;
Vec2 sStickOrigin;

float Dist(const Vec2& a, const Vec2& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool GamepadConnected() {
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (SDL_IsGameController(i)) {
            return true;
        }
    }
    return false;
}

bool MenuVisible() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr || ctx->GetWindow()->GetGui() == nullptr) {
        return false;
    }
    return ctx->GetWindow()->GetGui()->GetMenuOrMenubarVisible();
}

// True when the pad itself should be drawn and polled. The menu button outlives it so
// there is always a way back into the settings.
bool PadActive() {
    if (!Lighthouse::TouchControls_Active() || MenuVisible()) {
        return false;
    }
    if (CVarGetInteger(CVAR_TOUCH("HideWithGamepad"), 1) && sGamepadPresent) {
        return false;
    }
    return true;
}

void AddButton(Layout& l, PadControl id, Vec2 center, float radius, uint16_t mask, const char* label) {
    Button& b = l.buttons[id];
    b.center = center;
    b.radius = radius;
    b.halfExtent = { radius, radius };
    b.pill = false;
    b.mask = mask;
    b.label = label;
    b.enabled = true;
}

void AddPill(Layout& l, PadControl id, Vec2 center, Vec2 half, uint16_t mask, const char* label) {
    Button& b = l.buttons[id];
    b.center = center;
    b.halfExtent = half;
    b.radius = std::max(half.x, half.y);
    b.pill = true;
    b.mask = mask;
    b.label = label;
    b.enabled = true;
}

// Every CVar read here must also be mirrored in EnsureLayout's change check.
void BuildLayout(float aspect) {
    Layout l;
    l.aspect = aspect;
    // 1.2 is the largest scale at which no two controls overlap on any supported
    // aspect ratio, D-pad enabled.
    const float s = std::clamp(CVarGetFloat(CVAR_TOUCH("Scale"), 1.0f), 0.6f, 1.2f);
    const float inset = std::clamp(CVarGetFloat(CVAR_TOUCH("EdgeInset"), 0.05f), 0.0f, 0.15f);
    const float right = aspect - inset;

    // Shoulders and Start ride the top edge, clear of both thumbs.
    const Vec2 pill = { 0.095f * s, 0.052f * s };
    AddPill(l, CTRL_L, { inset + pill.x, inset + pill.y }, pill, BTN_L, "L");
    AddPill(l, CTRL_Z, { inset + pill.x * 3.4f, inset + pill.y }, pill, BTN_Z, "Z");
    AddPill(l, CTRL_R, { right - pill.x, inset + pill.y }, pill, BTN_R, "R");
    AddPill(l, CTRL_START, { right - pill.x * 3.4f, inset + pill.y }, pill, BTN_START, "START");

    l.menuHalfExtent = { 0.085f * s, 0.045f * s };
    l.menuCenter = { aspect * 0.5f, inset + l.menuHalfExtent.y };

    // Left thumb: floating analog stick anchored bottom-left.
    l.stickBase = 0.16f * s;
    l.stickKnob = 0.075f * s;
    l.stickHome = { inset + l.stickBase + 0.06f, 1.0f - inset - l.stickBase - 0.05f };
    l.stickZoneMin = { 0.0f, 0.30f };
    l.stickZoneMax = { aspect * 0.48f, 1.0f };

    // Right thumb: A/B, with the C cluster above them.
    AddButton(l, CTRL_A, { right - 0.13f * s, 1.0f - inset - 0.13f * s }, 0.10f * s, BTN_A, "A");
    AddButton(l, CTRL_B, { right - 0.33f * s, 1.0f - inset - 0.20f * s }, 0.085f * s, BTN_B, "B");

    const Vec2 c = { right - 0.16f * s, 1.0f - inset - 0.47f * s };
    const float cOff = 0.088f * s;
    const float cRad = 0.052f * s;
    AddButton(l, CTRL_CUP, { c.x, c.y - cOff }, cRad, BTN_CUP, "C");
    AddButton(l, CTRL_CDOWN, { c.x, c.y + cOff }, cRad, BTN_CDOWN, "C");
    AddButton(l, CTRL_CLEFT, { c.x - cOff, c.y }, cRad, BTN_CLEFT, "C");
    AddButton(l, CTRL_CRIGHT, { c.x + cOff, c.y }, cRad, BTN_CRIGHT, "C");

    // The D-pad is unused by most of the game, so it is opt-in. It sits above the stick,
    // anchored below the shoulder pills; the stick's capture zone is then pushed below it
    // so the two can't fight over a touch at any scale (buttons win in ClassifyTouch).
    const bool dpad = CVarGetInteger(CVAR_TOUCH("ShowDPad"), 0) != 0;
    const float dOff = 0.082f * s;
    const float dRad = 0.048f * s;
    const Vec2 d = { inset + 0.15f * s, inset + pill.y * 2.0f + dOff + dRad + 0.02f };
    if (dpad) {
        l.stickZoneMin.y = std::max(l.stickZoneMin.y, d.y + dOff + dRad + 0.01f);
    }
    AddButton(l, CTRL_DUP, { d.x, d.y - dOff }, dRad, BTN_DUP, "");
    AddButton(l, CTRL_DDOWN, { d.x, d.y + dOff }, dRad, BTN_DDOWN, "");
    AddButton(l, CTRL_DLEFT, { d.x - dOff, d.y }, dRad, BTN_DLEFT, "");
    AddButton(l, CTRL_DRIGHT, { d.x + dOff, d.y }, dRad, BTN_DRIGHT, "");
    for (int i = CTRL_DUP; i <= CTRL_DRIGHT; i++) {
        l.buttons[i].enabled = dpad;
    }

    sLayout = l;
    sLayoutValid = true;
}

// BuildLayout's inputs as of the last build, so rotating the device or dragging a settings
// slider rebuilds and an ordinary frame does not.
float sBuiltAspect = 0.0f;
float sBuiltScale = 0.0f;
float sBuiltInset = 0.0f;
int sBuiltDPad = 0;

void EnsureLayout(float aspect) {
    const float scale = CVarGetFloat(CVAR_TOUCH("Scale"), 1.0f);
    const float inset = CVarGetFloat(CVAR_TOUCH("EdgeInset"), 0.05f);
    const int dpad = CVarGetInteger(CVAR_TOUCH("ShowDPad"), 0);
    if (sLayoutValid && aspect == sBuiltAspect && scale == sBuiltScale && inset == sBuiltInset && dpad == sBuiltDPad) {
        return;
    }
    sBuiltAspect = aspect;
    sBuiltScale = scale;
    sBuiltInset = inset;
    sBuiltDPad = dpad;
    BuildLayout(aspect);
}

// Hit slop: how far past a control's drawn edge a touch still counts. Pushed past the gaps
// BuildLayout leaves, neighbouring controls overlap and ClassifyTouch picks the nearer one.
constexpr float kPillSlopX = 1.15f;
constexpr float kPillSlopY = 1.35f;
constexpr float kCircleSlop = 1.2f;
constexpr float kMenuSlopX = 1.2f;
constexpr float kMenuSlopY = 1.4f;
// N64 sticks read a little past 80 at the octagon corners; 80 is the safe full-range value.
constexpr float kStickRange = 80.0f;

bool HitsButton(const Button& b, const Vec2& p) {
    if (!b.enabled) {
        return false;
    }
    if (b.pill) {
        // A little slop so shoulder taps near the screen edge still register.
        return std::abs(p.x - b.center.x) <= b.halfExtent.x * kPillSlopX &&
               std::abs(p.y - b.center.y) <= b.halfExtent.y * kPillSlopY;
    }
    return Dist(p, b.center) <= b.radius * kCircleSlop;
}

bool HitsMenu(const Vec2& p) {
    return std::abs(p.x - sLayout.menuCenter.x) <= sLayout.menuHalfExtent.x * kMenuSlopX &&
           std::abs(p.y - sLayout.menuCenter.y) <= sLayout.menuHalfExtent.y * kMenuSlopY;
}

int ClassifyTouch(const Vec2& p, bool padActive, bool menuButtonActive) {
    if (menuButtonActive && HitsMenu(p)) {
        return kTargetMenu;
    }
    if (!padActive) {
        return kTargetNone;
    }
    // Nearest button wins, so overlapping hit slop can't double-fire.
    int best = kTargetNone;
    float bestDist = 0.0f;
    for (int i = 0; i < CTRL_COUNT; i++) {
        const Button& b = sLayout.buttons[i];
        if (!HitsButton(b, p)) {
            continue;
        }
        // Zero distance means a pill wins any tie by construction; today no pill overlaps a
        // circle at any scale BuildLayout allows, so a layout change should recheck that.
        const float d = b.pill ? 0.0f : Dist(p, b.center);
        if (best == kTargetNone || d < bestDist) {
            best = i;
            bestDist = d;
        }
    }
    if (best != kTargetNone) {
        return best;
    }
    if (p.x >= sLayout.stickZoneMin.x && p.x <= sLayout.stickZoneMax.x && p.y >= sLayout.stickZoneMin.y &&
        p.y <= sLayout.stickZoneMax.y) {
        return kTargetStick;
    }
    return kTargetNone;
}

void OpenMenu() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr || ctx->GetWindow()->GetGui() == nullptr) {
        return;
    }
    auto menu = ctx->GetWindow()->GetGui()->GetMenu();
    if (menu != nullptr) {
        menu->ToggleVisibility();
        // Matches what the Escape shortcut does in Gui::DrawElement.
        ctx->GetWindow()->GetMouseStateManager()->UpdateMouseCapture();
    }
}

} // namespace

extern "C" void TouchControls_Poll(void) {
    if (!Lighthouse::TouchControls_Active()) {
        sFingers.clear();
        sState = State{};
        sStickHeld = false;
        sMenuLatch = false;
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float h = io.DisplaySize.y;
    const float w = io.DisplaySize.x;
    if (h <= 0.0f || w <= 0.0f) {
        // Happens while backgrounded or mid-rotation; releasing everything beats
        // leaving a button latched into the pad.
        sFingers.clear();
        sState = State{};
        sStickHeld = false;
        sMenuLatch = false;
        return;
    }
    EnsureLayout(w / h);

    sGamepadPresent = GamepadConnected();
    const bool padActive = PadActive();
    // While the menu is open its own close button takes over: leaving ours on top would
    // also click whatever menu widget sits underneath, since SDL mirrors touches as mouse.
    const bool menuButtonActive = !MenuVisible();

    // Collect the fingers SDL currently reports. Positions are normalized to the window,
    // which sidesteps the point/pixel difference between ImGui and the drawable.
    std::vector<Finger> live;
    const int deviceCount = SDL_GetNumTouchDevices();
    for (int d = 0; d < deviceCount; d++) {
        const SDL_TouchID device = SDL_GetTouchDevice(d);
        const int fingerCount = SDL_GetNumTouchFingers(device);
        for (int f = 0; f < fingerCount; f++) {
            SDL_Finger* finger = SDL_GetTouchFinger(device, f);
            if (finger == nullptr) {
                continue;
            }
            Finger entry;
            entry.id = finger->id;
            entry.pos = { finger->x * sLayout.aspect, finger->y };
            live.push_back(entry);
        }
    }

    // Carry assignments forward; classify only newly-landed fingers.
    for (auto& finger : live) {
        auto prev = std::find_if(sFingers.begin(), sFingers.end(),
                                 [&](const Finger& old) { return old.id == finger.id; });
        if (prev != sFingers.end()) {
            finger.target = prev->target;
        } else {
            finger.target = ClassifyTouch(finger.pos, padActive, menuButtonActive);
        }
    }
    sFingers = std::move(live);

    State next;
    next.stickBase = sLayout.stickHome;
    next.stickKnob = sLayout.stickHome;

    bool menuHeld = false;
    const Finger* stickFinger = nullptr;
    for (const auto& finger : sFingers) {
        if (finger.target == kTargetMenu) {
            menuHeld = true;
        } else if (finger.target == kTargetStick) {
            // The finger that already owns the stick keeps it; otherwise the first one claims it.
            if (stickFinger == nullptr || (sStickHeld && finger.id == sStickFinger)) {
                stickFinger = &finger;
            }
        } else if (finger.target >= 0 && finger.target < CTRL_COUNT) {
            next.pressed[finger.target] = true;
            next.buttons |= sLayout.buttons[finger.target].mask;
        }
    }

    if (stickFinger != nullptr) {
        // Floating stick: the base sits where this finger first touched down.
        if (!sStickHeld || sStickFinger != stickFinger->id) {
            sStickHeld = true;
            sStickFinger = stickFinger->id;
            // Keep the base a full radius from the edges, or a finger that lands near one
            // could never reach full deflection towards it.
            const float r = sLayout.stickBase;
            sStickOrigin = { std::clamp(stickFinger->pos.x, r, sLayout.aspect - r),
                             std::clamp(stickFinger->pos.y, r, 1.0f - r) };
        }
        next.stickActive = true;
        next.stickBase = sStickOrigin;
        next.stickKnob = stickFinger->pos;
    } else {
        sStickHeld = false;
    }

    if (next.stickActive) {
        float dx = next.stickKnob.x - next.stickBase.x;
        float dy = next.stickKnob.y - next.stickBase.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const float maxLen = sLayout.stickBase;
        if (len > maxLen && len > 0.0f) {
            dx *= maxLen / len;
            dy *= maxLen / len;
            next.stickKnob = { next.stickBase.x + dx, next.stickBase.y + dy };
        }
        const float mag = std::min(len / maxLen, 1.0f);
        const float deadzone = std::clamp(CVarGetFloat(CVAR_TOUCH("Deadzone"), 0.12f), 0.0f, 0.5f);
        if (mag > deadzone && len > 0.0f) {
            // Rescale past the deadzone so the first pixel of travel isn't wasted.
            const float scaled = (mag - deadzone) / (1.0f - deadzone) * kStickRange;
            next.stickX = (int8_t)std::lround(std::clamp(dx / len * scaled, -kStickRange, kStickRange));
            // Screen y grows downward, the N64 stick's does not.
            next.stickY = (int8_t)std::lround(std::clamp(-dy / len * scaled, -kStickRange, kStickRange));
        }
    }

    next.menuPressed = menuHeld;
    // Toggle on release so a held finger doesn't flip the menu every frame.
    if (sMenuLatch && !menuHeld) {
        OpenMenu();
    }
    sMenuLatch = menuHeld;

    sState = next;
}

extern "C" void TouchControls_MergeInto(void* contPad) {
    if (contPad == nullptr || !PadActive()) {
        return;
    }
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetControlDeck() == nullptr || ctx->GetControlDeck()->GamepadGameInputBlocked()) {
        return;
    }

    OSContPad* pad = static_cast<OSContPad*>(contPad);
    pad->button |= sState.buttons;
    // Leave a physical stick in charge when it is being used.
    if (pad->stick_x == 0 && pad->stick_y == 0) {
        pad->stick_x = sState.stickX;
        pad->stick_y = sState.stickY;
    }
}

namespace Lighthouse {

bool TouchControls_Active() {
    return CVarGetInteger(CVAR_TOUCH("Enabled"), 1) != 0;
}

namespace {
ImU32 Shade(float alpha, bool pressed) {
    const float a = pressed ? std::min(alpha * 2.0f, 0.95f) : alpha;
    return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a));
}

void DrawLabel(ImDrawList* dl, const char* text, ImVec2 center, float size) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f),
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.9f)), text);
}
} // namespace

void TouchControls_Draw() {
    if (!TouchControls_Active()) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float h = io.DisplaySize.y;
    if (h <= 0.0f || !sLayoutValid) {
        return;
    }
    // Height units map to pixels by a single factor; see BuildLayout.
    const auto px = [h](const Vec2& v) { return ImVec2(v.x * h, v.y * h); };

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float alpha = std::clamp(CVarGetFloat(CVAR_TOUCH("Opacity"), 0.4f), 0.05f, 1.0f);

    // Drawn even when the pad is hidden, so the settings stay reachable -- but not over
    // the menu itself, which has its own close button.
    if (!MenuVisible()) {
        const ImVec2 menuMin = px({ sLayout.menuCenter.x - sLayout.menuHalfExtent.x,
                                    sLayout.menuCenter.y - sLayout.menuHalfExtent.y });
        const ImVec2 menuMax = px({ sLayout.menuCenter.x + sLayout.menuHalfExtent.x,
                                    sLayout.menuCenter.y + sLayout.menuHalfExtent.y });
        const float menuRound = (menuMax.y - menuMin.y) * 0.35f;
        dl->AddRectFilled(menuMin, menuMax, Shade(alpha * 0.6f, sState.menuPressed), menuRound);
        dl->AddRect(menuMin, menuMax, Shade(alpha, sState.menuPressed), menuRound, 0, h * 0.004f);
        DrawLabel(dl, "MENU", px(sLayout.menuCenter), sLayout.menuHalfExtent.y * 1.1f * h);
    }

    if (!PadActive()) {
        return;
    }

    for (int i = 0; i < CTRL_COUNT; i++) {
        const Button& b = sLayout.buttons[i];
        if (!b.enabled) {
            continue;
        }
        const bool pressed = sState.pressed[i];
        if (b.pill) {
            const ImVec2 min = px({ b.center.x - b.halfExtent.x, b.center.y - b.halfExtent.y });
            const ImVec2 max = px({ b.center.x + b.halfExtent.x, b.center.y + b.halfExtent.y });
            const float round = (max.y - min.y) * 0.45f;
            dl->AddRectFilled(min, max, Shade(alpha * 0.55f, pressed), round);
            dl->AddRect(min, max, Shade(alpha, pressed), round, 0, h * 0.004f);
        } else {
            const ImVec2 c = px(b.center);
            const float r = b.radius * h;
            dl->AddCircleFilled(c, r, Shade(alpha * 0.55f, pressed), 32);
            dl->AddCircle(c, r, Shade(alpha, pressed), 32, h * 0.004f);
        }
        // Pill labels are multi-character, so size them off the pill's height, not its width.
        DrawLabel(dl, b.label, px(b.center), (b.pill ? b.halfExtent.y * 1.1f : b.radius * 0.9f) * h);
    }

    // Stick: base ring plus knob. Idle it sits at its home position as a target.
    const ImVec2 base = px(sState.stickBase);
    const ImVec2 knob = px(sState.stickKnob);
    dl->AddCircleFilled(base, sLayout.stickBase * h, Shade(alpha * 0.35f, false), 48);
    dl->AddCircle(base, sLayout.stickBase * h, Shade(alpha, false), 48, h * 0.004f);
    dl->AddCircleFilled(knob, sLayout.stickKnob * h, Shade(alpha * 0.9f, sState.stickActive), 32);
}

} // namespace Lighthouse

#endif // __IOS__
