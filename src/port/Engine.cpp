#include "Engine.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <future>
#if defined(ENABLE_DEBUG_TOOLS) && defined(__ANDROID__)
#include <android/log.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif
#include "PR/libaudio.h"
#include <libultraship/libultraship.h>

#include <fast/Fast3dWindow.h>
#include <fast/backends/gfx_xr_view.h>
#include <fast/interpreter.h>
#include "fast/resource/ResourceType.h"
#include <fast/resource/factory/DisplayListFactory.h>
#include <fast/resource/factory/TextureFactory.h>
#include <fast/resource/factory/MatrixFactory.h>
#include <fast/resource/factory/VertexFactory.h>
#include <libultraship/bridge/gfxbridge.h>
#include <libultraship/controller/controldeck/ControlDeck.h>
#include <libultraship/libultra/AudioDmaRegistry.h>
#include <SDL2/SDL.h>
#include <ship/controller/controldevice/controller/mapping/ControllerDefaultMappings.h>
#include <ship/resource/factory/BlobFactory.h>
#include <ship/resource/type/Blob.h>
#include <ship/utils/StringHelper.h>
#include <ship/window/gui/Fonts.h>
#include <ship/window/gui/resource/Font.h>

#include "Audio/GameAudio.h"
#include "build.h"
#include "Extractor/ExtractFlow.h"
#include "Extractor/GameExtractor.h"
#include "ship/window/gui/FileBrowserWindow.h"
#include "port/FilePicker.h"
#include "Interpolation/FrameInterpolation.h"
#include "Nametag/Nametag.h"
#include "OS/OS.h"
#include "Network/Anchor/Anchor.h"
#include "port/Enhancements/Events/PortEnhancements.h"
#include "port/Patches/Patches.h"
#include "port/Save/SaveManager.h"
#include "port/UI/cvar_prefixes.h"
#include "ResourceHelpers.h"
#include "Localization/Language.h"
#include "Resource/Importers/AnimFactory.h"
#include "Resource/Importers/DemoInputFactory.h"
#include "Resource/Importers/DialogFactory.h"
#include "Resource/Importers/MapFactory.h"
#include "Resource/Importers/ModelFactory.h"
#include "Resource/Importers/SpriteFactory.h"
#include "src/port/Enhancements/Events/Hooks/Events.h"
#include "UI/LighthouseGui.hpp"
#include "UI/LighthouseModMenuWindow.h"
#include "LaunchArgs.h"

#ifdef __SWITCH__
#include <port/switch/SwitchImpl.h>
#endif

// Engine constants

#define SAMPLES_PER_FRAME (560 * 2 * 2)
#define gVIsPerFrame 2 // 30 Hz

const float imguiScaleOptionToValue[4] = { 0.75f, 1.0f, 1.5f, 2.0f };

static constexpr float MENU_ANGLE_PER_UNIT = 0.0009f;
static constexpr float MENU_ANGLE_MIN = 0.75f;

bool IsHeadsetWindow() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr) {
        return false;
    }
    const auto backend = ctx->GetWindow()->GetWindowBackend();
    return backend == static_cast<int32_t>(Fast::WindowBackend::FAST3D_OPENXR_OPENGL) ||
           backend == static_cast<int32_t>(Fast::WindowBackend::FAST3D_VISIONOS_METAL);
}

uint32_t DefaultImGuiScaleIndex() {
#ifdef LIGHTHOUSE_MOBILE
    static const uint32_t index = []() {
        auto window = Ship::Context::GetRawInstance()->GetWindow();
        if (window == nullptr) {
            return 0u;
        }
        if (IsHeadsetWindow()) {
            return 1u;
        }
        float shortSide = static_cast<float>(std::min(window->GetWidth(), window->GetHeight()));
#ifdef __ANDROID__
        float ddpi = 0.0f;
        float hdpi = 0.0f;
        float vdpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && vdpi > 0.0f) {
            shortSide = shortSide * 160.0f / vdpi;
        }
#endif
        return shortSide >= 600.0f ? 2u : 0u;
    }();
    return index;
#else
    return 1;
#endif
}

float ImGuiDensityScale() {
    float density = 1.0f;
#ifdef __ANDROID__
    static const float androidDensity = []() {
        float ddpi = 0.0f;
        float hdpi = 0.0f;
        float vdpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && vdpi > 0.0f) {
            return std::max(1.0f, vdpi / 160.0f);
        }
        return 1.0f;
    }();
    density = androidDensity;
#endif

#ifdef ENABLE_XR_WINDOW
    if (IsHeadsetWindow()) {
        auto window = Ship::Context::GetRawInstance()->GetWindow();
        const float angularWidth = Fast::GetXrWindowAngularWidth();
        if (angularWidth > 0.0f && window != nullptr && window->GetWidth() > 0) {
            return MENU_ANGLE_PER_UNIT * static_cast<float>(window->GetWidth()) /
                   std::max(angularWidth, MENU_ANGLE_MIN);
        }
        return density * 0.66f;
    }
#endif
    return density;
}

// Engine globals

namespace {
constexpr int kDemoAudioHoldFrames = 2;
const char* sOtrSignature = "__OTR__";

// Attract-demo audio hold
std::atomic<bool> sHoldAudio{ false };
int sHoldFramesRemaining = 0;
std::vector<std::shared_ptr<Ship::IResource>> sSoundfontResources;

// Frame pacing and rendering
bool sInterpolationRecorded = false;
std::vector<std::future<void>> sMapBuildFutures;
long long sPassBudgetNs = 0;

long long sFilteredSubFrameNs = 0;
int sDeliveredSubFrames = 0;
int sOverBudgetRun = 0;
} // namespace

