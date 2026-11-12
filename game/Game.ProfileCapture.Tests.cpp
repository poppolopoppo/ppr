module;

#include "pP/UnitTest.h"

module game.tests;

import engine.app;
import engine.core;
import engine.math;
import game.config;
import game.profile_capture;

import std;

namespace pP::game::tests::detail::profile_capture {
    // ------------------------------------------------------------------
    // fixtures
    // ------------------------------------------------------------------

    [[nodiscard]] TimePoint startTime() {
        return TimePoint{} + std::chrono::seconds{1000};
    }

    [[nodiscard]] std::filesystem::path captureDirectory(const std::string_view name) {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                                "ppr-profile-capture-tests" /
                                                std::string{name};
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
        return directory;
    }

    [[nodiscard]] std::string readFile(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    struct FakeSdk final : IProfileCaptureSdk {
        bool m_available{true};
        bool m_fail_initialize{};
        u32 m_initialize_calls{};
        u32 m_activate_calls{};
        u32 m_start_calls{};
        u32 m_poll_calls{};
        void *m_queue{};
        EProfileCaptureBackend m_backend{EProfileCaptureBackend::vulkan};
        ProfileCaptureSdkStatus m_status{};

        [[nodiscard]] bool available() const noexcept override {
            return m_available;
        }

        std::error_code initialize() override {
            ++m_initialize_calls;
            if (m_fail_initialize) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }

            m_status.m_injected = true;
            m_status.m_initialized = true;
            return {};
        }

        std::error_code activate(void *const native_queue, const EProfileCaptureBackend backend) override {
            ++m_activate_calls;
            m_backend = backend;
            if (backend != EProfileCaptureBackend::vulkan) {
                return std::make_error_code(std::errc::not_supported);
            }
            m_queue = native_queue;
            m_status.m_active = true;
            return {};
        }

        std::error_code startTrace() override {
            ++m_start_calls;
            m_status.m_tracing = true;
            return {};
        }

        std::error_code poll(ProfileCaptureSdkStatus &out_status) override {
            ++m_poll_calls;
            out_status = m_status;
            return {};
        }
    };

    [[nodiscard]] ProfileCaptureObservation admittedObservation(const u64 tick = 0u) {
        ProfileCaptureObservation observation{};
        observation.m_world_admitted = true;
        observation.m_tick = tick;
        observation.m_world_id = 7u;
        observation.m_publication_id = 11u;
        observation.m_scene_frame_index = 40u;
        observation.m_screen_frame_index = 40u;
        observation.m_camera_origin = float3{1.0f, 2.0f, 3.0f};
        observation.m_camera_forward = float3{0.0f, 0.0f, 1.0f};
        observation.m_logical_extent = int2{1280, 720};
        observation.m_framebuffer_extent = int2{1280, 720};
        observation.m_settings.push_back(ProfileCaptureSetting{"worldgen_profile", "lighting_lab"});
        return observation;
    }

    /// Drives the frozen admission pause and the exact readiness thresholds.
    void driveToReady(ProfileCapture &capture, const TimePoint start) {
        bool paused = false;
        for (u32 turn = 0u; turn < 8u and not paused; ++turn) {
            auto action = capture.advance(admittedObservation(), start);
            PPR_TEST_ASSERT(action.has_value());
            if (not action->has_value()) {
                continue;
            }
            if ((**action).m_kind == EProfileCaptureActionKind::pause_simulation) {
                capture.notePauseApplied(0u);
                paused = true;
            }
        }
        PPR_TEST_ASSERT(paused or capture.pauseBeforeFirstTickSatisfied());

        ProfileCaptureObservation frames = admittedObservation();
        frames.m_accepted_scene_frames = 100u;
        frames.m_active_presentation_seconds = 1.0;
        PPR_TEST_ASSERT(capture.advance(frames, start + std::chrono::seconds{1}).has_value());

        ProfileCaptureObservation ready = admittedObservation();
        ready.m_accepted_scene_frames = 20u;
        ready.m_active_presentation_seconds = 1.0;
        PPR_TEST_ASSERT(capture.advance(ready, start + std::chrono::seconds{2}).has_value());
        PPR_TEST_ASSERT(capture.readyWritten());
    }

