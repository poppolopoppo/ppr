module;

#include "pP/Macros.h"

module game.colony.panel;

import imgui;
import engine.app;
import engine.core;
import engine.math;
import engine.sim;
import game.colony.agents;
import game.display;
import game.ui;
import std;

namespace pP::colony {
    namespace {
        // ------------------------------------------------------------------
        // helpers
        // ------------------------------------------------------------------

        constexpr ImVec4 kAmber = ui::theme::kWarning.vec4();
        constexpr ImVec4 kGreen = ui::theme::kSuccess.vec4();
        constexpr ImVec4 kRed = ui::theme::kError.vec4();
        constexpr ImVec4 kCyan = ui::theme::kSelectionText.vec4();
        constexpr ImVec4 kOrange = ui::theme::kWarning.vec4();
        constexpr ImVec4 kSelectedRowBg = ui::theme::kSelection.vec4(0.18f);
        constexpr ImVec4 kHoveredRowBg = ui::theme::kAccent.vec4(0.18f);

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

        /// Cursor inspector: a decoration-free, input-transparent tooltip.
        /// Input transparency is load-bearing - the readout sits under the
        /// pointer, so it must never take the click that pins the cell it
        /// describes. Auto-resize keeps it as small as its content.
        constexpr ImGuiWindowFlags kInspectorFlags = ImGuiWindowFlags_NoDecoration |
                                                     ImGuiWindowFlags_NoInputs |
                                                     ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoFocusOnAppearing |
                                                     ImGuiWindowFlags_NoNav |
                                                     ImGuiWindowFlags_NoDocking |
                                                     ImGuiWindowFlags_AlwaysAutoResize;

        /// Cursor inspector surface: the authored ink token as a dark
        /// translucent tooltip, so the scene reads through while the cream
        /// value text keeps its contrast. The old fixed Inspector panel used
        /// the opaque tan surface; a pointer-following readout needs the dark
        /// one to stay legible over bright terrain.
        constexpr float kInspectorSurfaceAlpha = 0.82f;

        [[nodiscard]] float framedWidth(const char *label) noexcept {
            return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        [[nodiscard]] float plainWidth(const char *label) noexcept {
            return ImGui::CalcTextSize(label).x;
        }

        // ------------------------------------------------------------------
        // label/value tables
        // ------------------------------------------------------------------

        /// Every label rendered in a Colony panel label/value row, in
        /// section order. The shared label column is measured from this
        /// list; keep the list in sync when a row label changes. Help rows
        /// whose label is a shortcut key stay shorter than the longest entry
        /// here, so the same column serves them.
        constexpr const char *const kTableLabels[] = {
            // Colony counters:
            "Seed",
            "Tick",
            "Simulated",
            "Chunks",
            "Tiles",
            "Paths",
            "Dug",
            "Edits",
            // World generation:
            "Pack",
            "Profile",
            "Generator",
            "Content",
            "Known retained payload",
            "Sites",
            "Features",
            "Initial matter regions",
            "Persistent particles",
            "Layout",
            "Raster",
            "Initial matter",
            "Validation",
            "Layout scratch",
            "Features scratch",
            "Raster scratch",
            "Initial matter scratch",
            "Validation scratch",
            // Performance - frame:
            "Last",
            "Avg (since launch)",
            "Max (since launch)",
            "Split",
            "Frames",
            "Window",
            "Over",
            "Samples",
            // Performance - simulation:
            "Tick rate",
            "Ticks",
            "State",
            "Speed",
            // Performance - presentation:
            "Submitted",
            "Staged",
            "Cached",
            "Last edit",
            "Dirty",
            // Performance - work queue:
            "Colliders",
            "Errands",
            "Dig issued",
            "Dig wait",
            // Help:
            "MMB drag",
            "Wheel",
            "Focus",
            "Stop following",
            "Find colonist",
            "Inspect",
            "Dig",
            "Build wall",
            "Demolish",
            "LMB",
        };

        /// Shared label-column width: the widest label plus cell padding, so
        /// labels and values align across every Colony panel table.
        [[nodiscard]] float labelColumnWidth() {
            float width = 0.0f;
            for (const char *const label: kTableLabels) {
                width = std::max(width, ImGui::CalcTextSize(label).x);
            }

            return std::min(width + ImGui::GetStyle().CellPadding.x * 2.0f, ImGui::GetContentRegionAvail().x * 0.45f);
        }

        /// Every label the Inspector field table can show, in row order. Its
        /// fixed label column is measured from this list, so the read-out keeps
        /// one width as the pointer sweeps cells; keep the list in sync when a
        /// row label changes.
        constexpr const char *const kInspectorLabels[] = {
            "Cell",
            "Terrain",
            "Temperature",
            "Biome",
            "Feature",
            "Particle",
            "Particle temp",
            "Volume",
            "Mass",
            "Nearby",
            "At",
            "Goal",
            "Action",
            "Replans",
            "Colonist",
            "Key",
            "Follow",
        };

        /// Inspector value-field width in compact-face multiples: fits a cell
        /// coordinate pair or a particle mass while keeping the read-out narrow.
        constexpr float kInspectorFieldEm = 9.0f;

        /// Inspector label column: the widest read-out label plus cell padding.
        [[nodiscard]] float inspectorLabelColumnWidth() {
            float width = 0.0f;
            for (const char *const label: kInspectorLabels) {
                width = std::max(width, ImGui::CalcTextSize(label).x);
            }

            return width + ImGui::GetStyle().CellPadding.x * 2.0f;
        }

        /// Begin one two-column Colony label/value table with the shared
        /// label column.
        [[nodiscard]] bool beginLabelTable(const char *const id, const float label_width) {
            if (not ImGui::BeginTable(id, 2)) {
                return false;
            }

            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        /// A complete label/value text row: secondary label in the shared
        /// label column, primary value in the stretch column.
        void textRow(const char *const label, const std::string_view value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary.vec4());
            ImGui::TextWrapped("%s", label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(value.data(), value.data() + value.size());
            ImGui::PopTextWrapPos();
        }

        /// Disabled note text that wraps inside the current window or child
        /// (TextDisabled does not wrap).
        void noteText(const char *const text) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }

        /// One dense section header: a secondary-colour label with an optional
        /// help marker, closed by a separator. Replaces the always-visible
        /// caption that used to sit under every performance section.
        void sectionHeader(const char *const label, const char *const help) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary.vec4());
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            if (help != nullptr) {
                ImGui::SameLine();
                ui::helpMarker(help);
            }

