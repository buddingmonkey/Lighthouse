#include "LighthouseMenu.h"
#include "port/build.h"
#include "port/Engine.h"
#include "Notification.h"
#include "LighthouseInputEditorWindow.h"
#include "LighthouseModals.h"
#include "LighthouseModMenuWindow.h"
//#include <soh/GameVersions.h>
#include "port/ResourceHelpers.h"
#include "port/Controller/ControlSchemes.h"
#include "port/Localization/Language.h"
#include "port/Save/SaveConverter.h"
#include "UIWidgets.hpp"
#include <fast/Fast3dWindow.h>
#include <fast/backends/gfx_xr_view.h>
#include <spdlog/fmt/fmt.h>

#include "variables.h"

namespace LighthouseGui {

extern std::shared_ptr<LighthouseMenu> mLighthouseMenu;
extern std::shared_ptr<LighthouseModalWindow> mModalWindow;
using namespace UIWidgets;

static bool HeadsetWindow() {
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    return window != nullptr && window->GetWindowBackend() == Fast::WindowBackend::FAST3D_OPENXR_OPENGL;
}

static std::unordered_map<int32_t, const char*> imguiScaleOptions = {
    { 0, "Small" },
    { 1, "Normal" },
    { 2, "Large" },
    { 3, "X-Large" },
};

static const std::unordered_map<int32_t, const char*> menuThemeOptions = {
    { UIWidgets::Colors::Red, "Red" },
    { UIWidgets::Colors::DarkRed, "Dark Red" },
    { UIWidgets::Colors::Orange, "Orange" },
    { UIWidgets::Colors::Green, "Green" },
    { UIWidgets::Colors::DarkGreen, "Dark Green" },
    { UIWidgets::Colors::LightBlue, "Light Blue" },
    { UIWidgets::Colors::Blue, "Blue" },
    { UIWidgets::Colors::DarkBlue, "Dark Blue" },
    { UIWidgets::Colors::Indigo, "Indigo" },
    { UIWidgets::Colors::Violet, "Violet" },
    { UIWidgets::Colors::Purple, "Purple" },
    { UIWidgets::Colors::Brown, "Brown" },
    { UIWidgets::Colors::Gray, "Gray" },
    { UIWidgets::Colors::DarkGray, "Dark Gray" },
};

static const std::unordered_map<int32_t, const char*> textureFilteringMap = {
    { Fast::FILTER_THREE_POINT, "Three-Point" },
    { Fast::FILTER_LINEAR, "Linear" },
    { Fast::FILTER_NONE, "None" },
};

static const std::unordered_map<int32_t, const char*> notificationPosition = {
    { 0, "Top Left" }, { 1, "Top Right" }, { 2, "Bottom Left" }, { 3, "Bottom Right" }, { 4, "Hidden" },
};

static const std::unordered_map<int32_t, const char*> controlSchemeLabels = {
    { CONTROL_SCHEME_RETRO, "Retro" },
    { CONTROL_SCHEME_MODERN, "Modern" },
    { CONTROL_SCHEME_POCKET, "Pocket" },
};

#ifdef LIGHTHOUSE_TOUCH_CONTROLS
static const std::unordered_map<int32_t, const char*> touchLayoutLabels = {
    { 0, "Automatic" },
    { 1, "Phone" },
    { 2, "Tablet" },
};
#endif

static const std::unordered_map<int32_t, const char*> bootSequenceLabels = {
    { BOOTSEQUENCE_DEFAULT, "Default" },
    { BOOTSEQUENCE_AUTHENTIC, "Authentic" },
    { BOOTSEQUENCE_FILESELECT, "File Select" },
};

static const std::unordered_map<int32_t, const char*> saveConvertSlotLabels = {
    { SaveConverter::kSlotAll, "All games" },
    { 1, "Game 1" },
    { 2, "Game 2" },
    { 3, "Game 3" },
};

static int32_t sAppliedControlScheme = -1;

void LighthouseMenu::AddMenuSettings() {
    // Add Settings Menu
    AddMenuEntry("Settings", CVAR_SETTING("Menu.SettingsSidebarSection"));
    AddSidebarEntry("Settings", "General", 2);
    WidgetPath path = { "Settings", "General", SECTION_COLUMN_1 };

    // General - Settings
    AddWidget(path, "Menu Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Menu Theme", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Menu.Theme"))
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Changes the Theme of the Menu Widgets.")
                     .ComboMap(menuThemeOptions)
                     .DefaultIndex(Colors::LightBlue));
#if not defined(__SWITCH__) and not defined(__WIIU__)
    AddWidget(path, "Menu Controller Navigation", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_IMGUI_CONTROLLER_NAV)
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Allows controller navigation of the port menu (Settings, Enhancements,...)\nCAUTION: "
            "This will disable game inputs while the menu is visible.\n\nD-pad to move between "
            "items, A to select, B to move up in scope."));
    AddWidget(path, "Menu Background Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Menu.BackgroundOpacity"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().DefaultValue(0.85f).IsPercentage().Tooltip(
            "Sets the opacity of the background of the port menu."));

    AddWidget(path, "General Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Cursor Always Visible", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("CursorVisibility"))
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            Ship::Context::GetRawInstance()->GetWindow()->SetForceCursorVisibility(
                CVarGetInteger(CVAR_SETTING("CursorVisibility"), 0));
        })
        .Options(CheckboxOptions().Tooltip("Makes the cursor always visible, even in full screen."));
