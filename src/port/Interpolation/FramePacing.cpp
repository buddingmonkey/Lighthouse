#include "port/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(ENABLE_DEBUG_TOOLS) && defined(__ANDROID__)
#include <android/log.h>
#endif

#include <libultraship/libultraship.h>
#include <fast/Fast3dWindow.h>
#include <fast/backends/gfx_xr_view.h>
#include <fast/interpreter.h>

#include "FrameInterpolation.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Nametag/Nametag.h"
#include "port/OS/OS.h"
#include "port/Patches/Patches.h"
#include "port/UI/cvar_prefixes.h"

#define gVIsPerFrame 2 // 30 Hz

extern "C" bool prevAltAssets;
extern "C" void port_releaseRcpTask(void);

namespace {
long long sLastSubFrameNs = 0;
long long sFrameLatchNs = 0;
unsigned sFrameViSerial = 0;
bool sFrameTimed = false;

using Clock = std::chrono::steady_clock;
inline long long NsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

void SyncAltAssets() {
    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetRawInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

void ReportDrawTime(long long drawNs, uint32_t views, uint32_t drawCalls, uint32_t drawTextures, uint32_t markedCalls,
                    uint32_t markedTextures, uint32_t* flushCauses) {
#ifdef ENABLE_DEBUG_TOOLS
    static auto since = std::chrono::steady_clock::now();
    static long long total = 0;
    static long long worst = 0;
    static long long callTotal = 0;
    static uint32_t callWorst = 0;
    static uint32_t worstTextures = 0;
    static uint32_t worstMarkedCalls = 0;
    static uint32_t worstMarkedTextures = 0;
    static int subframes = 0;

    total += drawNs;
    if (drawNs > worst) {
        worst = drawNs;
    }
    callTotal += drawCalls;
    if (drawCalls > callWorst) {
        callWorst = drawCalls;
        worstTextures = drawTextures;
        worstMarkedCalls = markedCalls;
        worstMarkedTextures = markedTextures;
    }
    subframes++;

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - since).count();
    if (seconds < 5.0) {
        return;
    }

    SPDLOG_INFO("draw {:.2f} ms a sub-frame, worst {:.2f} ms, {:.0f} draws a sub-frame, worst {} over {} textures "
                "({} draws over {} textures in the marked pass), {} views, {:.1f} sub-frames a second",
                total / (double)subframes / 1.0e6, worst / 1.0e6, (double)callTotal / subframes, callWorst,
                worstTextures, worstMarkedCalls, worstMarkedTextures, views, subframes / seconds);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "draw %.2f ms a sub-frame, worst %.2f ms, %.0f draws a sub-frame, worst %u over %u textures "
                        "(%u draws over %u textures in the marked pass), %u views, %.1f sub-frames a second",
                        total / (double)subframes / 1.0e6, worst / 1.0e6, (double)callTotal / subframes, callWorst,
                        worstTextures, worstMarkedCalls, worstMarkedTextures, views, subframes / seconds);
#endif
    if (flushCauses != nullptr) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                            "marked flush causes: depth %u decal %u vp %u sciss %u tex %u sfb %u samp %u shader %u "
                            "alpha %u cap %u",
                            flushCauses[0], flushCauses[1], flushCauses[2], flushCauses[3], flushCauses[4],
                            flushCauses[5], flushCauses[6], flushCauses[7], flushCauses[8], flushCauses[9]);
#endif
        for (int i = 0; i < 10; i++) {
            flushCauses[i] = 0;
        }
    }

    since = now;
    total = 0;
    worst = 0;
    callTotal = 0;
    callWorst = 0;
    worstTextures = 0;
    worstMarkedCalls = 0;
    worstMarkedTextures = 0;
    subframes = 0;
#else
    (void)drawNs;
    (void)views;
    (void)drawCalls;
    (void)drawTextures;
    (void)markedCalls;
    (void)markedTextures;
    (void)flushCauses;
