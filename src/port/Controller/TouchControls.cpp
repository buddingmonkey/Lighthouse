#include "TouchControls.h"

void TouchControlsWindow::Draw() {
    Lighthouse::TouchControls_Draw();
}

#ifndef LIGHTHOUSE_TOUCH_CONTROLS

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

#ifdef __ANDROID__
#include <atomic>
#include <jni.h>
#endif

#include <SDL2/SDL.h>
#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <ship/controller/controldeck/ControlDeck.h>

#include "port/Controller/ControlSchemes.h"
#include "port/UI/cvar_prefixes.h"

#define CVAR_TOUCH(var) CVAR_SETTING("TouchControls." var)

namespace {

// None of the state in this file is locked, because all of it belongs to the window thread:
// Game.cpp's main loop calls TouchControls_Poll() and then OS_SiService() (which reaches
// TouchControls_MergeInto under the SI latch mutex, on that same thread), and the gui pass
// that reaches TouchControls_Draw() runs from ServiceRcp()/RenderGuiFrame() in the same loop.
// Moving input polling onto a thread of its own means guarding everything below.

// Layout lives in "height units": the y axis spans 0..1 and x spans 0..aspect, so a circle is a
// circle. Every constant below is authored in millimetres and converted once, so a control keeps
// its physical size whether the screen is 60 mm tall or 170.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// What the OS keeps for itself and the pad must stay off: a display cutout, and any system bar
// that is still on screen. Height units, like everything else here.
struct Insets {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;

    bool operator==(const Insets&) const = default;
};

// Where the four edges of the usable screen sit, once the margin and the insets are taken off it.
struct Bounds {
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// A phone is held with the index fingers curled over the top edge and both thumbs in the bottom
// corners. A tablet is held the same way, but its top corners are a hand's width out of reach, so
// the shoulder rail runs down the sides instead. Sizing physically covers every other difference.
enum DeviceClass {
    DEVICE_PHONE,
    DEVICE_TABLET,
};

// Ordered so that each group of controls that has to be placed, and kept on screen, as a unit is
// a contiguous run: the right thumb's face buttons, then the D-pad, then the shoulder rail.
enum PadControl {
    CTRL_A,
    CTRL_B,
    CTRL_CUP,
    CTRL_CDOWN,
    CTRL_CLEFT,
    CTRL_CRIGHT,
    CTRL_DUP,
    CTRL_DDOWN,
    CTRL_DLEFT,
    CTRL_DRIGHT,
    CTRL_Z,
    CTRL_L,
    CTRL_R,
    CTRL_START,
    CTRL_COUNT,
};

// Hit slop: how far past a control's drawn edge a touch still counts. Neighbours overlap once it
// is pushed past the gaps BuildLayout leaves, and ClassifyTouch picks whichever is hit deeper.
constexpr float kPillSlopX = 1.15f;
constexpr float kPillSlopY = 1.35f;
constexpr float kCircleSlop = 1.2f;
// Four buttons in a diamond have to be drawn small to fit, so the drawn disc is a target to aim
// at rather than the edge of what registers; ClassifyTouch settles where two of them meet.
constexpr float kClusterSlop = 1.55f;
constexpr float kMenuSlopX = 1.2f;
constexpr float kMenuSlopY = 1.4f;
// N64 sticks read a little past 80 at the octagon corners; 80 is the safe full-range value.
constexpr float kStickRange = 80.0f;
// A gamepad's right stick reaches MAX_AXIS_RANGE, and the camera's thresholds are set against
// that, so the touch stick has to agree or it reads as a different stick.
constexpr float kRightStickRange = 85.0f;
// What a gamepad's stick-to-button mappings fire at, as a fraction of full deflection.
constexpr float kAxisButtonThreshold = 0.25f;

struct Button {
    Vec2 center;
    float radius = 0.0f;
    // Pills (shoulders, Start) are drawn as rounded rects; radius stays the hit radius.
    Vec2 halfExtent;
    bool pill = false;
    float slop = kCircleSlop;
    uint16_t mask = 0;
    const char* label = "";
    // Way the face points, for the ones drawn as an arrow rather than lettered. Held per
    // button rather than worked out from the cluster, so mirroring cannot turn it around.
    Vec2 arrow;
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
    // Modern drives the camera from a right stick, so there the C diamond becomes one.
    bool rightStick = false;
    Vec2 rightStickHome;
    float rightStickBase = 0.0f;
    float rightStickKnob = 0.0f;
};

// A finger keeps whatever it first grabbed until it lifts, so sliding off a button
// doesn't drop the input mid-jump.
constexpr int kTargetNone = -1;
constexpr int kTargetStick = -2;
constexpr int kTargetMenu = -3;
constexpr int kTargetRightStick = -4;

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
    int8_t rightX = 0;
    int8_t rightY = 0;
    bool rightActive = false;
    Vec2 rightKnob;
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
// The right stick has a fixed base, so its finger needs no origin.
bool sRightHeld = false;
SDL_FingerID sRightFinger = 0;

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
    b.slop = kCircleSlop;
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
    b.slop = kCircleSlop;
    b.mask = mask;
    b.label = label;
    b.enabled = true;
}

#ifdef __ANDROID__
enum InsetEdge {
    INSET_LEFT,
    INSET_TOP,
    INSET_RIGHT,
    INSET_BOTTOM,
    INSET_COUNT,
};

// Written by the activity's inset listener on the UI thread, read by the window thread. Pixels,
// which is what SDL sizes an Android window in.
std::atomic<int> sInsetPx[INSET_COUNT] = {};
#endif

// The screen, less whatever the OS reserves along its edges.
Insets SafeArea(float pointHeight) {
#ifdef __ANDROID__
    if (pointHeight <= 0.0f) {
        return {};
    }
    const auto edge = [pointHeight](InsetEdge which) {
        return sInsetPx[which].load(std::memory_order_relaxed) / pointHeight;
    };
    const Insets in = { edge(INSET_LEFT), edge(INSET_RIGHT), edge(INSET_TOP), edge(INSET_BOTTOM) };
    // Half the screen is a bad reading rather than a real cutout, and the pad is better off in the
    // wrong place than crushed into nothing.
    if (in.left + in.right >= 0.5f || in.top + in.bottom >= 0.5f) {
        return {};
    }
    return in;
#else
    (void)pointHeight;
    return {};
#endif
}

// The layout works in whatever unit the platform sizes its windows in, so all it needs is how
// many of those cover a millimetre; nothing past this point deals in inches.
constexpr float kMmPerInch = 25.4f;

#ifdef __ANDROID__
// Android sizes its windows in pixels, so the plausible band is a screen density rather than
// Apple's fixed point size, and a tablet is told apart by physical width instead of a unit count.
constexpr float kMinPlausiblePpi = 200.0f;
constexpr float kMaxPlausiblePpi = 900.0f;
constexpr float kNominalPhonePpi = 400.0f;
constexpr float kNominalTabletPpi = 260.0f;
constexpr float kTabletMinShortSideMm = 100.0f;
#else
// Apple quotes the two densities in inches: 163 on every iPhone and on the iPad mini, 132 on the
// other iPads. Every landscape iPhone is at most 440 points tall and every iPad at least 744.
constexpr float kMinPlausiblePpi = 110.0f;
constexpr float kMaxPlausiblePpi = 200.0f;
constexpr float kPhonePointsPerMm = 163.0f / kMmPerInch;
constexpr float kTabletPointsPerMm = 132.0f / kMmPerInch;
constexpr float kTabletMinPointHeight = 600.0f;
#endif

// Window units per millimetre as the display reports them, or zero when the figure is not usable.
float MeasuredUnitsPerMm() {
    float ddpi = 0.0f;
    float hdpi = 0.0f;
    float vdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0) {
        return 0.0f;
    }
    const float ppi = vdpi / std::max(ImGui::GetIO().DisplayFramebufferScale.y, 1.0f);
    if (ppi < kMinPlausiblePpi || ppi > kMaxPlausiblePpi) {
        return 0.0f;
    }
    return ppi / kMmPerInch;
}