#endif
    AddWidget(path, "Search In Sidebar", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Menu.SidebarSearch"))
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            if (CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0)) {
                mLighthouseMenu->InsertSidebarSearch();
            } else {
                mLighthouseMenu->RemoveSidebarSearch();
            }
        })
        .Options(CheckboxOptions().Tooltip(
            "Displays the Search menu as a sidebar entry in Settings instead of in the header."));
    AddWidget(path, "Search Input Autofocus", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Menu.SearchAutofocus"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Search input box gets autofocus when visible. Does not affect using other widgets."));
    // ButtonOptions doesn't override Disabled()/DisabledTooltip(), so those return WidgetOptions&
    // and can't be chained into Options(); set them on a named local instead.
    ButtonOptions filesFolderOptions =
        ButtonOptions().Tooltip("Opens the folder that contains the save and mods folders, etc.");
#ifdef __IOS__
    // iOS has no file:// URL handler, so the button can't do anything there.
    filesFolderOptions.Disabled(true).DisabledTooltip(
        "Not available on iOS. Open the Files app and look under On My iPhone / On My iPad > Lighthouse "
        "to reach the same folder.");
#elif defined(__ANDROID__)
    // Android rejects a raw file:// Intent with FileUriExposedException, so the button can't do anything there.
    filesFolderOptions.Disabled(true).DisabledTooltip(
        "Not available on Android. Reach the same folder with a file manager, or over USB under "
        "Android/data > Lighthouse.");
