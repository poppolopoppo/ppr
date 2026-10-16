module;
#include "pP/Macros.h"
#include <cerrno>

module game.display;

import engine.app;
import engine.core;

import std;

namespace pP::game {

    // ------------------------------------------------------------------
    // display core
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(GameDisplay, info, none)
    namespace {
        [[nodiscard]] std::error_code displayException() noexcept {
            try {
                throw;
            } catch (const std::bad_alloc &) {
                return std::make_error_code(std::errc::not_enough_memory);
            } catch (const std::system_error &error) {
                return error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable);
            } catch (...) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }
    }

    Expected<DisplayDraft> displayDraftFrom(const DisplaySettings &settings) noexcept {
        try {
            DisplayDraft draft{};
            draft.m_pacing = settings.m_pacing;
            draft.m_cap_hz = settings.m_cap_hz;
            draft.m_paced_vsync = settings.m_paced_vsync;
            draft.m_throttle_unfocused = settings.m_throttle_unfocused;
            draft.m_window_mode = settings.m_window_mode;
            draft.m_monitor = settings.m_monitor.empty() ? std::string{"auto"} : settings.m_monitor;
            draft.m_width = settings.m_width;
            draft.m_height = settings.m_height;
            draft.m_refresh_hz = settings.m_refresh_hz;
            return draft;
        } catch (...) {
            return std::unexpected{displayException()};
        }
    }

    Expected<DisplaySettings> displaySettingsFrom(const DisplayDraft &draft) noexcept {
        try {
            DisplaySettings settings{};
            settings.m_pacing = draft.m_pacing;
            settings.m_cap_hz = draft.m_cap_hz;
            settings.m_paced_vsync = draft.m_paced_vsync;
            settings.m_throttle_unfocused = draft.m_throttle_unfocused;
            settings.m_window_mode = draft.m_window_mode;
            settings.m_monitor = draft.m_monitor;
            settings.m_width = draft.m_width;
            settings.m_height = draft.m_height;
            settings.m_refresh_hz = draft.m_refresh_hz;
            return settings;
        } catch (...) {
            return std::unexpected{displayException()};
        }
    }

    bool displaySettingsEqual(const DisplaySettings &lhs, const DisplaySettings &rhs) noexcept {
        return lhs.m_pacing == rhs.m_pacing and
               lhs.m_cap_hz == rhs.m_cap_hz and
               lhs.m_paced_vsync == rhs.m_paced_vsync and
               lhs.m_throttle_unfocused == rhs.m_throttle_unfocused and
               lhs.m_window_mode == rhs.m_window_mode and
               lhs.m_monitor == rhs.m_monitor and
               lhs.m_width == rhs.m_width and
               lhs.m_height == rhs.m_height and
               lhs.m_refresh_hz == rhs.m_refresh_hz;
    }

    bool displayDraftMatches(const DisplayDraft &draft, const DisplaySettings &committed) noexcept {
        return draft.m_pacing == committed.m_pacing and
               draft.m_cap_hz == committed.m_cap_hz and
               draft.m_paced_vsync == committed.m_paced_vsync and
               draft.m_throttle_unfocused == committed.m_throttle_unfocused and
               draft.m_window_mode == committed.m_window_mode and
               draft.m_monitor == committed.m_monitor and
               draft.m_width == committed.m_width and
               draft.m_height == committed.m_height and
               draft.m_refresh_hz == committed.m_refresh_hz;
    }

    std::string_view pacingName(const EFramePacing pacing) noexcept {
        return pacing_domain.nameOf(pacing);
    }

    std::string_view windowModeName(const EDisplayWindowMode mode) noexcept {
        return window_mode_domain.nameOf(mode);
    }

    DisplaySwitchingAvailability displaySwitchingAvailability(const DisplayCapabilities *const capabilities) noexcept {
        if (capabilities == nullptr) {
            return {
                .m_supported = false,
                .m_reason = "Window switching is unavailable: the monitor list could not be read."};
        }
        if (not capabilities->m_window_changes) {
            return {.m_supported = false, .m_reason = "Window switching is unsupported on this display backend."};
        }
        return {.m_supported = true};
    }

    /// Monitor preferences are names, never numeric handles: a panel or file
    /// value that is all digits names no monitor and is rejected here, with
    /// its own message, before the schema layer runs.
    [[nodiscard]] std::error_code rejectNumericMonitor(const DisplayDraft &draft, config::Diagnostics &diagnostics) {
        if (draft.m_monitor.empty() or draft.m_monitor == "auto") {
            return {};
        }
        const bool numeric = std::ranges::all_of(
            draft.m_monitor, [](const char digit) noexcept { return digit >= '0' and digit <= '9'; });
        if (not numeric) {
            return {};
        }

        const std::error_code error = std::make_error_code(std::errc::invalid_argument);
        diagnostics.add(
            "display-panel", "display.monitor", "Monitor must be auto or an enumerated name, never a number", error);
        return error;
    }

    config::Layer displayRuntimeLayer(const DisplayDraft &draft, std::string origin) {
        config::Layer layer{};
        layer.m_source = config::ESource::runtime;
        layer.m_origin = origin;
        const std::string location = layer.m_origin;
        layer.m_assignments.push_back(
            config::Assignment{
                "display.pacing", "panel", config::SourceValue::text(std::string{pacingName(draft.m_pacing)}), location,
                0u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.cap_hz", "panel", config::SourceValue::integer(static_cast<i64>(draft.m_cap_hz)), location,
                1u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.paced_vsync", "panel", config::SourceValue::boolean(draft.m_paced_vsync), location, 2u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.throttle_unfocused", "panel", config::SourceValue::boolean(draft.m_throttle_unfocused),
                location, 3u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.window_mode", "panel",
                config::SourceValue::text(std::string{windowModeName(draft.m_window_mode)}), location, 4u});
        layer.m_assignments.push_back(
            config::Assignment{"display.monitor", "panel", config::SourceValue::text(draft.m_monitor), location, 5u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.width", "panel", config::SourceValue::integer(static_cast<i64>(draft.m_width)), location, 6u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.height", "panel", config::SourceValue::integer(static_cast<i64>(draft.m_height)), location,
                7u});
        layer.m_assignments.push_back(
            config::Assignment{
                "display.refresh_hz", "panel", config::SourceValue::integer(static_cast<i64>(draft.m_refresh_hz)),
                location, 8u});
        return layer;
    }

    Expected<config::Resolved<DisplaySettings>> applyDisplayRuntime(
        const config::Resolved<DisplaySettings> &committed, const config::Layer &layer,
        config::Diagnostics &diagnostics) {
        config::Resolved<DisplaySettings> scratch = committed;
        if (const std::error_code error = config::applyRuntime(scratch, layer, diagnostics)) {
            return std::unexpected{error};
        }
        return scratch;
    }

    Expected<config::Resolved<DisplaySettings>> validateDisplayDraft(
        const config::Resolved<DisplaySettings> &committed, const DisplayDraft &draft,
        config::Diagnostics &diagnostics) {
        if (const std::error_code error = rejectNumericMonitor(draft, diagnostics)) {
            return std::unexpected{error};
        }
        return applyDisplayRuntime(committed, displayRuntimeLayer(draft, "display-panel"), diagnostics);
    }

    Expected<config::Resolved<DisplaySettings>> validateDisplayDraft(
        const config::Resolved<DisplaySettings> &committed, const DisplayDraft &draft,
        const DisplayCapabilities &capabilities, config::Diagnostics &diagnostics) {
        auto candidate = validateDisplayDraft(committed, draft, diagnostics);
        if (not candidate) {
            return std::unexpected{candidate.error()};
        }

        if (displayWindowChange(committed.m_value, candidate->m_value) and not capabilities.m_window_changes) {
            const std::error_code error = std::make_error_code(std::errc::operation_not_supported);
            diagnostics.add(
                "display capabilities", "display.window_mode", "Native window changes are unsupported", error);
            return std::unexpected{error};
        }

        if (const std::error_code error = validateDisplaySettings(candidate->m_value, capabilities, diagnostics)) {
            return std::unexpected{error};
        }
        return candidate;
    }

    Expected<config::Document> buildDisplayOverlayDocument(
        const config::Document &existing, const DisplaySettings &committed) {
        auto fresh = config::documentFrom(committed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, fresh);

        Array<std::string> persistable{};
        DisplaySettings probe{};

        config::visit(probe, [&](const auto &member, const auto &) {
            if (member.m_metadata.m_persistable) {
                persistable.push_back(std::string{member.m_metadata.m_name});
            }
        });

        config::Document merged{};
        for (const auto &entry: existing.m_entries) {
            if (entry.m_key.starts_with("display.")) {
                continue;
            }
            merged.m_entries.push_back(entry);
        }

        for (auto &entry: fresh->m_entries) {
            bool keep = false;
            for (const auto &name: persistable) {
                if (entry.m_key == name) {
                    keep = true;
                    break;
                }
            }
            if (not keep) {
                continue;
            }
            entry.m_location = "display-panel";
            merged.m_entries.push_back(std::move(entry));
        }
        return merged;
    }

    Expected<std::string> encodeDisplayOverlay(const config::Document &document) {
        auto body = config::encode(document);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, body);

        std::string out{"# Saved from the Display window (Save button): acknowledged picture settings only.\n"
                        "# Hand-edited base files are never rewritten by this action.\n"
                        "# The command line and environment entries still take priority over this file.\n"};
        out += *body;
        return out;
    }

    std::error_code writeFileAtomically(const std::filesystem::path &path, const std::string_view bytes) noexcept {
        std::error_code operational_error{};
        try {
            const std::basic_string_view<std::filesystem::path::value_type> path_text{path.native()};
            const auto failure = [&](const char *operation, const std::error_code error) {
                operational_error = error;
                PPR_LOG(
                    GameDisplay, error, "display overlay write failed",
                    {{"path", path_text}, {"operation", operation}, {"code", error.value()},
                        {"message", error.message()}});
                return error;
            };
            const auto stream_error = [] {
                return errno != 0 ? std::error_code{errno, std::generic_category()}
                                  : std::make_error_code(std::errc::io_error);
            };

            std::filesystem::path temp = path;
            temp += ".tmp";
            bool acquired = false;
            bool renamed = false;
            PPR_DEFER {
                if (acquired and not renamed) {
                    // Failed saves preserve the destination; removing the owned
                    // staging file is best effort and must not replace the cause.
                    std::error_code cleanup_error{};
                    std::filesystem::remove(temp, cleanup_error);
                    try {
                        PPR_LOG_WARNING_ON_FAIL(GameDisplay, cleanup_error);
                    } catch (...) {
                        // Best-effort teardown diagnostics must not throw from
                        // the scope guard or replace the original save failure.
                        PPR_LOG(
                            GameDisplay, warning,
                            "display overlay staging cleanup failed; diagnostic details unavailable");
                    }
                }
            };

            {
                errno = 0;
                std::ofstream out{temp, std::ios::binary | std::ios::trunc};
                if (not out) {
                    PPR_RETURN_ERROR_ON_FAIL(GameDisplay, failure("open temporary file", stream_error()));
                }
                acquired = true;
                errno = 0;
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (not out) {
                    PPR_RETURN_ERROR_ON_FAIL(GameDisplay, failure("write temporary file", stream_error()));
                }

                errno = 0;
                out.close();
                if (not out) {
                    PPR_RETURN_ERROR_ON_FAIL(GameDisplay, failure("close temporary file", stream_error()));
                }
            }

            std::error_code rename_error{};
            std::filesystem::rename(temp, path, rename_error);
            if (rename_error) {
                PPR_RETURN_ERROR_ON_FAIL(GameDisplay, failure("replace destination", rename_error));
            }
            renamed = true;
            return {};
        } catch (...) {
            const auto error = operational_error ? operational_error : displayException();
            try {
                PPR_LOG(
                    GameDisplay, error, "display overlay write exception",
                    {{"path", std::basic_string_view<std::filesystem::path::value_type>{path.native()}},
                        {"category", error.category().name()}, {"code", error.value()}});
                PPR_RETURN_ERROR_ON_FAIL(GameDisplay, error);
            } catch (...) {
                // Best-effort diagnostics can also exhaust memory. Preserve
                // the save result instead of terminating from this noexcept boundary.
                PPR_LOG(
                    GameDisplay, error, "display overlay save failed; diagnostic details unavailable",
                    {{"category", error.category().name()}, {"code", error.value()}});
            }
            return error;
        }
    }

    Expected<std::optional<config::Layer>> loadOverlayLayer(
        const std::filesystem::path &path, config::Diagnostics &diagnostics) {
        auto bytes = readOverlayBytes(path, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, bytes);
        if (not bytes->has_value()) {
            return std::optional<config::Layer>{std::nullopt};
        }

        auto document = config::decode(**bytes, path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, document);
        return std::optional<config::Layer>{
            config::toLayer(std::move(*document), "user-overlay(" + path.string() + ")")};
    }

    Expected<std::filesystem::path> resolveDisplayOverlayPath() {
        try {
            return hal::process::currentExecutablePath().parent_path() / kDisplayOverlayFileName;
        } catch (...) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, displayException());
            std::unreachable(); // Exception translation always produces failure.
        }
    }

    std::error_code armDisplayConfirm(DisplayConfirm &confirm, const DisplaySettings &prior) noexcept {
        return armDisplayConfirm(confirm, prior, std::nullopt);
    }

    std::error_code armDisplayConfirm(
        DisplayConfirm &confirm, const DisplaySettings &prior,
        const std::optional<WindowDisplayGeometry> &geometry) noexcept {
        try {
            DisplaySettings prepared_prior = prior;
            std::optional<WindowDisplayGeometry> prepared_geometry = geometry;
            armDisplayConfirmOwned(confirm, std::move(prepared_prior), std::move(prepared_geometry));
            return {};
        } catch (...) {
            return displayException();
        }
    }

    void armDisplayConfirmOwned(
        DisplayConfirm &confirm, DisplaySettings &&prior, std::optional<WindowDisplayGeometry> &&geometry) noexcept {
        confirm.m_prior = std::move(prior);
        confirm.m_has_prior_geometry = geometry.has_value();
        if (geometry) {
            confirm.m_prior_geometry = std::move(*geometry);
        }
        confirm.m_revert_pending = false;
        confirm.m_seconds_left = kDisplayConfirmSeconds;
        confirm.m_countdown_active = true;
        confirm.m_armed = true;
    }

    bool displayTrialUnresolved(const DisplayConfirm &confirm) noexcept {
        return confirm.m_armed;
    }

    Expected<DisplaySettings> displayRevertSettings(const DisplayConfirm &confirm) noexcept {
        try {
            DisplaySettings revert = confirm.m_prior;
            if (not confirm.m_has_prior_geometry) {
                return revert;
            }
            const WindowDisplayGeometry &geometry = confirm.m_prior_geometry;

            if (geometry.m_fullscreen) {
                revert.m_window_mode = EDisplayWindowMode::fullscreen;
                if (not geometry.m_monitor.empty()) {
                    revert.m_monitor = geometry.m_monitor;
                }
                if (geometry.m_size.x > 0 and geometry.m_size.y > 0) {
                    revert.m_width = static_cast<u32>(geometry.m_size.x);
                    revert.m_height = static_cast<u32>(geometry.m_size.y);
                }
                if (geometry.m_refresh_hz.has_value()) {
                    revert.m_refresh_hz = *geometry.m_refresh_hz;
                }
                return revert;
            }

            revert.m_window_mode = EDisplayWindowMode::windowed;
            revert.m_refresh_hz = 0u;
            if (geometry.m_size.x > 0 and geometry.m_size.y > 0) {
                revert.m_width = static_cast<u32>(geometry.m_size.x);
                revert.m_height = static_cast<u32>(geometry.m_size.y);
            }
            return revert;
        } catch (...) {
            return std::unexpected{displayException()};
        }
    }

    Expected<std::optional<WindowDisplayGeometry>> captureDisplayTrialGeometry(
        const bool window_change, const std23::function_ref<Expected<WindowDisplayGeometry>()> acquire) {
        auto geometry = acquire();
        if (not geometry) {
            if (window_change) {
                return std::unexpected{geometry.error()};
            }
            return std::optional<WindowDisplayGeometry>{std::nullopt};
        }
        if (window_change and (geometry->m_size.x <= 0 or geometry->m_size.y <= 0)) {
            return std::unexpected{std::make_error_code(std::errc::io_error)};
        }
        return std::optional<WindowDisplayGeometry>{std::move(*geometry)};
    }

    bool displayHasRuntimeOverrides(const config::Resolved<DisplaySettings> &resolved) noexcept {
        for (const auto &origin: resolved.m_origins) {
            if (origin.m_source == config::ESource::runtime) {
                return true;
            }
        }
        return false;
    }

    config::Resolved<DisplaySettings> displayResetCandidate(const config::Resolved<DisplaySettings> &baseline) {
        return baseline;
    }

    bool tickDisplayConfirm(DisplayConfirm &confirm, const float dt_seconds) noexcept {
        if (not confirm.m_armed or not confirm.m_countdown_active or confirm.m_revert_pending) {
            return false;
        }
        if (dt_seconds <= 0.0f) {
            return false;
        }
        confirm.m_seconds_left -= dt_seconds;
        if (confirm.m_seconds_left > 0.0f) {
            return false;
        }
        confirm.m_seconds_left = 0.0f;
        confirm.m_countdown_active = false;
        return true;
    }

    bool tickDisplayConfirmPoll(DisplayConfirm &confirm, const float dt_seconds, const bool armed_this_poll) noexcept {
        if (armed_this_poll) {
            return false;
        }
        return tickDisplayConfirm(confirm, dt_seconds);
    }

    Expected<std::string> readOverlayBytes(
        std::istream &stream, const std::uintmax_t size, const std::filesystem::path &path,
        config::Diagnostics &diagnostics) {
        const auto failure = [&](const std::error_code error = config::make_error_code(
                                     config::EConfigErrc::codec_error)) -> Expected<std::string> {
            diagnostics.add(
                path.string(), "", "cannot read picture-settings file '" + path.string() + "'", error, "user-settings");
            PPR_LOG(GameDisplay, error, "display overlay read failed", {{"path", path.string()}});
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, error);
            std::unreachable(); // Every caller supplies a read failure.
        };
        try {
            std::string bytes{};
            std::array<char, 4096> block{};
            std::uintmax_t remaining = size;

            while (remaining > 0u) {
                const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(remaining, block.size()));
                stream.read(block.data(), count);
                if (stream.fail() or stream.bad() or stream.gcount() != count) {
                    return failure();
                }
                bytes.append(block.data(), static_cast<std::size_t>(count));
                remaining -= static_cast<std::uintmax_t>(count);
            }

            // A file that grew since sizing is not a complete overlay snapshot.
            if (stream.peek() != std::char_traits<char>::eof() or stream.bad() or not stream.eof()) {
                return failure();
            }
            return bytes;
        } catch (...) {
            return failure(displayException());
        }
    }

    Expected<std::optional<std::string>> readOverlayBytes(
        const std::filesystem::path &path, config::Diagnostics &diagnostics) {
        std::error_code probe{};
        const bool present = std::filesystem::exists(path, probe);
        // A set probe is never confused with absence. The measured missing,
        // unstatable, and malformed-name cases on this Windows STL returned
        // clean absence; other paths and implementations may report errors.
        if (probe) {
            diagnostics.add(
                path.string(), "", "cannot probe picture-settings file '" + path.string() + "'",
                config::make_error_code(config::EConfigErrc::codec_error), "user-settings");
            PPR_LOG(GameDisplay, error, "display overlay file unavailable", {{"path", path.string()}});
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, config::make_error_code(config::EConfigErrc::codec_error));
        }
        if (not present) {
            return std::optional<std::string>{std::nullopt};
        }

        std::ifstream file{path, std::ios::binary};
        if (not file) {
            diagnostics.add(
                path.string(), "", "cannot open picture-settings file '" + path.string() + "'",
                config::make_error_code(config::EConfigErrc::codec_error), "user-settings");
            PPR_LOG(GameDisplay, error, "display overlay file unavailable", {{"path", path.string()}});
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, config::make_error_code(config::EConfigErrc::codec_error));
        }

        std::error_code size_error{};
        const std::uintmax_t size = std::filesystem::file_size(path, size_error);
        if (size_error) {
            diagnostics.add(
                path.string(), "", "cannot read picture-settings file '" + path.string() + "'",
                config::make_error_code(config::EConfigErrc::codec_error), "user-settings");
            PPR_LOG(GameDisplay, error, "display overlay file unavailable", {{"path", path.string()}});
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, config::make_error_code(config::EConfigErrc::codec_error));
        }

        auto bytes = readOverlayBytes(file, size, path, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameDisplay, bytes);
        return std::optional<std::string>{std::move(*bytes)};
    }

    void disarmDisplayConfirm(DisplayConfirm &confirm) noexcept {
        confirm.m_armed = false;
        confirm.m_countdown_active = false;
        confirm.m_seconds_left = 0.0f;
        confirm.m_revert_pending = false;
    }
} // namespace pP::game
