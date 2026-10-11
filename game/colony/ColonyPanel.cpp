module;

#include "Colony.Elements.h"

module game.colony.panel;

import imgui;
import engine.app;
import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.driver;
import game.colony.content;
import game.colony.worldgen;
import game.display;
import std;

namespace pP::colony {
    namespace {
        // Palette: the same values the Slice 1 panel used for PAUSED/RUNNING
        // and the same cyan/orange the game's debug-draw materials use, so
        // chrome, outline, and world agree. Every state is written as words
        // first; colour only repeats what the text already says.
        constexpr ImVec4 kAmber{1.0f, 0.72f, 0.34f, 1.0f};
        constexpr ImVec4 kGreen{0.48f, 0.88f, 0.62f, 1.0f};
        constexpr ImVec4 kRed{1.0f, 0.45f, 0.42f, 1.0f};
        constexpr ImVec4 kCyan{0.2f, 0.9f, 0.9f, 1.0f};
        constexpr ImVec4 kOrange{1.0f, 0.55f, 0.15f, 1.0f};

        /// Row tints for the colonist list: the tint repeats what the text
        /// already says (selected = cyan, hovered = orange), and the
        /// selected row also gets an accent bar + "> " prefix so the state
        /// never depends on colour alone.
        constexpr ImVec4 kSelectedRowBg{0.2f, 0.9f, 0.9f, 0.18f};
        constexpr ImVec4 kHoveredRowBg{1.0f, 0.55f, 0.15f, 0.12f};

        /// Fixed chrome bands: never movable/resizable/collapsible/saved/
        /// docked/scrolling, and never focus-stealing on first appearance
        /// (a first-frame focus steal would take keyboard capture from frame
        /// 2 onward and silence every shortcut until the player clicked the
        /// world again).
        constexpr ImGuiWindowFlags kChromeFlags = ImGuiWindowFlags_NoTitleBar |
                                                  ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                                  ImGuiWindowFlags_NoScrollbar |
                                                  ImGuiWindowFlags_NoScrollWithMouse |
                                                  ImGuiWindowFlags_NoDocking;

