#include "WarpSweep.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <libultraship/libultraship.h>

#include "enums.h"

extern "C" void func_8031D04C(enum map_e map, s32 exit_id);

namespace Lighthouse {
namespace DevTools {

namespace {

constexpr int kTicksPerSecond = 30;
constexpr int kPollTicks = 30;

const map_e kSweepMaps[] = {
    MAP_1_SM_SPIRAL_MOUNTAIN,
    MAP_2_MM_MUMBOS_MOUNTAIN,
    MAP_5_TTC_BLUBBERS_SHIP,
    MAP_6_TTC_NIPPERS_SHELL,
    MAP_7_TTC_TREASURE_TROVE_COVE,
    MAP_A_TTC_SANDCASTLE,
    MAP_B_CC_CLANKERS_CAVERN,
    MAP_C_MM_TICKERS_TOWER,
    MAP_D_BGS_BUBBLEGLOOP_SWAMP,
    MAP_E_MM_MUMBOS_SKULL,
    MAP_10_BGS_MR_VILE,
    MAP_11_BGS_TIPTUP,
    MAP_12_GV_GOBIS_VALLEY,
    MAP_13_GV_MEMORY_GAME,
    MAP_14_GV_SANDYBUTTS_MAZE,
    MAP_15_GV_WATER_PYRAMID,
    MAP_16_GV_RUBEES_CHAMBER,
    MAP_1A_GV_INSIDE_JINXY,
    MAP_1B_MMM_MAD_MONSTER_MANSION,
    MAP_1C_MMM_CHURCH,
    MAP_1D_MMM_CELLAR,
    MAP_1E_CS_START_NINTENDO,
    MAP_1F_CS_START_RAREWARE,
    MAP_20_CS_END_NOT_100,
    MAP_21_CC_WITCH_SWITCH_ROOM,
    MAP_22_CC_INSIDE_CLANKER,
    MAP_23_CC_GOLDFEATHER_ROOM,
    MAP_24_MMM_TUMBLARS_SHED,
    MAP_25_MMM_WELL,
    MAP_26_MMM_NAPPERS_ROOM,
    MAP_27_FP_FREEZEEZY_PEAK,
    MAP_28_MMM_EGG_ROOM,
    MAP_29_MMM_NOTE_ROOM,
    MAP_2A_MMM_FEATHER_ROOM,
    MAP_2B_MMM_SECRET_CHURCH_ROOM,
    MAP_2C_MMM_BATHROOM,
    MAP_2D_MMM_BEDROOM,
    MAP_2E_MMM_HONEYCOMB_ROOM,
    MAP_2F_MMM_WATERDRAIN_BARREL,
    MAP_30_MMM_MUMBOS_SKULL,
    MAP_31_RBB_RUSTY_BUCKET_BAY,
    MAP_34_RBB_ENGINE_ROOM,
    MAP_35_RBB_WAREHOUSE,
    MAP_36_RBB_BOATHOUSE,
    MAP_37_RBB_CONTAINER_1,
    MAP_38_RBB_CONTAINER_3,
    MAP_39_RBB_CREW_CABIN,
    MAP_3A_RBB_BOSS_BOOM_BOX,
    MAP_3B_RBB_STORAGE_ROOM,
    MAP_3C_RBB_KITCHEN,
    MAP_3D_RBB_NAVIGATION_ROOM,
    MAP_3E_RBB_CONTAINER_2,
    MAP_3F_RBB_CAPTAINS_CABIN,
    MAP_40_CCW_HUB,
    MAP_41_FP_BOGGYS_IGLOO,
    MAP_43_CCW_SPRING,
    MAP_44_CCW_SUMMER,
    MAP_45_CCW_AUTUMN,
    MAP_46_CCW_WINTER,
    MAP_47_BGS_MUMBOS_SKULL,
    MAP_48_FP_MUMBOS_SKULL,
    MAP_4A_CCW_SPRING_MUMBOS_SKULL,
    MAP_4B_CCW_SUMMER_MUMBOS_SKULL,
    MAP_4C_CCW_AUTUMN_MUMBOS_SKULL,
    MAP_4D_CCW_WINTER_MUMBOS_SKULL,
    MAP_53_FP_CHRISTMAS_TREE,
    MAP_5A_CCW_SUMMER_ZUBBA_HIVE,
    MAP_5B_CCW_SPRING_ZUBBA_HIVE,
    MAP_5C_CCW_AUTUMN_ZUBBA_HIVE,
    MAP_5E_CCW_SPRING_NABNUTS_HOUSE,
    MAP_5F_CCW_SUMMER_NABNUTS_HOUSE,
    MAP_60_CCW_AUTUMN_NABNUTS_HOUSE,
    MAP_61_CCW_WINTER_NABNUTS_HOUSE,
    MAP_62_CCW_WINTER_HONEYCOMB_ROOM,
    MAP_63_CCW_AUTUMN_NABNUTS_WATER_SUPPLY,
    MAP_64_CCW_WINTER_NABNUTS_WATER_SUPPLY,
    MAP_65_CCW_SPRING_WHIPCRACK_ROOM,
    MAP_66_CCW_SUMMER_WHIPCRACK_ROOM,
    MAP_67_CCW_AUTUMN_WHIPCRACK_ROOM,
    MAP_68_CCW_WINTER_WHIPCRACK_ROOM,
    MAP_69_GL_MM_LOBBY,
    MAP_6A_GL_TTC_AND_CC_PUZZLE,
    MAP_6B_GL_180_NOTE_DOOR,
    MAP_6C_GL_RED_CAULDRON_ROOM,
    MAP_6D_GL_TTC_LOBBY,
    MAP_6E_GL_GV_LOBBY,
    MAP_6F_GL_FP_LOBBY,
    MAP_70_GL_CC_LOBBY,
    MAP_71_GL_STATUE_ROOM,
    MAP_72_GL_BGS_LOBBY,
    MAP_74_GL_GV_PUZZLE,
    MAP_75_GL_MMM_LOBBY,
    MAP_76_GL_640_NOTE_DOOR,
    MAP_77_GL_RBB_LOBBY,
    MAP_78_GL_RBB_AND_MMM_PUZZLE,
    MAP_79_GL_CCW_LOBBY,
    MAP_7A_GL_CRYPT,
    MAP_7B_CS_INTRO_GL_DINGPOT_1,
    MAP_7C_CS_INTRO_BANJOS_HOUSE_1,
    MAP_7D_CS_SPIRAL_MOUNTAIN_1,
    MAP_7E_CS_SPIRAL_MOUNTAIN_2,
    MAP_7F_FP_WOZZAS_CAVE,
    MAP_80_GL_FF_ENTRANCE,
    MAP_81_CS_INTRO_GL_DINGPOT_2,
    MAP_82_CS_ENTERING_GL_MACHINE_ROOM,
    MAP_83_CS_GAME_OVER_MACHINE_ROOM,
    MAP_85_CS_SPIRAL_MOUNTAIN_3,
    MAP_86_CS_SPIRAL_MOUNTAIN_4,
    MAP_87_CS_SPIRAL_MOUNTAIN_5,
    MAP_88_CS_SPIRAL_MOUNTAIN_6,
    MAP_89_CS_INTRO_BANJOS_HOUSE_2,
    MAP_8A_CS_INTRO_BANJOS_HOUSE_3,
    MAP_8B_RBB_ANCHOR_ROOM,
    MAP_8C_SM_BANJOS_HOUSE,
    MAP_8D_MMM_INSIDE_LOGGO,
    MAP_8E_GL_FURNACE_FUN,
    MAP_8F_TTC_SHARKFOOD_ISLAND,
    MAP_90_GL_BATTLEMENTS,
    MAP_91_FILE_SELECT,
    MAP_92_GV_SNS_CHAMBER,
    MAP_93_GL_DINGPOT,
    MAP_94_CS_INTRO_SPIRAL_7,
    MAP_95_CS_END_ALL_100,
    MAP_96_CS_END_BEACH_1,
    MAP_97_CS_END_BEACH_2,
    MAP_98_CS_END_SPIRAL_MOUNTAIN_1,
    MAP_99_CS_END_SPIRAL_MOUNTAIN_2,
};

int sTick = 0;
int sTicksPerMap = 0;
size_t sNext = 0;
bool sActive = false;

} // namespace

void WarpSweepTick() {
#if defined(LIGHTHOUSE_MOBILE) && !defined(ENABLE_DEBUG_TOOLS) && !defined(LIGHTHOUSE_WARP_SWEEP)
    return;
#endif
    sTick++;
    if (!sActive) {
        if (sTick % kPollTicks != 0) {
            return;
        }
        const std::string request = Ship::Context::GetPathRelativeToAppDirectory("warp-sweep-request");
        std::error_code ec;
        if (!std::filesystem::exists(request, ec)) {
            return;
        }
        int seconds = 0;
        std::ifstream(request) >> seconds;
        std::filesystem::remove(request, ec);
        sTicksPerMap = (seconds > 0 ? seconds : 8) * kTicksPerSecond;
        sNext = 0;
        sActive = true;
        sTick = sTicksPerMap - kTicksPerSecond;
        SPDLOG_INFO("[sweep] start: {} maps, {} s each", std::size(kSweepMaps), sTicksPerMap / kTicksPerSecond);
        return;
    }
    if (sTick % sTicksPerMap != 0) {
        return;
    }
    if (sNext >= std::size(kSweepMaps)) {
        SPDLOG_INFO("[sweep] done");
        sActive = false;
        return;
    }
    const map_e map = kSweepMaps[sNext++];
    SPDLOG_INFO("[sweep] warp {} of {}: map 0x{:X}", sNext, std::size(kSweepMaps), (int)map);
    func_8031D04C(map, 0);
}

} // namespace DevTools
} // namespace Lighthouse