#endif
}

int sDeliveredSubframes = 0;
uint32_t sDrawnViews = 0;

// Draws one sub-frame for every view, and presents all but the last view.
void DrawSubframe(Fast::Interpreter* interpreter, const std::shared_ptr<Ship::Window>& wndBase, Gfx* commands,
                  const std::unordered_map<Mtx*, MtxF>& replacements) {
    auto wnd = std::static_pointer_cast<Fast::Fast3dWindow>(wndBase);
    auto gui = wndBase->GetGui();
    wndBase->GetMouseStateManager()->StartFrame();
    const uint32_t views = wnd->BeginRenderFrame();
#ifdef ENABLE_DEBUG_TOOLS
    interpreter->mDrawCallCount = 0;
    interpreter->mMarkedDrawCount = 0;
    interpreter->mDrawTextures.clear();
    interpreter->mMarkedTextures.clear();
#endif
    sDrawnViews = views;
    long long drawNs = 0;
    for (uint32_t view = 0; view < views; view++) {
        if (view > 0) {
            interpreter->EndFrame();
        }
        wnd->BeginRenderView(view);
        auto runT0 = Clock::now();
        gui->StartDraw();
        interpreter->StartFrame();
        wnd->RunViewCommands(view, commands, replacements);
        if (OS_ViBlackActive()) {
            interpreter->mGfxFrameBuffer = 0;
            auto rapi = interpreter->GetCurrentRenderingAPI();
            rapi->StartDrawToFramebuffer(0, 1.0f);
            rapi->ClearFramebuffer(true, false);
        }
        gui->EndDraw();
        drawNs += NsSince(runT0);
    }
    sLastSubFrameNs = drawNs;
    sDeliveredSubframes++;
#ifdef ENABLE_DEBUG_TOOLS
    ReportDrawTime(drawNs, views, interpreter->mDrawCallCount, (uint32_t)interpreter->mDrawTextures.size(),
                   interpreter->mMarkedDrawCount, (uint32_t)interpreter->mMarkedTextures.size(),
                   interpreter->mMarkedFlushCauses);
#else
    ReportDrawTime(drawNs, views, 0, 0, 0, 0, nullptr);
#endif
}

void PresentSubframe(Fast::Interpreter* interpreter) {
    if (sDrawnViews > 0) {
        interpreter->EndFrame();
    }
    CALL_EVENT(FrameDrawEnd);
    interpreter->mInterpolationIndex++;
}
} // namespace

void GameEngine::RunCommands(Gfx* Commands) {
    static const std::unordered_map<Mtx*, MtxF> kNoReplacements;
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    Nametag::SetSubframeBlend(1.0f);
    if (wndBase->IsFrameReady()) {
        DrawSubframe(interpreter, wndBase, Commands, kNoReplacements);
        PresentSubframe(interpreter);
    }
    SyncAltAssets();
}

int GameEngine::CurrentViPerTick() {
    int viPerTick = port_getDemoViCount();
    if (viPerTick <= 0) {
        viPerTick = gVIsPerFrame + port_getCutsceneExtraVis();
    }
    if (viPerTick < gVIsPerFrame) {
        viPerTick = gVIsPerFrame;
    }
    // Clamp to 15 for demo playbacks.
    if (viPerTick > 15) {
        viPerTick = 15;
    }
    return viPerTick;
}

namespace {
int EffectiveLogicFps() {
    int fps = 60 / GameEngine::CurrentViPerTick();
    return (fps < 1) ? 1 : fps;
}

int SubframesForTarget(int targetFps) {
    int subframes = targetFps / EffectiveLogicFps();
    return (subframes < 1) ? 1 : subframes;
}
} // namespace

bool GameEngine::IsInterpolationEnabled() {
    return (int)GetInterpolationFPS() > EffectiveLogicFps();
}