DeviceClass DeviceClassFor(float pointHeight, int setting) {
    switch (setting) {
        case 1:
            return DEVICE_PHONE;
        case 2:
            return DEVICE_TABLET;
        default:
            break;
    }
#ifdef __ANDROID__
    const float unitsPerMm = MeasuredUnitsPerMm();
    return unitsPerMm > 0.0f && pointHeight / unitsPerMm >= kTabletMinShortSideMm ? DEVICE_TABLET : DEVICE_PHONE;
#else
    return pointHeight >= kTabletMinPointHeight ? DEVICE_TABLET : DEVICE_PHONE;
#endif
}

// The per-class nominal covers devices SDL has no usable figure for; the size slider absorbs the rest.
float PointsPerMm(DeviceClass device) {
    const float measured = MeasuredUnitsPerMm();
    if (measured > 0.0f) {
        return measured;
    }
#ifdef __ANDROID__
    return (device == DEVICE_TABLET ? kNominalTabletPpi : kNominalPhonePpi) / kMmPerInch;
#else
    return device == DEVICE_TABLET ? kTabletPointsPerMm : kPhonePointsPerMm;
#endif
}

// Maps a finger's offset from the stick's base onto the N64's range. Direction comes from the
// offset as given and magnitude is capped separately, so a finger past the ring reads as full
// deflection: the ring is the edge of the gate, not a point to balance on. Deriving both from a
// pre-clamped offset instead would shrink the vector the further out the finger went, which walks
// the character when it should be running.
Vec2 StickVector(const Vec2& offset, float maxLen, float deadzone, float range) {
    const float len = std::sqrt(offset.x * offset.x + offset.y * offset.y);
    if (len <= 0.0f || maxLen <= 0.0f) {
        return {};
    }
    const float mag = std::min(len / maxLen, 1.0f);
    if (mag <= deadzone) {
        return {};
    }
    // Rescale past the deadzone so the first pixel of travel isn't wasted.
    const float scaled = (mag - deadzone) / (1.0f - deadzone) * range;
    return { offset.x / len * scaled, offset.y / len * scaled };
}