std::shared_ptr<Fast::Fast3dWindow> lhFast3dWindow;
bool portArchiveVersionMatch = false;
std::string assets_path;

float previousImGuiScale = 1.0f;

namespace fs = std::filesystem;

extern "C" {

// Reset support
extern s32 D_80275610;

bool prevAltAssets = false;
// bool gEnableGammaBoost = true;

// Soundfont symbols
u8* soundfont1ctl_ROM_START = NULL;
u8* soundfont1ctl_ROM_END = NULL;
u8* soundfont1tbl_ROM_START = NULL;
u8* soundfont2ctl_ROM_START = NULL;
u8* soundfont2ctl_ROM_END = NULL;
u8* soundfont2tbl_ROM_START = NULL;
}

std::vector<uint8_t*> MemoryPool;
GameEngine* GameEngine::Instance;

// Construction

GameEngine::GameEngine() {
#ifdef LIGHTHOUSE_MOBILE
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#ifdef __IOS__
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#endif

    this->context = Ship::Context::CreateUninitializedInstance("Lighthouse", "bk", "lighthouse.cfg.json");

#ifdef __SWITCH__
    Ship::Switch::Init(Ship::PreInitPhase);
    Ship::Switch::Init(Ship::PostInitPhase);
#endif

    this->context->InitConfiguration();
    this->context->InitConsoleVariables();
    assets_path = Ship::Context::LocateFileAcrossAppDirs("lighthouse.o2r");
    portArchiveVersionMatch = std::filesystem::exists(assets_path); // TODO: port archive versioning

    auto controlDeck = std::make_shared<LUS::ControlDeck>();

    this->context->InitControlDeck(controlDeck);
    this->context->InitResourceManager({ assets_path }, {}, 3, true);
    this->context->InitConsole();

    // Register console commands for menu buttons
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "reset", { [](std::shared_ptr<Ship::Console>, const std::vector<std::string>&, std::string*) -> bool {
                      gPortResetPending = 1; // lets audio spin-waits exit immediately
                      setBootMap(getDefaultBootMap());
                      D_80275610 = 3 + 1; // deferred: mainLoop picks this up next frame
                      CALL_EVENT(OnReset);
                      return 0;
                  },
                   "Reset the game." });
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "quit", { [](std::shared_ptr<Ship::Console>, const std::vector<std::string>&, std::string*) -> bool {
                     Ship::Context::GetRawInstance()->GetWindow()->Close();
                     return 0;
                 },
                  "Quit the game." });

    lhFast3dWindow = std::make_shared<Fast::Fast3dWindow>(std::vector<std::shared_ptr<Ship::GuiWindow>>({}));
    this->context->InitWindow(lhFast3dWindow);
    port_installLifecycleWatch();
    this->context->InitAudio({ .SampleRate = 22000, .SampleLength = 1024, .DesiredBuffered = 2208 });

    LighthouseGui::SetupMenu();

    if (portArchiveVersionMatch) {
        fontMono = CreateFontWithSize(16.0f, "fonts/Inconsolata-Regular.ttf");
        fontMonoLarger = CreateFontWithSize(20.0f, "fonts/Inconsolata-Regular.ttf");
        fontMonoLargest = CreateFontWithSize(24.0f, "fonts/Inconsolata-Regular.ttf");
        fontStandard = CreateFontWithSize(16.0f, "fonts/Montserrat-Regular.ttf");
        fontStandardLarger = CreateFontWithSize(20.0f, "fonts/Montserrat-Regular.ttf");
        fontStandardLargest = CreateFontWithSize(24.0f, "fonts/Montserrat-Regular.ttf");
        ImGui::GetIO().FontDefault = fontStandardLarger;
    }

    previousImGuiScale = 1.0f;
    ScaleImGui();
}

// Startup

static void RegisterResourceFactories(const std::shared_ptr<Ship::ResourceLoader>& loader) {
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinarySpriteV0>(),
                                    RESOURCE_FORMAT_BINARY, "Sprite",
                                    static_cast<uint32_t>(Torch::ResourceType::BKSprite), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryModelV0>(), RESOURCE_FORMAT_BINARY,
                                    "Model", static_cast<uint32_t>(Torch::ResourceType::BKModel), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKAnimationV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKAnimation",
                                    static_cast<uint32_t>(Torch::ResourceType::BKAnimation), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKDialogV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKDialog",
                                    static_cast<uint32_t>(Torch::ResourceType::BKDialog), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKQuizQuestionV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKQuizQuestion",
                                    static_cast<uint32_t>(Torch::ResourceType::BKQuizQuestion), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKGruntyQuestionV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKGruntyQuestion",
                                    static_cast<uint32_t>(Torch::ResourceType::BKGruntyQuestion), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKDemoInputV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKDemoInput",
                                    static_cast<uint32_t>(Torch::ResourceType::BKDemoInput), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKMapV0>(), RESOURCE_FORMAT_BINARY,
                                    "BKMap", static_cast<uint32_t>(Torch::ResourceType::BKMap), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 1);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vertex", static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLVertexV0>(), RESOURCE_FORMAT_XML, "Vertex",
                                    static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(),
                                    RESOURCE_FORMAT_BINARY, "DisplayList",
                                    static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLDisplayListV0>(), RESOURCE_FORMAT_XML,
                                    "DisplayList", static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), RESOURCE_FORMAT_BINARY,
                                    "Matrix", static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);

    loader->RegisterResourceFactory(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), RESOURCE_FORMAT_BINARY,
                                    "Blob", static_cast<uint32_t>(Ship::ResourceType::Blob), 0);
}

