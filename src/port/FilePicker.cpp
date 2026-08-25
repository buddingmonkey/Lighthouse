#include "port/FilePicker.h"

#if LIGHTHOUSE_NATIVE_FILE_DIALOG
#include <string>
#include <vector>
#include "portable-file-dialogs.h"
#include "port/DevTools/ThreadWatchdog.h"
#elif defined(__ANDROID__)
#include <jni.h>
#include <mutex>
#include <SDL2/SDL_system.h>
#include "spdlog/spdlog.h"
#endif

namespace fs = std::filesystem;

namespace Lighthouse {

#if LIGHTHOUSE_NATIVE_FILE_DIALOG
// portable-file-dialogs takes a flat filter list: { label, "*.a *.b", label2, "*.c", ... }.
static std::vector<std::string> ToPfdFilters(const std::vector<Ship::FileFilter>& filters) {
    std::vector<std::string> out;
    for (const auto& filter : filters) {
        out.push_back(filter.Label);
        std::string patterns;
        for (const auto& pattern : filter.Patterns) {
            if (!patterns.empty()) {
                patterns += " ";
            }
            patterns += pattern;
        }
        out.push_back(patterns);
    }
    if (out.empty()) {
        out = { "All Files", "*" };
    }
    return out;
}
#elif defined(__ANDROID__)
namespace {
std::mutex sPickMutex;
std::function<void(std::optional<fs::path>)> sPickCallback;
std::optional<fs::path> sPickResult;
bool sPickResultReady = false;
} // namespace

extern "C" JNIEXPORT void JNICALL Java_com_harbormasters_lighthouse_LighthouseActivity_nativeFilePicked(JNIEnv* env,
                                                                                                        jclass,
                                                                                                        jstring path) {
    std::optional<fs::path> picked;
    if (path != nullptr) {
        const char* chars = env->GetStringUTFChars(path, nullptr);
        if (chars != nullptr) {
            picked = fs::path(chars);
            env->ReleaseStringUTFChars(path, chars);
        }
    }
    std::lock_guard<std::mutex> lock(sPickMutex);
    sPickResult = std::move(picked);
    sPickResultReady = true;
}

static bool OpenAndroidPicker() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        SPDLOG_ERROR("No Android activity to open the file picker on");
        return false;
    }
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID open = env->GetMethodID(activityClass, "openFilePicker", "()V");
    const bool found = open != nullptr;
    if (found) {
        env->CallVoidMethod(activity, open);
    } else {
        env->ExceptionClear();
        SPDLOG_ERROR("LighthouseActivity.openFilePicker is missing");
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return found;
}
#endif

void PickFile(Ship::FileBrowserRequest request, std::function<void(std::optional<fs::path>)> onResult) {
#if LIGHTHOUSE_NATIVE_FILE_DIALOG
    const std::string startDir = request.StartDir.empty() ? "." : request.StartDir.string();
    const std::vector<std::string> filters = ToPfdFilters(request.Filters);

    std::optional<fs::path> result;
    {
        Lighthouse::ExpectedStall watchdogPause("native file dialog");
        if (request.Save) {
            const std::string defaultPath = (fs::path(startDir) / request.DefaultName).string();
            std::string selection = pfd::save_file(request.Title, defaultPath, filters).result();
            if (!selection.empty()) {
                result = fs::path(selection);
            }
        } else {
            std::vector<std::string> selection = pfd::open_file(request.Title, startDir, filters).result();
            if (!selection.empty()) {
                result = fs::path(selection.front());
            }
        }
    }
    if (onResult) {
        onResult(result);
    }
#elif defined(__ANDROID__)
    if (!request.Save) {
        {
            std::lock_guard<std::mutex> lock(sPickMutex);
            sPickCallback = std::move(onResult);
            sPickResult.reset();
            sPickResultReady = false;
        }
        if (!OpenAndroidPicker()) {
            std::lock_guard<std::mutex> lock(sPickMutex);
            sPickResultReady = true;
        }
        return;
    }
    request.OnResult = std::move(onResult);
    Ship::FileBrowserWindow::Open(std::move(request));
#else
    request.OnResult = std::move(onResult);
    Ship::FileBrowserWindow::Open(std::move(request));
#endif
}

void PumpFilePicker() {
#if !LIGHTHOUSE_NATIVE_FILE_DIALOG && defined(__ANDROID__)
    std::function<void(std::optional<fs::path>)> callback;
    std::optional<fs::path> result;
    {
        std::lock_guard<std::mutex> lock(sPickMutex);
        if (!sPickResultReady) {
            return;
        }
        sPickResultReady = false;
        callback = std::move(sPickCallback);
        sPickCallback = nullptr;
        result = std::move(sPickResult);
        sPickResult.reset();
    }
    if (callback) {
        callback(result);
    }
#endif
}

} // namespace Lighthouse