#endif
    AddWidget(path, "Open App Files Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            std::string filesPath = Ship::Context::GetRawInstance()->GetAppDirectoryPath();
            SDL_OpenURL(std::string("file:///" + std::filesystem::absolute(filesPath).string()).c_str());
        })
        .Options(filesFolderOptions);

    AddWidget(path, "Boot", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Boot Sequence", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("BootSequence"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_BOOT_TO_DEBUG_WARP_SCREEN_ON).active)
                info.activeDisables.push_back(DISABLE_FOR_BOOT_TO_DEBUG_WARP_SCREEN_ON);
        })
        .Options(ComboboxOptions()
                     .DefaultIndex(BOOTSEQUENCE_DEFAULT)
                     .LabelPosition(LabelPositions::Far)
                     .ComponentAlignment(ComponentAlignments::Right)
                     .ComboMap(bootSequenceLabels)
                     .Tooltip("Configure what happens when starting or resetting the game.\n\n"
                              "Default: Replace the N64 branding with LUS branding\n"
                              "Authentic: Keep the authentic N64 branding\n"
                              "File Select: Skip to file select menu"));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Save Conversion", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Save Slot", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("SaveConvertSlot"))
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .DefaultIndex(SaveConverter::kSlotAll)
                     .ComboMap(saveConvertSlotLabels)
                     .Tooltip("Which game the Import and Export buttons act on. \"All games\" covers every "
                              "slot."));
    AddWidget(path, "Import Save File", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            int slot = CVarGetInteger(CVAR_SETTING("SaveConvertSlot"), SaveConverter::kSlotAll);
            std::string target = slot == SaveConverter::kSlotAll ? "your saves" : ("Game " + std::to_string(slot));
            LighthouseGui::mModalWindow->RegisterPopup(
                "Import Save",
                "This overwrites " + target +
                    " for the game you're playing with a save imported from another platform. "
                    "It can't be undone.\n\n"
                    "Make sure you're playing the same game or romhack as the save you're importing.",
                "Select Save", "Cancel",
                [slot]() {
                    SaveConverter::PickAndImport(slot, [](SaveConverter::Result r) {
                        if (r.message.empty()) {
                            return; // cancelled
                        }
                        LighthouseGui::mModalWindow->RegisterPopup(r.ok ? "Save Import Complete" : "Save Import Failed",
                                                                   r.message, "OK", "", nullptr, nullptr);
                    });
                },
                nullptr);
        })
        .Options(ButtonOptions().Tooltip(
            "Import a save from another platform into the game you're playing. Works for the base game "
            "and romhacks, using whichever you currently have loaded.\n\n"
            "This overwrites your existing saves. Stop 'n' Swop items aren't carried over."));
    AddWidget(path, "Export Save File", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            int slot = CVarGetInteger(CVAR_SETTING("SaveConvertSlot"), SaveConverter::kSlotAll);
            SaveConverter::PickAndExport(slot, [](SaveConverter::Result r) {
                if (r.message.empty()) {
                    return; // cancelled
                }
                LighthouseGui::mModalWindow->RegisterPopup(r.ok ? "Save Export Complete" : "Save Export Failed",
                                                           r.message, "OK", "", nullptr, nullptr);
            });
        })
        .Options(ButtonOptions().Tooltip(
            "Export the save for the game you're playing to a Banjo: Recompiled file. Use the slot above "
            "to pick a single game, or All for every slot.\n\n"
            "Stop 'n' Swop items aren't carried over."));

    AddWidget(path, "Languages", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Dialog Language", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("DialogLanguage"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            auto opts = std::static_pointer_cast<ComboboxOptions>(info.options);
            opts->comboMap.clear();
            for (const auto& [key, name] : Lighthouse::GetLanguageComboEntries()) {
                opts->comboMap[key] = name;
            }
            int32_t cur = CVarGetInteger(CVAR_SETTING("DialogLanguage"), 0);
            if (!opts->comboMap.empty() && opts->comboMap.find(cur) == opts->comboMap.end()) {
                CVarSetInteger(CVAR_SETTING("DialogLanguage"), opts->comboMap.begin()->first);
            }
            if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_ROMHACK).active)
                info.activeDisables.push_back(DISABLE_FOR_ROMHACK);
            else if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_SINGLE_LANGUAGE).active)
                info.activeDisables.push_back(DISABLE_FOR_SINGLE_LANGUAGE);
            if (mLighthouseMenu->disabledMap.at(DISABLE_DURING_PARADE).active)
                info.activeDisables.push_back(DISABLE_DURING_PARADE);
        })
        .Callback([](WidgetInfo& info) {
            int32_t key = CVarGetInteger(CVAR_SETTING("DialogLanguage"), 0);
            for (const auto& name : Lighthouse::GetAvailableLanguageNames()) {
                if (Lighthouse::LanguageKey(name) == key) {
                    Lighthouse::SetActiveLanguage(name);
                    break;
                }
            }
        })
        .Options(ComboboxOptions()
                     .Tooltip("Select the in-game dialog language. Add more languages with the "
                              "\"Add Language Pack from ROM\" button.")
                     .LabelPosition(LabelPositions::Far)
                     .ComponentAlignment(ComponentAlignments::Right));
    AddWidget(path, "Add Language Pack from ROM", WIDGET_BUTTON)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_ROMHACK).active)
                info.activeDisables.push_back(DISABLE_FOR_ROMHACK);
        })
        .Callback([](WidgetInfo& info) {
            LighthouseGui::mModalWindow->RegisterPopup(
                "Add Language Pack from ROM", "Select any Banjo-Kazooie ROM whose language you want to add.\n",
                "Select ROM", "Cancel", []() { RequestInlineLanguagePackExtraction(); }, nullptr);
        })
        .Options(ButtonOptions()
                     .Size(Sizes::Inline)
                     .Tooltip("Pick a Banjo-Kazooie ROM and extract only its dialog into a slim language pack."));

    // Accessibility Options
    AddWidget(path, "Accessibility", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Disable Screen Shake", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("A11yDisableScreenShake"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Stops the camera shaking on impacts such as Beak Buster landings, boulders and boss attacks. "
            "Controller rumble is unaffected."));

    // Experimental Options
    AddWidget(path, "EXPERIMENTAL", WIDGET_SEPARATOR_TEXT).Options(TextOptions().Color(Colors::Orange));
    AddWidget(path, "ImGui Menu Scaling", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("ImGuiScale"))
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .ComboMap(imguiScaleOptions)
                     .Tooltip("Changes the scaling of the ImGui menu elements.")
                     .DefaultIndex(DefaultImGuiScaleIndex())
                     .ComponentAlignment(ComponentAlignments::Right)
                     .LabelPosition(LabelPositions::Far));
    //.Callback([](WidgetInfo& info) { GameEngine::Instance->ScaleImGui(); });

    // Audio Settings
    path.sidebarName = "Audio";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Audio", 3);

    AddWidget(path, "Master Volume", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_SETTING("Volume.Master"))
        .RaceDisable(false)
        .Options(IntSliderOptions().Min(0).Max(100).DefaultValue(40).ShowButtons(true).Format("%d%%"));
    // AddWidget(path, "Main Music Volume: %d %%", WIDGET_CVAR_SLIDER_INT)
    //     .CVar(CVAR_SETTING("Volume.MainMusic"))
    //     .RaceDisable(false)
    //     .Options(IntSliderOptions().Min(0).Max(100).DefaultValue(100).ShowButtons(true).Format(""));
    //.Callback([](WidgetInfo& info) {
    //     Audio_SetGameVolume(SEQ_PLAYER_BGM_MAIN,
    //                         ((float)CVarGetInteger(CVAR_SETTING("Volume.MainMusic"), 100) / 100.0f));
    // });
    // AddWidget(path, "Sub Music Volume: %d %%", WIDGET_CVAR_SLIDER_INT)
    //     .CVar(CVAR_SETTING("Volume.SubMusic"))
    //     .RaceDisable(false)
    //     .Options(IntSliderOptions().Min(0).Max(100).DefaultValue(100).ShowButtons(true).Format(""));
    //.Callback([](WidgetInfo& info) {
    //     Audio_SetGameVolume(SEQ_PLAYER_BGM_SUB,
    //                         ((float)CVarGetInteger(CVAR_SETTING("Volume.SubMusic"), 100) / 100.0f));
    // });
    // AddWidget(path, "Fanfare Volume: %d %%", WIDGET_CVAR_SLIDER_INT)
    //     .CVar(CVAR_SETTING("Volume.Fanfare"))
    //     .RaceDisable(false)
    //     .Options(IntSliderOptions().Min(0).Max(100).DefaultValue(100).ShowButtons(true).Format(""));
    //.Callback([](WidgetInfo& info) {
    //     Audio_SetGameVolume(SEQ_PLAYER_FANFARE,
    //                         ((float)CVarGetInteger(CVAR_SETTING("Volume.Fanfare"), 100) / 100.0f));
    // });
    // AddWidget(path, "Sound Effects Volume: %d %%", WIDGET_CVAR_SLIDER_INT)
    //     .CVar(CVAR_SETTING("Volume.SFX"))
    //     .RaceDisable(false)
    //     .Options(IntSliderOptions().Min(0).Max(100).DefaultValue(100).ShowButtons(true).Format(""));
    //.Callback([](WidgetInfo& info) {
    //     Audio_SetGameVolume(SEQ_PLAYER_SFX, ((float)CVarGetInteger(CVAR_SETTING("Volume.SFX"), 100) / 100.0f));
    // });
    AddWidget(path, "Audio API (Needs reload)", WIDGET_AUDIO_BACKEND).RaceDisable(false);

    // Graphics Settings
    static int32_t maxFps = 360;
    const char* tooltip = "Uses Matrix Interpolation to create extra frames, resulting in smoother graphics. This is "
                          "purely visual and does not impact game logic, execution of glitches etc.\n\nA higher target "
                          "FPS than your monitor's refresh rate will waste resources, and might give a worse result.";
    path.sidebarName = "Graphics";
    AddSidebarEntry("Settings", "Graphics", 2);
    AddWidget(path, "Graphics Options", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Fullscreen", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) { Ship::Context::GetRawInstance()->GetWindow()->ToggleFullscreen(); })
        .Options(ButtonOptions().Tooltip("Toggles Fullscreen On/Off."));
    AddWidget(path, "Internal Resolution", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_INTERNAL_RESOLUTION)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            Ship::Context::GetRawInstance()->GetWindow()->SetResolutionMultiplier(
                CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1));
        })
        .PreFunc([](WidgetInfo& info) {
            if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_ADVANCED_RESOLUTION_ON).active &&
                mLighthouseMenu->disabledMap.at(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_ADVANCED_RESOLUTION_ON);
                info.activeDisables.push_back(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON);
            } else if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_LOW_RES_MODE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_LOW_RES_MODE_ON);
            }
        })
        .Options(
            FloatSliderOptions()
                .Tooltip("Multiplies your output resolution by the value inputted, as a more intensive but effective "
                         "form of anti-aliasing.")
                .ShowButtons(false)
                .IsPercentage()
                .Min(0.5f)
                .Max(2.0f));
