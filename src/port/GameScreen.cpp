#include "port/GameScreen.h"

#ifdef __ANDROID__
#include <atomic>
#include <jni.h>
#include <SDL2/SDL_system.h>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "port/UI/cvar_prefixes.h"
#endif

#ifdef __ANDROID__
namespace {
std::atomic<int> sCount = 0;
std::atomic<int> sCurrent = 0;
std::atomic<bool> sChanged = false;
bool sRequested = false;
} // namespace

extern "C" JNIEXPORT void JNICALL Java_com_harbormasters_lighthouse_LighthouseActivity_nativeGameScreens(JNIEnv*,
                                                                                                         jclass,
                                                                                                         jint count,
                                                                                                         jint current) {
    sCount = count;
    sCurrent = current;
    sChanged = true;
}
#endif

namespace Lighthouse {

int GameScreenCount() {
#ifdef __ANDROID__
    return sCount;
#else
    return 1;
#endif
}

void ShowGameScreen(int index) {
#ifdef __ANDROID__
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        return;
    }
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID show = env->GetMethodID(activityClass, "setGameScreen", "(I)V");
    if (show != nullptr) {
        SPDLOG_INFO("Game screen {} requested", index);
        env->CallVoidMethod(activity, show, index);
    } else {
        env->ExceptionClear();
        SPDLOG_ERROR("LighthouseActivity.setGameScreen is missing");
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
#else
    (void)index;
#endif
}

void PumpGameScreen() {
#ifdef __ANDROID__
    if (!sRequested) {
        sRequested = true;
        ShowGameScreen(CVarGetInteger(CVAR_SETTING("GameScreen"), 0));
        return;
    }
    if (sChanged.exchange(false) && sCount > 1) {
        CVarSetInteger(CVAR_SETTING("GameScreen"), sCurrent);
    }
#endif
}

} // namespace Lighthouse
