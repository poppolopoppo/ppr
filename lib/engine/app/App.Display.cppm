module;

#include "pP/Macros.h"

export module engine.app:display;

import :display.settings;

import engine.core;

import std;

export namespace pP {
    struct DisplayModeCapability {
        u32 m_width{0u};
        u32 m_height{0u};
        std::optional<u32> m_refresh_hz{};
    };

    struct DisplayMonitorCapability {
        std::string m_name{};
        bool m_primary{false};
        DisplayModeCapability m_current{};
        Array<DisplayModeCapability> m_modes{};
    };

    /// Owned, handle-free enumeration snapshot; refresh absence means unknown.
    struct DisplayCapabilities {
        bool m_window_changes{false};
        bool m_exclusive_refresh{false};
        Array<DisplayMonitorCapability> m_monitors{};
    };

    [[nodiscard]] bool displayWindowChange(const DisplaySettings &a, const DisplaySettings &b) noexcept;
    [[nodiscard]] std::error_code validateDisplaySettings(
        const DisplaySettings &settings, const DisplayCapabilities &capabilities, config::Diagnostics &diagnostics);
    /// Background-throttle floor (§4.2): when display.throttle_unfocused is
    /// enabled and the background-priority policy is active, the effective
    /// CPU period is max(foreground period or zero, this floor).
    inline constexpr TimeSpan kUnfocusedThrottlePeriod{std::chrono::milliseconds{200}};

    /// Foreground CPU pacing target for committed settings (§4.1):
    /// uncapped → nullopt, follow-vsync → nullopt, paced → 1/cap_hz.
    /// A zero cap (unreachable through validated resolution, Range [1,1000])
    /// degrades to nullopt rather than dividing by zero.
    [[nodiscard]] std::optional<TimeSpan> foregroundFramePeriod(const DisplaySettings &settings) noexcept;

    /// Requested presentation synchronization (§4.1): uncapped → false,
    /// follow-vsync → true, paced → paced_vsync.
    [[nodiscard]] bool presentationVsync(const DisplaySettings &settings) noexcept;

    /// Foreground mapping (§4.1 table) as one value: the CPU target plus the
    /// requested vsync half. Implemented through the two helpers above.
    struct PacingTargets {
        std::optional<TimeSpan> m_target{};
        bool m_vsync{true};
    };

    [[nodiscard]] PacingTargets pacingTargets(const DisplaySettings &settings) noexcept;

    /// Background-throttle overlay (§4.2): inactive → foreground unchanged;
    /// active → max(foreground or zero, kUnfocusedThrottlePeriod). A slower
    /// explicit cap (e.g. paced 1fps) is preserved, never sped up to 5fps.
    /// Vsync is orthogonal and never toggles here.
    [[nodiscard]] std::optional<TimeSpan> effectiveFramePeriod(
        std::optional<TimeSpan> foreground, bool throttle_active) noexcept;

    /// Telemetry view (§5.1): requested policy plus computed targets and the
    /// throttle reason. Applied surface state (vsync actually presenting,
    /// monitor refresh) belongs to Phase 4 and is not represented here.
    struct DisplayView {
        DisplaySettings m_committed{};
        std::optional<TimeSpan> m_foreground{};
        std::optional<TimeSpan> m_effective{};
        bool m_vsync{true};
        bool m_background_priority{false};
        bool m_throttle_active{false};
    };

    enum class EDisplayTransaction : u8 {
        idle,
        unsubmitted,
        awaiting_ack,
        restore_unsubmitted,
        restore_awaiting_ack,
        degraded
    };

    /// Copied backend observation, independent of windows and lane transports.
    struct DisplayAppliedState {
        u64 m_generation{0u};
        int m_width{0};
        int m_height{0};
        bool m_vsync{true};
        bool m_available{false};
        std::error_code m_restoration_error{};
    };

    struct DisplayCandidate {
        config::Resolved<DisplaySettings> m_resolved{};
        u64 m_transaction_id{0u};
    };

    /// App-owned pacing policy: the committed DisplaySettings plus the pure
    /// foreground/effective/vsync mapping. Holds no window, monitor,
    /// renderer, or lane state — inject settings plus the
    /// background-priority flag, so unit tests never need a live surface.
    /// Application transports requests and observations; this policy state
    /// contains no window handles, renderer, or lane references.
    class DisplayController {
    public:
        DisplayController() = default;

        explicit DisplayController(const DisplaySettings &settings);

        void setCommitted(const DisplaySettings &settings);

        [[nodiscard]] const DisplaySettings &committed() const noexcept;

        [[nodiscard]] std::optional<TimeSpan> foregroundTarget() const noexcept;

        [[nodiscard]] bool presentationVsync() const noexcept;

        /// Focus callbacks preserve raw focus loss as background priority.
        /// Application::getTargetFrameTime/displayView apply the legacy
        /// opt-out centrally before passing this input; committed
        /// throttle_unfocused also gates activation. Vsync never toggles here.
        [[nodiscard]] bool throttleActive(bool background_priority) const noexcept;

        [[nodiscard]] std::optional<TimeSpan> effectiveTarget(bool background_priority) const noexcept;

        [[nodiscard]] DisplayView view(bool background_priority) const;

        [[nodiscard]] std::error_code begin(config::Resolved<DisplaySettings> candidate);
        [[nodiscard]] std::error_code begin(
            config::Resolved<DisplaySettings> candidate, const DisplayCapabilities &capabilities,
            config::Diagnostics &diagnostics);
        void markSubmitted() noexcept;
        /// OS mutation has started: cancellation must restore, not drop the candidate.
        void markWindowApplied() noexcept;
        /// Only a matching acknowledgement can publish settings/provenance/revision.
        [[nodiscard]] bool acknowledge(u64 transaction_id, DisplayAppliedState applied, std::error_code error);
        void cancelUnsubmitted() noexcept;
        void stopTransactions() noexcept;
        void startTransactions() noexcept;
        void invalidatePending() noexcept;
        [[nodiscard]] const std::optional<DisplayCandidate> &pending() const noexcept {
            return m_pending;
        }
        [[nodiscard]] EDisplayTransaction transactionState() const noexcept {
            return m_transaction_state;
        }
        [[nodiscard]] const config::Resolved<DisplaySettings> &resolved() const noexcept {
            return m_resolved;
        }
        [[nodiscard]] const DisplayAppliedState &applied() const noexcept {
            return m_applied;
        }
        [[nodiscard]] std::error_code transactionError() const noexcept {
            return m_transaction_error;
        }
        [[nodiscard]] bool admissionSuspended() const noexcept;
        [[nodiscard]] bool restoring() const noexcept;

    private:
        DisplaySettings m_committed{};
        config::Resolved<DisplaySettings> m_resolved{};
        std::optional<DisplayCandidate> m_pending{};
        DisplayAppliedState m_applied{};
        EDisplayTransaction m_transaction_state{EDisplayTransaction::idle};
        std::error_code m_transaction_error{};
        u64 m_next_transaction_id{0u};
        bool m_transactions_stopped{false};
        bool m_window_applied{false};
    };
} // namespace pP