// Places a control on the arc a thumb sweeps from the corner it grips. Angles run up from the
// inboard horizontal; inboard is +1 for the left thumb and -1 for the right one.
Vec2 Arc(const Vec2& pivot, float inboard, float radius, float degrees) {
    const float radians = degrees * 3.14159265f / 180.0f;
    return { pivot.x + inboard * radius * std::cos(radians), pivot.y - radius * std::sin(radians) };
}

// Slides a run of controls as a unit until its bounding box clears the screen edges and whatever
// sits above it. Clusters move whole, so this can't deform a diamond into a trapezium.
void FitRange(Layout& l, int first, int last, const Bounds& edge, float ceiling, bool withRightStick = false) {
    Vec2 min = { FLT_MAX, FLT_MAX };
    Vec2 max = { -FLT_MAX, -FLT_MAX };
    for (int i = first; i <= last; i++) {
        const Button& b = l.buttons[i];
        if (!b.enabled) {
            continue;
        }
        min.x = std::min(min.x, b.center.x - b.halfExtent.x);
        min.y = std::min(min.y, b.center.y - b.halfExtent.y);
        max.x = std::max(max.x, b.center.x + b.halfExtent.x);
        max.y = std::max(max.y, b.center.y + b.halfExtent.y);
    }
    const bool stick = withRightStick && l.rightStick;
    if (stick) {
        min.x = std::min(min.x, l.rightStickHome.x - l.rightStickBase);
        min.y = std::min(min.y, l.rightStickHome.y - l.rightStickBase);
        max.x = std::max(max.x, l.rightStickHome.x + l.rightStickBase);
        max.y = std::max(max.y, l.rightStickHome.y + l.rightStickBase);
    }
    if (min.x > max.x) {
        return;
    }

    Vec2 shift;
    if (min.x < edge.left) {
        shift.x = edge.left - min.x;
    } else if (max.x > edge.right) {
        shift.x = edge.right - max.x;
    }
    if (min.y < ceiling) {
        shift.y = ceiling - min.y;
    } else if (max.y > edge.bottom) {
        shift.y = edge.bottom - max.y;
    }
    for (int i = first; i <= last; i++) {
        l.buttons[i].center.x += shift.x;
        l.buttons[i].center.y += shift.y;
    }
    if (stick) {
        l.rightStickHome.x += shift.x;
        l.rightStickHome.y += shift.y;
    }
}

// Puts the stick under the right thumb and the face buttons under the left.
void MirrorLayout(Layout& l) {
    for (Button& b : l.buttons) {
        b.center.x = l.aspect - b.center.x;
    }
    // The clusters are directional, so swap their horizontal pairs back: mirroring the screen
    // shouldn't leave C-left sitting on the right of its own diamond.
    std::swap(l.buttons[CTRL_CLEFT].center, l.buttons[CTRL_CRIGHT].center);
    std::swap(l.buttons[CTRL_DLEFT].center, l.buttons[CTRL_DRIGHT].center);

    const float zoneMin = l.aspect - l.stickZoneMax.x;
    l.stickZoneMax.x = l.aspect - l.stickZoneMin.x;
    l.stickZoneMin.x = zoneMin;
    l.stickHome.x = l.aspect - l.stickHome.x;
    l.rightStickHome.x = l.aspect - l.rightStickHome.x;
}

// The dimensions that decide how tall the pad is, in millimetres: A's radius, how far above A the
// C cluster's centre sits, the diamond's own half-height and radius, and the shoulder rail a phone
// stacks above all of it. BuildLayout both places the controls from these and works out from them
// how large the pad is allowed to get, so they are named rather than written out twice.
constexpr float kARadiusMm = 6.5f;
constexpr float kACGapMm = 20.0f;
constexpr float kCOffYMm = 6.0f;
constexpr float kCRadiusMm = 3.8f;
constexpr float kZPillHalfYMm = 4.7f;
constexpr float kRailGapMm = 2.0f;
constexpr float kFaceMm = kARadiusMm + kACGapMm + kCOffYMm + kCRadiusMm;
constexpr float kRailMm = kZPillHalfYMm * 2.0f + kRailGapMm;
// The ring Modern puts in the diamond's place, and the face height that costs instead.
constexpr float kRightStickBaseMm = 8.5f;
constexpr float kModernFaceMm = kARadiusMm + kACGapMm + kRightStickBaseMm;

// Everything BuildLayout reads. Comparing the struct whole is what stops a new setting from
// silently failing to trigger a rebuild.
struct LayoutKey {
    float aspect = 0.0f;
    float pointHeight = 0.0f;
    float size = 0.0f;
    float reach = 0.0f;
    float margin = 0.0f;
    Insets insets;
    int device = 0;
    int dpad = 0;
    int mirror = 0;
    int scheme = 0;
    // Only moves the menu button, but it still belongs here: a gamepad connecting mid-session has
    // to rebuild the layout for that to take effect.
    int padHidden = 0;

