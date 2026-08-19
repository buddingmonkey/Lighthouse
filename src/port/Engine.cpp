#include "Engine.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <future>
#ifdef ENABLE_DEBUG_TOOLS
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

// Radians of the window one unit of ImGui scale covers, for a menu drawn on a window in the room,
// and the smallest angle the menu is laid out for. The floor holds about 830 units of menu width,
// which is what the widest row needs.
static constexpr float MENU_ANGLE_PER_UNIT = 0.0009f;
static constexpr float MENU_ANGLE_MIN = 0.75f;
std::shared_ptr<Fast::Fast3dWindow> lhFast3dWindow;

bool IsHeadsetWindow() {
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    return window != nullptr && window->GetWindowBackend() == Fast::WindowBackend::FAST3D_OPENXR_OPENGL;
}

uint32_t DefaultImGuiScaleIndex() {
#ifdef LIGHTHOUSE_MOBILE
    // A tablet takes the larger menu; a phone has too little screen to navigate it, so split at a 600 short side.
    static const uint32_t index = []() {
        auto window = Ship::Context::GetRawInstance()->GetWindow();
        if (window == nullptr) {
            return 0u;
        }
        // A headset window is measured in an angle, not in a screen size, so it takes the unit the
        // angle is set in rather than the phone or tablet step.
        if (IsHeadsetWindow()) {
            return 1u;
        }
        float shortSide = static_cast<float>(std::min(window->GetWidth(), window->GetHeight()));
#ifdef __ANDROID__
        // Android measures windows in pixels, and 600 is a count of density-independent ones.
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

// Android counts window units in pixels where iOS counts points, so the menu comes out much smaller there.
float ImGuiDensityScale() {
#ifdef __ANDROID__
    static const float density = []() {
        float ddpi = 0.0f;
        float hdpi = 0.0f;
        float vdpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && vdpi > 0.0f) {
            return std::max(1.0f, vdpi / 160.0f);
        }
        return 1.0f;
    }();

    // A headset reads the menu on a window in the room, and the only thing that decides whether a
    // letter can be read is the angle it covers in the eye. That angle is the angle the window
    // spans over the width of the picture drawn on it, so the scale is the width over the angle and
    // the display DPI does not come into it. DPI stood in for the width before, which held on a
    // headset whose panel is the picture and failed on one that draws the game across a whole
    // binocular panel: Quest hands the game 4128 pixels where Galaxy XR hands it 1536, and the same
    // DPI gave letters a third of the angle.
    //
    // A window put far away and left small covers too small an angle to hold a menu at that rate.
    // The angle has a floor, so the letters give up their angle rather than the menu its width.
    if (IsHeadsetWindow()) {
#ifdef ENABLE_OPENXR
        auto window = Ship::Context::GetRawInstance()->GetWindow();
        const float angularWidth = Fast::GetXrWindowAngularWidth();
        if (angularWidth > 0.0f && window != nullptr && window->GetWidth() > 0) {
            return MENU_ANGLE_PER_UNIT * static_cast<float>(window->GetWidth()) /
                   std::max(angularWidth, MENU_ANGLE_MIN);
        }
#endif
        return density * 0.66f;
    }
    return density;
#else
    return 1.0f;
#endif
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

// The cost of a sub-frame, filtered, and how many sub-frames the last tick put on the screen. One
// raw sample flaps too much to decide on, and the requested count says nothing about what was
// delivered.
long long sFilteredSubFrameNs = 0;
int sDeliveredSubFrames = 0;
} // namespace

bool portArchiveVersionMatch = false;
std::string assets_path;

// Tracks the scale already baked into the ImGui style, which always starts unscaled.
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
    // Otherwise the accelerometer appears as a phantom joystick in the device list.
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    // Without this hint SDL sets the Android activity to FULL_USER, which allows the portrait the manifest excludes.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#ifdef __IOS__
    // Play through the ring/silent switch like other games do.
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    // Hides the home indicator and defers the edge swipes that would otherwise reach the
    // touch controls sitting along the bottom of the screen.
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
    this->context->InitAudio({ .SampleRate = 22000, .SampleLength = 736, .DesiredBuffered = 2208 });

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
    // On a headset the scale follows the window as well as the setting, and the window resizes when
    // the game first says what field of view it needs and whenever a hand pulls a corner.
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
    // exec is unavailable to sandboxed apps; the user relaunches the app themselves.
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

// A headset walks the display list once per eye, so a list the processor is slow to walk is slow
// twice. This tells a sub-frame the processor holds up from one the graphics chip holds up: the
// draw time goes up with the first and stays where it is with the second.
void ReportDrawTime(long long drawNs, uint32_t views, uint32_t drawCalls) {
#ifdef ENABLE_DEBUG_TOOLS
    static auto since = std::chrono::steady_clock::now();
    static long long total = 0;
    static long long worst = 0;
    static long long callTotal = 0;
    static uint32_t callWorst = 0;
    static int subframes = 0;

    total += drawNs;
    if (drawNs > worst) {
        worst = drawNs;
    }
    callTotal += drawCalls;
    if (drawCalls > callWorst) {
        callWorst = drawCalls;
    }
    subframes++;

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - since).count();
    if (seconds < 5.0) {
        return;
    }

    SPDLOG_INFO("draw {:.2f} ms a sub-frame, worst {:.2f} ms, {:.0f} draws a sub-frame, worst {}, {} views, "
                "{:.1f} sub-frames a second",
                total / (double)subframes / 1.0e6, worst / 1.0e6, (double)callTotal / subframes, callWorst, views,
                subframes / seconds);
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "draw %.2f ms a sub-frame, worst %.2f ms, %.0f draws a sub-frame, worst %u, %u views, "
                        "%.1f sub-frames a second",
                        total / (double)subframes / 1.0e6, worst / 1.0e6, (double)callTotal / subframes, callWorst,
                        views, subframes / seconds);

    since = now;
    total = 0;
    worst = 0;
    callTotal = 0;
    callWorst = 0;
    subframes = 0;
