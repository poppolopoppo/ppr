module;

#include "pP/Macros.h"

#include <fstream>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

module game.profile_capture;

import engine.app;
import engine.core;
import engine.math;
import game.config;
import game.display;

import std;

namespace pP::game {
    PPR_DEFINE_LOG_CATEGORY(ProfileCapture, info, none)

    namespace {
        [[nodiscard]] u64 currentProcessId_() noexcept {
#ifndef _WIN32
            return static_cast<u64>(::getpid());
#else
            return static_cast<u64>(::_getpid());
#endif
        }

        [[nodiscard]] double elapsedSeconds_(const TimePoint begin, const TimePoint now) noexcept {
            if (now < begin) {
                return 0.0;
            }
            return time::seconds(now - begin);
        }

        [[nodiscard]] std::string readTrimmed_(const std::filesystem::path &path) {
            std::ifstream stream{path, std::ios::binary};
            if (not stream) {
                return {};
            }

            std::string line{};
            std::getline(stream, line);
            while (not line.empty() and (line.back() == '\r' or line.back() == '\n' or line.back() == ' ')) {
                line.pop_back();
            }
            return line;
        }
    } // namespace

    // ------------------------------------------------------------------
    // vocabulary
    // ------------------------------------------------------------------

    std::string_view profileCaptureToolName(const EProfileCaptureTool tool) noexcept {
        switch (tool) {
            case EProfileCaptureTool::systems: return "systems";
            case EProfileCaptureTool::graphics: return "graphics";
        }
        return "systems";
    }

    std::string_view profileCaptureWorkloadName(const EProfileCaptureWorkload workload) noexcept {
        switch (workload) {
            case EProfileCaptureWorkload::frozen: return "frozen";
            case EProfileCaptureWorkload::live: return "live";
        }
        return "frozen";
    }

    std::string_view profileCaptureBackendName(const EProfileCaptureBackend backend) noexcept {
        switch (backend) {
            case EProfileCaptureBackend::vulkan: return "vulkan";
            case EProfileCaptureBackend::d3d12: return "d3d12";
            case EProfileCaptureBackend::unsupported: return "unsupported";
        }
        return "unsupported";
    }

    std::string_view profileCaptureStatusName(const EProfileCaptureStatus status) noexcept {
        switch (status) {
            case EProfileCaptureStatus::pending: return "pending";
            case EProfileCaptureStatus::ready: return "ready";
            case EProfileCaptureStatus::complete: return "complete";
            case EProfileCaptureStatus::incomplete: return "incomplete";
            case EProfileCaptureStatus::failed: return "failed";
            case EProfileCaptureStatus::unsupported: return "unsupported";
        }
        return "pending";
    }

    // ------------------------------------------------------------------
    // artifact encoding
    // ------------------------------------------------------------------