    /// Executes one requested vendor operation through the fake adapter, as
    /// the real host does on the device-owner lane.
    std::error_code observeSdkAction(FakeSdk &sdk, const EProfileCaptureSdkOperation operation) {
        switch (operation) {
            case EProfileCaptureSdkOperation::initialize: return sdk.initialize();
            case EProfileCaptureSdkOperation::activate:
                return sdk.activate(
                    reinterpret_cast<void *>(static_cast<std::uintptr_t>(1u)), EProfileCaptureBackend::vulkan);
            case EProfileCaptureSdkOperation::start_trace: return sdk.startTrace();
            case EProfileCaptureSdkOperation::poll: {
                ProfileCaptureSdkStatus status{};
                const std::error_code result = sdk.poll(status);
                return result;
            }
            case EProfileCaptureSdkOperation::none: return std::make_error_code(std::errc::invalid_argument);
        }
        return {};
    }

    // ------------------------------------------------------------------
    // readiness
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (readiness_requires_120_accepted_frames_and_two_active_seconds) {
        const auto start = startTime();
        const auto directory = captureDirectory("readiness");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));
        PPR_TEST_ASSERT(not capture.readyWritten());
        PPR_TEST_ASSERT(capture.pauseBeforeFirstTickPending());

        bool paused = false;
        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        PPR_TEST_ASSERT((*pause)->m_kind == EProfileCaptureActionKind::pause_simulation);
        capture.notePauseApplied(0u);
        paused = true;
        PPR_TEST_ASSERT(paused);
        PPR_TEST_ASSERT(not capture.pauseBeforeFirstTickPending());
        PPR_TEST_ASSERT(not capture.tickPrecededPause());

        // Plenty of active presentation but too few accepted frames.
        ProfileCaptureObservation long_active = admittedObservation();
        long_active.m_accepted_scene_frames = 100u;
        long_active.m_active_presentation_seconds = 3.0;
        PPR_TEST_ASSERT(capture.advance(long_active, start + std::chrono::seconds{1}).has_value());
        PPR_TEST_ASSERT(not capture.readyWritten());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::pending);
        PPR_TEST_ASSERT(capture.acceptedFrames() == 100u);

        ProfileCaptureObservation short_by_one = admittedObservation();
        short_by_one.m_accepted_scene_frames = 19u;
        PPR_TEST_ASSERT(capture.advance(short_by_one, start + std::chrono::seconds{2}).has_value());
        PPR_TEST_ASSERT(not capture.readyWritten());
        PPR_TEST_ASSERT(capture.acceptedFrames() == 119u);

        // The 120th accepted frame crosses the frame threshold; active time was
        // already satisfied.
        ProfileCaptureObservation ready = admittedObservation();
        ready.m_accepted_scene_frames = 1u;
        PPR_TEST_ASSERT(capture.advance(ready, start + std::chrono::seconds{3}).has_value());
        PPR_TEST_ASSERT(capture.readyWritten());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::ready);
        PPR_TEST_ASSERT(capture.acceptedFrames() == 120u);
        PPR_TEST_ASSERT(capture.activePresentationSeconds() >= 3.0);

        // A second capture needs the active-presentation threshold as well:
        // plenty of frames and only 0.5 s is not ready.
        const auto active_directory = captureDirectory("readiness_active");
        ProfileCapture active_capture{};
        PPR_TEST_ASSERT(not active_capture.begin(
            active_directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::live, "s1", 1u, nullptr, start));
        ProfileCaptureObservation many_frames = admittedObservation();
        many_frames.m_accepted_scene_frames = 500u;
        many_frames.m_active_presentation_seconds = 0.5;
        PPR_TEST_ASSERT(active_capture.advance(many_frames, start + std::chrono::seconds{1}).has_value());
        PPR_TEST_ASSERT(not active_capture.readyWritten());
        many_frames.m_accepted_scene_frames = 0u;
        many_frames.m_active_presentation_seconds = 1.5;
        PPR_TEST_ASSERT(active_capture.advance(many_frames, start + std::chrono::seconds{2}).has_value());
        PPR_TEST_ASSERT(active_capture.readyWritten());

        const std::string ready_json = readFile(directory / "ready.json");
        PPR_TEST_ASSERT(ready_json.find("\"schema_version\": 1") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("\"world_id\": 7") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("\"tick\": 0") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("\"lighting_frame_index\": 40") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("\"logical\": [1280, 720]") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("\"camera\"") != std::string::npos);
        PPR_TEST_ASSERT(ready_json.find("lighting_lab") != std::string::npos);
    };

    PPR_UNIT_TEST (ui_only_and_rejected_frames_are_excluded_from_readiness) {
        const auto start = startTime();
        const auto directory = captureDirectory("exclusions");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));

        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        capture.notePauseApplied(0u);

        ProfileCaptureObservation noise = admittedObservation();
        noise.m_ui_only_frames = 500u;
        noise.m_rejected_frames = 200u;
        noise.m_active_presentation_seconds = 10.0;
        PPR_TEST_ASSERT(capture.advance(noise, start + std::chrono::seconds{5}).has_value());

        PPR_TEST_ASSERT(capture.acceptedFrames() == 0u);
        PPR_TEST_ASSERT(not capture.readyWritten());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::pending);
        PPR_TEST_ASSERT(not readFile(directory / "profile-capture.log").empty());

        // Retained UI frames and dropped frames remain visible in the final
        // record but never reach readiness.
        PPR_TEST_ASSERT(capture.finish({}) == std::errc::state_not_recoverable);
        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"ui_only_frames\": 500") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("\"rejected_frames\": 200") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("\"status\": \"incomplete\"") != std::string::npos);
    };

    PPR_UNIT_TEST (frozen_capture_pauses_before_the_first_tick) {
        const auto start = startTime();
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            captureDirectory("pause"), EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr,
            start));

        const auto action = capture.advance(admittedObservation(0u), start);
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((*action)->m_kind == EProfileCaptureActionKind::pause_simulation);
        PPR_TEST_ASSERT(not capture.pauseBeforeFirstTickSatisfied());
        capture.notePauseApplied(0u);
        PPR_TEST_ASSERT(capture.pauseBeforeFirstTickSatisfied());
        PPR_TEST_ASSERT(not capture.tickPrecededPause());

        // A tick observed before the pause is retained as an explicit defect.
        const auto late_directory = captureDirectory("pause_late");
        ProfileCapture late{};
        PPR_TEST_ASSERT(not late.begin(
            late_directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));
        const auto late_action = late.advance(admittedObservation(3u), start);
        PPR_TEST_ASSERT(late_action.has_value() and late_action->has_value());
        late.notePauseApplied(3u);
        PPR_TEST_ASSERT(late.tickPrecededPause());
        PPR_TEST_ASSERT(static_cast<bool>(late.finish({})));
        PPR_TEST_ASSERT(
            readFile(late_directory / "final-state.json").find("\"tick_preceded_pause\": true") != std::string::npos);
    };

    PPR_UNIT_TEST (live_workload_does_not_pause) {
        const auto start = startTime();
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            captureDirectory("live"), EProfileCaptureTool::systems, EProfileCaptureWorkload::live, "s1", 1u, nullptr,
            start));
        PPR_TEST_ASSERT(not capture.pauseBeforeFirstTickPending());

        ProfileCaptureObservation observation = admittedObservation(10u);
        observation.m_accepted_scene_frames = 120u;
        observation.m_active_presentation_seconds = 2.0;
        const auto action = capture.advance(observation, start + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(capture.readyWritten());
        PPR_TEST_ASSERT(not capture.pauseBeforeFirstTickSatisfied());
    };

    // ------------------------------------------------------------------
    // stop rendezvous and shutdown
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (wrong_session_stop_token_is_ignored) {
        const auto start = startTime();
        const auto directory = captureDirectory("stop_token");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "session-a", 1u, nullptr, start));
        driveToReady(capture, start);

        {
            std::ofstream stale{directory / "stop.request", std::ios::binary | std::ios::trunc};
            stale << "session-b\n";
        }
        const auto ignored = capture.advance(admittedObservation(), start + std::chrono::seconds{3});
        PPR_TEST_ASSERT(ignored.has_value() and not ignored->has_value());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::ready);
        PPR_TEST_ASSERT(
            readFile(directory / "profile-capture.log").find("ignored stop request for another session: session-b") !=
            std::string::npos);

        {
            std::ofstream matching{directory / "stop.request", std::ios::binary | std::ios::trunc};
            matching << "session-a\n";
        }
        const auto quit = capture.advance(admittedObservation(), start + std::chrono::seconds{4});
        PPR_TEST_ASSERT(quit.has_value() and quit->has_value());
        PPR_TEST_ASSERT((*quit)->m_kind == EProfileCaptureActionKind::quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::complete);
        PPR_TEST_ASSERT(not capture.finish({}));

        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"status\": \"complete\"") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("\"ready\": true") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("matching session stop request") != std::string::npos);
    };

    PPR_UNIT_TEST (close_during_capture_finalizes_incomplete_and_uses_ordinary_shutdown) {
        const auto start = startTime();
        const auto directory = captureDirectory("close");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));

        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        capture.notePauseApplied(0u);

        ProfileCaptureObservation closed = admittedObservation();
        closed.m_accepted_scene_frames = 10u;
        closed.m_close_requested = true;
        const auto action = capture.advance(closed, start + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);

        // Ordinary shutdown already owns the exit; no quit is emitted after a
        // real close.
        const auto afterwards = capture.advance(admittedObservation(), start + std::chrono::seconds{2});
        PPR_TEST_ASSERT(afterwards.has_value() and not afterwards->has_value());

        PPR_TEST_ASSERT(capture.finish({}) == std::errc::state_not_recoverable);
        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"status\": \"incomplete\"") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("application closed during capture") != std::string::npos);

        const std::error_code cleanup = std::make_error_code(std::errc::state_not_recoverable);
        PPR_TEST_ASSERT(capture.finish(cleanup) == std::errc::state_not_recoverable);
    };

    PPR_UNIT_TEST (readiness_deadline_reports_incomplete_and_invokes_ordinary_quit) {
        const auto start = startTime();
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            captureDirectory("deadline"), EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u,
            nullptr, start));

        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        capture.notePauseApplied(0u);

        ProfileCaptureObservation slow = admittedObservation();
        slow.m_accepted_scene_frames = 10u;
        slow.m_active_presentation_seconds = 0.2;
        const auto action = capture.advance(slow, start + std::chrono::seconds{181});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((*action)->m_kind == EProfileCaptureActionKind::quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);
        PPR_TEST_ASSERT(not capture.readyWritten());
    };

    PPR_UNIT_TEST (output_directory_must_be_fresh) {
        const auto start = startTime();
        const auto directory = captureDirectory("fresh");
        std::filesystem::create_directories(directory);
        {
            std::ofstream existing{directory / "stale.txt", std::ios::binary | std::ios::trunc};
        }

        ProfileCapture capture{};
        PPR_TEST_ASSERT(
            capture.begin(
                directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start) ==
            std::errc::directory_not_empty);
    };

    // ------------------------------------------------------------------
    // vendor operation sequencing
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (systems_tool_never_touches_the_vendor_sdk) {
        const auto start = startTime();
        const auto directory = captureDirectory("systems_no_sdk");
        FakeSdk sdk{};
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, &sdk, start));
        driveToReady(capture, start);

        {
            std::ofstream matching{directory / "stop.request", std::ios::binary | std::ios::trunc};
            matching << "s1\n";
        }
        PPR_TEST_ASSERT(capture.advance(admittedObservation(), start + std::chrono::seconds{3}).has_value());

        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::complete);
        PPR_TEST_ASSERT(capture.sdkOperations() == 0u);
        PPR_TEST_ASSERT(sdk.m_initialize_calls == 0u);
        PPR_TEST_ASSERT(sdk.m_activate_calls == 0u);
        PPR_TEST_ASSERT(sdk.m_start_calls == 0u);
        PPR_TEST_ASSERT(sdk.m_poll_calls == 0u);
    };

    PPR_UNIT_TEST (graphics_initializes_activates_starts_and_waits_for_the_finalized_trace) {
        const auto start = startTime();
        const auto directory = captureDirectory("graphics");
        FakeSdk sdk{};
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::graphics, EProfileCaptureWorkload::frozen, "s1", 1u, &sdk, start));
        driveToReady(capture, start);

        TimePoint now = start + std::chrono::seconds{2};
        std::optional<EProfileCaptureSdkOperation> completed{};

        const auto run_until = [&](const EProfileCaptureSdkOperation expected) {
            for (u32 turn = 0u; turn < 32u; ++turn) {
                now += std::chrono::milliseconds{60};
                ProfileCaptureObservation observation = admittedObservation();
                if (completed) {
                    observation.m_sdk_operation_completed = true;
                    observation.m_sdk_operation = *completed;
                    observation.m_sdk_status = sdk.m_status;
                    completed.reset();
                }

                auto action = capture.advance(observation, now);
                PPR_TEST_ASSERT(action.has_value());
                if (not action->has_value()) {
                    continue;
                }
                if ((**action).m_kind != EProfileCaptureActionKind::sdk_operation) {
                    continue;
                }
                PPR_TEST_ASSERT((**action).m_operation == expected);
                std::ignore = observeSdkAction(sdk, (**action).m_operation);
                completed = (**action).m_operation;
                return true;
            }
            return false;
        };

        PPR_TEST_ASSERT(run_until(EProfileCaptureSdkOperation::initialize));
        PPR_TEST_ASSERT(run_until(EProfileCaptureSdkOperation::activate));
        PPR_TEST_ASSERT(run_until(EProfileCaptureSdkOperation::start_trace));
        PPR_TEST_ASSERT(sdk.m_initialize_calls == 1u);
        PPR_TEST_ASSERT(sdk.m_activate_calls == 1u);
        PPR_TEST_ASSERT(sdk.m_start_calls == 1u);

        // The tool bounds the collection; the driver polls nonblocking until a
        // finalized trace path is reported, then requests the ordinary quit.
        bool quit = false;
        for (u32 turn = 0u; turn < 32u and not quit; ++turn) {
            now += std::chrono::milliseconds{60};
            ProfileCaptureObservation observation = admittedObservation();
            if (completed) {
                observation.m_sdk_operation_completed = true;
                observation.m_sdk_operation = *completed;
                observation.m_sdk_status = sdk.m_status;
                completed.reset();
            }

            auto action = capture.advance(observation, now);
            PPR_TEST_ASSERT(action.has_value());
            if (not action->has_value()) {
                continue;
            }
            if ((**action).m_kind == EProfileCaptureActionKind::quit) {
                quit = true;
            } else if ((**action).m_kind == EProfileCaptureActionKind::sdk_operation) {
                if ((**action).m_operation == EProfileCaptureSdkOperation::poll) {
                    sdk.m_status.m_tracing = false;
                    sdk.m_status.m_trace_files = 1u;
                    sdk.m_status.m_trace_path = "/tmp/trace.ngfx-gputrace";
                }
                std::ignore = observeSdkAction(sdk, (**action).m_operation);
                completed = (**action).m_operation;
            }
        }

        PPR_TEST_ASSERT(quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::complete);
        PPR_TEST_ASSERT(sdk.m_poll_calls >= 1u);
        PPR_TEST_ASSERT(not capture.finish({}));

        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"trace_finalized\": true") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("/tmp/trace.ngfx-gputrace") != std::string::npos);
    };

    PPR_UNIT_TEST (graphics_without_an_sdk_is_unsupported_not_substituted) {
        const auto start = startTime();
        const auto directory = captureDirectory("graphics_unsupported");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::graphics, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));

        const auto action = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((*action)->m_kind == EProfileCaptureActionKind::quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::unsupported);
        PPR_TEST_ASSERT(capture.sdkOperations() == 0u);
        PPR_TEST_ASSERT(capture.finish({}) == std::errc::state_not_recoverable);

        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"status\": \"unsupported\"") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("SDK adapter unavailable") != std::string::npos);
    };

    PPR_UNIT_TEST (graphics_initialize_failure_fails_the_capture_explicitly) {
        const auto start = startTime();
        const auto directory = captureDirectory("graphics_failure");
        FakeSdk sdk{};
        sdk.m_fail_initialize = true;
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::graphics, EProfileCaptureWorkload::frozen, "s1", 1u, &sdk, start));
        driveToReady(capture, start);

        TimePoint now = start + std::chrono::seconds{2};
        for (u32 turn = 0u; turn < 8u; ++turn) {
            now += std::chrono::milliseconds{60};
            ProfileCaptureObservation observation = admittedObservation();
            if (turn > 0u) {
                observation.m_sdk_operation_completed = true;
                observation.m_sdk_operation = EProfileCaptureSdkOperation::initialize;
                observation.m_sdk_operation_error = std::make_error_code(std::errc::state_not_recoverable);
            }
            std::ignore = capture.advance(observation, now);
        }

        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::failed);
        PPR_TEST_ASSERT(static_cast<bool>(capture.finish({})));
        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"status\": \"failed\"") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("graphics SDK operation failed") != std::string::npos);
    };

    PPR_UNIT_TEST (cleanup_error_after_completion_rewrites_final_state_incomplete) {
        const auto start = startTime();
        const auto directory = captureDirectory("cleanup");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));
        driveToReady(capture, start);
        {
            std::ofstream matching{directory / "stop.request", std::ios::binary | std::ios::trunc};
            matching << "s1\n";
        }
        const auto quit = capture.advance(admittedObservation(), start + std::chrono::seconds{3});
        PPR_TEST_ASSERT(quit.has_value() and quit->has_value());
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::complete);
        PPR_TEST_ASSERT(readFile(directory / "final-state.json").find("\"status\": \"complete\"") != std::string::npos);

        // The run boundary can report a cleanup failure after a completed
        // capture; the evidence must say incomplete and preserve the cause.
        const std::error_code cleanup = std::make_error_code(std::errc::io_error);
        PPR_TEST_ASSERT(static_cast<bool>(capture.finish(cleanup)));
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);
        const std::string final_state = readFile(directory / "final-state.json");
        PPR_TEST_ASSERT(final_state.find("\"status\": \"incomplete\"") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("shutdown cleanup failed") != std::string::npos);
        PPR_TEST_ASSERT(final_state.find("Input/output error") != std::string::npos);
    };

    PPR_UNIT_TEST (stop_request_during_warmup_quits_ordinarily_and_reports_incomplete) {
        const auto start = startTime();
        const auto directory = captureDirectory("stop_warmup");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "warm", 1u, nullptr, start));

        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        capture.notePauseApplied(0u);

        {
            std::ofstream matching{directory / "stop.request", std::ios::binary | std::ios::trunc};
            matching << "warm\n";
        }
        ProfileCaptureObservation frames = admittedObservation();
        frames.m_accepted_scene_frames = 10u;
        const auto quit = capture.advance(frames, start + std::chrono::seconds{2});
        PPR_TEST_ASSERT(quit.has_value() and quit->has_value());
        PPR_TEST_ASSERT((*quit)->m_kind == EProfileCaptureActionKind::quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);
        PPR_TEST_ASSERT(readFile(directory / "final-state.json").find("before readiness") != std::string::npos);
    };

    PPR_UNIT_TEST (stop_request_during_graphics_tracing_quits_without_fabricating_a_trace) {
        const auto start = startTime();
        const auto directory = captureDirectory("stop_tracing");
        FakeSdk sdk{};
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::graphics, EProfileCaptureWorkload::frozen, "gfx", 1u, &sdk, start));
        driveToReady(capture, start);

        TimePoint now = start + std::chrono::seconds{2};
        std::optional<EProfileCaptureSdkOperation> completed{};
        bool started = false;
        for (u32 turn = 0u; turn < 24u and not started; ++turn) {
            now += std::chrono::milliseconds{60};
            ProfileCaptureObservation observation = admittedObservation();
            if (completed) {
                observation.m_sdk_operation_completed = true;
                observation.m_sdk_operation = *completed;
                observation.m_sdk_status = sdk.m_status;
                completed.reset();
            }
            auto action = capture.advance(observation, now);
            PPR_TEST_ASSERT(action.has_value());
            if (not action->has_value()) {
                continue;
            }
            if ((**action).m_kind == EProfileCaptureActionKind::sdk_operation) {
                std::ignore = observeSdkAction(sdk, (**action).m_operation);
                completed = (**action).m_operation;
                started = (**action).m_operation == EProfileCaptureSdkOperation::start_trace;
            }
        }
        PPR_TEST_ASSERT(started);

        {
            std::ofstream matching{directory / "stop.request", std::ios::binary | std::ios::trunc};
            matching << "gfx\n";
        }
        bool quit = false;
        for (u32 turn = 0u; turn < 8u and not quit; ++turn) {
            now += std::chrono::milliseconds{120};
            ProfileCaptureObservation observation = admittedObservation();
            if (completed) {
                observation.m_sdk_operation_completed = true;
                observation.m_sdk_operation = *completed;
                observation.m_sdk_status = sdk.m_status;
                completed.reset();
            }
            auto action = capture.advance(observation, now);
            PPR_TEST_ASSERT(action.has_value());
            if (action->has_value() and (**action).m_kind == EProfileCaptureActionKind::quit) {
                quit = true;
            }
        }
        PPR_TEST_ASSERT(quit);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);
        PPR_TEST_ASSERT(
            readFile(directory / "final-state.json").find("before the trace finalized") != std::string::npos);
    };

    PPR_UNIT_TEST (graphics_on_a_non_vulkan_backend_is_unsupported_explicitly) {
        const auto start = startTime();
        const auto directory = captureDirectory("backend");
        FakeSdk sdk{};
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::graphics, EProfileCaptureWorkload::frozen, "s1", 1u, &sdk, start));

        capture.noteBackend(EProfileCaptureBackend::d3d12, start);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::unsupported);
        const auto action = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((*action)->m_kind == EProfileCaptureActionKind::quit);
        PPR_TEST_ASSERT(capture.sdkOperations() == 0u);
        PPR_TEST_ASSERT(readFile(directory / "final-state.json").find("Vulkan only") != std::string::npos);
    };

    PPR_UNIT_TEST (shutdown_before_completion_is_incomplete_and_keeps_evidence) {
        const auto start = startTime();
        const auto directory = captureDirectory("shutdown");
        ProfileCapture capture{};
        PPR_TEST_ASSERT(not capture.begin(
            directory, EProfileCaptureTool::systems, EProfileCaptureWorkload::frozen, "s1", 1u, nullptr, start));

        auto pause = capture.advance(admittedObservation(), start);
        PPR_TEST_ASSERT(pause.has_value() and pause->has_value());
        capture.notePauseApplied(0u);

        PPR_TEST_ASSERT(capture.finish({}) == std::errc::state_not_recoverable);
        PPR_TEST_ASSERT(capture.status() == EProfileCaptureStatus::incomplete);
        PPR_TEST_ASSERT(std::filesystem::exists(directory / "final-state.json"));
        PPR_TEST_ASSERT(std::filesystem::exists(directory / "profile-capture.log"));
        PPR_TEST_ASSERT(not std::filesystem::exists(directory / "ready.json"));
    };

} // namespace pP::game::tests::detail::profile_capture

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // profile capture group
    // ------------------------------------------------------------------

    const UnitTest profile_capture = UnitTest::Named("profile_capture") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::profile_capture::readiness_requires_120_accepted_frames_and_two_active_seconds,
            detail::profile_capture::ui_only_and_rejected_frames_are_excluded_from_readiness,
            detail::profile_capture::frozen_capture_pauses_before_the_first_tick,
            detail::profile_capture::live_workload_does_not_pause,
            detail::profile_capture::wrong_session_stop_token_is_ignored,
            detail::profile_capture::close_during_capture_finalizes_incomplete_and_uses_ordinary_shutdown,
            detail::profile_capture::readiness_deadline_reports_incomplete_and_invokes_ordinary_quit,
            detail::profile_capture::output_directory_must_be_fresh,
            detail::profile_capture::systems_tool_never_touches_the_vendor_sdk,
            detail::profile_capture::graphics_initializes_activates_starts_and_waits_for_the_finalized_trace,
            detail::profile_capture::graphics_without_an_sdk_is_unsupported_not_substituted,
            detail::profile_capture::graphics_initialize_failure_fails_the_capture_explicitly,
            detail::profile_capture::shutdown_before_completion_is_incomplete_and_keeps_evidence,
            detail::profile_capture::cleanup_error_after_completion_rewrites_final_state_incomplete,
            detail::profile_capture::stop_request_during_warmup_quits_ordinarily_and_reports_incomplete,
            detail::profile_capture::stop_request_during_graphics_tracing_quits_without_fabricating_a_trace,
            detail::profile_capture::graphics_on_a_non_vulkan_backend_is_unsupported_explicitly,
        });
    };

    const UnitTest &gameProfileCaptureTests() noexcept {
        return profile_capture;
    }
} // namespace pP::game::tests