    bool operator==(const LayoutKey&) const = default;
};

void BuildLayout(const LayoutKey& key) {
    Layout l;
    l.aspect = key.aspect;

    const DeviceClass device = DeviceClassFor(key.pointHeight, key.device);
    // Height units per millimetre. Every dimension below goes through one of the three lambdas, so
    // no constant in this function is a fraction of the screen.
    const float unit = PointsPerMm(device) / key.pointHeight;

    // The face group sitting under the shoulder rail is the tallest run of controls on screen, so
    // on a short screen that, rather than the slider, decides how large the pad can get. A phone
    // held in landscape has barely 55 mm to spend, and saturating the slider there beats letting
    // the controls grow into one another.
    const bool modern = key.scheme == CONTROL_SCHEME_MODERN;
    const float railMm = device == DEVICE_PHONE ? kRailMm : 0.0f;
    const float budget = (1.0f - key.insets.top - key.insets.bottom) / unit - 2.0f * key.margin;
    const float size = std::max(std::min(key.size, budget / ((modern ? kModernFaceMm : kFaceMm) + railMm)), 0.4f);

    const auto mm = [unit](float millimetres) { return millimetres * unit; };
    const auto sz = [unit, size](float millimetres) { return millimetres * unit * size; };
    // Reach moves a group as a whole nearer the corner it is gripped from; the spacing inside the
    // group stays on size alone, so pulling the pad in can't pull it into itself.
    const auto arc = [unit, size, &key](float millimetres) { return millimetres * unit * size * key.reach; };

    const float margin = mm(key.margin);
    // MirrorLayout reflects the pad but not the menu button, so a cutout down one side is taken
    // off both. It costs a millimetre or two of a side that a notch never covers more of.
    const float side = margin + std::max(key.insets.left, key.insets.right);
    const float left = side;
    const float right = key.aspect - side;
    const float top = margin + key.insets.top;
    const float bottom = 1.0f - margin - key.insets.bottom;
    const Bounds edge = { left, right, bottom };

    const Vec2 shoulder = { sz(7.0f), sz(4.2f) };
    // Z is held down for most of a fight, so it takes the corner and a wider pill than the rest.
    const Vec2 zPill = { sz(8.5f), sz(kZPillHalfYMm) };
    const float railGap = sz(1.5f);
    // Room a side rail needs above its anchor for the second pill of each pair.
    const float railTop = std::max(zPill.y + shoulder.y * 2.0f, shoulder.y * 3.0f) + railGap;

    // 7 mm square is Apple's 44 pt minimum target at every density, which is as small as this is
    // allowed to get. It deliberately ignores the size slider: shrinking the pad shouldn't take the
    // one control that leads back to the settings below the floor with it.
    const float menuHalf = mm(3.5f);
    l.menuHalfExtent = { menuHalf, menuHalf };
    const float menuStack = menuHalf * 2.0f + railGap;

    // A side rail exists because a tablet's top corners are out of reach. That only holds while the
    // screen is tall enough to put the rail below the menu button and still within reach -- force
    // the tablet layout onto a phone and it isn't, so the top rail is the honest answer there.
    const bool sideRail = device == DEVICE_TABLET && bottom - arc(75.0f) >= top + menuStack + railTop;
    // Top centre is a gap in the shoulder rail, and only a top rail has one. A side rail, or a pad
    // hidden for a gamepad that leaves this button alone over the game, tucks it into the corner.
    const bool cornerMenu = sideRail || key.padHidden != 0;
    l.menuCenter = { cornerMenu ? left + menuHalf : key.aspect * 0.5f, top + menuHalf };
    // The menu button wins any touch it covers, because ClassifyTouch tests it first, so a rail
    // sharing its corner has to start below it.
    const float railCeiling = cornerMenu ? top + menuStack : top;

    float railBottom = top;
    if (!sideRail) {
        railBottom = top + zPill.y * 2.0f;
        AddPill(l, CTRL_Z, { left + zPill.x, top + zPill.y }, zPill, BTN_Z, "Z");
        AddPill(l, CTRL_L, { left + zPill.x * 2.0f + railGap + shoulder.x, top + shoulder.y }, shoulder, BTN_L, "L");
        AddPill(l, CTRL_R, { right - shoulder.x, top + shoulder.y }, shoulder, BTN_R, "R");
        AddPill(l, CTRL_START, { right - shoulder.x * 3.0f - railGap, top + shoulder.y }, shoulder, BTN_START, "START");
    } else {
        // An index finger reaches further up the side than a thumb reaches across the face, but on
        // a tablet nowhere near the top corner. Anchor the rail where it does land and stack the
        // second control of each pair above the first.
        const float anchor =
            std::clamp(bottom - arc(75.0f), railCeiling + railTop, std::max(railCeiling + railTop, bottom - zPill.y));
        AddPill(l, CTRL_Z, { left + zPill.x, anchor }, zPill, BTN_Z, "Z");
        AddPill(l, CTRL_L, { left + shoulder.x, anchor - zPill.y - railGap - shoulder.y }, shoulder, BTN_L, "L");
        AddPill(l, CTRL_R, { right - shoulder.x, anchor }, shoulder, BTN_R, "R");
        AddPill(l, CTRL_START, { right - shoulder.x, anchor - shoulder.y * 2.0f - railGap }, shoulder, BTN_START,
                "START");
    }

    // Right thumb. A sits where the thumb comes to rest and the rest of the group hangs off it:
    // B inboard and a little above, the way the two sit on the controller, and the C cluster
    // further inboard again rather than stacked straight up over A, which is both the direction a
    // thumb extends worst and the one a landscape screen has least of. The C diamond is wider than
    // it is tall for the same reason.
    // A thumb curls from the corner of the screen, which is below the margin the controls are
    // kept off it by. Measured from the margin instead, both groups sit a button too high.
    const float pivotDrop = sz(kARadiusMm);
    const Vec2 rightPivot = { right, bottom + pivotDrop };
    const Vec2 a = Arc(rightPivot, -1.0f, arc(22.5f), 44.0f);
    AddButton(l, CTRL_A, a, sz(kARadiusMm), BTN_A, "A");
    AddButton(l, CTRL_B, { a.x - sz(15.0f), a.y - sz(1.5f) }, sz(5.8f), BTN_B, "B");

    const Vec2 c = { a.x - sz(14.0f), a.y - sz(kACGapMm) };
    const Vec2 cOff = { sz(8.5f), sz(kCOffYMm) };
    const float cRad = sz(kCRadiusMm);
    if (modern) {
        // Camera yaw and zoom come off the stick there, so the diamond is one. C-up and C-down
        // are still their own actions -- first person, and an egg while crouched -- and go
        // inboard rather than above and below, where the ring would put C-down over B.
        l.rightStick = true;
        l.rightStickHome = c;
        l.rightStickBase = sz(kRightStickBaseMm);
        l.rightStickKnob = sz(4.2f);
        const float cInboard = l.rightStickBase + sz(1.5f) + cRad;
        AddButton(l, CTRL_CUP, { c.x - cInboard, c.y - sz(4.5f) }, cRad, BTN_CUP, "");
        AddButton(l, CTRL_CDOWN, { c.x - cInboard, c.y + sz(4.5f) }, cRad, BTN_CDOWN, "");
    } else {
        AddButton(l, CTRL_CUP, { c.x, c.y - cOff.y }, cRad, BTN_CUP, "");
        AddButton(l, CTRL_CDOWN, { c.x, c.y + cOff.y }, cRad, BTN_CDOWN, "");
        AddButton(l, CTRL_CLEFT, { c.x - cOff.x, c.y }, cRad, BTN_CLEFT, "");
        AddButton(l, CTRL_CRIGHT, { c.x + cOff.x, c.y }, cRad, BTN_CRIGHT, "");
    }
    l.buttons[CTRL_CUP].arrow = { 0.0f, -1.0f };
    l.buttons[CTRL_CDOWN].arrow = { 0.0f, 1.0f };
    l.buttons[CTRL_CLEFT].arrow = { -1.0f, 0.0f };
    l.buttons[CTRL_CRIGHT].arrow = { 1.0f, 0.0f };
    for (int i = CTRL_CUP; i <= CTRL_CRIGHT; i++) {
        l.buttons[i].slop = kClusterSlop;
        l.buttons[i].enabled = !modern || i == CTRL_CUP || i == CTRL_CDOWN;
    }

    // Left thumb. The stick floats to wherever a finger lands in its zone, so stickHome is only
    // where the ring rests while nobody is holding it.
    const Vec2 leftPivot = { left, bottom + pivotDrop };
    l.stickBase = sz(10.5f);
    l.stickKnob = sz(5.0f);
    l.stickHome = Arc(leftPivot, 1.0f, arc(26.0f), 52.0f);
    l.stickHome.x = std::clamp(l.stickHome.x, l.stickBase, std::max(l.stickBase, l.aspect - l.stickBase));
    l.stickHome.y = std::clamp(l.stickHome.y, l.stickBase, std::max(l.stickBase, 1.0f - l.stickBase));

    // The D-pad is unused by most of the game, so it is opt-in. It sits directly above the stick's
    // resting ring, the same shape as the C cluster.
    const bool dpad = key.dpad != 0;
    const Vec2 dOff = { sz(9.0f), sz(6.5f) };
    const float dRad = sz(3.7f);
    const Vec2 d = { l.stickHome.x, l.stickHome.y - l.stickBase - dOff.y - dRad - sz(kRailGapMm) };
    AddButton(l, CTRL_DUP, { d.x, d.y - dOff.y }, dRad, BTN_DUP, "");
    AddButton(l, CTRL_DDOWN, { d.x, d.y + dOff.y }, dRad, BTN_DDOWN, "");
    AddButton(l, CTRL_DLEFT, { d.x - dOff.x, d.y }, dRad, BTN_DLEFT, "");
    AddButton(l, CTRL_DRIGHT, { d.x + dOff.x, d.y }, dRad, BTN_DRIGHT, "");
    for (int i = CTRL_DUP; i <= CTRL_DRIGHT; i++) {
        l.buttons[i].enabled = dpad;
        l.buttons[i].slop = kClusterSlop;
    }

    // Nothing above guarantees a control lands on screen or clear of the rail at every size and
    // reach, so square that up here rather than picking constants that suit the worst case and
    // waste room in every other one. Each group moves whole, so this can't deform a diamond or
    // shear the face buttons apart.
    const float clusterCeiling = railBottom + sz(kRailGapMm);
    for (int i = CTRL_Z; i <= CTRL_START; i++) {
        FitRange(l, i, i, edge, top);
    }
    FitRange(l, CTRL_A, CTRL_CRIGHT, edge, clusterCeiling, true);
    FitRange(l, CTRL_DUP, CTRL_DRIGHT, edge, clusterCeiling);

    // Anything inside the zone that isn't a button grabs the stick. Bound it by what a thumb can
    // sweep without the hand leaving the corner; the fractional cap only bites on a phone, where
    // that sweep covers most of the screen anyway.
    const float sweep = arc(60.0f);
    l.stickZoneMin = { 0.0f, std::max(bottom - sweep, 0.28f) };
    l.stickZoneMax = { std::min(left + sweep, key.aspect * 0.45f), 1.0f };
    if (dpad) {
        // Buttons win in ClassifyTouch either way, but keeping the zone off the D-pad means a
        // touch aimed between two of its buttons doesn't become a stick grab.
        const float dpadBottom = l.buttons[CTRL_DDOWN].center.y + dRad;
        l.stickZoneMin.y = std::max(l.stickZoneMin.y, dpadBottom + sz(1.0f));
    }

    if (key.mirror != 0) {
        MirrorLayout(l);
    }

    sLayout = l;
    sLayoutValid = true;
}