            ImGui::Separator();
        }

        // ------------------------------------------------------------------
        // bar flow
        // ------------------------------------------------------------------

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

        // ------------------------------------------------------------------
        // cell reads
        // ------------------------------------------------------------------

        /// One cell read for the status line. Residency comes from the
        /// presentation's presence because `getCell` reads a non-resident
        /// chunk back as vacuum (it would otherwise report a fake 0 K cell).
        void drawCellLine(const ColonyCellPresentation &cell) {
            ImGui::Text("Cell %u, %u", cell.m_position.m_x, cell.m_position.m_y);
            if (cell.m_presence == ECellPresence::unresident or cell.m_presence == ECellPresence::unavailable) {
                ImGui::SameLine();
                ImGui::TextDisabled("(chunk not resident)");
                return;
            }

            ImGui::SameLine();
            if (cell.m_presence == ECellPresence::vacuum) {
                ImGui::TextDisabled("· Empty terrain");
            } else if (cell.m_presence == ECellPresence::invalid_material) {
                ImGui::TextDisabled("· Invalid terrain material ID %u", cell.m_material_id);
            } else {
                ImGui::TextDisabled("· %.*s", static_cast<int>(cell.m_material.size()), cell.m_material.data());
            }

            if (cell.m_presence != ECellPresence::vacuum) {
                ImGui::SameLine();
                ImGui::TextDisabled("· %.1f K", static_cast<double>(cell.m_temperature_k));
            }
        }

        // ------------------------------------------------------------------
        // status line
        // ------------------------------------------------------------------

        /// Contextual feedback for the selected tool and colony controls.
        /// Inspect owns no section here: its live read-out is the cursor
        /// Inspector, which also carries the idle instruction, so a strip here
        /// would only duplicate it. Notices stay because they are interactive
        /// (the Dismiss button) and must surface even when the Inspector is
        /// hidden or the read-out is passive.
        void drawToolFeedback_(
            const ColonyPresentation &colony, const ColonyUiView &view, ColonyUiState &state,
            std::optional<ColonyIntent> &intent) {
            if (state.m_notice.visible()) {
                ImGui::SeparatorText(toolName(state.m_tool));
                const bool failed = static_cast<bool>(state.m_notice.m_error);
                ImGui::PushStyleColor(ImGuiCol_Text, failed ? kRed : kGreen);
                ImGui::TextWrapped("%s%s", failed ? "" : "[ok] ", state.m_notice.m_text.c_str());
                ImGui::PopStyleColor();
                if (failed) {
                    ImGui::TextDisabled(
                        "%s #%d", state.m_notice.m_error.category().name(), state.m_notice.m_error.value());
                }

                if (ImGui::SmallButton("Dismiss")) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::dismiss_notice};
                }