static void LoadLooseModDirectories(const std::string& patches_path) {
    if (patches_path.empty() || !std::filesystem::is_directory(patches_path)) {
        return;
    }
    for (const auto& p : std::filesystem::directory_iterator(patches_path)) {
        if (!p.is_directory()) {
            continue;
        }
        const std::string dirName = p.path().filename().generic_string();
        if (dirName == "~romhacks" || dirName == "~shared" || dirName == "~lang" || IsScopedModFolderName(dirName)) {
            continue;
        }
        SPDLOG_INFO("Found mod directory: {}", p.path().generic_string());
        Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(
            p.path().generic_string());
    }
}

static void LoadLanguagePacks() {
    const std::string lang_path = Ship::Context::GetPathRelativeToAppDirectory("mods/~lang");
    if (lang_path.empty() || !std::filesystem::is_directory(lang_path)) {
        return;
    }
    for (const auto& p : std::filesystem::directory_iterator(lang_path)) {
        if (p.is_regular_file() && p.path().extension() == ".o2r") {
            SPDLOG_INFO("Loading language pack: {}", p.path().generic_string());
            Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(
                p.path().generic_string());
        }
    }
}

void GameEngine::FinishInit() {
    for (const auto& archive : kRomArchives) {
        std::string romPath = Ship::Context::LocateFileAcrossAppDirs(archive, "bk");
        if (std::filesystem::exists(romPath)) {
            context->GetResourceManager()->GetArchiveManager()->AddArchive(romPath);
        }
    }

    const std::string patches_path = Ship::Context::GetPathRelativeToAppDirectory("mods");
    if (!patches_path.empty() && !std::filesystem::exists(patches_path)) {
        std::filesystem::create_directories(patches_path);
    }

    Lighthouse::ApplyLaunchHack();
    UpdateModFiles(true);
    LoadLooseModDirectories(patches_path);
    LoadLanguagePacks();

#if (_DEBUG)
    auto defaultLogLevel = spdlog::level::debug;
#else
    auto defaultLogLevel = spdlog::level::info;
#endif
    auto logLevel =
        static_cast<spdlog::level::level_enum>(CVarGetInteger(CVAR_DEVELOPER_TOOLS("LogLevel"), defaultLogLevel));
    context->InitLogging(logLevel, logLevel);
    Ship::Context::GetRawInstance()->GetLogger()->set_pattern("[%H:%M:%S.%e] [%s:%#] [%l] %v");
    SPDLOG_INFO("Starting Lighthouse version {} (Branch: {} | Commit: {})", (char*)gBuildVersion, (char*)gGitBranch,
                (char*)gGitCommitHash);
    Lighthouse::FlushLaunchHackLog();

    context->InitFileDropMgr();
    context->InitCrashHandler();
    context->InitEventSystem();

    lhFast3dWindow->SetTargetFps(60);
    lhFast3dWindow->SetMaximumFrameLatency(1);
    lhFast3dWindow->SetRendererUCode(ucode_f3d);

    // Opt-in to memoization
    if (auto interpreter = lhFast3dWindow->GetInterpreterWeak().lock()) {
        interpreter->SetResolvedResourceCacheEnabled(true);
    }

#ifdef USE_NETWORKING
    SDLNet_Init();
#endif

    RegisterResourceFactories(context->GetResourceManager()->GetResourceLoader());
    prevAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
    context->GetResourceManager()->SetAltAssetsEnabled(prevAltAssets);

    Lighthouse::RescanLanguages();

    LighthouseGui::SetupGuiElements();
    Lighthouse::RestoreModSelectionAfterLaunchHack();
    MaybeShowModConflictPopup();
    MaybeShowRomhackBaseMismatchPopup();
    Instance->AudioInit();
    // Instance->LoadDictionary();
    // Instance->LoadPlayerAnims();
#if defined(__SWITCH__) || defined(__WIIU__)
    CVarRegisterInteger(CVAR_IMGUI_CONTROLLER_NAV, 1); // always enable controller nav on switch/wii u
#endif
}

// Fonts and ImGui scaling

ImFont* GameEngine::CreateFontWithSize(float size, std::string fontPath) {
    auto mImGuiIo = &ImGui::GetIO();
    ImFont* font;
    if (fontPath == "") {
        ImFontConfig fontCfg = ImFontConfig();
        fontCfg.OversampleH = fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH = true;
        fontCfg.SizePixels = size;
        font = mImGuiIo->Fonts->AddFontDefault(&fontCfg);
    } else {
        auto initData = std::make_shared<Ship::ResourceInitData>();
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;

        initData->Format = RESOURCE_FORMAT_BINARY;
        initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_FONT);
        initData->ResourceVersion = 0;
        initData->Path = fontPath;
        std::shared_ptr<Ship::Font> fontData = std::static_pointer_cast<Ship::Font>(
            Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(fontPath, false, initData));
        font =
            mImGuiIo->Fonts->AddFontFromMemoryTTF(fontData->Data, static_cast<int>(fontData->DataSize), size, &config);
    }
    float iconFontSize = size * 2.0f / 3.0f;
    static const ImWchar sIconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    iconsConfig.GlyphMinAdvanceX = iconFontSize;
    mImGuiIo->Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, iconFontSize,
                                                          &iconsConfig, sIconsRanges);

    return font;
}