    std::string ProfileCapture::jsonString_(const std::string_view value) {
        std::string out{"\""};
        for (const char character: value) {
            switch (character) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += character; break;
            }
        }
        out += '"';
        return out;
    }

    std::string ProfileCapture::jsonVector_(const float3 &value) {
        return std::format("[{:.6f}, {:.6f}, {:.6f}]", value.x, value.y, value.z);
    }

    std::string ProfileCapture::jsonExtent_(const int2 &value) {
        return std::format("[{}, {}]", value.x, value.y);
    }

    std::string ProfileCapture::jsonSettings_() const {
        std::string out{"["};
        for (std::size_t index = 0; index < m_settings.size(); ++index) {
            if (index != 0u) {
                out += ',';
            }
            out += std::format(
                "\n      {{\"name\": {}, \"value\": {}}}", jsonString_(m_settings[index].m_name),
                jsonString_(m_settings[index].m_value));
        }
        if (not m_settings.empty()) {
            out += "\n    ";
        }
        out += ']';
        return out;
    }

    std::string ProfileCapture::readyJson_() const {
        return std::format(
            "{{\n"
            "  \"schema_version\": {},\n"
            "  \"pid\": {},\n"
            "  \"tool\": {},\n"
            "  \"workload\": {},\n"
            "  \"session_token\": {},\n"
            "  \"world_id\": {},\n"
            "  \"publication_id\": {},\n"
            "  \"tick\": {},\n"
            "  \"accepted_scene_frames\": {},\n"
            "  \"active_presentation_seconds\": {:.6f},\n"
            "  \"lighting_frame_index\": {},\n"
            "  \"screen_frame_index\": {},\n"
            "  \"camera\": {{\"origin\": {}, \"forward\": {}}},\n"
            "  \"extents\": {{\"logical\": {}, \"framebuffer\": {}}},\n"
            "  \"settings\": {}\n"
            "}}\n",
            kProfileCaptureSchemaVersion, m_process_id, jsonString_(profileCaptureToolName(m_tool)),
            jsonString_(profileCaptureWorkloadName(m_workload)), jsonString_(m_session_token), m_world_id,
            m_publication_id, m_tick, m_accepted_frames, m_active_seconds, m_scene_frame_index, m_screen_frame_index,
            jsonVector_(m_camera_origin), jsonVector_(m_camera_forward), jsonExtent_(m_logical_extent),
            jsonExtent_(m_framebuffer_extent), jsonSettings_());
    }

    std::string ProfileCapture::finalStateJson_() const {
        const bool trace_finalized = m_sdk_status.m_trace_files > 0u and
                                     not m_sdk_status.m_tracing and
                                     not m_sdk_status.m_draining;
        return std::format(
            "{{\n"
            "  \"schema_version\": {},\n"
            "  \"pid\": {},\n"
            "  \"tool\": {},\n"
            "  \"workload\": {},\n"
            "  \"backend\": {},\n"
            "  \"session_token\": {},\n"
            "  \"status\": {},\n"
            "  \"reason\": {},\n"
            "  \"ready\": {},\n"
            "  \"accepted_scene_frames\": {},\n"
            "  \"ui_only_frames\": {},\n"
            "  \"rejected_frames\": {},\n"
            "  \"active_presentation_seconds\": {:.6f},\n"
            "  \"ready_tick\": {},\n"
            "  \"stop_tick\": {},\n"
            "  \"lighting_frame_start\": {},\n"
            "  \"lighting_frame_end\": {},\n"
            "  \"screen_frame_start\": {},\n"
            "  \"screen_frame_end\": {},\n"
            "  \"frozen_pause_satisfied\": {},\n"
            "  \"pause_tick\": {},\n"
            "  \"tick_preceded_pause\": {},\n"
            "  \"simulation_advanced_while_frozen\": {},\n"
            "  \"sdk_available\": {},\n"
            "  \"sdk_operations\": {},\n"
            "  \"sdk_status\": {},\n"
            "  \"sdk_error\": {},\n"
            "  \"trace_finalized\": {},\n"
            "  \"trace_files\": {},\n"
            "  \"trace_path\": {},\n"
            "  \"cleanup_error\": {},\n"
            "  \"elapsed_seconds\": {:.6f}\n"
            "}}\n",
            kProfileCaptureSchemaVersion, m_process_id, jsonString_(profileCaptureToolName(m_tool)),
            jsonString_(profileCaptureWorkloadName(m_workload)), jsonString_(profileCaptureBackendName(m_backend)),
            jsonString_(m_session_token), jsonString_(profileCaptureStatusName(m_status)), jsonString_(m_reason),
            m_ready_written, m_accepted_frames, m_ui_only_frames, m_rejected_frames, m_active_seconds, m_ready_tick,
            m_stop_tick, m_ready_scene_frame, m_scene_frame_index, m_ready_screen_frame, m_screen_frame_index,
            m_frozen_pause_satisfied, m_pause_tick, m_tick_preceded_pause, m_frozen_advanced, m_sdk != nullptr,
            m_sdk_operations, m_sdk_status.m_status, jsonString_(m_sdk_error ? m_sdk_error.message() : std::string{}),
            trace_finalized, m_sdk_status.m_trace_files, jsonString_(m_sdk_status.m_trace_path),
            jsonString_(m_cleanup_error ? m_cleanup_error.message() : std::string{}),
            elapsedSeconds_(m_begin, m_last_now));
    }

    std::error_code ProfileCapture::writeReady_() {
        m_ready_tick = m_tick;
        m_ready_scene_frame = m_scene_frame_index;
        m_ready_screen_frame = m_screen_frame_index;

        PPR_RETURN_ERROR_ON_FAIL(ProfileCapture, writeFileAtomically(m_ready_path, readyJson_()));

        m_ready_written = true;
        writeLog_(
            std::format(
                "ready frames={} active_s={:.3f} tick={} world={} publication={}", m_accepted_frames, m_active_seconds,
                m_tick, m_world_id, m_publication_id));
        return {};
    }

    std::error_code ProfileCapture::writeFinalState_(const bool force) {
        if (m_final_written and not force) {
            return {};
        }
        if (const std::error_code error = writeFileAtomically(m_final_state_path, finalStateJson_())) {
            return error;
        }

        m_final_written = true;
        writeLog_(std::format("final status={} reason={}", profileCaptureStatusName(m_status), m_reason));
        return {};
    }

    void ProfileCapture::writeLog_(const std::string_view line) {
        if (m_log_path.empty()) {
            return;
        }
        try {
            std::ofstream log{m_log_path, std::ios::binary | std::ios::app};
            if (log) {
                log << std::format("[{:+.3f}] {}\n", elapsedSeconds_(m_begin, m_last_now), line);
            }
        } catch (...) {
            // Diagnostics are best effort; the atomic JSON records remain the
            // authoritative evidence and are rewritten on the next transition.
        }
    }

    // ------------------------------------------------------------------
    // capture driver
    // ------------------------------------------------------------------

    std::error_code ProfileCapture::begin(
        const std::filesystem::path &output, const EProfileCaptureTool tool, const EProfileCaptureWorkload workload,
        const std::string_view session_token, const u64 process_id, IProfileCaptureSdk *const sdk,
        const TimePoint now) {
        // Fresh artifact directory only: a stale/non-empty directory is
        // rejected instead of mixing another run's reports, logs or exports.
        std::error_code error{};
        if (std::filesystem::exists(output, error)) {
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(ProfileCapture, error);
            }
            if (not std::filesystem::is_directory(output, error) or error) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ProfileCapture, error ? error : std::make_error_code(std::errc::not_a_directory));
            }
            if (not std::filesystem::is_empty(output, error) or error) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ProfileCapture, error ? error : std::make_error_code(std::errc::directory_not_empty));
            }
        } else {
            std::filesystem::create_directories(output, error);
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(ProfileCapture, error);
            }
        }

        m_output = output;
        m_log_path = output / kProfileCaptureLogFileName;
        m_ready_path = output / kProfileCaptureReadyFileName;
        m_final_state_path = output / kProfileCaptureFinalStateFileName;
        m_stop_request_path = output / kProfileCaptureStopRequestFileName;
        m_tool = tool;
        m_workload = workload;
        m_process_id = process_id == 0u ? currentProcessId_() : process_id;
        m_session_token = session_token.empty() ? std::format("ppr-profile-{}", m_process_id)
                                                : std::string{session_token};
        m_sdk = sdk;
        m_begin = now;
        m_last_now = now;
        m_phase = EPhase::warming;
        m_status = EProfileCaptureStatus::pending;

        {
            std::ofstream log{m_log_path, std::ios::binary | std::ios::trunc};
        }
        writeLog_(
            std::format(
                "begin tool={} workload={} pid={} session={} output={}", profileCaptureToolName(m_tool),
                profileCaptureWorkloadName(m_workload), m_process_id, m_session_token, m_output.string()));

        // A requested Graphics capture with no profiling SDK build/adapter is
        // an explicit unsupported result, never a silent Systems substitute.
        if (m_tool == EProfileCaptureTool::graphics and (m_sdk == nullptr or not m_sdk->available())) {
            complete_(
                EProfileCaptureStatus::unsupported,
                "Nsight Graphics SDK adapter unavailable: configure the profiling build with the installed SDK", now);
        }

        return {};
    }

    void ProfileCapture::noteBackend(const EProfileCaptureBackend backend, const TimePoint now) {
        m_backend = backend;
        if (m_tool == EProfileCaptureTool::graphics and
            backend != EProfileCaptureBackend::vulkan and
            m_phase != EPhase::finished) {
            // The installed adapter is Vulkan-only. A D3D12 (or unknown) device
            // queue must never be reinterpreted as a Vulkan queue.
            complete_(
                EProfileCaptureStatus::unsupported,
                std::format(
                    "Nsight Graphics SDK adapter supports Vulkan only; observed backend is {}",
                    profileCaptureBackendName(m_backend)),
                now);
        }
    }

    Expected<std::optional<ProfileCaptureAction>> ProfileCapture::advance(
        const ProfileCaptureObservation &observed, const TimePoint now) {
        if (m_phase == EPhase::idle) {
            return std::optional<ProfileCaptureAction>{};
        }

        m_last_now = now;
        if (not observed.m_settings.empty()) {
            m_settings = observed.m_settings;
        }
        noteAccepted_(observed);
        captureIdentity_(observed);
        noteSdkResult_(observed);

        if (m_phase == EPhase::finished) {
            // Completion requests the ordinary quit unless the application
            // already asked to close or reported an operational error.
            if (m_suppress_quit) {
                return std::optional<ProfileCaptureAction>{};
            }
            return publish_(ProfileCaptureAction{EProfileCaptureActionKind::quit});
        }

        if (observed.m_shutdown_error) {
            complete_(
                EProfileCaptureStatus::incomplete, "application reported an operational error during capture", now,
                true);
            return std::optional<ProfileCaptureAction>{};
        }

        if (observed.m_close_requested) {
            complete_(EProfileCaptureStatus::incomplete, "application closed during capture", now, true);
            return std::optional<ProfileCaptureAction>{};
        }

        if (const double total = elapsedSeconds_(m_begin, now);
            total > static_cast<double>(kProfileCaptureTotalDeadlineSeconds)) {
            complete_(
                EProfileCaptureStatus::incomplete, std::format("total {:.0f} s capture deadline exceeded", total), now);
            return std::optional<ProfileCaptureAction>{};
        }

        if (pollStopRequest_(now)) {
            // The request is a completion; the ordinary quit is published below.
        }
        if (m_phase == EPhase::finished) {
            if (not m_suppress_quit) {
                return publish_(ProfileCaptureAction{EProfileCaptureActionKind::quit});
            }
            return std::optional<ProfileCaptureAction>{};
        }

        if (const auto action = run_(observed, now); action and action->has_value()) {
            return action;
        }

        // A completion reached in this turn requests the ordinary quit
        // immediately unless the application already owns its shutdown.
        if (m_phase == EPhase::finished and not m_suppress_quit) {
            return publish_(ProfileCaptureAction{EProfileCaptureActionKind::quit});
        }
        return std::optional<ProfileCaptureAction>{};
    }

    void ProfileCapture::notePauseApplied(const u64 tick) noexcept {
        if (m_frozen_pause_satisfied) {
            return;
        }
        m_frozen_pause_satisfied = true;
        m_pause_tick = tick;
        if (tick > 0u) {
            m_tick_preceded_pause = true;
        }
        writeLog_(std::format("frozen pause applied at tick {}", tick));
    }

    void ProfileCapture::noteAccepted_(const ProfileCaptureObservation &observed) {
        m_accepted_frames += observed.m_accepted_scene_frames;
        m_ui_only_frames += observed.m_ui_only_frames;
        m_rejected_frames += observed.m_rejected_frames;
        if (observed.m_active_presentation_seconds > 0.0) {
            m_active_seconds += observed.m_active_presentation_seconds;
        }
    }

    void ProfileCapture::captureIdentity_(const ProfileCaptureObservation &observed) {
        m_world_admitted = observed.m_world_admitted;
        m_tick = observed.m_tick;
        m_scene_frame_index = observed.m_scene_frame_index;
        m_screen_frame_index = observed.m_screen_frame_index;
        m_world_id = observed.m_world_id;
        m_publication_id = observed.m_publication_id;
        m_camera_origin = observed.m_camera_origin;
        m_camera_forward = observed.m_camera_forward;
        m_logical_extent = observed.m_logical_extent;
        m_framebuffer_extent = observed.m_framebuffer_extent;

        if (frozen() and m_frozen_pause_satisfied and observed.m_tick > m_pause_tick) {
            if (not m_frozen_advanced) {
                writeLog_(
                    std::format(
                        "warning: simulation advanced from tick {} to {} while frozen", m_pause_tick, observed.m_tick));
            }
            m_frozen_advanced = true;
        }
    }

    void ProfileCapture::noteSdkResult_(const ProfileCaptureObservation &observed) {
        if (not observed.m_sdk_operation_completed) {
            return;
        }

        m_sdk_poll_pending = false;
        m_sdk_status = observed.m_sdk_status;
        if (observed.m_sdk_operation_error) {
            m_sdk_error = observed.m_sdk_operation_error;
            writeLog_(
                std::format(
                    "sdk operation {} failed: {}", static_cast<u32>(observed.m_sdk_operation),
                    observed.m_sdk_operation_error.message()));
            return;
        }
        m_sdk_error = {};

        switch (observed.m_sdk_operation) {
            case EProfileCaptureSdkOperation::initialize: m_sdk_initialized = true; break;
            case EProfileCaptureSdkOperation::activate: m_sdk_active = true; break;
            case EProfileCaptureSdkOperation::start_trace:
            case EProfileCaptureSdkOperation::poll:
            case EProfileCaptureSdkOperation::none: break;
        }
    }

    std::optional<ProfileCaptureAction> ProfileCapture::publish_(const ProfileCaptureAction action) {
        if (action.m_kind == EProfileCaptureActionKind::quit) {
            if (m_quit_emitted) {
                return std::nullopt;
            }
            m_quit_emitted = true;
        }
        return action;
    }

    std::optional<ProfileCaptureAction> ProfileCapture::emitSdk_(
        const EProfileCaptureSdkOperation operation, const TimePoint now) {
        if (m_sdk == nullptr or not m_sdk->available()) {
            complete_(
                EProfileCaptureStatus::unsupported,
                "Nsight Graphics SDK adapter unavailable for a requested Graphics capture", now);
            return std::nullopt;
        }
        if (m_sdk_poll_pending) {
            return std::nullopt;
        }

        m_sdk_poll_pending = true;
        ++m_sdk_operations;
        return publish_(ProfileCaptureAction{EProfileCaptureActionKind::sdk_operation, operation});
    }

    Expected<std::optional<ProfileCaptureAction>> ProfileCapture::run_(
        const ProfileCaptureObservation &observed, const TimePoint now) {
        switch (m_phase) {
            case EPhase::warming: return advanceWarming_(observed, now);
            case EPhase::ready: return advanceReady_(now);
            case EPhase::tracing: return advanceTracing_(now);
            case EPhase::draining: return advanceDraining_(now);
            case EPhase::idle:
            case EPhase::finished: break;
        }
        return std::optional<ProfileCaptureAction>{};
    }

    std::optional<ProfileCaptureAction> ProfileCapture::advanceWarming_(
        const ProfileCaptureObservation &observed, const TimePoint now) {
        if (not observed.m_world_admitted) {
            if (elapsedSeconds_(m_begin, now) > static_cast<double>(kProfileCaptureReadyDeadlineSeconds)) {
                complete_(
                    EProfileCaptureStatus::incomplete, "readiness deadline exceeded before the world was admitted",
                    now);
            }
            return std::nullopt;
        }

        // Frozen capture pauses through the production action after world
        // admission and before the first simulation tick.
        if (frozen() and not m_frozen_pause_satisfied) {
            if (observed.m_tick > 0u) {
                m_tick_preceded_pause = true;
                writeLog_(std::format("warning: simulation tick {} observed before the frozen pause", observed.m_tick));
            }
            return publish_(ProfileCaptureAction{EProfileCaptureActionKind::pause_simulation});
        }

        const bool frames_ready = m_accepted_frames >= kProfileCaptureReadyFrames;
        const bool active_ready = m_active_seconds >= static_cast<double>(kProfileCaptureReadyActiveSeconds);
        if (not frames_ready or not active_ready) {
            if (elapsedSeconds_(m_begin, now) > static_cast<double>(kProfileCaptureReadyDeadlineSeconds)) {
                complete_(
                    EProfileCaptureStatus::incomplete,
                    std::format(
                        "readiness deadline exceeded: {} accepted frames and {:.3f} s active presentation",
                        m_accepted_frames, m_active_seconds),
                    now);
            }
            return std::nullopt;
        }

        if (const std::error_code error = writeReady_()) {
            complete_(
                EProfileCaptureStatus::failed, std::format("ready record write failed: {}", error.message()), now);
            return std::nullopt;
        }

        m_status = EProfileCaptureStatus::ready;
        m_phase = EPhase::ready;
        m_poll_time = TimePoint{};
        writeLog_("collection ready");
        return std::nullopt;
    }

    std::optional<ProfileCaptureAction> ProfileCapture::advanceReady_(const TimePoint now) {
        if (m_tool == EProfileCaptureTool::systems) {
            return std::nullopt;
        }
        if (now - m_poll_time < TimeSpan{std::chrono::milliseconds{50}}) {
            return std::nullopt;
        }
        m_poll_time = now;

        if (m_sdk_error) {
            complete_(
                EProfileCaptureStatus::failed, std::format("graphics SDK operation failed: {}", m_sdk_error.message()),
                now);
            return std::nullopt;
        }
        if (not m_sdk_initialized) {
            return emitSdk_(EProfileCaptureSdkOperation::initialize, now);
        }
        if (not m_sdk_active) {
            return emitSdk_(EProfileCaptureSdkOperation::activate, now);
        }

        if (const auto action = emitSdk_(EProfileCaptureSdkOperation::start_trace, now)) {
            m_sdk_started = true;
            m_trace_start = now;
            m_phase = EPhase::tracing;
            writeLog_("graphics trace started through the SDK");
            return action;
        }
        return std::nullopt;
    }

    std::optional<ProfileCaptureAction> ProfileCapture::advanceTracing_(const TimePoint now) {
        if (m_sdk_status.m_errored or m_sdk_error) {
            complete_(
                EProfileCaptureStatus::failed,
                std::format("graphics trace failed: {}", m_sdk_error ? m_sdk_error.message() : "vendor status errored"),
                now);
            return std::nullopt;
        }

        if (m_sdk_status.m_trace_files > 0u and not m_sdk_status.m_tracing and not m_sdk_status.m_draining) {
            m_phase = EPhase::draining;
            writeLog_(
                std::format(
                    "graphics trace finalized files={} path={}", m_sdk_status.m_trace_files,
                    m_sdk_status.m_trace_path));
            return std::nullopt;
        }

        const double since_start = elapsedSeconds_(m_trace_start, now);
        if (since_start > static_cast<double>(kProfileCaptureFinalizeDeadlineSeconds)) {
            complete_(
                EProfileCaptureStatus::incomplete,
                std::format("graphics trace did not finalize within {:.0f} s", since_start), now);
            return std::nullopt;
        }

        if (now - m_poll_time < TimeSpan{std::chrono::milliseconds{50}}) {
            return std::nullopt;
        }
        m_poll_time = now;
        return emitSdk_(EProfileCaptureSdkOperation::poll, now);
    }

    std::optional<ProfileCaptureAction> ProfileCapture::advanceDraining_(const TimePoint now) {
        if (not m_sdk_status.m_trace_path.empty()) {
            complete_(EProfileCaptureStatus::complete, "graphics trace finalized and exported by the tool", now);
            return std::nullopt;
        }

        const double since_start = elapsedSeconds_(m_trace_start, now);
        if (since_start > static_cast<double>(kProfileCaptureFinalizeDeadlineSeconds)) {
            complete_(
                EProfileCaptureStatus::incomplete,
                std::format("graphics trace finalization deadline exceeded after {:.0f} s", since_start), now);
            return std::nullopt;
        }

        if (now - m_poll_time < TimeSpan{std::chrono::milliseconds{50}}) {
            return std::nullopt;
        }
        m_poll_time = now;
        return emitSdk_(EProfileCaptureSdkOperation::poll, now);
    }

    bool ProfileCapture::pollStopRequest_(const TimePoint now) {
        // A supervisor can ask for the ordinary quit in any phase, including
        // during warm-up, tracing and draining; a wrong token never stops us.
        if (now - m_stop_poll_time < TimeSpan{std::chrono::milliseconds{100}}) {
            return false;
        }
        m_stop_poll_time = now;

        const std::string token = readTrimmed_(m_stop_request_path);
        if (token.empty()) {
            return false;
        }
        if (token != m_session_token) {
            writeLog_(std::format("ignored stop request for another session: {}", token));
            return false;
        }

        if (m_phase == EPhase::warming or not m_ready_written) {
            complete_(EProfileCaptureStatus::incomplete, "matching session stop request before readiness", now);
        } else if (m_tool == EProfileCaptureTool::graphics and
                   (m_sdk_status.m_trace_files == 0u or m_sdk_status.m_tracing or m_sdk_status.m_draining)) {
            complete_(
                EProfileCaptureStatus::incomplete, "matching session stop request before the trace finalized", now);
        } else {
            complete_(EProfileCaptureStatus::complete, "matching session stop request", now);
        }
        return true;
    }

    void ProfileCapture::complete_(
        const EProfileCaptureStatus status, std::string reason, const TimePoint now, const bool suppress_quit) {
        if (m_phase == EPhase::finished) {
            return;
        }

        m_last_now = now;
        m_status = status;
        m_reason = std::move(reason);
        m_phase = EPhase::finished;
        m_stop_tick = m_tick;
        m_suppress_quit = suppress_quit;
        writeLog_(std::format("complete status={} reason={}", profileCaptureStatusName(m_status), m_reason));

        if (const std::error_code error = writeFinalState_()) {
            writeLog_(std::format("final-state write failed: {}", error.message()));
        }
    }

    std::error_code ProfileCapture::finish(const std::error_code cleanup_error) {
        m_cleanup_error = cleanup_error;
        bool rewrite = false;

        if (not m_finalized) {
            m_finalized = true;
            if (m_phase != EPhase::finished) {
                complete_(
                    EProfileCaptureStatus::incomplete, "application shutdown before capture completion", m_last_now);
                rewrite = true;
            } else if (cleanup_error and m_status == EProfileCaptureStatus::complete) {
                m_status = EProfileCaptureStatus::incomplete;
                m_reason += std::format("; shutdown cleanup failed: {}", cleanup_error.message());
                rewrite = true;
            }
        } else if (cleanup_error and m_status == EProfileCaptureStatus::complete) {
            m_status = EProfileCaptureStatus::incomplete;
            m_reason += std::format("; shutdown cleanup failed: {}", cleanup_error.message());
            rewrite = true;
        }

        if (rewrite or not m_final_written) {
            if (const std::error_code error = writeFinalState_(true)) {
                return error;
            }
        }

        if (m_status == EProfileCaptureStatus::complete) {
            return {};
        }
        return std::make_error_code(std::errc::state_not_recoverable);
    }
} // namespace pP::game