#ifndef __WIIU__
    AddWidget(path, "Anti-aliasing (MSAA)", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_MSAA_VALUE)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            Ship::Context::GetRawInstance()->GetWindow()->SetMsaaLevel(CVarGetInteger(CVAR_MSAA_VALUE, 1));
        })
        .Options(
            IntSliderOptions()
                .Tooltip("Activates MSAA (multi-sample anti-aliasing) from 2x up to 8x, to smooth the edges of "
                         "rendered geometry.\n"
                         "Higher sample count will result in smoother edges on models, but may reduce performance.")
                .Min(1)
                .Max(8)
                .DefaultValue(1));
#endif
    auto fps = CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 30);
    const char* fpsFormat = fps == 30 ? "Original (%d)" : "%d";
    AddWidget(path, "Current FPS", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_SETTING("InterpolationFPS"))
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            auto options = std::static_pointer_cast<IntSliderOptions>(info.options);
            int32_t defaultValue = options->defaultValue;
            if (CVarGetInteger(info.cVar, defaultValue) == defaultValue) {
                options->format = "Original (%d)";
            } else {
                options->format = "%d";
            }
        })
        .PreFunc([](WidgetInfo& info) {
            if (mLighthouseMenu->disabledMap.at(DISABLE_FOR_MATCH_REFRESH_RATE_ON).active)
                info.activeDisables.push_back(DISABLE_FOR_MATCH_REFRESH_RATE_ON);
        })
        .Options(IntSliderOptions().Tooltip(tooltip).Min(30).Max(maxFps).DefaultValue(30).Format(fpsFormat));
    AddWidget(path, "Match Refresh Rate", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("MatchRefreshRate"))
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Matches interpolation value to the refresh rate of your display.")
                     .DefaultValue(HeadsetWindow()));