        /// Font-relative padding: ImGui style padding does not scale with
        /// DpiEnableScaleFonts, so every gap derives from the current font
        /// size and stays proportionate at 100/150/200% DPI.
        void pushChromeStyle() {
            const float fs = ImGui::GetFontSize();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 0.6f, fs * 0.5f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{fs * 0.5f, fs * 0.35f});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{fs * 0.45f, fs * 0.35f});
        }

        void popChromeStyle() {
            ImGui::PopStyleVar(3);
        }

        [[nodiscard]] float framedWidth(const char *label) noexcept {
            return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        [[nodiscard]] float plainWidth(const char *label) noexcept {
            return ImGui::CalcTextSize(label).x;
        }

        /// Wrapping row for the top bar: continues on one line while the next
        /// item fits the window's content width, otherwise stays on the fresh
        /// line the previous item left the cursor on. Controls reflow at
        /// narrow widths; nothing is ever hidden behind an overflow.
        struct BarFlow final {
            float m_avail{};
            float m_gap{};
            float m_line_w{};
            bool m_started{};

            void place(const float item_w) {
                if (m_started) {
                    if (m_line_w + m_gap + item_w <= m_avail) {
                        ImGui::SameLine(0.0f, m_gap);
                        m_line_w += m_gap;
                    } else {
                        m_line_w = 0.0f;
                    }
                }
                m_line_w += item_w;
                m_started = true;
            }
        };

        [[nodiscard]] const ColonyShortcut *findShortcut(
            const std::span<const ColonyShortcut> shortcuts, const EColonyIntent kind, const u32 arg) noexcept {
            for (const ColonyShortcut &row: shortcuts) {
                if (row.m_kind == kind and row.m_arg == arg) {
                    return &row;
                }
            }
            return nullptr;
        }

        /// Key label for one intent, straight from the caller's binding
        /// table ("" when it has no key of its own). First-run, camera, and
        /// tool copy name a key only through this, so Help can never drift
        /// from the bindings the Keyboard section renders.
        [[nodiscard]] const char *boundKey(
            const std::span<const ColonyShortcut> shortcuts, const EColonyIntent kind, const u32 arg) noexcept {
            const ColonyShortcut *row = findShortcut(shortcuts, kind, arg);
            return row != nullptr ? row->m_key : "";
        }

        /// Tooltip = tip text + key label, both read from the caller's
        /// binding table; no second, hand-maintained shortcut map exists
        /// anywhere in this file.
        void shortcutTip(const std::span<const ColonyShortcut> shortcuts, const EColonyIntent kind, const u32 arg) {
            if (const ColonyShortcut *row = findShortcut(shortcuts, kind, arg); row != nullptr) {
                ImGui::SetItemTooltip("%s\nKey: %s", row->m_tip, row->m_key);
            }
        }

        [[nodiscard]] const char *elementName(const u16 element) noexcept {
            switch (element) {
                case kElementVacuum: return "Vacuum";
                case kElementRock: return "Rock";
                case kElementIce: return "Ice";
                case kElementCaustic: return "Caustic";
                case kElementOil: return "Oil";
                case kElementRuins: return "Ruins";
                case kElementAbyssal: return "Abyssal";
                case kElementVent: return "Vent";
                default: return "Unknown";
            }
        }

        [[nodiscard]] const char *goalName(const u32 goal) noexcept {
            switch (static_cast<EGoal>(goal)) {
                case EGoal::seek: return "seek";
                case EGoal::work: return "work";
                case EGoal::flee: return "flee";
                default: return "idle";
            }
        }

        /// Display suffix for a stable agent key: the low 32 bits (the
        /// index half of `(generation<<32)|index`). Rows and the Inspector
        /// show this; identity-critical copy prints the whole key.
        [[nodiscard]] u32 keySuffix(const u64 key) noexcept {
            return static_cast<u32>(key);
        }

        [[nodiscard]] const char *toolName(const EColonyTool tool) noexcept {
            switch (tool) {
                case EColonyTool::inspect: return "Inspect";
                case EColonyTool::dig: return "Dig";
                case EColonyTool::build_wall: return "Build wall";
                case EColonyTool::demolish: return "Demolish";
                default: return "Inspect";
            }
        }

        /// What the active tool's mouse gesture does. Footprint rows name
        /// the real press/drag/release contract; the exact rect size, the
        /// clamp cap, and any "will not commit" reason ride the strip's
        /// size line, so the cap is stated once, not twice.
        [[nodiscard]] const char *gestureText(const EColonyTool tool) noexcept {
            switch (tool) {
                case EColonyTool::inspect: return "LMB: click a cell to pin";
                case EColonyTool::dig: return "LMB: click digs 1 cell, hold drags";
                case EColonyTool::build_wall: return "LMB: press, drag a rect, release to queue one wall; Esc cancels";
                case EColonyTool::demolish: return "LMB: press, drag a rect, release to demolish it; Esc cancels";
                default: return "";
            }
        }

        /// One cell read for the status line and Inspector. Residency comes
        /// from `isResident` because `getCell` reads a non-resident chunk
        /// back as vacuum (it would otherwise report a fake 0 K cell), and a
        /// temperature is only shown for real, resident non-vacuum cells.
        struct CellRead final {
            sim::GlobalCellPos m_pos{};
            bool m_resident{};
            bool m_has_cell{};
            u16 m_element{kElementVacuum};
            float m_temperature{};
        };

        [[nodiscard]] CellRead readCell(const sim::ChunkGrid &grid, const sim::GlobalCellPos pos) {
            CellRead out{.m_pos = pos};
            out.m_resident = grid.isResident(sim::chunkOf(pos));
            if (const Expected<sim::Cell> cell = grid.getCell(pos); cell) {
                out.m_has_cell = true;
                out.m_element = cell->m_element;
                out.m_temperature = cell->m_temperature;
            }
            return out;
        }

        /// "Cell x, y · element · temperature" with honest absent states.
        void drawCellLine(const ColonyDriver &driver, const CellRead &cell) {
            ImGui::Text("Cell %u, %u", cell.m_pos.m_x, cell.m_pos.m_y);
            ImGui::SameLine();
            if (not cell.m_resident or not cell.m_has_cell) {
                ImGui::TextDisabled("(chunk not resident)");
                return;
            }
            const auto *const catalog = driver.contentCatalog();
            if (catalog == nullptr) {
                ImGui::TextDisabled("· %s", elementName(cell.m_element));
            } else if (cell.m_element == 0u) {
                ImGui::TextDisabled("· Empty terrain");
            } else if (const auto *const material = catalog->findMaterial(cell.m_element)) {
                ImGui::TextDisabled("· %s (ID %u)", material->m_display_name.c_str(), cell.m_element);
            } else {
                ImGui::TextDisabled("· Invalid terrain material ID %u", cell.m_element);
            }
            if (cell.m_element != kElementVacuum) {
                ImGui::SameLine();
                ImGui::TextDisabled("· %.1f K", cell.m_temperature);
            }
        }

        /// Bottom status line: hovered cell/element, pinned target, then the
        /// newest bounded notice (with its error code) or the standing tool
        /// instruction. Returns its height for the side-panel layout.
        float drawStatusLine_(
            const ColonyDriver &driver, const ColonyUiView &view, ColonyUiState &state,
            std::optional<ColonyIntent> &intent) {
            const ImGuiIO &io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2{0.0f, io.DisplaySize.y}, ImGuiCond_Always, ImVec2{0.0f, 1.0f});
            ImGui::SetNextWindowSize(ImVec2{io.DisplaySize.x, 0.0f}, ImGuiCond_Always);
            ImGui::Begin("##status", nullptr, kChromeFlags);

            bool wrote_row = false;
            if (view.m_hover.m_world) {
                drawCellLine(driver, readCell(driver.grid(), view.m_hover.m_target));
                wrote_row = true;
                if (view.m_hover.m_reason[0] != '\0') {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
                    ImGui::Text("(%s)", view.m_hover.m_reason);
                    ImGui::PopStyleColor();
                }
            } else if (view.m_hover.m_reason[0] != '\0') {
                ImGui::TextDisabled("No cell under pointer (%s)", view.m_hover.m_reason);
                wrote_row = true;
            }

            if (state.m_pinned) {
                if (wrote_row) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(" | ");
                    ImGui::SameLine();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
                ImGui::Text("Pinned:");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                drawCellLine(driver, readCell(driver.grid(), *state.m_pinned));
            }

            if (state.m_notice.visible()) {
                // A notice carries a code only when something was rejected,
                // and only a failure prints one (name and value, so the
                // player can see which check said no). A success has no
                // code to print: it states its own result behind an
                // explicit "[ok] " label, so the good news never depends on
                // colour and never borrows a fake code suffix. Text,
                // colour, suffix, Dismiss, and lifetime are all unchanged
                // on the failure path.
                const bool failed = state.m_notice.m_error.value() != 0;
                if (failed) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                    ImGui::Text("%s", state.m_notice.m_text.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "(%s #%d)", state.m_notice.m_error.category().name(), state.m_notice.m_error.value());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::Text("[ok] %s", state.m_notice.m_text.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Dismiss")) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::dismiss_notice};
                }
            } else {
                ImGui::TextDisabled("%s", state.m_instruction.c_str());
            }

            const float height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
            ImGui::End();
            return height;
        }

        /// Wrapping top simulation bar plus the contextual tool strip and the
        /// build/regenerate popups. Every button dispatches the same intent
        /// as its keyboard shortcut (parity by construction). Returns the
        /// bar height for the side-panel layout.
        float drawTopBar_(
            const ColonyDriver &driver, const ColonyUiView &view, const std::span<const ColonyShortcut> shortcuts,
            ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            const ImGuiIO &io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{io.DisplaySize.x, 0.0f}, ImGuiCond_Always);
            ImGui::Begin("##topbar", nullptr, kChromeFlags);

            const float fs = ImGui::GetFontSize();
            BarFlow flow{.m_avail = ImGui::GetContentRegionAvail().x, .m_gap = fs * 0.4f};

            const auto place_button = [&flow, &intent, &shortcuts](
                                          const char *label, const EColonyIntent kind, const u32 arg) {
                flow.place(framedWidth(label));
                const bool pressed = ImGui::Button(label);
                shortcutTip(shortcuts, kind, arg);
                if (pressed) {
                    intent = ColonyIntent{.m_kind = kind, .m_arg = arg};
                }
            };

            // State chip first (word + colour, mirroring the old panel),
            // then the running speed; digits are uniform in the UI font, so
            // measuring one speed label covers all three.
            const char *state_word = driver.paused() ? "PAUSED" : "RUNNING";
            flow.place(plainWidth(state_word));
            ImGui::PushStyleColor(ImGuiCol_Text, driver.paused() ? kAmber : kGreen);
            ImGui::Text("%s", state_word);
            ImGui::PopStyleColor();

            flow.place(plainWidth("·  1x"));
            ImGui::Text("·  %ux", driver.speed());

            place_button(driver.paused() ? "Resume" : "Pause", EColonyIntent::pause_toggle, 0u);

            ImGui::BeginDisabled(not driver.paused());
            place_button("Step", EColonyIntent::step, 0u);
            ImGui::EndDisabled();

            flow.place(plainWidth("|"));
            ImGui::TextDisabled("|");

            for (u32 index = 1u; index <= 3u; ++index) {
                const char *label = index == 1u ? "1x" : (index == 2u ? "2x" : "3x");
                const bool active = driver.speed() == index;
                flow.place(framedWidth(label));
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                }
                const bool pressed = ImGui::Button(label);
                if (active) {
                    ImGui::PopStyleColor();
                }
                shortcutTip(shortcuts, EColonyIntent::set_speed, index);
                if (pressed) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::set_speed, .m_arg = index};
                }
            }

            flow.place(plainWidth("|"));
            ImGui::TextDisabled("|");

            // Tool selection. The active tool carries brackets + colour +
            // text together; Dig emits the same toggle_dig intent as G, and
            // Inspect emits the same select_tool intent as I.
            const auto place_tool = [&flow, &intent, &shortcuts, &state](
                                        const char *label, const char *active_label, const EColonyTool tool,
                                        const EColonyIntent kind, const u32 arg) {
                const bool active = state.m_tool == tool;
                const char *shown = active ? active_label : label;
                flow.place(framedWidth(shown));
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
                }
                const bool pressed = ImGui::Button(shown);
                if (active) {
                    ImGui::PopStyleColor();
                }
                shortcutTip(shortcuts, kind, arg);
                if (pressed) {
                    intent = ColonyIntent{.m_kind = kind, .m_arg = arg};
                }
            };
            place_tool(
                "Inspect", "[Inspect]", EColonyTool::inspect, EColonyIntent::select_tool,
                static_cast<u32>(EColonyTool::inspect));
            place_tool("Dig", "[Dig]", EColonyTool::dig, EColonyIntent::toggle_dig, 0u);
            place_button("Build...", EColonyIntent::open_build_menu, 0u);

            flow.place(plainWidth("|"));
            ImGui::TextDisabled("|");

            // Seed is a readout, not a button; "New world" asks first.
            char seed_label[32]{};
            std::snprintf(seed_label, sizeof(seed_label), "Seed %llu", static_cast<unsigned long long>(driver.seed()));
            flow.place(plainWidth(seed_label));
            ImGui::TextDisabled("%s", seed_label);
            place_button("New game", EColonyIntent::regenerate, 0u);

            flow.place(plainWidth("|"));
            ImGui::TextDisabled("|");

            {
                const bool active = state.m_tab == EColonyPanelTab::performance;
                const char *shown = active ? "[Performance]" : "Performance";
                flow.place(framedWidth(shown));
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                }
                const bool pressed = ImGui::Button(shown);
                if (active) {
                    ImGui::PopStyleColor();
                }
                shortcutTip(shortcuts, EColonyIntent::toggle_performance, 0u);
                if (pressed) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::toggle_performance};
                }
            }
            place_button("?", EColonyIntent::help, 0u);
            {
                const bool open = state.m_display_open;
                const char *shown = open ? "[Display]" : "Display";
                flow.place(framedWidth(shown));
                if (open) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                }
                const bool pressed = ImGui::Button(shown);
                if (open) {
                    ImGui::PopStyleColor();
                }
                ImGui::SetItemTooltip("Picture and display settings");
                if (pressed) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::toggle_display};
                }
            }

            // Second band: contextual tool strip (tool, gesture, size
            // readout, Cancel/Esc).
            ImGui::Spacing();
            ImGui::Separator();
            BarFlow strip{.m_avail = ImGui::GetContentRegionAvail().x, .m_gap = fs * 0.4f};

            char tool_label[48]{};
            std::snprintf(tool_label, sizeof(tool_label), "Tool: %s", toolName(state.m_tool));
            strip.place(plainWidth(tool_label));
            const ImVec4 tool_color = state.m_tool == EColonyTool::dig
                                          ? kCyan
                                          : (state.m_tool == EColonyTool::inspect ? kGreen : kOrange);
            ImGui::PushStyleColor(ImGuiCol_Text, tool_color);
            ImGui::Text("%s", tool_label);
            ImGui::PopStyleColor();

            const char *gesture = gestureText(state.m_tool);
            strip.place(plainWidth(gesture));
            ImGui::TextDisabled("%s", gesture);

            // Size readout: dig and inspect keep the standing one-cell
            // brush line; the footprint tools swap it for their live rect -
            // the idle expectation, the rect being sized, the cap that cut
            // it, or why a release would not commit. A reason is text here,
            // never drawn into the world, and the orange only underlines
            // what "Won't commit" already says.
            char size_label[96]{};
            const bool footprint = state.m_tool == EColonyTool::build_wall or state.m_tool == EColonyTool::demolish;
            bool size_hot = false;
            if (footprint) {
                const ColonyPreviewView &preview = view.m_preview;
                if (not preview.m_active) {
                    std::snprintf(
                        size_label, sizeof(size_label), "Rect: drag to size, max %u cells", preview.m_max_edge);
                } else if (preview.m_reason[0] != '\0') {
                    std::snprintf(size_label, sizeof(size_label), "Won't commit: %s", preview.m_reason);
                    size_hot = true;
                } else if (preview.m_clamped_x or preview.m_clamped_y) {
                    std::snprintf(
                        size_label, sizeof(size_label), "Rect: %ux%u · Clamped to %u", preview.m_width,
                        preview.m_height, preview.m_max_edge);
                } else {
                    std::snprintf(size_label, sizeof(size_label), "Rect: %ux%u", preview.m_width, preview.m_height);
                }
            } else {
                std::snprintf(size_label, sizeof(size_label), "Brush: 1 cell");
            }

            strip.place(plainWidth(size_label));
            if (size_hot) {
                ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
                ImGui::Text("%s", size_label);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("%s", size_label);
            }

            strip.place(framedWidth("Cancel"));
            if (ImGui::Button("Cancel")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::cancel};
            }
            shortcutTip(shortcuts, EColonyIntent::cancel, 0u);

            // Build menu: B and the button both land here; items only select
            // tools (Slice 5 owns placement), so the menu never places by
            // itself. The one-shot trigger is consumed in this frame's draw.
            if (state.m_build_menu_open) {
                state.m_build_menu_open = false;
                ImGui::OpenPopup("Build tools");
            }
            if (ImGui::BeginPopup("Build tools")) {
                if (ImGui::MenuItem("Rock wall")) {
                    intent = ColonyIntent{
                        .m_kind = EColonyIntent::select_tool, .m_arg = static_cast<u32>(EColonyTool::build_wall)};
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Demolish")) {
                    intent = ColonyIntent{
                        .m_kind = EColonyIntent::select_tool, .m_arg = static_cast<u32>(EColonyTool::demolish)};
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TextDisabled("Only these two so far; other blueprints come later.");
                ImGui::EndPopup();
            }

            // Regenerate confirmation (destructive replacement). OpenPopup
            // is idempotent while the flag stands; the modal handles its own
            // Escape here because ImGui only auto-closes non-modal popups,
            // and emitting a cancel intent instead would clear a pin the
            // player never touched.
            if (state.m_regen_confirm_open) {
                ImGui::OpenPopup("Start a new game?");
            }
            if (ImGui::BeginPopupModal("Start a new game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Generate a new world and reset the colony?");
                ImGui::TextDisabled("Gameplay pauses while the new game is prepared.");
                ImGui::TextDisabled(
                    "The current game (seed %llu) is replaced only on success.",
                    static_cast<unsigned long long>(driver.seed()));
                if (ImGui::Button("Start new game")) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::regenerate, .m_confirmed = true};
                    state.m_regen_confirm_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    state.m_regen_confirm_open = false;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    state.m_regen_confirm_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            const float height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
            ImGui::End();
            return height;
        }

        /// Persistent Inspector: Hovering vs Pinned header, coordinates,
        /// element, temperature, pin/unpin, the Focus control, the selected
        /// colonist's stable key with its follow state, and the nearest
        /// colonist within 8 metres with honest empty states.
        void drawGeneratedCellMetadata_(const ColonyDriver &driver, sim::GlobalCellPos target);

        void drawInspector_(
            const ColonyDriver &driver, const ColonyUiView &view, ColonyUiState &state,
            std::optional<ColonyIntent> &intent) {
            ImGui::SeparatorText("Inspector");

            const bool pinned = state.m_pinned.has_value();
            const std::optional<sim::GlobalCellPos> target =
                pinned
                    ? state.m_pinned
                    : (view.m_hover.m_world ? std::optional<sim::GlobalCellPos>{view.m_hover.m_target} : std::nullopt);

            if (pinned) {
                ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
                ImGui::Text("Pinned");
                ImGui::PopStyleColor();
            } else if (view.m_hover.m_world) {
                ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
                ImGui::Text("Hovering");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("Nothing selected");
            }

            if (target) {
                drawCellLine(driver, readCell(driver.grid(), *target));
                drawGeneratedCellMetadata_(driver, *target);
            } else {
                ImGui::TextDisabled("No cell under the pointer.");
            }

            // Always enabled: with a pin it unpins, without one it pins
            // the hovered cell through togglePin_ (or posts an honest
            // notice when the pointer is off-world), so the button never
            // needs a world read to gate on and never leaks a world action.
            if (ImGui::Button(pinned ? "Unpin" : "Pin")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::toggle_pin};
            }
            ImGui::SameLine();
            // Gated on the colonist selection, not on a cell under the
            // pointer: this Focus follows the selected colonist, and the
            // pointer is over this very button (so the world hover reads as
            // chrome). Gating on a cell target left it disabled exactly when
            // a colonist was selected and enabled only when it would post
            // "Select a colonist first".
            ImGui::BeginDisabled(not view.m_selected_agent.has_value());
            if (ImGui::Button("Focus")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::focus_selection};
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(
                view.m_selected_agent.has_value()
                    ? "Jump to the selected colonist and follow it"
                    : "No colonist selected - click one in the world, or use a row's Focus in the Colony section");

            // Slice 4: name the held selection by its stable key and say
            // out loud whether the camera is following it, so "selected"
            // and "following" are never guessed from the cell header
            // above. Written before the early return so it shows even
            // when no cell is hovered or pinned.
            if (view.m_selected_agent.has_value()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
                ImGui::Text(
                    "Colonist A%u · key %llu", keySuffix(*view.m_selected_agent),
                    static_cast<unsigned long long>(*view.m_selected_agent));
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (view.m_following) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::Text("· Following");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("· not following");
                }
            } else {
                ImGui::TextDisabled("No colonist selected.");
            }

            if (not target.has_value()) {
                return;
            }

            // Nearest colonist within 8 metres of the inspected cell.
            ImGui::Spacing();
            const float target_x = (static_cast<float>(target->m_x) + 0.5f) / driver.cellsPerMetre();
            const float target_y = (static_cast<float>(target->m_y) + 0.5f) / driver.cellsPerMetre();
            std::optional<std::size_t> nearest{};
            float nearest_d2 = 8.0f * 8.0f;
            const std::span<const AgentSummary> agents = driver.agentSummaries();
            for (std::size_t index = 0u; index < agents.size(); ++index) {
                const float dx = agents[index].m_x - target_x;
                const float dy = agents[index].m_y - target_y;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= nearest_d2 and (not nearest.has_value() or d2 < nearest_d2)) {
                    nearest_d2 = d2;
                    nearest = index;
                }
            }

            if (not nearest.has_value()) {
                ImGui::TextDisabled("No colonist within 8 m.");
                return;
            }

            const AgentSummary &row = agents[*nearest];
            ImGui::Text("Nearest colonist  A%u @ %.0f,%.0f", keySuffix(row.m_key), row.m_x, row.m_y);
            if (row.m_has_plan) {
                ImGui::Text("Plan  %s · action %u · replans %u", goalName(row.m_goal), row.m_pc, row.m_replans);
            } else {
                ImGui::TextDisabled("Plan  idle (no plan)");
            }
            ImGui::TextDisabled("A-number is the key's low 32 bits; selection holds the whole 64-bit key.");
        }

        void drawWorldGeneration_(const ColonyDriver &driver, std::optional<ColonyIntent> &intent) {
            const auto *const prepared = driver.preparedWorld();
            if (prepared == nullptr or not ImGui::CollapsingHeader("World generation")) {
                return;
            }
            const auto &descriptor = prepared->descriptor();
            const auto &world = prepared->generated();
            const auto pack = prepared->catalog().packId();
            ImGui::Text("Pack: %.*s", static_cast<int>(pack.size()), pack.data());
            ImGui::Text("Profile: %s", descriptor.m_profile.c_str());
            ImGui::Text("Seed: %016llx", static_cast<unsigned long long>(descriptor.m_seed));
            ImGui::Text("Generator: %u", descriptor.m_generator_version);
            ImGui::Text("Content: %016llx", static_cast<unsigned long long>(descriptor.m_content_fingerprint));
            ImGui::TextDisabled("3 fine cells/metre; reference thickness 1 m");
            constexpr std::array<const char *, 5u> phases{
                "Layout", "Features", "Raster", "Initial matter", "Validation"};
            for (std::size_t index = 0u; index < phases.size(); ++index) {
                ImGui::Text("%s: %.3f s", phases[index], time::seconds(world.m_report.m_phase_times[index]));
            }
            ImGui::Text(
                "Sites: %llu; features: %llu", static_cast<unsigned long long>(world.m_layout.sites().size()),
                static_cast<unsigned long long>(world.m_geometry.m_instances.size()));
            ImGui::Text(
                "Initial matter regions: %llu", static_cast<unsigned long long>(world.m_matter.m_regions.size()));
            ImGui::TextDisabled(
                driver.initialMatterBaselineValid() ? "Initial matter: unsimulated baseline"
                                                    : "Initial matter preview invalidated by terrain edits");
            if (ImGui::Button("Reload content and start new game")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::regenerate};
            }
        }

        void drawGeneratedCellMetadata_(const ColonyDriver &driver, const sim::GlobalCellPos target) {
            const auto *const prepared = driver.preparedWorld();
            if (prepared == nullptr) {
                return;
            }
            const auto &world = prepared->generated();
            const auto &catalog = prepared->catalog();
            const auto &recipe = world.m_plan.recipe();
            const double x_m = (static_cast<double>(target.m_x) + 0.5) * recipe.m_cell_size_m;
            const double y_m = (static_cast<double>(target.m_y) + 0.5) * recipe.m_cell_size_m;
            const u32 biome = sim::generationBiomeAt(world.m_plan, world.m_layout, x_m, y_m);
            if (biome < catalog.biomes().size()) {
                ImGui::Text("Biome: %s", catalog.biomes()[biome].m_display_name.c_str());
            }
            for (const auto &instance: world.m_geometry.m_instances) {
                if (instance.m_feature >= recipe.m_features.size() or instance.m_feature >= catalog.features().size()) {
                    continue;
                }
                const auto bounds = recipe.m_features[instance.m_feature].m_bounds;
                const i64 x = static_cast<i64>(target.m_x) - instance.m_origin_x;
                const i64 y = static_cast<i64>(target.m_y) - instance.m_origin_y;
                if (x >= bounds.m_min_x and x < bounds.m_max_x and y >= bounds.m_min_y and y < bounds.m_max_y) {
                    ImGui::Text("Feature footprint: %s", catalog.features()[instance.m_feature].m_display_name.c_str());
                }
            }
            if (not driver.initialMatterBaselineValid()) {
                ImGui::TextDisabled("Initial matter baseline hidden: terrain edited");
                return;
            }
            // Emitted spans are sorted by row then X. Inspect one point without
            // walking the whole world or the potentially million-span baseline.
            for (const auto &region: world.m_matter.m_regions) {
                const auto found = std::lower_bound(
                    region.m_spans.begin(), region.m_spans.end(), target,
                    [](const sim::InitialMatterSpan &span, const sim::GlobalCellPos cell) {
                        return span.m_y < cell.m_y or (span.m_y == cell.m_y and span.m_end_x <= cell.m_x);
                    });
                if (found == region.m_spans.end() or found->m_y != target.m_y or target.m_x < found->m_begin_x) {
                    continue;
                }
                if (const auto *const material = catalog.findMaterial(region.m_material)) {
                    ImGui::Text("Initial matter (unsimulated): %s", material->m_display_name.c_str());
                    ImGui::Text("Initial temperature: %.1f K", region.m_temperature_k);
                    ImGui::Text("Initial density: %.3f kg/m3", region.m_bulk_density_kg_m3);
                    if (region.m_fill_height_m) {
                        ImGui::Text("Baseline fill height: %.3f m", *region.m_fill_height_m);
                    }
                }
                return; // The baseline format assigns at most one material per span cell.
            }
            ImGui::TextDisabled("No initial matter assigned here");
        }

        /// Colony section: the Slice 1/2 counters, then the colonist list —
        /// one row per live summary with a per-row Focus button, the Find
        /// colonist control (same intent as F) and the Stop-follow control
        /// (same intent as Esc) — plus a shortcut pointer derived from the
        /// binding table.
        void drawColonySection_(
            const ColonyDriver &driver, const ColonyUiView &view, const std::span<const ColonyShortcut> shortcuts,
            ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            const ColonyPanelCounts &counts = view.m_counts;

            ImGui::Text("Seed  %llu", static_cast<unsigned long long>(driver.seed()));
            ImGui::Text("Tick  %llu", static_cast<unsigned long long>(driver.tickCount()));
            ImGui::Text("Simulated  %.1f ms", driver.simMs());
            ImGui::Separator();

            ImGui::TextColored(driver.paused() ? kAmber : kGreen, "%s", driver.paused() ? "PAUSED" : "RUNNING");
            ImGui::SameLine();
            ImGui::Text("·  %ux", driver.speed());

            ImGui::Text("Chunks  %u / %u", driver.grid().residentChunkCount(), sim::kChunkCount);
            ImGui::Text("Tiles   %u visible", counts.m_visible_tiles);
            ImGui::Text("Chunks  %u visible", counts.m_visible_chunks);
            ImGui::Text("Paths   %u ok / %u partial / %u blocked", counts.m_paths, counts.m_partials, counts.m_blocked);
            ImGui::Text("Errands %u active", counts.m_errands);
            ImGui::Text("Dug     %u cells (wake %u)", counts.m_dug_total, counts.m_wake_radius);
            ImGui::Text("Edits   %u chunks", counts.m_affected_chunks);
            ImGui::Text(
                "Dig     %s · highlight %s", counts.m_dig_mode ? "ON" : "off", counts.m_show_highlight ? "ON" : "off");

            // Colonists: the two controls first, then one row per live
            // summary. Both controls dispatch intents the keyboard already
            // uses, and both tooltips read from the binding table.
            ImGui::SeparatorText("Colonists");
            const float fs = ImGui::GetFontSize();
            const float gap = fs * 0.4f;
            BarFlow tools{.m_avail = ImGui::GetContentRegionAvail().x, .m_gap = gap};

            // Find: the label's key text comes from the binding table, so
            // the button can never advertise a key that is not bound.
            char find_label[48]{"Find colonist"};
            if (const ColonyShortcut *find_row = findShortcut(shortcuts, EColonyIntent::find_next, 0u);
                find_row != nullptr) {
                std::snprintf(find_label, sizeof(find_label), "Find colonist (%s)", find_row->m_key);
            }
            tools.place(framedWidth(find_label));
            if (ImGui::Button(find_label)) {
                intent = ColonyIntent{.m_kind = EColonyIntent::find_next};
            }
            shortcutTip(shortcuts, EColonyIntent::find_next, 0u);

            // Stop following: shown only while the camera actually follows,
            // and it dispatches the same cancel Esc uses - which stops a
            // follow first, so the label says exactly what the press does.
            if (view.m_following) {
                tools.place(framedWidth("Stop following"));
                if (ImGui::Button("Stop following")) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::cancel};
                }
                shortcutTip(shortcuts, EColonyIntent::cancel, 0u);
            }

            ImGui::Spacing();
            const float focus_w = framedWidth("Focus");
            for (const AgentSummary &row: driver.agentSummaries()) {
                const bool selected = view.m_selected_agent == row.m_key;
                const bool hovered = view.m_hovered_agent == row.m_key;

                // O2/rest come from the caller's keyed needs read; an
                // absent read prints honestly instead of a fake number.
                char o2_text[16]{"n/a"};
                char rest_text[16]{"n/a"};
                for (const AgentNeedsRow &needs: view.m_agent_needs) {
                    if (needs.m_key != row.m_key) {
                        continue;
                    }
                    std::snprintf(o2_text, sizeof(o2_text), "%.0f", needs.m_o2);
                    std::snprintf(rest_text, sizeof(rest_text), "%.0f", needs.m_rest);
                    break;
                }

                const char *prefix = selected ? "> " : "  ";
                char label[192]{};
                if (row.m_has_plan) {
                    std::snprintf(
                        label, sizeof(label), "%sA%u · plan · O2 %s · rest %s · %s · pc %u · replans %u", prefix,
                        keySuffix(row.m_key), o2_text, rest_text, goalName(row.m_goal), row.m_pc, row.m_replans);
                } else {
                    std::snprintf(
                        label, sizeof(label), "%sA%u · idle · O2 %s · rest %s · no plan", prefix, keySuffix(row.m_key),
                        o2_text, rest_text);
                }

                // Row chrome goes down first so the text sits on top: the
                // selected row gets a cyan tint, a "> " prefix and an
                // accent bar in the left gutter; the hovered row (its
                // marker is under the pointer) a faint orange tint. The
                // state therefore reads through symbol, background and
                // accent as well as colour.
                const bool one_line = plainWidth(label) + gap + focus_w <= ImGui::GetContentRegionAvail().x;
                const float row_h = one_line ? std::max(ImGui::GetTextLineHeight(), ImGui::GetFrameHeight())
                                             : ImGui::GetTextLineHeight() +
                                                   ImGui::GetStyle().ItemSpacing.y +
                                                   ImGui::GetFrameHeight();
                const ImVec2 row_min = ImGui::GetCursorScreenPos();
                if (selected or hovered) {
                    const ImVec4 tint = selected ? kSelectedRowBg : kHoveredRowBg;
                    const ImVec2 row_max{row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_h};
                    ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, ImGui::ColorConvertFloat4ToU32(tint));
                    if (selected) {
                        const ImVec2 bar_min{row_min.x - fs * 0.45f, row_min.y};
                        const ImVec2 bar_max{row_min.x - fs * 0.15f, row_min.y + row_h};
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            bar_min, bar_max, ImGui::ColorConvertFloat4ToU32(kCyan));
                    }
                }

                const bool tinted = selected or hovered;
                if (tinted) {
                    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kCyan : kOrange);
                }
                ImGui::Text("%s", label);
                if (tinted) {
                    ImGui::PopStyleColor();
                }

                // Unique ID per key: every row's Focus reads the same.
                char focus_label[40]{};
                std::snprintf(
                    focus_label, sizeof(focus_label), "Focus##agent%llu", static_cast<unsigned long long>(row.m_key));
                if (one_line) {
                    ImGui::SameLine(0.0f, gap);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - focus_w);
                }
                if (ImGui::Button(focus_label)) {
                    // Two-step: `focus_selection` carries no argument, so
                    // the row parks its key in the UI state first and the
                    // caller adopts it through the same selection path a
                    // world click uses before focusing (see the intent's
                    // case in the caller's dispatch).
                    state.m_focus_agent = row.m_key;
                    intent = ColonyIntent{.m_kind = EColonyIntent::focus_selection};
                }
                ImGui::SetItemTooltip("Jump to this colonist and follow it");
            }

            ImGui::Spacing();
            const ColonyShortcut *help_row = findShortcut(shortcuts, EColonyIntent::help, 0u);
            if (help_row != nullptr) {
                ImGui::TextDisabled("Shortcuts: press %s for the full list.", help_row->m_key);
            }
        }

        /// Bounded performance and operational diagnostics for the current view.
        void drawPerformance_(const ColonyUiView &view) {
            const ColonyPerfView &perf = view.m_perf;

            ImGui::SeparatorText("Frame - inter-frame delta, not GPU/present");
            ImGui::Text("Last       %.2f ms", perf.m_frame_ms);
            ImGui::Text("Avg        %.2f ms  since launch", perf.m_avg_frame_ms);
            ImGui::Text("Max        %.2f ms  since launch", perf.m_max_frame_ms);
            ImGui::Text(
                "Split      total %.2f ms  main %.2f ms  render (lane) %.2f ms", perf.m_frame_ms, perf.m_main_ms,
                perf.m_render_ms);
            ImGui::Text("Frames     %llu since launch", static_cast<unsigned long long>(perf.m_frames));
            ImGui::Text(
                "Window     min %.2f  p50 %.2f  p95 %.2f  max %.2f ms", perf.m_min_ms, perf.m_p50_ms, perf.m_p95_ms,
                perf.m_window_max_ms);
            ImGui::Text(
                "Over       %llu in window above 16.67 ms", static_cast<unsigned long long>(perf.m_over_budget));
            ImGui::Text(
                "Samples    %llu/%u in window", static_cast<unsigned long long>(perf.m_window_samples),
                perf.m_window_def);
            ImGui::TextDisabled(
                "Window: %u-sample ring; nearest-rank percentiles; bounded refresh.", perf.m_window_def);
            if (perf.m_gpu_validation) {
                ImGui::TextDisabled("GPU validation: on (expect slower frames than Release).");
            } else {
                ImGui::TextDisabled("GPU validation: off (pass --gpu-validation to enable).");
            }

            ImGui::SeparatorText("Simulation - simulated time, not CPU cost");
            ImGui::Text("Tick rate  %.1f Hz  fixed step (0 before the first tick)", perf.m_tick_hz);
            ImGui::Text("Ticks      %llu", static_cast<unsigned long long>(perf.m_tick_count));
            ImGui::Text("Simulated  %.1f ms", perf.m_sim_ms);
            ImGui::Text("State      %s", perf.m_paused ? "Paused" : "Running");
            ImGui::Text("Speed      %ux", perf.m_speed);
            ImGui::TextDisabled("Ticks and sim time belong to this world; both restart on regen.");

            ImGui::SeparatorText("Presentation - instantaneous, not totals");
            ImGui::Text("Submitted  %u / %u chunks", perf.m_submitted_chunks, perf.m_total_chunks);
            ImGui::Text("Staged     %u tiles", perf.m_staged_tiles);
            ImGui::Text("Cached     %u tiles", perf.m_cached_tiles);
            ImGui::Text("Last edit  %u chunks", perf.m_last_edit_chunks);
            ImGui::Text("Dirty      %u chunks", perf.m_dirty_chunks);
            ImGui::TextDisabled("Submitted is this frame's submission; the rest are live values.");

            ImGui::SeparatorText("Work queue - backlog now, counters this world");
            ImGui::Text("Colliders  %llu waiting", static_cast<unsigned long long>(perf.m_collider_pending));
            ImGui::Text("Errands    %u live", perf.m_live_errands);
            ImGui::Text("Dig issued %llu", static_cast<unsigned long long>(perf.m_dig_issued));
            ImGui::Text("Dig wait   %llu pending", static_cast<unsigned long long>(perf.m_dig_pending_est));
            ImGui::TextDisabled("Pending is issued minus applied - an upper bound, restarted by regen.");


        }

        /// Right side panel: section buttons, the persistent Inspector above
        /// the scrolling body (it can never scroll out of view), then the
        /// selected section. `bar_height`/`status_height` come from the bands
        /// drawn earlier this frame. Returns the panel width for the
        /// caller's overlay clip (0 when a degenerate window skips the
        /// frame, so the clip falls back to the full width).
        [[nodiscard]] float drawSidePanel_(
            const ColonyDriver &driver, const ColonyUiView &view, const std::span<const ColonyShortcut> shortcuts,
            ColonyUiState &state, std::optional<ColonyIntent> &intent, const float bar_height,
            const float status_height) {
            const ImGuiIO &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            const float panel_w = std::min(std::max(fs * 34.0f, 220.0f), io.DisplaySize.x * 0.45f);
            const float panel_h = io.DisplaySize.y - bar_height - status_height;
            if (panel_h < fs * 6.0f) {
                // Degenerate window (tiny or minimized): skip this frame
                // rather than submit an inverted rect.
                return 0.0f;
            }

            ImGui::SetNextWindowPos(ImVec2{io.DisplaySize.x - panel_w, bar_height}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{panel_w, panel_h}, ImGuiCond_Always);
            ImGui::Begin("##side", nullptr, kChromeFlags);

            // Section buttons (not a tab bar) so both emit intents and hand
            // keyboard capture back like every other chrome control.
            BarFlow tabs{.m_avail = ImGui::GetContentRegionAvail().x, .m_gap = fs * 0.4f};
            const auto tab_button = [&tabs, &intent, &state](
                                        const char *label, const char *active_label, const EColonyPanelTab tab) {
                const bool active = state.m_tab == tab;
                const char *shown = active ? active_label : label;
                tabs.place(framedWidth(shown));
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                }
                const bool pressed = ImGui::Button(shown);
                if (active) {
                    ImGui::PopStyleColor();
                }
                if (pressed) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::select_tab, .m_arg = static_cast<u32>(tab)};
                }
            };
            tab_button("Colony", "[Colony]", EColonyPanelTab::colony);
            tab_button("Performance", "[Performance]", EColonyPanelTab::performance);
            ImGui::Spacing();

            ImGui::BeginChild("##inspector", ImVec2{0.0f, 0.0f}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
            drawInspector_(driver, view, state, intent);
            ImGui::EndChild();

            ImGui::BeginChild("##body", ImVec2{0.0f, 0.0f});
            if (state.m_tab == EColonyPanelTab::performance) {
                drawPerformance_(view);
            } else {
                drawWorldGeneration_(driver, intent);
                drawColonySection_(driver, view, shortcuts, state, intent);
            }
            ImGui::EndChild();

            ImGui::End();
            return panel_w;
        }

        /// Reopenable help window: every key row comes from the binding
        /// table, and camera/tool copy names a key only by reading that same
        /// table (through `boundKey`), so Help cannot drift from the actual
        /// bindings. The first-run block stays until dismissed and lives for
        /// this session only; the window itself reopens from the "?" button
        /// or F1 whenever the player wants the reference back.
        void drawHelpWindow_(
            const std::span<const ColonyShortcut> shortcuts, ColonyUiState &state,
            std::optional<ColonyIntent> &intent) {
            if (not state.m_help_open) {
                return;
            }

            const ImGuiIO &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            ImGui::SetNextWindowPos(
                ImVec2{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
            ImGui::SetNextWindowSize(ImVec2{std::min(io.DisplaySize.x * 0.6f, fs * 70.0f), 0.0f}, ImGuiCond_Always);
            ImGui::Begin("##help", nullptr, kChromeFlags);

            if (not state.m_intro_dismissed) {
                ImGui::SeparatorText("First run");
                ImGui::TextDisabled(
                    "Pick a tool (%s inspect, %s dig, %s build menu), then click or drag the world.",
                    boundKey(shortcuts, EColonyIntent::select_tool, static_cast<u32>(EColonyTool::inspect)),
                    boundKey(shortcuts, EColonyIntent::toggle_dig, 0u),
                    boundKey(shortcuts, EColonyIntent::open_build_menu, 0u));
                ImGui::TextDisabled("Colonists keep working while you play.");
                ImGui::TextDisabled(
                    "%s shows the chunk grid, %s the edit highlight.",
                    boundKey(shortcuts, EColonyIntent::toggle_grid_overlay, 0u),
                    boundKey(shortcuts, EColonyIntent::toggle_edit_highlight, 0u));
                ImGui::TextDisabled(
                    "Reopen this window any time: the ? button in the top bar, or %s.",
                    boundKey(shortcuts, EColonyIntent::help, 0u));
                if (ImGui::SmallButton("Got it")) {
                    state.m_intro_dismissed = true;
                }
                ImGui::Spacing();
            }

            ImGui::SeparatorText("Camera");
            ImGui::Text("%-14s %s", "MMB drag", "pan the camera");
            ImGui::Text("%-14s %s", "Wheel", "zoom in and out");
            ImGui::Text("%-14s %s", "Focus", "Inspector: jump to the selected colonist and follow it");
            ImGui::Text("%-14s %s", "Stop following", "colony list, or Esc: hand the camera back");
            ImGui::Text("%-14s %s", "Find colonist", "selects only - press Focus to follow");

            ImGui::Spacing();
            ImGui::SeparatorText("Tools");
            const EColonyTool tools[4]{
                EColonyTool::inspect, EColonyTool::dig, EColonyTool::build_wall, EColonyTool::demolish};
            for (const EColonyTool tool: tools) {
                ImGui::Text("%-11s %s", toolName(tool), gestureText(tool));
            }
            ImGui::TextDisabled(
                "%s opens the build menu (Rock wall, Demolish).",
                boundKey(shortcuts, EColonyIntent::open_build_menu, 0u));

            ImGui::Spacing();
            ImGui::SeparatorText("Keyboard");
            for (const ColonyShortcut &row: shortcuts) {
                ImGui::Text("%-7s %s", row.m_key, row.m_tip);
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Mouse");
            ImGui::Text("LMB    uses the active tool (Inspect pins, Dig digs)");

            ImGui::Spacing();
            if (ImGui::Button("Close")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::help};
            }
            ImGui::End();
        }

        /// Keep draft synchronization and game action policy at the caller boundary.
        void drawDisplayWindow_(
            const ColonyDisplaySnapshot &snap, ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            if (not state.m_display_open) {
                return;
            }
            ColonyDisplaySession &session = state.m_display;
            if (not session.m_synced) {
                auto draft = game::displayDraftFrom(snap.m_view.m_committed);
                if (not draft) {
                    throw std::system_error{draft.error()};
                }
                session.m_draft = std::move(*draft);
                session.m_synced = true;
            }
            const auto availability = game::displaySwitchingAvailability(snap.m_capabilities);
            auto &draft = session.m_draft;
            const ui::DisplayWidgetState widget{
                .m_draft =
                    {draft.m_pacing, draft.m_cap_hz, draft.m_paced_vsync, draft.m_throttle_unfocused,
                        draft.m_window_mode, draft.m_monitor, draft.m_width, draft.m_height, draft.m_refresh_hz},
                .m_matches = game::displayDraftMatches(draft, snap.m_view.m_committed),
                .m_availability = {availability.m_supported, availability.m_reason},
                .m_trial_open = game::displayTrialUnresolved(session.m_confirm),
                .m_confirm_armed = session.m_confirm.m_armed,
                .m_revert_pending = session.m_confirm.m_revert_pending,
                .m_countdown_active = session.m_confirm.m_countdown_active,
                .m_seconds_left = session.m_confirm.m_seconds_left,
                .m_revert_failed = session.m_revert_failed,
                .m_last_error = session.m_last_error};
            const auto action = ui::drawDisplayWidget(snap, widget);
            if (not action) {
                return;
            }
            switch (*action) {
                case ui::EDisplayWidgetAction::use_monitor_cap:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_use_monitor_cap};
                    break;
                case ui::EDisplayWidgetAction::apply:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_apply};
                    break;
                case ui::EDisplayWidgetAction::reset_overrides:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_reset_overrides};
                    break;
                case ui::EDisplayWidgetAction::save:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_save};
                    break;
                case ui::EDisplayWidgetAction::keep:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_keep};
                    break;
                case ui::EDisplayWidgetAction::revert:
                    intent = ColonyIntent{.m_kind = EColonyIntent::display_revert};
                    break;
                case ui::EDisplayWidgetAction::close:
                    intent = ColonyIntent{.m_kind = EColonyIntent::toggle_display};
                    break;
            }
        }
    } // namespace

    ColonyUiFrame drawColonyUi(
        const ColonyDriver &driver, const ColonyUiView &view, const ColonyDisplaySnapshot &display,
        const std::span<const ColonyShortcut> shortcuts, ColonyUiState &state) {
        std::optional<ColonyIntent> intent{};

        pushChromeStyle();
        struct ChromeScope {
            bool active{true};
            void close() noexcept {
                if (active) {
                    active = false;
                    popChromeStyle();
                }
            }
            ~ChromeScope() noexcept {
                close();
            }
        } chrome_scope;
        const float status_height = drawStatusLine_(driver, view, state, intent);
        const float bar_height = drawTopBar_(driver, view, shortcuts, state, intent);
        const float side_width = drawSidePanel_(driver, view, shortcuts, state, intent, bar_height, status_height);
        drawHelpWindow_(shortcuts, state, intent);
        drawDisplayWindow_(display, state, intent);
        chrome_scope.close();

        // Popup/focus bookkeeping after every window has run for this frame.
        // AnyPopupId + AnyPopupLevel reads the open-popup stack size, so a
        // modal or menu counts no matter where it sits in the stack.
        const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

        // Esc mirror: the engine's UI listener consumes Escape exactly when
        // last frame's widgets held keyboard capture (the same
        // WantCaptureKeyboard flag sampled this frame), so this branch and
        // the colony Esc action are mutually exclusive - never a double
        // cancel. While a popup owns the key, ImGui closes non-modal popups
        // itself and the regenerate modal handles its own Escape in
        // drawTopBar_; no cancel intent is emitted there.
        if (not popup_open and
            not intent.has_value() and
            ImGui::GetIO().WantCaptureKeyboard and
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            intent = ColonyIntent{.m_kind = EColonyIntent::cancel};
        }

        ColonyUiFrame frame{};
        frame.m_intent = intent;
        frame.m_chrome_top = bar_height;
        frame.m_chrome_bottom = status_height;
        frame.m_chrome_right = side_width;
        // Hand the keyboard back to the game after a completed click or on
        // the frame a popup closes (mirrors the engine's background-click
        // deselect). Active editors and drags retain keyboard focus across
        // release so subsequent typing remains owned by their widget.
        // Never while a popup is open: a null focus request
        // closes every popup (FocusWindow(NULL) -> ClosePopupsOverWindow
        // (NULL) -> ClosePopupToLevel(0)).
        frame.m_focus_release = not popup_open and
                                not ImGui::IsAnyItemActive() and
                                (ImGui::IsMouseReleased(ImGuiMouseButton_Left) or state.m_popup_visible_last_frame);
        state.m_popup_visible_last_frame = popup_open;
        return frame;
    }
} // namespace pP::colony