void GameEngine::ScaleImGui() {
    int32_t imGuiScaleIndex = CVarGetInteger("gSettings.ImGuiScale", DefaultImGuiScaleIndex());
    float scale = imguiScaleOptionToValue[imGuiScaleIndex] * ImGuiDensityScale();
    if (fabsf(scale - previousImGuiScale) < 0.001f) {
        return;
    }

    float newScale = scale / previousImGuiScale;
    ImGui::GetStyle().ScaleAllSizes(newScale);
    ImGui::GetIO().FontGlobalScale = scale;
    previousImGuiScale = scale;
}

// Lifecycle

void GameEngine::Create(int argc, char* argv[]) {
    Lighthouse::ParseLaunchArgs(argc, argv);
    const auto instance = Instance = new GameEngine();
    // instance->AudioInit();
    // DisplayListPatch::Run();
    // BK renders at 292x216, not the standard 320x240.
    GfxSetNativeDimensions(292, 216);
    instance->RunExtract(argc, argv);
    instance->FinishInit();
    PortEnhancements_Init();
    Anchor::Init();
    SaveManager_Init();
    ShipInit::InitAll();
    ShipInit::Init("BOOT");
    atexit([]() {
        if (Instance && Instance->context && Instance->context->GetControlDeck()) {
            for (int i = 0; i < 4; i++) {
                auto controller = Instance->context->GetControlDeck()->GetControllerByPort(i);
                if (controller) {
                    controller->GetRumble()->StopRumble();
                }
            }
        }
    });
}

extern void ResourceHelpers_ClearRefCache();
void ReleaseSoundfonts();

void GameEngine::Destroy() {
    if (Instance->context && Instance->context->GetControlDeck()) {
        for (int i = 0; i < 4; i++) {
            auto controller = Instance->context->GetControlDeck()->GetControllerByPort(i);
            if (controller) {
                controller->GetRumble()->StopRumble();
            }
        }
    }

    LighthouseGui::Destroy();
    lhFast3dWindow = nullptr;
    ResourceHelpers_ClearRefCache();
    AudioDma_Clear();
    ReleaseSoundfonts();
    if (Instance->context && Instance->context->GetResourceManager()) {
        Instance->context->GetResourceManager()->UnloadResources("*");
    }
    Ship::Context::DestroyInstance();
    Instance->context = nullptr;
    // PortEnhancements_Exit();
    for (auto ptr : MemoryPool) {
        free(ptr);
    }
    MemoryPool.clear();
#ifdef __SWITCH__
    Ship::Switch::Exit();
#endif
}

void GameEngine::StartFrame() const {
    ScaleImGui();

    using Ship::KbScancode;
    const int32_t dwScancode = this->context->GetWindow()->GetLastScancode();
    this->context->GetWindow()->SetLastScancode(-1);

    switch (dwScancode) {
        case KbScancode::LUS_KB_TAB: {
            // Toggle HD Assets
            CVarSetInteger(CVAR_SETTING("Mods.AlternateAssets"),
                           !CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1));
            break;
        }
        case KbScancode::LUS_KB_F4: {
            // gNextGameState = GSTATE_BOOT;
            break;
        }
        default:
            break;
    }
}

void GameEngine::RenderGuiFrame() const {
    if (lhFast3dWindow == nullptr) {
        return;
    }
    lhFast3dWindow->HandleEvents();
    if (!lhFast3dWindow->IsFrameReady()) {
        return;
    }
    auto gui = lhFast3dWindow->GetGui();
    gui->StartDraw();
    lhFast3dWindow->StartFrame();
    lhFast3dWindow->RunGuiOnly();
    gui->EndDraw();
    lhFast3dWindow->EndFrame();
}

bool GameEngine::sRelaunchRequested = false;

void GameEngine::RelaunchIfRequested(int argc, char* argv[]) {
    if (!sRelaunchRequested) {
        return;
    }
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            SPDLOG_ERROR("Relaunch failed: CreateProcess error {}", GetLastError());
        }
    }
#elif defined(LIGHTHOUSE_MOBILE)
    (void)argc;
    (void)argv;
#elif defined(__linux__) || defined(__APPLE__)
    execv(argv[0], argv);
    SPDLOG_ERROR("Relaunch failed: execv error {}", strerror(errno));
#endif
}

// Audio

extern "C" uint32_t GameEngine_GetSamplesPerFrame() {
    return SAMPLES_PER_FRAME;
}

extern "C" void port_beginDemoAudioHold(void) {
    if (kDemoAudioHoldFrames <= 0) {
        return;
    }
    sHoldFramesRemaining = kDemoAudioHoldFrames;
    sHoldAudio.store(true);
}

extern "C" void port_tickDemoAudioHold(void) {
    if (sHoldAudio.load() && --sHoldFramesRemaining <= 0) {
        sHoldAudio.store(false);
    }
}

extern "C" int port_audioHeld(void) {
    return sHoldAudio.load() ? 1 : 0;
}

void ReleaseSoundfonts() {
    sSoundfontResources.clear();
}

