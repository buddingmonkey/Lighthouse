#include "Rando.h"
#include <ship/Context.h>
#include "port/Enhancements/Events/Hooks/Events.h"
#include "ObjectBehavior/ObjectBehavior.h"
#include "MiscBehavior/MiscBehavior.h"
#include "port/Rando/CheckTracker/CheckTracker.h"
// #include "port/Rando/EntranceTracker/EntranceTracker.h"
#include "port/Rando/Spoiler/Spoiler.h"
#include "port/ShipInit.hpp"

#include "spdlog/spdlog.h"

#include <fstream>
#include <filesystem>
namespace fs = std::filesystem;

int16_t selectedFileNum = DEFAULT_FILE_NUM;

// Resolved on first use: the app directory comes from the platform at runtime, and on Android
// that is SDL's JNI bridge, which does not exist yet while static initialisers run.
static const fs::path& RandomizerFolderPath() {
    static const fs::path path(Ship::Context::GetPathRelativeToAppDirectory("randomizer", "bk64"));
    return path;
}

// Entry point for the module, run once on game boot
void Rando::Init() {
    if (!fs::exists(RandomizerFolderPath())) {
        fs::create_directory(RandomizerFolderPath());
    }

    Rando::Spoiler::RefreshSpoilerLogs();
    Rando::MiscBehavior::Init();
    // Rando::EntranceTracker::Init();
    // Ship::Context::GetInstance()->GetFileDropMgr()->RegisterDropHandler(Rando::Spoiler::HandleFileDropped);

    REGISTER_LISTENER(InitRandoEvents, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        InitRandoEvents* ev = (InitRandoEvents*)event;

        Rando::ObjectBehavior::Init();
        Rando::CheckTracker::Init();
    });
}
