module;
#include "pP/Macros.h"

module engine.app;

import :display;
import :display.settings;

import engine.core;

import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(DisplayPolicy, info, none)

    // ------------------------------------------------------------------
    // display policy
    // ------------------------------------------------------------------

    DisplayMonitorSelection findDisplayMonitor(
        const std::string_view preference, const DisplayCapabilities &capabilities) noexcept {
        DisplayMonitorSelection result{};
        const auto find = [&](const bool identities) {
            for (const DisplayMonitorCapability &monitor: capabilities.m_monitors) {
                const bool matches = preference == "auto" ? monitor.m_primary
                                                          : (identities ? not monitor.m_identity.empty() and
                                                                              preference == monitor.m_identity
                                                                        : preference == monitor.m_name);
                if (matches) {
                    if (result.m_monitor != nullptr) {
                        result.m_monitor = nullptr;
                        result.m_ambiguous = true;
                        return;
                    }
                    result.m_monitor = &monitor;
                }
            }
        };
        find(true);
        // A serialized native token is never reinterpreted as another monitor's label.
        if (result.m_monitor == nullptr and
            not result.m_ambiguous and
            preference != "auto" and
            not preference.starts_with("win32-monitor:")) {
            find(false);
        }
        return result;
    }

    Expected<const DisplayMonitorCapability *> resolveDisplayMonitor(
        const std::string_view preference, const DisplayCapabilities &capabilities, config::Diagnostics &diagnostics) {
        const DisplayMonitorSelection result = findDisplayMonitor(preference, capabilities);
        if (result.m_ambiguous) {
            const auto error = std::make_error_code(std::errc::invalid_argument);
            diagnostics.add("display capabilities", "display.monitor", "Ambiguous monitor preference", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DisplayPolicy, error);
        }
        if (result.m_monitor == nullptr and preference != "auto") {
            const auto error = std::make_error_code(std::errc::no_such_device);
            diagnostics.add(
                "display capabilities", "display.monitor", "Monitor preference is absent from current enumeration",
                error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DisplayPolicy, error);
        }
        return result.m_monitor;
    }

    std::optional<TimeSpan> foregroundFramePeriod(const DisplaySettings &settings) noexcept {
        switch (settings.m_pacing) {
            case EFramePacing::uncapped: return std::nullopt;
            case EFramePacing::follow_vsync: return std::nullopt;
            case EFramePacing::paced: {
                if (settings.m_cap_hz == 0u) {
                    return std::nullopt;
                }
                return std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / static_cast<double>(settings.m_cap_hz)});
            }
        }
        return std::nullopt;
    }

    bool presentationVsync(const DisplaySettings &settings) noexcept {
        switch (settings.m_pacing) {
            case EFramePacing::uncapped: return false;
            case EFramePacing::follow_vsync: return true;
            case EFramePacing::paced: return settings.m_paced_vsync;
        }
        return true;
    }

    PacingTargets pacingTargets(const DisplaySettings &settings) noexcept {
        return PacingTargets{
            .m_target = foregroundFramePeriod(settings),
            .m_vsync = presentationVsync(settings),
        };
    }

    std::optional<TimeSpan> effectiveFramePeriod(
        const std::optional<TimeSpan> foreground, const bool throttle_active) noexcept {
        if (not throttle_active) {
            return foreground;
        }
        if (not foreground.has_value()) {
            return kUnfocusedThrottlePeriod;
        }
        if (*foreground >= kUnfocusedThrottlePeriod) {
            return foreground;
        }
        return kUnfocusedThrottlePeriod;
    }

    bool displayWindowChange(const DisplaySettings &a, const DisplaySettings &b) noexcept {
        return a.m_window_mode != b.m_window_mode or
               a.m_monitor != b.m_monitor or
               a.m_width != b.m_width or
               a.m_height != b.m_height or
               a.m_refresh_hz != b.m_refresh_hz;
    }

    std::error_code validateDisplaySettings(
        const DisplaySettings &settings, const DisplayCapabilities &capabilities, config::Diagnostics &diagnostics) {
        if (const std::error_code error = DisplaySettings::validate(settings, diagnostics)) {
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, error);
        }
        const auto reject = [&](const std::string_view property, const std::string_view message,
                                const std::errc reason = std::errc::invalid_argument) {
            const std::error_code error = std::make_error_code(reason);
            diagnostics.add("display capabilities", std::string{property}, std::string{message}, error);
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, error);
            return std::error_code{};
        };
        const auto resolved = resolveDisplayMonitor(settings.m_monitor, capabilities, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, resolved);
        const DisplayMonitorCapability *const selected = *resolved;
        if (settings.m_window_mode == EDisplayWindowMode::windowed) {
            return {};
        }
        if (not capabilities.m_window_changes or selected == nullptr) {
            return reject(
                "display.window_mode", "Exclusive fullscreen is unavailable", std::errc::operation_not_supported);
        }
        if (settings.m_refresh_hz != 0u and not capabilities.m_exclusive_refresh) {
            return reject(
                "display.refresh_hz", "Explicit exclusive refresh is unsupported", std::errc::operation_not_supported);
        }
        const u32 width = settings.m_width != 0u ? settings.m_width : selected->m_current.m_width;
        const u32 height = settings.m_height != 0u ? settings.m_height : selected->m_current.m_height;
        const bool supported = std::ranges::any_of(selected->m_modes, [&](const DisplayModeCapability &mode) {
            return mode.m_width == width and
                   mode.m_height == height and
                   (settings.m_refresh_hz == 0u or mode.m_refresh_hz == settings.m_refresh_hz);
        });
        return supported ? std::error_code{}
                         : reject("display.width", "Resolution/refresh is not an enumerated exclusive mode");
    }

    // ------------------------------------------------------------------
    // display controller
    // ------------------------------------------------------------------

    DisplayController::DisplayController(const DisplaySettings &settings) : m_committed(settings) {
        m_resolved.m_value = settings;
    }

    void DisplayController::setCommitted(const DisplaySettings &settings) {
        DisplaySettings committed = settings;
        DisplaySettings resolved = settings;
        m_committed = std::move(committed);
        m_resolved.m_value = std::move(resolved);
    }

    const DisplaySettings &DisplayController::committed() const noexcept {
        return m_committed;
    }

    std::error_code DisplayController::begin(config::Resolved<DisplaySettings> candidate) {
        config::Diagnostics diagnostics{};
        return begin(std::move(candidate), DisplayCapabilities{}, diagnostics);
    }

    std::error_code DisplayController::begin(
        config::Resolved<DisplaySettings> candidate, const DisplayCapabilities &capabilities,
        config::Diagnostics &diagnostics) {
        if (m_transactions_stopped) {
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, std::make_error_code(std::errc::operation_canceled));
        }
        if (m_pending or m_transaction_state == EDisplayTransaction::degraded) {
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, std::make_error_code(std::errc::device_or_resource_busy));
        }
        DisplaySettings &settings = candidate.m_value;
        if (const std::error_code error = validateDisplaySettings(settings, capabilities, diagnostics)) {
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, error);
        }
        const auto monitor = resolveDisplayMonitor(settings.m_monitor, capabilities, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, monitor);
        if (settings.m_monitor != "auto" and *monitor != nullptr and not (*monitor)->m_identity.empty()) {
            settings.m_monitor = (*monitor)->m_identity;
        }
        if (displayWindowChange(settings, m_committed) and not capabilities.m_window_changes) {
            const auto error = std::make_error_code(std::errc::operation_not_supported);
            diagnostics.add(
                "display capabilities", "display.window_mode", "Native window changes are unsupported", error);
            PPR_RETURN_ERROR_ON_FAIL(DisplayPolicy, error);
        }
        candidate.m_revision = m_resolved.m_revision + 1u;
        m_pending.emplace(DisplayCandidate{std::move(candidate), ++m_next_transaction_id});
        m_transaction_error = {};
        m_window_applied = false;
        m_transaction_state = EDisplayTransaction::unsubmitted;
        return {};
    }

    bool DisplayController::restoring() const noexcept {
        return m_transaction_state == EDisplayTransaction::restore_unsubmitted or
               m_transaction_state == EDisplayTransaction::restore_awaiting_ack;
    }

    bool DisplayController::admissionSuspended() const noexcept {
        return m_pending.has_value() or m_transaction_state == EDisplayTransaction::degraded;
    }

    void DisplayController::markSubmitted() noexcept {
        if (m_transaction_state == EDisplayTransaction::unsubmitted) {
            m_transaction_state = EDisplayTransaction::awaiting_ack;
        } else if (m_transaction_state == EDisplayTransaction::restore_unsubmitted) {
            m_transaction_state = EDisplayTransaction::restore_awaiting_ack;
        }
    }

    bool DisplayController::acknowledge(
        const u64 transaction_id, const DisplayAppliedState applied, const std::error_code error) {
        if (not m_pending or
            m_pending->m_transaction_id != transaction_id or
            (m_transaction_state != EDisplayTransaction::awaiting_ack and
                m_transaction_state != EDisplayTransaction::restore_awaiting_ack)) {
            return false;
        }
        const bool restore = restoring();
        m_applied = applied;
        if (error) {
            if (not m_transaction_error) {
                m_transaction_error = error;
            }
            if (restore or (m_transactions_stopped and not m_window_applied)) {
                if (restore and not m_applied.m_restoration_error) {
                    m_applied.m_restoration_error = error;
                }
                m_transaction_state = EDisplayTransaction::degraded;
                m_pending.reset();
            } else {
                m_pending->m_transaction_id = ++m_next_transaction_id;
                m_transaction_state = EDisplayTransaction::restore_unsubmitted;
            }
            return true;
        }
        if (not restore) {
            DisplaySettings committed = m_pending->m_resolved.m_value;
            m_resolved = std::move(m_pending->m_resolved);
            m_committed = std::move(committed);
        }
        m_pending.reset();
        m_window_applied = false;
        m_transaction_state = EDisplayTransaction::idle;
        return true;
    }

    void DisplayController::cancelUnsubmitted() noexcept {
        if (m_window_applied and m_transaction_state == EDisplayTransaction::unsubmitted) {
            if (not m_transaction_error) {
                m_transaction_error = std::make_error_code(std::errc::operation_canceled);
            }
            m_pending->m_transaction_id = ++m_next_transaction_id;
            m_transaction_state = EDisplayTransaction::restore_unsubmitted;
            return;
        }
        if (m_window_applied and m_transaction_state == EDisplayTransaction::restore_unsubmitted) {
            return;
        }
        if (m_transaction_state == EDisplayTransaction::unsubmitted or
            m_transaction_state == EDisplayTransaction::restore_unsubmitted) {
            m_pending.reset();
            m_transaction_state = m_transaction_error ? EDisplayTransaction::degraded : EDisplayTransaction::idle;
        }
    }

    void DisplayController::stopTransactions() noexcept {
        m_transactions_stopped = true;
        cancelUnsubmitted();
    }

    void DisplayController::startTransactions() noexcept {
        m_window_applied = false;
        m_transactions_stopped = false;
        m_pending.reset();
        m_transaction_state = EDisplayTransaction::idle;
        m_transaction_error = {};
        m_applied = {};
    }

    void DisplayController::invalidatePending() noexcept {
        m_window_applied = false;
        m_pending.reset();
        m_applied = {};
        m_transaction_state = EDisplayTransaction::idle;
        m_transaction_error = std::make_error_code(std::errc::operation_canceled);
    }

    void DisplayController::markWindowApplied() noexcept {
        if (m_pending) {
            m_window_applied = true;
        }
    }

    std::optional<TimeSpan> DisplayController::foregroundTarget() const noexcept {
        return foregroundFramePeriod(m_committed);
    }

    bool DisplayController::presentationVsync() const noexcept {
        return pP::presentationVsync(m_committed);
    }

    bool DisplayController::throttleActive(const bool background_priority) const noexcept {
        return background_priority and m_committed.m_throttle_unfocused;
    }

    std::optional<TimeSpan> DisplayController::effectiveTarget(const bool background_priority) const noexcept {
        return effectiveFramePeriod(foregroundTarget(), throttleActive(background_priority));
    }

    DisplayView DisplayController::view(const bool background_priority) const {
        const bool throttle_active = throttleActive(background_priority);
        return DisplayView{
            .m_committed = m_committed,
            .m_foreground = foregroundTarget(),
            .m_effective = effectiveFramePeriod(foregroundTarget(), throttle_active),
            .m_vsync = presentationVsync(),
            .m_background_priority = background_priority,
            .m_throttle_active = throttle_active,
        };
    }
} // namespace pP