static void LoadSoundfonts() {
    auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
    sSoundfontResources.clear();

    auto loadBlob = [&rm](const char* path, uint8_t*& start, uint8_t*& end) {
        auto res = rm->LoadResource(path);
        if (res) {
            start = (uint8_t*)res->GetRawPointer();
            end = start + res->GetPointerSize();
            AudioDma_Register(start, res->GetPointerSize());
            sSoundfontResources.push_back(res);
        } else {
            SPDLOG_ERROR("[Audio] Failed to load soundfont '{}'", path);
        }
    };

    loadBlob("soundfont/soundfont1ctl", soundfont1ctl_ROM_START, soundfont1ctl_ROM_END);
    loadBlob("soundfont/soundfont2ctl", soundfont2ctl_ROM_START, soundfont2ctl_ROM_END);

    // tbl assets don't need END — only START is referenced
    auto loadTbl = [&rm](const char* path, uint8_t*& start) {
        auto res = rm->LoadResource(path);
        if (res) {
            start = (uint8_t*)res->GetRawPointer();
            AudioDma_Register(start, res->GetPointerSize());
            sSoundfontResources.push_back(res);
        } else {
            SPDLOG_ERROR("[Audio] Failed to load soundfont '{}'", path);
        }
    };

    loadTbl("soundfont/soundfont1tbl", soundfont1tbl_ROM_START);
    loadTbl("soundfont/soundfont2tbl", soundfont2tbl_ROM_START);
}

void GameEngine::AudioInit() {
    LoadSoundfonts();
}

// Frame pacing and rendering

namespace {
using Clock = std::chrono::steady_clock;
inline long long NsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
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

} // namespace

void GameEngine::RunCommands(Gfx* Commands, const std::vector<std::unordered_map<Mtx*, MtxF>>& mtx_replacements,
                             size_t frameCount, float blendBase, float blendStep) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    const auto passT0 = Clock::now();
    sDeliveredSubFrames = 0;
    for (size_t frameIdx = 0; frameIdx < frameCount; frameIdx++) {
        if (frameIdx >= 1 && frameIdx - 1 < sMapBuildFutures.size()) {
            sMapBuildFutures[frameIdx - 1].wait();
        }
        if (frameIdx > 0 && sFilteredSubFrameNs > 0 && (sPassBudgetNs - NsSince(passT0)) < sFilteredSubFrameNs) {
            break;
        }
        const auto& m = mtx_replacements[frameIdx];
        const float subframeBlend = (blendStep > 0.0f && frameCount > 1)
                                        ? std::min(blendBase + (float)(frameIdx + 1) * blendStep, 1.0f)
                                        : ((frameCount > 1) ? (float)(frameIdx + 1) / (float)frameCount : 1.0f);
        if (frameCount > 1) {
            FrameInterpolation_ApplyAnimVertices(subframeBlend);
        }
        Nametag::SetSubframeBlend(subframeBlend);
        bool isFinalFrame = (frameIdx == frameCount - 1);
        if (frameCount > 1 || wndBase->IsFrameReady()) {
            auto gui = wndBase->GetGui();
            wndBase->GetMouseStateManager()->StartFrame();
            const uint32_t views = wnd->BeginRenderFrame();
            long long drawNs = 0;
#ifdef ENABLE_DEBUG_TOOLS
            interpreter->mDrawCallCount = 0;
            interpreter->mMarkedDrawCount = 0;
            interpreter->mDrawTextures.clear();
            interpreter->mMarkedTextures.clear();
#endif
            for (uint32_t view = 0; view < views; view++) {
                wnd->BeginRenderView(view);
                auto runT0 = Clock::now();
                gui->StartDraw();
                interpreter->StartFrame();
                interpreter->Run(Commands, m);
                if (OS_ViBlackActive()) {
                    interpreter->mGfxFrameBuffer = 0;
                    auto rapi = interpreter->GetCurrentRenderingAPI();
                    rapi->StartDrawToFramebuffer(0, 1.0f);
                    rapi->ClearFramebuffer(true, false);
                }
                gui->EndDraw();
                drawNs += NsSince(runT0);
                interpreter->EndFrame();
            }
            long long sample = drawNs;
            bool believe = true;
            if (sPassBudgetNs > 0 && drawNs > sPassBudgetNs) {
                sample = sPassBudgetNs;
                believe = ++sOverBudgetRun > 1;
            } else {
                sOverBudgetRun = 0;
            }
            if (believe) {
                if (sample > sFilteredSubFrameNs) {
                    sFilteredSubFrameNs = sample;
                } else {
                    sFilteredSubFrameNs += (sample - sFilteredSubFrameNs) / 8;
                }
            }
            sDeliveredSubFrames++;
#ifdef ENABLE_DEBUG_TOOLS
            ReportDrawTime(drawNs, views, interpreter->mDrawCallCount, (uint32_t)interpreter->mDrawTextures.size(),
                           interpreter->mMarkedDrawCount, (uint32_t)interpreter->mMarkedTextures.size(),
                           interpreter->mMarkedFlushCauses);
#else
            ReportDrawTime(drawNs, views, 0, 0, 0, 0, nullptr);
#endif
            CALL_EVENT(FrameDrawEnd);
        }
        interpreter->mInterpolationIndex++;
    }
    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetRawInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

void GameEngine::SetInterpolationRecorded(bool recorded) {
    sInterpolationRecorded = recorded;
}

