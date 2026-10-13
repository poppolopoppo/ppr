module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // display
    // ------------------------------------------------------------------

    namespace Display {
        struct TransactionApp final : Application {
            u32 m_settlements{0u};
            u32 m_updates{0u};

            TransactionApp()
                : Application(
                      ApplicationDomain{.m_is_interactive = false, .m_needs_user_interface = false},
                      "display-transaction", {}) {
            }

            ~TransactionApp() override {
                std::ignore = shutdown();
            }

            std::error_code boot() {
                const std::error_code error = initialize();
                if (not error) {
                    bindDisplaySurface_(WindowHandle{reinterpret_cast<void *>(1u)}, 7u);
                }
                return error;
            }
            void executor(RenderSurfaceExecutor execute) {
                renderLane()->setSurfaceExecutor(std::move(execute));
            }
            std::error_code pump() {
                return pumpRenderCompletions_();
            }
            std::error_code probe(bool &skip) {
                return probeFrameAdmissionPreUpdate_(skip);
            }
            SurfaceGeneration generation() const {
                return laneSurfaceGeneration(WindowHandle{reinterpret_cast<void *>(1u)});
            }
            void recreate() {
                bindDisplaySurface_(WindowHandle{reinterpret_cast<void *>(1u)}, 99u);
            }
            std::error_code synchronize() {
                bool ran = false;
                const std::error_code error = renderLane()->runOnLaneSync([]() -> std::error_code { return {}; }, &ran);
                return ran ? error : std::make_error_code(std::errc::not_connected);
            }
            std::error_code close() {
                return shutdown();
            }
            std::error_code finish(std::error_code prior) {
                return finishRun_(prior);
            }
            void settleAdmitted() {
                RenderFrameResult result{};
                result.m_reservation_epoch = reservationEpoch_();
                result.m_reservation_frame_id = getFrameIndex();
                onLaneFrameConsumed_(result);
            }
            void onLaneFrameConsumed_(const RenderFrameResult &result) override {
                ++m_settlements;
                Application::onLaneFrameConsumed_(result);
            }
            std::error_code update(TimeSpan) override {
                ++m_updates;
                return {};
            }
        };

        config::Resolved<DisplaySettings> pacedCandidate() {
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_pacing = EFramePacing::paced;
            candidate.m_value.m_cap_hz = 120u;
            candidate.m_value.m_paced_vsync = false;
            candidate.m_origins.push_back(
                config::PropertyOrigin{"display.cap_hz", config::ESource::runtime, "display panel"});
            return candidate;
        }

        DisplayCapabilities monitorCapabilities() {
            DisplayCapabilities result{};
            result.m_window_changes = true;
            result.m_exclusive_refresh = true;
            DisplayMonitorCapability monitor{};
            monitor.m_name = "Panel A";
            monitor.m_primary = true;
            monitor.m_current = {1920u, 1080u, 60u};
            monitor.m_modes.push_back({1920u, 1080u, 60u});
            monitor.m_modes.push_back({1920u, 1080u, std::nullopt});
            result.m_monitors.push_back(std::move(monitor));
            return result;
        }

        PPR_UNIT_TEST (monitor_identity_disambiguates_names_and_rejects_stale_tokens) {
            DisplayCapabilities capabilities = monitorCapabilities();
            auto &first = capabilities.m_monitors.front();
            first.m_identity = "win32-monitor:panel-a";
            DisplayMonitorCapability second = first;
            second.m_identity = "win32-monitor:panel-b";
            second.m_primary = false;
            second.m_current.m_refresh_hz = 144u;
            capabilities.m_monitors.push_back(std::move(second));
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(
                resolveDisplayMonitor("Panel A", capabilities, diagnostics).error() == std::errc::invalid_argument);
            const auto selected = resolveDisplayMonitor("win32-monitor:panel-b", capabilities, diagnostics);
            PPR_TEST_ASSERT(selected.has_value() and *selected == &capabilities.m_monitors.back());
            PPR_TEST_ASSERT((*selected)->m_current.m_refresh_hz == 144u);
            PPR_TEST_ASSERT(
                resolveDisplayMonitor("auto", capabilities, diagnostics).value() == &capabilities.m_monitors.front());
            capabilities.m_monitors.back().m_name = "win32-monitor:missing";
            PPR_TEST_ASSERT(
                resolveDisplayMonitor("win32-monitor:missing", capabilities, diagnostics).error() ==
                std::errc::no_such_device);
            capabilities.m_monitors.pop_back();
            PPR_TEST_ASSERT(
                resolveDisplayMonitor("win32-monitor:panel-b", capabilities, diagnostics).error() ==
                std::errc::no_such_device);
        };

        PPR_UNIT_TEST (unique_legacy_monitor_is_canonicalized_before_ack_and_save) {
            DisplayCapabilities capabilities = monitorCapabilities();
            capabilities.m_monitors.front().m_identity = "win32-monitor:panel-a";
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_monitor = "Panel A";
            DisplayController controller{};
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not controller.begin(std::move(candidate), capabilities, diagnostics));
            PPR_TEST_ASSERT(controller.committed().m_monitor == "auto");
            PPR_TEST_ASSERT(controller.pending()->m_resolved.m_value.m_monitor == "win32-monitor:panel-a");
            controller.markSubmitted();
            PPR_TEST_ASSERT(controller.acknowledge(
                controller.pending()->m_transaction_id,
                {.m_generation = 7u, .m_width = 1280, .m_height = 720, .m_available = true}, {}));
            const auto document = config::documentFrom(controller.committed());
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find_if(
                document->m_entries, [](const config::Entry &value) { return value.m_key == "display.monitor"; });
            PPR_TEST_ASSERT(entry != document->m_entries.end() and entry->m_value.m_text == "win32-monitor:panel-a");
            // Resetting a legacy startup preference to the same native monitor
            // must not invent a window change or require native mutation support.
            config::Resolved<DisplaySettings> legacy_reset = controller.resolved();
            legacy_reset.m_value.m_monitor = "Panel A";
            capabilities.m_window_changes = false;
            PPR_TEST_ASSERT(not controller.begin(std::move(legacy_reset), capabilities, diagnostics));
            PPR_TEST_ASSERT(not displayWindowChange(controller.pending()->m_resolved.m_value, controller.committed()));
            controller.cancelUnsubmitted();
            capabilities.m_monitors.front().m_name = "Renamed display";
            PPR_TEST_ASSERT(not validateDisplaySettings(controller.committed(), capabilities, diagnostics));
        };

        PPR_UNIT_TEST (monitor_without_persistent_identity_keeps_unique_legacy_policy) {
            DisplayCapabilities capabilities = monitorCapabilities();
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_monitor = "Panel A";
            config::Diagnostics diagnostics{};
            DisplayController controller{};
            PPR_TEST_ASSERT(not controller.begin(std::move(candidate), capabilities, diagnostics));
            PPR_TEST_ASSERT(controller.pending()->m_resolved.m_value.m_monitor == "Panel A");
            auto duplicate = capabilities.m_monitors.front();
            duplicate.m_primary = false;
            capabilities.m_monitors.push_back(std::move(duplicate));
            const auto match = findDisplayMonitor("Panel A", capabilities);
            PPR_TEST_ASSERT(match.m_ambiguous and match.m_monitor == nullptr);
            PPR_TEST_ASSERT(
                resolveDisplayMonitor("Panel A", capabilities, diagnostics).error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (window_schema_defaults_cli_and_source_permissions) {
            config::Diagnostics diagnostics{};
            const auto resolved = config::resolve<DisplaySettings>({}, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            PPR_TEST_ASSERT(resolved->m_value.m_window_mode == EDisplayWindowMode::windowed);
            PPR_TEST_ASSERT(resolved->m_value.m_monitor == "auto");
            PPR_TEST_ASSERT(resolved->m_value.m_width == 0u and resolved->m_value.m_height == 0u);
            PPR_TEST_ASSERT(resolved->m_value.m_refresh_hz == 0u);
            const auto options = displayCliOptions();
            PPR_TEST_ASSERT(options.size() == 9u);
            config::visit(resolved->m_value, [&](const auto &member, const auto &) {
                PPR_TEST_ASSERT(member.m_metadata.m_sources == config::ESource::all);
                PPR_TEST_ASSERT(member.m_metadata.m_mutability == config::EMutability::runtime);
                PPR_TEST_ASSERT(member.m_metadata.m_persistable);
                PPR_TEST_ASSERT(std::ranges::any_of(options, [&](const config::OptionDecl &option) {
                    return option.m_canonical == member.m_metadata.m_name;
                }));
            });
            config::Layer invalid{};
            invalid.m_source = config::ESource::cli;
            invalid.m_assignments.push_back(
                {.m_name = "display.window_mode", .m_value = config::SourceValue::text("borderless")});
            const std::array layers{invalid};
            PPR_TEST_ASSERT(not config::resolve<DisplaySettings>(layers, diagnostics));
        };

        PPR_UNIT_TEST (window_schema_precedence_and_monitor_name_persistence) {
            config::Layer file{};
            file.m_source = config::ESource::file;
            file.m_assignments.push_back({.m_name = "display.width", .m_value = config::SourceValue::integer(800)});
            file.m_assignments.push_back({.m_name = "display.height", .m_value = config::SourceValue::integer(600)});
            file.m_assignments.push_back(
                {.m_name = "display.monitor", .m_value = config::SourceValue::text("Panel A")});
            config::Layer cli{};
            cli.m_source = config::ESource::cli;
            cli.m_assignments.push_back({.m_name = "display.width", .m_value = config::SourceValue::text("1024")});
            config::Layer runtime{};
            runtime.m_source = config::ESource::runtime;
            runtime.m_assignments.push_back({.m_name = "display.width", .m_value = config::SourceValue::integer(1280)});
            const std::array layers{file, cli, runtime};
            config::Diagnostics diagnostics{};
            const auto resolved = config::resolve<DisplaySettings>(layers, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            PPR_TEST_ASSERT(resolved->m_value.m_width == 1280u);
            PPR_TEST_ASSERT(resolved->find("display.width")->m_source == config::ESource::runtime);
            const auto document = config::documentFrom(resolved->m_value);
            PPR_TEST_ASSERT(document.has_value() and document->m_entries.size() == 9u);
            const auto monitor = std::ranges::find_if(
                document->m_entries, [](const config::Entry &entry) { return entry.m_key == "display.monitor"; });
            PPR_TEST_ASSERT(monitor != document->m_entries.end());
            PPR_TEST_ASSERT(monitor->m_value.m_kind == config::EFileKind::text);
            PPR_TEST_ASSERT(monitor->m_value.m_text == "Panel A");
            const auto encoded = config::encode(*document);
            PPR_TEST_ASSERT(encoded.has_value());
            const auto decoded = config::decode(*encoded, "overlay", diagnostics);
            PPR_TEST_ASSERT(decoded.has_value());
            const std::array roundtrip{config::toLayer(*decoded, "overlay")};
            const auto saved = config::resolve<DisplaySettings>(roundtrip, diagnostics);
            PPR_TEST_ASSERT(saved.has_value() and saved->m_value.m_monitor == "Panel A");
        };

        PPR_UNIT_TEST (window_schema_source_validation_and_no_numeric_monitor_handles) {
            config::Diagnostics diagnostics{};
            for (const config::ESource source:
                {config::ESource::file, config::ESource::environment, config::ESource::cli, config::ESource::runtime}) {
                config::Layer layer{};
                layer.m_source = source;
                layer.m_assignments.push_back(
                    {.m_name = "display.window_mode", .m_value = config::SourceValue::text("fullscreen")});
                layer.m_assignments.push_back(
                    {.m_name = "display.monitor", .m_value = config::SourceValue::text("Panel A")});
                layer.m_assignments.push_back(
                    {.m_name = "display.width", .m_value = config::SourceValue::integer(1920)});
                layer.m_assignments.push_back(
                    {.m_name = "display.height", .m_value = config::SourceValue::integer(1080)});
                layer.m_assignments.push_back(
                    {.m_name = "display.refresh_hz", .m_value = config::SourceValue::integer(60)});
                const std::array layers{layer};
                PPR_TEST_ASSERT(config::resolve<DisplaySettings>(layers, diagnostics).has_value());
            }
            config::Layer invalid{};
            invalid.m_source = config::ESource::file;
            invalid.m_assignments.push_back(
                {.m_name = "display.monitor", .m_value = config::SourceValue::integer(1234)});
            PPR_TEST_ASSERT(not config::resolve<DisplaySettings>(std::array{invalid}, diagnostics));
            invalid.m_assignments.clear();
            invalid.m_assignments.push_back({.m_name = "display.width", .m_value = config::SourceValue::integer(800)});
            PPR_TEST_ASSERT(not config::resolve<DisplaySettings>(std::array{invalid}, diagnostics));
            invalid.m_assignments.clear();
            invalid.m_assignments.push_back(
                {.m_name = "display.refresh_hz", .m_value = config::SourceValue::integer(60)});
            PPR_TEST_ASSERT(not config::resolve<DisplaySettings>(std::array{invalid}, diagnostics));
            invalid.m_source = static_cast<config::ESource>(0x80u);
            invalid.m_assignments.clear();
            invalid.m_assignments.push_back(
                {.m_name = "display.monitor", .m_value = config::SourceValue::text("auto")});
            PPR_TEST_ASSERT(not config::resolve<DisplaySettings>(std::array{invalid}, diagnostics));
        };

        PPR_UNIT_TEST (window_mode_capabilities_cross_fields_and_unknown_refresh) {
            DisplayCapabilities capabilities = monitorCapabilities();
            config::Diagnostics diagnostics{};
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_window_mode = EDisplayWindowMode::fullscreen;
            candidate.m_value.m_width = 1920u;
            candidate.m_value.m_height = 1080u;
            candidate.m_value.m_refresh_hz = 60u;
            DisplayController valid{};
            PPR_TEST_ASSERT(not valid.begin(candidate, capabilities, diagnostics));
            PPR_TEST_ASSERT(valid.committed().m_window_mode == EDisplayWindowMode::windowed);
            valid.markSubmitted();
            PPR_TEST_ASSERT(valid.acknowledge(
                valid.pending()->m_transaction_id,
                {.m_generation = 8u, .m_width = 1920, .m_height = 1080, .m_available = true}, {}));
            PPR_TEST_ASSERT(valid.committed().m_window_mode == EDisplayWindowMode::fullscreen);

            const auto rejected = [&](const DisplaySettings &settings, const DisplayCapabilities &view) {
                DisplayController controller{};
                config::Resolved<DisplaySettings> invalid{};
                invalid.m_value = settings;
                diagnostics.clear();
                const std::error_code error = controller.begin(std::move(invalid), view, diagnostics);
                PPR_TEST_ASSERT(error and diagnostics.hasErrors());
                PPR_TEST_ASSERT(not controller.pending());
            };
            DisplaySettings settings = candidate.m_value;
            settings.m_window_mode = static_cast<EDisplayWindowMode>(99u);
            rejected(settings, capabilities);
            settings = candidate.m_value;
            settings.m_monitor = "removed";
            rejected(settings, capabilities);
            settings = candidate.m_value;
            settings.m_height = 0u;
            rejected(settings, capabilities);
            settings = candidate.m_value;
            settings.m_window_mode = EDisplayWindowMode::windowed;
            rejected(settings, capabilities);
            settings = candidate.m_value;
            settings.m_refresh_hz = 144u;
            rejected(settings, capabilities);
            settings = candidate.m_value;
            capabilities.m_exclusive_refresh = false;
            rejected(settings, capabilities);
            capabilities = monitorCapabilities();
            capabilities.m_monitors.front().m_modes.erase(capabilities.m_monitors.front().m_modes.begin());
            rejected(settings, capabilities); // Unknown refresh never proves 60Hz support.
            PPR_TEST_ASSERT(not capabilities.m_monitors.front().m_modes.front().m_refresh_hz);
            settings.m_refresh_hz = 0u;
            PPR_TEST_ASSERT(not validateDisplaySettings(settings, capabilities, diagnostics));
        };

        PPR_UNIT_TEST (monitor_preference_is_revalidated_after_enumeration) {
            DisplayCapabilities capabilities = monitorCapabilities();
            config::Diagnostics diagnostics{};
            DisplaySettings settings{};
            settings.m_monitor = "Panel A";
            PPR_TEST_ASSERT(not validateDisplaySettings(settings, capabilities, diagnostics));
            capabilities.m_monitors.front().m_name = "Panel B";
            PPR_TEST_ASSERT(
                validateDisplaySettings(settings, capabilities, diagnostics) ==
                std::make_error_code(std::errc::no_such_device));
            settings.m_monitor = "auto";
            PPR_TEST_ASSERT(not validateDisplaySettings(settings, capabilities, diagnostics));
            settings.m_monitor = "Panel B";
            capabilities.m_monitors.push_back(capabilities.m_monitors.front());
            PPR_TEST_ASSERT(
                validateDisplaySettings(settings, capabilities, diagnostics) ==
                std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        };

        PPR_UNIT_TEST (cancel_after_native_apply_requires_acknowledged_restore) {
            DisplayController controller{};
            config::Diagnostics diagnostics{};
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_window_mode = EDisplayWindowMode::fullscreen;
            PPR_TEST_ASSERT(not controller.begin(candidate, monitorCapabilities(), diagnostics));
            controller.markWindowApplied();
            const u64 apply_id = controller.pending()->m_transaction_id;
            controller.stopTransactions();
            PPR_TEST_ASSERT(controller.restoring() and controller.admissionSuspended());
            PPR_TEST_ASSERT(controller.pending()->m_transaction_id != apply_id);
            controller.markSubmitted();
            PPR_TEST_ASSERT(controller.acknowledge(
                controller.pending()->m_transaction_id,
                {.m_generation = 8u, .m_width = 800, .m_height = 600, .m_available = true}, {}));
            PPR_TEST_ASSERT(controller.transactionState() == EDisplayTransaction::idle);
            PPR_TEST_ASSERT(controller.committed().m_window_mode == EDisplayWindowMode::windowed);
            PPR_TEST_ASSERT(controller.resolved().m_revision == 0u);
        };

        PPR_UNIT_TEST (ack_with_suspended_admission_and_zero_frames_commits_pacing) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::binary_semaphore entered{0}, release{0};
            bool released = false;
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &result) {
                entered.release();
                release.acquire();
                result.m_applied = {
                    .m_generation = 8u, .m_extent = int2{80, 80}, .m_vsync = false, .m_available = true};
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(test_app.displayController().admissionSuspended());
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not test_app.pump()); // Posts control without an admitted frame.
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));
            bool skip = false;
            PPR_TEST_ASSERT(not test_app.probe(skip) and skip);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(test_app.generation() == 7u);
            release.release();
            released = true;
            // The owner cannot run the handoff until it has published the ack.
            // Its main-side wait pump may itself perform the commit.
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(not test_app.displayController().admissionSuspended());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 1u);
            const config::PropertyOrigin *const origin = test_app.displayController().resolved().find("display.cap_hz");
            PPR_TEST_ASSERT(origin != nullptr);
            PPR_TEST_ASSERT(origin->m_location == "display panel");
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == foregroundFramePeriod(pacedCandidate().m_value));
            PPR_TEST_ASSERT(test_app.generation() == 8u);
            PPR_TEST_ASSERT(test_app.m_updates == 0u and test_app.m_settlements == 0u);
            skip = true;
            PPR_TEST_ASSERT(not test_app.probe(skip) and not skip);
            test_app.settleAdmitted();
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (stale_ack_cannot_commit_to_recreated_window) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::binary_semaphore entered{0}, release{0};
            bool released = false;
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &result) {
                entered.release();
                release.acquire();
                result.m_applied = {
                    .m_generation = 8u, .m_extent = int2{80, 80}, .m_vsync = false, .m_available = true};
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));
            test_app.recreate();
            release.release();
            released = true;
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(test_app.generation() == 99u);
            PPR_TEST_ASSERT(test_app.m_settlements == 0u);
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (failed_reconfigure_restores_through_same_transaction_path) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::counting_semaphore<2> executed{0};
            std::atomic<u32> calls{0u};
            PPR_DEFER {
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                const u32 call = calls.fetch_add(1u);
                result.m_applied = {
                    .m_generation = 8u + call, .m_extent = int2{80, 80}, .m_vsync = true, .m_available = true};
                if (call == 0u) {
                    result.m_error = std::make_error_code(std::errc::io_error);
                } else if (request.m_identity.m_generation != 8u or not request.m_settings.m_vsync) {
                    result.m_error = std::make_error_code(std::errc::invalid_argument);
                }
                executed.release();
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(executed.try_acquire_for(std::chrono::seconds{30}));
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(executed.try_acquire_for(std::chrono::seconds{30}));
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(calls.load() == 2u);
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::idle);
            PPR_TEST_ASSERT(
                test_app.displayController().transactionError() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(test_app.generation() == 9u and test_app.m_settlements == 0u);
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (failed_reconfigure_and_failed_restore_report_degraded_actual_state) {
            DisplayController controller{};
            PPR_TEST_ASSERT(not controller.begin(pacedCandidate()));
            const u64 first_id = controller.pending()->m_transaction_id;
            controller.markSubmitted();
            const std::error_code apply_error = std::make_error_code(std::errc::io_error);
            PPR_TEST_ASSERT(controller.acknowledge(first_id, {.m_generation = 8u}, apply_error));
            PPR_TEST_ASSERT(controller.restoring() and controller.admissionSuspended());
            const u64 restore_id = controller.pending()->m_transaction_id;
            PPR_TEST_ASSERT(restore_id != first_id);
            controller.markSubmitted();
            const std::error_code restore_error = std::make_error_code(std::errc::not_connected);
            PPR_TEST_ASSERT(controller.acknowledge(
                restore_id, {.m_generation = 9u, .m_available = false, .m_restoration_error = restore_error},
                restore_error));
            PPR_TEST_ASSERT(controller.transactionState() == EDisplayTransaction::degraded);
            PPR_TEST_ASSERT(controller.transactionError() == apply_error);
            PPR_TEST_ASSERT(not controller.applied().m_available);
            PPR_TEST_ASSERT(controller.applied().m_restoration_error == restore_error);
            PPR_TEST_ASSERT(controller.resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not controller.foregroundTarget().has_value());
        };

        PPR_UNIT_TEST (failed_restore_stops_admission_and_retains_first_error) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::counting_semaphore<2> executed{0};
            std::atomic<u32> calls{0u};
            PPR_DEFER {
                std::ignore = test_app.close();
            };
            const std::error_code apply_error = std::make_error_code(std::errc::io_error);
            const std::error_code restore_error = std::make_error_code(std::errc::permission_denied);
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &result) {
                const u32 call = calls.fetch_add(1u);
                // The backend still reports a usable old surface; app-level
                // restoration nevertheless failed and must remain visible.
                result.m_applied = {
                    .m_generation = 8u + call, .m_extent = int2{80, 80}, .m_vsync = true, .m_available = true};
                result.m_error = call == 0u ? apply_error : restore_error;
                executed.release();
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(executed.try_acquire_for(std::chrono::seconds{30}));
            PPR_TEST_ASSERT(not test_app.synchronize());
            std::ignore = test_app.pump();
            PPR_TEST_ASSERT(executed.try_acquire_for(std::chrono::seconds{30}));
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(test_app.pump() == apply_error);
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::degraded);
            PPR_TEST_ASSERT(test_app.displayController().applied().m_restoration_error == restore_error);
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            bool skip = false;
            PPR_TEST_ASSERT(test_app.probe(skip) == apply_error);
            PPR_TEST_ASSERT(test_app.close() == apply_error);
        };

        PPR_UNIT_TEST (shutdown_cancels_unsubmitted_transaction_without_lane_execution) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::atomic<u32> executed{0u};
            PPR_DEFER {
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &) { executed.fetch_add(1u); });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(not test_app.close());
            PPR_TEST_ASSERT(executed.load() == 0u);
            PPR_TEST_ASSERT(not test_app.displayController().pending());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(
                test_app.requestDisplaySettings(pacedCandidate()) ==
                std::make_error_code(std::errc::operation_canceled));
        };

        PPR_UNIT_TEST (shutdown_drains_accepted_transaction_without_frame_settlement) {
            TransactionApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::binary_semaphore entered{0}, release{0};
            bool released = false;
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &result) {
                entered.release();
                release.acquire();
                result.m_applied = {
                    .m_generation = 8u, .m_extent = int2{80, 80}, .m_vsync = false, .m_available = true};
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(pacedCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));
            test_app.setGpuTeardownEntryHookForTest([&] {
                release.release();
                released = true;
            });
            const std::error_code first_error = std::make_error_code(std::errc::io_error);
            PPR_TEST_ASSERT(test_app.finish(first_error) == first_error);
            PPR_TEST_ASSERT(not test_app.displayController().pending());
            PPR_TEST_ASSERT(test_app.m_settlements == 0u);
        };
        // Phase 3 DisplayController: pure pacing policy without a live
        // window/monitor — settings in, foreground/effective targets and
        // throttle state out. No Application, no platform, no renderer.

        PPR_UNIT_TEST (default_is_follow_vsync_without_cpu_cap) {
            DisplayController controller{};
            PPR_TEST_ASSERT(controller.committed().m_pacing == EFramePacing::follow_vsync);
            PPR_TEST_ASSERT(controller.committed().m_throttle_unfocused);
            PPR_TEST_ASSERT(not controller.foregroundTarget().has_value());
            PPR_TEST_ASSERT(controller.presentationVsync());
            PPR_TEST_ASSERT(not controller.throttleActive(false));
            PPR_TEST_ASSERT(controller.throttleActive(true));
        };

        PPR_UNIT_TEST (mode_mapping_covers_foreground_table) {
            DisplaySettings uncapped{};
            uncapped.m_pacing = EFramePacing::uncapped;
            PPR_TEST_ASSERT(not foregroundFramePeriod(uncapped).has_value());
            PPR_TEST_ASSERT(not presentationVsync(uncapped));

            DisplaySettings follow{};
            PPR_TEST_ASSERT(not foregroundFramePeriod(follow).has_value());
            PPR_TEST_ASSERT(presentationVsync(follow));

            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 144u;
            paced.m_paced_vsync = false;
            const TimeSpan k144 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 144.0});
            PPR_TEST_ASSERT(foregroundFramePeriod(paced) == k144);
            PPR_TEST_ASSERT(not presentationVsync(paced));
            paced.m_paced_vsync = true;
            PPR_TEST_ASSERT(presentationVsync(paced));

            const PacingTargets targets = pacingTargets(paced);
            PPR_TEST_ASSERT(targets.m_target == k144);
            PPR_TEST_ASSERT(targets.m_vsync);
        };

        PPR_UNIT_TEST (paced_zero_cap_degrades_to_uncapped) {
            // Defensive only: validated resolution constrains cap_hz to
            // [1,1000], so a hand-built zero must never divide by zero.
            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 0u;
            PPR_TEST_ASSERT(not foregroundFramePeriod(paced).has_value());
            PPR_TEST_ASSERT(not DisplayController{paced}.foregroundTarget().has_value());
        };

        PPR_UNIT_TEST (overlay_inactive_passes_foreground_through) {
            const TimeSpan k60 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 60.0});
            PPR_TEST_ASSERT(effectiveFramePeriod(k60, false) == k60);
            PPR_TEST_ASSERT(not effectiveFramePeriod(std::nullopt, false).has_value());

            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 60u;
            DisplayController controller{paced};
            PPR_TEST_ASSERT(controller.effectiveTarget(false) == k60);
        };

        PPR_UNIT_TEST (overlay_floors_uncapped_at_200ms) {
            const TimeSpan kFloor{std::chrono::milliseconds{200}};
            PPR_TEST_ASSERT(kUnfocusedThrottlePeriod == kFloor);
            PPR_TEST_ASSERT(effectiveFramePeriod(std::nullopt, true) == kFloor);

            const TimeSpan k60 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 60.0});
            PPR_TEST_ASSERT(effectiveFramePeriod(k60, true) == kFloor);

            DisplayController follow{};
            PPR_TEST_ASSERT(follow.throttleActive(true));
            PPR_TEST_ASSERT(follow.effectiveTarget(true) == kFloor);
            PPR_TEST_ASSERT(not follow.effectiveTarget(false).has_value());
        };

        PPR_UNIT_TEST (overlay_preserves_slower_explicit_cap) {
            // A paced 1fps foreground stays 1fps under the overlay instead
            // of speeding up to 5fps.
            const TimeSpan k1Fps = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0});
            PPR_TEST_ASSERT(effectiveFramePeriod(k1Fps, true) == k1Fps);

            DisplaySettings slow{};
            slow.m_pacing = EFramePacing::paced;
            slow.m_cap_hz = 1u;
            DisplayController controller{slow};
            PPR_TEST_ASSERT(controller.foregroundTarget() == k1Fps);
            PPR_TEST_ASSERT(controller.effectiveTarget(true) == k1Fps);
            PPR_TEST_ASSERT(controller.effectiveTarget(false) == k1Fps);
        };

        PPR_UNIT_TEST (overlay_disabled_by_setting) {
            DisplaySettings settings{};
            settings.m_throttle_unfocused = false;
            DisplayController controller{settings};
            PPR_TEST_ASSERT(not controller.throttleActive(true));
            PPR_TEST_ASSERT(not controller.effectiveTarget(true).has_value());
        };

        PPR_UNIT_TEST (controller_view_reports_telemetry_snapshot) {
            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 60u;
            DisplayController controller{paced};

            const TimeSpan k60 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 60.0});
            const TimeSpan kFloor{std::chrono::milliseconds{200}};
            const DisplayView active = controller.view(true);
            PPR_TEST_ASSERT(active.m_committed.m_pacing == EFramePacing::paced);
            PPR_TEST_ASSERT(active.m_committed.m_cap_hz == 60u);
            PPR_TEST_ASSERT(active.m_foreground == k60);
            PPR_TEST_ASSERT(active.m_effective == kFloor);
            PPR_TEST_ASSERT(active.m_vsync);
            PPR_TEST_ASSERT(active.m_background_priority);
            PPR_TEST_ASSERT(active.m_throttle_active);

            const DisplayView idle = controller.view(false);
            PPR_TEST_ASSERT(idle.m_foreground == k60);
            PPR_TEST_ASSERT(idle.m_effective == k60);
            PPR_TEST_ASSERT(not idle.m_throttle_active);

            DisplayController follow{};
            follow.setCommitted(DisplaySettings{});
            PPR_TEST_ASSERT(follow.committed().m_pacing == EFramePacing::follow_vsync);
            PPR_TEST_ASSERT(follow.presentationVsync());
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest display = UnitTest::Named("display") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Display::monitor_identity_disambiguates_names_and_rejects_stale_tokens,
            detail::Display::unique_legacy_monitor_is_canonicalized_before_ack_and_save,
            detail::Display::monitor_without_persistent_identity_keeps_unique_legacy_policy,
            detail::Display::window_schema_defaults_cli_and_source_permissions,
            detail::Display::window_schema_precedence_and_monitor_name_persistence,
            detail::Display::window_schema_source_validation_and_no_numeric_monitor_handles,
            detail::Display::window_mode_capabilities_cross_fields_and_unknown_refresh,
            detail::Display::monitor_preference_is_revalidated_after_enumeration,
            detail::Display::cancel_after_native_apply_requires_acknowledged_restore,
            detail::Display::default_is_follow_vsync_without_cpu_cap,
            detail::Display::mode_mapping_covers_foreground_table,
            detail::Display::paced_zero_cap_degrades_to_uncapped,
            detail::Display::overlay_inactive_passes_foreground_through,
            detail::Display::overlay_floors_uncapped_at_200ms,
            detail::Display::overlay_preserves_slower_explicit_cap,
            detail::Display::overlay_disabled_by_setting,
            detail::Display::controller_view_reports_telemetry_snapshot,
            detail::Display::ack_with_suspended_admission_and_zero_frames_commits_pacing,
            detail::Display::stale_ack_cannot_commit_to_recreated_window,
            detail::Display::failed_reconfigure_restores_through_same_transaction_path,
            detail::Display::failed_reconfigure_and_failed_restore_report_degraded_actual_state,
            detail::Display::failed_restore_stops_admission_and_retains_first_error,
            detail::Display::shutdown_cancels_unsubmitted_transaction_without_lane_execution,
            detail::Display::shutdown_drains_accepted_transaction_without_frame_settlement,
        });
    };

    const UnitTest &displayTests() noexcept {
        return display;
    }
} // namespace pP::tests