// Decided once per list at submit, so the pickup in ServiceRcp and the pass that draws it agree.
bool GameEngine::WantsTimedPass(bool recorded, int viPerTick) {
    return recorded && !GfxDebuggerIsDebugging() && (int)GetInterpolationFPS() > 60 / viPerTick;
}

void GameEngine::SetFrameTiming(long long latchNs, unsigned viSerial, bool timed) {
    sFrameViSerial = viSerial;
    sFrameLatchNs = latchNs;
    sFrameTimed = timed;
}

namespace {
// Room left before the next swap has to latch, for the last draw and the game's swap behind it.
constexpr long long kReleaseMarginNs = 4000000;
constexpr int kMaxTimedSubframes = 32;
// How far a prefetched blend may be from the sub-frame's own before it is redone, as a share of the tick.
constexpr float kPrefetchSlack = 0.03f;

void BuildReplacements(float t, std::unordered_map<Mtx*, MtxF>& replacements) {
    if (t < 1.0f) {
        FrameInterpolation_Interpolate(t, replacements);
    } else {
        replacements.clear();
    }
}

class Prefetcher {
public:
    void Start(float t, std::unordered_map<Mtx*, MtxF>* out) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mThread.joinable()) {
            mThread = std::thread([this] { Run(); });
        }
        mT = t;
        mOut = out;
        mBusy = true;
        mCv.notify_all();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait(lock, [this] { return !mBusy; });
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;) {
            mCv.wait(lock, [this] { return mOut != nullptr; });
            auto* out = mOut;
            const float t = mT;
            mOut = nullptr;
            lock.unlock();
            BuildReplacements(t, *out);
            lock.lock();
            mBusy = false;
            mCv.notify_all();
        }
    }

    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::unordered_map<Mtx*, MtxF>* mOut = nullptr;
    float mT = 0.0f;
    bool mBusy = false;
};

Prefetcher& GetPrefetcher() {
    static Prefetcher* sPrefetcher = new Prefetcher();
    return *sPrefetcher;
}

