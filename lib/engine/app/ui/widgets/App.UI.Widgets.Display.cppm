module;

export module engine.app:ui.widgets.display;

import :display;
import :display.settings;
import :service.window;
import engine.core;
import std;

export namespace pP::ui {
    /// One monitor's reported identity for the Display window. Assembled by
    /// the caller from the window service; a zero refresh means the service did not report
    /// one, and the panel prints "unknown" instead of guessing.
    struct DisplayWidgetMonitor final {
        std::string m_name{};
        int m_width{};
        int m_height{};
        int m_refresh_hz{};
        bool m_primary{false};
        std::string m_identity{};
    };

    /// Everything the Display window renders: the copied controller
    /// view (requested policy, computed targets, throttle reason), the
    /// acknowledged device report, the pending transaction plus the last
    /// failure, and the monitor telemetry. Requested policy and reported
    /// outcomes stay in separate sections; unavailable measurements read as
    /// unavailable, never as zero. The monitor list borrows the caller's
    /// cache for the frame. The
    /// capability and geometry views are owned by the caller (handle-free
    /// snapshots from displayCapabilities/displayWindowGeometry); null
    /// means the query failed, and the switching controls render disabled
    /// with the stored reason while telemetry stays visible.
    struct DisplayWidgetSnapshot final {
        DisplayView m_view{};
        DisplayAppliedState m_applied{};
        EDisplayTransaction m_transaction{EDisplayTransaction::idle};
        std::error_code m_error{};
        bool m_has_pending{false};
        u64 m_pending_id{0u};
        u64 m_revision{0u};
        /// True when committed state carries runtime-layer assignments:
        /// Reset has something to remove even when the draft matches
        /// committed (the post-acknowledgement state), so it gates
        /// independently of the Apply dirty state.
        bool m_has_runtime_overrides{false};
        std::span<const DisplayWidgetMonitor> m_monitors{};
        bool m_fullscreen{false};
        std::string_view m_fullscreen_monitor{};
        const DisplayCapabilities *m_capabilities{nullptr};
        std::error_code m_capabilities_error{};
        const WindowDisplayGeometry *m_geometry{nullptr};
    };

    /// Borrowed editable fields; caller retains ownership and validates emitted actions.
    struct DisplayWidgetDraft {
        EFramePacing &m_pacing;
        u32 &m_cap_hz;
        bool &m_paced_vsync;
        bool &m_throttle_unfocused;
        EDisplayWindowMode &m_window_mode;
        std::string &m_monitor;
        u32 &m_width;
        u32 &m_height;
        u32 &m_refresh_hz;
    };
    struct DisplayWidgetAvailability {
        bool m_supported{};
        const char *m_reason{};
    };
    /// Read-only policy/confirmation projection; widget does not tick or commit trials.
    struct DisplayWidgetState {
        DisplayWidgetDraft m_draft;
        bool m_matches{};
        DisplayWidgetAvailability m_availability{};
        bool m_trial_open{};
        bool m_confirm_armed{};
        bool m_revert_pending{};
        bool m_countdown_active{};
        float m_seconds_left{};
        bool m_revert_failed{};
        std::string_view m_last_error{};
    };
    enum class EDisplayWidgetAction : u8 {
        apply,
        reset_overrides,
        save,
        close,
        keep,
        revert,
        use_monitor_cap
    };
    [[nodiscard]] std::optional<EDisplayWidgetAction> drawDisplayWidget(
        const DisplayWidgetSnapshot &snapshot, DisplayWidgetState state, bool &open);
    [[nodiscard]] std::optional<EDisplayWidgetAction> drawDisplayConfirmation(
        bool trial_open, bool confirm_armed, bool revert_pending, bool countdown_active, float seconds_left,
        bool revert_failed, std::string_view last_error);
}