namespace {
struct SubframePacing {
    int subframes;
    int fps;
    int viPerTick;
    long long budgetNs;
    float blendBase;
    float blendStep;
};

constexpr int RATE_SETTLE_TICKS = 90;

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
    if (++waited < RATE_SETTLE_TICKS) {
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

void ReportTickRate(int subframes, int delivered) {
#ifdef ENABLE_DEBUG_TOOLS
    static auto since = std::chrono::steady_clock::now();
    static int ticks = 0;
    static long long subframeTotal = 0;
    static long long deliveredTotal = 0;

    ticks++;
    subframeTotal += subframes;
    deliveredTotal += delivered;

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
    SPDLOG_INFO("game ticks {:.2f} of {} a second, {:.2f} sub-frames a tick asked and {:.2f} drawn, display {} Hz",
                ticks / seconds, 60 / gVIsPerFrame, (double)subframeTotal / ticks, (double)deliveredTotal / ticks,
                rate);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "game ticks %.2f of %d a second, %.2f sub-frames a tick asked and %.2f drawn, display %u Hz",
                        ticks / seconds, 60 / gVIsPerFrame, (double)subframeTotal / ticks,
                        (double)deliveredTotal / ticks, rate);
#endif

    since = now;
    ticks = 0;
    subframeTotal = 0;
    deliveredTotal = 0;
#else
    (void)subframes;
    (void)delivered;
#endif
}

constexpr int PACING_PROBE_TICKS = 30;

constexpr int PACING_GAP_TICKS = 8;

constexpr int PACING_WORK_MARGIN = 2;

int CurrentViPerTick() {
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

int EffectiveLogicFps() {
    int fps = 60 / CurrentViPerTick();
    return (fps < 1) ? 1 : fps;
}

int SubframesForTarget(int targetFps) {
    int subframes = targetFps / EffectiveLogicFps();
    return (subframes < 1) ? 1 : subframes;
}

SubframePacing ComputeSubframePacing() {
    int target_fps = (int)GameEngine::Instance->GetInterpolationFPS();
    int viPerTick = CurrentViPerTick();
    int subframesPerTick = SubframesForTarget(target_fps);

    float blendStep = 0.0f;
    int slotCount = 0;
    static float sSlotCarry = 0.0f;
    if (IsHeadsetWindow() && target_fps > 0) {
        const float slots = (float)target_fps * (float)viPerTick / 60.0f;
        if (slots >= 2.0f) {
            if (fabsf(slots - roundf(slots)) <= 0.05f) {
                subframesPerTick = (int)roundf(slots);
            } else {
                blendStep = 1.0f / slots;
                slotCount = (int)floorf((1.0f - sSlotCarry) * slots + 0.0001f);
                if (slotCount >= 2) {
                    subframesPerTick = slotCount;
                } else {
                    blendStep = 0.0f;
                    slotCount = 0;
                }
            }
        }
    }

    if (!sInterpolationRecorded) {
        subframesPerTick = 1;
    }

    if (IsHeadsetWindow()) {
        static int allowed = 0;
        static int probeCountdown = 0;
        static int asked = 0;
        static bool wasShort = false;
        static int scaledVi = 0;

        if (allowed < 1) {
            allowed = subframesPerTick;
        }

        if (scaledVi > 0 && viPerTick != scaledVi) {
            allowed = (allowed * viPerTick + scaledVi - 1) / scaledVi;
        }
        scaledVi = viPerTick;

        static Clock::time_point lastTick;
        const bool resumed = lastTick.time_since_epoch().count() != 0 && sPassBudgetNs > 0 &&
                             NsSince(lastTick) > sPassBudgetNs * PACING_GAP_TICKS;
        lastTick = Clock::now();
        if (resumed) {
            sFilteredSubFrameNs = 0;
            sDeliveredSubFrames = 0;
            sOverBudgetRun = 0;
            wasShort = false;
        }

        const bool isShort = asked > 0 && sDeliveredSubFrames > 0 && sDeliveredSubFrames < asked;
        const bool fitsTick = sFilteredSubFrameNs <= 0 || sPassBudgetNs <= 0 ||
                              sFilteredSubFrameNs * PACING_WORK_MARGIN * asked < sPassBudgetNs;
        if (isShort && wasShort && !fitsTick) {
            allowed = asked - 1;
            probeCountdown = PACING_PROBE_TICKS;
        } else if (!isShort && --probeCountdown <= 0) {
            allowed++;
            probeCountdown = PACING_PROBE_TICKS;
        }
        wasShort = isShort;

        if (allowed < 1) {
            allowed = 1;
        }
        if (allowed > subframesPerTick + 1) {
            allowed = subframesPerTick + 1;
        }
        asked = (allowed < subframesPerTick) ? allowed : subframesPerTick;
        subframesPerTick = asked;
    }

    float blendBase = 0.0f;
    if (blendStep > 0.0f && subframesPerTick == slotCount) {
        blendBase = sSlotCarry;
        sSlotCarry += (float)slotCount * blendStep - 1.0f;
        if (fabsf(sSlotCarry) < 0.001f) {
            sSlotCarry = 0.0f;
        }
    } else {
        blendStep = 0.0f;
        sSlotCarry = 0.0f;
    }

#ifdef ENABLE_XR_WINDOW
    {
        static double nextReport = 0.0;
        static double windowStart = 0.0;
        static int ticks = 0;
        static int askedTotal = 0;
        static int deliveredTotal = 0;
        static int viTotal = 0;
        const double now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count() /
                           1000.0;
        ++ticks;
        askedTotal += subframesPerTick;
        deliveredTotal += sDeliveredSubFrames;
        viTotal += viPerTick;
        if (now >= nextReport) {
            const double window = now - windowStart;
            if (nextReport > 0.0 && ticks > 0 && window > 0.0) {
                SPDLOG_INFO("xr pacing: target {} Hz, ticks {:.1f}/s, vi {:.2f}, asked {:.2f}, delivered {:.2f}, "
                            "work {:.2f} ms",
                            target_fps, (double)ticks / window, (double)viTotal / ticks, (double)askedTotal / ticks,
                            (double)deliveredTotal / ticks, (double)sFilteredSubFrameNs / 1e6);
            }
            nextReport = now + 1.0;
            windowStart = now;
            ticks = 0;
            askedTotal = 0;
            deliveredTotal = 0;
            viTotal = 0;
        }
    }
#endif

    int fps = subframesPerTick * 60 / viPerTick;
    if (fps < 1) {
        fps = 1;
    }

    const long long budgetNs =
        (blendStep > 0.0f) ? 1000000000LL * subframesPerTick / target_fps : 1000000000LL * viPerTick / 60;

    return { subframesPerTick, fps, viPerTick, budgetNs, blendBase, blendStep };
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
} // namespace

bool GameEngine::IsInterpolationEnabled() {
    return (int)GetInterpolationFPS() > EffectiveLogicFps();
}

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    SelectDisplayRefreshRate(wnd.get());

#ifdef ENABLE_XR_WINDOW
    Fast::SetXrDioramaDepth(CVarGetFloat(CVAR_SETTING("XrDioramaDepth"), 2.0f));
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
#endif

#ifdef ENABLE_OPENXR
    wnd->SetResolutionMultiplier(CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1.0f));

