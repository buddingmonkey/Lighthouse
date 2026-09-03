// 9.7 tick phase measurement. Temporary: it comes out again under 9.6.
//
// Every other number the port reports covers the render thread. This one covers the game thread,
// across the frame token seam in viMgr_func_8024BFD8: the exit of one tick to the entry of the
// next is the game's own work, and nothing else measures it.

extern "C" {
#include <ultra64.h>
#include "libultraship/libultra/gbi.h"
#include "port/Patches/Patches.h"
}

#ifdef ENABLE_XR_WINDOW

#include <atomic>
#include <chrono>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;

// The stamps are the game thread's alone. The sums are read and cleared by the report, which runs
// on the render thread, so those need the lock.
Clock::time_point sExit;
Clock::time_point sEnter;
Clock::time_point sToken;
Clock::time_point sRetrace;
bool sArmed = false;
bool sBroken = true;

std::atomic<long long> sDrawEnd{ 0 };

std::mutex sMutex;
double sSum[5];
double sLogicMax;
int sTicks;

double Ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}
} // namespace

extern "C" {

void port_tickPhaseEnter(int waitsForToken) {
    sArmed = waitsForToken != 0;
    if (!sArmed) {
        // func_802E3524 waits here as well, with no token. The tick around that one is not a tick.
        sBroken = true;
        return;
    }
    sEnter = Clock::now();
}

void port_tickPhaseToken(void) {
    if (sArmed) {
        sToken = Clock::now();
    }
}

void port_tickPhaseRetrace(void) {
    if (sArmed) {
        sRetrace = Clock::now();
    }
}

void port_tickPhaseExit(void) {
    if (!sArmed) {
        return;
    }
    sArmed = false;

    const Clock::time_point now = Clock::now();
    const bool broken = sBroken;
    const double logic = Ms(sExit, sEnter);
    sExit = now;
    sBroken = false;
    if (broken || logic > 10000.0) {
        return;
    }

    const long long drawEnd = sDrawEnd.load(std::memory_order_relaxed);
    const Clock::time_point draw{ Clock::duration(drawEnd) };
    const double handoff = (drawEnd != 0 && draw < sToken) ? Ms(draw, sToken) : 0.0;

    std::lock_guard<std::mutex> lock(sMutex);
    sSum[0] += logic;
    sSum[1] += Ms(sEnter, sToken);
    sSum[2] += Ms(sToken, sRetrace);
    sSum[3] += Ms(sRetrace, now);
    sSum[4] += handoff;
    if (logic > sLogicMax) {
        sLogicMax = logic;
    }
    ++sTicks;
}

void port_tickPhaseDrawEnd(void) {
    sDrawEnd.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

int port_tickPhaseTake(double out[6]) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sTicks < 1) {
        return 0;
    }
    out[0] = sSum[0] / sTicks;
    out[1] = sLogicMax;
    for (int part = 1; part < 5; ++part) {
        out[part + 1] = sSum[part] / sTicks;
        sSum[part] = 0.0;
    }
    sSum[0] = 0.0;
    sLogicMax = 0.0;
    sTicks = 0;
    return 1;
}

} // extern "C"

#else

extern "C" {
void port_tickPhaseEnter(int waitsForToken) {
    (void)waitsForToken;
}
void port_tickPhaseToken(void) {
}
void port_tickPhaseRetrace(void) {
}
void port_tickPhaseExit(void) {
}
void port_tickPhaseDrawEnd(void) {
}
int port_tickPhaseTake(double out[6]) {
    (void)out;
    return 0;
}
} // extern "C"

#endif
