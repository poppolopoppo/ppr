module;

#include "pP/Macros.h"

#include <fstream>

module game.display_exercise;

import engine.app;
import engine.core;
import engine.math;
import game.config;
import game.display;

import std;

namespace pP::game {
    PPR_DEFINE_LOG_CATEGORY(DisplayExercise, info, none)

    namespace {
        constexpr std::string_view kReportFileName{"display-exercise.json"};
        constexpr std::string_view kLogFileName{"display-exercise.log"};
        constexpr std::string_view kBuildKind{PPR_ENABLE_DEBUG ? "debug" : "release"};
#ifndef PPR_GAME_SOURCE_IDENTITY
#define PPR_GAME_SOURCE_IDENTITY "unknown"
#endif
        constexpr std::string_view kSourceIdentity{PPR_GAME_SOURCE_IDENTITY};

        [[nodiscard]] DisplayDraft windowedDraft(const u32 width, const u32 height) {
            DisplayDraft draft{};
            draft.m_window_mode = EDisplayWindowMode::windowed;
            draft.m_width = width;
            draft.m_height = height;
            return draft;
        }

        [[nodiscard]] DisplayDraft policyDraft(
            const EFramePacing pacing, const u32 cap_hz, const bool paced_vsync, const bool throttle_unfocused) {
            DisplayDraft draft{};
            draft.m_pacing = pacing;
            draft.m_cap_hz = cap_hz;
            draft.m_paced_vsync = paced_vsync;
            draft.m_throttle_unfocused = throttle_unfocused;
            return draft;
        }

        [[nodiscard]] DisplayDraft fullscreenDraft(
            const std::string_view monitor, const u32 width, const u32 height, const std::optional<u32> refresh) {
            DisplayDraft draft{};
            draft.m_window_mode = EDisplayWindowMode::fullscreen;
            draft.m_monitor = std::string{monitor};
            draft.m_width = width;
            draft.m_height = height;
            draft.m_refresh_hz = refresh.value_or(0u);
            return draft;
        }

        [[nodiscard]] WorldLightDisplaySettings lightDraft(
            const i32 sky_hour, const ELightingDiagnostic diagnostic = ELightingDiagnostic::off,
            const u32 cascade = 1u) {
            return WorldLightDisplaySettings{sky_hour, diagnostic, cascade};
        }

        [[nodiscard]] PostDisplaySettings postDraft(
            const bool auto_exposure = true, const float exposure_ev = 0.0f, const float adaptation_tau = 0.7f,
            const float ev_min = -4.0f, const float ev_max = 4.0f, const bool bloom = true,
            const float bloom_intensity = 0.125f, const float bloom_threshold = 1.0f) {
            PostDisplaySettings post{};
            post.m_auto_exposure = auto_exposure;
            post.m_exposure_ev = exposure_ev;
            post.m_adaptation_tau = adaptation_tau;
            post.m_ev_min = ev_min;
            post.m_ev_max = ev_max;
            post.m_bloom = bloom;
            post.m_bloom_intensity = bloom_intensity;
            post.m_bloom_threshold = bloom_threshold;
            return post;
        }

        [[nodiscard]] bool sameLight(
            const WorldLightDisplaySettings &lhs, const WorldLightDisplaySettings &rhs) noexcept {
            return lhs.m_sky_hour == rhs.m_sky_hour and
                   lhs.m_diagnostic == rhs.m_diagnostic and
                   lhs.m_cascade == rhs.m_cascade;
        }

        [[nodiscard]] bool samePixel(const PixelDisplaySettings &lhs, const PixelDisplaySettings &rhs) noexcept {
            return lhs.m_pixel_scale == rhs.m_pixel_scale;
        }

        [[nodiscard]] bool samePost(const PostDisplaySettings &lhs, const PostDisplaySettings &rhs) noexcept {
            return lhs.m_auto_exposure == rhs.m_auto_exposure and
                   lhs.m_exposure_ev == rhs.m_exposure_ev and
                   lhs.m_adaptation_tau == rhs.m_adaptation_tau and
                   lhs.m_ev_min == rhs.m_ev_min and
                   lhs.m_ev_max == rhs.m_ev_max and
                   lhs.m_bloom == rhs.m_bloom and
                   lhs.m_bloom_intensity == rhs.m_bloom_intensity and
                   lhs.m_bloom_threshold == rhs.m_bloom_threshold;
        }

        [[nodiscard]] bool sameInspection(
            const LightingInspectionSettings &lhs, const LightingInspectionSettings &rhs) noexcept {
            return lhs.m_diagnostic == rhs.m_diagnostic and lhs.m_cascade == rhs.m_cascade;
        }

        [[nodiscard]] std::string withDetails(std::string_view base, std::string_view detail) {
            std::string text{base};
            if (not detail.empty()) {
                text += ": ";
                text += detail;
            }
            return text;
        }