    Fast::SetXrStereo(CVarGetInteger(CVAR_SETTING("XrStereo"), 1) != 0);
    Fast::SetXrEdgeSoftness(CVarGetFloat(CVAR_SETTING("XrEdgeSoftness"), 0.36f));
    Fast::SetXrEdgeFloat(CVarGetFloat(CVAR_SETTING("XrEdgeFloat"), 0.15f));
#endif

    // if(gEnableGammaBoost) {
    //     wnd->EnableSRGBMode();
    // }
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);

    static std::vector<std::unordered_map<Mtx*, MtxF>> mtx_replacements;

    const SubframePacing pacing = ComputeSubframePacing();
    const int subframesPerTick = pacing.subframes;
    const int fps = pacing.fps;

    if ((int)mtx_replacements.size() < subframesPerTick) {
        mtx_replacements.resize(subframesPerTick);
    }
    size_t activeFrames = 0;
    sMapBuildFutures.clear();
    for (int i = 1; i <= subframesPerTick; i++) {
        const float t = (pacing.blendStep > 0.0f) ? pacing.blendBase + (float)i * pacing.blendStep
                                                  : (float)i / (float)subframesPerTick;
        if (t < 0.9995f) {
            if (i == 1) {
                FrameInterpolation_Interpolate(t, mtx_replacements[activeFrames]);
            } else {
                auto* map = &mtx_replacements[activeFrames];
                sMapBuildFutures.push_back(
                    std::async(std::launch::async, [t, map] { FrameInterpolation_Interpolate(t, *map); }));
            }
        } else {
            mtx_replacements[activeFrames].clear();
        }
        activeFrames++;
    }

    sPassBudgetNs = pacing.budgetNs;

    if (wnd != nullptr) {
        wnd->SetTargetFps(fps);
        wnd->SetMaximumFrameLatency(2);
    }

    if (GfxDebuggerIsDebugging()) {
        if (mtx_replacements.empty()) {
            mtx_replacements.emplace_back();
        }
        mtx_replacements[0].clear();
        activeFrames = 1;
    }

    RunCommands(commands, mtx_replacements, activeFrames, pacing.blendBase, pacing.blendStep);
    ReportTickRate(pacing.subframes, sDeliveredSubFrames);

    for (auto& f : sMapBuildFutures) {
        if (f.valid()) {
            f.wait();
        }
    }
    sMapBuildFutures.clear();
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

// Version reporting and message boxes

void GameEngine::ShowMessage(const char* title, const char* message, SDL_MessageBoxFlags type) {
#if defined(__SWITCH__)
    SPDLOG_ERROR(message);
#else
    SDL_ShowSimpleMessageBox(type, title, message, nullptr);
    SPDLOG_ERROR(message);
#endif
}

bool GameEngine::HasVersion(BKVersion ver) {
    auto versions = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetGameVersions();
    return std::find(versions.begin(), versions.end(), ver) != versions.end();
}

extern "C" bool GameEngine_HasVersion(BKVersion ver) {
    return GameEngine::HasVersion(ver);
}

std::vector<BKVersion> GameEngine::GetAvailableVersions() {
    static constexpr BKVersion kKnown[] = { BK_VER_US_10, BK_VER_US_11, BK_VER_PAL, BK_VER_JP };
    auto loaded = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetGameVersions();
    std::vector<BKVersion> present;
    for (BKVersion ver : kKnown) {
        if (std::find(loaded.begin(), loaded.end(), static_cast<uint32_t>(ver)) != loaded.end()) {
            present.push_back(ver);
        }
    }
    return present;
}

extern "C" uint32_t GameEngine_GetSampleRate() {
    auto player = Ship::Context::GetRawInstance()->GetAudio()->GetAudioPlayer();
    if (player == nullptr) {
        return 0;
    }

    if (!player->IsInitialized()) {
        return 0;
    }

    return player->GetSampleRate();
}