#ifdef ENABLE_OPENXR
    AddWidget(path, "Window Range", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("XrWindowRange"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(FloatSliderOptions()
                     .Tooltip("How far away the window hangs. The window keeps the size it has, so a further range "
                              "makes it small in the eye and takes it out into the room. The depth of the world "
                              "behind it does not change with the range.\n\nThe move bar under the window sets the "
                              "same range, so this reads back what a pull towards you left.")
                     .Min(0.5f)
                     .Max(4.0f)
                     .DefaultValue(1.3f)
                     .Format("%.2f m"));
    AddWidget(path, "Window Size", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("XrWindowScale"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(FloatSliderOptions()
                     .Tooltip("How large the glass is, against the width the game's field of view gives it at the "
                              "nearest range. The range does not touch it, so a large screen far away asks for both: "
                              "move the window out, then make it larger.\n\nThe corner handles set the same size.")
                     .Min(0.5f)
                     .Max(8.0f)
                     .DefaultValue(2.6f)
                     .Format("%.2f"));
    AddWidget(path, "Diorama Depth", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("XrDioramaDepth"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(FloatSliderOptions()
                     .Tooltip("How deep the world reaches behind the glass. The farthest thing the game draws sits "
                              "this far behind the window, and everything nearer sorts itself in between, whatever "
                              "the range and size of the window.\n\nA small depth keeps the whole world near the "
                              "glass, which is the easiest to look at for a long session. No depth can make the "
                              "eyes diverge.")
                     .Min(0.5f)
                     .Max(4.0f)
                     .DefaultValue(2.0f)
                     .Format("%.2f m"));
    AddWidget(path, "Edge Float", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("XrEdgeFloat"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(FloatSliderOptions()
                     .Tooltip("How far the side edges of the window come towards you, against the space between your "
                              "eyes. At a side edge one eye sees a little of the world the other cannot, as it does "
                              "at any window. Floating the edge forward makes that sliver read as something behind a "
                              "near frame instead of as one eye covered.\n\nEach eye gives up this much width at one "
                              "side, so a large value takes a visible strip off the picture. Zero turns it off.")
                     .Min(0.0f)
                     .Max(1.0f)
                     .DefaultValue(0.15f)
                     .Format("%.2f"));
    AddWidget(path, "Edge Softness", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("XrEdgeSoftness"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(FloatSliderOptions()
                     .Tooltip("How far the picture fades out at the edge of the window. It softens the frame but it "
                              "does not change what each eye sees through it, so use Edge Float for that.")
                     .Min(0.0f)
                     .Max(3.0f)
                     .DefaultValue(0.36f)
                     .Format("%.2f"));
    AddWidget(path, "Max Refresh Rate", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_SETTING("XrMaxRate"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(IntSliderOptions()
                     .Tooltip("The fastest the headset is asked to run. The game draws a whole number of frames a "
                              "tick, so it can only use a rate its 30 Hz logic divides into, and it takes the "
                              "fastest one the headset offers. Hold it down for battery or for heat.\n\nA rate the "
                              "device cannot keep runs the game slowly rather than dropping frames, so if the game "
                              "ticks under 30 a second, set this lower.")
                     .Min(60)
                     .Max(120)
                     .DefaultValue(120)
                     .Format("%d Hz"));
    AddWidget(path, "Recenter Window", WIDGET_BUTTON)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Callback([](WidgetInfo&) { Fast::RecenterXrWindow(); })
        .Options(
            ButtonOptions().Size(Sizes::Inline).Tooltip("Puts the window in front of you, where you are looking now."));
    AddWidget(path, "Stereo", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("XrStereo"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !HeadsetWindow(); })
        .Options(CheckboxOptions()
                     .Tooltip("Draws the frame once per eye so the world has depth. Turn it off to draw one image "
                              "for both eyes, which costs half as much and sends one layer to the compositor.")
                     .DefaultValue(true));
#endif
    AddWidget(path, "Renderer API (Needs reload)", WIDGET_VIDEO_BACKEND).RaceDisable(false);
    AddWidget(path, "Enable Vsync", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_VSYNC_ENABLED)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { info.isHidden = mLighthouseMenu->disabledMap.at(DISABLE_FOR_NO_VSYNC).active; })
        .Options(CheckboxOptions()
                     .Tooltip("Removes tearing, but clamps your max FPS to your displays refresh rate.")
                     .DefaultValue(true));
    AddWidget(path, "Windowed Fullscreen", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SDL_WINDOWED_FULLSCREEN)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mLighthouseMenu->disabledMap.at(DISABLE_FOR_NO_WINDOWED_FULLSCREEN).active;
        })
        .Options(CheckboxOptions().Tooltip("Enables Windowed Fullscreen Mode."));
    AddWidget(path, "Allow multi-windows", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENABLE_MULTI_VIEWPORTS)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mLighthouseMenu->disabledMap.at(DISABLE_FOR_NO_MULTI_VIEWPORT).active;
        })
        .Options(CheckboxOptions()
                     .Tooltip("Allows multiple windows to be opened at once. Requires a reload to take effect.")
                     .DefaultValue(true));
    AddWidget(path, "Texture Filter (Needs reload)", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_TEXTURE_FILTER)
        .RaceDisable(false)
        .Options(ComboboxOptions().Tooltip("Sets the applied Texture Filtering.").ComboMap(textureFilteringMap));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Advanced Graphics Options", WIDGET_SEPARATOR_TEXT);

    // Controls
    path.sidebarName = "Controls";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Controls", 2);

    AddWidget(path, "Controller Bindings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Clear Devices", WIDGET_BUTTON)
        .Callback([](WidgetInfo& info) {
            LighthouseGui::mModalWindow->RegisterPopup(
                "Clear Config",
                "This will completely erase the controls config, including registered devices.\nContinue?", "Clear",
                "Cancel",
                []() {
                    Ship::Context::GetRawInstance()->GetConsoleVariables()->ClearBlock(CVAR_PREFIX_SETTING
                                                                                       ".Controllers");
                    uint8_t bits = 0;
                    Ship::Context::GetRawInstance()->GetControlDeck()->Init(&bits);
                },
                nullptr);
        })
        .Options(ButtonOptions().Size(Sizes::Inline));
    AddWidget(path, "Popout Bindings Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ControllerConfiguration"))
        .RaceDisable(false)
        .WindowName("Configure Controller")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Bindings Window."));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Additional Control Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Control Scheme", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Controls.Scheme"))
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            if (sAppliedControlScheme < 0) {
                sAppliedControlScheme = CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_RETRO);
            }
        })
        .Callback([](WidgetInfo& info) {
            int32_t selected = CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_RETRO);
            if (selected == sAppliedControlScheme) {
                return;
            }
            LighthouseGui::mModalWindow->RegisterPopup(
                "Apply Control Scheme", "This overwrites your gamepad bindings with the selected preset.\nContinue?",
                "Apply", "Cancel",
                [selected]() {
                    ControlSchemes_Apply(selected);
                    sAppliedControlScheme = selected;
                },
                []() { CVarSetInteger(CVAR_SETTING("Controls.Scheme"), sAppliedControlScheme); });
        })
        .Options(ComboboxOptions()
                     .Tooltip("Applies a preset gamepad layout (asks to confirm first).\n"
                              "Retro: Traditional N64 controls.\n"
                              "Modern: Xbox Live Arcade controls.\n"
                              "Pocket: D-Pad friendly controls.\n\n"
                              "Applying a scheme overwrites the gamepad bindings; you can still customize "
                              "them afterwards in the bindings window.")
                     .ComboMap(controlSchemeLabels)
                     .DefaultIndex(CONTROL_SCHEME_RETRO));
    AddWidget(path, "Toggle Talon Trot", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Controls.TalonTrotToggle"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Talon Trot stays active without holding crouch. Enter the trot as usual, then release "
            "crouch and it sticks; tap crouch again to stop.\nWhen off, Talon Trot lasts only while "
            "you hold crouch."));
    AddWidget(path, "Pocket: Hold to Tip-toe", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Controls.PocketTipToeHold"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Pocket scheme only. The R2 button slows movement to a tip-toe walk.\n"
                                           "Off: tap R2 to toggle tip-toe on/off.\nOn: tip-toe only "
                                           "while R2 is held."));

