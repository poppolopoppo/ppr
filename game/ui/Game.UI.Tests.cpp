module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.app;
import engine.math;
import engine.asset;
import game.colony.content;
import game.colony.worldgen_panel;
import engine.sim;
import game.ui;
import game.ui.model;
import game.colony.driver;
import game.colony.agents;
import game.colony.panel;
import imgui;
import imgui_internal;
import std;

namespace pP::game::tests::detail::presentation {
    // ------------------------------------------------------------------
    // viewport reachability
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (utility_windows_fit_normal_narrow_and_degenerate_viewports) {
        for (const auto viewport: {float2{1280.0f, 720.0f}, float2{480.0f, 320.0f}, float2{1.0f, 1.0f}}) {
            const auto placement = ui::fitUtilityWindow(viewport, float2{1100.0f, 820.0f}, 12.0f);

            PPR_TEST_ASSERT(placement.m_position.x >= 0.0f and placement.m_position.y >= 0.0f);
            PPR_TEST_ASSERT(placement.m_size.x > 0.0f and placement.m_size.y > 0.0f);
            PPR_TEST_ASSERT(placement.m_position.x + placement.m_size.x <= viewport.x);
            PPR_TEST_ASSERT(placement.m_position.y + placement.m_size.y <= viewport.y);
        }

        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{2000.0f, 1200.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame();
            ui::prepareUtilityWindow(1100.0f, 820.0f, false);
            if (frame == 2) {
                ImGui::SetNextWindowSize(ImVec2{1600.0f, 1000.0f});
            }

            ImGui::Begin("Resizable workspace");
            if (frame == 2) {
                const auto size = ImGui::GetWindowSize();
                PPR_TEST_ASSERT(size.x == 1600.0f and size.y == 1000.0f);
            }

            ImGui::End();
            ImGui::Render();
        }
    };

    // ------------------------------------------------------------------
    // display confirmation lifetime
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (resolved_display_trial_releases_modal_when_parent_is_closed) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ImGui::NewFrame();
        std::ignore = pP::ui::drawDisplayConfirmation(true, true, false, true, 10.0f, false, {});
        ImGui::Render();
        PPR_TEST_ASSERT(ImGui::IsPopupOpen("Confirm display change"));

        ImGui::NewFrame();
        std::ignore = pP::ui::drawDisplayConfirmation(true, true, false, true, 10.0f, false, {});
        ImGui::Render();
        PPR_TEST_ASSERT(io.WantCaptureKeyboard);

        ImGui::NewFrame();
        std::ignore = pP::ui::drawDisplayConfirmation(false, false, false, false, 0.0f, false, {});
        ImGui::Render();
        PPR_TEST_ASSERT(not ImGui::IsPopupOpen("Confirm display change"));

