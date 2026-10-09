module;

#include "pP/UnitTest.h"
#include "Game.TestAllocationFailure.h"

module game.tests;

import engine.app;
import engine.core;
import game.config;
import game.display;

import std;

namespace pP::game::tests::detail::display {
    using namespace pP::config;

    [[nodiscard]] Resolved<DisplaySettings> committedDisplay() {
        Resolved<DisplaySettings> resolved{};
        resolved.m_value = DisplaySettings{};
        resolved.m_origins.push_back(PropertyOrigin{"display.pacing", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.cap_hz", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.paced_vsync", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.throttle_unfocused", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.window_mode", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.monitor", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.width", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.height", ESource::none, "default"});
        resolved.m_origins.push_back(PropertyOrigin{"display.refresh_hz", ESource::none, "default"});
        resolved.m_revision = 7u;
        return resolved;
    }

    [[nodiscard]] DisplayCapabilities switchingCapabilities() {
        DisplayCapabilities capabilities{};
        capabilities.m_window_changes = true;
        capabilities.m_exclusive_refresh = true;
        DisplayMonitorCapability monitor{};
        monitor.m_name = "Test Panel";
        monitor.m_primary = true;
        monitor.m_current = DisplayModeCapability{1920u, 1080u, std::optional<u32>{60u}};
        monitor.m_modes.push_back(DisplayModeCapability{1920u, 1080u, std::optional<u32>{60u}});
        monitor.m_modes.push_back(DisplayModeCapability{1920u, 1080u, std::optional<u32>{144u}});
        monitor.m_modes.push_back(DisplayModeCapability{1280u, 720u, std::optional<u32>{60u}});
        capabilities.m_monitors.push_back(std::move(monitor));
        return capabilities;
    }

    [[nodiscard]] DisplayDraft fullscreenDraft() {
        DisplayDraft draft{};
        draft.m_window_mode = EDisplayWindowMode::fullscreen;
        draft.m_monitor = "Test Panel";
        draft.m_width = 1920u;
        draft.m_height = 1080u;
        draft.m_refresh_hz = 144u;
        return draft;
    }

    PPR_UNIT_TEST (draft_round_trip_matches_committed) {
        const DisplaySettings committed{};
        const auto prepared = displayDraftFrom(committed);
        PPR_TEST_ASSERT(prepared.has_value());
        const DisplayDraft draft = *prepared;
        PPR_TEST_ASSERT(displayDraftMatches(draft, committed));
        const auto settings = displaySettingsFrom(draft);
        PPR_TEST_ASSERT(settings.has_value());
        PPR_TEST_ASSERT(displaySettingsEqual(*settings, committed));

        DisplayDraft edited = draft;
        edited.m_pacing = EFramePacing::paced;
        edited.m_cap_hz = 120u;
        PPR_TEST_ASSERT(not displayDraftMatches(edited, committed));
    };

    PPR_UNIT_TEST (monitor_copy_failure_never_substitutes_defaults) {
        DisplaySettings committed{};
        committed.m_monitor = std::string(512u, 'm');
        DisplayDraft draft{};
        draft.m_monitor = committed.m_monitor;
        {
            const AllocationFailure fault{};
            const auto result = displayDraftFrom(committed);
            PPR_TEST_ASSERT(not result);
            PPR_TEST_ASSERT(result.error() == std::errc::not_enough_memory);
        }
        {
            const AllocationFailure fault{};
            const auto result = displaySettingsFrom(draft);
            PPR_TEST_ASSERT(not result);
            PPR_TEST_ASSERT(result.error() == std::errc::not_enough_memory);
        }
        {
            const AllocationFailure fault{};
            PPR_TEST_ASSERT(displayDraftMatches(draft, committed));
        }
        PPR_TEST_ASSERT(committed.m_monitor == draft.m_monitor);
    };

    PPR_UNIT_TEST (confirm_preparation_failure_preserves_complete_snapshot) {
        DisplaySettings prior{};
        prior.m_monitor = std::string(512u, 'p');
        std::optional<WindowDisplayGeometry> geometry{std::in_place};
        geometry->m_monitor = std::string(1024u, 'g');
        geometry->m_size = int2{1280, 720};
        geometry->m_fullscreen = true;
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, DisplaySettings{}));
        const DisplaySettings original = confirm.m_prior;
        const float seconds = confirm.m_seconds_left;
        for (const int fail_after: {0, 1}) {
            const AllocationFailure fault{fail_after};
            const auto error = armDisplayConfirm(confirm, prior, geometry);
            PPR_TEST_ASSERT(error == std::errc::not_enough_memory);
            PPR_TEST_ASSERT(displaySettingsEqual(confirm.m_prior, original));
            PPR_TEST_ASSERT(not confirm.m_has_prior_geometry);
            PPR_TEST_ASSERT(confirm.m_armed);
            PPR_TEST_ASSERT(confirm.m_seconds_left == seconds);
        }
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, prior, geometry));
        for (const int fail_after: {0, 1}) {
            const AllocationFailure fault{fail_after};
            const auto revert = displayRevertSettings(confirm);
            PPR_TEST_ASSERT(not revert);
            PPR_TEST_ASSERT(revert.error() == std::errc::not_enough_memory);
            PPR_TEST_ASSERT(confirm.m_prior.m_monitor == prior.m_monitor);
            PPR_TEST_ASSERT(confirm.m_prior_geometry.m_monitor == geometry->m_monitor);
            PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        }
    };

    PPR_UNIT_TEST (acknowledgement_moves_owned_snapshots_without_allocation) {
        DisplaySettings prior{};
        prior.m_monitor = std::string(512u, 'p');
        std::optional<WindowDisplayGeometry> geometry{std::in_place};
        geometry->m_monitor = std::string(512u, 'g');
        DisplayConfirm confirm{};
        const AllocationFailure fault{};
        armDisplayConfirmOwned(confirm, std::move(prior), std::move(geometry));
        PPR_TEST_ASSERT(confirm.m_armed);
        PPR_TEST_ASSERT(confirm.m_has_prior_geometry);
        PPR_TEST_ASSERT(confirm.m_prior.m_monitor.size() == 512u);
        PPR_TEST_ASSERT(confirm.m_prior_geometry.m_monitor.size() == 512u);
    };

    PPR_UNIT_TEST (runtime_layer_preserves_long_monitor_name) {
        DisplayDraft draft{};
        draft.m_monitor = std::string(512u, 'm');

        const auto layer = displayRuntimeLayer(draft, "test");
        Diagnostics diagnostics{};
        const auto applied = applyDisplayRuntime(committedDisplay(), layer, diagnostics);
        PPR_TEST_ASSERT(applied.has_value());
        PPR_TEST_ASSERT(applied->m_value.m_monitor == draft.m_monitor);
        PPR_TEST_ASSERT(applied->m_value.m_monitor != "auto");
        PPR_TEST_ASSERT(layer.m_assignments.size() == 9u);
    };

    PPR_UNIT_TEST (switching_round_trip_matches_committed) {
        DisplaySettings committed{};
        committed.m_window_mode = EDisplayWindowMode::fullscreen;
        committed.m_monitor = "Test Panel";
        committed.m_width = 1920u;
        committed.m_height = 1080u;
        committed.m_refresh_hz = 144u;
        const auto prepared = displayDraftFrom(committed);
        PPR_TEST_ASSERT(prepared.has_value());
        const DisplayDraft draft = *prepared;
        PPR_TEST_ASSERT(displayDraftMatches(draft, committed));
        const auto settings = displaySettingsFrom(draft);
        PPR_TEST_ASSERT(settings.has_value());
        PPR_TEST_ASSERT(displaySettingsEqual(*settings, committed));

        DisplayDraft edited = draft;
        edited.m_monitor = "Other Panel";
        PPR_TEST_ASSERT(not displayDraftMatches(edited, committed));
        edited = draft;
        edited.m_refresh_hz = 60u;
        PPR_TEST_ASSERT(not displayDraftMatches(edited, committed));
        edited = draft;
        edited.m_window_mode = EDisplayWindowMode::windowed;
        PPR_TEST_ASSERT(not displayDraftMatches(edited, committed));
    };

    PPR_UNIT_TEST (fullscreen_draft_validates_against_capabilities) {
        const DisplayCapabilities capabilities = switchingCapabilities();
        Diagnostics diagnostics{};
        auto candidate = validateDisplayDraft(committedDisplay(), fullscreenDraft(), capabilities, diagnostics);
        PPR_TEST_ASSERT(candidate.has_value());
        PPR_TEST_ASSERT(candidate->m_value.m_window_mode == EDisplayWindowMode::fullscreen);
        PPR_TEST_ASSERT(candidate->m_value.m_monitor == "Test Panel");
        PPR_TEST_ASSERT(candidate->m_value.m_width == 1920u);
        PPR_TEST_ASSERT(candidate->m_value.m_height == 1080u);
        PPR_TEST_ASSERT(candidate->m_value.m_refresh_hz == 144u);
        const PropertyOrigin *origin = candidate->find("display.window_mode");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::runtime);
    };

    PPR_UNIT_TEST (windowed_draft_passes_without_window_support) {
        const DisplayCapabilities capabilities{};
        Diagnostics diagnostics{};
        auto candidate = validateDisplayDraft(committedDisplay(), DisplayDraft{}, capabilities, diagnostics);
        PPR_TEST_ASSERT(candidate.has_value());
        PPR_TEST_ASSERT(candidate->m_value.m_window_mode == EDisplayWindowMode::windowed);
    };

    PPR_UNIT_TEST (fullscreen_rejected_without_window_support) {
        const DisplayCapabilities capabilities{};
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), fullscreenDraft(), capabilities, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::operation_not_supported));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unknown_monitor_rejected) {
        DisplayDraft draft = fullscreenDraft();
        draft.m_monitor = "Ghost Panel";
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, switchingCapabilities(), diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::no_such_device));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (numeric_monitor_rejected_as_handle) {
        DisplayDraft draft = fullscreenDraft();
        draft.m_monitor = "0";
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, switchingCapabilities(), diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(diagnostics.hasErrors());

        Diagnostics schema_diagnostics{};
        const auto schema_rejected = validateDisplayDraft(committedDisplay(), draft, schema_diagnostics);
        PPR_TEST_ASSERT(not schema_rejected.has_value());
        PPR_TEST_ASSERT(schema_diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unknown_window_mode_rejected) {
        DisplayDraft draft{};
        draft.m_window_mode = static_cast<EDisplayWindowMode>(9u);
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (windowed_refresh_rejected) {
        DisplayDraft draft{};
        draft.m_refresh_hz = 60u;
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unpaired_dimensions_rejected) {
        DisplayDraft draft{};
        draft.m_width = 1920u;
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unlisted_resolution_rejected) {
        DisplayDraft draft = fullscreenDraft();
        draft.m_width = 800u;
        draft.m_height = 600u;
        draft.m_refresh_hz = 0u;
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, switchingCapabilities(), diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unlisted_refresh_rejected) {
        DisplayDraft draft = fullscreenDraft();
        draft.m_refresh_hz = 75u;
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), draft, switchingCapabilities(), diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (explicit_refresh_rejected_when_unsupported) {
        DisplayCapabilities capabilities = switchingCapabilities();
        capabilities.m_exclusive_refresh = false;
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(committedDisplay(), fullscreenDraft(), capabilities, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::operation_not_supported));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (switching_availability_gates_on_capabilities) {
        const DisplaySwitchingAvailability missing = displaySwitchingAvailability(nullptr);
        PPR_TEST_ASSERT(not missing.m_supported);
        PPR_TEST_ASSERT(missing.m_reason[0] != '\0');

        const DisplayCapabilities unsupported{};
        const DisplaySwitchingAvailability gated = displaySwitchingAvailability(&unsupported);
        PPR_TEST_ASSERT(not gated.m_supported);
        PPR_TEST_ASSERT(gated.m_reason[0] != '\0');

        const DisplayCapabilities supported = switchingCapabilities();
        const DisplaySwitchingAvailability open = displaySwitchingAvailability(&supported);
        PPR_TEST_ASSERT(open.m_supported);
    };

    PPR_UNIT_TEST (overlay_merge_carries_switching_fields) {
        config::Document existing{};

        DisplaySettings committed{};
        committed.m_window_mode = EDisplayWindowMode::fullscreen;
        committed.m_monitor = "Test Panel";
        committed.m_width = 1280u;
        committed.m_height = 720u;
        committed.m_refresh_hz = 60u;

        auto merged = buildDisplayOverlayDocument(existing, committed);
        PPR_TEST_ASSERT(merged.has_value());

        auto bytes = encodeDisplayOverlay(*merged);
        PPR_TEST_ASSERT(bytes.has_value());
        Diagnostics round_diagnostics{};
        auto round = decode(*bytes, "round", round_diagnostics);
        PPR_TEST_ASSERT(round.has_value());

        bool mode_fresh = false;
        bool monitor_fresh = false;
        bool size_fresh = false;
        bool refresh_fresh = false;
        for (const auto &entry: round->m_entries) {
            if (entry.m_key == "display.window_mode" and entry.m_value.m_kind == EFileKind::text) {
                mode_fresh = entry.m_value.m_text == "fullscreen";
            }
            if (entry.m_key == "display.monitor" and entry.m_value.m_kind == EFileKind::text) {
                monitor_fresh = entry.m_value.m_text == "Test Panel";
            }
            if (entry.m_key == "display.width" and entry.m_value.m_kind == EFileKind::integer) {
                size_fresh = entry.m_value.m_integer == 1280;
            }
            if (entry.m_key == "display.refresh_hz" and entry.m_value.m_kind == EFileKind::integer) {
                refresh_fresh = entry.m_value.m_integer == 60;
            }
        }
        PPR_TEST_ASSERT(mode_fresh);
        PPR_TEST_ASSERT(monitor_fresh);
        PPR_TEST_ASSERT(size_fresh);
        PPR_TEST_ASSERT(refresh_fresh);

        for (const auto &entry: round->m_entries) {
            if (entry.m_key.starts_with("display.")) {
                bool persistable = false;
                DisplaySettings probe{};
                visit(probe, [&](const auto &member, const auto &) {
                    if (member.m_metadata.m_name == entry.m_key and member.m_metadata.m_persistable) {
                        persistable = true;
                    }
                });
                PPR_TEST_ASSERT(persistable);
            }
        }
    };

    PPR_UNIT_TEST (window_change_drives_trial_decision) {
        const DisplaySettings before{};
        PPR_TEST_ASSERT(not displayWindowChange(before, before));

        DisplaySettings paced = before;
        paced.m_pacing = EFramePacing::paced;
        PPR_TEST_ASSERT(not displayWindowChange(paced, before));

        DisplaySettings mode = before;
        mode.m_window_mode = EDisplayWindowMode::fullscreen;
        PPR_TEST_ASSERT(displayWindowChange(mode, before));

        DisplaySettings monitor = before;
        monitor.m_monitor = "Test Panel";
        PPR_TEST_ASSERT(displayWindowChange(monitor, before));

        DisplaySettings size = before;
        size.m_width = 1280u;
        size.m_height = 720u;
        PPR_TEST_ASSERT(displayWindowChange(size, before));

        DisplaySettings refresh = before;
        refresh.m_refresh_hz = 60u;
        PPR_TEST_ASSERT(displayWindowChange(refresh, before));
    };

    PPR_UNIT_TEST (valid_draft_validates_with_runtime_provenance) {
        DisplayDraft draft{};
        draft.m_pacing = EFramePacing::paced;
        draft.m_cap_hz = 120u;
        draft.m_paced_vsync = false;

        Diagnostics diagnostics{};
        auto candidate = validateDisplayDraft(committedDisplay(), draft, diagnostics);
        PPR_TEST_ASSERT(candidate.has_value());
        PPR_TEST_ASSERT(candidate->m_value.m_pacing == EFramePacing::paced);
        PPR_TEST_ASSERT(candidate->m_value.m_cap_hz == 120u);
        PPR_TEST_ASSERT(not candidate->m_value.m_paced_vsync);
        PPR_TEST_ASSERT(candidate->m_value.m_throttle_unfocused);
        const PropertyOrigin *origin = candidate->find("display.cap_hz");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::runtime);
    };

    PPR_UNIT_TEST (cap_out_of_range_rejected_with_diagnostics) {
        DisplayDraft draft{};
        draft.m_pacing = EFramePacing::paced;
        draft.m_cap_hz = 0u;

        const Resolved<DisplaySettings> before = committedDisplay();
        Diagnostics diagnostics{};
        const auto rejected = validateDisplayDraft(before, draft, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == make_error_code(EConfigErrc::out_of_range));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (unknown_pacing_name_rejected) {
        Layer layer{};
        layer.m_source = ESource::runtime;
        layer.m_origin = "display-panel";
        layer.m_assignments.push_back(
            Assignment{"display.pacing", "panel", SourceValue::text("hyper"), "display-panel", 0u});

        Diagnostics diagnostics{};
        const auto rejected = applyDisplayRuntime(committedDisplay(), layer, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(rejected.error() == make_error_code(EConfigErrc::unknown_enum_value));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (runtime_layer_cannot_touch_startup_properties) {
        // Guard: every DisplaySettings member the panel writes must accept a
        // runtime layer, or Apply could never submit it.
        DisplaySettings probe{};
        bool all_runtime = true;
        visit(probe, [&](const auto &member, const auto &) {
            if (member.m_metadata.m_mutability != EMutability::runtime) {
                all_runtime = false;
            }
        });
        PPR_TEST_ASSERT(all_runtime);
    };

    PPR_UNIT_TEST (overlay_merge_replaces_display_preserves_unrelated) {
        const std::string existing_text{"[audio]\nvolume = 7\n\n[display]\npacing = \"uncapped\"\ncap_hz = 30\n"};
        Diagnostics decode_diagnostics{};
        auto existing = decode(existing_text, "existing", decode_diagnostics);
        PPR_TEST_ASSERT(existing.has_value());

        DisplaySettings committed{};
        committed.m_pacing = EFramePacing::paced;
        committed.m_cap_hz = 144u;
        committed.m_paced_vsync = false;
        committed.m_throttle_unfocused = true;

        auto merged = buildDisplayOverlayDocument(*existing, committed);
        PPR_TEST_ASSERT(merged.has_value());

        auto bytes = encodeDisplayOverlay(*merged);
        PPR_TEST_ASSERT(bytes.has_value());
        Diagnostics round_diagnostics{};
        auto round = decode(*bytes, "round", round_diagnostics);
        PPR_TEST_ASSERT(round.has_value());

        bool audio_kept = false;
        bool stale_gone = false;
        bool pacing_fresh = false;
        bool cap_fresh = false;
        for (const auto &entry: round->m_entries) {
            if (entry.m_key == "audio.volume" and entry.m_value.m_kind == EFileKind::integer) {
                audio_kept = entry.m_value.m_integer == 7;
            }
            if (entry.m_key == "display.pacing" and entry.m_value.m_kind == EFileKind::text) {
                pacing_fresh = entry.m_value.m_text == "paced";
                stale_gone = entry.m_value.m_text != "uncapped";
            }
            if (entry.m_key == "display.cap_hz" and entry.m_value.m_kind == EFileKind::integer) {
                cap_fresh = entry.m_value.m_integer == 144;
            }
        }
        PPR_TEST_ASSERT(audio_kept);
        PPR_TEST_ASSERT(stale_gone);
        PPR_TEST_ASSERT(pacing_fresh);
        PPR_TEST_ASSERT(cap_fresh);

        for (const auto &entry: round->m_entries) {
            if (entry.m_key.starts_with("display.")) {
                bool persistable = false;
                DisplaySettings probe{};
                visit(probe, [&](const auto &member, const auto &) {
                    if (member.m_metadata.m_name == entry.m_key and member.m_metadata.m_persistable) {
                        persistable = true;
                    }
                });
                PPR_TEST_ASSERT(persistable);
            }
        }
    };

    PPR_UNIT_TEST (overlay_header_documents_override_order) {
        config::Document empty{};
        auto bytes = encodeDisplayOverlay(empty);
        PPR_TEST_ASSERT(bytes.has_value());
        PPR_TEST_ASSERT(bytes->find("command line") != std::string::npos);
        PPR_TEST_ASSERT(bytes->find("environment") != std::string::npos);
    };

    PPR_UNIT_TEST (missing_overlay_is_absence_not_error) {
        const std::filesystem::path missing = std::filesystem::temp_directory_path() /
                                              "ppr-display-overlay-missing.toml";
        std::error_code remove_error{};
        std::filesystem::remove(missing, remove_error);

        Diagnostics diagnostics{};
        const auto absent = loadOverlayLayer(missing, diagnostics);
        PPR_TEST_ASSERT(absent.has_value());
        PPR_TEST_ASSERT(not absent->has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (atomic_write_round_trips_and_clears_temp) {
        static std::atomic<u64> counter{0u};
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ppr-display-overlay-test";
        std::error_code dir_error{};
        std::filesystem::create_directories(dir, dir_error);
        PPR_TEST_ASSERT(not dir_error);
        const std::filesystem::path path = dir / ("overlay-" + std::to_string(counter.fetch_add(1u)) + ".toml");

        const std::string payload{"[display]\npacing = \"paced\"\n"};
        const std::error_code written = writeFileAtomically(path, payload);
        PPR_TEST_ASSERT(not written);

        std::ifstream file{path, std::ios::binary};
        PPR_TEST_ASSERT(static_cast<bool>(file));
        const std::string back{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        PPR_TEST_ASSERT(back == payload);

        std::filesystem::path temp = path;
        temp += ".tmp";
        std::error_code probe{};
        PPR_TEST_ASSERT(not std::filesystem::exists(temp, probe));

        std::error_code remove_error{};
        std::filesystem::remove(path, remove_error);
    };

    PPR_UNIT_TEST (confirm_counts_down_and_expires_once) {
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not tickDisplayConfirm(confirm, 1.0f));

        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, DisplaySettings{}));
        PPR_TEST_ASSERT(confirm.m_armed);
        PPR_TEST_ASSERT(not tickDisplayConfirm(confirm, kDisplayConfirmSeconds - 1.0f));
        PPR_TEST_ASSERT(confirm.m_armed);
        PPR_TEST_ASSERT(tickDisplayConfirm(confirm, 1.0f));

        disarmDisplayConfirm(confirm);
        PPR_TEST_ASSERT(not confirm.m_armed);
        PPR_TEST_ASSERT(not tickDisplayConfirm(confirm, 1000.0f));
    };

    PPR_UNIT_TEST (confirm_holds_while_revert_pends) {
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, DisplaySettings{}));
        confirm.m_revert_pending = true;
        PPR_TEST_ASSERT(not tickDisplayConfirm(confirm, kDisplayConfirmSeconds * 2.0f));
        PPR_TEST_ASSERT(confirm.m_armed);
    };

    PPR_UNIT_TEST (revert_restores_observed_geometry_not_requested_zeros) {
        DisplayConfirm confirm{};
        const DisplaySettings prior{};
        WindowDisplayGeometry geometry{};
        geometry.m_position.x = 100;
        geometry.m_position.y = 200;
        geometry.m_size.x = 1280;
        geometry.m_size.y = 720;
        geometry.m_framebuffer_size.x = 1280;
        geometry.m_framebuffer_size.y = 720;
        geometry.m_fullscreen = false;
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, prior, std::optional<WindowDisplayGeometry>{geometry}));
        PPR_TEST_ASSERT(confirm.m_has_prior_geometry);

        const auto prepared = displayRevertSettings(confirm);
        PPR_TEST_ASSERT(prepared.has_value());
        const DisplaySettings &revert = *prepared;
        PPR_TEST_ASSERT(revert.m_width == 1280u);
        PPR_TEST_ASSERT(revert.m_height == 720u);
        PPR_TEST_ASSERT(revert.m_window_mode == EDisplayWindowMode::windowed);
        PPR_TEST_ASSERT(revert.m_refresh_hz == 0u);
        PPR_TEST_ASSERT(revert.m_monitor == "auto");
    };

    PPR_UNIT_TEST (revert_restores_exclusive_monitor_and_refresh) {
        DisplaySettings prior{};
        prior.m_window_mode = EDisplayWindowMode::fullscreen;
        prior.m_monitor = "Test Panel";
        prior.m_width = 1920u;
        prior.m_height = 1080u;
        prior.m_refresh_hz = 144u;

        WindowDisplayGeometry geometry{};
        geometry.m_size.x = 1920;
        geometry.m_size.y = 1080;
        geometry.m_framebuffer_size.x = 1920;
        geometry.m_framebuffer_size.y = 1080;
        geometry.m_refresh_hz = std::optional<u32>{144u};
        geometry.m_fullscreen = true;
        geometry.m_monitor = "Test Panel";

        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, prior, std::optional<WindowDisplayGeometry>{geometry}));
        const auto prepared = displayRevertSettings(confirm);
        PPR_TEST_ASSERT(prepared.has_value());
        const DisplaySettings &revert = *prepared;
        PPR_TEST_ASSERT(displaySettingsEqual(revert, prior));

        DisplayConfirm unknown_refresh{};
        WindowDisplayGeometry partial = geometry;
        partial.m_refresh_hz.reset();
        PPR_TEST_ASSERT(not armDisplayConfirm(unknown_refresh, prior, std::optional<WindowDisplayGeometry>{partial}));
        const auto unknown_revert = displayRevertSettings(unknown_refresh);
        PPR_TEST_ASSERT(unknown_revert.has_value());
        PPR_TEST_ASSERT(unknown_revert->m_refresh_hz == 144u);
    };

    PPR_UNIT_TEST (revert_without_geometry_keeps_requested_policy) {
        DisplayConfirm confirm{};
        DisplaySettings prior{};
        prior.m_width = 800u;
        prior.m_height = 600u;
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, prior));
        PPR_TEST_ASSERT(not confirm.m_has_prior_geometry);
        const auto revert = displayRevertSettings(confirm);
        PPR_TEST_ASSERT(revert.has_value());
        PPR_TEST_ASSERT(displaySettingsEqual(*revert, prior));
    };

    PPR_UNIT_TEST (revert_retry_survives_failed_revert_timeout) {
        DisplayConfirm confirm{};
        WindowDisplayGeometry geometry{};
        geometry.m_size.x = 1280;
        geometry.m_size.y = 720;
        geometry.m_fullscreen = false;
        PPR_TEST_ASSERT(
            not armDisplayConfirm(confirm, DisplaySettings{}, std::optional<WindowDisplayGeometry>{geometry}));
        const auto before = displayRevertSettings(confirm);
        PPR_TEST_ASSERT(before.has_value());
        PPR_TEST_ASSERT(before->m_width == 1280u);

        // A failed revert stops the countdown but the explicit retry
        // must still restore observed state, never requested zeros.
        PPR_TEST_ASSERT(tickDisplayConfirmPoll(confirm, kDisplayConfirmSeconds, false));
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        const auto retry = displayRevertSettings(confirm);
        PPR_TEST_ASSERT(retry.has_value());
        PPR_TEST_ASSERT(displaySettingsEqual(*retry, *before));
        PPR_TEST_ASSERT(retry->m_width == 1280u);
        PPR_TEST_ASSERT(retry->m_height == 720u);
    };

    PPR_UNIT_TEST (trial_blocks_apply_until_resolved) {
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not displayTrialUnresolved(confirm));
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, DisplaySettings{}));
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        confirm.m_revert_pending = true;
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        confirm.m_revert_pending = false;
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        disarmDisplayConfirm(confirm);
        PPR_TEST_ASSERT(not displayTrialUnresolved(confirm));
    };

    PPR_UNIT_TEST (timeout_failed_revert_blocks_apply_and_reset_without_retry_storm) {
        const Resolved<DisplaySettings> baseline = committedDisplay();
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, baseline.m_value));
        PPR_TEST_ASSERT(tickDisplayConfirmPoll(confirm, kDisplayConfirmSeconds, false));

        // Exercise a validation failure after expiry, as the Revert intent does.
        DisplayDraft unavailable = fullscreenDraft();
        Diagnostics diagnostics{};
        const auto revert = validateDisplayDraft(baseline, unavailable, DisplayCapabilities{}, diagnostics);
        PPR_TEST_ASSERT(not revert);
        PPR_TEST_ASSERT(not confirm.m_revert_pending);
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));

        DisplaySettings watched_prior = confirm.m_prior;
        int submissions = 0;
        const auto attempt = [&](const Resolved<DisplaySettings> &candidate) {
            if (displayTrialUnresolved(confirm)) {
                return std::make_error_code(std::errc::device_or_resource_busy);
            }
            watched_prior = candidate.m_value;
            ++submissions;
            return std::error_code{};
        };
        Resolved<DisplaySettings> apply = baseline;
        apply.m_value.m_cap_hz = 120u;
        PPR_TEST_ASSERT(attempt(apply) == std::make_error_code(std::errc::device_or_resource_busy));
        PPR_TEST_ASSERT(
            attempt(displayResetCandidate(baseline)) == std::make_error_code(std::errc::device_or_resource_busy));
        PPR_TEST_ASSERT(submissions == 0);
        PPR_TEST_ASSERT(displaySettingsEqual(watched_prior, baseline.m_value));
        PPR_TEST_ASSERT(not tickDisplayConfirmPoll(confirm, kDisplayConfirmSeconds * 2.0f, false));
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));

        // An explicit retry remains possible; successful Revert or Keep resolves.
        confirm.m_revert_pending = true;
        PPR_TEST_ASSERT(displayTrialUnresolved(confirm));
        disarmDisplayConfirm(confirm);
        PPR_TEST_ASSERT(not attempt(apply));
        PPR_TEST_ASSERT(submissions == 1);
    };

    PPR_UNIT_TEST (resize_after_telemetry_refresh_apply_and_reset_revert_latest_geometry) {
        WindowDisplayGeometry cached{};
        cached.m_size.x = 800;
        cached.m_size.y = 600;
        WindowDisplayGeometry current = cached;
        current.m_size.x = 1280;
        current.m_size.y = 720;
        int acquisitions = 0;
        const auto acquire = [&]() -> Expected<WindowDisplayGeometry> {
            ++acquisitions;
            return current;
        };
        const DisplaySettings prior{};
        DisplaySettings apply = prior;
        apply.m_width = 1920u;
        apply.m_height = 1080u;
        DisplaySettings reset_prior = apply;
        const Resolved<DisplaySettings> reset = displayResetCandidate(committedDisplay());

        for (const bool window_change:
            {displayWindowChange(apply, prior), displayWindowChange(reset.m_value, reset_prior)}) {
            auto geometry = captureDisplayTrialGeometry(window_change, acquire);
            PPR_TEST_ASSERT(geometry.has_value());
            PPR_TEST_ASSERT(geometry->has_value());
            DisplayConfirm confirm{};
            PPR_TEST_ASSERT(not armDisplayConfirm(confirm, prior, *geometry));
            const auto revert = displayRevertSettings(confirm);
            PPR_TEST_ASSERT(revert.has_value());
            PPR_TEST_ASSERT(revert->m_width == 1280u);
            PPR_TEST_ASSERT(revert->m_height == 720u);
            PPR_TEST_ASSERT(revert->m_width != static_cast<u32>(cached.m_size.x));
        }
        PPR_TEST_ASSERT(acquisitions == 2);
    };

    PPR_UNIT_TEST (geometry_acquisition_failure_blocks_window_mutation_only) {
        const std::error_code error = std::make_error_code(std::errc::not_connected);
        const auto acquire = [&]() -> Expected<WindowDisplayGeometry> { return std::unexpected{error}; };
        const auto window = captureDisplayTrialGeometry(true, acquire);
        PPR_TEST_ASSERT(not window);
        PPR_TEST_ASSERT(window.error() == error);
        const auto policy = captureDisplayTrialGeometry(false, acquire);
        PPR_TEST_ASSERT(policy.has_value());
        PPR_TEST_ASSERT(not policy->has_value());
        const auto invalid = captureDisplayTrialGeometry(true, []() -> Expected<WindowDisplayGeometry> {
            WindowDisplayGeometry degenerate{};
            degenerate.m_size = int2{0, 0};
            return degenerate;
        });
        PPR_TEST_ASSERT(not invalid);
        PPR_TEST_ASSERT(invalid.error() == std::make_error_code(std::errc::io_error));
    };

    PPR_UNIT_TEST (arming_poll_holds_full_countdown) {
        DisplayConfirm confirm{};
        PPR_TEST_ASSERT(not armDisplayConfirm(confirm, DisplaySettings{}));
        PPR_TEST_ASSERT(not tickDisplayConfirmPoll(confirm, kDisplayConfirmSeconds * 2.0f, true));
        PPR_TEST_ASSERT(confirm.m_seconds_left == kDisplayConfirmSeconds);
        PPR_TEST_ASSERT(not tickDisplayConfirmPoll(confirm, kDisplayConfirmSeconds - 1.0f, false));
        PPR_TEST_ASSERT(tickDisplayConfirmPoll(confirm, 1.0f, false));
    };

    PPR_UNIT_TEST (reset_restores_baseline_provenance_not_factory_defaults) {
        Resolved<DisplaySettings> baseline = committedDisplay();
        baseline.m_value.m_cap_hz = 90u;
        for (auto &origin: baseline.m_origins) {
            if (origin.m_name == "display.cap_hz") {
                origin.m_source = ESource::environment;
                origin.m_location = "environment";
            }
        }

        Resolved<DisplaySettings> committed = baseline;
        Layer layer{};
        layer.m_source = ESource::runtime;
        layer.m_origin = "display-panel";
        layer.m_assignments.push_back(
            Assignment{"display.cap_hz", "panel", SourceValue::integer(30), "display-panel", 0u});
        Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not applyRuntime(committed, layer, diagnostics));
        PPR_TEST_ASSERT(displayHasRuntimeOverrides(committed));

        const Resolved<DisplaySettings> candidate = displayResetCandidate(baseline);
        PPR_TEST_ASSERT(candidate.m_value.m_cap_hz == 90u);
        PPR_TEST_ASSERT(candidate.m_value.m_cap_hz != DisplaySettings{}.m_cap_hz);
        const PropertyOrigin *origin = candidate.find("display.cap_hz");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::environment);
        PPR_TEST_ASSERT(not displayHasRuntimeOverrides(candidate));
    };

    PPR_UNIT_TEST (reset_gate_independent_of_apply_dirty_state) {
        PPR_TEST_ASSERT(not displayHasRuntimeOverrides(committedDisplay()));

        DisplayDraft draft{};
        draft.m_pacing = EFramePacing::paced;
        draft.m_cap_hz = 120u;
        Diagnostics diagnostics{};
        auto applied = validateDisplayDraft(committedDisplay(), draft, diagnostics);
        PPR_TEST_ASSERT(applied.has_value());
        const auto synced = displayDraftFrom(applied->m_value);
        PPR_TEST_ASSERT(synced.has_value());
        PPR_TEST_ASSERT(displayDraftMatches(*synced, applied->m_value));
        PPR_TEST_ASSERT(displayHasRuntimeOverrides(*applied));
    };

    PPR_UNIT_TEST (overlay_unreadable_is_error_not_absence) {
        // A directory is not an overlay file. Opening may fail (Windows)
        // or succeed (POSIX); sizing or reading must still reject it.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ppr-display-overlay-unreadable";
        std::error_code dir_error{};
        std::filesystem::create_directories(dir, dir_error);
        PPR_TEST_ASSERT(not dir_error);

        Diagnostics load_diagnostics{};
        const auto loaded = loadOverlayLayer(dir, load_diagnostics);
        PPR_TEST_ASSERT(not loaded.has_value());
        PPR_TEST_ASSERT(load_diagnostics.hasErrors());

        Diagnostics save_diagnostics{};
        const auto prior = readOverlayBytes(dir, save_diagnostics);
        PPR_TEST_ASSERT(not prior.has_value());
        PPR_TEST_ASSERT(save_diagnostics.hasErrors());

        std::error_code remove_error{};
        std::filesystem::remove(dir, remove_error);
    };

    class FailingOverlayBuffer final : public std::stringbuf {
        bool m_throw;
        int m_reads{0};

    public:
        explicit FailingOverlayBuffer(const bool throws) : std::stringbuf(std::string(5000u, 'x')), m_throw(throws) {
        }

    protected:
        std::streamsize xsgetn(char *const bytes, const std::streamsize count) override {
            if (++m_reads == 1) {
                return std::stringbuf::xsgetn(bytes, count);
            }
            if (m_throw) {
                throw std::runtime_error{"injected overlay read failure"};
            }
            // Premature EOF after a successful block, without setting flags.
            return std::stringbuf::xsgetn(bytes, std::min<std::streamsize>(3, count));
        }
    };

    PPR_UNIT_TEST (overlay_mid_read_failure_is_error) {
        std::stringstream clean{"[display]\n"};
        Diagnostics clean_diagnostics{};
        const auto bytes = readOverlayBytes(clean, 10u, "test-overlay", clean_diagnostics);
        PPR_TEST_ASSERT(bytes.has_value());
        PPR_TEST_ASSERT(*bytes == "[display]\n");
        PPR_TEST_ASSERT(not clean_diagnostics.hasErrors());

        FailingOverlayBuffer buffer{false};
        std::istream stream{&buffer};
        Diagnostics diagnostics{};
        const auto failed = readOverlayBytes(stream, 5000u, "test-overlay", diagnostics);
        PPR_TEST_ASSERT(not failed);
        PPR_TEST_ASSERT(diagnostics.hasErrors());
        PPR_TEST_ASSERT(stream.eof());
    };

    PPR_UNIT_TEST (overlay_buffer_exception_is_error) {
        for (const bool propagate: {false, true}) {
            FailingOverlayBuffer buffer{true};
            std::istream stream{&buffer};
            if (propagate) {
                stream.exceptions(std::ios::badbit);
            }
            Diagnostics diagnostics{};
            const auto failed = readOverlayBytes(stream, 5000u, "test-overlay", diagnostics);
            PPR_TEST_ASSERT(not failed);
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }
    };

    PPR_UNIT_TEST (overlay_sits_between_base_and_environment) {
        const CliSchema schema = gameCliSchema();
        Diagnostics parsed_diagnostics{};
        auto parsed = parseCliArgs(std::span<const std::string_view>{}, schema, parsed_diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());

        auto fileLayer = [](const i64 cap) {
            Layer layer{};
            layer.m_source = ESource::file;
            layer.m_origin = "base";
            layer.m_assignments.push_back(Assignment{"display.cap_hz", "", SourceValue::integer(cap), "base", 0u});
            return layer;
        };
        auto environmentLayer = []() {
            Layer layer{};
            layer.m_source = ESource::environment;
            layer.m_origin = "environment";
            layer.m_assignments.push_back(
                Assignment{"display.cap_hz", "", SourceValue::integer(90), "environment", 0u});
            return layer;
        };

        // Overlay beats the base file but loses to the environment.
        {
            const Layer base = fileLayer(30);
            Layer overlay = fileLayer(60);
            overlay.m_origin = "user-overlay";
            const Layer environment = environmentLayer();
            const Layer layers[3]{base, overlay, environment};
            Diagnostics diagnostics{};
            auto with_overlay = resolveGameCli(*parsed, std::span<const Layer>{layers, 3u}, diagnostics);
            PPR_TEST_ASSERT(with_overlay.has_value());
            PPR_TEST_ASSERT(with_overlay->m_display.m_value.m_cap_hz == 90u);
        }

        // Without the environment the overlay wins over the base file.
        {
            const Layer layers[2]{fileLayer(30), fileLayer(60)};
            Diagnostics diagnostics{};
            auto resolved = resolveGameCli(*parsed, std::span<const Layer>{layers, 2u}, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            PPR_TEST_ASSERT(resolved->m_display.m_value.m_cap_hz == 60u);
        }

        // The command line still wins over the overlay.
        {
            Diagnostics cli_diagnostics{};
            Array<std::string_view> args{};
            args.push_back(std::string_view{"--display.cap_hz=120"});
            auto cli = parseCliArgs(
                std::span<const std::string_view>{args.data(), args.size()}, schema, cli_diagnostics);
            PPR_TEST_ASSERT(cli.has_value());
            const Layer layers[1]{fileLayer(60)};
            Diagnostics diagnostics{};
            auto resolved = resolveGameCli(*cli, std::span<const Layer>{layers, 1u}, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            PPR_TEST_ASSERT(resolved->m_display.m_value.m_cap_hz == 120u);
        }
    };
} // namespace pP::game::tests::detail::display

namespace pP::game::tests {
    const UnitTest game_display = UnitTest::Named("game_display") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::display::draft_round_trip_matches_committed,
            detail::display::monitor_copy_failure_never_substitutes_defaults,
            detail::display::confirm_preparation_failure_preserves_complete_snapshot,
            detail::display::acknowledgement_moves_owned_snapshots_without_allocation,
            detail::display::runtime_layer_preserves_long_monitor_name,
            detail::display::switching_round_trip_matches_committed,
            detail::display::fullscreen_draft_validates_against_capabilities,
            detail::display::windowed_draft_passes_without_window_support,
            detail::display::fullscreen_rejected_without_window_support,
            detail::display::unknown_monitor_rejected,
            detail::display::numeric_monitor_rejected_as_handle,
            detail::display::unknown_window_mode_rejected,
            detail::display::windowed_refresh_rejected,
            detail::display::unpaired_dimensions_rejected,
            detail::display::unlisted_resolution_rejected,
            detail::display::unlisted_refresh_rejected,
            detail::display::explicit_refresh_rejected_when_unsupported,
            detail::display::switching_availability_gates_on_capabilities,
            detail::display::overlay_merge_carries_switching_fields,
            detail::display::window_change_drives_trial_decision,
            detail::display::valid_draft_validates_with_runtime_provenance,
            detail::display::cap_out_of_range_rejected_with_diagnostics,
            detail::display::unknown_pacing_name_rejected,
            detail::display::runtime_layer_cannot_touch_startup_properties,
            detail::display::overlay_merge_replaces_display_preserves_unrelated,
            detail::display::overlay_header_documents_override_order,
            detail::display::missing_overlay_is_absence_not_error,
            detail::display::atomic_write_round_trips_and_clears_temp,
            detail::display::confirm_counts_down_and_expires_once,
            detail::display::confirm_holds_while_revert_pends,
            detail::display::revert_restores_observed_geometry_not_requested_zeros,
            detail::display::revert_restores_exclusive_monitor_and_refresh,
            detail::display::revert_without_geometry_keeps_requested_policy,
            detail::display::revert_retry_survives_failed_revert_timeout,
            detail::display::trial_blocks_apply_until_resolved,
            detail::display::timeout_failed_revert_blocks_apply_and_reset_without_retry_storm,
            detail::display::resize_after_telemetry_refresh_apply_and_reset_revert_latest_geometry,
            detail::display::geometry_acquisition_failure_blocks_window_mutation_only,
            detail::display::arming_poll_holds_full_countdown,
            detail::display::reset_restores_baseline_provenance_not_factory_defaults,
            detail::display::reset_gate_independent_of_apply_dirty_state,
            detail::display::overlay_unreadable_is_error_not_absence,
            detail::display::overlay_mid_read_failure_is_error,
            detail::display::overlay_buffer_exception_is_error,
            detail::display::overlay_sits_between_base_and_environment,
        });
    };

    const UnitTest &gameDisplayTests() noexcept {
        return game_display;
    }
} // namespace pP::game::tests