#ifdef LIGHTHOUSE_TOUCH_CONTROLS
    AddWidget(path, "On-Screen Controls", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Show On-Screen Controls", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("TouchControls.Enabled"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip("Draws a virtual N64 controller over the game."));
    AddWidget(path, "Hide With Gamepad", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("TouchControls.HideWithGamepad"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Hides the on-screen controls while a physical controller is connected."));
    AddWidget(path, "Show D-Pad", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("TouchControls.ShowDPad"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false));
    AddWidget(path, "Left-Handed Layout", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("TouchControls.Mirror"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Swaps the two halves of the pad, putting the stick under your right thumb."));
    AddWidget(path, "Layout", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("TouchControls.Layout"))
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Phone puts the shoulder buttons along the top edge, under the index fingers.\n"
                              "Tablet moves them down the left and right edges, where a hand holding a "
                              "larger screen can still reach them.\n\n"
                              "Automatic picks from the screen size.")
                     .ComboMap(touchLayoutLabels)
                     .DefaultIndex(0));
    AddWidget(path, "Control Size", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("TouchControls.Scale"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.7f).Max(1.4f).DefaultValue(1.0f).Format("%.2f").Tooltip(
            "Controls are sized in real-world units, so they stay the same size on your thumb "
            "whatever screen you play on.\n\nOn a short screen this stops going up before the slider "
            "does, because the buttons have run out of room."));
    AddWidget(path, "Control Reach", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("TouchControls.Reach"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.8f).Max(1.25f).DefaultValue(1.0f).Format("%.2f").Tooltip(
            "How far from the corners you grip the controls sit. Turn it down for smaller hands; "
            "the buttons keep their size and spacing either way."));
    AddWidget(path, "Control Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("TouchControls.Opacity"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.05f).Max(1.0f).DefaultValue(0.4f).Format("%.2f"));
    AddWidget(path, "Edge Margin", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("TouchControls.EdgeMargin"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.0f).Max(10.0f).DefaultValue(3.0f).Format("%.1f mm").Tooltip(
            "Clearance between the controls and the screen edge, for the cutout and the "
            "gesture bar."));
    AddWidget(path, "Touch Stick Deadzone", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("TouchControls.Deadzone"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.0f).Max(0.5f).DefaultValue(0.12f).Format("%.2f"));
#endif

    // Input Viewer
    path.sidebarName = "Input Viewer";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", path.sidebarName, 2);
    AddWidget(path, "Input Viewer", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Input Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("InputViewer"))
        .RaceDisable(false)
        .WindowName("Input Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Input Viewer.").EmbedWindow(false));

    AddWidget(path, "Input Viewer Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Popout Input Viewer Settings", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("InputViewerSettings"))
        .RaceDisable(false)
        .WindowName("Input Viewer Settings")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Input Viewer Settings Window."));

    // Notifications
    path.sidebarName = "Notifications";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", path.sidebarName, 3);
    AddWidget(path, "Position", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Notifications.Position"))
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Which corner of the screen notifications appear in.")
                     .ComboMap(notificationPosition)
                     .DefaultIndex(3));
    AddWidget(path, "Duration (seconds):", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Notifications.Duration"))
        .RaceDisable(false)
        .Options(FloatSliderOptions()
                     .Tooltip("How long notifications are displayed for.")
                     .Format("%.1f")
                     .Step(0.1f)
                     .Min(3.0f)
                     .Max(30.0f)
                     .DefaultValue(10.0f));
    AddWidget(path, "Background Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Notifications.BgOpacity"))
        .RaceDisable(false)
        .Options(FloatSliderOptions()
                     .Tooltip("How opaque the background of notifications is.")
                     .DefaultValue(0.5f)
                     .IsPercentage());
    AddWidget(path, "Size:", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Notifications.Size"))
        .RaceDisable(false)
        .Options(FloatSliderOptions()
                     .Tooltip("How large notifications are.")
                     .Format("%.1f")
                     .Step(0.1f)
                     .Min(1.0f)
                     .Max(5.0f)
                     .DefaultValue(1.8f));
    AddWidget(path, "Test Notification", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            Notification::Emit({
                .itemIcon = "__OTR__textures/icon_item_24_static/gQuestIconGoldSkulltulaTex",
                .prefix = "This",
                .message = "is a",
                .suffix = "test.",
            });
        })
        .Options(ButtonOptions().Tooltip("Displays a test notification."));

    // Romhack Menu
    path.sidebarName = "Romhack Menu";
    AddSidebarEntry("Settings", path.sidebarName, 1);

    AddWidget(path, "Popout Romhack Menu Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("RomhackMenu"))
        .WindowName("Romhack Menu")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Romhack Menu Window."));

    // Mod Menu
    path.sidebarName = "Mod Menu";
    AddSidebarEntry("Settings", path.sidebarName, 1);

    AddWidget(path, "Popout Mod Menu Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ModMenu"))
        .WindowName("Mod Menu")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Mod Menu Window."));
}

} // namespace LighthouseGui