        ImGui::NewFrame();
        std::ignore = pP::ui::drawDisplayConfirmation(false, false, false, false, 0.0f, false, {});
        ImGui::Render();
        PPR_TEST_ASSERT(not io.WantCaptureKeyboard);
    };

    PPR_UNIT_TEST (colony_chrome_preserves_a_world_region_at_normal_and_narrow_sizes) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        colony::ColonyPresentation snapshot{.m_speed = 1u};
        const colony::ColonyUiView view{.m_colony = &snapshot};
        colony::ColonyUiState state{};
        Array<colony::AgentSummary> crowded(64u);
        for (std::size_t index = 0u; index < crowded.size(); ++index) {
            crowded[index].m_key = index + 1u;
        }

        for (const auto extent: {ImVec2{1280.0f, 720.0f}, ImVec2{640.0f, 480.0f}}) {
            io.DisplaySize = extent;
            snapshot.m_agents = {};
            colony::ColonyUiFrame frame{};
            for (int settled = 0; settled < 3; ++settled) {
                ImGui::NewFrame();
                frame = colony::drawColonyUi(view, {}, {}, state);
                ImGui::Render();
            }

            PPR_TEST_ASSERT(frame.m_chrome_right == 0.0f and frame.m_chrome_left == 0.0f);
            PPR_TEST_ASSERT(frame.m_chrome_bottom == 0.0f);
            const ui::ChromeBounds chrome{
                .m_top = frame.m_chrome_top,
                .m_bottom = frame.m_chrome_bottom,
                .m_right = frame.m_chrome_right,
                .m_left = frame.m_chrome_left};
            const float2 client{extent.x, extent.y};
            PPR_TEST_ASSERT(not ui::isWorldPoint(float2{8.0f, frame.m_chrome_top * 0.5f}, client, chrome));
            PPR_TEST_ASSERT(
                ui::isWorldPoint(float2{frame.m_chrome_left + 1.0f, frame.m_chrome_top + 1.0f}, client, chrome));
            PPR_TEST_ASSERT(frame.m_chrome_top + frame.m_chrome_bottom < extent.y * 0.5f);
            PPR_TEST_ASSERT(not frame.m_intent);
            PPR_TEST_ASSERT(state.m_colony_open and not state.m_performance_open);

            ImGuiWindow *const summary = ImGui::FindWindowByName("Colony###colony_summary");
            PPR_TEST_ASSERT(summary != nullptr);
            const float available_height = extent.y - frame.m_chrome_top - frame.m_chrome_bottom;
            PPR_TEST_ASSERT(summary->Size.y <= available_height);
            const ImGuiWindow *const rail = ImGui::FindWindowByName("##tool_rail");
            PPR_TEST_ASSERT(rail != nullptr and rail->Size.y < available_height * 0.6f);
            PPR_TEST_ASSERT(ImGui::FindWindowByName("##side") == nullptr);
            PPR_TEST_ASSERT(ImGui::FindWindowByName("##status") == nullptr);
            PPR_TEST_ASSERT(ui::isWorldPoint(float2{8.0f, extent.y - 8.0f}, client, chrome));
            PPR_TEST_ASSERT(ui::isWorldPoint(float2{extent.x - 8.0f, extent.y * 0.5f}, client, chrome));
            const float2 summary_point{
                summary->Pos.x + summary->Size.x * 0.5f, summary->Pos.y + summary->Size.y * 0.5f};
            PPR_TEST_ASSERT(ui::isWorldPoint(summary_point, client, chrome));

            io.AddMousePosEvent(summary_point.x, summary_point.y);
            ImGui::NewFrame();
            std::ignore = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();
            PPR_TEST_ASSERT(io.WantCaptureMouse);

            snapshot.m_agents = crowded;
            for (int settled = 0; settled < 3; ++settled) {
                ImGui::NewFrame();
                std::ignore = colony::drawColonyUi(view, {}, {}, state);
                ImGui::Render();
            }

            PPR_TEST_ASSERT(summary->Size.y <= available_height);
            PPR_TEST_ASSERT(summary->Pos.y + summary->Size.y <= extent.y - frame.m_chrome_bottom);
            PPR_TEST_ASSERT(summary->ScrollMax.y > 0.0f and summary->ScrollbarY);

            io.AddMousePosEvent(summary->Pos.x + summary->Size.x * 0.5f, summary->Pos.y + summary->Size.y * 0.5f);
            ImGui::NewFrame();
            std::ignore = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();

            io.AddMouseWheelEvent(0.0f, -1.0f);
            ImGui::NewFrame();
            std::ignore = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();
            PPR_TEST_ASSERT(summary->Scroll.y > 0.0f and io.WantCaptureMouse);
        }
    };

    PPR_UNIT_TEST (colony_and_performance_visibility_are_independent) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        colony::ColonyPresentation snapshot{.m_speed = 1u};
        pP::ui::PerformanceWidget performance;
        performance.observe(16.0f, 4.0f);
        performance.observeGpu(1u, 4.0f);
        const colony::ColonyUiView view{.m_colony = &snapshot, .m_performance = &performance};
        colony::ColonyUiState state{};
        const auto frame = [&]() {
            ImGui::NewFrame();
            const auto result = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();
            return result;
        };

        std::ignore = frame();
        std::ignore = frame();

        const auto active = [](const char *const name) {
            const auto *const window = ImGui::FindWindowByName(name);
            return window != nullptr and window->Active;
        };

        PPR_TEST_ASSERT(active("Colony###colony_summary") and not active("Performance"));

        state.m_performance_open = true;
        std::ignore = frame();
        std::ignore = frame();
        std::ignore = frame();
        PPR_TEST_ASSERT(active("Colony###colony_summary") and active("Performance"));
        PPR_TEST_ASSERT(ImGui::FindWindowByName("##performance_widget") == nullptr);
        auto *const perf_window = ImGui::FindWindowByName("Performance");
        PPR_TEST_ASSERT(performance.gpuSampleCount() == 1u);
        // The three histories and the four dense sections are always drawn:
        // narrowing the window below its content scrolls instead of hiding
        // the default read-out.
        ImGui::SetWindowSize(perf_window, ImVec2{320.0f, 240.0f});
        std::ignore = frame();
        PPR_TEST_ASSERT(perf_window->Size.x == 320.0f and perf_window->ScrollMax.y > 0.0f);

        state.m_colony_open = false;
        std::ignore = frame();
        PPR_TEST_ASSERT(not active("Colony###colony_summary") and active("Performance"));

        state.m_tool = colony::EColonyTool::dig;
        std::ignore = frame();
        PPR_TEST_ASSERT(active("Tool context###colony_summary") and active("Performance"));
        PPR_TEST_ASSERT(not state.m_colony_open);

        state.m_tool = colony::EColonyTool::inspect;
        state.m_colony_open = true;
        state.m_performance_open = false;
        PPR_TEST_ASSERT(not frame().m_intent);
        PPR_TEST_ASSERT(active("Colony###colony_summary") and not active("Performance"));
        PPR_TEST_ASSERT(performance.sampleCount() == 1u and performance.gpuSampleCount() == 1u);
    };

    PPR_UNIT_TEST (inspector_is_a_passive_pointer_readout_only_in_inspect_mode) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        colony::ColonyPresentation snapshot{.m_speed = 1u, .m_has_inspected = true};
        colony::ColonyUiView view{.m_colony = &snapshot};
        colony::ColonyUiState state{};
        const auto frame = [&]() {
            ImGui::NewFrame();
            std::ignore = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();
        };

        // Any tool but Inspect keeps the cursor clear of the read-out.
        state.m_tool = colony::EColonyTool::dig;
        view.m_hover.m_world = true;
        io.AddMousePosEvent(400.0f, 600.0f);
        frame();
        PPR_TEST_ASSERT(ImGui::FindWindowByName("Inspector") == nullptr);

        // A pointer over a panel describes no cell, so Inspect stays clear too.
        state.m_tool = colony::EColonyTool::inspect;
        view.m_hover.m_world = false;
        view.m_hover.m_over_ui = true;
        frame();
        PPR_TEST_ASSERT(ImGui::FindWindowByName("Inspector") == nullptr);

        // Inspect over terrain shows a small passive tooltip that tracks the
        // pointer. The first frames settle the auto-fit size before the pivot
        // position takes.
        view.m_hover.m_over_ui = false;
        view.m_hover.m_world = true;
        io.AddMousePosEvent(1000.0f, 200.0f);
        for (int settle = 0; settle < 3; ++settle) {
            frame();
        }
        auto *const inspector = ImGui::FindWindowByName("Inspector");
        PPR_TEST_ASSERT(inspector != nullptr and inspector->Active);
        PPR_TEST_ASSERT((inspector->Flags & ImGuiWindowFlags_NoInputs) != 0);
        // The two-column field table is fixed-width, so the tooltip auto-fits a
        // narrow panel instead of growing with the longest value.
        PPR_TEST_ASSERT(inspector->Size.x > 100.0f and inspector->Size.x < 400.0f);
        const ImVec2 first{inspector->Pos.x, inspector->Pos.y};
        // Near the top-right edge the read-out flips its right edge back toward
        // the pointer instead of running off the frame.
        PPR_TEST_ASSERT(first.x < 1000.0f and first.y >= 200.0f);
        PPR_TEST_ASSERT(not io.WantCaptureMouse);

        // Moving the pointer near the bottom-left edge re-anchors the read-out
        // and it still never takes the pick.
        io.AddMousePosEvent(400.0f, 600.0f);
        for (int settle = 0; settle < 3; ++settle) {
            frame();
        }
        PPR_TEST_ASSERT(inspector != nullptr and inspector->Active);
        PPR_TEST_ASSERT(inspector->Pos.x != first.x or inspector->Pos.y != first.y);
        PPR_TEST_ASSERT(inspector->Pos.x >= 400.0f and inspector->Pos.y < 600.0f);
        PPR_TEST_ASSERT(not io.WantCaptureMouse);

        // Once pinned, the read-out anchors to the cell's projected position
        // instead of the pointer, so it tracks the cell under pan/zoom even
        // while the pointer is somewhere else entirely.
        state.m_pinned = sim::GlobalCellPos{12u, 34u};
        view.m_pinned_screen = colony::ColonyScreenPoint{.m_x = 760.0f, .m_y = 380.0f};
        io.AddMousePosEvent(120.0f, 120.0f);
        for (int settle = 0; settle < 3; ++settle) {
            frame();
        }
        PPR_TEST_ASSERT(inspector != nullptr and inspector->Active);
        PPR_TEST_ASSERT(inspector->Pos.x >= 760.0f and inspector->Pos.x < 800.0f);
        PPR_TEST_ASSERT(inspector->Pos.y >= 380.0f and inspector->Pos.y < 420.0f);
    };

    PPR_UNIT_TEST (native_menu_click_opens_utilities_and_display_emits_one_intent) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        ImGui::SetCurrentContext(context);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.Fonts->AddFontDefault();
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        colony::ColonyPresentation snapshot{.m_speed = 1u};
        const colony::ColonyUiView view{.m_colony = &snapshot};
        colony::ColonyUiState state{};
        const auto frame = [&]() {
            ImGui::NewFrame();
            const auto result = colony::drawColonyUi(view, {}, {}, state);
            ImGui::Render();
            return result;
        };

        // Internals locate the native item's submitted navigation rectangle;
        // actual mouse down/up frames drive Button, OpenPopup and MenuItem.
        const auto itemCenter = [&](ImGuiWindow *const window, const ImGuiID id) {
            PPR_TEST_ASSERT(window != nullptr);

            // Settle native popup navigation before locating a non-default row;
            // keep the previous pointer from overriding the requested nav target.
            io.AddMousePosEvent(-100.0f, -100.0f);
            std::ignore = frame();
            ImGui::FocusWindow(window);
            ImGui::SetNavCursorVisible(true);
            ImGui::SetNavID(id, ImGuiNavLayer_Main, 0u, ImRect{});
            std::ignore = frame();

            const ImRect rect = window->NavRectRel[ImGuiNavLayer_Main];
            PPR_TEST_ASSERT(rect.GetWidth() > 0.0f and rect.GetHeight() > 0.0f);
            return ImGui::WindowRectRelToAbs(window, rect).GetCenter();
        };

        const auto click = [&](const ImVec2 position) {
            io.AddMousePosEvent(position.x, position.y);
            std::ignore = frame();

            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            std::ignore = frame();

            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            return frame();
        };

        std::ignore = frame();
        std::ignore = frame();
        ImGuiWindow *const topbar = ImGui::FindWindowByName("##topbar");
        PPR_TEST_ASSERT(topbar != nullptr);

        const auto opened = click(itemCenter(topbar, topbar->GetID("Menu")));
        PPR_TEST_ASSERT(not opened.m_intent);
        PPR_TEST_ASSERT(state.m_popup_visible_last_frame);
        PPR_TEST_ASSERT(not context->OpenPopupStack.empty());

        ImGuiWindow *const popup = context->OpenPopupStack.back().Window;
        PPR_TEST_ASSERT(popup != nullptr and popup->Active);

        for (const auto item:
            {std::pair{"Display", colony::EColonyIntent::toggle_display},
                std::pair{"Performance", colony::EColonyIntent::toggle_performance},
                std::pair{"Colony", colony::EColonyIntent::toggle_colony},
                std::pair{"Quit", colony::EColonyIntent::quit}}) {
            if (not state.m_popup_visible_last_frame) {
                std::ignore = click(itemCenter(topbar, topbar->GetID("Menu")));
            }

            auto *const menu = context->OpenPopupStack.back().Window;
            const auto selected = click(itemCenter(menu, menu->GetID(item.first)));
            PPR_TEST_ASSERT(selected.m_intent.has_value());
            _.log(
                std::format(
                    "Menu {}: expected intent {}, received {}", item.first, static_cast<u32>(item.second),
                    static_cast<u32>(selected.m_intent->m_kind))
                    .c_str());
            PPR_TEST_ASSERT(selected.m_intent->m_kind == item.second);
            PPR_TEST_ASSERT(not state.m_popup_visible_last_frame);
            PPR_TEST_ASSERT(not frame().m_intent);
        }

        // The rail uses the same typed actions as keyboard dispatch. Build opens
        // the native popup on the next draw, and a menu choice selects a tool.
        auto *const rail = ImGui::FindWindowByName("##tool_rail");
        PPR_TEST_ASSERT(rail != nullptr);
        const auto dig = click(itemCenter(rail, rail->GetID("###tool1")));
        PPR_TEST_ASSERT(dig.m_intent and dig.m_intent->m_kind == colony::EColonyIntent::toggle_dig);

        const auto build = click(itemCenter(rail, rail->GetID("###tool2")));
        PPR_TEST_ASSERT(build.m_intent and build.m_intent->m_kind == colony::EColonyIntent::open_build_menu);

        state.m_build_menu_open = true;
        std::ignore = frame();
        PPR_TEST_ASSERT(not context->OpenPopupStack.empty());
        auto *const build_menu = context->OpenPopupStack.back().Window;
        std::ignore = frame();
        const auto wall = click(itemCenter(build_menu, build_menu->GetID("Wall")));
        PPR_TEST_ASSERT(wall.m_intent and wall.m_intent->m_kind == colony::EColonyIntent::select_tool);
        PPR_TEST_ASSERT(wall.m_intent->m_arg == static_cast<u32>(colony::EColonyTool::build_wall));
        PPR_TEST_ASSERT(not frame().m_intent);
    };

    // ------------------------------------------------------------------
    // native authoring workspace interaction
    // ------------------------------------------------------------------

    struct WorkspaceFixture {
        colony::ContentSourceSet m_source{};
        std::optional<asset::PackSet> m_index{};
        colony::WorldgenPanelState m_state{};
        u64 m_revision{1u};
        bool m_has_game{}, m_can_start_game{true};

        explicit WorkspaceFixture(const bool numeric) {
            const auto root = std::filesystem::current_path();
            std::string manifest{R"(schema_version = 1
pack_id = "ppr.ui"
[assets.first]
kind = "sim.material"
descriptor = "first.toml"
[assets.second]
kind = "sim.biome"
descriptor = "second.toml"
)"};
            if (numeric) {
                manifest.replace(manifest.find("sim.material"), std::string_view{"sim.material"}.size(), "sim.element");
            }

            m_source.m_packs.push_back({root, {root / "ui.pack.toml", manifest}});
            config::Diagnostics diagnostics;
            auto index = asset::PackSet::index(m_source.m_packs, colony::kContentAssetKinds, diagnostics);
            PPR_TEST_ASSERT(index.has_value());
            m_index.emplace(std::move(*index));

            for (const auto &entry: m_index->assets()) {
                const std::string bytes = entry.m_name.m_local == "first" ? numeric ? R"(schema_version = 1
density = 2000.0
)"
                                                                                    : R"(schema_version = 1
[appearance]
display_name = "Before"
)"
                                                                          : R"(schema_version = 1
)";
                m_source.m_descriptors.push_back({entry.m_descriptor, bytes});
            }

            m_state.m_selected_category = numeric ? "elements" : "materials";
            m_state.m_selected_key = "ppr.ui:first";
            m_state.m_focus_property = numeric ? "density" : "appearance.display_name";
            const char *const search = numeric ? "density" : "display_name";
            std::snprintf(m_state.m_property_search.data(), m_state.m_property_search.size(), "%s", search);
            m_state.m_browser.m_show_sort_headers = false;
            const auto first = std::ranges::find_if(
                m_index->assets(), [](const auto &entry) { return entry.m_name.m_local == "first"; });
            PPR_TEST_ASSERT(first != m_index->assets().end());
            m_state.m_selected_asset = first->m_key;
            m_state.m_browser.m_active_key = first->m_key;
        }

        Array<colony::WorkshopIntent> frame() {
            ImGui::NewFrame();
            auto intents = colony::drawWorldgenPanel(
                m_state, {.m_source = &m_source,
                             .m_revision = m_revision,
                             .m_validated_revision = m_revision,
                             .m_index = &*m_index,
                             .m_has_game = m_has_game,
                             .m_can_start_game = m_can_start_game});
            ImGui::Render();
            return intents;
        }

        void addWorldChoices(const std::string_view first, const std::string_view second) {
            m_source.m_packs.front().m_manifest.m_bytes += R"(
[assets.world_a]
kind = "sim.world"
descriptor = "world_a.toml"
[assets.world_b]
kind = "sim.world"
descriptor = "world_b.toml"
)";
            config::Diagnostics diagnostics;
            auto index = asset::PackSet::index(m_source.m_packs, colony::kContentAssetKinds, diagnostics);
            PPR_TEST_ASSERT(index.has_value());
            m_index.emplace(std::move(*index));

            for (const auto &entry: m_index->assets()) {
                if (entry.m_name.m_local != "world_a" and entry.m_name.m_local != "world_b") {
                    continue;
                }

                const auto bytes = std::format(
                    R"(schema_version = 1
[profile.biomes.sample]
biome = "{}"
)",
                    entry.m_name.m_local == "world_a" ? first : second);
                m_source.m_descriptors.push_back({entry.m_descriptor, bytes});
            }

            std::snprintf(m_state.m_profile.data(), m_state.m_profile.size(), "%s", "ppr.ui:world_a");
            std::snprintf(
                m_state.m_biome.data(), m_state.m_biome.size(), "%.*s", static_cast<int>(first.size()), first.data());
        }
    };

    ImGuiWindow *workspaceChild(ImGuiContext &context, const char *const name) {
        for (auto *const window: context.Windows) {
            if (window->Active and std::string_view{window->Name}.find(name) != std::string_view::npos) {
                return window;
            }
        }

        PPR_TEST_ASSERT(false);
        return nullptr;
    }

    PPR_UNIT_TEST (workspace_tile_navigation_commits_old_text_and_numeric_edits) {
        for (const u32 mode: {0u, 1u, 2u}) {
            const bool numeric = mode != 0u;
            const bool narrow = mode == 2u;
            ImGuiContext *const context = ImGui::CreateContext();
            PPR_DEFER {
                ImGui::DestroyContext(context);
            };
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2{narrow ? 640.0f : 1280.0f, 720.0f};
            io.DeltaTime = 1.0f / 60.0f;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            unsigned char *pixels{};
            int width{}, height{};
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

            WorkspaceFixture fixture{numeric};
            fixture.m_state.m_show_properties = narrow;
            std::ignore = fixture.frame();
            std::ignore = fixture.frame();
            ImGuiWindow *const properties = workspaceChild(*context, "/Properties_");
            ImGuiID seed = properties->GetID("Property tree");
            if (not numeric) {
                seed = ImHashStr("appearance", 0u, seed);
                seed = ImHashStr("appearance", 0u, seed);
            }

            seed = ImHashStr(numeric ? "density" : "appearance.display_name", 0u, seed);
            const ImGuiID value_id = ImHashStr("##value", 0u, seed);
            ImGui::FocusWindow(properties);
            ImGui::SetNavID(value_id, ImGuiNavLayer_Main, 0u, ImRect{});
            std::ignore = fixture.frame();
            const auto rect = properties->NavRectRel[ImGuiNavLayer_Main];
            PPR_TEST_ASSERT(rect.GetWidth() > 0.0f and rect.GetHeight() > 0.0f);

            const ImVec2 field = ImGui::WindowRectRelToAbs(properties, rect).GetCenter();
            io.AddMousePosEvent(field.x, field.y);
            io.AddKeyEvent(ImGuiMod_Ctrl, numeric);
            std::ignore = fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            std::ignore = fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            std::ignore = fixture.frame();

            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            std::ignore = fixture.frame();
            PPR_TEST_ASSERT(context->ActiveId == value_id);

            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
            std::ignore = fixture.frame();
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            std::ignore = fixture.frame();
            if (not context->InputEventsQueue.empty()) {
                const auto diagnostic = std::format(
                    "Before typing: mode={}, pending events={}", mode, context->InputEventsQueue.Size);
                _.log(diagnostic.c_str());
            }

            PPR_TEST_ASSERT(context->InputEventsQueue.empty());

            io.AddInputCharactersUTF8(numeric ? "2150" : "After");
            std::ignore = fixture.frame();
            if (numeric) {
                auto *const input = ImGui::GetInputTextState(value_id);
                PPR_TEST_ASSERT(context->ActiveId == value_id and input != nullptr);
                if (std::string_view{input->GetText()} != "2150") {
                    const auto diagnostic = std::format(
                        "Numeric input: mode={}, text='{}', pending events={}", mode, input->GetText(),
                        context->InputEventsQueue.Size);
                    _.log(diagnostic.c_str());
                }

                PPR_TEST_ASSERT(std::string_view{input->GetText()} == "2150");
            } else {
                PPR_TEST_ASSERT(fixture.m_state.m_edit_key == "appearance.display_name");
                PPR_TEST_ASSERT(std::string_view{fixture.m_state.m_text_edit.data()} == "After");
            }

            Array<colony::WorkshopIntent> navigation_intents{};
            if (narrow) {
                ImGuiTabBar *bar{};
                ImGuiTabItem *assets_tab{};
                for (int index = 0; index < context->TabBars.GetMapSize(); ++index) {
                    auto *const candidate = context->TabBars.TryGetMapData(index);
                    if (candidate == nullptr) {
                        continue;
                    }

                    for (auto &tab: candidate->Tabs) {
                        if (std::string_view{ImGui::TabBarGetTabName(candidate, &tab)} == "Assets") {
                            bar = candidate;
                            assets_tab = &tab;
                        }
                    }
                }

                PPR_TEST_ASSERT(bar != nullptr and assets_tab != nullptr);
                const ImVec2 position{
                    bar->BarRect.Min.x + assets_tab->Offset - bar->ScrollingAnim + assets_tab->Width * 0.5f,
                    (bar->BarRect.Min.y + bar->BarRect.Max.y) * 0.5f};
                io.AddMousePosEvent(position.x, position.y);
                std::ignore = fixture.frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                navigation_intents = fixture.frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                auto released = fixture.frame();
                navigation_intents.insert(navigation_intents.end(), released.begin(), released.end());
                PPR_TEST_ASSERT(fixture.m_state.m_selected_key == "ppr.ui:first");
            }

            ImGuiWindow *const grid = workspaceChild(*context, "/Assets_");
            const auto assets = fixture.m_index->assets();
            const auto target = std::ranges::find_if(
                assets, [](const auto &entry) { return entry.m_name.m_local == "second"; });
            PPR_TEST_ASSERT(target != assets.end());
            const auto catalog_index = static_cast<std::size_t>(target - assets.begin());
            const auto visible = std::ranges::find(fixture.m_state.m_browser.m_visible, catalog_index);
            PPR_TEST_ASSERT(visible != fixture.m_state.m_browser.m_visible.end());
            const auto position = static_cast<std::size_t>(visible - fixture.m_state.m_browser.m_visible.begin());
            const auto &layout = fixture.m_state.m_browser.m_layout;
            const auto columns = static_cast<std::size_t>(layout.m_column_count);
            const ImVec2 tile{
                grid->DC.CursorStartPos.x +
                    layout.m_outer_padding +
                    static_cast<float>(position % columns) * layout.m_item_step +
                    layout.m_item_size * 0.5f,
                grid->DC.CursorStartPos.y +
                    layout.m_outer_padding +
                    static_cast<float>(position / columns) * layout.m_item_step +
                    layout.m_item_size * 0.5f};
            io.AddMousePosEvent(tile.x, tile.y);
            std::ignore = fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            auto intents = fixture.frame();
            intents.insert(intents.end(), navigation_intents.begin(), navigation_intents.end());

            const auto edit = std::ranges::find_if(
                intents, [](const auto &intent) { return intent.m_kind == colony::EWorkshopIntent::edit_definition; });
            PPR_TEST_ASSERT(edit != intents.end());
            PPR_TEST_ASSERT(std::ranges::count_if(intents, [](const auto &intent) {
                return intent.m_kind == colony::EWorkshopIntent::edit_definition;
            }) == 1);
            PPR_TEST_ASSERT(edit->m_key == "ppr.ui:first");
            PPR_TEST_ASSERT(edit->m_property == (numeric ? "density" : "appearance.display_name"));
            if (numeric) {
                PPR_TEST_ASSERT(edit->m_value.m_floating == 2150.0);
            } else {
                PPR_TEST_ASSERT(edit->m_value.m_text == "After");
            }

            PPR_TEST_ASSERT(fixture.m_state.m_selected_key == "ppr.ui:second");
            PPR_TEST_ASSERT(fixture.m_state.m_selected_asset == target->m_key);
            PPR_TEST_ASSERT(ImGui::FindWindowByName("Asset Browser") == nullptr);

            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            std::ignore = fixture.frame();
            PPR_TEST_ASSERT(std::ranges::none_of(fixture.frame(), [](const auto &intent) {
                return intent.m_kind == colony::EWorkshopIntent::edit_definition;
            }));
        }
    };

    PPR_UNIT_TEST (workspace_identity_survives_filter_revision_and_native_game_actions_are_distinct) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        WorkspaceFixture fixture{false};
        std::ignore = fixture.frame();
        std::ignore = fixture.frame();
        const auto selected = fixture.m_state.m_selected_asset;

        fixture.m_source.m_packs.front().m_manifest.m_bytes += R"(
