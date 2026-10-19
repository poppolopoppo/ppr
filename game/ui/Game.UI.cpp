module;

#include "pP/Macros.h"

module game.ui;

import engine.app;
import imgui;
import std;

namespace pP::game::ui {
    // ------------------------------------------------------------------
    // utility window placement
    // ------------------------------------------------------------------

    WindowPlacement fitUtilityWindow(const float2 viewport, const float2 preferred, const float margin) noexcept {
        const float2 available{std::max(1.0f, viewport.x), std::max(1.0f, viewport.y)};
        const float inset = std::clamp(margin, 0.0f, std::min(available.x, available.y) * 0.1f);
        const float2 size{
            std::clamp(preferred.x, 1.0f, std::max(1.0f, available.x - inset * 2.0f)),
            std::clamp(preferred.y, 1.0f, std::max(1.0f, available.y - inset * 2.0f))};

        return {.m_position = (available - size) * 0.5f, .m_size = size};
    }

    void prepareUtilityWindow(const float preferred_width, const float preferred_height, const bool constrain_maximum) {
        const ImGuiViewport *const viewport = ImGui::GetMainViewport();
        const auto placement = fitUtilityWindow(
            float2{viewport->WorkSize.x, viewport->WorkSize.y}, float2{preferred_width, preferred_height},
            ImGui::GetFontSize() * 0.75f);

        ImGui::SetNextWindowPos(
            ImVec2{viewport->WorkPos.x + placement.m_position.x, viewport->WorkPos.y + placement.m_position.y},
            ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2{placement.m_size.x, placement.m_size.y}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2{
                std::min(placement.m_size.x, 16.0f * ImGui::GetFontSize()),
                std::min(placement.m_size.y, 12.0f * ImGui::GetFontSize())},
            constrain_maximum ? ImVec2{placement.m_size.x, placement.m_size.y}
                              : ImVec2{(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()});
    }

    // ------------------------------------------------------------------
    // world progress
    // ------------------------------------------------------------------

    bool worldActionAvailable(const WorldProgressView &view, const EWorldAction action) noexcept {
        if (not view.m_open or view.m_closing) {
            return false;
        }

        switch (action) {
            case EWorldAction::retry:
            case EWorldAction::quit: return view.m_terminal;
            case EWorldAction::cancel:
                return not view.m_terminal and not view.m_published and not view.m_activation_pending;
            case EWorldAction::none: return false;
        }

        return false;
    }

    EWorldAction drawWorldProgress(const WorldProgressView &view) {
        if (not view.m_open) {
            pP::ui::WindowTitleFont title_font{};
            const bool visible = ImGui::BeginPopupModal("Generating terrain", nullptr, ImGuiWindowFlags_NoResize);
            title_font.close();
            if (visible) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            return EWorldAction::none;
        }

        ImGui::OpenPopup("Generating terrain");
        prepareUtilityWindow(ImGui::GetFontSize() * 32.0f, ImGui::GetFontSize() * 18.0f);

        pP::ui::WindowTitleFont title_font{};
        const bool visible = ImGui::BeginPopupModal("Generating terrain", nullptr, ImGuiWindowFlags_NoResize);
        title_font.close();
        if (not visible) {
            return EWorldAction::none;
        }

        const char *label = "Queued";
        switch (view.m_job.m_stage) {
            case colony::EWorldJobStage::queued: break;
            case colony::EWorldJobStage::layout: label = "Layout"; break;
            case colony::EWorldJobStage::terrain: label = "Terrain composition"; break;
            case colony::EWorldJobStage::caves: label = "Caves and features"; break;
            case colony::EWorldJobStage::initial_matter: label = "Initial matter"; break;
            case colony::EWorldJobStage::validation: label = "Final validation"; break;
            case colony::EWorldJobStage::physics: label = "Preparing physics"; break;
            case colony::EWorldJobStage::publishing: label = "Publishing"; break;
            case colony::EWorldJobStage::retiring: label = "Retiring previous session"; break;
            case colony::EWorldJobStage::capturing: label = "Preparing diagnostic preview"; break;
        }

        if (view.m_closing) {
            label = "Closing: draining world work";
        } else if (view.m_job.m_state == colony::EWorldJobState::cancelling) {
            label = "Cancelling...";
        } else if (view.m_terminal) {
            label = "World unavailable";
        } else if (view.m_published) {
            label = "Waiting for world presentation";
        } else if (view.m_job.m_state == colony::EWorldJobState::prepared) {
            label = view.m_activation_pending ? "Activating new session"
                    : view.m_candidate_ready  ? "Terrain ready"
                                              : "Preparing candidate graphics";
        }

        const float reserve = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##body", ImVec2{0.0f, std::max(1.0f, ImGui::GetContentRegionAvail().y - reserve)});
        ImGui::SeparatorText("Preparing your world");
        ImGui::TextWrapped("%s", label);

        if (not view.m_closing and
            view.m_job.m_state == colony::EWorldJobState::prepared and
            not view.m_published and
            view.m_staged_chunks < view.m_total_chunks) {
            ImGui::ProgressBar(
                static_cast<float>(view.m_staged_chunks) / static_cast<float>(view.m_total_chunks), ImVec2{-1.0f, 0.0f},
                "Terrain batches staged");
            ImGui::Text("%u / %u chunks staged", view.m_staged_chunks, view.m_total_chunks);
        } else if (not view.m_closing and
                   view.m_job.m_state == colony::EWorldJobState::generating and
                   view.m_job.m_total != 0u and
                   not view.m_published) {
            ImGui::ProgressBar(
                static_cast<float>(view.m_job.m_completed) / static_cast<float>(view.m_job.m_total),
                ImVec2{-1.0f, 0.0f}, "Current-stage progress");
            ImGui::Text("%u / %u chunks", view.m_job.m_completed, view.m_job.m_total);
        } else {
            const float progress = view.m_terminal
                                       ? 0.0f
                                       : -1.0f - static_cast<float>(std::fmod(view.m_animation_seconds, 1.0));
            ImGui::ProgressBar(progress, ImVec2{-1.0f, 0.0f}, view.m_terminal ? "Preparation stopped" : "Working");
        }

        if (view.m_job.m_error) {
            ImGui::TextWrapped("%s", view.m_job.m_error.message().c_str());
            if (not view.m_diagnostic.empty()) {
                ImGui::PushTextWrapPos();
                ImGui::TextUnformatted(view.m_diagnostic.data(), view.m_diagnostic.data() + view.m_diagnostic.size());
                ImGui::PopTextWrapPos();
            }
        }

        ImGui::EndChild();
        ImGui::Separator();

        EWorldAction action{};
        if (worldActionAvailable(view, EWorldAction::retry)) {
            if (ImGui::Button("Retry")) {
                action = EWorldAction::retry;
            }

            ImGui::SameLine();
            if (ImGui::Button("Quit")) {
                action = EWorldAction::quit;
            }
        } else if (worldActionAvailable(view, EWorldAction::cancel)) {
            if (ImGui::Button("Cancel")) {
                action = EWorldAction::cancel;
            }
        }

        ImGui::EndPopup();
        return action;
    }

    // ------------------------------------------------------------------
    // unsaved authoring edits
    // ------------------------------------------------------------------

    bool unsavedActionAvailable(const UnsavedEditsView &view, const EUnsavedAction action) noexcept {
        if (not view.m_open) {
            return false;
        }

        switch (action) {
            case EUnsavedAction::save: return not view.m_busy and not view.m_saving;
            case EUnsavedAction::discard: return not view.m_saving;
            case EUnsavedAction::cancel: return true;
            case EUnsavedAction::none: return false;
        }

        return false;
    }

    EUnsavedAction drawUnsavedEdits(const UnsavedEditsView &view) {
        if (view.m_open) {
            ImGui::OpenPopup("Unsaved biome edits");
        }

        prepareUtilityWindow(ImGui::GetFontSize() * 36.0f, ImGui::GetFontSize() * 18.0f);

        pP::ui::WindowTitleFont title_font{};
        const bool visible = ImGui::BeginPopupModal("Unsaved biome edits", nullptr, ImGuiWindowFlags_NoResize);
        title_font.close();
        if (not visible) {
            return EUnsavedAction::none;
        }

        if (not view.m_open) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return EUnsavedAction::none;
        }

        const float reserve = 4.0f * ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##body", ImVec2{0.0f, std::max(1.0f, ImGui::GetContentRegionAvail().y - reserve)});
        ImGui::TextWrapped(
            "Save biome edits before continuing? Discard replaces the draft only after the requested operation succeeds.");
        ImGui::TextWrapped("%.*s", static_cast<int>(view.m_status.size()), view.m_status.data());
        ImGui::EndChild();
        ImGui::Separator();

        EUnsavedAction action{};
        ImGui::BeginDisabled(not unsavedActionAvailable(view, EUnsavedAction::save));
        if (ImGui::Button("Save and continue", ImVec2{-1.0f, 0.0f})) {
            action = EUnsavedAction::save;
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(not unsavedActionAvailable(view, EUnsavedAction::discard));
        if (ImGui::Button("Discard and continue", ImVec2{-1.0f, 0.0f})) {
            action = EUnsavedAction::discard;
        }
        ImGui::EndDisabled();

        if (ImGui::Button("Cancel transition", ImVec2{-1.0f, 0.0f})) {
            action = EUnsavedAction::cancel;
        }

        ImGui::EndPopup();
        return action;
    }

    // ------------------------------------------------------------------
    // frontend observations and projected feedback
    // ------------------------------------------------------------------

    FrontendInput frontendInput() noexcept {
        const ImGuiIO &io = ImGui::GetIO();
        const auto *const viewport = ImGui::GetMainViewport();

        return {
            .m_pointer = float2{io.MousePos.x - viewport->Pos.x, io.MousePos.y - viewport->Pos.y},
            .m_mouse_captured = io.WantCaptureMouse,
            .m_keyboard_captured = io.WantCaptureKeyboard,
            .m_primary_pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left)};
    }