// Draws the frame at the window's rate until its successor is due to latch, blending each sub-frame to
// where its present lands between the previous frame and this one.
void RunTimedPass(Gfx* commands, int fps) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        port_releaseRcpTask();
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    static std::unordered_map<Mtx*, MtxF> maps[2];
    Prefetcher& prefetcher = GetPrefetcher();
    bool prefetching = false;
    float prefetchT = -1.0f;
    // The tick after this list's sets its VI count when it polls input, right after the submit, and that
    // count decides when this frame latches.
    const long long waitEndNs = OS_SteadyNs() + 3000000;
    while (port_getDemoViSerial() == sFrameViSerial && OS_SteadyNs() < waitEndNs) {
        port_serviceRenderRequests();
        port_waitDemoViSerial(sFrameViSerial, 250);
    }
    const int vis = std::max(port_getDemoViCount(), 2);
    const long long tickNs = 1000000000LL * vis / 60;
    const long long presentNs = 1000000000LL / fps;
    const long long passStartNs = OS_SteadyNs();
    long long anchorNs = sFrameLatchNs;
    if (anchorNs == 0 || std::llabs(passStartNs - anchorNs) > tickNs) {
        const long long latchNs = OS_ViLastLatchNs();
        anchorNs = (latchNs > 0 && passStartNs - latchNs < tickNs) ? latchNs : passStartNs;
    }
    const long long releaseBy = anchorNs + tickNs - kReleaseMarginNs;
    long long prevRunNs = 0;
    long long runIntervalNs = presentNs;
    int count = 0;

    for (;;) {
        const long long now = OS_SteadyNs();
        if (prevRunNs != 0) {
            runIntervalNs = std::max(presentNs, now - prevRunNs);
        }
        prevRunNs = now;
        // Last when another draw after this one could not be finished by the release deadline.
        const bool last = count + 1 >= kMaxTimedSubframes || now + runIntervalNs + sLastSubFrameNs > releaseBy;
        float t = std::clamp((float)(now + presentNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
        auto& replacements = maps[count & 1];
        bool prefetched = false;
        if (prefetching) {
            prefetcher.Wait();
            prefetching = false;
            if (std::fabs(prefetchT - t) <= kPrefetchSlack) {
                t = prefetchT;
                prefetched = true;
            }
        }
        if (!prefetched) {
            BuildReplacements(t, replacements);
        }
        if (!last) {
            prefetchT = std::clamp((float)(now + runIntervalNs + presentNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
            prefetcher.Start(prefetchT, &maps[(count + 1) & 1]);
            prefetching = true;
        }
        FrameInterpolation_ApplyAnimVertices(t);
        Nametag::SetSubframeBlend(t);
        DrawSubframe(interpreter, wndBase, commands, replacements);
        if (last) {
            port_releaseRcpTask();
        }
        PresentSubframe(interpreter);
        count++;
        if (last) {
            break;
        }
    }
    if (prefetching) {
        prefetcher.Wait();
    }
    SyncAltAssets();
}
} // namespace

namespace {
constexpr int kRateSettleTicks = 90;

void SelectDisplayRefreshRate(Fast::Fast3dWindow* wnd) {
    if (!IsHeadsetWindow()) {
        return;
    }
    const int cap = CVarGetInteger(CVAR_SETTING("XrMaxRate"), 120);

    const float logicRate = 60.0f / gVIsPerFrame;
    std::vector<float> rates;
    for (float rate : wnd->GetSupportedRefreshRates()) {
        const float multiple = rate / logicRate;
        if (fabsf(multiple - roundf(multiple)) < 0.01f && rate <= (float)cap) {
            rates.push_back(rate);
        }
    }
    if (rates.empty()) {
        return;
    }
    std::sort(rates.begin(), rates.end(), std::greater<float>());

    static int askedCap = -1;
    static float asked = 0.0f;
    static int waited = 0;
    if (askedCap != cap) {
        askedCap = cap;
        asked = 0.0f;
        waited = 0;
    }

    if (asked <= 0.0f) {
        asked = rates.front();
        wnd->SetRefreshRate(asked);
        waited = 0;
        return;
    }

    if (fabsf((float)wnd->GetCurrentRefreshRate() - asked) < 0.5f) {
        waited = 0;
        return;
    }
    if (++waited < kRateSettleTicks) {
        return;
    }
    waited = 0;
    for (size_t i = 0; i + 1 < rates.size(); i++) {
        if (fabsf(rates[i] - asked) < 0.5f) {
            asked = rates[i + 1];
            wnd->SetRefreshRate(asked);
            return;
        }
    }
}

void ReportTickRate(int delivered) {
#ifdef ENABLE_DEBUG_TOOLS
    static auto since = std::chrono::steady_clock::now();
    static int ticks = 0;
    static long long deliveredTotal = 0;
    static long long viTotal = 0;

    ticks++;
    deliveredTotal += delivered;
    viTotal += GameEngine::CurrentViPerTick();

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - since).count();
    if (seconds < 5.0) {
        return;
    }

    uint32_t rate = 0;
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    if (window != nullptr) {
        rate = window->GetCurrentRefreshRate();
    }
    SPDLOG_INFO("game ticks {:.1f} a second, vi {:.2f}, {:.2f} sub-frames drawn a tick, display {} Hz", ticks / seconds,
                (double)viTotal / ticks, (double)deliveredTotal / ticks, rate);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "game ticks %.1f a second, vi %.2f, %.2f sub-frames drawn a tick, display %u Hz",
                        ticks / seconds, (double)viTotal / ticks, (double)deliveredTotal / ticks, rate);
#endif

    since = now;
    ticks = 0;
    deliveredTotal = 0;
    viTotal = 0;
#else
    (void)delivered;
#endif
}

#ifdef ENABLE_OPENXR
bool SyncXrSetting(const char* cVar, float low, float high, float defaultValue, float& pushed, float held,
                   void (*apply)(float), float (*convert)(float)) {
    const float shown = std::clamp(CVarGetFloat(cVar, defaultValue), low, high);
    if (shown != pushed) {
        apply(convert(shown));
        pushed = shown;
        return false;
    }
    const float left = std::clamp(convert(held), low, high);
    if (fabsf(left - shown) > 0.001f) {
        CVarSetFloat(cVar, left);
        pushed = left;
        return true;
    }
    return false;
}
#endif

void ApplyHeadsetSettings(Fast::Fast3dWindow* wnd) {
    SelectDisplayRefreshRate(wnd);

#ifdef ENABLE_XR_WINDOW
    Fast::SetXrDioramaDepth(CVarGetFloat(CVAR_SETTING("XrDioramaDepth"), 2.0f));
    Fast::SetXrDepthLimit(CVarGetFloat(CVAR_SETTING("XrDepthLimit"), 1.0f));
    Fast::SetXrSteadyDepth(CVarGetInteger(CVAR_SETTING("XrSteadyDepth"), 1) != 0);
#endif

#ifdef ENABLE_OPENXR
    static float pushedRange = 0.0f;
    static float pushedScale = 0.0f;
    const bool rangeMoved =
        SyncXrSetting(CVAR_SETTING("XrWindowRange"), 0.5f, 4.0f, 1.3f, pushedRange, Fast::GetXrWindowDistance(),
                      Fast::SetXrWindowDistance, [](float value) { return value; });
    const bool scaleMoved =
        SyncXrSetting(CVAR_SETTING("XrWindowScale"), 0.5f, 8.0f, 2.6f, pushedScale, Fast::GetXrWindowScale(),
                      Fast::SetXrWindowScale, [](float value) { return value; });

    static bool wasMoving = false;
    const bool moving = rangeMoved || scaleMoved;
    if (wasMoving && !moving) {
        CVarSave();
    }
    wasMoving = moving;

    wnd->SetResolutionMultiplier(CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1.0f));

    Fast::SetXrStereo(CVarGetInteger(CVAR_SETTING("XrStereo"), 1) != 0);
    Fast::SetXrEdgeSoftness(CVarGetFloat(CVAR_SETTING("XrEdgeSoftness"), 0.36f));
    Fast::SetXrEdgeFloat(CVarGetFloat(CVAR_SETTING("XrEdgeFloat"), 0.15f));
#else
    (void)wnd;
#endif
}
} // namespace

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    // if(gEnableGammaBoost) {
    //     wnd->EnableSRGBMode();
    // }
    ApplyHeadsetSettings(wnd.get());
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);
    sDeliveredSubframes = 0;

    if (sFrameTimed) {
        const int fps = (int)GetInterpolationFPS();
        wnd->SetTargetFps(fps);
        wnd->SetMaximumFrameLatency(2);
        RunTimedPass(commands, fps);
    } else {
        wnd->SetTargetFps(EffectiveLogicFps());
        wnd->SetMaximumFrameLatency(2);
        RunCommands(commands);
    }
    ReportTickRate(sDeliveredSubframes);
}

uint32_t GameEngine::GetInterpolationFPS() {
    if (CVarGetInteger(CVAR_SETTING("MatchRefreshRate"), IsHeadsetWindow() ? 1 : 0)) {
        return Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger(CVAR_VSYNC_ENABLED, 1) ||
               !Ship::Context::GetRawInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 60));
    }

    return CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 30);
}

uint32_t GameEngine::GetInterpolationFrameCount() {
    return static_cast<uint32_t>(SubframesForTarget((int)GetInterpolationFPS()));
}

extern "C" uint32_t GameEngine_GetInterpolationFrameCount() {
    return GameEngine::GetInterpolationFrameCount();
}