// BuildLayout's inputs as of the last build, so rotating the device or dragging a settings slider
// rebuilds and an ordinary frame does not.
LayoutKey sBuiltKey;

void EnsureLayout(float aspect, float pointHeight) {
    LayoutKey key;
    key.aspect = aspect;
    key.pointHeight = pointHeight;
    key.size = std::clamp(CVarGetFloat(CVAR_TOUCH("Scale"), 1.0f), 0.7f, 1.4f);
    key.reach = std::clamp(CVarGetFloat(CVAR_TOUCH("Reach"), 1.0f), 0.8f, 1.25f);
    key.margin = std::clamp(CVarGetFloat(CVAR_TOUCH("EdgeMargin"), 3.0f), 0.0f, 10.0f);
    key.insets = SafeArea(pointHeight);
    key.device = CVarGetInteger(CVAR_TOUCH("Layout"), 0);
    key.dpad = CVarGetInteger(CVAR_TOUCH("ShowDPad"), 0);
    key.mirror = CVarGetInteger(CVAR_TOUCH("Mirror"), 0);
    key.scheme = CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_RETRO);
    // Deliberately not PadActive(): that also goes false while the menu is open, which would move
    // the button out from under the finger that just opened it.
    key.padHidden = CVarGetInteger(CVAR_TOUCH("HideWithGamepad"), 1) && sGamepadPresent ? 1 : 0;
    if (sLayoutValid && key == sBuiltKey) {
        return;
    }
    sBuiltKey = key;
    BuildLayout(key);
}