                return;
            }

            if (state.m_tool == EColonyTool::inspect) {
                return;
            }

            ImGui::SeparatorText(toolName(state.m_tool));
            ImGui::TextWrapped("%s", state.m_instruction.c_str());
            if (view.m_preview.m_active) {
                const auto &preview = view.m_preview;
                if (preview.m_reason[0] != '\0') {
                    ImGui::TextWrapped("Will not commit: %s", preview.m_reason);
                } else {
                    ImGui::Text(
                        "Footprint %u × %u%s", preview.m_width, preview.m_height,
                        preview.m_clamped ? " · clamped" : "");
                }
            } else if (colony.m_has_hover) {
                drawCellLine(colony.m_hover);
            }
        }

        // ------------------------------------------------------------------
        // top bar
        // ------------------------------------------------------------------

        /// Simulation/status ribbon with management actions, plus the
        /// build/regenerate popups. Every button dispatches the same intent
        /// as its keyboard shortcut (parity by construction). Returns the
        /// bar height for the side-panel layout.
        float drawTopBar_(
            const ColonyPresentation &colony, const std::span<const ColonyShortcut> shortcuts,
            std::optional<ColonyIntent> &intent) {
            const ImGuiIO &io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{io.DisplaySize.x, 0.0f}, ImGuiCond_Always);
            // Concept ribbon: one opaque accent-yellow band carrying ink text
            // across the full width. It deliberately keeps the default window
            // alpha (no floating-panel translucency) and the chrome flags'
            // NoTitleBar, so it never grows a window title or a second header.
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kAccent.vec4());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kAccentText.vec4());
            ImGui::Begin("##topbar", nullptr, kChromeFlags);
            const float fs = ImGui::GetFontSize();
            BarFlow flow{.m_avail = ImGui::GetContentRegionAvail().x, .m_gap = ImGui::GetStyle().ItemSpacing.x};

            ImFont *const heading_font = ui::theme::font(ui::theme::EFont::heading);
            ImGui::PushFont(heading_font, heading_font->LegacySize);
            flow.place(plainWidth("PPR"));
            ImGui::TextUnformatted("PPR");
            ImGui::PopFont();
            const char *const pause_label = colony.m_paused ? ui::icons::kPlayLabel.data()
                                                            : ui::icons::kPauseLabel.data();
            flow.place(framedWidth(pause_label));
            if (ImGui::Button(pause_label)) {
                intent = ColonyIntent{.m_kind = EColonyIntent::pause_toggle};
            }
            shortcutTip(shortcuts, EColonyIntent::pause_toggle, 0u);

            flow.place(framedWidth(ui::icons::kStepLabel.data()));
            ImGui::BeginDisabled(not colony.m_paused);
            if (ImGui::Button(ui::icons::kStepLabel.data())) {
                intent = ColonyIntent{.m_kind = EColonyIntent::step};
            }
            ImGui::EndDisabled();
            shortcutTip(shortcuts, EColonyIntent::step, 0u);

            flow.place(framedWidth("3x") + fs);
            ImGui::SetNextItemWidth(framedWidth("3x") + fs);
            constexpr const char *speeds[]{"1x", "2x", "3x"};
            int speed = static_cast<int>(colony.m_speed) - 1;
            if (ImGui::Combo("##speed", &speed, speeds, 3)) {
                intent = ColonyIntent{.m_arg = static_cast<u32>(speed + 1), .m_kind = EColonyIntent::set_speed};
            }

            char status[128]{};
            std::snprintf(
                status, sizeof(status), "Time %.0fs | %llu colonists", colony.m_simulated_ms / 1000.0,
                static_cast<unsigned long long>(colony.m_agents.size()));
            flow.place(plainWidth(status));
            ImGui::TextUnformatted(status);

            const auto management = [&](const std::string_view label, const EColonyIntent kind) {
                flow.place(framedWidth(label.data()));
                if (ImGui::Button(label.data())) {
                    intent = ColonyIntent{.m_kind = kind};
                }

                shortcutTip(shortcuts, kind, 0u);
            };

            const float ordinary_gap = flow.m_gap;
            const float management_width = framedWidth(ui::icons::kFolderLabel.data()) +
                                           framedWidth(ui::icons::kSettingsLabel.data()) +
                                           framedWidth(ui::icons::kChartLabel.data()) +
                                           framedWidth(ui::icons::kHelpLabel.data()) +
                                           framedWidth("Menu") +
                                           4.0f * ordinary_gap;
            if (flow.m_line_w + ordinary_gap + management_width <= flow.m_avail) {
                flow.m_gap = flow.m_avail - flow.m_line_w - management_width;
            }

            management(ui::icons::kFolderLabel, EColonyIntent::open_workshop);
            flow.m_gap = ordinary_gap;
            management(ui::icons::kSettingsLabel, EColonyIntent::toggle_display);
            management(ui::icons::kChartLabel, EColonyIntent::toggle_performance);
            management(ui::icons::kHelpLabel, EColonyIntent::help);

            flow.place(framedWidth("Menu"));
            if (ImGui::Button("Menu")) {
                ImGui::OpenPopup("##utilities");
            }

            if (ImGui::BeginPopup("##utilities")) {
                ImGui::TextDisabled("Seed %llu", static_cast<unsigned long long>(colony.m_seed));
                ImGui::Separator();
                for (const auto item:
                    {std::pair{"Colony", EColonyIntent::toggle_colony},
                        std::pair{"Display", EColonyIntent::toggle_display},
                        std::pair{"Performance", EColonyIntent::toggle_performance},
                        std::pair{"Worldgen Workshop", EColonyIntent::open_workshop},
                        std::pair{"Help", EColonyIntent::help}, std::pair{"New game", EColonyIntent::regenerate}}) {
                    if (ImGui::MenuItem(item.first)) {
                        intent = ColonyIntent{.m_kind = item.second};
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) {
                    intent = ColonyIntent{.m_kind = EColonyIntent::quit};
                }

                ImGui::EndPopup();
            }

            const float height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
            ImGui::End();
            ImGui::PopStyleColor(2);
            return height;
        }

        // ------------------------------------------------------------------
        // inspector
        // ------------------------------------------------------------------

        /// One Inspector read-out row: a key and a read-only value field. The
        /// value renders as a disabled edit control rather than plain text, so
        /// the read-out reads as a compact form; the Inspector window is
        /// input-transparent, so the field can never take the pick the read-out
        /// describes. Keys use the panel tan, not the shared secondary ink,
        /// which is authored for the light tan panels and all but vanishes on
        /// this dark surface. `row` only namespaces the widget ids.
        void fieldRow(const int row, const char *const label, const char *const value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kPanel.vec4());
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();

            char buffer[192]{};
            std::snprintf(buffer, sizeof(buffer), "%s", value);
            ImGui::PushID(row);
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
            ImGui::PopID();
        }

        /// Inspector read-out: the union of the former fixed Inspector panel and
        /// the tool strip's Inspect section. It shows the pinned or pointer
        /// cell with its generated metadata and nearest colonist, then any
        /// selected colonist, as a two-column field table. Pinning stays a
        /// world click and Esc unpins, and Focus/Stop-following live in the
        /// Colony list, so this passive read-out owns no controls and repeats
        /// neither.
        void drawInspector_(const ColonyPresentation &colony, const ColonyUiView &view, ColonyUiState &state) {
            if (not colony.m_has_inspected and not view.m_selected_agent) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18.0f);
                ImGui::TextUnformatted(state.m_instruction.c_str());
                ImGui::PopTextWrapPos();
                return;
            }

            int row = 0;
            char value[192]{};

            if (ImGui::BeginTable("##inspector_fields", 2)) {
                ImGui::TableSetupColumn(
                    "##inspector_label", ImGuiTableColumnFlags_WidthFixed, inspectorLabelColumnWidth());
                ImGui::TableSetupColumn(
                    "##inspector_value", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kInspectorFieldEm);

                if (colony.m_has_inspected) {
                    const ColonyCellPresentation &cell = colony.m_inspected;

                    std::snprintf(value, sizeof(value), "%u, %u", cell.m_position.m_x, cell.m_position.m_y);
                    fieldRow(row++, "Cell", value);

                    const bool resident = cell.m_presence != ECellPresence::unresident and
                                          cell.m_presence != ECellPresence::unavailable;
                    if (not resident) {
                        fieldRow(row++, "Terrain", "(chunk not resident)");
                    } else if (cell.m_presence == ECellPresence::vacuum) {
                        fieldRow(row++, "Terrain", "Empty");
                    } else {
                        if (cell.m_presence == ECellPresence::invalid_material) {
                            std::snprintf(value, sizeof(value), "invalid material %u", cell.m_material_id);
                        } else {
                            std::snprintf(
                                value, sizeof(value), "%.*s", static_cast<int>(cell.m_material.size()),
                                cell.m_material.data());
                        }

                        fieldRow(row++, "Terrain", value);
                        std::snprintf(value, sizeof(value), "%.1f K", static_cast<double>(cell.m_temperature_k));
                        fieldRow(row++, "Temperature", value);
                    }

                    if (cell.m_generated) {
                        if (not cell.m_biome.empty()) {
                            std::snprintf(
                                value, sizeof(value), "%.*s", static_cast<int>(cell.m_biome.size()),
                                cell.m_biome.data());
                            fieldRow(row++, "Biome", value);
                        }

                        for (const auto feature: cell.m_features) {
                            std::snprintf(
                                value, sizeof(value), "%.*s", static_cast<int>(feature.size()), feature.data());
                            fieldRow(row++, "Feature", value);
                        }

                        if (not cell.m_particle_snapshot_current) {
                            fieldRow(row++, "Particle", "Unavailable; pin to capture");
                        } else if (cell.m_particle) {
                            const ColonyParticlePresentation &particle = *cell.m_particle;
                            std::snprintf(
                                value, sizeof(value), "%llu %.*s", static_cast<unsigned long long>(particle.m_id),
                                static_cast<int>(particle.m_material.size()), particle.m_material.data());
                            fieldRow(row++, "Particle", value);
                            std::snprintf(value, sizeof(value), "%.1f K", particle.m_temperature_k);
                            fieldRow(row++, "Particle temp", value);
                            std::snprintf(value, sizeof(value), "%.6f m3", particle.m_volume_m3);
                            fieldRow(row++, "Volume", value);
                            std::snprintf(value, sizeof(value), "%.6f kg", particle.m_mass_kg);
                            fieldRow(row++, "Mass", value);
                        } else {
                            fieldRow(row++, "Particle", "none");
                        }

                        std::snprintf(
                            value, sizeof(value), "%llu",
                            static_cast<unsigned long long>(cell.m_particle_snapshot_tick));
                        fieldRow(row++, "Particle tick", value);
                    }

                    if (cell.m_nearest) {
                        const AgentSummary &nearest = *cell.m_nearest;
                        std::snprintf(value, sizeof(value), "A%u", keySuffix(nearest.m_key));
                        fieldRow(row++, "Nearby", value);
                        std::snprintf(
                            value, sizeof(value), "%.0f, %.0f", static_cast<double>(nearest.m_x),
                            static_cast<double>(nearest.m_y));
                        fieldRow(row++, "At", value);
                        fieldRow(row++, "Goal", nearest.m_has_plan ? goalName(nearest.m_goal) : "Idle");
                        std::snprintf(value, sizeof(value), "%u", nearest.m_pc);
                        fieldRow(row++, "Action", value);
                        std::snprintf(value, sizeof(value), "%u", nearest.m_replans);
                        fieldRow(row++, "Replans", value);
                    } else {
                        fieldRow(row++, "Nearby", "none");
                    }
                }

                if (view.m_selected_agent) {
                    std::snprintf(value, sizeof(value), "A%u", keySuffix(*view.m_selected_agent));
                    fieldRow(row++, "Colonist", value);
                    std::snprintf(
                        value, sizeof(value), "%llu", static_cast<unsigned long long>(*view.m_selected_agent));
                    fieldRow(row++, "Key", value);
                    fieldRow(row++, "Follow", view.m_following ? "Following" : "Selected");
                }

                ImGui::EndTable();
            }
        }

        // ------------------------------------------------------------------
        // world generation
        // ------------------------------------------------------------------

        void drawWorldGeneration_(
            const ColonyPresentation &colony, ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            const auto &world = colony.m_world;
            if (not world.m_available or not ImGui::CollapsingHeader("World generation")) {
                return;
            }
            // The scale reference for every figure in this section rides the
            // header as a marker instead of an always-visible caption line.
            ImGui::SameLine();
            ui::helpMarker("3 fine cells/metre; reference thickness 1 m");

            const auto &descriptor = world;
            const auto pack = world.m_pack;
            const float label_width = labelColumnWidth();
            char value[96]{};

            if (beginLabelTable("##worldgen_identity", label_width)) {
                textRow("Pack", pack);
                textRow("Profile", descriptor.m_profile);
                std::snprintf(value, sizeof(value), "%016llx", static_cast<unsigned long long>(descriptor.m_seed));
                textRow("Seed", value);
                std::snprintf(value, sizeof(value), "%u", descriptor.m_generator_version);
                textRow("Generator", value);
                std::snprintf(
                    value, sizeof(value), "%016llx", static_cast<unsigned long long>(descriptor.m_content_fingerprint));
                textRow("Content", value);
                ImGui::EndTable();
            }

            constexpr std::array<const char *, 5u> phases{
                "Layout", "Features", "Raster", "Initial matter", "Validation"};
            if (beginLabelTable("##worldgen_report", label_width)) {
                for (std::size_t index = 0u; index < phases.size(); ++index) {
                    std::snprintf(value, sizeof(value), "%.3f s", world.m_phase_seconds[index]);
                    textRow(phases[index], value);
                }

                if (world.m_retained_bytes != 0u) {
                    std::snprintf(
                        value, sizeof(value), "%.2f MiB", static_cast<double>(world.m_retained_bytes) / 1048576.0);
                    textRow("Known retained payload", value);
                    for (std::size_t index = 0u; index < phases.size(); ++index) {
                        char label[48]{};
                        std::snprintf(label, sizeof(label), "%s scratch", phases[index]);
                        std::snprintf(
                            value, sizeof(value), "%.2f MiB",
                            static_cast<double>(world.m_scratch_bytes[index]) / 1048576.0);
                        textRow(label, value);
                    }
                }

                ImGui::EndTable();
            }

            if (world.m_retained_bytes != 0u) {
                // Caveat rides a marker; the absent-data message below is an
                // empty-state read-out and stays visible.
                ui::helpMarker("Payload estimates; exclude allocator, task and stack overhead");
            } else {
                noteText("Allocation observations unrecorded (restored checkpoint)");
            }

            if (beginLabelTable("##worldgen_scale", label_width)) {
                std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(world.m_sites));
                textRow("Sites", value);
                std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(world.m_features));
                textRow("Features", value);
                std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(world.m_matter_regions));
                textRow("Initial matter regions", value);
                std::snprintf(value, sizeof(value), "%u", world.m_particles);
                textRow("Persistent particles", value);
                ImGui::EndTable();
            }
            constexpr std::array<const char *, 5u> modes{
                "Textured terrain", "Material IDs", "Biomes", "Temperature", "Current particles"};
            int selected = static_cast<int>(state.m_preview_mode);
            if (ImGui::Combo("Preview", &selected, modes.data(), static_cast<int>(modes.size()))) {
                state.m_preview_mode = static_cast<EGridDiagnosticMode>(selected);
                intent = ColonyIntent{.m_arg = static_cast<u32>(selected), .m_kind = EColonyIntent::select_preview};
            }
            if (state.m_preview_mode == EGridDiagnosticMode::temperature) {
                ImGui::InputFloat("Minimum K", &state.m_temperature_min_k);
                ImGui::InputFloat("Maximum K", &state.m_temperature_max_k);
                if (ImGui::Button("Apply temperature range")) {
                    intent = ColonyIntent{
                        .m_arg = static_cast<u32>(state.m_preview_mode), .m_kind = EColonyIntent::select_preview};
                }
            }
            if (ImGui::Button("Reload content and start new game")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::regenerate};
            }
        }

        // ------------------------------------------------------------------
        // colony section
        // ------------------------------------------------------------------

        /// Colony section: simulation status and the colonist list, followed by collapsed diagnostics —
        /// one row per live summary with a per-row Focus button, the Find
        /// colonist control (same intent as F) and the Stop-follow control
        /// (same intent as Esc) — plus a shortcut pointer derived from the
        /// binding table.
        void drawColonySection_(
            const ColonyPresentation &colony, const ColonyUiView &view, const std::span<const ColonyShortcut> shortcuts,
            ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            ImGui::TextColored(colony.m_paused ? kAmber : kGreen, "%s", colony.m_paused ? "PAUSED" : "RUNNING");
            ImGui::SameLine();
            ImGui::Text("·  %ux", colony.m_speed);

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
            for (const AgentSummary &row: colony.m_agents) {
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

                    std::snprintf(o2_text, sizeof(o2_text), "%.0f", static_cast<double>(needs.m_o2));
                    std::snprintf(rest_text, sizeof(rest_text), "%.0f", static_cast<double>(needs.m_rest));
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
                ImGui::TextWrapped("%s", label);
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

            if (ImGui::CollapsingHeader("Diagnostics")) {
                const ColonyPanelCounts &counts = view.m_counts;
                const float label_width = labelColumnWidth();
                char value[96]{};

                if (beginLabelTable("##colony_status", label_width)) {
                    std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(colony.m_seed));
                    textRow("Seed", value);
                    std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(colony.m_tick_count));
                    textRow("Tick", value);
                    std::snprintf(value, sizeof(value), "%.1f ms", colony.m_simulated_ms);
                    textRow("Simulated", value);
                    ImGui::EndTable();
                }

                if (beginLabelTable("##colony_counts", label_width)) {
                    std::snprintf(value, sizeof(value), "%u / %u", colony.m_resident_chunks, sim::kChunkCount);
                    textRow("Chunks", value);
                    std::snprintf(value, sizeof(value), "%u visible", counts.m_visible_tiles);
                    textRow("Tiles", value);
                    std::snprintf(value, sizeof(value), "%u visible", counts.m_visible_chunks);
                    textRow("Chunks", value);
                    std::snprintf(
                        value, sizeof(value), "%u ok / %u partial / %u blocked", counts.m_paths, counts.m_partials,
                        counts.m_blocked);
                    textRow("Paths", value);
                    std::snprintf(value, sizeof(value), "%u active", counts.m_errands);
                    textRow("Errands", value);
                    std::snprintf(value, sizeof(value), "%u cells (wake %u)", counts.m_dug_total, counts.m_wake_radius);
                    textRow("Dug", value);
                    std::snprintf(value, sizeof(value), "%u chunks", counts.m_affected_chunks);
                    textRow("Edits", value);
                    std::snprintf(
                        value, sizeof(value), "%s · highlight %s", counts.m_dig_mode ? "ON" : "off",
                        counts.m_show_highlight ? "ON" : "off");
                    textRow("Dig", value);
                    ImGui::EndTable();
                }
            }

            ImGui::Spacing();
            // Documentation pointer, not a status line: the sentence moves
            // into the marker's tooltip.
            const ColonyShortcut *help_row = findShortcut(shortcuts, EColonyIntent::help, 0u);
            if (help_row != nullptr) {
                char shortcuts_tip[96]{};
                std::snprintf(
                    shortcuts_tip, sizeof(shortcuts_tip), "Shortcuts: press %s for the full list.", help_row->m_key);
                ui::helpMarker(shortcuts_tip);
            }
        }

        // ------------------------------------------------------------------
        // performance
        // ------------------------------------------------------------------

        /// Bounded performance and operational diagnostics for the current view.
        /// Each section is one label/value table under a compact header; the
        /// caveats that used to sit under every table live in the header's help
        /// marker.
        void drawPerformance_(const ColonyUiView &view) {
            const ColonyPerfView &perf = view.m_perf;
            const float label_width = labelColumnWidth();
            char value[96]{};
            char help[256]{};

            std::snprintf(
                help, sizeof(help),
                "Inter-frame delta, never GPU or present time. %u-sample ring with nearest-rank percentiles; %s.",
                perf.m_window_def,
                perf.m_gpu_validation ? "GPU validation on (expect slower frames than Release)"
                                      : "GPU validation off (pass --gpu-validation to enable)");
            sectionHeader("Frame", help);
            if (beginLabelTable("##perf_frame", label_width)) {
                std::snprintf(value, sizeof(value), "%.2f ms", static_cast<double>(perf.m_frame_ms));
                textRow("Last", value);
                std::snprintf(value, sizeof(value), "%.2f ms", static_cast<double>(perf.m_avg_frame_ms));
                textRow("Avg (since launch)", value);
                std::snprintf(value, sizeof(value), "%.2f ms", static_cast<double>(perf.m_max_frame_ms));
                textRow("Max (since launch)", value);
                std::snprintf(
                    value, sizeof(value), "total %.2f ms · main %.2f ms · render (lane) %.2f ms",
                    static_cast<double>(perf.m_frame_ms), static_cast<double>(perf.m_main_ms),
                    static_cast<double>(perf.m_render_ms));
                textRow("Split", value);
                std::snprintf(
                    value, sizeof(value), "%llu since launch", static_cast<unsigned long long>(perf.m_frames));
                textRow("Frames", value);
                std::snprintf(
                    value, sizeof(value), "min %.2f · p50 %.2f · p95 %.2f · max %.2f ms",
                    static_cast<double>(perf.m_min_ms), static_cast<double>(perf.m_p50_ms),
                    static_cast<double>(perf.m_p95_ms), static_cast<double>(perf.m_window_max_ms));
                textRow("Window", value);
                std::snprintf(
                    value, sizeof(value), "%llu in window above 16.67 ms",
                    static_cast<unsigned long long>(perf.m_over_budget));
                textRow("Over", value);
                std::snprintf(
                    value, sizeof(value), "%llu/%u in window", static_cast<unsigned long long>(perf.m_window_samples),
                    perf.m_window_def);
                textRow("Samples", value);
                ImGui::EndTable();
            }

            sectionHeader("Simulation", "Simulated time, not CPU cost. Ticks and sim time restart on regen.");
            if (beginLabelTable("##perf_simulation", label_width)) {
                std::snprintf(
                    value, sizeof(value), "%.1f Hz · fixed step (0 before the first tick)",
                    static_cast<double>(perf.m_tick_hz));
                textRow("Tick rate", value);
                std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(perf.m_tick_count));
                textRow("Ticks", value);
                std::snprintf(value, sizeof(value), "%.1f ms", static_cast<double>(perf.m_sim_ms));
                textRow("Simulated", value);
                textRow("State", perf.m_paused ? "Paused" : "Running");
                std::snprintf(value, sizeof(value), "%ux", perf.m_speed);
                textRow("Speed", value);
                ImGui::EndTable();
            }

            sectionHeader(
                "Presentation", "Instantaneous, not totals. Submitted is this frame's; the rest are live values.");
            if (beginLabelTable("##perf_presentation", label_width)) {
                std::snprintf(value, sizeof(value), "%u / %u chunks", perf.m_submitted_chunks, perf.m_total_chunks);
                textRow("Submitted", value);
                std::snprintf(value, sizeof(value), "%u tiles", perf.m_staged_tiles);
                textRow("Staged", value);
                std::snprintf(value, sizeof(value), "%u tiles", perf.m_cached_tiles);
                textRow("Cached", value);
                std::snprintf(value, sizeof(value), "%u chunks", perf.m_last_edit_chunks);
                textRow("Last edit", value);
                std::snprintf(value, sizeof(value), "%u chunks", perf.m_dirty_chunks);
                textRow("Dirty", value);
                ImGui::EndTable();
            }

            sectionHeader(
                "Work queue", "Backlog now, counters this world. Pending is issued minus applied - an upper bound.");
            if (beginLabelTable("##perf_work", label_width)) {
                std::snprintf(
                    value, sizeof(value), "%llu waiting", static_cast<unsigned long long>(perf.m_collider_pending));
                textRow("Colliders", value);
                std::snprintf(value, sizeof(value), "%u live", perf.m_live_errands);
                textRow("Errands", value);
                std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(perf.m_dig_issued));
                textRow("Dig issued", value);
                std::snprintf(
                    value, sizeof(value), "%llu pending", static_cast<unsigned long long>(perf.m_dig_pending_est));
                textRow("Dig wait", value);
                ImGui::EndTable();
            }
        }

        // ------------------------------------------------------------------
        // independent information windows and supported tool rail
        // ------------------------------------------------------------------

        void drawColonySummary_(
            const ColonyPresentation &colony, const ColonyUiView &view, const std::span<const ColonyShortcut> shortcuts,
            ColonyUiState &state, std::optional<ColonyIntent> &intent, const float top, const float rail) {
            if (not state.m_colony_open and state.m_tool == EColonyTool::inspect and not state.m_notice.visible()) {
                return;
            }

            const auto &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            const float width = std::min(fs * 22.0f, std::max(fs * 8.0f, io.DisplaySize.x - rail - fs));
            const float maximum_height = std::max(1.0f, io.DisplaySize.y - top - fs);
            ImGui::SetNextWindowPos(ImVec2{rail + fs * 0.5f, top + fs * 0.5f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2{width, maximum_height * 0.75f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(
                ImVec2{fs * 12.0f, fs * 8.0f}, ImVec2{io.DisplaySize.x, maximum_height});
            const auto flags = ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoDocking;
            const char *const title = state.m_colony_open ? "Colony###colony_summary" : "Tool context###colony_summary";
            ImGui::SetNextWindowBgAlpha(ui::theme::kFloatingPanelAlpha);
            // The title bar is the panel's one header: push the heading face
            // for Begin only, so the body draws on the current face.
            ui::WindowTitleFont title_font{};
            const bool visible = ImGui::Begin(title, state.m_colony_open ? &state.m_colony_open : nullptr, flags);
            title_font.close();
            if (visible) {
                drawToolFeedback_(colony, view, state, intent);
                if (state.m_colony_open) {
                    drawColonySection_(colony, view, shortcuts, state, intent);
                    drawWorldGeneration_(colony, state, intent);
                }
            }

            ImGui::End();
        }

        /// Cursor Inspector: a tiny dark tooltip that follows the pointer while
        /// the Inspect tool is active. It is deliberately input-transparent -
        /// pinning is a world click and Esc unpins - so the read-out can sit
        /// under the pointer without stealing the pick it describes. Any other
        /// tool hides it, keeping the world clear.
        void drawInspectorWindow_(const ColonyPresentation &colony, const ColonyUiView &view, ColonyUiState &state) {
            if (state.m_tool != EColonyTool::inspect) {
                return;
            }
            // Keep the world read-out out of any open menu or modal: it would
            // otherwise be drawn across the popup under the same pointer.
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                return;
            }

            // The read-out exists only for something it can describe: a pinned
            // cell, or the terrain under the pointer. A pointer over a panel
            // describes no cell, so the tooltip stays out of the way instead of
            // covering the controls it would float over.
            const bool pinned = state.m_pinned.has_value();
            const bool over_terrain = view.m_hover.m_world and not view.m_hover.m_over_ui;
            if (not pinned and not over_terrain) {
                return;
            }

            // The whole read-out binds to the compact face: the anchor offset,
            // the label column, the value fields and the window padding all
            // derive from GetFontSize(), so one push shrinks them together.
            ImFont *const readout_font = ui::theme::font(ui::theme::EFont::tiny);
            ImGui::PushFont(readout_font, readout_font->LegacySize);

            const ImGuiIO &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            // A pinned read-out is anchored to its cell's projected position,
            // so it tracks the cell across pan/zoom; a cell the camera has left
            // falls back to the pointer. An unpinned read-out follows the
            // pointer. Either way the anchor flips near the viewport edges so
            // the read-out stays on screen instead of hanging off the frame.
            const bool have_anchor = pinned and view.m_pinned_screen.has_value();
            const ImVec2 anchor = have_anchor ? ImVec2{view.m_pinned_screen->m_x, view.m_pinned_screen->m_y}
                                              : ImVec2{io.MousePos.x, io.MousePos.y};
            const bool flip_x = anchor.x > io.DisplaySize.x * 0.6f;
            const bool flip_y = anchor.y > io.DisplaySize.y * 0.6f;
            ImGui::SetNextWindowPos(
                ImVec2{anchor.x + (flip_x ? -fs * 0.75f : fs * 0.75f), anchor.y + (flip_y ? -fs * 0.75f : fs * 0.75f)},
                ImGuiCond_Always, ImVec2{flip_x ? 1.0f : 0.0f, flip_y ? 1.0f : 0.0f});
            ImGui::SetNextWindowSizeConstraints(ImVec2{fs * 14.0f, 0.0f}, ImVec2{fs * 26.0f, io.DisplaySize.y});

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kTextPrimary.vec4(kInspectorSurfaceAlpha));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kPaper.vec4());
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::theme::kRaised.vec4(0.88f));
            // Value fields: the theme's cream frame is authored for the light
            // tan surfaces and washes the dimmed read-only text out here, so
            // the read-out insets them with the ink surface and lets the single
            // control border carry the field outline.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::theme::kTextPrimary.vec4());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 0.6f, fs * 0.45f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{fs * 0.35f, fs * 0.15f});
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{fs * 0.25f, fs * 0.12f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin("Inspector", nullptr, kInspectorFlags)) {
                drawInspector_(colony, view, state);
            }

            ImGui::End();
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(4);
            ImGui::PopFont();
        }

        void drawPerformanceWindow_(const ColonyUiView &view, ColonyUiState &state, const float top) {
            if (not state.m_performance_open) {
                return;
            }

            const auto &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            const float width = std::min(fs * 32.0f, io.DisplaySize.x);
            const float height = std::max(1.0f, std::min(fs * 40.0f, io.DisplaySize.y - top - fs));
            ImGui::SetNextWindowPos(
                ImVec2{std::max(0.0f, io.DisplaySize.x - width - fs * 0.5f), top + fs * 0.5f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2{width, height}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(ui::theme::kFloatingPanelAlpha);
            // The title bar is the panel's one header: push the heading face
            // for Begin only, so the body draws on the current face.
            ui::WindowTitleFont title_font{};
            const bool visible = ImGui::Begin(
                "Performance", &state.m_performance_open, ImGuiWindowFlags_NoFocusOnAppearing);
            title_font.close();
            if (visible) {
                // The window-level orientation note keeps the tooltip it had
                // on the old heading row, but now rides its own marker row so
                // the title alone reads as the header.
                ui::helpMarker(
                    "Inter-frame cadence (never GPU or present time), the game UI lane split, and completed GPU "
                    "timing. Sampling never drives frame admission. Hover a graph or section marker for its detail.");

                // The three histories are the window's default read: drawn first
                // and unconditionally, so opening Performance shows them without
                // expanding or scrolling anything.
                if (view.m_performance != nullptr) {
                    view.m_performance->drawContents();
                }

                drawPerformance_(view);
            }

            ImGui::End();
        }

        float drawToolRail_(
            const std::span<const ColonyShortcut> shortcuts, ColonyUiState &state, std::optional<ColonyIntent> &intent,
            const float top) {
            const auto &io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            const float width = std::min(fs * 2.875f, io.DisplaySize.x);
            const float height = std::max(
                1.0f, std::min(fs * 9.5f + ImGui::GetStyle().ItemSpacing.y * 3.0f, io.DisplaySize.y - top));
            ImGui::SetNextWindowPos(ImVec2{0.0f, top}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2{width, height}, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 0.25f, fs * 0.25f});
            ImGui::Begin(
                "##tool_rail", nullptr,
                kChromeFlags & ~(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));

            for (const auto tool:
                {EColonyTool::inspect, EColonyTool::dig, EColonyTool::build_wall, EColonyTool::demolish}) {
                const bool selected = state.m_tool == tool;
                const auto icon = tool == EColonyTool::inspect      ? ui::icons::kInspectLabel
                                  : tool == EColonyTool::dig        ? ui::icons::kDigLabel
                                  : tool == EColonyTool::build_wall ? ui::icons::kCubesLabel
                                                                    : ui::icons::kDemolishLabel;
                char label[48]{};
                std::snprintf(label, sizeof(label), "%.*s###tool%u", 3, icon.data(), static_cast<u32>(tool));
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ui::theme::kAccent.vec4());
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::theme::kAccentHover.vec4());
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::theme::kAccentActive.vec4());
                }

                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected ? 2.0f : 1.0f);
                const bool pressed = ImGui::Button(label, ImVec2{fs * 2.25f, fs * 2.25f});
                ImGui::PopStyleVar();
                if (selected) {
                    ImGui::PopStyleColor(3);
                }

                const auto kind = tool == EColonyTool::dig          ? EColonyIntent::toggle_dig
                                  : tool == EColonyTool::build_wall ? EColonyIntent::open_build_menu
                                                                    : EColonyIntent::select_tool;
                const u32 argument = kind == EColonyIntent::select_tool ? static_cast<u32>(tool) : 0u;
                shortcutTip(shortcuts, kind, argument);
                if (ImGui::IsItemHovered() and shortcuts.empty()) {
                    ImGui::SetTooltip("%s", tool == EColonyTool::build_wall ? "Build" : toolName(tool));
                }

                if (pressed) {
                    intent = ColonyIntent{.m_arg = argument, .m_kind = kind};
                }
            }

            // Both the visible Build button and B request this popup in its owning window.
            if (state.m_build_menu_open) {
                state.m_build_menu_open = false;
                ImGui::OpenPopup("Build tools");
            }

            if (ImGui::BeginPopup("Build tools")) {
                if (ImGui::MenuItem("Wall")) {
                    intent = ColonyIntent{
                        .m_arg = static_cast<u32>(EColonyTool::build_wall), .m_kind = EColonyIntent::select_tool};
                }

                if (ImGui::MenuItem("Demolish")) {
                    intent = ColonyIntent{
                        .m_arg = static_cast<u32>(EColonyTool::demolish), .m_kind = EColonyIntent::select_tool};
                }

                ImGui::EndPopup();
            }

            ImGui::End();
            ImGui::PopStyleVar();
            return width;
        }

        /// Reopenable help window: every key row comes from the binding
        /// table, and camera/tool copy names a key only by reading that same
        /// table (through `boundKey`), so Help cannot drift from the actual
        /// bindings. The first-run block stays until dismissed and lives for
        /// this session only; the window itself reopens from Menu > Help
        /// or F1 whenever the player wants the reference back.
        void drawHelpWindow_(
            const std::span<const ColonyShortcut> shortcuts, ColonyUiState &state,
            std::optional<ColonyIntent> &intent) {
            if (not state.m_help_open) {
                return;
            }

            const float fs = ImGui::GetFontSize();
            game::ui::prepareUtilityWindow(fs * 42.0f, fs * 36.0f);
            ImGui::SetNextWindowBgAlpha(ui::theme::kFloatingPanelAlpha);
            // The title bar is the panel's one header: push the heading face
            // for Begin only, so the body draws on the current face.
            ui::WindowTitleFont title_font{};
            ImGui::Begin("Help", nullptr, ImGuiWindowFlags_NoDocking);
            title_font.close();

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
                    "Reopen this window any time: Menu > Help, or %s.", boundKey(shortcuts, EColonyIntent::help, 0u));
                if (ImGui::SmallButton("Got it")) {
                    state.m_intro_dismissed = true;
                }
                ImGui::Spacing();
            }

            ImGui::SeparatorText("Camera");
            const float label_width = labelColumnWidth();
            if (beginLabelTable("##help_camera", label_width)) {
                textRow("MMB drag", "pan the camera");
                textRow("Wheel", "zoom in and out");
                textRow("Focus", "Inspector: jump to the selected colonist and follow it");
                textRow("Stop following", "colony list, or Esc: hand the camera back");
                textRow("Find colonist", "selects only - press Focus to follow");
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Tools");
            const EColonyTool tools[4]{
                EColonyTool::inspect, EColonyTool::dig, EColonyTool::build_wall, EColonyTool::demolish};
            if (beginLabelTable("##help_tools", label_width)) {
                for (const EColonyTool tool: tools) {
                    textRow(toolName(tool), gestureText(tool));
                }

                ImGui::EndTable();
            }

            char help_note[96]{};
            std::snprintf(
                help_note, sizeof(help_note), "%s opens the build menu (Wall, Demolish).",
                boundKey(shortcuts, EColonyIntent::open_build_menu, 0u));
            ui::helpMarker(help_note);

            ImGui::Spacing();
            ImGui::SeparatorText("Keyboard");
            if (beginLabelTable("##help_keyboard", label_width)) {
                for (const ColonyShortcut &row: shortcuts) {
                    textRow(row.m_key, row.m_tip);
                }

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Mouse");
            if (beginLabelTable("##help_mouse", label_width)) {
                textRow("LMB", "uses the active tool (Inspect pins, Dig digs)");
                ImGui::EndTable();
            }

            ImGui::Spacing();
            if (ImGui::Button("Close")) {
                intent = ColonyIntent{.m_kind = EColonyIntent::help};
            }
            ImGui::End();
        }

        /// Keep draft synchronization and game action policy at the caller boundary.
        void drawDisplayWindow_(
            const ColonyDisplaySnapshot &snap, ColonyUiState &state, std::optional<ColonyIntent> &intent) {
            ColonyDisplaySession &session = state.m_display;
            const bool trial_open = game::displayTrialUnresolved(session.m_confirm);
            const auto confirmation = ui::drawDisplayConfirmation(
                trial_open, session.m_confirm.m_armed, session.m_confirm.m_revert_pending,
                session.m_confirm.m_countdown_active, session.m_confirm.m_seconds_left, session.m_revert_failed,
                session.m_last_error);
            if (not state.m_display_open and not trial_open) {
                return;
            }
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
                .m_retro = {state.m_retro.m_enabled, state.m_retro.m_pixel_scale},
                .m_matches = game::displayDraftMatches(draft, snap.m_view.m_committed),
                .m_availability = {availability.m_supported, availability.m_reason},
                .m_trial_open = game::displayTrialUnresolved(session.m_confirm),
                .m_confirm_armed = session.m_confirm.m_armed,
                .m_revert_pending = session.m_confirm.m_revert_pending,
                .m_countdown_active = session.m_confirm.m_countdown_active,
                .m_seconds_left = session.m_confirm.m_seconds_left,
                .m_revert_failed = session.m_revert_failed,
                .m_last_error = session.m_last_error};
            const bool was_open = state.m_display_open;
            auto action = state.m_display_open ? ui::drawDisplayWidget(snap, widget, state.m_display_open)
                                               : std::optional<ui::EDisplayWidgetAction>{};
            if (was_open and not state.m_display_open and action == ui::EDisplayWidgetAction::close) {
                session.m_synced = false;
                action.reset();
            }

            if (confirmation) {
                action = confirmation;
            }

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

    // ------------------------------------------------------------------
    // colony ui
    // ------------------------------------------------------------------

    ColonyUiFrame drawColonyUi(
        const ColonyUiView &view, const ColonyDisplaySnapshot &display, const std::span<const ColonyShortcut> shortcuts,
        ColonyUiState &state) {
        std::optional<ColonyIntent> intent{};

        const hal::ProfileScope scope{"Game.UI.Draw"};
        PPR_ASSERT(view.m_colony != nullptr);
        const ColonyPresentation &colony = *view.m_colony;
        const float bar_height = drawTopBar_(colony, shortcuts, intent);
        const float rail_width = drawToolRail_(shortcuts, state, intent, bar_height);
        drawColonySummary_(colony, view, shortcuts, state, intent, bar_height, rail_width);
        drawPerformanceWindow_(view, state, bar_height);
        drawHelpWindow_(shortcuts, state, intent);
        drawDisplayWindow_(display, state, intent);
        // Drawn last so the cursor read-out stays on top of the other panels;
        // it is input-transparent, so stacking above them costs no interaction.
        drawInspectorWindow_(colony, view, state);

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
        // Finite windows use native capture; only the full-width ribbon reserves an edge.

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
