#include "LighthouseModals.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <vector>
#include <string>
#include "UIWidgets.hpp"
#include "LighthouseGui.hpp"
#include "LighthouseModMenuWindow.h"

struct LighthouseModal {
    std::string title_;
    std::string message_;
    std::string button1_;
    std::string button2_;
    std::function<void()> button1callback_;
    std::function<void()> button2callback_;
};
std::vector<LighthouseModal> modals;

bool closePopup = false;

void LighthouseModalWindow::Draw() {
    // TEMP DIAGNOSTIC (iOS black-screen investigation) -- remove once resolved.
    static int sDrawCalls = 0;
    if (sDrawCalls < 3 || sDrawCalls % 300 == 0) {
        SPDLOG_INFO("[modal] Draw #{} visible={} queued={}", sDrawCalls, IsVisible(), modals.size());
    }
    sDrawCalls++;

    if (!IsVisible()) {
        return;
    }
    DrawElement();
    // Sync up the IsVisible flag if it was changed by ImGui
    SyncVisibilityConsoleVariable();
}

void LighthouseModalWindow::DrawElement() {
    DrawInlineModExtraction();

    if (modals.size() > 0) {
        LighthouseModal curModal = modals.at(0);
        if (!ImGui::IsPopupOpen(curModal.title_.c_str())) {
            ImVec2 center = ImGui::GetMainViewport()->GetWorkCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup(curModal.title_.c_str());
        }
        if (closePopup) {
            ImGui::CloseCurrentPopup();
            modals.erase(modals.begin());
            closePopup = false;
        }
        const bool popupBegan = ImGui::BeginPopupModal(curModal.title_.c_str(), NULL,
                                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
                                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                                           ImGuiWindowFlags_NoSavedSettings);
        // TEMP DIAGNOSTIC (iOS black-screen investigation) -- remove once resolved.
        static int sBeginCalls = 0;
        if (sBeginCalls < 3 || sBeginCalls % 300 == 0) {
            SPDLOG_INFO("[modal] '{}' began={} isOpen={}", curModal.title_, popupBegan,
                        ImGui::IsPopupOpen(curModal.title_.c_str()));
        }
        sBeginCalls++;

        if (popupBegan) {
            // Show the nav highlight on the default button (button1) the moment the popup opens, so a
            // gamepad/keyboard user sees the selection immediately rather than after a first input.
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetNavCursorVisible(true);
            }
            ImGui::Text("%s", curModal.message_.c_str());
            UIWidgets::PushStyleButton(THEME_COLOR);
            if (ImGui::Button(curModal.button1_.c_str())) {
                if (curModal.button1callback_ != nullptr) {
                    curModal.button1callback_();
                }
                ImGui::CloseCurrentPopup();
                modals.erase(modals.begin());
            }
            ImGui::SetItemDefaultFocus();
            UIWidgets::PopStyleButton();
            if (curModal.button2_ != "") {
                ImGui::SameLine();
                UIWidgets::PushStyleButton(THEME_COLOR);
                if (ImGui::Button(curModal.button2_.c_str())) {
                    if (curModal.button2callback_ != nullptr) {
                        curModal.button2callback_();
                    }
                    ImGui::CloseCurrentPopup();
                    modals.erase(modals.begin());
                }
                UIWidgets::PopStyleButton();
            }
            // EndPopup() is only valid when BeginPopupModal() returned true. Calling it
            // unconditionally corrupts ImGui's window stack on any frame where the popup is
            // not open -- the closePopup path below reaches exactly that state.
            ImGui::EndPopup();
        }
    }
}

void LighthouseModalWindow::RegisterPopup(std::string title, std::string message, std::string button1,
                                          std::string button2, std::function<void()> button1callback,
                                          std::function<void()> button2callback) {
    modals.push_back({ title, message, button1, button2, button1callback, button2callback });
}

bool LighthouseModalWindow::IsPopupOpen(std::string title) {
    return !modals.empty() && modals.at(0).title_ == title;
}

size_t LighthouseModalWindow::PopupsQueued() {
    return modals.size();
}

void LighthouseModalWindow::DismissPopup() {
    closePopup = true;
}