// How far inside its slop region a touch landed: 1 at the centre, 0 at the edge, negative outside.
// Pills and circles are measured the same way so the two can be compared where they sit adjacent,
// which they do on a tablet, where the shoulder rail runs down the side past the face buttons.
float HitDepth(const Button& b, const Vec2& p) {
    if (b.pill) {
        if (b.halfExtent.x <= 0.0f || b.halfExtent.y <= 0.0f) {
            return -1.0f;
        }
        // A little slop so shoulder taps near the screen edge still register.
        const float dx = std::abs(p.x - b.center.x) / (b.halfExtent.x * kPillSlopX);
        const float dy = std::abs(p.y - b.center.y) / (b.halfExtent.y * kPillSlopY);
        return 1.0f - std::max(dx, dy);
    }
    if (b.radius <= 0.0f) {
        return -1.0f;
    }
    return 1.0f - Dist(p, b.center) / (b.radius * b.slop);
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
    // Deepest hit wins, so overlapping slop can't double-fire, and a touch between two controls
    // goes to the one it is further inside rather than to whichever came first in the array.
    int best = kTargetNone;
    float bestDepth = 0.0f;
    for (int i = 0; i < CTRL_COUNT; i++) {
        const Button& b = sLayout.buttons[i];
        if (!b.enabled) {
            continue;
        }
        const float depth = HitDepth(b, p);
        if (depth < 0.0f) {
            continue;
        }
        if (best == kTargetNone || depth > bestDepth) {
            best = i;
            bestDepth = depth;
        }
    }
    if (best != kTargetNone) {
        return best;
    }
    // The right stick is grabbed from its ring rather than a zone: the buttons it sits among
    // are close enough that a zone would swallow them.
    if (sLayout.rightStick && Dist(p, sLayout.rightStickHome) <= sLayout.rightStickBase * kCircleSlop) {
        return kTargetRightStick;
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

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_com_harbormasters_lighthouse_LighthouseActivity_nativeSafeAreaInsets(
    JNIEnv*, jclass, jint left, jint top, jint right, jint bottom) {
    const jint values[INSET_COUNT] = { left, top, right, bottom };
    for (int i = 0; i < INSET_COUNT; i++) {
        sInsetPx[i].store(std::max<int>(values[i], 0), std::memory_order_relaxed);
    }
}
#endif

extern "C" void TouchControls_Poll(void) {
    if (!Lighthouse::TouchControls_Active()) {
        sFingers.clear();
        sState = State{};
        sStickHeld = false;
        sRightHeld = false;
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
        sRightHeld = false;
        sMenuLatch = false;
        return;
    }
    // Refreshed before EnsureLayout, which folds it into the layout key to place the menu button.
    sGamepadPresent = GamepadConnected();
    // DisplaySize is in points -- ImGui's SDL2 backend takes it from SDL_GetWindowSize -- which is
    // what lets the layout be sized in millimetres.
    EnsureLayout(w / h, h);

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
        auto prev =
            std::find_if(sFingers.begin(), sFingers.end(), [&](const Finger& old) { return old.id == finger.id; });
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
    next.rightKnob = sLayout.rightStickHome;

    bool menuHeld = false;
    const Finger* stickFinger = nullptr;
    const Finger* rightFinger = nullptr;
    for (const auto& finger : sFingers) {
        if (finger.target == kTargetMenu) {
            menuHeld = true;
        } else if (finger.target == kTargetStick) {
            // The finger that already owns the stick keeps it; otherwise the first one claims it.
            if (stickFinger == nullptr || (sStickHeld && finger.id == sStickFinger)) {
                stickFinger = &finger;
            }
        } else if (finger.target == kTargetRightStick) {
            if (rightFinger == nullptr || (sRightHeld && finger.id == sRightFinger)) {
                rightFinger = &finger;
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
        const Vec2 offset = { next.stickKnob.x - next.stickBase.x, next.stickKnob.y - next.stickBase.y };
        const float maxLen = sLayout.stickBase;
        const float len = std::sqrt(offset.x * offset.x + offset.y * offset.y);
        if (len > maxLen && len > 0.0f) {
            // Pin the drawn knob to the ring. Only the drawing is clamped; StickVector reads the
            // offset as it came in, so travel past the ring stays at full deflection.
            next.stickKnob = { next.stickBase.x + offset.x * maxLen / len, next.stickBase.y + offset.y * maxLen / len };
        }
        const float deadzone = std::clamp(CVarGetFloat(CVAR_TOUCH("Deadzone"), 0.12f), 0.0f, 0.5f);
        const Vec2 stick = StickVector(offset, maxLen, deadzone, kStickRange);
        next.stickX = (int8_t)std::lround(std::clamp(stick.x, -kStickRange, kStickRange));
        // Screen y grows downward, the N64 stick's does not.
        next.stickY = (int8_t)std::lround(std::clamp(-stick.y, -kStickRange, kStickRange));
    }

    if (rightFinger != nullptr) {
        sRightHeld = true;
        sRightFinger = rightFinger->id;
        next.rightActive = true;
        const Vec2 home = sLayout.rightStickHome;
        const Vec2 offset = { rightFinger->pos.x - home.x, rightFinger->pos.y - home.y };
        const float maxLen = sLayout.rightStickBase;
        const float len = std::sqrt(offset.x * offset.x + offset.y * offset.y);
        next.rightKnob = (len > maxLen && len > 0.0f)
                             ? Vec2{ home.x + offset.x * maxLen / len, home.y + offset.y * maxLen / len }
                             : rightFinger->pos;
        const float deadzone = std::clamp(CVarGetFloat(CVAR_TOUCH("Deadzone"), 0.12f), 0.0f, 0.5f);
        const Vec2 right = StickVector(offset, maxLen, deadzone, kRightStickRange);
        next.rightX = (int8_t)std::lround(std::clamp(right.x, -kRightStickRange, kRightStickRange));
        next.rightY = (int8_t)std::lround(std::clamp(-right.y, -kRightStickRange, kRightStickRange));
        // Modern reads yaw and zoom from the stick itself, but its Wonderwing gesture and the
        // picture puzzles read the C bits a gamepad's stick-to-button mapping would set.
        if (std::abs(right.x) > kRightStickRange * kAxisButtonThreshold) {
            next.buttons |= right.x > 0.0f ? BTN_CRIGHT : BTN_CLEFT;
        }
    } else {
        sRightHeld = false;
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
    if (pad->right_stick_x == 0 && pad->right_stick_y == 0) {
        pad->right_stick_x = sState.rightX;
        pad->right_stick_y = sState.rightY;
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

// Points a triangle outward, wound clockwise in screen space as ImGui's fill needs.
void DrawArrow(ImDrawList* dl, ImVec2 center, float radius, Vec2 outward, ImU32 color) {
    const float len = std::sqrt(outward.x * outward.x + outward.y * outward.y);
    if (len <= 0.0f) {
        return;
    }
    const Vec2 f = { outward.x / len, outward.y / len };
    const Vec2 side = { -f.y, f.x };
    const float tip = radius * 0.6f;
    const float back = radius * 0.32f;
    const float half = radius * 0.44f;
    dl->AddTriangleFilled(ImVec2(center.x + f.x * tip, center.y + f.y * tip),
                          ImVec2(center.x - f.x * back + side.x * half, center.y - f.y * back + side.y * half),
                          ImVec2(center.x - f.x * back - side.x * half, center.y - f.y * back - side.y * half), color);
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
        const ImVec2 menuMin =
            px({ sLayout.menuCenter.x - sLayout.menuHalfExtent.x, sLayout.menuCenter.y - sLayout.menuHalfExtent.y });
        const ImVec2 menuMax =
            px({ sLayout.menuCenter.x + sLayout.menuHalfExtent.x, sLayout.menuCenter.y + sLayout.menuHalfExtent.y });
        const float menuRound = (menuMax.y - menuMin.y) * 0.3f;
        dl->AddRectFilled(menuMin, menuMax, Shade(alpha * 0.45f, sState.menuPressed), menuRound);
        dl->AddRect(menuMin, menuMax, Shade(alpha, sState.menuPressed), menuRound, 0, h * 0.004f);

        // Three bars rather than the word MENU. At this size a word would be unreadable, and
        // drawing the rules by hand avoids depending on a glyph the loaded font may not carry.
        const ImVec2 center = px(sLayout.menuCenter);
        const float span = menuMax.x - menuMin.x;
        const float barHalfW = span * 0.25f;
        const float barHalfH = std::max(span * 0.037f, 0.5f);
        const float barGap = span * 0.22f;
        const ImU32 barShade = Shade(std::min(alpha * 1.7f, 0.95f), sState.menuPressed);
        for (int i = -1; i <= 1; i++) {
            const float y = center.y + i * barGap;
            dl->AddRectFilled(ImVec2(center.x - barHalfW, y - barHalfH), ImVec2(center.x + barHalfW, y + barHalfH),
                              barShade, barHalfH);
        }
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
        if (b.arrow.x != 0.0f || b.arrow.y != 0.0f) {
            DrawArrow(dl, px(b.center), b.radius * h, b.arrow, Shade(std::min(alpha * 1.7f, 0.95f), pressed));
        } else {
            // Pill labels are multi-character, so size them off the pill's height, not its width.
            DrawLabel(dl, b.label, px(b.center), (b.pill ? b.halfExtent.y * 1.1f : b.radius * 0.9f) * h);
        }
    }

    if (sLayout.rightStick) {
        const ImVec2 rBase = px(sLayout.rightStickHome);
        const ImVec2 rKnob = px(sState.rightActive ? sState.rightKnob : sLayout.rightStickHome);
        dl->AddCircleFilled(rBase, sLayout.rightStickBase * h, Shade(alpha * 0.35f, false), 48);
        dl->AddCircle(rBase, sLayout.rightStickBase * h, Shade(alpha, false), 48, h * 0.004f);
        dl->AddCircleFilled(rKnob, sLayout.rightStickKnob * h, Shade(alpha * 0.9f, sState.rightActive), 32);
    } else {
        // The letter belongs in the middle of the diamond, as on the controller.
        const Vec2 cCenter = { (sLayout.buttons[CTRL_CLEFT].center.x + sLayout.buttons[CTRL_CRIGHT].center.x) * 0.5f,
                               (sLayout.buttons[CTRL_CUP].center.y + sLayout.buttons[CTRL_CDOWN].center.y) * 0.5f };
        DrawLabel(dl, "C", px(cCenter), sLayout.buttons[CTRL_CUP].radius * 1.25f * h);
    }

    // Stick: base ring plus knob. Idle it sits at its home position as a target.
    const ImVec2 base = px(sState.stickBase);
    const ImVec2 knob = px(sState.stickKnob);
    dl->AddCircleFilled(base, sLayout.stickBase * h, Shade(alpha * 0.35f, false), 48);
    dl->AddCircle(base, sLayout.stickBase * h, Shade(alpha, false), 48, h * 0.004f);
    dl->AddCircleFilled(knob, sLayout.stickKnob * h, Shade(alpha * 0.9f, sState.stickActive), 32);
}

} // namespace Lighthouse

#endif // LIGHTHOUSE_TOUCH_CONTROLS