#else
    (void)drawNs;
    (void)views;
    (void)drawCalls;
#endif
}

} // namespace

void GameEngine::RunCommands(Gfx* Commands, const std::vector<std::unordered_map<Mtx*, MtxF>>& mtx_replacements,
                             size_t frameCount) {
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
        // Stop once another sub-frame no longer fits in what the tick's worth
        // of wall time has left.
        if (frameIdx > 0 && sFilteredSubFrameNs > 0 && (sPassBudgetNs - NsSince(passT0)) < sFilteredSubFrameNs) {
            break;
        }
        const auto& m = mtx_replacements[frameIdx];
        const float subframeBlend = (frameCount > 1) ? (float)(frameIdx + 1) / (float)frameCount : 1.0f;
        if (frameCount > 1) {
            FrameInterpolation_ApplyAnimVertices(subframeBlend);
        }
        Nametag::SetSubframeBlend(subframeBlend);
        bool isFinalFrame = (frameIdx == frameCount - 1);
        if (frameCount > 1 || wndBase->IsFrameReady()) {
            auto gui = wndBase->GetGui();
            wndBase->GetMouseStateManager()->StartFrame();
            // A headset draws the sub-frame once per eye, each with its own off-axis projection.
            const uint32_t views = wnd->BeginRenderFrame();
            long long drawNs = 0;
            interpreter->mDrawCallCount = 0;
            for (uint32_t view = 0; view < views; view++) {
                wnd->BeginRenderView(view);
                // Sample the CPU cost of producing this sub-frame, both eyes and neither present.
                // A present waits for the display, and the budget below must weigh the work, not
                // the wait, or a sub-frame that exactly fills its slot looks like one that misses.
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
            // Believe a rise at once, so a scene that gets heavy does not overrun even one tick.
            // Ease a fall in, so one cheap sub-frame does not ask for the full count again. A
            // sub-frame longer than the whole tick is a hitch and not the price of the next one,
            // so let it raise the estimate to the budget and no further.
            const long long sample = (sPassBudgetNs > 0 && drawNs > sPassBudgetNs) ? sPassBudgetNs : drawNs;
            if (sample > sFilteredSubFrameNs) {
                sFilteredSubFrameNs = sample;
            } else {
                sFilteredSubFrameNs += (sample - sFilteredSubFrameNs) / 8;
            }
            sDeliveredSubFrames++;
            ReportDrawTime(drawNs, views, interpreter->mDrawCallCount);
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
};

// A headset only looks right when its refresh rate is a whole multiple of the game's logic rate:
// the sub-frame count below is an integer, so anything else beats against the panel. Banjo-Kazooie
// runs its logic at 30 Hz, and a 72 Hz panel therefore presents 60. Ask for the fastest rate that
// divides, once, and let ComputeSubframePacing read it back through GetCurrentRefreshRate.
// Ticks to let a rate settle before the next one down is taken. The window re-asks a refused rate
// a few times, each on an event from the runtime, so the wait has to outlast that.
constexpr int RATE_SETTLE_TICKS = 90;

void SelectDisplayRefreshRate(Fast::Fast3dWindow* wnd) {
    if (!IsHeadsetWindow()) {
        return;
    }
    // Quest offers 120 as well as 90, and both divide. The setting is how a user who would rather
    // have the battery, or who finds the tick rate falling short, holds it down.
    const int cap = CVarGetInteger(CVAR_SETTING("XrMaxRate"), 120);

    // The rates come with the session, which is up after the first frames draw. Asking once and
    // counting the ask as spent left the panel wherever the runtime put it, because the list was
    // still empty at the only tick that looked at it.
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

    // A runtime is free to refuse the rate and say nothing more about it. Wait out the window's own
    // retries, then take the next rate down, because a panel the logic rate does not divide beats
    // against the game for the whole run.
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

// A display the game cannot keep up with does not drop frames, it runs the game slowly: the VI
// retrace gates a tick on the render finishing. So the speed of the game is the measurement that
// decides whether a refresh rate can be used, and the present rate on its own says nothing.
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
    __android_log_print(ANDROID_LOG_INFO, "LighthouseXR",
                        "game ticks %.2f of %d a second, %.2f sub-frames a tick asked and %.2f drawn, display %u Hz",
                        ticks / seconds, 60 / gVIsPerFrame, (double)subframeTotal / ticks,
                        (double)deliveredTotal / ticks, rate);

    since = now;
    ticks = 0;
    subframeTotal = 0;
    deliveredTotal = 0;
#else
    (void)subframes;
    (void)delivered;
#endif
}

// Ticks between two attempts to raise the sub-frame count again. One truncated tick is the cost of
// finding out that a scene got cheaper, so pay it about once a second, not every tick.
constexpr int PACING_PROBE_TICKS = 30;

// Game-logic VI per tick: gVIsPerFrame (=2 -> 30 Hz) normally; demo
// replay and cutscene stutter raise it for slow N64 frames.
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

    if (!sInterpolationRecorded) {
        subframesPerTick = 1;
    }

    // A sub-frame the tick has no time for is not drawn: RunCommands leaves the pass part way
    // through, and the interpolation map it built is thrown away. Ask for what the tick really
    // delivers instead, so a heavy scene settles on a steady count rather than asking for six and
    // putting three on the screen.
    //
    // The measurement is the delivered count, not the draw time. Draw time is only the part of a
    // sub-frame between StartDraw and EndDraw: it leaves out the frame submission and the wait for
    // the display, which on a headset is most of the cost. The delivered count already holds all
    // of it. Pacing on the wall time of a sub-frame would not do, because the pacing itself puts
    // the wait there, so the number would chase its own tail down to one.
    //
    // A cutscene, a dialog and a demo each change the VI count, and with it both the length of a
    // tick and the count of sub-frames that fits in one. So the learned count is measured against
    // what the last tick asked for, and a target that drops is not written back into it: the
    // number has to survive the way out of the cutscene as well as the way in.
    {
        static int allowed = 0;
        static int probeCountdown = 0;
        static int asked = 0;
        static bool wasShort = false;

        if (allowed < 1) {
            allowed = subframesPerTick;
        }

        const bool isShort = asked > 0 && sDeliveredSubFrames > 0 && sDeliveredSubFrames < asked;
        if (isShort && wasShort) {
            // Two ticks in a row ran out of time, so the scene is heavy. One tick on its own is a
            // hitch - a map load, a first texture upload - and it must not cost seconds of a lower
            // rate, which is what the transitions into and out of a cutscene showed.
            allowed = sDeliveredSubFrames;
            probeCountdown = PACING_PROBE_TICKS;
        } else if (!isShort && --probeCountdown <= 0) {
            // Ask for one more now and then, or a scene that gets cheaper never gets it back.
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

    // paceFps drives DXGI's per-present wait so that subframes * 1/paceFps =
    // viPerTick/60 wall (= game time per tick). Derived from viPerTick rather than
    // effective_logic_fps: the latter is truncated (VI=7 -> 8, not 8.57), which would
    // stretch wall time on the odd VI counts demo playback hands us every tick.
    int fps = subframesPerTick * 60 / viPerTick;
    if (fps < 1) {
        fps = 1;
    }

    return { subframesPerTick, fps, viPerTick };
}

#ifdef ENABLE_OPENXR
// The menu and the window's own handles write the same number. Push it when the menu moved it since
// the last frame, and read the window back when the menu did not, so the one that moved last wins
// and a slider always shows where the hand left the window. Both numbers are themselves, so the
// same conversion serves both ways.
void SyncXrSetting(const char* cVar, float low, float high, float defaultValue, float& pushed, float held,
                   void (*apply)(float), float (*convert)(float)) {
    const float shown = std::clamp(CVarGetFloat(cVar, defaultValue), low, high);
    if (shown != pushed) {
        apply(convert(shown));
        pushed = shown;
        return;
    }
    const float left = std::clamp(convert(held), low, high);
    if (fabsf(left - shown) > 0.001f) {
        CVarSetFloat(cVar, left);
        pushed = left;
    }
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

#ifdef ENABLE_OPENXR
    // The range is meters from the user to the glass, and the size is meters of glass. They are
    // apart: a range that goes out leaves the window the width it had, so it goes small in the eye
    // and takes the diorama into the room with it. The move bar and the corner handles write the
    // same two numbers from inside the headset.
    static float pushedRange = 0.0f;
    static float pushedScale = 0.0f;
    SyncXrSetting(CVAR_SETTING("XrWindowRange"), 0.5f, 4.0f, 1.3f, pushedRange, Fast::GetXrWindowDistance(),
                  Fast::SetXrWindowDistance, [](float value) { return value; });
    SyncXrSetting(CVAR_SETTING("XrWindowScale"), 0.5f, 8.0f, 2.6f, pushedScale, Fast::GetXrWindowScale(),
                  Fast::SetXrWindowScale, [](float value) { return value; });
    Fast::SetXrDioramaDepth(CVarGetFloat(CVAR_SETTING("XrDioramaDepth"), 2.0f));

    // The window covers part of the view, and everything the game draws past what that window can
    // show is thrown away. The window backend now reports the size of that fit as the window size,
    // so the multiplier carries Internal Resolution alone: 1 is one game pixel to an eye pixel and
    // 2 is a picture the blit resolves down.
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
        if (i < subframesPerTick) {
            float t = (float)i / (float)subframesPerTick;
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

    sPassBudgetNs = 1000000000LL * pacing.viPerTick / 60;

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

    RunCommands(commands, mtx_replacements, activeFrames);
    ReportTickRate(pacing.subframes, sDeliveredSubFrames);

    for (auto& f : sMapBuildFutures) {
        if (f.valid()) {
            f.wait();
        }
    }
    sMapBuildFutures.clear();
}

uint32_t GameEngine::GetInterpolationFPS() {
    // A headset is not a screen a player chooses a frame rate for. Anything below the panel rate
    // beats against it, and the parallax the off-axis frustum bakes into each eye only updates as
    // often as the game presents. The setting stays reachable, it just starts on.
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
