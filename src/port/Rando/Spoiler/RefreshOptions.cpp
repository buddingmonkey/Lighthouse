#include "Spoiler.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <filesystem>

#include <libultraship/libultra/types.h>
#include "port/UI/cvar_prefixes.h"

std::vector<std::string> Rando::Spoiler::spoilerLogs;
// Resolved on first use, not at static-init time; see the note in Rando.cpp.
static const std::filesystem::path& RandomizerFolderPath() {
    static const std::filesystem::path path(Ship::Context::GetPathRelativeToAppDirectory("randomizer", "bk64"));
    return path;
}

void Rando::Spoiler::RefreshSpoilerLogs() {
    Rando::Spoiler::spoilerLogs.clear();

    Rando::Spoiler::spoilerLogs.push_back("Generate New Seed");
    s32 spoilerFileIndex = -1;

    if (!std::filesystem::exists(RandomizerFolderPath())) {
        std::filesystem::create_directory(RandomizerFolderPath());
    }

    for (const auto& entry : std::filesystem::directory_iterator(RandomizerFolderPath())) {
        if (entry.is_regular_file()) {
            std::string fileName = entry.path().filename().string();

            Rando::Spoiler::spoilerLogs.push_back(fileName);

            // Check if the current file is the one set in the cvar
            if (fileName == CVarGetString(CVAR_RANDOMIZER_SETTING("SpoilerFile"), "")) {
                spoilerFileIndex = Rando::Spoiler::spoilerLogs.size() - 1;
            }
        }
    }

    if (spoilerFileIndex == -1) {
        CVarSetInteger(CVAR_RANDOMIZER_SETTING("SpoilerFileIndex"), 0);
        CVarSetString(CVAR_RANDOMIZER_SETTING("SpoilerFile"), "");
    } else {
        CVarSetInteger(CVAR_RANDOMIZER_SETTING("SpoilerFileIndex"), spoilerFileIndex);
    }
}