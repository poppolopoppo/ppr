module;

module engine.app;
import :ui.widgets.display;
import :display.settings;
import engine.core;
import imgui;
import std;

namespace pP::ui {
    namespace {
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
                                                  ImGuiWindowFlags_NoScrollbar |
                                                  ImGuiWindowFlags_NoScrollWithMouse |
                                                  ImGuiWindowFlags_NoDocking;

        constexpr ImVec4 kRed{1.0f, 0.45f, 0.42f, 1.0f};
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

    /// Dedicated Display window (help-window precedent: centered, fixed
    /// chrome, reopenable). Edits land in the session draft only; every
    /// applied change leaves as a typed intent the caller submits through
    /// the display transaction path. Requested policy and the device
    /// report stay in separate sections; unavailable measurements read as
    /// unavailable.
    std::optional<EDisplayWidgetAction> drawDisplayWidget(const DisplayWidgetSnapshot &snap, DisplayWidgetState state) {
        std::optional<EDisplayWidgetAction> intent{};
        const ImGuiIO &io = ImGui::GetIO();
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(
            ImVec2{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
        ImGui::SetNextWindowSize(ImVec2{std::min(io.DisplaySize.x * 0.6f, fs * 70.0f), 0.0f}, ImGuiCond_Always);
        ImGui::Begin("##display", nullptr, kChromeFlags);
        const DisplayWindowScope window_scope{};
        ImGui::SeparatorText("Display");

        const DisplaySettings &committed = snap.m_view.m_committed;
        const bool matches = state.m_matches;

        ImGui::SeparatorText("Picture");
        {
            const std::string_view current = pacingName(state.m_draft.m_pacing);
            const char *preview = current.empty() ? "?" : current.data();
            if (ImGui::BeginCombo("Pace", preview)) {
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
            int cap = state.m_draft.m_cap_hz > 100000u ? 100000 : static_cast<int>(state.m_draft.m_cap_hz);
            if (ImGui::InputInt("Frame cap (Hz)", &cap, 1, 10)) {
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
            ImGui::Checkbox("Vsync in paced mode", &state.m_draft.m_paced_vsync);
            vsync_disabled.close();
            ImGui::SetItemTooltip(
                "%s\n%s%s", "display.paced_vsync", displayLabel("display.paced_vsync").data(),
                paced ? "" : " (paced mode only)");
        }

        DisplayDisabledScope throttle_disabled{snap.m_has_pending};
        ImGui::Checkbox("Slow down when unfocused", &state.m_draft.m_throttle_unfocused);
        throttle_disabled.close();
        ImGui::SetItemTooltip(
            "%s\n%s", "display.throttle_unfocused", displayLabel("display.throttle_unfocused").data());

        {
            const DisplayWidgetMonitor *monitor = capMonitor(snap);
            const bool known = monitor != nullptr and monitor->m_refresh_hz > 0;
            DisplayDisabledScope monitor_cap_disabled{not known or snap.m_has_pending};
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

        ImGui::SeparatorText("Window");
        const DisplayWidgetAvailability &availability = state.m_availability;
        const DisplayMonitorCapability *switch_monitor = switchMonitor_(snap, state.m_draft.m_monitor);
        {
            const DisplayDisabledScope window_disabled{snap.m_has_pending or not availability.m_supported};
            {
                const std::string_view current = windowModeName(state.m_draft.m_window_mode);
                const char *preview = current.empty() ? "?" : current.data();
                if (ImGui::BeginCombo("Mode", preview)) {
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
                const char *preview = state.m_draft.m_monitor == "auto"
                                          ? "Automatic"
                                          : (switch_monitor != nullptr ? switch_monitor->m_name.c_str()
                                                                       : "Unavailable monitor");
                if (ImGui::BeginCombo("Monitor", preview)) {
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
                    ImGui::TextDisabled("No monitors reported; staying on Automatic.");
                }
            }

            if (state.m_draft.m_window_mode == EDisplayWindowMode::windowed) {
                if (state.m_draft.m_refresh_hz != 0u) {
                    state.m_draft.m_refresh_hz = 0u;
                }
                int width = clampDisplayExtent(
                    state.m_draft.m_width > 2147483647u ? 2147483647 : static_cast<int>(state.m_draft.m_width));
                if (ImGui::InputInt("Width", &width, 16, 160)) {
                    state.m_draft.m_width = static_cast<u32>(clampDisplayExtent(width));
                }
                ImGui::SetItemTooltip("%s\n%s", "display.width", displayLabel("display.width").data());
                int height = clampDisplayExtent(
                    state.m_draft.m_height > 2147483647u ? 2147483647 : static_cast<int>(state.m_draft.m_height));
                if (ImGui::InputInt("Height", &height, 16, 160)) {
                    state.m_draft.m_height = static_cast<u32>(clampDisplayExtent(height));
                }
                ImGui::SetItemTooltip("%s\n%s", "display.height", displayLabel("display.height").data());
                ImGui::TextDisabled("Zero keeps the current window size.");
                DisplayDisabledScope window_refresh_disabled{true};
                char refresh_preview[] = "Platform default";
                if (ImGui::BeginCombo("Refresh", refresh_preview)) {
                    const DisplayComboScope combo_scope{};
                }
                window_refresh_disabled.close();
                ImGui::SetItemTooltip("%s\n%s", "display.refresh_hz", displayLabel("display.refresh_hz").data());
                ImGui::TextDisabled("Refresh applies to fullscreen only.");
            } else {
                const std::vector<std::pair<u32, u32>> resolutions = switchResolutions_(switch_monitor);
                DisplayDisabledScope resolution_disabled{switch_monitor == nullptr or resolutions.empty()};
                char size_preview[32]{"Monitor default"};
                if (state.m_draft.m_width != 0u or state.m_draft.m_height != 0u) {
                    std::snprintf(
                        size_preview, sizeof(size_preview), "%u x %u", state.m_draft.m_width, state.m_draft.m_height);
                }
                if (ImGui::BeginCombo("Resolution", size_preview)) {
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
                    ImGui::TextDisabled("The chosen monitor is not in the reported list.");
                } else if (resolutions.empty()) {
                    ImGui::TextDisabled("No modes reported for this monitor; explicit sizes cannot be checked.");
                } else {
                    ImGui::TextDisabled("Default uses the monitor's current mode; a size must match a listed mode.");
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
                char refresh_preview[32]{"Platform default"};
                if (state.m_draft.m_refresh_hz != 0u) {
                    std::snprintf(refresh_preview, sizeof(refresh_preview), "%u Hz", state.m_draft.m_refresh_hz);
                }
                if (ImGui::BeginCombo("Refresh", refresh_preview)) {
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
                    ImGui::TextDisabled("Explicit refresh is unsupported; using the platform default.");
                } else if (refreshes.empty() and availability.m_supported) {
                    ImGui::TextDisabled("No refresh values reported; using the platform default.");
                }
            }
        }
        if (not availability.m_supported) {
            ImGui::TextDisabled(
                "%s", availability.m_reason != nullptr ? availability.m_reason : "Display switching unavailable.");
            if (snap.m_capabilities == nullptr and snap.m_capabilities_error.value() != 0) {
                ImGui::TextDisabled(
                    "(monitor list failed: %s #%d)", snap.m_capabilities_error.message().c_str(),
                    snap.m_capabilities_error.value());
            }
        }

        const bool trial_open = state.m_trial_open;
        DisplayDisabledScope apply_disabled{snap.m_has_pending or trial_open or matches};
        if (ImGui::Button("Apply")) {
            intent = EDisplayWidgetAction::apply;
        }
        apply_disabled.close();
        ImGui::SameLine();
        DisplayDisabledScope reset_disabled{snap.m_has_pending or trial_open or not snap.m_has_runtime_overrides};
        if (ImGui::Button("Reset to startup")) {
            intent = EDisplayWidgetAction::reset_overrides;
        }
        reset_disabled.close();
        ImGui::SameLine();
        if (ImGui::Button("Save to file")) {
            intent = EDisplayWidgetAction::save;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            intent = EDisplayWidgetAction::close;
        }
        if (snap.m_has_pending) {
            ImGui::TextDisabled("Waiting for the picture device; Apply is on hold.");
        } else if (trial_open) {
            ImGui::TextDisabled("Confirm the trial (Keep or Revert) before applying more changes.");
        } else if (not matches) {
            ImGui::TextDisabled("Edits not yet applied.");
        } else if (snap.m_has_runtime_overrides) {
            ImGui::TextDisabled("Applied settings can still go back to the startup values.");
        }

        if (state.m_confirm_armed and not state.m_revert_pending) {
            ImGui::Spacing();
            ImGui::SeparatorText("Confirm");
            ImGui::Text("New picture settings are on trial.");
            if (state.m_countdown_active) {
                ImGui::Text("%.0f s left to Keep them.", static_cast<double>(state.m_seconds_left));
            } else if (state.m_revert_failed) {
                ImGui::Text("The revert did not go through; the previous settings may still be active.");
            }
            if (ImGui::Button("Keep")) {
                intent = EDisplayWidgetAction::keep;
            }
            ImGui::SameLine();
            if (ImGui::Button(state.m_revert_failed ? "Try revert again" : "Revert now")) {
                intent = EDisplayWidgetAction::revert;
            }
        } else if (state.m_revert_pending) {
            ImGui::Spacing();
            ImGui::TextDisabled("Reverting to the previous picture settings...");
        } else if (state.m_revert_failed) {
            ImGui::Spacing();
            ImGui::Text("The revert did not go through; the previous settings may still be active.");
            if (ImGui::Button("Try revert again")) {
                intent = EDisplayWidgetAction::revert;
            }
        }

        ImGui::SeparatorText("Requested");
        {
            const std::string_view pacing = pacingName(committed.m_pacing);
            ImGui::Text("Pace  %s", pacing.empty() ? "?" : pacing.data());
            if (committed.m_pacing == EFramePacing::paced) {
                ImGui::Text("Cap  %u Hz", committed.m_cap_hz);
            } else {
                ImGui::TextDisabled("Cap  %u Hz (paced mode only)", committed.m_cap_hz);
            }
            {
                const std::string_view mode = windowModeName(committed.m_window_mode);
                ImGui::Text("Mode  %s", mode.empty() ? "?" : mode.data());
                ImGui::Text("Monitor  %s", committed.m_monitor == "auto" ? "automatic" : committed.m_monitor.c_str());
                if (committed.m_width != 0u and committed.m_height != 0u) {
                    ImGui::Text("Size  %u x %u", committed.m_width, committed.m_height);
                } else {
                    ImGui::TextDisabled("Size  default (current mode or window size)");
                }
                if (committed.m_refresh_hz != 0u) {
                    ImGui::Text("Refresh  %u Hz", committed.m_refresh_hz);
                } else {
                    ImGui::TextDisabled("Refresh  platform default");
                }
            }
            ImGui::Text("Vsync  %s (requested)", snap.m_view.m_vsync ? "on" : "off");
            char foreground[32]{};
            char effective[32]{};
            cpuTargetText(foreground, sizeof(foreground), snap.m_view.m_foreground);
            cpuTargetText(effective, sizeof(effective), snap.m_view.m_effective);
            ImGui::Text("Foreground CPU target  %s", foreground);
            ImGui::Text("Effective CPU target  %s", effective);
            if (not committed.m_throttle_unfocused) {
                ImGui::Text("Unfocused slowing  off (setting disabled)");
            } else if (snap.m_view.m_background_priority) {
                ImGui::Text("Unfocused slowing  on (window in background, slowing now)");
            } else {
                ImGui::Text("Unfocused slowing  on (window focused)");
            }
        }

        ImGui::SeparatorText("Device report");
        {
            if (snap.m_applied.m_available) {
                ImGui::Text(
                    "Picture device  %dx%d, vsync %s, change #%llu", snap.m_applied.m_width, snap.m_applied.m_height,
                    snap.m_applied.m_vsync ? "on" : "off",
                    static_cast<unsigned long long>(snap.m_applied.m_generation));
            } else {
                ImGui::TextDisabled("Picture device  not reported yet");
            }
            if (snap.m_has_pending) {
                ImGui::Text(
                    "Change in flight  %s (#%llu)", transactionWords(snap.m_transaction),
                    static_cast<unsigned long long>(snap.m_pending_id));
                ImGui::TextDisabled("A queued change is not applied until the device answers.");
            } else {
                ImGui::TextDisabled("Change in flight  none");
            }
            if (snap.m_error.value() != 0) {
                ImGui::Text(
                    "Last problem  %s (%s #%d)", snap.m_error.message().c_str(), snap.m_error.category().name(),
                    snap.m_error.value());
            } else {
                ImGui::TextDisabled("Last problem  none");
            }
            if (snap.m_monitors.empty()) {
                ImGui::TextDisabled("Monitor  not reported");
            } else {
                for (const DisplayWidgetMonitor &monitor: snap.m_monitors) {
                    char refresh[24]{"unknown"};
                    if (monitor.m_refresh_hz > 0) {
                        std::snprintf(refresh, sizeof(refresh), "%d Hz", monitor.m_refresh_hz);
                    }
                    ImGui::Text(
                        "Monitor  %s (%dx%d at %s)%s", monitor.m_name.c_str(), monitor.m_width, monitor.m_height,
                        refresh, monitor.m_primary ? " (main)" : "");
                }
            }
            if (snap.m_fullscreen) {
                const std::string_view monitor = snap.m_fullscreen_monitor.empty() ? std::string_view{"unknown"}
                                                                                   : snap.m_fullscreen_monitor;
                ImGui::Text(
                    "Window  fullscreen on %.*s",
                    static_cast<int>(
                        std::min(monitor.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                    monitor.data());
            } else {
                ImGui::TextDisabled("Window  windowed mode");
            }
            if (snap.m_geometry != nullptr) {
                const WindowDisplayGeometry &geometry = *snap.m_geometry;
                ImGui::Text(
                    "Observed window  %dx%d at %d,%d (draws %dx%d)", geometry.m_size.x, geometry.m_size.y,
                    geometry.m_position.x, geometry.m_position.y, geometry.m_framebuffer_size.x,
                    geometry.m_framebuffer_size.y);
                if (geometry.m_fullscreen) {
                    char observed_refresh[24]{"unknown refresh"};
                    if (geometry.m_refresh_hz.has_value()) {
                        std::snprintf(observed_refresh, sizeof(observed_refresh), "%u Hz", *geometry.m_refresh_hz);
                    }
                    ImGui::Text(
                        "Observed exclusive  %s at %s",
                        geometry.m_monitor.empty() ? "unknown monitor" : geometry.m_monitor.c_str(), observed_refresh);
                } else {
                    ImGui::TextDisabled("Observed exclusive  windowed (no exclusive monitor or refresh)");
                }
            } else {
                ImGui::TextDisabled("Observed window  not reported yet");
            }
            ImGui::TextDisabled("A present call succeeding is not proof the picture reached the screen.");
        }

        if (not state.m_last_error.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            struct ColorScope {
                ~ColorScope() noexcept {
                    ImGui::PopStyleColor();
                }
            } color_scope;
            ImGui::TextWrapped("%.*s", checked_cast<int>(state.m_last_error.size()), state.m_last_error.data());
        }
        return intent;
    }
}