// End

// C ABI shims

Fast::Interpreter* GameEngine_GetInterpreter() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow())
        ->GetInterpreterWeak()
        .lock()
        .get();
}

extern "C" float GameEngine_GetAspectRatio() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.aspect_ratio;
}

extern "C" uint32_t GameEngine_GetGameVersion() {
    return 0x00000001;
}

extern "C" uint8_t GameEngine_OTRSigCheck(const char* data) {
    if (data == nullptr) {
        return 0;
    }
    return strncmp(data, sOtrSignature, strlen(sOtrSignature)) == 0;
}

extern "C" void GameEngine_GetTextureInfo(const char* path, int32_t* width, int32_t* height, float* scale,
                                          bool* custom) {
    if (GameEngine_OTRSigCheck(path) != 1) {
        *custom = false;
        return;
    }
    std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(path));
    *width = tex->Width;
    *height = tex->Height;
    *scale = tex->VPixelScale;
    *custom = tex->Flags & (1 << 0);
}

// Gets the width of the main ImGui window
extern "C" uint32_t OTRGetCurrentWidth() {
    return GameEngine::Instance->context->GetWindow()->GetWidth();
}

// Gets the height of the main ImGui window
extern "C" uint32_t OTRGetCurrentHeight() {
    return GameEngine::Instance->context->GetWindow()->GetHeight();
}

extern "C" float OTRGetHUDAspectRatio() {
    if (CVarGetInteger("gHUDAspectRatio.Enabled", 0) == 0 || CVarGetInteger("gHUDAspectRatio.X", 0) == 0 ||
        CVarGetInteger("gHUDAspectRatio.Y", 0) == 0) {
        return GameEngine_GetAspectRatio();
    }
    return ((float)CVarGetInteger("gHUDAspectRatio.X", 1) / (float)CVarGetInteger("gHUDAspectRatio.Y", 1));
}

static float OTRDimensionFromEdge(float v, float aspectRatio, bool fromRight) {
    auto interpreter = GameEngine_GetInterpreter();
    const uint32_t nativeWidth = interpreter->mNativeDimensions.width;
    const float aspect = (aspectRatio > 0) ? aspectRatio : interpreter->mCurDimensions.aspect_ratio;
    const float halfSpan = (nativeWidth * 3.0f / 4.0f / 2.0f) * aspect;

    return fromRight ? (nativeWidth / 2 + halfSpan - (nativeWidth - v)) : (nativeWidth / 2 - halfSpan + v);
}

extern "C" float OTRGetDimensionFromLeftEdge(float v) {
    return OTRDimensionFromEdge(v, 0.0f, false);
}

extern "C" float OTRGetDimensionFromRightEdge(float v) {
    return OTRDimensionFromEdge(v, 0.0f, true);
}

extern "C" float OTRGetDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    return OTRDimensionFromEdge(v, aspectRatio, false);
}

extern "C" float OTRGetDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    return OTRDimensionFromEdge(v, aspectRatio, true);
}

extern "C" float OTRGetDimensionFromLeftEdgeOverride(float v) {
    return OTRDimensionFromEdge(v, OTRGetHUDAspectRatio(), false);
}

extern "C" float OTRGetDimensionFromRightEdgeOverride(float v) {
    return OTRDimensionFromEdge(v, OTRGetHUDAspectRatio(), true);
}

extern "C" uint32_t OTRGetGameRenderWidth() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.width;
}

extern "C" uint32_t OTRGetGameRenderHeight() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.height;
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdge(float v) {
    return ((int)floorf(OTRGetDimensionFromLeftEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdge(float v) {
    return ((int)ceilf(OTRGetDimensionFromRightEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    return ((int)floorf(OTRGetDimensionFromLeftEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    return ((int)ceilf(OTRGetDimensionFromRightEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeOverride(float v) {
    return OTRGetRectDimensionFromLeftEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeOverride(float v) {
    return OTRGetRectDimensionFromRightEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int32_t OTRConvertHUDXToScreenX(int32_t v) {
    auto interpreter = GameEngine_GetInterpreter();
    float gameAspectRatio = interpreter->mCurDimensions.aspect_ratio;
    int32_t gameHeight = interpreter->mCurDimensions.height;
    int32_t gameWidth = interpreter->mCurDimensions.width;
    float hudAspectRatio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    int32_t hudHeight = gameHeight;
    int32_t hudWidth = static_cast<int32_t>(hudHeight * hudAspectRatio);
    float hudScreenRatio = (hudWidth / (float)SCREEN_WIDTH);
    float hudCoord = v * hudScreenRatio;
    float gameOffset = static_cast<float>((gameWidth - hudWidth) / 2);
    float gameCoord = hudCoord + gameOffset;
    float gameScreenRatio = ((float)SCREEN_WIDTH / gameWidth);
    float screenScaledCoord = gameCoord * gameScreenRatio;
    int32_t screenScaledCoordInt = static_cast<int32_t>(screenScaledCoord);
    return screenScaledCoordInt;
}

extern "C" void* GameEngine_Malloc(size_t size) {
    MemoryPool.push_back((uint8_t*)malloc(size));
    return MemoryPool.back();
}

extern "C" void GameEngine_Free(void* ptr) {
    for (auto it = MemoryPool.begin(); it != MemoryPool.end(); ++it) {
        if (*it == ptr) {
            free(ptr);
            MemoryPool.erase(it);
            break;
        }
    }
}