        [[nodiscard]] TimeSpan secondsSpan(const float seconds) noexcept {
            return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>{seconds});
        }
    } // namespace

    // ------------------------------------------------------------------
    // action names and requested payloads
    // ------------------------------------------------------------------

    std::string_view DisplayExercise::actionName_(const EDisplayExerciseActionKind kind) noexcept {
        switch (kind) {
            case EDisplayExerciseActionKind::apply_display: return "apply_display";
            case EDisplayExerciseActionKind::keep_display: return "keep_display";
            case EDisplayExerciseActionKind::revert_display: return "revert_display";
            case EDisplayExerciseActionKind::reset_display: return "reset_display";
            case EDisplayExerciseActionKind::save_display: return "save_display";
            case EDisplayExerciseActionKind::live_picture: return "live_picture";
            case EDisplayExerciseActionKind::reset_live_picture: return "reset_live_picture";
            case EDisplayExerciseActionKind::restore_live_baseline: return "restore_live_baseline";
            case EDisplayExerciseActionKind::invalid_draft: return "invalid_draft";
            case EDisplayExerciseActionKind::minimize_window: return "minimize_window";
            case EDisplayExerciseActionKind::restore_window: return "restore_window";
            case EDisplayExerciseActionKind::reload_overlay: return "reload_overlay";
            case EDisplayExerciseActionKind::capture_image: return "capture_image";
            case EDisplayExerciseActionKind::wait_suspension: return "wait_suspension";
            case EDisplayExerciseActionKind::quit: return "quit";
        }
        return "unknown";
    }

    std::string DisplayExercise::describeRequested_(
        const Step &step, const DisplayExerciseObservation &observed) const {
        switch (step.m_kind) {
            case EDisplayExerciseActionKind::apply_display:
            case EDisplayExerciseActionKind::invalid_draft:
                return std::format(
                    "window_mode={} monitor={} width={} height={} refresh_hz={} pacing={} cap_hz={} paced_vsync={} throttle_unfocused={}",
                    static_cast<u32>(step.m_draft.m_window_mode), step.m_draft.m_monitor, step.m_draft.m_width,
                    step.m_draft.m_height, step.m_draft.m_refresh_hz, static_cast<u32>(step.m_draft.m_pacing),
                    step.m_draft.m_cap_hz, step.m_draft.m_paced_vsync, step.m_draft.m_throttle_unfocused);
            case EDisplayExerciseActionKind::live_picture: {
                const WorldLightDisplaySettings light = step.m_keep_world_light ? observed.m_live_world_light
                                                                                : step.m_world_light;
                const PixelDisplaySettings pixel = step.m_keep_pixel ? observed.m_live_pixel : step.m_pixel;
                const PostDisplaySettings post = step.m_keep_post ? observed.m_live_post : step.m_post;
                return std::format(
                    "pixel_scale={} sky_hour={} diagnostic={} cascade={} auto_exposure={} exposure_ev={} adaptation_tau={} ev_min={} ev_max={} bloom={} bloom_intensity={} bloom_threshold={}",
                    pixel.m_pixel_scale, light.m_sky_hour, static_cast<u32>(light.m_diagnostic), light.m_cascade,
                    post.m_auto_exposure, post.m_exposure_ev, post.m_adaptation_tau, post.m_ev_min, post.m_ev_max,
                    post.m_bloom, post.m_bloom_intensity, post.m_bloom_threshold);
            }
            case EDisplayExerciseActionKind::capture_image: return "image=" + step.m_image;
            case EDisplayExerciseActionKind::minimize_window:
            case EDisplayExerciseActionKind::restore_window:
            case EDisplayExerciseActionKind::reset_display:
            case EDisplayExerciseActionKind::reset_live_picture:
            case EDisplayExerciseActionKind::restore_live_baseline:
            case EDisplayExerciseActionKind::save_display:
            case EDisplayExerciseActionKind::reload_overlay:
            case EDisplayExerciseActionKind::wait_suspension:
            case EDisplayExerciseActionKind::quit: return std::string{actionName_(step.m_kind)};
            default: return std::string{actionName_(step.m_kind)};
        }
    }

    // ------------------------------------------------------------------
    // status and exit mapping
    // ------------------------------------------------------------------

    std::string_view displayExerciseStatusName(const EDisplayExerciseStatus status) noexcept {
        switch (status) {
            case EDisplayExerciseStatus::pending: return "pending";
            case EDisplayExerciseStatus::passed: return "passed";
            case EDisplayExerciseStatus::failed: return "failed";
            case EDisplayExerciseStatus::unsupported: return "unsupported";
            case EDisplayExerciseStatus::incomplete: return "incomplete";
        }
        return "incomplete";
    }

    int displayExerciseExitCode(const EDisplayExerciseStatus status) noexcept {
        switch (status) {
            case EDisplayExerciseStatus::passed: return 0;
            case EDisplayExerciseStatus::failed: return 1;
            case EDisplayExerciseStatus::unsupported:
            case EDisplayExerciseStatus::incomplete:
            case EDisplayExerciseStatus::pending: return 3;
        }
        return 3;
    }

    // ------------------------------------------------------------------
    // plan construction
    // ------------------------------------------------------------------

    void DisplayExercise::addCase_(std::string id, std::string group, std::string description) {
        m_cases.push_back(
            DisplayExerciseCase{
                .m_id = std::move(id),
                .m_group = std::move(group),
                .m_description = std::move(description),
                .m_status = EDisplayExerciseStatus::pending});
    }

    void DisplayExercise::addStep_(Step step) {
        m_steps.push_back(std::move(step));
    }

    void DisplayExercise::addApplyKeep_(
        std::string case_id, DisplayDraft draft, const bool accept_clamp, const bool completes_case) {
        Step apply{};
        apply.m_kind = EDisplayExerciseActionKind::apply_display;
        apply.m_case = case_id;
        apply.m_draft = std::move(draft);
        apply.m_completes_case = false;
        apply.m_keep_after_ack = true;
        apply.m_accept_clamp = accept_clamp;
        apply.m_record_as_tested = true;
        addStep_(std::move(apply));

        Step keep{};
        keep.m_kind = EDisplayExerciseActionKind::keep_display;
        keep.m_case = std::move(case_id);
        keep.m_completes_case = completes_case;
        keep.m_record_as_tested = false;
        addStep_(std::move(keep));
    }

    void DisplayExercise::addApplyThen_(
        std::string case_id, DisplayDraft draft, const bool revert, const bool timeout) {
        Step apply{};
        apply.m_kind = EDisplayExerciseActionKind::apply_display;
        apply.m_case = case_id;
        apply.m_draft = std::move(draft);
        apply.m_completes_case = not revert;
        apply.m_revert_after_ack = revert;
        apply.m_timeout_after_ack = timeout;
        apply.m_record_as_tested = true;
        addStep_(std::move(apply));

        if (revert) {
            Step back{};
            back.m_kind = EDisplayExerciseActionKind::revert_display;
            back.m_case = std::move(case_id);
            back.m_completes_case = true;
            back.m_record_as_tested = false;
            addStep_(std::move(back));
        }
    }

    void DisplayExercise::addWindowed_(
        std::string case_id, const u32 width, const u32 height, const bool accept_clamp, const bool completes_case) {
        addApplyKeep_(std::move(case_id), windowedDraft(width, height), accept_clamp, completes_case);
    }

    void DisplayExercise::addLive_(
        std::string case_id, const bool keep_world_light, WorldLightDisplaySettings world_light, const bool keep_pixel,
        PixelDisplaySettings pixel, const bool keep_post, PostDisplaySettings post, const bool require_pixel_grid,
        const bool require_inspection_stamp, const bool completes_case) {
        Step step{};
        step.m_kind = EDisplayExerciseActionKind::live_picture;
        step.m_case = std::move(case_id);
        step.m_world_light = std::move(world_light);
        step.m_pixel = pixel;
        step.m_post = std::move(post);
        step.m_keep_world_light = keep_world_light;
        step.m_keep_pixel = keep_pixel;
        step.m_keep_post = keep_post;
        step.m_require_pixel_grid = require_pixel_grid;
        step.m_require_inspection_stamp = require_inspection_stamp;
        step.m_require_unchanged_world = true;
        step.m_completes_case = completes_case;
        addStep_(std::move(step));
    }

    void DisplayExercise::addCapture_(std::string case_id, std::string image) {
        Step step{};
        step.m_kind = EDisplayExerciseActionKind::capture_image;
        step.m_case = std::move(case_id);
        step.m_image = std::move(image);
        step.m_completes_case = true;
        addStep_(std::move(step));
    }

    void DisplayExercise::addFullscreenGroup_() {
        addCase_("fullscreen_discovery", "7-fullscreen", "Fullscreen capability and monitor discovery");
        Step discover{};
        discover.m_kind = EDisplayExerciseActionKind::wait_suspension;
        discover.m_case = "fullscreen_discovery";
        discover.m_completes_case = false;
        discover.m_expand_fullscreen = true;
        addStep_(std::move(discover));

        addCase_(
            "fullscreen_primary_smallest", "7-fullscreen", "Primary monitor smallest supported exclusive resolution");
        addCase_(
            "fullscreen_primary_largest", "7-fullscreen", "Primary monitor largest supported exclusive resolution");
        addCase_(
            "fullscreen_primary_lowest_refresh", "7-fullscreen",
            "Primary monitor lowest known refresh at a supported resolution");
        addCase_(
            "fullscreen_primary_highest_refresh", "7-fullscreen",
            "Primary monitor highest known refresh at a supported resolution");
        addCase_("fullscreen_capture", "7-fullscreen", "Fullscreen group capture");
        addCapture_("fullscreen_capture", "07-fullscreen.png");
        // Monitor cases are discovered at runtime; insert them by group rather
        // than appending after the later transaction/window-lifecycle groups.
        m_fullscreen_case_insert = m_cases.size();
    }

    void DisplayExercise::buildPermanentPlan_() {
        m_cases.clear();
        m_steps.clear();

        // 1. initial load/present and baseline capture.
        addCase_("initial_load", "1-initial-load", "Initial load, baseline identity, extents and image");
        addCapture_("initial_load", "01-baseline.png");

        // 2. windowed requests, odd/even repeats and the platform clamp.
        addCase_("windowed_1280x720_first", "2-windowed", "Windowed 1280x720");
        addWindowed_("windowed_1280x720_first", 1280u, 720u);
        addCase_("windowed_1919x1079", "2-windowed", "Windowed odd 1919x1079");
        addWindowed_("windowed_1919x1079", 1919u, 1079u);
        addCase_("windowed_1921x1081", "2-windowed", "Windowed odd 1921x1081");
        addWindowed_("windowed_1921x1081", 1921u, 1081u);
        addCase_("windowed_640x481", "2-windowed", "Windowed small odd 640x481");
        addWindowed_("windowed_640x481", 640u, 481u);
        addCase_("windowed_1280x720_return", "2-windowed", "Windowed 1280x720 return");
        addWindowed_("windowed_1280x720_return", 1280u, 720u);
        addCase_("windowed_minimum", "2-windowed", "Minimum useful window request and observed platform clamp");
        addWindowed_("windowed_minimum", 320u, 200u, true);
        for (u32 repeat = 1u; repeat <= 3u; ++repeat) {
            const std::string prefix = std::format("windowed_pair{}", repeat);
            addCase_(prefix + "_odd", "2-windowed", "Odd/even repeat 1919x1079");
            addWindowed_(prefix + "_odd", 1919u, 1079u);
            addCase_(prefix + "_even", "2-windowed", "Odd/even repeat 1921x1081");
            addWindowed_(prefix + "_even", 1921u, 1081u);
        }
        addCase_("windowed_capture", "2-windowed", "Windowed group capture");
        addCapture_("windowed_capture", "02-windowed.png");

        // 3. pixelization P1/2/3/8 including both odd extents at P2.
        addCase_("pixel_restore_1280x720", "3-pixelization", "Restore 1280x720 before pixelization");
        addWindowed_("pixel_restore_1280x720", 1280u, 720u);
        for (const u32 scale: {1u, 2u, 3u, 8u}) {
            const std::string id = std::format("pixel_p{}", scale);
            addCase_(id, "3-pixelization", std::format("Pixelization P{} at 1280x720", scale));
            addLive_(id, true, {}, false, PixelDisplaySettings{scale}, true, {}, true, false);
        }
        for (const auto [width, height]: {std::pair{1919u, 1079u}, std::pair{1921u, 1081u}}) {
            const std::string id = std::format("pixel_p2_{}x{}", width, height);
            addCase_(id, "3-pixelization", std::format("Pixelization P2 at {}x{}", width, height));
            addWindowed_(id, width, height, false, false);
            // Exercise a real P transition at this odd extent, then verify the
            // requested P2 grid from the actual framebuffer.
            addLive_(id, true, {}, false, PixelDisplaySettings{1u}, true, {}, false, false, false);
            addLive_(id, true, {}, false, PixelDisplaySettings{2u}, true, {}, true, false, true);
        }
        addCase_("pixel_capture", "3-pixelization", "Pixelization group capture");
        addCapture_("pixel_capture", "03-pixelization.png");

        // 4. exposure and bloom boundaries at the odd 1921x1081 extent.
        addCase_("post_restore_baseline", "4-exposure-bloom", "Restore startup live values");
        addStep_(Step{.m_kind = EDisplayExerciseActionKind::restore_live_baseline, .m_case = "post_restore_baseline"});
        const auto post_case = [this](std::string id, std::string description, PostDisplaySettings post) {
            addCase_(std::move(id), "4-exposure-bloom", std::move(description));
            addLive_(m_cases.back().m_id, true, {}, true, {}, false, std::move(post));
        };
        post_case("post_auto_on", "Automatic exposure on", postDraft());
        post_case("post_auto_off", "Automatic exposure off", postDraft(false));
        post_case("post_ev_minus16", "Manual EV -16", postDraft(false, -16.0f));
        post_case("post_ev_zero", "Manual EV 0", postDraft(false, 0.0f));
        post_case("post_ev_plus16", "Manual EV +16", postDraft(false, 16.0f));
        post_case("post_tau_0_1", "Adaptation tau 0.1", postDraft(true, 0.0f, 0.1f));
        post_case("post_tau_0_7", "Adaptation tau 0.7", postDraft(true, 0.0f, 0.7f));
        post_case("post_tau_5", "Adaptation tau 5", postDraft(true, 0.0f, 5.0f));
        post_case("post_limits_minus8", "Automatic limits [-8,-8]", postDraft(true, 0.0f, 0.7f, -8.0f, -8.0f));
        post_case("post_limits_minus4_4", "Automatic limits [-4,4]", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f));
        post_case("post_limits_plus8", "Automatic limits [8,8]", postDraft(true, 0.0f, 0.7f, 8.0f, 8.0f));
        post_case("post_bloom_off", "Bloom off", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, false));
        post_case("post_bloom_on", "Bloom on", postDraft());
        post_case("post_intensity_0", "Bloom intensity 0", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 0.0f));
        post_case(
            "post_intensity_0_125", "Bloom intensity 0.125", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 0.125f));
        post_case("post_intensity_2", "Bloom intensity 2", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 2.0f));
        post_case(
            "post_threshold_0", "Bloom threshold 0", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 0.125f, 0.0f));
        post_case(
            "post_threshold_1", "Bloom threshold 1", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 0.125f, 1.0f));
        post_case(
            "post_threshold_16", "Bloom threshold 16", postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, true, 0.125f, 16.0f));
        post_case(
            "post_auto_off_bloom_on_odd", "Auto exposure off with bloom on at an odd extent",
            postDraft(false, 0.0f, 0.7f, -4.0f, 4.0f, true));
        post_case(
            "post_auto_on_bloom_off_odd", "Auto exposure on with bloom off at an odd extent",
            postDraft(true, 0.0f, 0.7f, -4.0f, 4.0f, false));
        addCase_("post_capture", "4-exposure-bloom", "Exposure and bloom group capture");
        addCapture_("post_capture", "04-post.png");
        addCase_("post_restore_baseline_end", "4-exposure-bloom", "Restore startup live values after exposure/bloom");
        addStep_(
            Step{.m_kind = EDisplayExerciseActionKind::restore_live_baseline, .m_case = "post_restore_baseline_end"});

        // 5. sky hours, every diagnostic enum and cascade inspection levels.
        addCase_("inspection_restore_baseline", "5-inspection", "Restore startup live values");
        addStep_(
            Step{.m_kind = EDisplayExerciseActionKind::restore_live_baseline, .m_case = "inspection_restore_baseline"});
        for (const i32 hour: {-1, 0, 12, 23}) {
            const std::string id = std::format("sky_{}", hour);
            addCase_(id, "5-inspection", std::format("Sky hour {}", hour));
            addLive_(id, false, lightDraft(hour), true, {}, true, {});
        }
        constexpr std::array diagnostics{
            ELightingDiagnostic::off,
            ELightingDiagnostic::probes,
            ELightingDiagnostic::source,
            ELightingDiagnostic::occupancy,
            ELightingDiagnostic::jump,
            ELightingDiagnostic::cascade,
        };
        for (const ELightingDiagnostic diagnostic: diagnostics) {
            const std::string id = std::format("diagnostic_{}", static_cast<u32>(diagnostic));
            addCase_(id, "5-inspection", std::format("Diagnostic view {}", static_cast<u32>(diagnostic)));
            addLive_(id, false, lightDraft(12, diagnostic, 1u), true, {}, true, {}, false, true);
        }
        for (const u32 cascade: {1u, 5u}) {
            const std::string id = std::format("cascade_{}", cascade);
            addCase_(id, "5-inspection", std::format("Cascade inspection level {}", cascade));
            addLive_(id, false, lightDraft(12, ELightingDiagnostic::cascade, cascade), true, {}, true, {}, false, true);
        }
        addCase_("inspection_capture", "5-inspection", "Inspection group capture");
        addCapture_("inspection_capture", "05-inspection.png");
        addCase_(
            "inspection_restore_baseline_end", "5-inspection",
            "Restore startup live values after sky/diagnostic/cascade");
        addStep_(
            Step{
                .m_kind = EDisplayExerciseActionKind::restore_live_baseline,
                .m_case = "inspection_restore_baseline_end"});

        // 6. pacing, cap domain, VSync and unfocused throttling.
        struct PacingCase final {
            const char *m_id;
            const char *m_description;
            EFramePacing m_pacing;
            u32 m_cap_hz;
            bool m_vsync;
            bool m_throttle;
        };
        constexpr std::array pacing_cases{
            PacingCase{"pacing_uncapped", "Pacing uncapped", EFramePacing::uncapped, 60u, true, true},
            PacingCase{"pacing_follow_vsync", "Pacing follow-vsync", EFramePacing::follow_vsync, 60u, true, true},
            PacingCase{"pacing_paced", "Pacing paced", EFramePacing::paced, 60u, true, true},
            PacingCase{"pacing_cap_min", "Frame cap minimum", EFramePacing::paced, 1u, true, true},
            PacingCase{"pacing_cap_default", "Frame cap default", EFramePacing::paced, 60u, true, true},
            PacingCase{"pacing_cap_max", "Frame cap maximum", EFramePacing::paced, 1000u, true, true},
            PacingCase{"pacing_vsync_on", "VSync on", EFramePacing::paced, 60u, true, true},
            PacingCase{"pacing_vsync_off", "VSync off", EFramePacing::paced, 60u, false, true},
            PacingCase{"pacing_throttle_on", "Unfocused throttle on", EFramePacing::paced, 60u, true, true},
            PacingCase{"pacing_throttle_off", "Unfocused throttle off", EFramePacing::paced, 60u, true, false},
            PacingCase{
                "pacing_restore_baseline", "Restore baseline pacing", EFramePacing::follow_vsync, 60u, true, true},
        };
        for (const PacingCase &entry: pacing_cases) {
            addCase_(entry.m_id, "6-pacing", entry.m_description);
            addApplyKeep_(entry.m_id, policyDraft(entry.m_pacing, entry.m_cap_hz, entry.m_vsync, entry.m_throttle));
        }
        addCase_("pacing_capture", "6-pacing", "Pacing group capture");
        addCapture_("pacing_capture", "06-pacing.png");

        // 7. fullscreen discovery and per-monitor/mode coverage.
        addFullscreenGroup_();

        // 8. disruptive trials, Reset, Save, isolated reload and invalid drafts.
        addCase_("trial_apply_keep", "8-transactions", "Apply a different window, wait for the armed trial, then Keep");
        addWindowed_("trial_apply_keep", 1024u, 768u);
        m_steps[m_steps.size() - 2u].m_require_trial_armed = true;
        addCase_("trial_apply_revert", "8-transactions", "Apply then Revert");
        addApplyThen_("trial_apply_revert", windowedDraft(1919u, 1079u), true, false);
        addCase_("trial_apply_timeout", "8-transactions", "Apply then production confirmation timeout");
        addApplyThen_("trial_apply_timeout", windowedDraft(640u, 481u), false, true);

        addCase_("reset_display_overrides", "8-transactions", "Reset runtime display overrides through the panel path");
        addApplyKeep_("reset_display_overrides", policyDraft(EFramePacing::paced, 120u, false, true), false, false);
        addStep_(
            Step{
                .m_kind = EDisplayExerciseActionKind::reset_display,
                .m_case = "reset_display_overrides",
                .m_completes_case = false});
        addStep_(
            Step{
                .m_kind = EDisplayExerciseActionKind::keep_display,
                .m_case = "reset_display_overrides",
                .m_completes_case = true,
                .m_record_as_tested = false});

        addCase_("reset_live_picture", "8-transactions", "Reset live lighting and post through their own action");
        addLive_(
            "reset_live_picture", false, lightDraft(23, ELightingDiagnostic::cascade, 5u), false,
            PixelDisplaySettings{5u}, false, postDraft(false, 8.0f, 1.5f, -3.0f, 3.0f, true, 1.0f, 2.0f), false, false);
        m_steps.back().m_completes_case = false;
        addStep_(
            Step{
                .m_kind = EDisplayExerciseActionKind::reset_live_picture,
                .m_case = "reset_live_picture",
                .m_completes_case = true});

        addCase_("save_overlay", "8-transactions", "Save acknowledged values to the isolated overlay");
        addStep_(Step{.m_kind = EDisplayExerciseActionKind::save_display, .m_case = "save_overlay"});
        addCase_("reload_overlay", "8-transactions", "Reload the isolated overlay through the production resolver");
        addStep_(Step{.m_kind = EDisplayExerciseActionKind::reload_overlay, .m_case = "reload_overlay"});
        addCase_("invalid_draft_rejected", "8-transactions", "Invalid draft rejected before native mutation");
        {
            DisplayDraft invalid = windowedDraft(1280u, 720u);
            invalid.m_refresh_hz = 60u;
            Step step{};
            step.m_kind = EDisplayExerciseActionKind::invalid_draft;
            step.m_case = "invalid_draft_rejected";
            step.m_draft = std::move(invalid);
            step.m_expect_rejection = true;
            addStep_(std::move(step));
        }
        addCase_("transactions_capture", "8-transactions", "Transaction group capture");
        addCapture_("transactions_capture", "08-transactions.png");

        // 9. minimize/restore twice and the ordinary production quit.
        for (u32 round = 1u; round <= 2u; ++round) {
            const std::string minimize = std::format("minimize_{}", round);
            addCase_(minimize, "9-window-lifecycle", "Minimize through the ordinary window service");
            Step iconify{};
            iconify.m_kind = EDisplayExerciseActionKind::minimize_window;
            iconify.m_case = minimize;
            iconify.m_record_as_tested = true;
            addStep_(std::move(iconify));

            const std::string suspension = std::format("minimized_suspension_{}", round);
            addCase_(suspension, "9-window-lifecycle", "No fresh presented frame while minimized");
            Step wait{};
            wait.m_kind = EDisplayExerciseActionKind::wait_suspension;
            wait.m_case = suspension;
            wait.m_record_as_tested = false;
            addStep_(std::move(wait));

            const std::string restore = std::format("restore_{}", round);
            addCase_(restore, "9-window-lifecycle", "Restore and observe resumed presentation");
            Step resume{};
            resume.m_kind = EDisplayExerciseActionKind::restore_window;
            resume.m_case = restore;
            resume.m_require_minimized = true;
            resume.m_record_as_tested = true;
            addStep_(std::move(resume));
        }
        addCase_("quit", "9-window-lifecycle", "Ordinary production quit and clean drain");
        Step quit{};
        quit.m_kind = EDisplayExerciseActionKind::quit;
        quit.m_case = "quit";
        addStep_(std::move(quit));
    }

    void DisplayExercise::expandFullscreen_(const DisplayExerciseObservation &observed) {
        std::ignore = observed;
        const auto finishUnsupported = [this](std::string_view reason) {
            markCase_("fullscreen_discovery", EDisplayExerciseStatus::unsupported, std::string{reason});
            for (const std::string_view id:
                {"fullscreen_primary_smallest", "fullscreen_primary_largest", "fullscreen_primary_lowest_refresh",
                    "fullscreen_primary_highest_refresh"}) {
                markCase_(id, EDisplayExerciseStatus::unsupported, std::string{reason});
            }
        };

        if (not m_capabilities_available) {
            finishUnsupported(
                m_capabilities_error ? m_capabilities_error.message()
                                     : std::string{"display capabilities unavailable"});
            return;
        }
        if (not m_capabilities.m_window_changes) {
            finishUnsupported("native window changes unsupported");
            return;
        }
        if (m_capabilities.m_monitors.empty()) {
            finishUnsupported("no enumerated monitors");
            return;
        }

        markCase_("fullscreen_discovery", EDisplayExerciseStatus::passed, "capabilities enumerated");

        // Discovered per-monitor cases are inserted inside group 7 rather than
        // appended after the later groups.
        const auto addFullscreenCase = [this](std::string id, std::string description) {
            DisplayExerciseCase entry{};
            entry.m_id = std::move(id);
            entry.m_group = "7-fullscreen";
            entry.m_description = std::move(description);
            entry.m_status = EDisplayExerciseStatus::pending;
            m_cases.insert(m_cases.begin() + static_cast<std::ptrdiff_t>(m_fullscreen_case_insert), std::move(entry));
            ++m_fullscreen_case_insert;
        };

        Array<Step> inserted{};
        const auto appendSteps = [&](const std::string &case_id, const std::string_view monitor,
                                     const DisplayModeCapability &mode) {
            Step fullscreen{};
            fullscreen.m_kind = EDisplayExerciseActionKind::apply_display;
            fullscreen.m_case = case_id;
            fullscreen.m_draft = fullscreenDraft(monitor, mode.m_width, mode.m_height, mode.m_refresh_hz);
            fullscreen.m_completes_case = false;
            fullscreen.m_accept_clamp = true;
            fullscreen.m_record_as_tested = true;
            inserted.push_back(std::move(fullscreen));

            Step keep{};
            keep.m_kind = EDisplayExerciseActionKind::keep_display;
            keep.m_case = case_id;
            keep.m_completes_case = false;
            keep.m_record_as_tested = false;
            inserted.push_back(std::move(keep));

            Step back{};
            back.m_kind = EDisplayExerciseActionKind::apply_display;
            back.m_case = case_id;
            back.m_draft = windowedDraft(1280u, 720u);
            back.m_completes_case = false;
            back.m_record_as_tested = false;
            inserted.push_back(std::move(back));

            Step back_keep{};
            back_keep.m_kind = EDisplayExerciseActionKind::keep_display;
            back_keep.m_case = case_id;
            back_keep.m_completes_case = true;
            back_keep.m_record_as_tested = false;
            inserted.push_back(std::move(back_keep));
        };

        struct CoveredMode final {
            std::string m_monitor{};
            std::string m_case{};
            u32 m_width{};
            u32 m_height{};
            std::optional<u32> m_refresh_hz{};
        };
        Array<CoveredMode> covered{};
        const auto coveringCase = [&](const std::string_view monitor,
                                      const DisplayModeCapability &mode) -> const CoveredMode * {
            for (const CoveredMode &entry: covered) {
                if (entry.m_monitor == monitor and
                    entry.m_width == mode.m_width and
                    entry.m_height == mode.m_height and
                    entry.m_refresh_hz == mode.m_refresh_hz) {
                    return &entry;
                }
            }
            return nullptr;
        };
        const auto noteCovered = [&](std::string_view monitor, const std::string &case_id,
                                     const DisplayModeCapability &mode) {
            covered.push_back(
                CoveredMode{
                    .m_monitor = std::string{monitor},
                    .m_case = case_id,
                    .m_width = mode.m_width,
                    .m_height = mode.m_height,
                    .m_refresh_hz = mode.m_refresh_hz});
        };

        // Every enumerated unambiguous monitor at its current supported mode.
        for (std::size_t index = 0u; index < m_capabilities.m_monitors.size(); ++index) {
            const DisplayMonitorCapability &monitor = m_capabilities.m_monitors[index];
            const std::string case_id = std::format("fullscreen_monitor_{}", index);
            bool ambiguous = false;
            for (std::size_t other = 0u; other < m_capabilities.m_monitors.size(); ++other) {
                if (other != index and m_capabilities.m_monitors[other].m_name == monitor.m_name) {
                    ambiguous = true;
                    break;
                }
            }
            if (ambiguous) {
                addFullscreenCase(case_id, "Monitor fullscreen (ambiguous duplicate name)");
                markCase_(case_id, EDisplayExerciseStatus::unsupported, "ambiguous duplicate monitor name");
                continue;
            }
            if (monitor.m_current.m_width == 0u or monitor.m_current.m_height == 0u) {
                addFullscreenCase(case_id, "Monitor fullscreen (no current mode)");
                markCase_(case_id, EDisplayExerciseStatus::unsupported, "monitor reports no current mode");
                continue;
            }
            addFullscreenCase(case_id, std::format("Monitor '{}' fullscreen at its current mode", monitor.m_name));
            appendSteps(case_id, monitor.m_name, monitor.m_current);
            noteCovered(monitor.m_name, case_id, monitor.m_current);
        }

        // Primary monitor resolution/refresh boundaries, deduplicated.
        const DisplayMonitorCapability *primary = nullptr;
        for (const DisplayMonitorCapability &monitor: m_capabilities.m_monitors) {
            if (monitor.m_primary) {
                primary = &monitor;
                break;
            }
        }
        if (primary == nullptr) {
            primary = &m_capabilities.m_monitors.front();
        }
        bool primary_unique = true;
        for (const DisplayMonitorCapability &monitor: m_capabilities.m_monitors) {
            if (&monitor != primary and monitor.m_name == primary->m_name) {
                primary_unique = false;
            }
        }
        if (not primary_unique or primary->m_modes.empty()) {
            const std::string reason = not primary_unique ? "ambiguous primary monitor name" : "no enumerated modes";
            for (const std::string_view id:
                {"fullscreen_primary_smallest", "fullscreen_primary_largest", "fullscreen_primary_lowest_refresh",
                    "fullscreen_primary_highest_refresh"}) {
                markCase_(id, EDisplayExerciseStatus::unsupported, reason);
            }
        } else {
            const auto area = [](const DisplayModeCapability &mode) {
                return static_cast<u64>(mode.m_width) * static_cast<u64>(mode.m_height);
            };
            const DisplayModeCapability *smallest = nullptr;
            const DisplayModeCapability *largest = nullptr;
            const DisplayModeCapability *lowest_refresh = nullptr;
            const DisplayModeCapability *highest_refresh = nullptr;
            for (const DisplayModeCapability &mode: primary->m_modes) {
                if (mode.m_width == 0u or mode.m_height == 0u) {
                    continue;
                }
                if (smallest == nullptr or area(mode) < area(*smallest)) {
                    smallest = &mode;
                }
                if (largest == nullptr or area(mode) > area(*largest)) {
                    largest = &mode;
                }
                if (mode.m_refresh_hz) {
                    if (lowest_refresh == nullptr or *mode.m_refresh_hz < *lowest_refresh->m_refresh_hz) {
                        lowest_refresh = &mode;
                    }
                    if (highest_refresh == nullptr or *mode.m_refresh_hz > *highest_refresh->m_refresh_hz) {
                        highest_refresh = &mode;
                    }
                }
            }

            const auto appendPrimary = [&](const std::string_view case_id, const DisplayModeCapability *mode) {
                if (mode == nullptr) {
                    markCase_(case_id, EDisplayExerciseStatus::unsupported, "no supported mode");
                    return;
                }
                if (const CoveredMode *covered_by = coveringCase(primary->m_name, *mode); covered_by != nullptr) {
                    markCase_(
                        case_id, EDisplayExerciseStatus::passed,
                        "identical mode already exercised by " + covered_by->m_case);
                    return;
                }
                appendSteps(std::string{case_id}, primary->m_name, *mode);
                noteCovered(primary->m_name, std::string{case_id}, *mode);
            };

            appendPrimary("fullscreen_primary_smallest", smallest);
            appendPrimary("fullscreen_primary_largest", largest);
            appendPrimary("fullscreen_primary_lowest_refresh", lowest_refresh);
            appendPrimary("fullscreen_primary_highest_refresh", highest_refresh);
        }

        // Replace the discovery marker with the generated steps.
        if (m_next_step < m_steps.size()) {
            m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_next_step));
        }
        m_steps.insert(m_steps.begin() + static_cast<std::ptrdiff_t>(m_next_step), inserted.begin(), inserted.end());
    }

    // ------------------------------------------------------------------
    // begin and finish
    // ------------------------------------------------------------------

    std::error_code DisplayExercise::begin(const std::filesystem::path &output, const TimePoint now) {
        if (output.empty()) {
            PPR_RETURN_ERROR_ON_FAIL(DisplayExercise, std::make_error_code(std::errc::invalid_argument));
        }
        // Never let the exercise write its isolated overlay over the user's
        // executable-adjacent picture settings.
        std::error_code error{};
        const std::filesystem::path executable = hal::process::currentExecutablePath();
        if (not executable.empty()) {
            const std::filesystem::path executable_dir = std::filesystem::weakly_canonical(
                executable.parent_path(), error);
            if (not error) {
                const std::filesystem::path output_dir = std::filesystem::weakly_canonical(output, error);
                if (not error and output_dir == executable_dir) {
                    PPR_RETURN_ERROR_ON_FAIL(DisplayExercise, std::make_error_code(std::errc::invalid_argument));
                }
            }
            error = {};
        }

        // Fresh artifact directory only: a stale/non-empty directory is
        // rejected instead of mixing old images, JSON, logs or an overlay.
        if (std::filesystem::exists(output, error)) {
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(DisplayExercise, error);
            }
            if (not std::filesystem::is_directory(output, error) or error) {
                PPR_RETURN_ERROR_ON_FAIL(
                    DisplayExercise, error ? error : std::make_error_code(std::errc::not_a_directory));
            }
            if (not std::filesystem::is_empty(output, error) or error) {
                PPR_RETURN_ERROR_ON_FAIL(
                    DisplayExercise, error ? error : std::make_error_code(std::errc::directory_not_empty));
            }
        } else {
            std::filesystem::create_directories(output, error);
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(DisplayExercise, error);
            }
        }

        m_output = output;
        m_log_path = output / kLogFileName;
        m_report_path = output / kReportFileName;
        m_begin = now;
        m_last_now = now;
        m_phase = EPhase::loading;
        m_status = EDisplayExerciseStatus::pending;

        {
            std::ofstream log{m_log_path, std::ios::binary | std::ios::trunc};
        }

        buildPermanentPlan_();

        writeLog_(std::format("begin output={}", m_output.string()));
        PPR_RETURN_ERROR_ON_FAIL(DisplayExercise, writeReport_());
        return {};
    }

    std::error_code DisplayExercise::beginForTest(
        const std::filesystem::path &output, const TimePoint now,
        const std::span<const DisplayExerciseTestStep> steps) {
        if (const std::error_code error = begin(output, now)) {
            return error;
        }

        m_cases.clear();
        m_steps.clear();
        m_next_step = 0u;
        m_fullscreen_case_insert = m_cases.size();
        for (const DisplayExerciseTestStep &source: steps) {
            if (source.m_case.empty()) {
                continue;
            }
            if (findCase_(source.m_case) == nullptr) {
                addCase_(source.m_case, "test", source.m_case);
            }
            Step step{};
            step.m_kind = source.m_kind;
            step.m_case = source.m_case;
            step.m_draft = source.m_draft;
            step.m_world_light = source.m_world_light;
            step.m_pixel = source.m_pixel;
            step.m_post = source.m_post;
            step.m_completes_case = source.m_completes_case;
            step.m_expect_rejection = source.m_expect_rejection;
            step.m_keep_after_ack = source.m_keep_after_ack;
            step.m_revert_after_ack = source.m_revert_after_ack;
            step.m_timeout_after_ack = source.m_timeout_after_ack;
            step.m_require_pixel_grid = source.m_require_pixel_grid;
            step.m_require_inspection_stamp = source.m_require_inspection_stamp;
            step.m_require_unchanged_world = source.m_require_unchanged_world;
            step.m_accept_clamp = source.m_accept_clamp;
            step.m_require_trial_armed = source.m_require_trial_armed;
            addStep_(std::move(step));
        }

        if (findCase_("quit") == nullptr) {
            addCase_("quit", "test", "Ordinary production quit and clean drain");
            addStep_(Step{.m_kind = EDisplayExerciseActionKind::quit, .m_case = "quit"});
        }
        return writeReport_();
    }

    std::error_code DisplayExercise::finish(const std::error_code cleanup_error) {
        if (m_finalized) {
            return displayExerciseExitCode(m_status) == 0 ? std::error_code{}
                                                          : std::make_error_code(std::errc::state_not_recoverable);
        }
        m_finalized = true;
        m_cleanup_error = cleanup_error;
        m_phase = EPhase::finished;

        if (DisplayExerciseCase *quit = findCase_("quit");
            quit != nullptr and quit->m_status == EDisplayExerciseStatus::pending) {
            if (m_quit_confirmed and not cleanup_error) {
                quit->m_status = EDisplayExerciseStatus::passed;
            } else {
                quit->m_status = EDisplayExerciseStatus::incomplete;
                quit->m_reason = cleanup_error ? withDetails("shutdown error", cleanup_error.message())
                                               : std::string{"production quit was not observed"};
            }
        }
        markUnreached_(
            cleanup_error ? withDetails("incomplete run", cleanup_error.message())
                          : std::string{"run ended before case completion"});
        computeFinalStatus_();
        writeLog_(
            std::format(
                "finish status={} cleanup_error={}", displayExerciseStatusName(m_status),
                cleanup_error ? cleanup_error.message() : "none"));
        if (const std::error_code error = writeReport_()) {
            m_status = EDisplayExerciseStatus::failed;
            return error;
        }
        switch (m_status) {
            case EDisplayExerciseStatus::passed: return {};
            case EDisplayExerciseStatus::failed: return std::make_error_code(std::errc::state_not_recoverable);
            case EDisplayExerciseStatus::unsupported: return std::make_error_code(std::errc::operation_not_supported);
            case EDisplayExerciseStatus::incomplete:
            case EDisplayExerciseStatus::pending: return std::make_error_code(std::errc::timed_out);
        }
        return std::make_error_code(std::errc::timed_out);
    }

    // ------------------------------------------------------------------
    // advance
    // ------------------------------------------------------------------

    Expected<std::optional<DisplayExerciseAction>> DisplayExercise::advance(
        const DisplayExerciseObservation &observed, const TimePoint now) {
        return run_(observed, now);
    }

    Expected<std::optional<DisplayExerciseAction>> DisplayExercise::run_(
        const DisplayExerciseObservation &observed, const TimePoint now) {
        if (m_phase == EPhase::idle or m_phase == EPhase::finished) {
            return std::optional<DisplayExerciseAction>{};
        }
        m_last_now = now;

        // The host refreshes the capability snapshot only when asked; keep at
        // most two waits so an unavailable enumeration cannot spin.
        if (observed.m_capabilities_available and m_need_capabilities) {
            m_capabilities = observed.m_capabilities;
            m_capabilities_available = true;
            m_capabilities_error = {};
            m_need_capabilities = false;
            m_capability_waits = 0u;
        } else if (m_need_capabilities) {
            ++m_capability_waits;
            if (m_capability_waits >= 2u) {
                m_capabilities = observed.m_capabilities;
                m_capabilities_available = observed.m_capabilities_available;
                m_capabilities_error = observed.m_capabilities_error;
                m_need_capabilities = false;
                m_capability_waits = 0u;
            }
        }

        // A real user close outranks every pending plan step: stop emitting,
        // retain cleanup evidence, and never start another native trial.
        if (observed.m_close_requested and not m_quit_emitted and not m_user_close) {
            onUserClose_(observed, now);
            return std::optional<DisplayExerciseAction>{};
        }
        if (m_quit_emitted and observed.m_close_requested) {
            m_quit_confirmed = true;
            if (DisplayExerciseCase *quit = findCase_("quit");
                quit != nullptr and quit->m_status == EDisplayExerciseStatus::pending) {
                quit->m_status = EDisplayExerciseStatus::passed;
            }
        }
        if (m_actions_suppressed) {
            return std::optional<DisplayExerciseAction>{};
        }

        if (m_cleanup_pending) {
            if (not observed.m_confirm_armed and not observed.m_transaction_pending) {
                m_cleanup_pending = false;
                m_cleanup_keep_sent = false;
                writeLog_("trial cleanup keep settled");
            } else if (now - m_await_start > secondsSpan(5.0f)) {
                m_cleanup_pending = false;
                m_stop_reason = "cleanup Keep did not resolve the trial";
                m_status = EDisplayExerciseStatus::incomplete;
                beginStopping_(m_stop_reason);
                return std::optional<DisplayExerciseAction>{};
            } else {
                return std::optional<DisplayExerciseAction>{};
            }
        }

        if (m_status == EDisplayExerciseStatus::pending and now - m_begin > secondsSpan(kDisplayExerciseRunSeconds)) {
            m_stop_reason = "run deadline";
            markUnreached_("run deadline");
            m_status = EDisplayExerciseStatus::incomplete;
            beginStopping_(m_stop_reason);
        }

        if (m_phase == EPhase::loading) {
            if (now - m_begin > secondsSpan(kDisplayExerciseInitialLoadSeconds)) {
                markCase_(
                    "initial_load", EDisplayExerciseStatus::incomplete, "initial world/present deadline exceeded");
                if (DisplayExerciseCase *quit = findCase_("quit"); quit != nullptr) {
                    quit->m_status = EDisplayExerciseStatus::pending;
                }
                m_stop_reason = "initial-load deadline";
                m_status = EDisplayExerciseStatus::incomplete;
                beginStopping_(m_stop_reason);
                return std::optional<DisplayExerciseAction>{};
            }
            if (not observed.m_world_ready or observed.m_presented_frame == 0u) {
                return std::optional<DisplayExerciseAction>{};
            }
            captureBaseline_(observed, now);
            m_phase = EPhase::running;
            m_need_capabilities = true;
        }

        if (m_phase == EPhase::draining) {
            if (not observed.m_transaction_pending) {
                m_phase = EPhase::running;
            } else if (now - m_await_start > secondsSpan(kDisplayExerciseConfirmRollbackSeconds)) {
                m_stop_reason = "outstanding transaction did not settle";
                m_status = EDisplayExerciseStatus::incomplete;
                beginStopping_(m_stop_reason);
                return std::optional<DisplayExerciseAction>{};
            } else {
                return std::optional<DisplayExerciseAction>{};
            }
        }

        if (m_phase == EPhase::stopping) {
            if (m_quit_emitted) {
                return std::optional<DisplayExerciseAction>{};
            }
            // Never overlap an outstanding native trial with the quit action.
            if (observed.m_transaction_pending) {
                return std::optional<DisplayExerciseAction>{};
            }
            writeLog_("emit quit");
            DisplayExerciseAction action{};
            action.m_kind = EDisplayExerciseActionKind::quit;
            action.m_case = "quit";
            auto published = publish_(std::move(action));
            if (published) {
                m_quit_emitted = true;
            }
            return published;
        }

        if (m_awaiting_step) {
            if (not pollAwaiting_(observed, now)) {
                return std::optional<DisplayExerciseAction>{};
            }
            m_awaiting_step.reset();
            if (m_phase != EPhase::running) {
                return std::optional<DisplayExerciseAction>{};
            }
        }

        while (m_next_step < m_steps.size()) {
            Step &step = m_steps[m_next_step];

            // A rejected/failed trial keeps the production confirmation gate
            // armed. Resolve it with the same Keep intent the panel emits
            // before starting another transactional case; this is not a
            // second policy and it never overlaps the outstanding trial.
            const bool transactional = step.m_kind == EDisplayExerciseActionKind::apply_display or
                                       step.m_kind == EDisplayExerciseActionKind::reset_display or
                                       step.m_kind == EDisplayExerciseActionKind::invalid_draft;
            if (not observed.m_confirm_armed) {
                m_cleanup_keep_sent = false;
            } else if (transactional and not m_cleanup_keep_sent) {
                m_cleanup_keep_sent = true;
                m_cleanup_pending = true;
                m_await_start = now;
                writeLog_(std::format("emit trial cleanup keep before case={}", step.m_case));
                DisplayExerciseAction action{};
                action.m_kind = EDisplayExerciseActionKind::keep_display;
                action.m_case = step.m_case;
                return publish_(std::move(action));
            }

            if (step.m_expand_fullscreen) {
                expandFullscreen_(observed);
                continue;
            }
            if (not gateStep_(step, observed)) {
                ++m_next_step;
                continue;
            }
            const std::size_t index = m_next_step;
            auto action = emitStep_(step, observed, now);
            if (not action) {
                ++m_next_step;
                continue;
            }
            auto published = publish_(std::move(*action));
            if (not published) {
                ++m_next_step;
                continue;
            }
            m_awaiting_step = index;
            ++m_next_step;
            m_phase = EPhase::awaiting;
            return published;
        }

        if (not m_quit_emitted) {
            m_phase = EPhase::stopping;
            return run_(observed, now);
        }
        return std::optional<DisplayExerciseAction>{};
    }

    void DisplayExercise::captureBaseline_(const DisplayExerciseObservation &observed, const TimePoint now) {
        m_baseline_recorded = true;
        m_baseline_display = observed.m_committed;
        m_baseline_world_light = observed.m_live_world_light;
        m_baseline_pixel = observed.m_live_pixel;
        m_baseline_post = observed.m_live_post;
        m_baseline_world = observed.m_presented_world;
        m_baseline_logical = observed.m_logical_extent;
        m_baseline_framebuffer = observed.m_framebuffer_extent;
        m_baseline_revision = observed.m_settings_revision;
        m_baseline_frame = observed.m_presented_frame;
        m_cli_assignments = observed.m_cli_assignments;
        m_backend = observed.m_backend;
        m_gpu = observed.m_gpu;
        m_executable_path = observed.m_executable_path;
        m_executable_bytes = observed.m_executable_bytes;
        if (const auto grid = makePresentationGrid(
                observed.m_framebuffer_extent, observed.m_live_pixel.m_pixel_scale)) {
            m_baseline_grid = *grid;
        }
        writeLog_(
            std::format(
                "baseline revision={} frame={} logical={}x{} framebuffer={}x{} world={}/{}/{:016x}",
                m_baseline_revision, m_baseline_frame, m_baseline_logical.x, m_baseline_logical.y,
                m_baseline_framebuffer.x, m_baseline_framebuffer.y, m_baseline_world.m_job_id,
                m_baseline_world.m_world_generation, m_baseline_world.m_content_fingerprint));
        std::ignore = now;
    }

    bool DisplayExercise::gateStep_(Step &step, const DisplayExerciseObservation &observed) {
        const auto unsupported = [&](std::string reason) {
            markCase_(step.m_case, EDisplayExerciseStatus::unsupported, std::move(reason));
            return false;
        };
        const auto incomplete = [&](std::string reason) {
            markCase_(step.m_case, EDisplayExerciseStatus::incomplete, std::move(reason));
            return false;
        };

        // A case already classified unsupported/failed/incomplete owns no
        // further actions; skip its remaining steps instead of re-entering a
        // half-finished transaction pair.
        if (const DisplayExerciseCase *entry = findCase_(step.m_case);
            entry != nullptr and
            entry->m_status != EDisplayExerciseStatus::pending and
            entry->m_status != EDisplayExerciseStatus::passed) {
            return false;
        }

        switch (step.m_kind) {
            case EDisplayExerciseActionKind::apply_display:
            case EDisplayExerciseActionKind::invalid_draft: {
                const auto settings = displaySettingsFrom(step.m_draft);
                if (not settings) {
                    return incomplete("draft conversion failed");
                }
                if (not step.m_expect_rejection and
                    requestsWindowChange_(*settings, observed.m_committed, m_capabilities)) {
                    if (not m_capabilities_available) {
                        return unsupported(
                            m_capabilities_error ? m_capabilities_error.message()
                                                 : std::string{"capabilities unavailable"});
                    }
                    if (not m_capabilities.m_window_changes) {
                        return unsupported("native window changes unsupported");
                    }
                    if (settings->m_window_mode == EDisplayWindowMode::fullscreen) {
                        const DisplayMonitorSelection selection = findDisplayMonitor(
                            settings->m_monitor, m_capabilities);
                        if (selection.m_ambiguous) {
                            return unsupported("ambiguous monitor name");
                        }
                        if (selection.m_monitor == nullptr) {
                            return unsupported("monitor not enumerated");
                        }
                    }
                }
                break;
            }
            case EDisplayExerciseActionKind::live_picture:
            case EDisplayExerciseActionKind::reset_live_picture:
            case EDisplayExerciseActionKind::restore_live_baseline:
                if (observed.m_framebuffer_extent.x <= 0 or observed.m_framebuffer_extent.y <= 0) {
                    return incomplete("framebuffer extent unavailable");
                }
                break;
            default: break;
        }
        return true;
    }

    std::optional<DisplayExerciseAction> DisplayExercise::publish_(DisplayExerciseAction action) {
        if (m_actions_suppressed) {
            // Real telemetry: any emission path reached after a user close is
            // counted, so the injected-time test fails instead of trusting a
            // constant zero.
            ++m_emitted_after_close;
            writeLog_(std::format("suppressed emission after close case={}", action.m_case));
            return std::nullopt;
        }
        ++m_emitted_actions;
        return action;
    }

    std::optional<DisplayExerciseAction> DisplayExercise::emitStep_(
        Step &step, const DisplayExerciseObservation &observed, const TimePoint now) {
        m_await_start = now;
        m_await_deadline = now + secondsSpan(kDisplayExerciseTransitionSeconds);
        m_await_prior = observed.m_committed;
        m_await_revision = observed.m_settings_revision;
        m_await_frame = observed.m_presented_frame;
        m_await_live_revision = observed.m_live_revision;
        m_await_iconify_events = observed.m_iconify_events;
        m_await_history_reset_count = observed.m_history_reset_count;
        m_await_pixel = observed.m_live_pixel;
        m_await_logical = observed.m_logical_extent;
        m_await_confirmed = false;
        m_await_window_change = false;
        if (step.m_kind == EDisplayExerciseActionKind::apply_display or
            step.m_kind == EDisplayExerciseActionKind::invalid_draft) {
            if (const auto settings = displaySettingsFrom(step.m_draft)) {
                m_await_window_change = requestsWindowChange_(*settings, observed.m_committed, m_capabilities);
            }
        } else if (step.m_kind == EDisplayExerciseActionKind::revert_display) {
            // The production trial restore pins the observed pre-trial geometry,
            // so a revert is a native resize exactly when the applied trial was.
            m_await_window_change = m_last_apply_window_change;
        } else if (step.m_kind == EDisplayExerciseActionKind::reset_display) {
            m_await_window_change = requestsWindowChange_(m_baseline_display, observed.m_committed, m_capabilities);
        }

        DisplayExerciseAction action{};
        action.m_kind = step.m_kind;
        action.m_case = step.m_case;
        action.m_image = step.m_image;
        if (step.m_kind == EDisplayExerciseActionKind::quit) {
            m_quit_emitted = true;
        }

        switch (step.m_kind) {
            case EDisplayExerciseActionKind::apply_display:
            case EDisplayExerciseActionKind::invalid_draft: action.m_draft = step.m_draft; break;
            case EDisplayExerciseActionKind::live_picture:
                action.m_world_light = step.m_keep_world_light ? observed.m_live_world_light : step.m_world_light;
                action.m_pixel = step.m_keep_pixel ? observed.m_live_pixel : step.m_pixel;
                action.m_post = step.m_keep_post ? observed.m_live_post : step.m_post;
                break;
            default: break;
        }

        if (DisplayExerciseCase *entry = findCase_(step.m_case); entry != nullptr and step.m_record_as_tested) {
            entry->m_action = std::string{actionName_(step.m_kind)};
            entry->m_requested = describeRequested_(step, observed);
            entry->m_start_seconds = time::seconds(now >= m_begin ? now - m_begin : TimeSpan{});
            if (step.m_draft.m_width != 0u or step.m_draft.m_height != 0u) {
                entry->m_requested_width = step.m_draft.m_width;
                entry->m_requested_height = step.m_draft.m_height;
            }
        }
        writeLog_(
            std::format(
                "emit case={} kind={} revision={} frame={} logical={}x{}", step.m_case, static_cast<u32>(step.m_kind),
                observed.m_settings_revision, observed.m_presented_frame, observed.m_logical_extent.x,
                observed.m_logical_extent.y));
        return action;
    }

    bool DisplayExercise::pollAwaiting_(const DisplayExerciseObservation &observed, const TimePoint now) {
        if (not m_awaiting_step or *m_awaiting_step >= m_steps.size()) {
            return true;
        }
        Step &step = m_steps[*m_awaiting_step];

        // A window-changing acknowledgement is only actionable after a fresh
        // actual-present frame stamped with the acknowledged surface generation.
        // Geometry or  acknowledgement alone does not prove the native surface
        // settled; starting another resize before it does overlaps the trial.
        const auto freshSurfacePresented = [&]() {
            return not m_await_window_change or (observed.m_presented_frame > m_await_frame and
                                                    observed.m_presented_generation != 0u and
                                                    observed.m_presented_generation >= observed.m_surface_generation);
        };

        if (step.m_kind == EDisplayExerciseActionKind::wait_suspension) {
            if (now - m_await_start < secondsSpan(kDisplayExerciseSuspensionSeconds)) {
                return false;
            }
            if (observed.m_presented_frame != m_await_frame) {
                completeStep_(
                    step, EDisplayExerciseStatus::incomplete, "presentation advanced while minimized", observed, now);
            } else if (not observed.m_background_priority) {
                completeStep_(
                    step, EDisplayExerciseStatus::incomplete, "no acknowledged focus-loss background priority",
                    observed, now);
            } else if (observed.m_throttle_active != observed.m_committed.m_throttle_unfocused) {
                completeStep_(
                    step, EDisplayExerciseStatus::failed,
                    "observed throttle reason disagrees with the acknowledged policy", observed, now);
            } else {
                completeStep_(
                    step, EDisplayExerciseStatus::passed,
                    std::format("suspended without fresh frames; throttle_active={}", observed.m_throttle_active),
                    observed, now);
            }
            return true;
        }

        if (now > m_await_deadline) {
            completeStep_(step, EDisplayExerciseStatus::incomplete, "transition deadline exceeded", observed, now);
            m_phase = observed.m_transaction_pending ? EPhase::draining : EPhase::running;
            m_await_start = now;
            return true;
        }

        switch (step.m_kind) {
            case EDisplayExerciseActionKind::apply_display: {
                if (step.m_timeout_after_ack and m_await_confirmed) {
                    if (observed.m_settings_revision > m_timeout_ack_revision and
                        not observed.m_transaction_pending and
                        observed.m_ack_settled) {
                        if (not awaitMatchesPrior_(observed)) {
                            completeStep_(
                                step, EDisplayExerciseStatus::failed,
                                "confirmation timeout did not restore the prior values", observed, now);
                        } else if (not freshSurfacePresented()) {
                            return false;
                        } else if (now - m_ack_time < secondsSpan(kDisplayConfirmSeconds)) {
                            completeStep_(
                                step, EDisplayExerciseStatus::failed,
                                "confirmation reverted before the production timeout", observed, now);
                        } else {
                            completeStep_(
                                step, EDisplayExerciseStatus::passed, "production confirmation timed out and reverted",
                                observed, now);
                        }
                        return true;
                    }
                    return false;
                }
                const bool acked = observed.m_settings_revision > m_await_revision and
                                   not observed.m_transaction_pending and
                                   observed.m_ack_settled;
                if (not acked) {
                    if (not observed.m_transaction_pending and observed.m_action_error) {
                        if (step.m_expect_rejection) {
                            completeStep_(
                                step, EDisplayExerciseStatus::passed, "draft rejected before native mutation", observed,
                                now);
                        } else {
                            completeStep_(
                                step, EDisplayExerciseStatus::failed,
                                withDetails("display transaction rejected", observed.m_action_message), observed, now);
                        }
                        return true;
                    }
                    return false;
                }

                // A matching acknowledgement must publish the requested
                // configuration. A stale/mismatched acknowledgement never
                // completes a case or advances the plan.
                if (not awaitMatchesDraft_(step, observed)) {
                    m_pending_stale_ack = true;
                    writeLog_(
                        std::format(
                            "stale/mismatched ack case={} revision={}", step.m_case, observed.m_settings_revision));
                    return false;
                }
                m_pending_stale_ack = false;
                if (not freshSurfacePresented()) {
                    if (now - m_last_wait_log > secondsSpan(1.0f)) {
                        m_last_wait_log = now;
                        writeLog_(
                            std::format(
                                "waiting fresh surface case={} baseline_frame={} presented_frame={} presented_generation={} surface_generation={}",
                                step.m_case, m_await_frame, observed.m_presented_frame, observed.m_presented_generation,
                                observed.m_surface_generation));
                    }
                    return false;
                }

                if (not step.m_accept_clamp and step.m_draft.m_width != 0u and step.m_draft.m_height != 0u) {
                    const int2 logical = observed.m_logical_extent;
                    if (logical.x <= 0 or logical.y <= 0) {
                        completeStep_(
                            step, EDisplayExerciseStatus::incomplete, "acknowledged but logical geometry unavailable",
                            observed, now);
                        return true;
                    }
                    if (static_cast<u32>(logical.x) != step.m_draft.m_width or
                        static_cast<u32>(logical.y) != step.m_draft.m_height) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            std::format(
                                "acknowledged extent {}x{} does not match requested {}x{}", logical.x, logical.y,
                                step.m_draft.m_width, step.m_draft.m_height),
                            observed, now);
                        return true;
                    }
                }

                if (observed.m_confirm_armed) {
                    m_confirmation_started = true;
                }
                if (step.m_require_trial_armed and not observed.m_confirm_armed) {
                    return false;
                }
                if (step.m_timeout_after_ack) {
                    if (not observed.m_confirm_armed) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            "acknowledgement did not arm the production confirmation trial", observed, now);
                        return true;
                    }
                    m_confirmation_started = true;
                    m_await_confirmed = true;
                    m_timeout_ack_revision = observed.m_settings_revision;
                    m_ack_time = now;
                    m_await_deadline = now + secondsSpan(kDisplayExerciseConfirmRollbackSeconds);
                    // Wait for the production timeout revert.
                    return false;
                }
                completeStep_(step, EDisplayExerciseStatus::passed, "acknowledged", observed, now);
                return true;
            }
            case EDisplayExerciseActionKind::keep_display:
                if (observed.m_action_error) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, withDetails("Keep failed", observed.m_action_message),
                        observed, now);
                    return true;
                }
                if (observed.m_confirm_armed or observed.m_transaction_pending) {
                    return false;
                }
                if (observed.m_presented_frame <= m_await_frame) {
                    return false;
                }
                if (not displaySettingsEqual(observed.m_committed, m_await_prior)) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed,
                        "committed values changed after Keep (late revert/timeout)", observed, now);
                    return true;
                }
                completeStep_(
                    step, EDisplayExerciseStatus::passed, "trial kept; committed values unchanged", observed, now);
                return true;

            case EDisplayExerciseActionKind::revert_display: {
                const bool acked = observed.m_settings_revision > m_await_revision and
                                   not observed.m_transaction_pending and
                                   observed.m_ack_settled;
                if (not acked) {
                    if (not observed.m_transaction_pending and observed.m_action_error) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            withDetails("Revert failed", observed.m_action_message), observed, now);
                        return true;
                    }
                    return false;
                }
                if (not freshSurfacePresented()) {
                    return false;
                }
                if (not displaySettingsEqual(observed.m_committed, m_last_apply_prior)) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, "revert acknowledgement did not restore the prior values",
                        observed, now);
                    return true;
                }
                completeStep_(step, EDisplayExerciseStatus::passed, "reverted to prior values", observed, now);
                return true;
            }
            case EDisplayExerciseActionKind::reset_display: {
                const bool acked = observed.m_settings_revision > m_await_revision and
                                   not observed.m_transaction_pending and
                                   observed.m_ack_settled;
                if (not acked) {
                    if (not observed.m_transaction_pending and observed.m_action_error) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            withDetails("Reset failed", observed.m_action_message), observed, now);
                        return true;
                    }
                    return false;
                }
                if (not freshSurfacePresented()) {
                    return false;
                }
                if (step.m_completes_case and observed.m_confirm_armed) {
                    completeStep_(
                        step, EDisplayExerciseStatus::incomplete,
                        "Reset acknowledgement left an armed confirmation trial", observed, now);
                    return true;
                }
                if (not displaySettingsEqual(observed.m_committed, m_baseline_display)) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, "reset acknowledgement did not restore startup values",
                        observed, now);
                    return true;
                }
                completeStep_(step, EDisplayExerciseStatus::passed, "reset to startup values", observed, now);
                return true;
            }
            case EDisplayExerciseActionKind::live_picture: {
                if (observed.m_presented_frame <= m_await_frame or
                    observed.m_presented_live_revision <= m_await_live_revision) {
                    return false;
                }
                if (step.m_require_unchanged_world and
                    m_baseline_world.m_world_generation != 0u and
                    observed.m_presented_world != m_baseline_world) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, "live edit changed the presented world identity",
                        observed, now);
                    return true;
                }
                if (step.m_require_pixel_grid) {
                    const u32 scale = observed.m_live_pixel.m_pixel_scale;
                    const auto grid = makePresentationGrid(observed.m_framebuffer_extent, scale);
                    if (not grid) {
                        completeStep_(
                            step, EDisplayExerciseStatus::incomplete,
                            withDetails("presentation grid unavailable", grid.error().message()), observed, now);
                        return true;
                    }
                    // Independent exact R=ceil(W/P), S=ceil(R/2), B=2*S from the
                    // actual observed framebuffer, not the requested window.
                    const auto axis = [scale](const i32 extent) -> i32 {
                        const i64 r = (static_cast<i64>(extent) + static_cast<i64>(scale) - 1) / scale;
                        const i64 s = (r + 1) / 2;
                        return static_cast<i32>(2 * s);
                    };
                    const int2 expected_backing{
                        axis(observed.m_framebuffer_extent.x), axis(observed.m_framebuffer_extent.y)};
                    if (grid->m_window.x != observed.m_framebuffer_extent.x or
                        grid->m_window.y != observed.m_framebuffer_extent.y or
                        grid->m_pixel_scale != scale or
                        grid->m_backing.x != expected_backing.x or
                        grid->m_backing.y != expected_backing.y) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            "observed presentation grid does not match exact W/P/R/S/B", observed, now);
                        return true;
                    }
                    if (not observed.m_history_available or not observed.m_history_established) {
                        completeStep_(
                            step, EDisplayExerciseStatus::incomplete, "reconstruction history state unavailable",
                            observed, now);
                        return true;
                    }
                    if (observed.m_history_reset_count <= m_await_history_reset_count) {
                        // A P change must reset history; wait for the accepted
                        // reset stamp rather than inferring it from the grid.
                        return false;
                    }
                    const ReconstructionHistoryStamp &reset = observed.m_history_last_reset;
                    if (reset.m_grid != *grid) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed, "history reset used a different presentation grid",
                            observed, now);
                        return true;
                    }
                    if (reset.m_world != m_baseline_world) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed, "history reset changed the world-lighting identity",
                            observed, now);
                        return true;
                    }
                }
                if (step.m_require_inspection_stamp) {
                    const bool overlay_expected = observed.m_live_world_light.m_diagnostic != ELightingDiagnostic::off;
                    if (overlay_expected and (not observed.m_presented_inspection or
                                                 not sameInspection(
                                                     observed.m_presented_inspection_settings,
                                                     lightingInspectionFrom(observed.m_live_world_light)))) {
                        return false;
                    }
                }
                completeStep_(step, EDisplayExerciseStatus::passed, "fresh presented frame observed", observed, now);
                return true;
            }
            case EDisplayExerciseActionKind::reset_live_picture:
            case EDisplayExerciseActionKind::restore_live_baseline: {
                if (observed.m_presented_frame <= m_await_frame or
                    observed.m_presented_live_revision <= m_await_live_revision) {
                    return false;
                }
                const bool reset = step.m_kind == EDisplayExerciseActionKind::reset_live_picture;
                const WorldLightDisplaySettings expected_light = reset ? WorldLightDisplaySettings{}
                                                                       : m_baseline_world_light;
                const PostDisplaySettings expected_post = reset ? PostDisplaySettings{} : m_baseline_post;
                const PixelDisplaySettings expected_pixel = reset ? m_await_pixel : m_baseline_pixel;
                if (not sameLight(observed.m_live_world_light, expected_light) or
                    not samePost(observed.m_live_post, expected_post) or
                    not samePixel(observed.m_live_pixel, expected_pixel)) {
                    return false;
                }
                completeStep_(
                    step, EDisplayExerciseStatus::passed,
                    reset ? "live values reset through their own action" : "live values restored to startup", observed,
                    now);
                return true;
            }
            case EDisplayExerciseActionKind::invalid_draft: {
                if (observed.m_action_submitted) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, "invalid draft reached native submission", observed, now);
                    return true;
                }
                if (observed.m_action_error) {
                    const bool specific = observed.m_action_error ==
                                              std::make_error_code(std::errc::invalid_argument) and
                                          (observed.m_action_message.find("display.refresh_hz") != std::string::npos or
                                              observed.m_action_message.find("Refresh") != std::string::npos);
                    if (not specific) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed,
                            withDetails("invalid draft was not rejected by the schema rule", observed.m_action_message),
                            observed, now);
                        return true;
                    }
                    if (observed.m_settings_revision != m_await_revision or
                        observed.m_logical_extent.x != m_await_logical.x or
                        observed.m_logical_extent.y != m_await_logical.y) {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed, "invalid draft rejection still mutated native state",
                            observed, now);
                        return true;
                    }
                    completeStep_(
                        step, EDisplayExerciseStatus::passed,
                        "invalid draft rejected by the schema rule before mutation", observed, now);
                    return true;
                }
                return false;
            }
            case EDisplayExerciseActionKind::minimize_window:
                if (observed.m_iconified and observed.m_iconify_events > m_await_iconify_events) {
                    m_minimize_iconify_events = observed.m_iconify_events;
                    completeStep_(step, EDisplayExerciseStatus::passed, "window iconified", observed, now);
                    return true;
                }
                return false;

            case EDisplayExerciseActionKind::restore_window:
                if (step.m_require_minimized and m_minimize_iconify_events == 0u) {
                    completeStep_(
                        step, EDisplayExerciseStatus::incomplete, "restore skipped: no acknowledged minimize event",
                        observed, now);
                    return true;
                }
                if (not observed.m_iconified and observed.m_presented_frame > m_await_frame) {
                    completeStep_(step, EDisplayExerciseStatus::passed, "window restored and presented", observed, now);
                    return true;
                }
                return false;

            case EDisplayExerciseActionKind::capture_image:
                if (not observed.m_capture_complete) {
                    return false;
                }
                if (observed.m_capture_error) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed,
                        withDetails("capture failed", observed.m_capture_error.message()), observed, now);
                    return true;
                }
                if (DisplayExerciseCase *entry = findCase_(step.m_case); entry != nullptr) {
                    entry->m_image = observed.m_capture_path;
                }
                completeStep_(step, EDisplayExerciseStatus::passed, "image captured", observed, now);
                return true;

            case EDisplayExerciseActionKind::save_display:
                if (not observed.m_save_complete) {
                    return false;
                }
                if (observed.m_save_error) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed,
                        withDetails("Save failed", observed.m_save_error.message()), observed, now);
                    return true;
                }
                m_saved_display = observed.m_committed;
                m_saved_world_light = observed.m_live_world_light;
                m_saved_pixel = observed.m_live_pixel;
                m_saved_post = observed.m_live_post;
                completeStep_(step, EDisplayExerciseStatus::passed, "acknowledged values saved", observed, now);
                return true;

            case EDisplayExerciseActionKind::reload_overlay: {
                if (not observed.m_reload_complete) {
                    return false;
                }
                if (observed.m_reload_error) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed,
                        withDetails("overlay reload failed", observed.m_reload_error.message()), observed, now);
                    return true;
                }
                if (not displaySettingsEqual(observed.m_reload_display, m_saved_display)) {
                    if (cliOverrides_("display.pacing") or
                        cliOverrides_("display.window_mode") or
                        cliOverrides_("display.width") or
                        cliOverrides_("display.height")) {
                        completeStep_(
                            step, EDisplayExerciseStatus::incomplete,
                            "explicit CLI display assignments override the saved overlay", observed, now);
                    } else {
                        completeStep_(
                            step, EDisplayExerciseStatus::failed, "reloaded display values differ from saved values",
                            observed, now);
                    }
                    return true;
                }
                if (not sameLight(observed.m_reload_world_light, m_saved_world_light) or
                    not samePixel(observed.m_reload_pixel, m_saved_pixel) or
                    not samePost(observed.m_reload_post, m_saved_post)) {
                    completeStep_(
                        step, EDisplayExerciseStatus::failed, "reloaded live values differ from saved values", observed,
                        now);
                    return true;
                }
                completeStep_(
                    step, EDisplayExerciseStatus::passed, "isolated overlay reloaded through resolver", observed, now);
                return true;
            }
            case EDisplayExerciseActionKind::quit:
                if (observed.m_close_requested) {
                    m_quit_confirmed = true;
                    completeStep_(step, EDisplayExerciseStatus::passed, "production quit observed", observed, now);
                    return true;
                }
                return false;
            default: return false;
        }
    }

    void DisplayExercise::completeStep_(
        Step &step, const EDisplayExerciseStatus status, std::string reason, const DisplayExerciseObservation &observed,
        const TimePoint now) {
        EDisplayExerciseStatus effective = status;
        if (effective == EDisplayExerciseStatus::passed and
            step.m_completes_case and
            observed.m_confirm_armed and
            (step.m_kind == EDisplayExerciseActionKind::apply_display or
                step.m_kind == EDisplayExerciseActionKind::reset_display)) {
            effective = EDisplayExerciseStatus::incomplete;
            reason = "case completed with an armed confirmation trial";
        }
        if (effective == EDisplayExerciseStatus::passed and
            (step.m_kind == EDisplayExerciseActionKind::apply_display or
                step.m_kind == EDisplayExerciseActionKind::reset_display or
                step.m_kind == EDisplayExerciseActionKind::revert_display) and
            m_pending_stale_ack) {
            // Real stale-ACK telemetry: a case completed while the last observed
            // acknowledgement was mismatched. This must remain false.
            m_stale_ack_advanced = true;
        }
        m_pending_stale_ack = false;

        if (DisplayExerciseCase *entry = findCase_(step.m_case); entry != nullptr) {
            entry->m_final_logical = observed.m_logical_extent;
            entry->m_final_framebuffer = observed.m_framebuffer_extent;
            entry->m_end_seconds = time::seconds(now >= m_begin ? now - m_begin : TimeSpan{});
            if (step.m_record_as_tested) {
                entry->m_observed_logical = observed.m_logical_extent;
                entry->m_observed_framebuffer = observed.m_framebuffer_extent;
                entry->m_fullscreen = observed.m_fullscreen;
                entry->m_monitor = observed.m_monitor;
                entry->m_refresh_hz = observed.m_refresh_hz;
                entry->m_ack_revision = observed.m_settings_revision;
                entry->m_presented_frame = observed.m_presented_frame;
                entry->m_trial_armed = entry->m_trial_armed or observed.m_confirm_armed;
                entry->m_trial_seconds_left = observed.m_confirm_seconds_left;
                if (step.m_kind == EDisplayExerciseActionKind::live_picture) {
                    if (const auto grid = makePresentationGrid(
                            observed.m_framebuffer_extent, observed.m_live_pixel.m_pixel_scale)) {
                        entry->m_presentation_grid = *grid;
                        entry->m_has_presentation_grid = true;
                    }
                    entry->m_history_reset = observed.m_history_available and
                                             observed.m_history_reset_count > m_await_history_reset_count;
                    if (entry->m_history_reset) {
                        entry->m_history_grid = observed.m_history_last_reset.m_grid;
                        entry->m_has_history_grid = true;
                        entry->m_history_reset_frame = observed.m_history_last_reset.m_frame;
                        entry->m_history_index = observed.m_history_last_reset.m_index;
                        entry->m_history_phase = observed.m_history_last_reset.m_phase;
                    }
                }
            }
            if (step.m_kind == EDisplayExerciseActionKind::keep_display) {
                entry->m_trial_kept = not observed.m_confirm_armed and not observed.m_transaction_pending;
                entry->m_trial_seconds_left = observed.m_confirm_seconds_left;
            }
            if (effective != EDisplayExerciseStatus::passed or step.m_completes_case) {
                entry->m_status = effective;
                entry->m_reason = std::move(reason);
            }
        }
        if (step.m_kind == EDisplayExerciseActionKind::apply_display) {
            // Revert/timeout steps are later actions: carry the actual
            // pre-apply committed values and native-change fact across the
            // intermediate acknowledgement.
            m_last_apply_prior = m_await_prior;
            m_last_apply_window_change = m_await_window_change;
        }
        writeLog_(
            std::format(
                "step case={} status={} revision={} frame={} presentation_generation={} surface_generation={} logical={}x{} framebuffer={}x{} focused={} iconified={} focus_losses={} iconify_events={}",
                step.m_case, displayExerciseStatusName(effective), observed.m_settings_revision,
                observed.m_presented_frame, observed.m_presented_generation, observed.m_surface_generation,
                observed.m_logical_extent.x, observed.m_logical_extent.y, observed.m_framebuffer_extent.x,
                observed.m_framebuffer_extent.y, observed.m_focused, observed.m_iconified, observed.m_focus_losses,
                observed.m_iconify_events));
        m_phase = EPhase::running;
        m_await_start = now;
        std::ignore = writeReport_();
    }

    bool DisplayExercise::requestsWindowChange_(
        const DisplaySettings &candidate, const DisplaySettings &committed,
        const DisplayCapabilities &capabilities) const {
        if (candidate.m_window_mode != committed.m_window_mode) {
            return true;
        }
        if (candidate.m_window_mode == EDisplayWindowMode::fullscreen) {
            if (candidate.m_refresh_hz != committed.m_refresh_hz) {
                return true;
            }
            if (candidate.m_width != 0u and
                (candidate.m_width != committed.m_width or candidate.m_height != committed.m_height)) {
                return true;
            }
            if (candidate.m_monitor != "auto") {
                for (const DisplayMonitorCapability &monitor: capabilities.m_monitors) {
                    if (candidate.m_monitor == monitor.m_name or candidate.m_monitor == monitor.m_identity) {
                        if (committed.m_monitor != monitor.m_name and committed.m_monitor != monitor.m_identity) {
                            return true;
                        }
                        break;
                    }
                }
            }
            return false;
        }
        // Windowed: zero dimensions mean "keep the current size", so they are
        // not a native resize even though the production coarse comparison
        // treats zero and explicit geometry as different values.
        return candidate.m_width != 0u and
               (candidate.m_width != committed.m_width or candidate.m_height != committed.m_height);
    }

    bool DisplayExercise::awaitMatchesDraft_(const Step &step, const DisplayExerciseObservation &observed) const {
        const DisplaySettings &committed = observed.m_committed;
        const DisplayDraft &draft = step.m_draft;
        if (committed.m_pacing != draft.m_pacing or
            committed.m_cap_hz != draft.m_cap_hz or
            committed.m_paced_vsync != draft.m_paced_vsync or
            committed.m_throttle_unfocused != draft.m_throttle_unfocused) {
            return false;
        }

        if (committed.m_window_mode != draft.m_window_mode) {
            return false;
        }

        if (draft.m_monitor != "auto" and committed.m_monitor != draft.m_monitor) {
            // The production controller may canonicalise the friendly monitor
            // name to its stable OS identity before acknowledging.
            bool via_identity = false;
            for (const DisplayMonitorCapability &monitor: m_capabilities.m_monitors) {
                if (monitor.m_name == draft.m_monitor and
                    not monitor.m_identity.empty() and
                    committed.m_monitor == monitor.m_identity) {
                    via_identity = true;
                    break;
                }
            }
            if (not via_identity) {
                return false;
            }
        }

        if (draft.m_width != 0u and (static_cast<u32>(committed.m_width) != draft.m_width or
                                        static_cast<u32>(committed.m_height) != draft.m_height)) {
            return false;
        }

        if (draft.m_refresh_hz != 0u and committed.m_refresh_hz != draft.m_refresh_hz) {
            return false;
        }
        return true;
    }

    bool DisplayExercise::awaitMatchesPrior_(const DisplayExerciseObservation &observed) const {
        return displaySettingsEqual(observed.m_committed, m_await_prior);
    }

    DisplayExerciseCase *DisplayExercise::findCase_(const std::string_view id) noexcept {
        for (DisplayExerciseCase &entry: m_cases) {
            if (entry.m_id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    void DisplayExercise::markCase_(
        const std::string_view id, const EDisplayExerciseStatus status, std::string reason) {
        if (DisplayExerciseCase *entry = findCase_(id); entry != nullptr) {
            entry->m_status = status;
            entry->m_reason = std::move(reason);
        }
    }

    void DisplayExercise::markUnreached_(const std::string_view reason) {
        for (DisplayExerciseCase &entry: m_cases) {
            if (entry.m_status == EDisplayExerciseStatus::pending and entry.m_id != "quit") {
                entry.m_status = EDisplayExerciseStatus::incomplete;
                entry.m_reason = std::string{reason};
            }
        }
    }

    void DisplayExercise::beginStopping_(std::string_view reason) {
        if (m_quit_emitted or m_actions_suppressed) {
            return;
        }
        m_stop_reason = std::string{reason};
        if (m_status == EDisplayExerciseStatus::pending) {
            m_status = EDisplayExerciseStatus::incomplete;
        }
        if (m_awaiting_step) {
            // Do not abandon a submitted native trial underneath the stop.
            m_phase = EPhase::draining;
        } else {
            m_phase = EPhase::stopping;
        }
        writeLog_(std::format("stopping reason={}", m_stop_reason));
        std::ignore = writeReport_();
    }

    void DisplayExercise::onUserClose_(const DisplayExerciseObservation &observed, const TimePoint now) {
        m_user_close = true;
        m_actions_suppressed = true;
        m_phase = EPhase::stopping;
        m_stop_reason = "user close";
        if (m_status == EDisplayExerciseStatus::pending) {
            m_status = EDisplayExerciseStatus::incomplete;
        }
        markUnreached_("user close before case completion");
        if (DisplayExerciseCase *quit = findCase_("quit");
            quit != nullptr and quit->m_status == EDisplayExerciseStatus::pending) {
            quit->m_status = EDisplayExerciseStatus::incomplete;
            quit->m_reason = "user close interrupted the production quit";
        }
        m_emitted_after_close = 0u;
        writeLog_(
            std::format(
                "user close frame={} transaction_pending={} shutdown_error={}", observed.m_presented_frame,
                observed.m_transaction_pending,
                observed.m_shutdown_error ? observed.m_shutdown_error.message() : "none"));
        std::ignore = writeReport_();
        std::ignore = now;
    }

    void DisplayExercise::computeFinalStatus_() {
        bool has_failed = false;
        bool has_incomplete = false;
        bool has_unsupported = false;
        bool has_pending = false;
        for (const DisplayExerciseCase &entry: m_cases) {
            switch (entry.m_status) {
                case EDisplayExerciseStatus::failed: has_failed = true; break;
                case EDisplayExerciseStatus::incomplete: has_incomplete = true; break;
                case EDisplayExerciseStatus::unsupported: has_unsupported = true; break;
                case EDisplayExerciseStatus::pending: has_pending = true; break;
                case EDisplayExerciseStatus::passed: break;
            }
        }
        if (m_cleanup_error or has_failed) {
            m_status = EDisplayExerciseStatus::failed;
        } else if (has_incomplete or has_pending) {
            m_status = EDisplayExerciseStatus::incomplete;
        } else if (has_unsupported) {
            m_status = EDisplayExerciseStatus::unsupported;
        } else {
            m_status = EDisplayExerciseStatus::passed;
        }
    }

    bool DisplayExercise::cliOverrides_(const std::string_view property) const {
        for (const std::string &entry: m_cli_assignments) {
            if (entry == property) {
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // artifacts
    // ------------------------------------------------------------------

    std::string DisplayExercise::jsonString_(const std::string_view value) {
        std::string out{"\""};
        for (const char character: value) {
            switch (character) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(character) < 0x20u) {
                        out += std::format("\\u{:04x}", static_cast<unsigned char>(character));
                    } else {
                        out += character;
                    }
                    break;
            }
        }
        out += '"';
        return out;
    }

    std::string DisplayExercise::jsonVector_(const int2 &value) {
        return std::format("[{},{}]", value.x, value.y);
    }

    std::string DisplayExercise::jsonGrid_(const PresentationGrid &grid) {
        return std::format(
            "{{\"window\":{},\"resolved\":{},\"raster\":{},\"backing\":{},\"pixel_scale\":{}}}",
            jsonVector_(grid.m_window), jsonVector_(grid.m_resolved), jsonVector_(grid.m_raster),
            jsonVector_(grid.m_backing), grid.m_pixel_scale);
    }

    std::string DisplayExercise::statusText_(const EDisplayExerciseStatus status) {
        return std::string{displayExerciseStatusName(status)};
    }

    std::string DisplayExercise::reportJson_() const {
        std::string json;
        json += std::format(
            "{{\n\"schema_version\":1,\n\"status\":{},\n\"command\":\"exercise-display\",\n",
            jsonString_(statusText_(m_status)));
        json += std::format(
            "\"identity\":{{\"executable\":{},\"bytes\":{},\"build\":{},\"backend\":{},\"gpu\":{},\"source_identity\":{},\"driver\":\"unavailable in RHI DeviceInfo\"}},\n",
            jsonString_(m_executable_path), m_executable_bytes, jsonString_(kBuildKind), jsonString_(m_backend),
            jsonString_(m_gpu), jsonString_(kSourceIdentity));
        json += "\"invocation\":{\"profile\":\"lighting_lab\",\"map_size\":1024,\"seed\":\"0x000000000000002a\",";
        json += std::format("\"output\":{},\"explicit_cli_assignments\":[", jsonString_(m_output.string()));
        for (std::size_t index = 0u; index < m_cli_assignments.size(); ++index) {
            json += std::format("{}{}", index == 0u ? "" : ",", jsonString_(m_cli_assignments[index]));
        }
        json += "]},\n";

        json += std::format(
            "\"capabilities\":{{\"available\":{},\"window_changes\":{},\"exclusive_refresh\":{},\"monitors\":[",
            m_capabilities_available, m_capabilities.m_window_changes, m_capabilities.m_exclusive_refresh);
        for (std::size_t index = 0u; index < m_capabilities.m_monitors.size(); ++index) {
            const DisplayMonitorCapability &monitor = m_capabilities.m_monitors[index];
            json += std::format(
                "{}{{\"name\":{},\"primary\":{},\"current\":{{\"width\":{},\"height\":{},\"refresh_hz\":{}}},\"modes\":[",
                index == 0u ? "" : ",", jsonString_(monitor.m_name), monitor.m_primary, monitor.m_current.m_width,
                monitor.m_current.m_height,
                monitor.m_current.m_refresh_hz ? std::format("{}", *monitor.m_current.m_refresh_hz) : "null");
            for (std::size_t mode_index = 0u; mode_index < monitor.m_modes.size(); ++mode_index) {
                const DisplayModeCapability &mode = monitor.m_modes[mode_index];
                json += std::format(
                    "{}{{\"width\":{},\"height\":{},\"refresh_hz\":{}}}", mode_index == 0u ? "" : ",", mode.m_width,
                    mode.m_height, mode.m_refresh_hz ? std::format("{}", *mode.m_refresh_hz) : "null");
            }
            json += "]}";
        }
        json += "]},\n";

        json += std::format(
            "\"baseline\":{{\"recorded\":{},\"settings_revision\":{},\"presented_frame\":{},\"logical\":{},\"framebuffer\":{},\"presentation_grid\":{{",
            m_baseline_recorded, m_baseline_revision, m_baseline_frame, jsonVector_(m_baseline_logical),
            jsonVector_(m_baseline_framebuffer));
        json += std::format(
            "\"window\":{},\"resolved\":{},\"raster\":{},\"backing\":{},\"pixel_scale\":{}}},\"world\":{{",
            jsonVector_(m_baseline_grid.m_window), jsonVector_(m_baseline_grid.m_resolved),
            jsonVector_(m_baseline_grid.m_raster), jsonVector_(m_baseline_grid.m_backing),
            m_baseline_grid.m_pixel_scale);
        json += std::format(
            "\"job_id\":{},\"generation\":{},\"fingerprint\":\"{:016x}\"}},\"display\":{{", m_baseline_world.m_job_id,
            m_baseline_world.m_world_generation, m_baseline_world.m_content_fingerprint);
        json += std::format(
            "\"pacing\":{},\"cap_hz\":{},\"paced_vsync\":{},\"throttle_unfocused\":{},\"window_mode\":{},\"monitor\":{},\"width\":{},\"height\":{},\"refresh_hz\":{}}}}},\n",
            static_cast<u32>(m_baseline_display.m_pacing), m_baseline_display.m_cap_hz,
            m_baseline_display.m_paced_vsync, m_baseline_display.m_throttle_unfocused,
            static_cast<u32>(m_baseline_display.m_window_mode), jsonString_(m_baseline_display.m_monitor),
            m_baseline_display.m_width, m_baseline_display.m_height, m_baseline_display.m_refresh_hz);

        json += std::format(
            "\"live_baseline\":{{\"sky_hour\":{},\"diagnostic\":{},\"cascade\":{},\"pixel_scale\":{},\"auto_exposure\":{},\"exposure_ev\":{},\"adaptation_tau\":{},\"ev_min\":{},\"ev_max\":{},\"bloom\":{},\"bloom_intensity\":{},\"bloom_threshold\":{}}},\n",
            m_baseline_world_light.m_sky_hour, static_cast<u32>(m_baseline_world_light.m_diagnostic),
            m_baseline_world_light.m_cascade, m_baseline_pixel.m_pixel_scale, m_baseline_post.m_auto_exposure,
            m_baseline_post.m_exposure_ev, m_baseline_post.m_adaptation_tau, m_baseline_post.m_ev_min,
            m_baseline_post.m_ev_max, m_baseline_post.m_bloom, m_baseline_post.m_bloom_intensity,
            m_baseline_post.m_bloom_threshold);
        json += std::format(
            "\"retained_odd_extent\":{{\"width\":1921,\"height\":1081,\"through\":\"6-pacing\",\"reason\":{}}},\n",
            jsonString_(
                "Group 3 leaves the odd 1921x1081 window in place so exposure, bloom, sky, diagnostic, cascade and pacing cases exercise a real odd framebuffer; group 7 restores 1280x720."));

        json += "\"cases\":[";
        for (std::size_t index = 0u; index < m_cases.size(); ++index) {
            const DisplayExerciseCase &entry = m_cases[index];
            json += std::format(
                "{}{{\"id\":{},\"group\":{},\"description\":{},\"status\":{},\"reason\":{},\"action\":{},\"requested\":{},\"requested_logical\":[{},{}],\"observed_logical\":{},\"observed_framebuffer\":{},\"final_logical\":{},\"final_framebuffer\":{},\"fullscreen\":{},\"monitor\":{},\"refresh_hz\":{},\"ack_revision\":{},\"presented_frame\":{},\"timestamps\":{{\"start_seconds\":{:.3f},\"end_seconds\":{:.3f}}},\"presentation_grid\":{},\"history\":{{\"reset\":{},\"frame\":{},\"index\":{},\"phase\":{},\"grid\":{}}},\"trial\":{{\"armed\":{},\"kept\":{},\"seconds_left\":{:.2f}}},\"image\":{}}}",
                index == 0u ? "" : ",", jsonString_(entry.m_id), jsonString_(entry.m_group),
                jsonString_(entry.m_description), jsonString_(statusText_(entry.m_status)), jsonString_(entry.m_reason),
                jsonString_(entry.m_action), jsonString_(entry.m_requested), entry.m_requested_width,
                entry.m_requested_height, jsonVector_(entry.m_observed_logical),
                jsonVector_(entry.m_observed_framebuffer), jsonVector_(entry.m_final_logical),
                jsonVector_(entry.m_final_framebuffer), entry.m_fullscreen, jsonString_(entry.m_monitor),
                entry.m_refresh_hz ? std::format("{}", *entry.m_refresh_hz) : "null", entry.m_ack_revision,
                entry.m_presented_frame, entry.m_start_seconds, entry.m_end_seconds,
                entry.m_has_presentation_grid ? jsonGrid_(entry.m_presentation_grid) : "null", entry.m_history_reset,
                entry.m_history_reset_frame, entry.m_history_index, entry.m_history_phase,
                entry.m_has_history_grid ? jsonGrid_(entry.m_history_grid) : "null", entry.m_trial_armed,
                entry.m_trial_kept, entry.m_trial_seconds_left, jsonString_(entry.m_image));
        }
        json += "],\n";

        json += std::format(
            "\"cleanup\":{{\"error\":{},\"quit_confirmed\":{},\"user_close\":{}}},\n",
            jsonString_(m_cleanup_error ? m_cleanup_error.message() : std::string{"none"}), m_quit_confirmed,
            m_user_close);
        json += std::format("\"logs\":[{}],\n", jsonString_(kLogFileName));
        json += std::format(
            "\"run_deadline_seconds\":{},\"stop_reason\":{},\n", kDisplayExerciseRunSeconds,
            jsonString_(m_stop_reason));
        json += "\"outcome\":{\"stale_ack_advanced\":";
        json += m_stale_ack_advanced ? "true" : "false";
        json += ",\"emitted_actions\":";
        json += std::format(
            "{},\"emitted_actions_after_close\":{},\"elapsed_seconds\":{:.3f}}}", m_emitted_actions,
            m_emitted_after_close, time::seconds(m_last_now >= m_begin ? m_last_now - m_begin : TimeSpan{}));
        json += "\n}\n";
        return json;
    }

    std::error_code DisplayExercise::writeReport_() {
        if (m_report_path.empty()) {
            return {};
        }
        return writeFileAtomically(m_report_path, reportJson_());
    }

    void DisplayExercise::writeLog_(const std::string_view line) {
        if (m_log_path.empty()) {
            return;
        }
        const auto elapsed = m_last_now >= m_begin ? m_last_now - m_begin : TimeSpan{};
        try {
            std::ofstream log{m_log_path, std::ios::binary | std::ios::app};
            if (log) {
                log << std::format("[{:+.3f}] {}\n", time::seconds(elapsed), line);
            }
        } catch (...) {
            // Diagnostics are best effort; the atomic JSON report remains the
            // authoritative evidence and is rewritten on the next state change.
        }
    }
} // namespace pP::game
