#include "DebugPad.h"

#ifdef __ANDROID__

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <libultraship/libultra/controller.h>

#include "ship/Context.h"

namespace {

struct NamedButton {
    const char* name;
    uint32_t bit;
};

const NamedButton kButtons[] = {
    { "A", BTN_A },         { "B", BTN_B },           { "Z", BTN_Z },     { "START", BTN_START },
    { "L", BTN_L },         { "R", BTN_R },           { "CUP", BTN_CUP }, { "CDOWN", BTN_CDOWN },
    { "CLEFT", BTN_CLEFT }, { "CRIGHT", BTN_CRIGHT }, { "DUP", BTN_DUP }, { "DDOWN", BTN_DDOWN },
    { "DLEFT", BTN_DLEFT }, { "DRIGHT", BTN_DRIGHT },
};

struct PadState {
    uint32_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t rightX = 0;
    int8_t rightY = 0;
};

PadState sState;
std::chrono::steady_clock::time_point sExpiry;
bool sHolding = false;
std::filesystem::file_time_type sStamp;
bool sHasStamp = false;
int sPollCountdown = 0;

std::string RequestPath() {
    return Ship::Context::GetPathRelativeToAppDirectory("debug-pad");
}

int8_t ClampAxis(long value) {
    if (value > 80) {
        return 80;
    }
    if (value < -80) {
        return -80;
    }
    return static_cast<int8_t>(value);
}

void Apply(const std::string& line) {
    PadState next;
    long holdMs = 0;

    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        long x = 0;
        long y = 0;
        if (sscanf(token.c_str(), "stick=%ld,%ld", &x, &y) == 2) {
            next.stickX = ClampAxis(x);
            next.stickY = ClampAxis(y);
            continue;
        }
        if (sscanf(token.c_str(), "cstick=%ld,%ld", &x, &y) == 2) {
            next.rightX = ClampAxis(x);
            next.rightY = ClampAxis(y);
            continue;
        }
        if (sscanf(token.c_str(), "ms=%ld", &x) == 1) {
            holdMs = x;
            continue;
        }
        for (const NamedButton& button : kButtons) {
            if (token == button.name) {
                next.buttons |= button.bit;
                break;
            }
        }
    }

    sState = next;
    sHolding = holdMs > 0;
    if (sHolding) {
        sExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    }
}

void Poll() {
    if (--sPollCountdown > 0) {
        return;
    }
    sPollCountdown = 6;

    std::error_code ec;
    const std::string path = RequestPath();
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;
    }
    if (sHasStamp && stamp == sStamp) {
        return;
    }
    sStamp = stamp;
    sHasStamp = true;

    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    Apply(line);
}

} // namespace

extern "C" void DebugPad_MergeInto(void* contPad) {
    if (contPad == nullptr) {
        return;
    }

    Poll();

    if (sHolding && std::chrono::steady_clock::now() >= sExpiry) {
        sState = PadState();
        sHolding = false;
    }

    OSContPad* pad = static_cast<OSContPad*>(contPad);
    pad->button |= sState.buttons;
    if (pad->stick_x == 0 && pad->stick_y == 0) {
        pad->stick_x = sState.stickX;
        pad->stick_y = sState.stickY;
    }
    if (pad->right_stick_x == 0 && pad->right_stick_y == 0) {
        pad->right_stick_x = sState.rightX;
        pad->right_stick_y = sState.rightY;
    }
}

#endif
