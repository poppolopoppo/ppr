module;

module engine.app;
import :display.settings;
import :ui.imgui;
import :ui.widgets.display;
import engine.core;
import imgui;
import std;

namespace pP::ui {
    namespace {
        // ------------------------------------------------------------------
        // widget helpers
        // ------------------------------------------------------------------

        struct DisplayWindowScope {
            ~DisplayWindowScope() noexcept {
                ImGui::End();
            }
        };

        struct DisplayDisabledScope {
            bool active{true};
            explicit DisplayDisabledScope(const bool disabled) {
                ImGui::BeginDisabled(disabled);
            }
            void close() noexcept {
                if (active) {
                    active = false;
                    ImGui::EndDisabled();
                }
            }
            ~DisplayDisabledScope() noexcept {
                close();
            }
        };

        struct DisplayComboScope {
            ~DisplayComboScope() noexcept {
                ImGui::EndCombo();
            }
        };

        constexpr ImGuiWindowFlags kChromeFlags = ImGuiWindowFlags_NoTitleBar |
                                                  ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                                  ImGuiWindowFlags_NoDocking;

        // ------------------------------------------------------------------
        // graphite presentation
        // ------------------------------------------------------------------

        /// Filled-accent primary action. The accent fill is always paired
        /// with its dark text token; secondary actions stay neutral.
        [[nodiscard]] bool primaryButton(const char *const label) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent.vec4());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover.vec4());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive.vec4());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccentText.vec4());
            const bool pressed = ImGui::Button(label);
            ImGui::PopStyleColor(4);
            return pressed;
        }

        /// Every label rendered in a Display label/value table, in section
        /// order. The shared label column is measured from this list; keep
        /// the list in sync when a row label changes.
        constexpr const char *const kDisplayTableLabels[] = {
            // Picture:
            "Pace", "Frame cap (Hz)", "Vsync in paced mode", "Slow down when unfocused", "Monitor cap",
            // Window:
            "Mode", "Monitor", "Width", "Height", "Resolution", "Refresh",
            // Retro:
            "Pixel art", "Pixel scale",
            // Requested:
            "Cap", "Size", "Vsync", "Foreground CPU target", "Effective CPU target", "Unfocused slowing",
            // Device report:
            "Picture device", "Change in flight", "Last problem", "Window", "Observed window", "Observed exclusive"};

        /// Shared label-column width: the widest label plus cell padding, so
        /// labels and values align across all Display tables.
        [[nodiscard]] float displayLabelColumnWidth() {
            float width = 0.0f;
            for (const char *const label: kDisplayTableLabels) {
                width = std::max(width, ImGui::CalcTextSize(label).x);
            }

            return std::min(width + ImGui::GetStyle().CellPadding.x * 2.0f, ImGui::GetContentRegionAvail().x * 0.42f);
        }

        /// Begin one two-column Display table with the shared label column.
        [[nodiscard]] bool beginLabelTable(const char *const id, const float label_width) {
            if (not ImGui::BeginTable(id, 2)) {
                return false;
            }

            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        /// One secondary-text label in the shared label column, aligned with
        /// framed values.
        void labelCell(const char *const label) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary.vec4());
            ImGui::TextWrapped("%s", label);
            ImGui::PopStyleColor();
        }

        /// Open a label/value row and render its label; the caller continues
        /// in the value cell.
        void beginValueRow(const char *const label) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            labelCell(label);
            ImGui::TableNextColumn();
        }

        /// A complete label/value text row. Unavailable values read as
        /// disabled; text wraps inside the value column.
        void textRow(const char *const label, const char *const value, const bool available = true) {
            beginValueRow(label);
            ImGui::AlignTextToFramePadding();
            if (not available) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            ImGui::TextWrapped("%s", value);
            if (not available) {
                ImGui::PopStyleColor();
            }
        }

        /// Disabled note text that wraps inside the current cell or window
        /// (TextDisabled does not wrap).
        void noteText(const char *const text) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }

        void continueActionRow(const char *const next_label) {
            const float width = ImGui::CalcTextSize(next_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float remaining = ImGui::GetCursorScreenPos().x +
                                    ImGui::GetContentRegionAvail().x -
                                    ImGui::GetItemRectMax().x;
            if (remaining >= width + ImGui::GetStyle().ItemSpacing.x) {
                ImGui::SameLine();
            }
        }

        /// Footer height for the pinned action/confirmation/error area, so
        /// the scrollable body never covers it. Wrapped overflow beyond
        /// these estimates stays reachable through the window scrollbar.
        [[nodiscard]] float displayFooterReserve(const bool confirm_visible, const bool error_visible) {
            const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            float rows = 1.0f;
            float used = 0.0f;
            for (const char *const label: {"Apply", "Reset to startup", "Save to file", "Close"}) {
                const float width = ImGui::CalcTextSize(label).x + 2.0f * ImGui::GetStyle().FramePadding.x;
                if (used > 0.0f and used + ImGui::GetStyle().ItemSpacing.x + width > available) {
                    ++rows;
                    used = 0.0f;
                }

                used += width + (used > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
            }

            float reserve = rows * ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
            if (confirm_visible) {
                reserve += 3.0f * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
            }
            if (error_visible) {
                reserve += 3.0f * ImGui::GetTextLineHeightWithSpacing();
            }
            return reserve;
        }

        // ------------------------------------------------------------------
        // display domain
        // ------------------------------------------------------------------

        [[nodiscard]] std::string_view pacingName(const EFramePacing value) noexcept {
            for (const auto &entry: pacing_domain.m_entries) {
                if (entry.m_value == value) {
                    return entry.m_name;
                }
            }
            return {};
        }
        [[nodiscard]] std::string_view windowModeName(const EDisplayWindowMode value) noexcept {
            for (const auto &entry: window_mode_domain.m_entries) {
                if (entry.m_value == value) {
                    return entry.m_name;
                }
            }
            return {};
        }
        [[nodiscard]] std::string_view displayLabel(const std::string_view canonical) {
            std::string_view out{};
            DisplaySettings probe{};
            config::visit(probe, [&](const auto &member, const auto &) {
                if (member.m_metadata.m_name == canonical) {
                    out = member.m_metadata.m_description;
                }
            });
            return out;
        }

        /// Cap range text from the schema Range ([1, 1000]); falls back to
        /// the literal only if the schema ever stops describing the field.
        [[nodiscard]] std::string displayCapRange() {
            std::string out{"[1, 1000]"};
            DisplaySettings probe{};
            config::visit(probe, [&](const auto &member, const auto &) {
                if (member.m_metadata.m_name != "display.cap_hz") {
                    return;
                }
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(member.m_constraint)>, config::Range<u32>>) {
                    out = member.m_constraint.describe();
                }
            });
            return out;
        }

        /// One CPU target for the telemetry block: "no cap" when uncapped,
        /// otherwise milliseconds. Never a frame rate, never a present count.
        void cpuTargetText(char *const buffer, const std::size_t size, const std::optional<TimeSpan> &target) {
            if (not target.has_value()) {
                std::snprintf(buffer, size, "no cap");
                return;
            }
            std::snprintf(
                buffer, size, "%.2f ms",
                static_cast<double>(std::chrono::duration<float, std::milli>(*target).count()));
        }

        [[nodiscard]] const char *transactionWords(const EDisplayTransaction state) noexcept {
            switch (state) {
                case EDisplayTransaction::idle: return "idle";
                case EDisplayTransaction::unsubmitted: return "waiting to send";
                case EDisplayTransaction::awaiting_ack: return "waiting for the picture device";
                case EDisplayTransaction::restore_unsubmitted: return "sending the restore";
                case EDisplayTransaction::restore_awaiting_ack: return "waiting for the restore";
                case EDisplayTransaction::degraded: return "needs attention";
            }
            return "idle";
        }

        /// Reported monitor the cap action reads: the primary one, else the
        /// first enumerated. Null when the service reported nothing.
        [[nodiscard]] const DisplayWidgetMonitor *capMonitor(const DisplayWidgetSnapshot &snap) noexcept {
            if (snap.m_monitors.empty()) {
                return nullptr;
            }
            for (const DisplayWidgetMonitor &monitor: snap.m_monitors) {
                if (monitor.m_primary) {
                    return &monitor;
                }
            }
            return &snap.m_monitors.front();
        }

        /// Capability entry behind the draft's monitor preference: "auto"
        /// reads the primary monitor, anything else needs an exact reported
        /// name. Null when the preference names nothing reported (never
        /// enumerated, or unplugged since).
        [[nodiscard]] const DisplayMonitorCapability *switchMonitor_(
            const DisplayWidgetSnapshot &snap, const std::string &preference) noexcept {
            if (snap.m_capabilities == nullptr) {
                return nullptr;
            }
            return findDisplayMonitor(preference, *snap.m_capabilities).m_monitor;
        }

        /// Distinct sorted exclusive resolutions reported for one monitor.
        [[nodiscard]] std::vector<std::pair<u32, u32>> switchResolutions_(
            const DisplayMonitorCapability *const monitor) {
            std::vector<std::pair<u32, u32>> out{};
            if (monitor == nullptr) {
                return out;
            }
            for (const DisplayModeCapability &mode: monitor->m_modes) {
                const std::pair<u32, u32> size{mode.m_width, mode.m_height};
                if (std::ranges::find(out, size) == out.end()) {
                    out.push_back(size);
                }
            }
            std::ranges::sort(out);
            return out;
        }

        /// Distinct sorted known refreshes for one resolution on one
        /// monitor. Zero dimensions read the monitor's current mode. Unknown
        /// stays unknown: no value is invented here.
        [[nodiscard]] std::vector<u32> switchRefreshes_(
            const DisplayMonitorCapability *const monitor, u32 width, u32 height) {
            std::vector<u32> out{};
            if (monitor == nullptr) {
                return out;
            }
            if (width == 0u or height == 0u) {
                width = monitor->m_current.m_width;
                height = monitor->m_current.m_height;
            }
            for (const DisplayModeCapability &mode: monitor->m_modes) {
                if (mode.m_width != width or mode.m_height != height or not mode.m_refresh_hz.has_value()) {
                    continue;
                }
                if (std::ranges::find(out, *mode.m_refresh_hz) == out.end()) {
                    out.push_back(*mode.m_refresh_hz);
                }
            }
            std::ranges::sort(out);
            return out;
        }

        /// Clamp helper for the windowed size inputs (native geometry range).
        [[nodiscard]] int clampDisplayExtent(const int value) noexcept {
            if (value < 0) {
                return 0;
            }
            if (value > 2147483647) {
                return 2147483647;
            }
            return value;
        }

    }

    // ------------------------------------------------------------------
    // display widget
    // ------------------------------------------------------------------

    /// Dedicated Display window (help-window precedent: centered, fixed
    /// chrome, reopenable). Edits land in the session draft only; every
    /// applied change leaves as a typed intent the caller submits through
    /// the display transaction path. Requested policy and the device
    /// report stay in separate sections; unavailable measurements read as
    /// unavailable. The body scrolls and action/error content stays visible.
    std::optional<EDisplayWidgetAction> drawDisplayWidget(
        const DisplayWidgetSnapshot &snap, DisplayWidgetState state, bool &open) {
        std::optional<EDisplayWidgetAction> intent{};
        const ImGuiIO &io = ImGui::GetIO();
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(
            ImVec2{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_FirstUseEver, ImVec2{0.5f, 0.5f});
        ImGui::SetNextWindowSize(
            ImVec2{
                std::min(std::max(1.0f, io.DisplaySize.x - fs * 1.5f), fs * 46.0f),
                std::min(std::max(1.0f, io.DisplaySize.y - fs * 1.5f), fs * 42.0f)},
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2{std::min(io.DisplaySize.x, fs * 24.0f), std::min(io.DisplaySize.y, fs * 18.0f)},
            ImVec2{io.DisplaySize.x, io.DisplaySize.y});
        ImGui::SetNextWindowBgAlpha(ui::theme::kFloatingPanelAlpha);
        ui::WindowTitleFont title_font{};
        const bool visible = ImGui::Begin("Display", &open, ImGuiWindowFlags_NoCollapse);
        title_font.close();
        const DisplayWindowScope window_scope{};
        if (not visible) {
            if (not open) {
                return EDisplayWidgetAction::close;
            }

            return std::nullopt;
        }

        const DisplaySettings &committed = snap.m_view.m_committed;
        const bool matches = state.m_matches;
        const float label_width = displayLabelColumnWidth();

        // The body owns the scrolling: everything that grows (sections,
        // tables, notes) stays reachable, while the footer below keeps the
        // actions, confirmation and error content visible.
        const float footer_reserve = displayFooterReserve(false, not state.m_last_error.empty());
        const ImVec2 body_size{0.0f, std::max(4.0f * fs, ImGui::GetContentRegionAvail().y - footer_reserve)};
        ImGui::BeginChild("##body", body_size);

        ImGui::SeparatorText("Picture");
        if (beginLabelTable("##display_picture", label_width)) {
            {
                beginValueRow("Pace");
                const std::string_view current = pacingName(state.m_draft.m_pacing);
                const char *preview = current.empty() ? "?" : current.data();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##pace", preview)) {
                    const DisplayComboScope combo_scope{};
                    for (const auto &entry: pacing_domain.m_entries) {
                        const bool selected = state.m_draft.m_pacing == entry.m_value;
                        if (ImGui::Selectable(entry.m_name.data(), selected)) {
                            state.m_draft.m_pacing = entry.m_value;
                        }
                    }
                }
                ImGui::SetItemTooltip("%s\n%s", "display.pacing", displayLabel("display.pacing").data());
            }

            {
                const bool paced = state.m_draft.m_pacing == EFramePacing::paced;
                DisplayDisabledScope cap_disabled{snap.m_has_pending or not paced};
                beginValueRow("Frame cap (Hz)");
                int cap = state.m_draft.m_cap_hz > 100000u ? 100000 : static_cast<int>(state.m_draft.m_cap_hz);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##cap", &cap, 1, 10)) {
                    if (cap < 0) {
                        cap = 0;
                    }
                    if (cap > 100000) {
                        cap = 100000;
                    }
                    state.m_draft.m_cap_hz = static_cast<u32>(cap);
                }
                cap_disabled.close();
                ImGui::SetItemTooltip(
                    "%s\n%s %s%s", "display.cap_hz", displayLabel("display.cap_hz").data(), displayCapRange().c_str(),
                    paced ? "" : " (paced mode only)");
            }

            {
                const bool paced = state.m_draft.m_pacing == EFramePacing::paced;
                DisplayDisabledScope vsync_disabled{snap.m_has_pending or not paced};
                beginValueRow("Vsync in paced mode");
                ImGui::Checkbox("##vsync", &state.m_draft.m_paced_vsync);
                vsync_disabled.close();
                ImGui::SetItemTooltip(
                    "%s\n%s%s", "display.paced_vsync", displayLabel("display.paced_vsync").data(),
                    paced ? "" : " (paced mode only)");
            }

            DisplayDisabledScope throttle_disabled{snap.m_has_pending};
            beginValueRow("Slow down when unfocused");
            ImGui::Checkbox("##throttle", &state.m_draft.m_throttle_unfocused);
            throttle_disabled.close();
            ImGui::SetItemTooltip(
                "%s\n%s", "display.throttle_unfocused", displayLabel("display.throttle_unfocused").data());

            {
                const DisplayWidgetMonitor *monitor = capMonitor(snap);
                const bool known = monitor != nullptr and monitor->m_refresh_hz > 0;
                DisplayDisabledScope monitor_cap_disabled{not known or snap.m_has_pending};
                beginValueRow("Monitor cap");
                char cap_label[48]{"Monitor refresh unknown"};
                if (known) {
                    std::snprintf(cap_label, sizeof(cap_label), "Use %d Hz as cap", monitor->m_refresh_hz);
                }
                if (ImGui::Button(cap_label)) {
                    intent = EDisplayWidgetAction::use_monitor_cap;
                }
                monitor_cap_disabled.close();
                ImGui::SetItemTooltip("Draft paced mode with the cap from the reported monitor refresh.");
            }

            ImGui::EndTable();
        }

        ImGui::SeparatorText("Window");
        const DisplayWidgetAvailability &availability = state.m_availability;
        const DisplayMonitorCapability *switch_monitor = switchMonitor_(snap, state.m_draft.m_monitor);
        if (beginLabelTable("##display_window", label_width)) {
            DisplayDisabledScope window_disabled{snap.m_has_pending or not availability.m_supported};
            {
                beginValueRow("Mode");
                const std::string_view current = windowModeName(state.m_draft.m_window_mode);
                const char *preview = current.empty() ? "?" : current.data();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##mode", preview)) {
                    const DisplayComboScope combo_scope{};
                    for (const auto &entry: window_mode_domain.m_entries) {
                        const bool selected = state.m_draft.m_window_mode == entry.m_value;
                        if (ImGui::Selectable(entry.m_name.data(), selected)) {
                            state.m_draft.m_window_mode = entry.m_value;
                            if (entry.m_value == EDisplayWindowMode::windowed) {
                                state.m_draft.m_refresh_hz = 0u;
                            }
                        }
                    }
                }
                ImGui::SetItemTooltip("%s\n%s", "display.window_mode", displayLabel("display.window_mode").data());
            }

            {
                const bool listed = snap.m_capabilities != nullptr and not snap.m_capabilities->m_monitors.empty();
                const DisplayDisabledScope monitor_disabled{not listed};
                beginValueRow("Monitor");
                const char *preview = state.m_draft.m_monitor == "auto"
                                          ? "Automatic"
                                          : (switch_monitor != nullptr ? switch_monitor->m_name.c_str()
                                                                       : "Unavailable monitor");
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##monitor", preview)) {
                    const DisplayComboScope combo_scope{};
                    if (ImGui::Selectable("Automatic", state.m_draft.m_monitor == "auto")) {
                        state.m_draft.m_monitor = "auto";
                    }
                    if (snap.m_capabilities != nullptr) {
                        int ordinal = 0;
                        for (const DisplayMonitorCapability &monitor: snap.m_capabilities->m_monitors) {
                            ImGui::PushID(ordinal++);
                            const bool selected = switch_monitor == &monitor and state.m_draft.m_monitor != "auto";
                            const bool ambiguous_legacy = monitor.m_identity.empty() and
                                                          std::ranges::count_if(
                                                              snap.m_capabilities->m_monitors,
                                                              [&](const DisplayMonitorCapability &other) {
                                                                  return other.m_name == monitor.m_name;
                                                              }) > 1;
                            const DisplayDisabledScope identity_disabled{ambiguous_legacy};
                            char label[256];
                            std::snprintf(
                                label, sizeof(label), "%s (%d: %u x %u, %u Hz)", monitor.m_name.c_str(), ordinal,
                                monitor.m_current.m_width, monitor.m_current.m_height,
                                monitor.m_current.m_refresh_hz.value_or(0u));
                            if (ImGui::Selectable(label, selected)) {
                                state.m_draft.m_monitor = monitor.m_identity.empty() ? monitor.m_name
                                                                                     : monitor.m_identity;
                            }
                            ImGui::SetItemTooltip(
                                "%s", monitor.m_identity.empty()
                                          ? "Persistent identity unavailable; only unique names can be selected."
                                          : monitor.m_identity.c_str());
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::SetItemTooltip("%s\n%s", "display.monitor", displayLabel("display.monitor").data());
                if (not listed and availability.m_supported) {
                    noteText("No monitors reported; staying on Automatic.");
                }
            }

            if (state.m_draft.m_window_mode == EDisplayWindowMode::windowed) {
                if (state.m_draft.m_refresh_hz != 0u) {
                    state.m_draft.m_refresh_hz = 0u;
                }

                beginValueRow("Width");
                int width = clampDisplayExtent(
                    state.m_draft.m_width > 2147483647u ? 2147483647 : static_cast<int>(state.m_draft.m_width));
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##width", &width, 16, 160)) {
                    state.m_draft.m_width = static_cast<u32>(clampDisplayExtent(width));
                }
                ImGui::SetItemTooltip("%s\n%s", "display.width", displayLabel("display.width").data());

                beginValueRow("Height");
                int height = clampDisplayExtent(
                    state.m_draft.m_height > 2147483647u ? 2147483647 : static_cast<int>(state.m_draft.m_height));
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##height", &height, 16, 160)) {
                    state.m_draft.m_height = static_cast<u32>(clampDisplayExtent(height));
                }
                ImGui::SetItemTooltip("%s\n%s", "display.height", displayLabel("display.height").data());
                noteText("Zero keeps the current window size.");

                DisplayDisabledScope window_refresh_disabled{true};
                beginValueRow("Refresh");
                char refresh_preview[] = "Platform default";
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##refresh", refresh_preview)) {
                    const DisplayComboScope combo_scope{};
                }
                window_refresh_disabled.close();
                ImGui::SetItemTooltip("%s\n%s", "display.refresh_hz", displayLabel("display.refresh_hz").data());
                noteText("Refresh applies to fullscreen only.");
            } else {
                const std::vector<std::pair<u32, u32>> resolutions = switchResolutions_(switch_monitor);
                {
                    DisplayDisabledScope resolution_disabled{switch_monitor == nullptr or resolutions.empty()};
                    beginValueRow("Resolution");
                    char size_preview[32]{"Monitor default"};
                    if (state.m_draft.m_width != 0u or state.m_draft.m_height != 0u) {
                        std::snprintf(
                            size_preview, sizeof(size_preview), "%u x %u", state.m_draft.m_width,
                            state.m_draft.m_height);
                    }
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##resolution", size_preview)) {
                        const DisplayComboScope combo_scope{};
                        const bool is_default = state.m_draft.m_width == 0u and state.m_draft.m_height == 0u;
                        if (ImGui::Selectable("Monitor default", is_default)) {
                            state.m_draft.m_width = 0u;
                            state.m_draft.m_height = 0u;
                        }
                        for (const auto &[mode_w, mode_h]: resolutions) {
                            char mode_label[32]{};
                            std::snprintf(mode_label, sizeof(mode_label), "%u x %u", mode_w, mode_h);
                            const bool selected = state.m_draft.m_width == mode_w and state.m_draft.m_height == mode_h;
                            if (ImGui::Selectable(mode_label, selected)) {
                                state.m_draft.m_width = mode_w;
                                state.m_draft.m_height = mode_h;
                            }
                        }
                    }
                    resolution_disabled.close();
                    ImGui::SetItemTooltip("%s\n%s", "display.width", displayLabel("display.width").data());
                    if (switch_monitor == nullptr) {
                        noteText("The chosen monitor is not in the reported list.");
                    } else if (resolutions.empty()) {
                        noteText("No modes reported for this monitor; explicit sizes cannot be checked.");
                    } else {
                        noteText("Default uses the monitor's current mode; a size must match a listed mode.");
                    }
                }

                const std::vector<u32> refreshes = switchRefreshes_(
                    switch_monitor, state.m_draft.m_width, state.m_draft.m_height);
                const bool refreshable = snap.m_capabilities != nullptr and
                                         snap.m_capabilities->m_exclusive_refresh and
                                         not refreshes.empty();
                if (not refreshable and state.m_draft.m_refresh_hz != 0u) {
                    state.m_draft.m_refresh_hz = 0u;
                }

                DisplayDisabledScope refresh_disabled{not refreshable};
                beginValueRow("Refresh");
                char refresh_preview[32]{"Platform default"};
                if (state.m_draft.m_refresh_hz != 0u) {
                    std::snprintf(refresh_preview, sizeof(refresh_preview), "%u Hz", state.m_draft.m_refresh_hz);
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##refresh", refresh_preview)) {
                    const DisplayComboScope combo_scope{};
                    if (ImGui::Selectable("Platform default", state.m_draft.m_refresh_hz == 0u)) {
                        state.m_draft.m_refresh_hz = 0u;
                    }
                    for (const u32 refresh: refreshes) {
                        char refresh_label[32]{};
                        std::snprintf(refresh_label, sizeof(refresh_label), "%u Hz", refresh);
                        if (ImGui::Selectable(refresh_label, state.m_draft.m_refresh_hz == refresh)) {
                            state.m_draft.m_refresh_hz = refresh;
                        }
                    }
                }
                refresh_disabled.close();
                ImGui::SetItemTooltip("%s\n%s", "display.refresh_hz", displayLabel("display.refresh_hz").data());
                if (snap.m_capabilities != nullptr and not snap.m_capabilities->m_exclusive_refresh) {
                    noteText("Explicit refresh is unsupported; using the platform default.");
                } else if (refreshes.empty() and availability.m_supported) {
                    noteText("No refresh values reported; using the platform default.");
                }
            }

            window_disabled.close();
            ImGui::EndTable();
        }
        if (not availability.m_supported) {
            noteText(availability.m_reason != nullptr ? availability.m_reason : "Display switching unavailable.");
            if (snap.m_capabilities == nullptr and snap.m_capabilities_error.value() != 0) {
                char failure[256]{};
                std::snprintf(
                    failure, sizeof(failure), "(monitor list failed: %s #%d)",
                    snap.m_capabilities_error.message().c_str(), snap.m_capabilities_error.value());
                noteText(failure);
            }
        }

        // Retro is a live session control: the game pushes these values to the
        // editor every frame, so this section never edits the display draft,
        // emits a widget action, or joins the apply/save transaction.
        ImGui::SeparatorText("Retro");
        if (beginLabelTable("##display_retro", label_width)) {
            {
                beginValueRow("Pixel art");
                ImGui::Checkbox("##retro_enabled", &state.m_retro.m_enabled);
                ImGui::SetItemTooltip("Render the scene in the retro pixel-art style.");
            }

            {
                DisplayDisabledScope scale_disabled{not state.m_retro.m_enabled};
                beginValueRow("Pixel scale");
                int scale = static_cast<int>(std::clamp(state.m_retro.m_pixel_scale, 2u, 8u));
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##retro_scale", &scale, 2, 8)) {
                    state.m_retro.m_pixel_scale = static_cast<u32>(scale);
                }
                scale_disabled.close();
                ImGui::SetItemTooltip("Pixel scale of the retro picture, from 2 to 8.");
            }

            ImGui::EndTable();
        }
        noteText(
            "The scene renders at a lower internal resolution with a 32-color palette and ordered "
            "dithering; the UI stays native. Changes apply immediately and are not part of Apply or Save.");

        ImGui::SeparatorText("Requested");
        if (beginLabelTable("##display_requested", label_width)) {
            {
                const std::string_view pacing = pacingName(committed.m_pacing);
                textRow("Pace", pacing.empty() ? "?" : pacing.data());
            }

            {
                char cap_text[48]{};
                if (committed.m_pacing == EFramePacing::paced) {
                    std::snprintf(cap_text, sizeof(cap_text), "%u Hz", committed.m_cap_hz);
                    textRow("Cap", cap_text);
                } else {
                    std::snprintf(cap_text, sizeof(cap_text), "%u Hz (paced mode only)", committed.m_cap_hz);
                    textRow("Cap", cap_text, false);
                }
            }

            {
                const std::string_view mode = windowModeName(committed.m_window_mode);
                textRow("Mode", mode.empty() ? "?" : mode.data());
            }

            textRow("Monitor", committed.m_monitor == "auto" ? "automatic" : committed.m_monitor.c_str());

            {
                char size_text[48]{};
                if (committed.m_width != 0u and committed.m_height != 0u) {
                    std::snprintf(size_text, sizeof(size_text), "%u x %u", committed.m_width, committed.m_height);
                    textRow("Size", size_text);
                } else {
                    textRow("Size", "default (current mode or window size)", false);
                }
            }

            {
                char refresh_text[32]{};
                if (committed.m_refresh_hz != 0u) {
                    std::snprintf(refresh_text, sizeof(refresh_text), "%u Hz", committed.m_refresh_hz);
                    textRow("Refresh", refresh_text);
                } else {
                    textRow("Refresh", "platform default", false);
                }
            }

            textRow("Vsync", snap.m_view.m_vsync ? "on (requested)" : "off (requested)");

            {
                char foreground[32]{};
                char effective[32]{};
                cpuTargetText(foreground, sizeof(foreground), snap.m_view.m_foreground);
                cpuTargetText(effective, sizeof(effective), snap.m_view.m_effective);
                textRow("Foreground CPU target", foreground);
                textRow("Effective CPU target", effective);
            }

            if (not committed.m_throttle_unfocused) {
                textRow("Unfocused slowing", "off (setting disabled)");
            } else if (snap.m_view.m_background_priority) {
                textRow("Unfocused slowing", "on (window in background, slowing now)");
            } else {
                textRow("Unfocused slowing", "on (window focused)");
            }

            ImGui::EndTable();
        }

        ImGui::SeparatorText("Device report");
        if (beginLabelTable("##display_device", label_width)) {
            {
                char device[128]{};
                if (snap.m_applied.m_available) {
                    std::snprintf(
                        device, sizeof(device), "%dx%d, vsync %s, change #%llu", snap.m_applied.m_width,
                        snap.m_applied.m_height, snap.m_applied.m_vsync ? "on" : "off",
                        static_cast<unsigned long long>(snap.m_applied.m_generation));
                    textRow("Picture device", device);
                } else {
                    textRow("Picture device", "not reported yet", false);
                }
            }

            {
                if (snap.m_has_pending) {
                    char flight[64]{};
                    std::snprintf(
                        flight, sizeof(flight), "%s (#%llu)", transactionWords(snap.m_transaction),
                        static_cast<unsigned long long>(snap.m_pending_id));
                    textRow("Change in flight", flight);
                    noteText("A queued change is not applied until the device answers.");
                } else {
                    textRow("Change in flight", "none", false);
                }
            }

            {
                if (snap.m_error.value() != 0) {
                    char problem[512]{};
                    std::snprintf(
                        problem, sizeof(problem), "%s (%s #%d)", snap.m_error.message().c_str(),
                        snap.m_error.category().name(), snap.m_error.value());
                    textRow("Last problem", problem);
                } else {
                    textRow("Last problem", "none", false);
                }
            }

            beginValueRow("Monitor");
            ImGui::AlignTextToFramePadding();
            if (snap.m_monitors.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("not reported");
                ImGui::PopStyleColor();
            } else {
                for (const DisplayWidgetMonitor &monitor: snap.m_monitors) {
                    char refresh[24]{"unknown"};
                    if (monitor.m_refresh_hz > 0) {
                        std::snprintf(refresh, sizeof(refresh), "%d Hz", monitor.m_refresh_hz);
                    }
                    char line[512]{};
                    std::snprintf(
                        line, sizeof(line), "%s (%dx%d at %s)%s", monitor.m_name.c_str(), monitor.m_width,
                        monitor.m_height, refresh, monitor.m_primary ? " (main)" : "");
                    ImGui::TextWrapped("%s", line);
                }
            }

            {
                if (snap.m_fullscreen) {
                    const std::string_view monitor = snap.m_fullscreen_monitor.empty() ? std::string_view{"unknown"}
                                                                                       : snap.m_fullscreen_monitor;
                    char fullscreen_line[512]{};
                    std::snprintf(
                        fullscreen_line, sizeof(fullscreen_line), "fullscreen on %.*s",
                        static_cast<int>(
                            std::min(monitor.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                        monitor.data());
                    textRow("Window", fullscreen_line);
                } else {
                    textRow("Window", "windowed mode", false);
                }
            }

            {
                if (snap.m_geometry != nullptr) {
                    const WindowDisplayGeometry &geometry = *snap.m_geometry;
                    char observed[160]{};
                    std::snprintf(
                        observed, sizeof(observed), "%dx%d at %d,%d (draws %dx%d)", geometry.m_size.x,
                        geometry.m_size.y, geometry.m_position.x, geometry.m_position.y, geometry.m_framebuffer_size.x,
                        geometry.m_framebuffer_size.y);
                    textRow("Observed window", observed);
                    if (geometry.m_fullscreen) {
                        char observed_refresh[24]{"unknown refresh"};
                        if (geometry.m_refresh_hz.has_value()) {
                            std::snprintf(observed_refresh, sizeof(observed_refresh), "%u Hz", *geometry.m_refresh_hz);
                        }
                        char exclusive[256]{};
                        std::snprintf(
                            exclusive, sizeof(exclusive), "%s at %s",
                            geometry.m_monitor.empty() ? "unknown monitor" : geometry.m_monitor.c_str(),
                            observed_refresh);
                        textRow("Observed exclusive", exclusive);
                    } else {
                        textRow("Observed exclusive", "windowed (no exclusive monitor or refresh)", false);
                    }
                } else {
                    textRow("Observed window", "not reported yet", false);
                }
            }

            ImGui::EndTable();
        }
        noteText("A present call succeeding is not proof the picture reached the screen.");

        ImGui::EndChild();

        ImGui::Separator();
        const bool trial_open = state.m_trial_open;
        DisplayDisabledScope apply_disabled{snap.m_has_pending or trial_open or matches};
        if (primaryButton("Apply")) {
            intent = EDisplayWidgetAction::apply;
        }
        apply_disabled.close();
        continueActionRow("Reset to startup");
        DisplayDisabledScope reset_disabled{snap.m_has_pending or trial_open or not snap.m_has_runtime_overrides};
        if (ImGui::Button("Reset to startup")) {
            intent = EDisplayWidgetAction::reset_overrides;
        }
        reset_disabled.close();
        continueActionRow("Save to file");
        if (ImGui::Button("Save to file")) {
            intent = EDisplayWidgetAction::save;
        }
        continueActionRow("Close");
        if (ImGui::Button("Close")) {
            intent = EDisplayWidgetAction::close;
        }
        if (snap.m_has_pending) {
            noteText("Waiting for the picture device; Apply is on hold.");
        } else if (trial_open) {
            noteText("Confirm the trial (Keep or Revert) before applying more changes.");
        } else if (not matches) {
            noteText("Edits not yet applied.");
        } else if (snap.m_has_runtime_overrides) {
            noteText("Applied settings can still go back to the startup values.");
        }

        if (not state.m_last_error.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kError.vec4());
            ImGui::TextWrapped("%.*s", checked_cast<int>(state.m_last_error.size()), state.m_last_error.data());
            ImGui::PopStyleColor();
        }
        if (not open) {
            return EDisplayWidgetAction::close;
        }

        return intent;
    }

    std::optional<EDisplayWidgetAction> drawDisplayConfirmation(
        const bool trial_open, const bool confirm_armed, const bool revert_pending, const bool countdown_active,
        const float seconds_left, const bool revert_failed, const std::string_view last_error) {
        if (not trial_open) {
            ui::WindowTitleFont title_font{};
            const bool modal_open = ImGui::BeginPopupModal(
                "Confirm display change", nullptr, ImGuiWindowFlags_NoResize);
            title_font.close();
            if (modal_open) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            return std::nullopt;
        }

        ImGui::OpenPopup("Confirm display change");
        const ImGuiViewport *const viewport = ImGui::GetMainViewport();
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints(
            ImVec2{std::min(viewport->WorkSize.x, fs * 24.0f), std::min(viewport->WorkSize.y, fs * 9.0f)},
            viewport->WorkSize);
        ui::WindowTitleFont title_font{};
        const bool modal_open = ImGui::BeginPopupModal("Confirm display change", nullptr, ImGuiWindowFlags_NoResize);
        title_font.close();
        if (not modal_open) {
            return std::nullopt;
        }

        std::optional<EDisplayWidgetAction> intent{};
        ImGui::TextWrapped("Keep the new picture settings before the trial expires.");
        if (countdown_active) {
            ImGui::Text("%.0f seconds remain.", static_cast<double>(seconds_left));
        }
        if (revert_pending) {
            ImGui::TextWrapped("Reverting to the previous picture settings...");
        }
        if (revert_failed) {
            ImGui::TextWrapped("Revert failed. The previous settings may still be active.");
        }
        if (not last_error.empty()) {
            ImGui::TextWrapped("%.*s", checked_cast<int>(last_error.size()), last_error.data());
        }

        ImGui::BeginDisabled(not confirm_armed or revert_pending);
        if (ImGui::Button("Keep", ImVec2{-1.0f, 0.0f})) {
            intent = EDisplayWidgetAction::keep;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(revert_pending);
        if (ImGui::Button(revert_failed ? "Try revert again" : "Revert now", ImVec2{-1.0f, 0.0f})) {
            intent = EDisplayWidgetAction::revert;
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
        return intent;
    }
}