    bool isWorldPoint(const float2 point, const float2 client_extent, const ChromeBounds chrome) noexcept {
        return point.x >= chrome.m_left and
               point.x < client_extent.x - chrome.m_right and
               point.y >= chrome.m_top and
               point.y < client_extent.y - chrome.m_bottom;
    }

    void drawFeedback(
        const std::span<const FeedbackRect> rectangles, const float2 client_extent, const ChromeBounds chrome) {
        const hal::ProfileScope scope{"Game.UI.Feedback"};
        const ImGuiIO &io = ImGui::GetIO();
        if (std::abs(io.DisplaySize.x - client_extent.x) > 1.0f or
            std::abs(io.DisplaySize.y - client_extent.y) > 1.0f) {
            return;
        }

        const auto *const viewport = ImGui::GetMainViewport();
        const ImVec2 clip_min{viewport->Pos.x + chrome.m_left, viewport->Pos.y + chrome.m_top};
        const ImVec2 clip_max{
            viewport->Pos.x + client_extent.x - chrome.m_right, viewport->Pos.y + client_extent.y - chrome.m_bottom};
        if (clip_max.x <= clip_min.x or clip_max.y <= clip_min.y) {
            return;
        }

        ImDrawList *const draw = ImGui::GetBackgroundDrawList();
        draw->PushClipRect(clip_min, clip_max, true);
        for (const FeedbackRect &rect: rectangles) {
            const auto color = rect.m_role == EFeedbackRole::selection ? pP::ui::theme::kSelection
                                                                       : pP::ui::theme::kAccent;
            const ImVec2 a{viewport->Pos.x + rect.m_min.x, viewport->Pos.y + rect.m_min.y};
            const ImVec2 b{viewport->Pos.x + rect.m_max.x, viewport->Pos.y + rect.m_max.y};
            const ImU32 packed = ImGui::GetColorU32(color.vec4());
            switch (rect.m_kind) {
                case EFeedbackKind::outline:
                    draw->AddRect(a, b, packed, 0.0f, ImDrawFlags_None, rect.m_thickness);
                    break;
                case EFeedbackKind::fill: draw->AddRectFilled(a, b, packed); break;
                case EFeedbackKind::line: draw->AddLine(a, b, packed, rect.m_thickness); break;
            }
        }

        draw->PopClipRect();
    }
}
