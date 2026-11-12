module;

#include "pP/UnitTest.h"

module game.tests;

import engine.app;
import engine.core;
import engine.math;
import game.config;
import game.display;
import game.display_exercise;

import std;

namespace pP::game::tests::detail::display_exercise {
    // ------------------------------------------------------------------
    // display exercise
    // ------------------------------------------------------------------

    [[nodiscard]] TimeSpan advanceSpan(const float seconds) noexcept {
        return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>{seconds});
    }

    [[nodiscard]] TimePoint startTime() {
        return TimePoint{} + std::chrono::seconds{1000};
    }

    [[nodiscard]] std::filesystem::path exerciseDirectory(const std::string_view name) {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                                "ppr-display-exercise-tests" /
                                                std::string{name};
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
        return directory;
    }

    [[nodiscard]] std::string readFile(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] EDisplayExerciseStatus caseStatus(const DisplayExercise &exercise, const std::string_view id) {
        for (const DisplayExerciseCase &entry: exercise.cases()) {
            if (entry.m_id == id) {
                return entry.m_status;
            }
        }
        return EDisplayExerciseStatus::pending;
    }

    [[nodiscard]] DisplayExerciseObservation readyObservation() {
        DisplayExerciseObservation observation{};
        observation.m_capabilities_available = true;
        observation.m_capabilities.m_window_changes = true;
        observation.m_capabilities.m_exclusive_refresh = true;
        DisplayMonitorCapability monitor{};
        monitor.m_name = "Test Panel";
        monitor.m_primary = true;
        monitor.m_current = DisplayModeCapability{1920u, 1080u, std::optional<u32>{60u}};
        monitor.m_modes.push_back(DisplayModeCapability{1920u, 1080u, std::optional<u32>{60u}});
        monitor.m_modes.push_back(DisplayModeCapability{1280u, 720u, std::optional<u32>{60u}});
        monitor.m_modes.push_back(DisplayModeCapability{1280u, 720u, std::optional<u32>{144u}});
        observation.m_capabilities.m_monitors.push_back(std::move(monitor));

        observation.m_committed = DisplaySettings{};
        observation.m_settings_revision = 1u;
        observation.m_surface_generation = 1u;
        observation.m_presented_generation = 1u;
        observation.m_logical_extent = int2{1280, 720};
        observation.m_framebuffer_extent = int2{1280, 720};
        observation.m_focused = true;
        observation.m_world_ready = true;
        observation.m_presented_frame = 10u;
        observation.m_presented_generation = 1u;
        observation.m_presented_world = GridWorldIdentity{
            .m_job_id = 1u, .m_world_generation = 1u, .m_content_fingerprint = 0xabcdu};
        observation.m_history_available = true;
        observation.m_history_established = true;
        observation.m_history_reset_count = 1u;
        observation.m_history_last_reset = ReconstructionHistoryStamp{
            .m_world = observation.m_presented_world,
            .m_grid = PresentationGrid{},
            .m_index = 0u,
            .m_phase = 0u,
            .m_frame = 10u,
            .m_established = true,
            .m_reset = true};
        observation.m_live_revision = 1u;
        observation.m_presented_live_revision = 1u;
        observation.m_startup_display = observation.m_committed;
        observation.m_backend = "test";
        observation.m_gpu = "test";
        observation.m_executable_path = "test";
        return observation;
    }

    [[nodiscard]] DisplayExerciseTestStep step(
        const EDisplayExerciseActionKind kind, const std::string_view case_id, const bool completes = true) {
        DisplayExerciseTestStep entry{};
        entry.m_kind = kind;
        entry.m_case = std::string{case_id};
        entry.m_completes_case = completes;
        return entry;
    }

    [[nodiscard]] DisplayExerciseTestStep windowedStep(
        const std::string_view case_id, const u32 width, const u32 height) {
        DisplayExerciseTestStep entry = step(EDisplayExerciseActionKind::apply_display, case_id, false);
        entry.m_draft.m_window_mode = EDisplayWindowMode::windowed;
        entry.m_draft.m_width = width;
        entry.m_draft.m_height = height;
        entry.m_keep_after_ack = true;
        return entry;
    }

    void notePresented(DisplayExerciseObservation &observation) {
        ++observation.m_live_revision;
        ++observation.m_presented_frame;
        observation.m_presented_live_revision = observation.m_live_revision;
    }

    // ------------------------------------------------------------------
    // invocation and plan
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (begin_rejects_empty_output) {
        DisplayExercise exercise{};
        PPR_TEST_ASSERT(exercise.begin({}, startTime()) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(exercise.status() == EDisplayExerciseStatus::pending);
        PPR_TEST_ASSERT(exercise.cases().empty());
    };

    PPR_UNIT_TEST (permanent_plan_reports_schema_and_nine_groups) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("schema");
        PPR_TEST_ASSERT(not exercise.begin(directory, startTime()));

        const std::string report = readFile(directory / "display-exercise.json");
        PPR_TEST_ASSERT(report.find("\"schema_version\":1") != std::string::npos);
        for (const std::string_view group:
            {"1-initial-load", "2-windowed", "3-pixelization", "4-exposure-bloom", "5-inspection", "6-pacing",
                "7-fullscreen", "8-transactions", "9-window-lifecycle"}) {
            PPR_TEST_ASSERT(report.find(group) != std::string::npos);
        }
        PPR_TEST_ASSERT(report.find("\"status\":\"pending\"") != std::string::npos);
        PPR_TEST_ASSERT(exercise.cases().size() > 60u);
    };

    // ------------------------------------------------------------------
    // acknowledgements and trials
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (stale_acks_never_advance_and_confirmation_waits_for_matching_ack) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("ack");
        std::array steps{
            windowedStep("apply", 800u, 600u),
            step(EDisplayExerciseActionKind::keep_display, "apply"),
        };
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);
        PPR_TEST_ASSERT(exercise.emittedActions() == 1u);
        PPR_TEST_ASSERT(not exercise.confirmationStarted());

        // Outstanding native trial: no overlap, no progress.
        observation.m_transaction_pending = true;
        for (int tick = 0; tick < 5; ++tick) {
            action = exercise.advance(observation, t0 + std::chrono::seconds{2 + tick});
            PPR_TEST_ASSERT(action.has_value() and not action->has_value());
            PPR_TEST_ASSERT(exercise.emittedActions() == 1u);
        }

        // A mismatched/stale acknowledgement publishes a different candidate.
        observation.m_transaction_pending = false;
        observation.m_settings_revision = 2u;
        observation.m_committed = DisplaySettings{};
        action = exercise.advance(observation, t0 + std::chrono::seconds{8});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(not exercise.confirmationStarted());
        PPR_TEST_ASSERT(not exercise.staleAckAdvanced());
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::pending);

        // Matching acknowledgement publishes the requested values and arms.
        observation.m_settings_revision = 3u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed = DisplaySettings{};
        observation.m_committed.m_width = 800u;
        observation.m_committed.m_height = 600u;
        observation.m_confirm_armed = true;
        observation.m_logical_extent = int2{800, 600};
        observation.m_framebuffer_extent = int2{800, 600};
        action = exercise.advance(observation, t0 + std::chrono::seconds{9});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::keep_display);
        PPR_TEST_ASSERT(exercise.confirmationStarted());
        PPR_TEST_ASSERT(not exercise.staleAckAdvanced());

        observation.m_confirm_armed = false;
        observation.m_presented_frame = 12u;
        action = exercise.advance(observation, t0 + std::chrono::seconds{10});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (timeout_waits_for_the_production_confirmation_duration) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("timeout");
        DisplayExerciseTestStep apply = windowedStep("timeout", 640u, 481u);
        apply.m_keep_after_ack = false;
        apply.m_timeout_after_ack = true;
        apply.m_completes_case = true;
        std::array steps{apply};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);

        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed.m_width = 640u;
        observation.m_committed.m_height = 481u;
        observation.m_confirm_armed = true;
        observation.m_logical_extent = int2{640, 481};
        observation.m_framebuffer_extent = int2{640, 481};
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(exercise.confirmationStarted());
        PPR_TEST_ASSERT(caseStatus(exercise, "timeout") == EDisplayExerciseStatus::pending);

        // The production timer has not expired yet: an early revert is a
        // failure, never a fabricated timeout pass.
        observation.m_settings_revision = 3u;
        observation.m_surface_generation = 3u;
        observation.m_presented_generation = 3u;
        observation.m_presented_frame = 12u;
        observation.m_committed = DisplaySettings{};
        observation.m_confirm_armed = false;
        action = exercise.advance(observation, t0 + std::chrono::seconds{6});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "timeout") == EDisplayExerciseStatus::failed);

        // A fresh run: the real production duration plus a margin passes.
        DisplayExercise passing{};
        const auto second = exerciseDirectory("timeout-pass");
        PPR_TEST_ASSERT(not passing.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        action = passing.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed.m_width = 640u;
        observation.m_committed.m_height = 481u;
        observation.m_confirm_armed = true;
        observation.m_logical_extent = int2{640, 481};
        observation.m_framebuffer_extent = int2{640, 481};
        action = passing.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        observation.m_settings_revision = 3u;
        observation.m_surface_generation = 3u;
        observation.m_presented_generation = 3u;
        observation.m_presented_frame = 12u;
        observation.m_committed = DisplaySettings{};
        observation.m_confirm_armed = false;
        action = passing.advance(
            observation, t0 + std::chrono::seconds{2} + advanceSpan(kDisplayConfirmSeconds + 1.0f));
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(passing, "timeout") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (deadline_reports_incomplete_and_still_quits) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("deadline");
        std::array steps{step(EDisplayExerciseActionKind::apply_display, "apply")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        // No matching acknowledgement arrives before the transition deadline.
        action = exercise.advance(observation, t0 + std::chrono::seconds{1} + advanceSpan(11.0f));
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::incomplete);
        PPR_TEST_ASSERT(exercise.status() == EDisplayExerciseStatus::pending);

        observation.m_close_requested = true;
        action = exercise.advance(observation, t0 + std::chrono::seconds{14});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        const std::error_code result = exercise.finish({});
        PPR_TEST_ASSERT(result == std::errc::timed_out);
        PPR_TEST_ASSERT(exercise.status() == EDisplayExerciseStatus::incomplete);
        PPR_TEST_ASSERT(displayExerciseExitCode(exercise.status()) == 3);
    };

    // ------------------------------------------------------------------
    // status classification
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (unsupported_capability_is_not_a_pass) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("unsupported");
        std::array steps{step(EDisplayExerciseActionKind::apply_display, "fullscreen")};
        steps[0].m_draft.m_window_mode = EDisplayWindowMode::fullscreen;
        steps[0].m_draft.m_monitor = "Missing Panel";
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        observation.m_capabilities.m_window_changes = false;
        auto action = exercise.advance(observation, startTime() + std::chrono::seconds{1});
        // The capability snapshot is requested first, then classified.
        if (action.has_value() and action->has_value()) {
            action = exercise.advance(observation, startTime() + std::chrono::seconds{2});
        }
        PPR_TEST_ASSERT(caseStatus(exercise, "fullscreen") == EDisplayExerciseStatus::unsupported);
        PPR_TEST_ASSERT(not action->has_value() or (**action).m_kind == EDisplayExerciseActionKind::quit);
    };

    PPR_UNIT_TEST (failed_transaction_is_retained_distinct_from_incomplete) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("failed");
        std::array steps{step(EDisplayExerciseActionKind::apply_display, "apply")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        auto action = exercise.advance(observation, startTime() + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        observation.m_transaction_pending = false;
        observation.m_action_error = std::make_error_code(std::errc::invalid_argument);
        observation.m_action_message = "rejected";
        action = exercise.advance(observation, startTime() + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::failed);
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);
    };

    // ------------------------------------------------------------------
    // user close and evidence
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (user_close_during_pending_resize_stops_actions_and_preserves_evidence) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("close");
        std::array steps{windowedStep("resize", 800u, 600u), step(EDisplayExerciseActionKind::keep_display, "resize")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);

        observation.m_transaction_pending = true;
        observation.m_close_requested = true;
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(exercise.emittedActionsAfterClose() == 0u);
        PPR_TEST_ASSERT(caseStatus(exercise, "resize") == EDisplayExerciseStatus::incomplete);
        PPR_TEST_ASSERT(exercise.status() == EDisplayExerciseStatus::incomplete);

        for (int tick = 0; tick < 4; ++tick) {
            action = exercise.advance(observation, t0 + std::chrono::seconds{3 + tick});
            PPR_TEST_ASSERT(action.has_value() and not action->has_value());
            PPR_TEST_ASSERT(exercise.emittedActionsAfterClose() == 0u);
        }

        const std::error_code result = exercise.finish({});
        PPR_TEST_ASSERT(result == std::errc::timed_out);
        const std::string report = readFile(directory / "display-exercise.json");
        PPR_TEST_ASSERT(report.find("\"user_close\":true") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"emitted_actions_after_close\":0") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"status\":\"incomplete\"") != std::string::npos);
    };

    // ------------------------------------------------------------------
    // restoration, reset, save and reload
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (revert_restores_the_prior_acknowledged_values) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("revert");
        DisplayExerciseTestStep apply = windowedStep("revert", 1919u, 1079u);
        apply.m_keep_after_ack = false;
        apply.m_revert_after_ack = true;
        apply.m_completes_case = false;
        std::array steps{apply, step(EDisplayExerciseActionKind::revert_display, "revert")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);

        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed.m_width = 1919u;
        observation.m_committed.m_height = 1079u;
        observation.m_confirm_armed = true;
        observation.m_logical_extent = int2{1919, 1079};
        observation.m_framebuffer_extent = int2{1919, 1079};
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::revert_display);

        observation.m_settings_revision = 3u;
        observation.m_surface_generation = 3u;
        observation.m_presented_generation = 3u;
        observation.m_presented_frame = 12u;
        observation.m_committed = DisplaySettings{};
        observation.m_confirm_armed = false;
        observation.m_logical_extent = int2{1280, 720};
        observation.m_framebuffer_extent = int2{1280, 720};
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "revert") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (reset_restores_startup_display_values) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("reset");
        DisplayExerciseTestStep apply = step(EDisplayExerciseActionKind::apply_display, "reset", false);
        apply.m_draft.m_pacing = EFramePacing::paced;
        apply.m_draft.m_cap_hz = 120u;
        apply.m_draft.m_paced_vsync = false;
        apply.m_keep_after_ack = true;
        std::array steps{
            apply, step(EDisplayExerciseActionKind::keep_display, "reset", false),
            step(EDisplayExerciseActionKind::reset_display, "reset")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        observation.m_settings_revision = 2u;
        observation.m_committed.m_pacing = EFramePacing::paced;
        observation.m_committed.m_cap_hz = 120u;
        observation.m_committed.m_paced_vsync = false;
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::keep_display);

        observation.m_presented_frame = 12u;
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::reset_display);

        observation.m_settings_revision = 3u;
        observation.m_committed = DisplaySettings{};
        action = exercise.advance(observation, t0 + std::chrono::seconds{4});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "reset") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (save_and_reload_compare_acknowledged_values) {
        DisplayExercise passing{};
        const auto directory = exerciseDirectory("save-reload");
        std::array steps{
            step(EDisplayExerciseActionKind::save_display, "save"),
            step(EDisplayExerciseActionKind::reload_overlay, "reload")};
        PPR_TEST_ASSERT(not passing.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = passing.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::save_display);

        observation.m_save_complete = true;
        action = passing.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::reload_overlay);

        observation.m_reload_complete = true;
        observation.m_reload_display = observation.m_committed;
        observation.m_reload_world_light = observation.m_live_world_light;
        observation.m_reload_pixel = observation.m_live_pixel;
        observation.m_reload_post = observation.m_live_post;
        action = passing.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(passing, "save") == EDisplayExerciseStatus::passed);
        PPR_TEST_ASSERT(caseStatus(passing, "reload") == EDisplayExerciseStatus::passed);

        DisplayExercise failing{};
        const auto second = exerciseDirectory("save-reload-mismatch");
        PPR_TEST_ASSERT(not failing.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        action = failing.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_save_complete = true;
        action = failing.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_reload_complete = true;
        observation.m_reload_display = observation.m_committed;
        observation.m_reload_display.m_cap_hz = 99u;
        action = failing.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(caseStatus(failing, "reload") == EDisplayExerciseStatus::failed);
    };

    PPR_UNIT_TEST (invalid_draft_never_reaches_native_submission) {
        DisplayExercise rejected{};
        const auto directory = exerciseDirectory("invalid-draft");
        DisplayExerciseTestStep invalid = step(EDisplayExerciseActionKind::invalid_draft, "invalid");
        invalid.m_draft.m_window_mode = EDisplayWindowMode::windowed;
        invalid.m_draft.m_refresh_hz = 60u;
        invalid.m_expect_rejection = true;
        std::array steps{invalid};
        PPR_TEST_ASSERT(not rejected.beginForTest(directory, startTime(), steps));
        DisplayExerciseObservation observation = readyObservation();
        auto action = rejected.advance(observation, startTime() + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_action_error = std::make_error_code(std::errc::invalid_argument);
        observation.m_action_message = "display.refresh_hz: Refresh is an exclusive fullscreen request only";
        observation.m_action_submitted = false;
        action = rejected.advance(observation, startTime() + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(rejected, "invalid") == EDisplayExerciseStatus::passed);

        DisplayExercise submitted{};
        const auto second = exerciseDirectory("invalid-draft-submitted");
        PPR_TEST_ASSERT(not submitted.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        action = submitted.advance(observation, startTime() + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_action_submitted = true;
        action = submitted.advance(observation, startTime() + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(submitted, "invalid") == EDisplayExerciseStatus::failed);
    };

    // ------------------------------------------------------------------
    // overall outcome
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (policy_only_apply_does_not_wait_for_a_surface_change) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("policy-only");
        DisplayExerciseTestStep apply = step(EDisplayExerciseActionKind::apply_display, "policy");
        apply.m_draft.m_pacing = EFramePacing::paced;
        apply.m_draft.m_cap_hz = 120u;
        apply.m_draft.m_paced_vsync = false;
        std::array steps{apply};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        observation.m_committed.m_width = 1280u;
        observation.m_committed.m_height = 720u;
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);

        // A zero-size policy draft keeps the current window, so the ack is
        // complete without waiting for a new surface generation or geometry.
        observation.m_settings_revision = 2u;
        observation.m_committed.m_pacing = EFramePacing::paced;
        observation.m_committed.m_cap_hz = 120u;
        observation.m_committed.m_paced_vsync = false;
        observation.m_committed.m_width = 1280u;
        observation.m_committed.m_height = 720u;
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);
        PPR_TEST_ASSERT(caseStatus(exercise, "policy") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (matching_ack_without_fresh_surface_does_not_advance) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("ack-no-surface");
        DisplayExerciseTestStep apply = step(EDisplayExerciseActionKind::apply_display, "apply");
        apply.m_draft.m_window_mode = EDisplayWindowMode::windowed;
        apply.m_draft.m_width = 800u;
        apply.m_draft.m_height = 600u;
        std::array steps{apply};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        observation.m_surface_generation = 5u;
        observation.m_presented_generation = 5u;
        observation.m_committed.m_width = 1280u;
        observation.m_committed.m_height = 720u;
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        // Matching values without a fresh presented surface are not enough.
        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 6u;
        observation.m_committed.m_width = 800u;
        observation.m_committed.m_height = 600u;
        observation.m_logical_extent = int2{800, 600};
        observation.m_framebuffer_extent = int2{800, 600};
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::pending);

        observation.m_presented_generation = 6u;
        observation.m_presented_frame = 11u;
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "apply") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (cleanup_keep_resolves_an_armed_trial_before_the_next_transaction) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("cleanup-keep");
        DisplayExerciseTestStep first = step(EDisplayExerciseActionKind::apply_display, "first");
        first.m_draft.m_width = 800u;
        first.m_draft.m_height = 600u;
        DisplayExerciseTestStep second = step(EDisplayExerciseActionKind::apply_display, "second");
        second.m_draft.m_width = 1024u;
        second.m_draft.m_height = 768u;
        std::array steps{first, second};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed.m_width = 800u;
        observation.m_committed.m_height = 600u;
        observation.m_logical_extent = int2{800, 600};
        observation.m_framebuffer_extent = int2{800, 600};
        observation.m_confirm_armed = true;
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::keep_display);
        PPR_TEST_ASSERT(caseStatus(exercise, "first") == EDisplayExerciseStatus::incomplete);

        observation.m_confirm_armed = false;
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::apply_display);
        PPR_TEST_ASSERT((**action).m_case == "second");
    };

    PPR_UNIT_TEST (trial_apply_keep_requires_arm_and_settles_without_late_revert) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("trial-keep");
        DisplayExerciseTestStep apply = step(EDisplayExerciseActionKind::apply_display, "trial", false);
        apply.m_draft.m_width = 1024u;
        apply.m_draft.m_height = 768u;
        apply.m_keep_after_ack = true;
        apply.m_require_trial_armed = true;
        std::array steps{apply, step(EDisplayExerciseActionKind::keep_display, "trial")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());

        observation.m_settings_revision = 2u;
        observation.m_surface_generation = 2u;
        observation.m_presented_generation = 2u;
        observation.m_presented_frame = 11u;
        observation.m_committed.m_width = 1024u;
        observation.m_committed.m_height = 768u;
        observation.m_logical_extent = int2{1024, 768};
        observation.m_framebuffer_extent = int2{1024, 768};
        observation.m_confirm_armed = false;
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(not exercise.confirmationStarted());

        observation.m_confirm_armed = true;
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::keep_display);
        PPR_TEST_ASSERT(exercise.confirmationStarted());

        observation.m_confirm_armed = false;
        observation.m_presented_frame = 12u;
        action = exercise.advance(observation, t0 + std::chrono::seconds{4});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);
        PPR_TEST_ASSERT(caseStatus(exercise, "trial") == EDisplayExerciseStatus::passed);
        for (const DisplayExerciseCase &entry: exercise.cases()) {
            if (entry.m_id == "trial") {
                PPR_TEST_ASSERT(entry.m_trial_armed);
                PPR_TEST_ASSERT(entry.m_trial_kept);
            }
        }
    };

    PPR_UNIT_TEST (pixel_case_records_exact_grid_and_history_reset) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("pixel-history");
        DisplayExerciseTestStep live = step(EDisplayExerciseActionKind::live_picture, "pixel");
        live.m_pixel = PixelDisplaySettings{2u};
        std::array steps{live};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        observation.m_live_pixel = PixelDisplaySettings{3u};
        observation.m_framebuffer_extent = int2{1919, 1079};
        observation.m_logical_extent = int2{1919, 1079};
        observation.m_history_reset_count = 5u;
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::live_picture);

        const auto grid = makePresentationGrid(int2{1919, 1079}, 2u);
        PPR_TEST_ASSERT(grid.has_value());
        observation.m_live_pixel = PixelDisplaySettings{2u};
        ++observation.m_live_revision;
        ++observation.m_presented_frame;
        observation.m_presented_live_revision = observation.m_live_revision;
        observation.m_history_reset_count = 6u;
        observation.m_history_last_reset = ReconstructionHistoryStamp{
            .m_world = observation.m_presented_world,
            .m_grid = *grid,
            .m_index = 1u,
            .m_phase = 0u,
            .m_frame = observation.m_presented_frame,
            .m_established = true,
            .m_reset = true};
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT(caseStatus(exercise, "pixel") == EDisplayExerciseStatus::passed);
        for (const DisplayExerciseCase &entry: exercise.cases()) {
            if (entry.m_id == "pixel") {
                PPR_TEST_ASSERT(entry.m_has_presentation_grid);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_resolved.x == 960);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_resolved.y == 540);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_raster.x == 480);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_raster.y == 270);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_backing.x == 960);
                PPR_TEST_ASSERT(entry.m_presentation_grid.m_backing.y == 540);
                PPR_TEST_ASSERT(entry.m_history_reset);
                PPR_TEST_ASSERT(entry.m_has_history_grid);
            }
        }
    };

    PPR_UNIT_TEST (non_pixel_live_edits_must_not_reset_history) {
        DisplayExercise passing{};
        const auto first = exerciseDirectory("live-history-pass");
        DisplayExerciseTestStep live = step(EDisplayExerciseActionKind::live_picture, "live");
        live.m_world_light.m_sky_hour = 0;
        std::array steps{live};
        PPR_TEST_ASSERT(not passing.beginForTest(first, startTime(), steps));
        DisplayExerciseObservation observation = readyObservation();
        observation.m_history_reset_count = 5u;
        const TimePoint t0 = startTime();
        auto action = passing.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        ++observation.m_live_revision;
        ++observation.m_presented_frame;
        observation.m_presented_live_revision = observation.m_live_revision;
        observation.m_live_world_light.m_sky_hour = 0;
        action = passing.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(passing, "live") == EDisplayExerciseStatus::passed);

        DisplayExercise failing{};
        const auto second = exerciseDirectory("live-history-reset");
        PPR_TEST_ASSERT(not failing.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        observation.m_history_reset_count = 5u;
        action = failing.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        ++observation.m_live_revision;
        ++observation.m_presented_frame;
        observation.m_presented_live_revision = observation.m_live_revision;
        observation.m_live_world_light.m_sky_hour = 0;
        observation.m_history_reset_count = 6u;
        action = failing.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(failing, "live") == EDisplayExerciseStatus::failed);
    };

    PPR_UNIT_TEST (invalid_draft_only_passes_for_the_schema_rejection) {
        DisplayExercise wrong_error{};
        const auto first = exerciseDirectory("invalid-wrong");
        DisplayExerciseTestStep invalid = step(EDisplayExerciseActionKind::invalid_draft, "invalid");
        invalid.m_expect_rejection = true;
        std::array steps{invalid};
        PPR_TEST_ASSERT(not wrong_error.beginForTest(first, startTime(), steps));
        DisplayExerciseObservation observation = readyObservation();
        auto action = wrong_error.advance(observation, startTime() + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_action_error = std::make_error_code(std::errc::device_or_resource_busy);
        observation.m_action_message = "A picture change is already in flight.";
        action = wrong_error.advance(observation, startTime() + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(wrong_error, "invalid") == EDisplayExerciseStatus::failed);

        DisplayExercise schema{};
        const auto second = exerciseDirectory("invalid-schema");
        PPR_TEST_ASSERT(not schema.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        action = schema.advance(observation, startTime() + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_action_error = std::make_error_code(std::errc::invalid_argument);
        observation.m_action_message = "display.refresh_hz: Refresh is an exclusive fullscreen request only";
        action = schema.advance(observation, startTime() + std::chrono::seconds{2});
        PPR_TEST_ASSERT(caseStatus(schema, "invalid") == EDisplayExerciseStatus::passed);
    };

    PPR_UNIT_TEST (output_directory_must_be_fresh_and_separate_from_the_executable) {
        const auto directory = exerciseDirectory("freshness");
        std::filesystem::create_directories(directory);
        {
            std::ofstream stale{directory / "stale.txt", std::ios::binary};
            stale << "stale";
        }
        DisplayExercise nonempty{};
        PPR_TEST_ASSERT(nonempty.begin(directory, startTime()) == std::errc::directory_not_empty);

        DisplayExercise executable_dir{};
        const std::filesystem::path executable = hal::process::currentExecutablePath();
        PPR_TEST_ASSERT(not executable.empty());
        PPR_TEST_ASSERT(executable_dir.begin(executable.parent_path(), startTime()) == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (passed_requires_every_case_and_clean_shutdown) {
        DisplayExercise exercise{};
        const auto directory = exerciseDirectory("passed");
        std::array steps{step(EDisplayExerciseActionKind::capture_image, "capture")};
        PPR_TEST_ASSERT(not exercise.beginForTest(directory, startTime(), steps));

        DisplayExerciseObservation observation = readyObservation();
        const TimePoint t0 = startTime();
        auto action = exercise.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::capture_image);

        observation.m_capture_complete = true;
        observation.m_capture_path = (directory / "baseline.png").string();
        action = exercise.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        PPR_TEST_ASSERT((**action).m_kind == EDisplayExerciseActionKind::quit);

        observation.m_close_requested = true;
        action = exercise.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        PPR_TEST_ASSERT(not exercise.finish({}));
        PPR_TEST_ASSERT(exercise.status() == EDisplayExerciseStatus::passed);
        PPR_TEST_ASSERT(displayExerciseExitCode(exercise.status()) == 0);
        const std::string report = readFile(directory / "display-exercise.json");
        PPR_TEST_ASSERT(report.find("\"status\":\"passed\"") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"quit_confirmed\":true") != std::string::npos);

        DisplayExercise unclean{};
        const auto second = exerciseDirectory("unclean");
        PPR_TEST_ASSERT(not unclean.beginForTest(second, startTime(), steps));
        observation = readyObservation();
        action = unclean.advance(observation, t0 + std::chrono::seconds{1});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_capture_complete = true;
        action = unclean.advance(observation, t0 + std::chrono::seconds{2});
        PPR_TEST_ASSERT(action.has_value() and action->has_value());
        observation.m_close_requested = true;
        action = unclean.advance(observation, t0 + std::chrono::seconds{3});
        PPR_TEST_ASSERT(action.has_value() and not action->has_value());
        const std::error_code cleanup = std::make_error_code(std::errc::state_not_recoverable);
        PPR_TEST_ASSERT(unclean.finish(cleanup) == cleanup);
        PPR_TEST_ASSERT(unclean.status() == EDisplayExerciseStatus::failed);
    };

} // namespace pP::game::tests::detail::display_exercise

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // display exercise group
    // ------------------------------------------------------------------

    const UnitTest display_exercise = UnitTest::Named("display_exercise") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::display_exercise::begin_rejects_empty_output,
            detail::display_exercise::permanent_plan_reports_schema_and_nine_groups,
            detail::display_exercise::stale_acks_never_advance_and_confirmation_waits_for_matching_ack,
            detail::display_exercise::timeout_waits_for_the_production_confirmation_duration,
            detail::display_exercise::deadline_reports_incomplete_and_still_quits,
            detail::display_exercise::unsupported_capability_is_not_a_pass,
            detail::display_exercise::failed_transaction_is_retained_distinct_from_incomplete,
            detail::display_exercise::user_close_during_pending_resize_stops_actions_and_preserves_evidence,
            detail::display_exercise::revert_restores_the_prior_acknowledged_values,
            detail::display_exercise::reset_restores_startup_display_values,
            detail::display_exercise::save_and_reload_compare_acknowledged_values,
            detail::display_exercise::invalid_draft_never_reaches_native_submission,
            detail::display_exercise::policy_only_apply_does_not_wait_for_a_surface_change,
            detail::display_exercise::matching_ack_without_fresh_surface_does_not_advance,
            detail::display_exercise::cleanup_keep_resolves_an_armed_trial_before_the_next_transaction,
            detail::display_exercise::trial_apply_keep_requires_arm_and_settles_without_late_revert,
            detail::display_exercise::pixel_case_records_exact_grid_and_history_reset,
            detail::display_exercise::non_pixel_live_edits_must_not_reset_history,
            detail::display_exercise::invalid_draft_only_passes_for_the_schema_rejection,
            detail::display_exercise::output_directory_must_be_fresh_and_separate_from_the_executable,
            detail::display_exercise::passed_requires_every_case_and_clean_shutdown,
        });
    };

    const UnitTest &gameDisplayExerciseTests() noexcept {
        return display_exercise;
    }
} // namespace pP::game::tests