[assets.earlier]
kind = "sim.material"
descriptor = "earlier.toml"
)";
        config::Diagnostics diagnostics;
        auto reordered = asset::PackSet::index(fixture.m_source.m_packs, colony::kContentAssetKinds, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        fixture.m_index.emplace(std::move(*reordered));

        std::snprintf(
            fixture.m_state.m_browser.m_search.data(), fixture.m_state.m_browser.m_search.size(), "%s",
            "no matching asset");
        ++fixture.m_revision;
        std::ignore = fixture.frame();
        PPR_TEST_ASSERT(fixture.m_state.m_browser.m_visible.empty());
        PPR_TEST_ASSERT(
            fixture.m_state.m_selected_asset == selected and fixture.m_state.m_selected_key == "ppr.ui:first");

        const auto click = [&](const char *const identity) {
            ImGuiWindow *const window = ImGui::FindWindowByName("Authoring Workspace###Worldgen Workshop");
            PPR_TEST_ASSERT(window != nullptr);
            ImGui::FocusWindow(window);
            ImGui::SetNavID(window->GetID(identity), ImGuiNavLayer_Main, 0u, ImRect{});
            std::ignore = fixture.frame();
            const auto rect = window->NavRectRel[ImGuiNavLayer_Main];
            PPR_TEST_ASSERT(rect.GetWidth() > 0.0f and rect.GetHeight() > 0.0f);


            const ImVec2 position = ImGui::WindowRectRelToAbs(window, rect).GetCenter();
            io.AddMousePosEvent(position.x, position.y);
            std::ignore = fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            std::ignore = fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            return fixture.frame();
        };

        const auto start = click("###workshop_game");
        PPR_TEST_ASSERT(std::ranges::any_of(start, [](const auto &intent) {
            return intent.m_kind == colony::EWorkshopIntent::start_game;
        }));
        fixture.m_has_game = true;
        const auto resume = click("###workshop_game");
        PPR_TEST_ASSERT(std::ranges::any_of(resume, [](const auto &intent) {
            return intent.m_kind == colony::EWorkshopIntent::return_to_game;
        }));
        PPR_TEST_ASSERT(std::ranges::none_of(resume, [](const auto &intent) {
            return intent.m_kind == colony::EWorkshopIntent::close;
        }));

        const auto quit = click("###workshop_quit");
        PPR_TEST_ASSERT(std::ranges::any_of(quit, [](const auto &intent) {
            return intent.m_kind == colony::EWorkshopIntent::close;
        }));
        PPR_TEST_ASSERT(fixture.m_state.m_selected_asset == selected);
    };

    PPR_UNIT_TEST (workspace_preview_choices_follow_current_draft_profile_and_replacement_source) {
        ImGuiContext *const context = ImGui::CreateContext();
        PPR_DEFER {
            ImGui::DestroyContext(context);
        };
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        const auto canPreview = [&](WorkspaceFixture &fixture, const bool expected = true) {
            std::ignore = fixture.frame();
            std::ignore = fixture.frame();
            auto *const window = ImGui::FindWindowByName("Authoring Workspace###Worldgen Workshop");
            PPR_TEST_ASSERT(window != nullptr);
            ImGui::FocusWindow(window);
            ImGui::SetNavID(window->GetID("Prepare preview"), ImGuiNavLayer_Main, 0u, ImRect{});
            ImGui::SetNavCursorVisible(true);
            std::ignore = fixture.frame();
            PPR_TEST_ASSERT(context->NavId == window->GetID("Prepare preview"));
            PPR_TEST_ASSERT(context->NavWindow == window and context->NavCursorVisible);

            io.AddKeyEvent(ImGuiKey_Enter, true);
            const auto intents = fixture.frame();
            io.AddKeyEvent(ImGuiKey_Enter, false);
            std::ignore = fixture.frame();

            const bool actual = std::ranges::count_if(intents, [](const auto &intent) {
                return intent.m_kind == colony::EWorkshopIntent::preview;
            }) == 1;
            if (actual != expected) {
                const auto diagnostic = std::format(
                    "Preview gate expected={} actual={} profile={} biome={} revision={} worlds={} nav={} active={} navflags={}",
                    expected, actual, fixture.m_state.m_profile.data(), fixture.m_state.m_biome.data(),
                    fixture.m_revision, fixture.m_state.m_record_keys[4u].size(), context->NavId, context->ActiveId,
                    static_cast<int>(context->NavIdItemFlags));
                _.log(diagnostic.c_str());
            }

            return actual;
        };

        WorkspaceFixture fixture{false};
        fixture.addWorldChoices("first_biome", "second_biome");
        PPR_TEST_ASSERT(canPreview(fixture));
        PPR_TEST_ASSERT(canPreview(fixture));

        auto source = std::ranges::find_if(fixture.m_source.m_descriptors, [](const auto &entry) {
            return entry.m_path.filename() == "world_a.toml";
        });
        PPR_TEST_ASSERT(source != fixture.m_source.m_descriptors.end());
        const auto original = source->m_bytes;
        source->m_bytes.replace(
            source->m_bytes.find("first_biome"), std::string_view{"first_biome"}.size(), "edited_biome");
        ++fixture.m_revision;
        PPR_TEST_ASSERT(not canPreview(fixture, false));

        std::snprintf(fixture.m_state.m_biome.data(), fixture.m_state.m_biome.size(), "%s", "edited_biome");
        PPR_TEST_ASSERT(canPreview(fixture));

        source->m_bytes = original;
        ++fixture.m_revision;
        std::snprintf(fixture.m_state.m_biome.data(), fixture.m_state.m_biome.size(), "%s", "first_biome");
        PPR_TEST_ASSERT(canPreview(fixture));

        std::snprintf(fixture.m_state.m_profile.data(), fixture.m_state.m_profile.size(), "%s", "ppr.ui:world_b");
        PPR_TEST_ASSERT(not canPreview(fixture, false));
        std::snprintf(fixture.m_state.m_biome.data(), fixture.m_state.m_biome.size(), "%s", "second_biome");
        PPR_TEST_ASSERT(canPreview(fixture));

        std::snprintf(fixture.m_state.m_profile.data(), fixture.m_state.m_profile.size(), "%s", "ppr.ui:missing");
        PPR_TEST_ASSERT(not canPreview(fixture, false));
        PPR_TEST_ASSERT(not canPreview(fixture, false));

        std::snprintf(fixture.m_state.m_profile.data(), fixture.m_state.m_profile.size(), "%s", "ppr.ui:world_a");
        std::snprintf(fixture.m_state.m_biome.data(), fixture.m_state.m_biome.size(), "%s", "first_biome");
        source->m_bytes = "broken = [";
        ++fixture.m_revision;
        PPR_TEST_ASSERT(not canPreview(fixture, false));

        source->m_bytes = original;
        ++fixture.m_revision;
        PPR_TEST_ASSERT(canPreview(fixture));

        WorkspaceFixture replacement{false};
        replacement.addWorldChoices("replacement_biome", "second_biome");
        replacement.m_revision = fixture.m_revision;
        replacement.m_state = std::move(fixture.m_state);
        PPR_TEST_ASSERT(not canPreview(replacement, false));

        std::snprintf(
            replacement.m_state.m_biome.data(), replacement.m_state.m_biome.size(), "%s", "replacement_biome");
        PPR_TEST_ASSERT(canPreview(replacement));
    };

    // ------------------------------------------------------------------
    // synchronous snapshot and expected absence
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (snapshot_distinguishes_absence_unresident_vacuum_and_material) {
        colony::ColonyDriver driver{};
        colony::ColonyPresentation snapshot{};
        ui::captureColonyPresentation(snapshot, driver, std::nullopt, std::nullopt);
        PPR_TEST_ASSERT(not snapshot.m_has_hover and not snapshot.m_has_inspected);
        PPR_TEST_ASSERT(not snapshot.m_world.m_available and snapshot.m_agents.empty());

        const sim::GlobalCellPos target{17u, 21u};
        ui::captureColonyPresentation(snapshot, driver, target, target);
        PPR_TEST_ASSERT(snapshot.m_has_hover and snapshot.m_has_inspected);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == colony::ECellPresence::unresident);
        PPR_TEST_ASSERT(not snapshot.m_inspected.m_particle and not snapshot.m_inspected.m_nearest);

        PPR_TEST_ASSERT(not driver.grid().setCell(target, {0u, 293.0f}));
        ui::captureColonyPresentation(snapshot, driver, target, target);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == colony::ECellPresence::vacuum);

        PPR_TEST_ASSERT(not driver.grid().setCell(target, {1u, 305.0f}));
        ui::captureColonyPresentation(snapshot, driver, target, target);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == colony::ECellPresence::material);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_material == "Rock");
        PPR_TEST_ASSERT(snapshot.m_inspected.m_temperature_k == 305.0f);

        PPR_TEST_ASSERT(not driver.grid().setCell(target, {2u, 220.0f}));
        PPR_TEST_ASSERT(snapshot.m_inspected.m_material == "Rock" and snapshot.m_inspected.m_temperature_k == 305.0f);

        ui::captureColonyPresentation(snapshot, driver, std::nullopt, std::nullopt);
        PPR_TEST_ASSERT(not snapshot.m_has_hover and not snapshot.m_has_inspected);
    };

    // ------------------------------------------------------------------
    // modal action contracts
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (world_actions_close_during_activation_publication_and_shutdown) {
        ui::WorldProgressView view{.m_open = true};
        PPR_TEST_ASSERT(ui::worldActionAvailable(view, ui::EWorldAction::cancel));
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::retry));

        view.m_activation_pending = true;
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::cancel));
        view.m_activation_pending = false;
        view.m_published = true;
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::cancel));

        view.m_published = false;
        view.m_terminal = true;
        PPR_TEST_ASSERT(ui::worldActionAvailable(view, ui::EWorldAction::retry));
        PPR_TEST_ASSERT(ui::worldActionAvailable(view, ui::EWorldAction::quit));
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::cancel));

        view.m_closing = true;
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::retry));
        PPR_TEST_ASSERT(not ui::worldActionAvailable(view, ui::EWorldAction::quit));
    };

    PPR_UNIT_TEST (unsaved_edits_allow_cancel_while_a_save_settles) {
        ui::UnsavedEditsView view{.m_open = true};
        PPR_TEST_ASSERT(ui::unsavedActionAvailable(view, ui::EUnsavedAction::save));
        PPR_TEST_ASSERT(ui::unsavedActionAvailable(view, ui::EUnsavedAction::discard));

        view.m_saving = true;
        PPR_TEST_ASSERT(not ui::unsavedActionAvailable(view, ui::EUnsavedAction::save));
        PPR_TEST_ASSERT(not ui::unsavedActionAvailable(view, ui::EUnsavedAction::discard));
        PPR_TEST_ASSERT(ui::unsavedActionAvailable(view, ui::EUnsavedAction::cancel));

        view.m_open = false;
        PPR_TEST_ASSERT(not ui::unsavedActionAvailable(view, ui::EUnsavedAction::cancel));
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // game UI suite
    // ------------------------------------------------------------------

    const UnitTest presentation_suite = UnitTest::Named("ui") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::presentation::utility_windows_fit_normal_narrow_and_degenerate_viewports,
            detail::presentation::colony_chrome_preserves_a_world_region_at_normal_and_narrow_sizes,
            detail::presentation::colony_and_performance_visibility_are_independent,
            detail::presentation::inspector_is_a_passive_pointer_readout_only_in_inspect_mode,
            detail::presentation::native_menu_click_opens_utilities_and_display_emits_one_intent,
            detail::presentation::workspace_tile_navigation_commits_old_text_and_numeric_edits,
            detail::presentation::workspace_identity_survives_filter_revision_and_native_game_actions_are_distinct,
            detail::presentation::workspace_preview_choices_follow_current_draft_profile_and_replacement_source,
            detail::presentation::snapshot_distinguishes_absence_unresident_vacuum_and_material,
            detail::presentation::world_actions_close_during_activation_publication_and_shutdown,
            detail::presentation::unsaved_edits_allow_cancel_while_a_save_settles,
        });
    };

    const UnitTest &gameUiTests() noexcept {
        return presentation_suite;
    }
}
