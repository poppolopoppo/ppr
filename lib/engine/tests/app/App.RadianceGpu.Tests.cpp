module;
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.rhi;
import engine.math;
import engine.shader;
import engine.image;
import std;

namespace pP::tests::detail::RadianceGpu {
    // ------------------------------------------------------------------
    // direct GPU transport fixture with explicit completion ownership
    // ------------------------------------------------------------------

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue{};
        FrameFence m_fence{};
        lighting::RadianceCascadePass m_pass{};

        // The writer borrows this callable until the leaf or shutdown drains and restores
        // its predecessor. The mutex publishes that predecessor before use.
        std::mutex m_diagnostic_mutex{};
        std::optional<Log::Policy> m_previous_policy{};
        std::function<void(const Log::Entry &)> m_diagnostic_sink{};
        u64 m_diagnostic_sequence{};
        std::optional<Log::ELevel> m_previous_minimum{};
        bool m_shader_attempted{};
        bool m_rhi_attempted{};
        bool m_diagnostic_enabled{};
        bool m_precision_expected{};

        void beginDiagnostics() {
            if (m_diagnostic_enabled) {
                return;
            }

            const auto diagnostic_setting = config::getenvLookup("PPR_TEST_RADIANCE_CASCADE_DIAGNOSTIC");
            if (diagnostic_setting.has_value() and *diagnostic_setting == "1") {
                m_diagnostic_sink = [this](const Log::Entry &entry) {
                    const std::lock_guard sink_lock{m_diagnostic_mutex};
                    if (entry.m_site.m_category.m_name.view() == "RadiancePass") {
                        using Period = TimePoint::duration::period;
                        std::println(
                            "RADIANCE_DIAGNOSTIC_LOG seq={} raw_tick={} tick_num={} tick_den={} thread_id={} line={} phase={} params={}",
                            ++m_diagnostic_sequence, entry.m_timestamp.time_since_epoch().count(), Period::num,
                            Period::den, entry.m_thread_id.m_value, entry.m_site.m_location.line(), entry.m_message,
                            entry.m_params);
                        std::ignore = std::fflush(nullptr); // Best-effort observation before watchdog termination.
                    }

                    (*m_previous_policy)(entry);
                };

                PPR_TEST_ASSERT(not Log::flush(true));

                const std::lock_guard setup_lock{m_diagnostic_mutex};
                m_previous_policy = Log::setWriterPolicy(m_diagnostic_sink);
                m_diagnostic_enabled = true;
                m_previous_minimum = Log::setMinimumVerboseLevel(Log::ELevel::info);
            }
        }

        void endDiagnostics() {
            if (m_diagnostic_enabled) {
                std::ignore = Log::setMinimumVerboseLevel(*m_previous_minimum);

                if (Log::flush(true)) {
                    hal::fatalExit("radiance-diagnostic-log-drain-failed");
                }

                std::ignore = Log::setWriterPolicy(*m_previous_policy);
                m_previous_policy.reset();
                m_previous_minimum.reset();
                m_diagnostic_enabled = false;
                std::println("RADIANCE_DIAGNOSTIC_DRAINED");
                std::ignore = std::fflush(nullptr); // Best-effort final diagnostic sentinel.
            }
        }

        void observePhase(const std::string_view phase) const {
            if (m_diagnostic_enabled) {
                using Clock = std::chrono::steady_clock;
                using Period = Clock::period;
                std::println(
                    "RADIANCE_FIELD_PHASE raw_tick={} tick_num={} tick_den={} phase={} completed={} next={}",
                    Clock::now().time_since_epoch().count(), Period::num, Period::den, phase, u64(m_fence.completed()),
                    u64(m_fence.nextValue()));
                std::ignore = std::fflush(nullptr); // Best-effort watchdog observation, never a verdict.
            }
        }

        void initialize(
            const bool initialize_transport = true, const lighting::TerrainPrecisionSelection precision = {}) {
            beginDiagnostics();
            observePhase("fixture.cold-initialize.begin");

            m_shader_attempted = true;
            PPR_TEST_ASSERT(not m_shaders->initialize());

            const auto validation_setting = config::getenvLookup("PPR_TEST_GPU_VALIDATION");
            const bool validation = validation_setting.has_value() and *validation_setting == "1";
            m_rhi->setCoreValidation(validation);
            m_rhi->setGpuAssistedValidation(validation);
            PPR_TEST_ASSERT(m_rhi->isCoreValidationEnabled() == validation);
            PPR_TEST_ASSERT(m_rhi->isGpuAssistedValidationEnabled() == validation);
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif

            m_rhi_attempted = true;
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            auto &device = m_rhi->getDevice();
            PPR_TEST_ASSERT(not make_error_code(device.getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));
            PPR_TEST_ASSERT(not m_fence.initialize(device));
            if (initialize_transport) {
                PPR_TEST_ASSERT(not m_pass.initialize(device, *m_shaders, std::filesystem::current_path(), precision));
                m_precision_expected = precision.m_precision ==
                                       lighting::TerrainPrecision::qualified_fp64_refinement_experimental;
            }

            observePhase("fixture.cold-initialize.end");
        }

        void proveIdle() {
            // Unknown completion must retain every producer/view lease.
            if (m_queue and make_error_code(m_queue->waitOnHost())) {
                hal::fatalExit("radiance-qualification-completion-unknown");
            }
        }

        void shutdown(const bool preserve_failure = false) {
            // Restore even when a cleanup assertion reports a separate failure.
            // The callable and its synchronization state outlive the final drain.
            PPR_DEFER {
                endDiagnostics();
            };

            // This test boundary owns submission and stops creating commands
            // before proving idleness. Unknown completion must retain resources.
            proveIdle();

            const auto pass_error = m_pass.shutdown({GpuQuiescence::kIdleProven, {}});
            const auto fence_error = m_fence.shutdown();
            m_queue.setNull();
            const auto rhi_error = m_rhi_attempted ? m_rhi->shutdown() : std::error_code{};
            const auto shader_error = m_shader_attempted ? m_shaders->shutdown() : std::error_code{};

            // An already failed scenario must retain its original named verdict.
            // Cleanup still attempts each independent owner and reports errors.
            for (const auto &[name, error]: std::array{
                     std::pair{"pass", pass_error}, std::pair{"fence", fence_error}, std::pair{"rhi", rhi_error},
                     std::pair{"shader", shader_error}}) {
                if (preserve_failure) {
                    if (error) {
                        std::println(
                            "RADIANCE_CLEANUP_ERROR owner={} category={} value={} message={}", name,
                            error.category().name(), error.value(), error.message());
                        std::ignore = std::fflush(nullptr); // Best-effort preservation of cleanup evidence.
                    }
                } else {
                    PPR_TEST_ASSERT(not error);
                }
            }
        }

        void observeReadFailure(const std::error_code &error) const {
            if (m_diagnostic_enabled and error) {
                std::println(
                    "RADIANCE_DIAGNOSTIC_READ_ERROR category={} value={} message={}", error.category().name(),
                    error.value(), error.message());
                std::ignore = std::fflush(
                    nullptr); // Best-effort observation; original assertions remain authoritative.
            }
        }

        void submit(const bool cascades = false, const bool receivers = false, const bool background = false) {
            observePhase("submission.encode.begin");
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(m_queue->createCommandEncoder(encoder.writeRef())));
            const auto completed = m_fence.pollCompleted();
            PPR_TEST_ASSERT(completed.has_value());
            const PreRenderContext context{m_rhi->getDevice(), *encoder, *completed, {}};
            PPR_TEST_ASSERT(not (
                background ? m_pass.encodeTerrainBackgroundView(context)
                           : (cascades ? m_pass.encodeCascades(context) : m_pass.encodeTrace(context))));

            observePhase("submission.encode.end");

            rhi::ComPtr<rhi::ICommandBuffer> commands{};
            observePhase("submission.finish.begin");
            const auto finish_error = make_error_code(encoder->finish(commands.writeRef()));
            if (finish_error) {
                encoder.setNull();
                commands.setNull();
                PPR_TEST_ASSERT(not m_pass.discardUnsubmitted());
                PPR_TEST_ASSERT(not finish_error);
            }

            observePhase("submission.finish.end");

            rhi::ICommandBuffer *command_pointer = commands.get();
            rhi::IFence *signal = m_fence.fence();
            const FenceCompletionValue value = m_fence.nextValue();
            const u64 raw_value = value;
            rhi::SubmitDesc description{};
            description.commandBuffers = &command_pointer;
            description.commandBufferCount = 1u;
            description.signalFences = &signal;
            description.signalFenceValues = &raw_value;
            description.signalFenceCount = 1u;
            observePhase("submission.queue.begin");
            const auto submit_error = make_error_code(m_queue->submit(description));
            if (submit_error) {
                PPR_TEST_ASSERT(not m_pass.notifySubmitFailed());
                PPR_TEST_ASSERT(not submit_error);
            }

            observePhase("submission.queue.end");

            m_fence.commitSubmitted(value);
            PPR_TEST_ASSERT(not m_pass.notifySubmitted(value));

            const auto premature_statistics = m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
            if (m_precision_expected) {
                PPR_TEST_ASSERT(
                    not premature_statistics and
                    premature_statistics.error() == std::errc::resource_unavailable_try_again);
            } else {
                PPR_TEST_ASSERT(premature_statistics.has_value() and not *premature_statistics);
            }

            // The deliberately old completion is explicit, independent of how
            // quickly this small batch actually executes on the device.
            if (background) {
                const auto premature = m_pass.acquireTerrainBackgroundView(kFenceValueNone);
                PPR_TEST_ASSERT(not premature and premature.error() == std::errc::resource_unavailable_try_again);
            } else if (receivers) {
                const auto premature = m_pass.readTerrainReceivers(kFenceValueNone);
                PPR_TEST_ASSERT(not premature and premature.error() == std::errc::resource_unavailable_try_again);
            } else {
                const auto premature = m_pass.readCompleted(kFenceValueNone);
                PPR_TEST_ASSERT(not premature and premature.error() == std::errc::resource_unavailable_try_again);
            }

            observePhase("submission.fence-wait.begin");
            PPR_TEST_ASSERT(not m_fence.waitFor(value));
            observePhase("submission.fence-wait.end");
        }
    };

    // ------------------------------------------------------------------
    // loop-local receiver and background scenario ownership
    // ------------------------------------------------------------------

    struct TerrainFieldScope {
        std::unique_ptr<Fixture> m_fixture{};
        std::function<void()> m_receiver_lease_check{};
        std::function<void()> m_background_lease_check{};
        bool m_initialized{};
        bool m_receiver_started{};
        bool m_receiver_completed{};
        bool m_background_started{};
        bool m_background_completed{};

        Fixture &fixture() {
            if (not m_fixture) {
                m_fixture = std::make_unique<Fixture>();
            }

            return *m_fixture;
        }

        void initialize(const lighting::TerrainPrecisionSelection precision = {}) {
            auto &current = fixture();
            if (m_initialized) {
                current.beginDiagnostics();
                current.observePhase("fixture.warm-reuse");
            } else {
                current.initialize(true, precision);
                m_initialized = true;
            }
        }

        void shutdown() {
            if (not m_fixture) {
                return;
            }

            const bool settled = m_initialized and
                                 std::uncaught_exceptions() == 0 and
                                 (not m_receiver_started or m_receiver_completed) and
                                 (not m_background_started or m_background_completed) and
                                 slotFenceComplete(m_fixture->m_fence.completed(), m_fixture->m_fence.lastSubmitted());

            // Failure/unwinding uses the existing queue-idle teardown, without a
            // second success assertion. Keep all retained leases until that proof.
            PPR_DEFER {
                m_fixture->proveIdle();

                m_receiver_lease_check = {};
                m_background_lease_check = {};
                m_fixture->shutdown(not settled or std::uncaught_exceptions() != 0);
                m_fixture.reset();
            };

            // Both successful scenarios keep their generations alive through
            // this single fenced producer shutdown and its original lease checks.
            if (settled) {
                m_fixture->beginDiagnostics();
                m_fixture->observePhase("field.final-shutdown.begin");
                PPR_TEST_ASSERT(not m_fixture->m_pass.shutdown(m_fixture->m_fence.completed()));

                bool checked_lease{};
                if (m_receiver_lease_check) {
                    m_receiver_lease_check();
                    checked_lease = true;
                }

                if (m_background_lease_check) {
                    m_background_lease_check();
                    checked_lease = true;
                }

                if (checked_lease) {
                    m_fixture->observePhase("field.post-shutdown-leases.verdict.pass");
                }
            }
        }
    };

    // Borrowed only while the suite's stack scope recurses these two named
    // leaves. No fixture/service ownership survives a runner loop or filter.
    thread_local TerrainFieldScope *s_terrain_field_scope{};

    TerrainFieldScope &terrainFieldScope_(TerrainFieldScope &isolated) {
        const auto precision = config::getenvLookup("PPR_TEST_TERRAIN_PRODUCTION_FP64");
        const bool private_run = (precision and *precision == "1") or
                                 config::getenvLookup("PPR_TEST_TERRAIN_SPIRV_DIRECTORY").has_value() or
                                 config::getenvLookup("PPR_TEST_TERRAIN_FP64_PROTOTYPE_DIRECTORY").has_value() or
                                 config::getenvLookup("PPR_TEST_FP64_CORPUS_DIRECTORY").has_value();
        return s_terrain_field_scope and not private_run ? *s_terrain_field_scope : isolated;
    }

    lighting::OpticalCell vacuum_() {
        lighting::OpticalCell cell{};
        cell.m_kind = lighting::OpticalKind::vacuum;
        return cell;
    }

    void assertClose_(const lighting::RadianceInterval &actual, const lighting::RadianceInterval &expected) {
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            const double radiance_tolerance = 1e-5 + 1e-4 * std::abs(expected.m_radiance[channel]);
            const double transmission_tolerance = 1e-5 + 1e-4 * std::abs(expected.m_transmittance[channel]);
            PPR_TEST_ASSERT(
                std::isfinite(actual.m_radiance[channel]) and
                std::abs(actual.m_radiance[channel] - expected.m_radiance[channel]) <= radiance_tolerance);
            PPR_TEST_ASSERT(
                actual.m_transmittance[channel] >= 0.0 and
                actual.m_transmittance[channel] <= 1.0 and
                std::abs(actual.m_transmittance[channel] - expected.m_transmittance[channel]) <=
                    transmission_tolerance);
        }
    }

    // ------------------------------------------------------------------
    // test-only double evaluation of the selected RC schedule
    // ------------------------------------------------------------------

    // Independent nonnegative binary16 rounding for this test reference only.
    // Frexp chooses the exact power-of-two quantum; ties explicitly go to even.
    double halfQuantum_(const double value) {
        if (value < std::ldexp(1.0, -14)) {
            return std::ldexp(1.0, -24);
        }

        int exponent{};
        static_cast<void>(std::frexp(value, &exponent));
        return std::ldexp(1.0, std::max(-24, exponent - 11));
    }

    double roundHalf_(const double value) {
        PPR_TEST_ASSERT(std::isfinite(value) and value >= 0.0 and value <= 65504.0);
        const double quantum = halfQuantum_(value);
        const double scaled = value / quantum;
        const double integer = std::floor(scaled);
        const double remainder = scaled - integer;
        const bool up = remainder > 0.5 or (remainder == 0.5 and std::fmod(integer, 2.0) != 0.0);
        return (integer + (up ? 1.0 : 0.0)) * quantum;
    }

    u32 expectedHalfBits_(const double value) {
        const double rounded = roundHalf_(value);
        if (rounded < std::ldexp(1.0, -14)) {
            return static_cast<u32>(std::ldexp(rounded, 24));
        }

        int exponent{};
        static_cast<void>(std::frexp(rounded, &exponent));
        const u32 fraction = static_cast<u32>(rounded / std::ldexp(1.0, exponent - 11)) - 1024u;
        return (static_cast<u32>(exponent + 14) << 10u) | fraction;
    }

    void quantizeInterval_(lighting::RadianceInterval &interval) {
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            interval.m_radiance[channel] = roundHalf_(interval.m_radiance[channel]);
            interval.m_transmittance[channel] = roundHalf_(interval.m_transmittance[channel]);
        }
    }

    void assertStoredClose_(
        const lighting::RadianceInterval &actual, const lighting::RadianceInterval &expected,
        const std::array<u32, 4> *const words = nullptr, const std::size_t record = 0u) {
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            // One adjacent half quantum accommodates FP32/double arithmetic near
            // rounding ties. This is implementation equivalence, not physical GI error.
            const double l_tolerance = halfQuantum_(expected.m_radiance[channel]) +
                                       1e-5 +
                                       1e-4 * expected.m_radiance[channel];
            const double t_tolerance = halfQuantum_(expected.m_transmittance[channel]) + 1.1e-4;
            const bool l_match = std::isfinite(actual.m_radiance[channel]) and
                                 actual.m_radiance[channel] >= 0.0 and
                                 std::abs(actual.m_radiance[channel] - expected.m_radiance[channel]) <= l_tolerance;
            const bool t_match = actual.m_transmittance[channel] >= 0.0 and
                                 actual.m_transmittance[channel] <= 1.0 and
                                 std::abs(actual.m_transmittance[channel] - expected.m_transmittance[channel]) <=
                                     t_tolerance;
            if (not l_match or not t_match) {
                std::println(
                    "PACK DIAG RC record={} channel={} Lactual={} Lexpected={} Tactual={} Texpected={} Ltol={} Ttol={} expected_half_L={:04x} expected_half_T={:04x}",
                    record, channel, actual.m_radiance[channel], expected.m_radiance[channel],
                    actual.m_transmittance[channel], expected.m_transmittance[channel], l_tolerance, t_tolerance,
                    expectedHalfBits_(expected.m_radiance[channel]),
                    expectedHalfBits_(expected.m_transmittance[channel]));
                if (words != nullptr) {
                    std::println(
                        "PACK DIAG RC raw={:08x},{:08x},{:08x},{:08x} actual_fp32_L={:08x} actual_fp32_T={:08x} expected_fp32_L={:08x} expected_fp32_T={:08x}",
                        (*words)[0], (*words)[1], (*words)[2], (*words)[3],
                        std::bit_cast<u32>(static_cast<float>(actual.m_radiance[channel])),
                        std::bit_cast<u32>(static_cast<float>(actual.m_transmittance[channel])),
                        std::bit_cast<u32>(static_cast<float>(expected.m_radiance[channel])),
                        std::bit_cast<u32>(static_cast<float>(expected.m_transmittance[channel])));
                }
            }

            PPR_TEST_ASSERT(
                std::isfinite(actual.m_radiance[channel]) and
                actual.m_radiance[channel] >= 0.0 and
                std::abs(actual.m_radiance[channel] - expected.m_radiance[channel]) <= l_tolerance);
            PPR_TEST_ASSERT(
                actual.m_transmittance[channel] >= 0.0 and
                actual.m_transmittance[channel] <= 1.0 and
                std::abs(actual.m_transmittance[channel] - expected.m_transmittance[channel]) <= t_tolerance);
        }
    }

    struct CpuCascadeFixture {
        lighting::RcResourceEstimate m_layout{};
        std::vector<std::vector<lighting::RadianceInterval>> m_intervals{};
        std::vector<std::vector<std::array<double, 2>>> m_directions{};
        u64 m_connector_queries{};
        u64 m_top_interval_queries{};
        u64 m_coarse_reads{};
    };

    lighting::RcEstimateRequest cascadeRequest_(const u32 width, const u32 height) {
        lighting::RcEstimateRequest request{};
        request.m_interval_bytes = 32u;
        request.m_optical_cell_bytes = 40u;
        request.m_width = width;
        request.m_height = height;
        request.m_c0_tile_width = width;
        request.m_c0_tile_height = height;
        request.m_live_generations = 1u;
        request.m_cascade_count = 1u;
        u64 outer = 1u;

        while (double(outer) < std::hypot(double(width), double(height))) {
            outer *= 2u;
            ++request.m_cascade_count;
        }

        // Fixture resource allowance, not a lighting/product budget.
        request.m_budget_bytes = 64u * 1024u * 1024u;
        request.m_max_resource_bytes = 32u * 1024u * 1024u;
        request.m_additional_peak_bytes = 1024u * 1024u;
        return request;
    }

    lighting::RadianceInterval connector_(
        const lighting::OpticalGridView &grid, const std::array<double, 2> start, const std::array<double, 2> end,
        const lighting::OpticalTerrainSnapshot *const terrain = nullptr) {
        const std::array<double, 2> delta{end[0] - start[0], end[1] - start[1]};
        const double length = std::hypot(delta[0], delta[1]);
        if (terrain != nullptr) {
            const auto result = lighting::traceOpticalTerrainConnectorReference(*terrain, start, end);
            PPR_TEST_ASSERT(result.has_value());
            return result->m_interval;
        }

        if (length == 0.0) {
            return {};
        }

        const lighting::OpticalRay ray{
            {grid.m_min_m[0] + start[0] * grid.m_cell_size_m, grid.m_min_m[1] + start[1] * grid.m_cell_size_m}, delta,
            0.0, length * grid.m_cell_size_m};
        const auto result = lighting::traceOpticalReference(grid, ray);
        PPR_TEST_ASSERT(result.has_value());
        return *result;
    }

    CpuCascadeFixture solveCpuCascades_(
        const lighting::OpticalGridView &grid, const lighting::RcEstimateRequest &request,
        const bool wrong_child_start = false, const bool half_storage = false,
        const lighting::OpticalTerrainSnapshot *const terrain = nullptr, const std::size_t minimum_level = 0u) {
        CpuCascadeFixture result{};
        const auto layout = lighting::estimateRcResources(request);
        PPR_TEST_ASSERT(layout.has_value());
        result.m_layout = *layout;
        result.m_intervals.resize(request.m_cascade_count);
        result.m_directions.resize(request.m_cascade_count);

        for (std::size_t level = 0u; level < result.m_layout.m_cascades.size(); ++level) {
            const auto &cascade = result.m_layout.m_cascades[level];
            result.m_intervals[level].resize(static_cast<std::size_t>(cascade.m_resident_records));
            for (u64 bin = 0u; bin < cascade.m_directions; ++bin) {
                const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / double(cascade.m_directions);
                // Exactly the uploaded center table, evaluated by double transport.
                result.m_directions[level].push_back({double(float(std::cos(angle))), double(float(std::sin(angle)))});
            }
        }

        for (std::size_t level = result.m_intervals.size(); level-- > minimum_level;) {
            const auto &cascade = result.m_layout.m_cascades[level];
            const u64 width = cascade.m_padded_probes[0];
            const u64 height = cascade.m_padded_probes[1];
            for (u64 y = 0u; y < height; ++y) {
                for (u64 x = 0u; x < width; ++x) {
                    const std::array<double, 2> position{
                        0.5 + double(x * cascade.m_spacing_cells), 0.5 + double(y * cascade.m_spacing_cells)};

                    for (u64 bin = 0u; bin < cascade.m_directions; ++bin) {
                        const auto &direction = result.m_directions[level][static_cast<std::size_t>(bin)];
                        auto &output = result.m_intervals[level][static_cast<std::size_t>(
                            (y * width + x) * cascade.m_directions + bin)];
                        if (level + 1u == result.m_intervals.size()) {
                            ++result.m_top_interval_queries;
                            const lighting::OpticalRay ray{
                                {grid.m_min_m[0] + position[0] * grid.m_cell_size_m,
                                    grid.m_min_m[1] + position[1] * grid.m_cell_size_m},
                                direction, double(cascade.m_near_cells) * grid.m_cell_size_m,
                                double(result.m_layout.m_terminal_far_cells) * grid.m_cell_size_m};
                            if (terrain != nullptr) {
                                const auto traced = lighting::traceOpticalTerrainParametricReference(
                                    *terrain, {position, direction, double(cascade.m_near_cells),
                                                  2.0 * double(result.m_layout.m_terminal_far_cells)});
                                PPR_TEST_ASSERT(traced.has_value());
                                output = traced->m_interval;
                            } else {
                                const auto traced = lighting::traceOpticalReference(grid, ray);
                                PPR_TEST_ASSERT(traced.has_value());
                                output = *traced;
                            }
                            if (half_storage) {
                                quantizeInterval_(output);
                            }

                            continue;
                        }

                        const auto &parent = result.m_layout.m_cascades[level + 1u];
                        output.m_transmittance = {};

                        for (u64 child = 0u; child < 2u; ++child) {
                            const u64 upper_bin = 2u * bin + child;
                            const auto &child_direction =
                                result.m_directions[level + 1u][static_cast<std::size_t>(upper_bin)];
                            const auto &start_direction = wrong_child_start ? child_direction : direction;
                            const std::array<double, 2> start{
                                position[0] + double(cascade.m_near_cells) * start_direction[0],
                                position[1] + double(cascade.m_near_cells) * start_direction[1]};

                            for (u64 upper_y = 0u; upper_y < 2u; ++upper_y) {
                                for (u64 upper_x = 0u; upper_x < 2u; ++upper_x) {
                                    const double fraction_x = (x % 2u == 0u ? 0.0 : 0.5);
                                    const double fraction_y = (y % 2u == 0u ? 0.0 : 0.5);
                                    const double weight = (upper_x == 0u ? 1.0 - fraction_x : fraction_x) *
                                                          (upper_y == 0u ? 1.0 - fraction_y : fraction_y);
                                    if (weight == 0.0) {
                                        continue;
                                    }

                                    const u64 px = x / 2u + (upper_x == 0u ? 0u : x % 2u);
                                    const u64 py = y / 2u + (upper_y == 0u ? 0u : y % 2u);
                                    PPR_TEST_ASSERT(px < parent.m_padded_probes[0] and py < parent.m_padded_probes[1]);
                                    const std::array<double, 2> end{
                                        0.5 +
                                            double(px * parent.m_spacing_cells) +
                                            double(parent.m_near_cells) * child_direction[0],
                                        0.5 +
                                            double(py * parent.m_spacing_cells) +
                                            double(parent.m_near_cells) * child_direction[1]};
                                    ++result.m_connector_queries;
                                    const auto near = connector_(grid, start, end, terrain);
                                    ++result.m_coarse_reads;
                                    const auto &far = result.m_intervals[level + 1u][static_cast<std::size_t>(
                                        (py * parent.m_padded_probes[0] + px) * parent.m_directions + upper_bin)];
                                    const auto composed = lighting::composeIntervals(near, far);
                                    PPR_TEST_ASSERT(composed.has_value());
                                    for (std::size_t channel = 0u; channel < 3u; ++channel) {
                                        output.m_radiance[channel] += 0.5 * weight * composed->m_radiance[channel];
                                        output.m_transmittance[channel] += 0.5 *
                                                                           weight *
                                                                           composed->m_transmittance[channel];
                                    }
                                }
                            }
                        }

                        if (half_storage) {
                            quantizeInterval_(output);
                        }
                    }
                }
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    // exterior receiver production, independent reconstruction, and leases
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // actual Grid linear-light continuation qualification
    // ------------------------------------------------------------------

    struct ReceiverProbe {
        float m_x{}, m_y{};
        u32 m_material{17u}, m_reserved{};
        float m_normal_x{}, m_normal_y{}, m_normal_z{1.0f};
        u32 m_operation{};
    };
    static_assert(sizeof(ReceiverProbe) == 32u and alignof(ReceiverProbe) == 4u);
    static_assert(std::is_standard_layout_v<ReceiverProbe> and std::is_trivially_copyable_v<ReceiverProbe>);
    static_assert(PPR_OFFSETOF(ReceiverProbe, m_x) == 0u and PPR_OFFSETOF(ReceiverProbe, m_y) == 4u);
    static_assert(PPR_OFFSETOF(ReceiverProbe, m_material) == 8u);
    static_assert(PPR_OFFSETOF(ReceiverProbe, m_reserved) == 12u);
    static_assert(PPR_OFFSETOF(ReceiverProbe, m_normal_x) == 16u and PPR_OFFSETOF(ReceiverProbe, m_operation) == 28u);
    static_assert(PPR_OFFSETOF(ReceiverProbe, m_normal_y) == 20u and PPR_OFFSETOF(ReceiverProbe, m_normal_z) == 24u);

    struct ReceiverDirection {
        float m_x{}, m_y{};
    };
    static_assert(sizeof(ReceiverDirection) == 8u and alignof(ReceiverDirection) == 4u);
    static_assert(PPR_OFFSETOF(ReceiverDirection, m_y) == 4u);

    struct ReceiverLookupFixture {
        Fixture m_base{};
        shader::SharedModule m_module{};
        shader::ComPtr<slang::IEntryPoint> m_entry{};
        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};

        void initialize() {
            m_base.initialize(false); // No production radiance pipeline families.

            PPR_TEST_ASSERT(not m_base.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders/grid_tiles.slang", "grid_receiver_lookup",
                m_module.writeRef()));
            PPR_TEST_ASSERT(not make_error_code(
                m_module->findEntryPointByName("qualifyGridReceiverContinuation", m_entry.writeRef())));

            auto *const layout = m_module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (const auto &[name, size]: std::array<std::pair<const char *, std::size_t>, 4u>{
                     std::pair{"g_grid_receiver_probe_inputs", sizeof(ReceiverProbe)},
                     {"g_grid_receiver_probe_outputs", 16u}, {"g_grid_receiver_probe_params", 16u},
                     {"g_grid_receivers", 48u}}) {
                bool found = false;
                for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                    auto *const field = layout->getParameterByIndex(index);
                    if (std::string_view{field->getName()} != name) {
                        continue;
                    }

                    auto *const element = field->getTypeLayout()->getElementTypeLayout();
                    PPR_TEST_ASSERT(element != nullptr);
                    PPR_TEST_ASSERT(element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == size);
                    found = true;
                    break;
                }

                PPR_TEST_ASSERT(found);
            }

            slang::IComponentType *entries[]{m_entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = m_module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics{};
            PPR_TEST_ASSERT(not make_error_code(m_base.m_rhi->getDevice().createShaderProgram(
                description, m_program.writeRef(), diagnostics.writeRef())));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = m_program;
            PPR_TEST_ASSERT(
                not make_error_code(m_base.m_rhi->getDevice().createComputePipeline(pipeline, m_pipeline.writeRef())));
        }

        void shutdown() {
            if (m_base.m_queue and make_error_code(m_base.m_queue->waitOnHost())) {
                hal::fatalExit("receiver-lookup-completion-unknown");
            }

            m_pipeline.setNull();
            m_program.setNull();
            m_entry.setNull();
            m_module = {};

            m_base.shutdown();
        }

        std::vector<std::array<float, 4u>> lookup(
            const lighting::TerrainReceiverSet &receivers, const std::span<const lighting::TerrainReceiverResult> field,
            const std::span<const ReceiverProbe> probes, const u32 mode,
            const std::span<const lighting::TerrainReceiverAngular> angular = {},
            const std::span<const ReceiverDirection> directions = {}) {
            PPR_TEST_ASSERT(not probes.empty() and probes.size() < 65536u);
            PPR_TEST_ASSERT(field.size() == receivers.features().size());
            auto &device = m_base.m_rhi->getDevice();
            const auto view = receivers.geometry()->view();
            PPR_TEST_ASSERT(
                view.m_width <= 128u and view.m_height <= 128u and view.m_min_m[0u] == 0.0 and view.m_min_m[1u] == 0.0);

            std::vector<rhi::ComPtr<rhi::IBuffer>> buffers{};
            rhi::ComPtr<rhi::IBuffer> output{}, readback{};
            rhi::ComPtr<rhi::IShaderObject> root{};
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            rhi::ComPtr<rhi::ICommandBuffer> commands{};
            PPR_DEFER {
                if (make_error_code(m_base.m_queue->waitOnHost())) {
                    hal::fatalExit("receiver-lookup-buffer-completion-unknown");
                }
            };

            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(m_program, root.writeRef())));
            rhi::ShaderCursor cursor{root};
            const auto upload = [&](const char *const name, const void *const data, const u64 bytes, const u32 stride) {
                PPR_TEST_ASSERT(bytes != 0u);

                rhi::BufferDesc description{};
                description.size = bytes;
                description.elementSize = stride;
                description.memoryType = rhi::MemoryType::Upload;
                description.usage = rhi::BufferUsage::ShaderResource;
                description.defaultState = rhi::ResourceState::ShaderResource;
                rhi::ComPtr<rhi::IBuffer> buffer{};
                PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, data, buffer.writeRef())));
                PPR_TEST_ASSERT(buffer->getDesc().size == bytes and buffer->getDesc().elementSize == stride);

                buffers.push_back(buffer);
                PPR_TEST_ASSERT(
                    not make_error_code(cursor[name].setBinding(rhi::Binding(buffer, {.offset = 0u, .size = bytes}))));
            };

            upload(
                "g_lighting_features", receivers.features().data(), receivers.features().size_bytes(),
                sizeof(lighting::TerrainReceiverFeature));
            upload("g_lighting_results", field.data(), field.size_bytes(), sizeof(lighting::TerrainReceiverResult));
            upload(
                "g_lighting_cells", receivers.cells().data(), receivers.cells().size_bytes(),
                sizeof(lighting::TerrainReceiverCellRange));
            upload(
                "g_lighting_components", receivers.components().data(), receivers.components().size_bytes(),
                sizeof(lighting::TerrainReceiverComponentRange));
            upload(
                "g_lighting_candidates", receivers.candidates().data(), receivers.candidates().size_bytes(),
                sizeof(u32));
            PPR_TEST_ASSERT(angular.empty() or (angular.size() == field.size() * 16u and directions.size() == 16u));
            const ReceiverDirection unused_direction{1.0f, 0.0f};
            upload(
                "g_lighting_angular", angular.empty() ? static_cast<const void *>(field.data()) : angular.data(),
                angular.empty() ? field.size_bytes() : angular.size_bytes(), sizeof(lighting::TerrainReceiverAngular));
            upload(
                "g_lighting_directions", directions.empty() ? &unused_direction : directions.data(),
                directions.empty() ? sizeof(unused_direction) : directions.size_bytes(), sizeof(ReceiverDirection));
            upload("g_grid_receiver_probe_inputs", probes.data(), probes.size_bytes(), sizeof(ReceiverProbe));
            const auto points = math::contourPoints();
            upload("g_contour_points", points.data(), points.size_bytes(), sizeof(math::ContourPoint));

            std::vector<u32> words(8192u, 0u);
            for (u32 y = 0u; y < view.m_height; ++y) {
                for (u32 x = 0u; x < view.m_width; ++x) {
                    const u32 index = y * 128u + x;
                    words[index / 2u] |= u32{view.m_labels[y * view.m_width + x]} << ((index & 1u) * 16u);
                }
            }

            std::array<u32, 1024u> slots{};
            slots.fill(0xffffffffu);
            slots[0u] = 0u;
            upload("g_cell_materials", words.data(), words.size() * sizeof(u32), sizeof(u32));
            upload("g_chunk_slots", slots.data(), sizeof(slots), sizeof(u32));

            const std::array<u32, 12u> receiver_parameters{
                mode, view.m_width, view.m_height, static_cast<u32>(field.size()), 0u, 0u,
                static_cast<u32>(receivers.components().size()), static_cast<u32>(receivers.candidates().size()),
                angular.empty() ? 0u : 1u, 0u, angular.empty() ? 0u : 16u, 0u};
            static_assert(sizeof(receiver_parameters) == 48u);
            const std::array<u32, 4u> probe_parameters{static_cast<u32>(probes.size()), 0u, 0u, 0u};
            GridPass::GridFrame frame{};
            frame.m_boundary_material_id = 0u;

            rhi::ShaderCursor constants{};
            PPR_TEST_ASSERT(not make_error_code(cursor["g_grid_frame"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_grid_receivers"].getDereferenced(constants)));
            PPR_TEST_ASSERT(
                not make_error_code(constants.setData(receiver_parameters.data(), sizeof(receiver_parameters))));
            PPR_TEST_ASSERT(not make_error_code(cursor["g_grid_receiver_probe_params"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(probe_parameters.data(), sizeof(probe_parameters))));

            std::vector<std::array<float, 4u>> rows(probes.size());
            const u64 bytes = rows.size() * sizeof(rows[0]);
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = sizeof(rows[0]);
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
            description.defaultState = rhi::ResourceState::UnorderedAccess;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, output.writeRef())));

            description.memoryType = rhi::MemoryType::ReadBack;
            description.usage = rhi::BufferUsage::CopyDestination;
            description.defaultState = rhi::ResourceState::CopyDestination;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, readback.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(
                cursor["g_grid_receiver_probe_outputs"].setBinding(
                    rhi::Binding(output, {.offset = 0u, .size = bytes}))));

            PPR_TEST_ASSERT(not make_error_code(m_base.m_queue->createCommandEncoder(encoder.writeRef())));
            for (const auto &buffer: buffers) {
                encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);
            }

            encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(readback, rhi::ResourceState::CopyDestination);
            auto *const pass = encoder->beginComputePass();
            PPR_TEST_ASSERT(pass != nullptr);
            pass->bindPipeline(m_pipeline, root);
            pass->dispatchCompute((static_cast<u32>(probes.size()) + 63u) / 64u, 1u, 1u);
            pass->end();

            encoder->globalBarrier();
            encoder->setBufferState(output, rhi::ResourceState::CopySource);
            encoder->copyBuffer(readback, 0u, output, 0u, bytes);
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));

            rhi::ICommandBuffer *command = commands.get();
            rhi::IFence *signal = m_base.m_fence.fence();
            const auto submitted = m_base.m_fence.nextValue();
            const u64 raw_submitted = submitted;
            rhi::SubmitDesc submission{};
            submission.commandBuffers = &command;
            submission.commandBufferCount = 1u;
            submission.signalFences = &signal;
            submission.signalFenceValues = &raw_submitted;
            submission.signalFenceCount = 1u;
            PPR_TEST_ASSERT(not make_error_code(m_base.m_queue->submit(submission)));

            m_base.m_fence.commitSubmitted(submitted);
            PPR_TEST_ASSERT(not m_base.m_fence.waitFor(submitted));

            void *mapped{};
            PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
            std::memcpy(rows.data(), mapped, static_cast<std::size_t>(bytes));
            PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));
            return rows;
        }
    };

    std::shared_ptr<const lighting::TerrainReceiverSet> receiverLookupShape_(
        const std::span<const u16> labels, const u32 width, const u32 height) {
        const std::array<lighting::OpticalTerrainMaterial, 3u> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f},
            {lighting::OpticalCell{{}, {}, {}, lighting::OpticalKind::opaque}, 17u, 0.9f},
            {lighting::OpticalCell{{}, {}, {1.0, 0.0, 0.0}, lighting::OpticalKind::opaque}, 23u, 0.9f}};
        const auto shape = lighting::OpticalTerrainSnapshot::capture(
            {labels, materials, {}, 1.0, width, height, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(shape.has_value());

        const auto receivers = lighting::TerrainReceiverSet::capture(
            std::make_shared<const lighting::OpticalTerrainSnapshot>(*shape));
        PPR_TEST_ASSERT(receivers.has_value());
        return std::make_shared<const lighting::TerrainReceiverSet>(*receivers);
    }

    std::array<double, 3u> receiverMeanReference_(
        const lighting::TerrainReceiverSet &receivers, const std::span<const lighting::TerrainReceiverResult> field,
        const ReceiverProbe &probe, const bool interpolate) {
        const auto view = receivers.geometry()->view();
        const i32 x = static_cast<i32>(std::floor(probe.m_x - 0.5f));
        const i32 y = static_cast<i32>(std::floor(probe.m_y - 0.5f));
        const auto cell = receivers.cells()[u32(y) * (view.m_width - 1u) + u32(x)];
        u32 mask{};
        const auto points = math::contourPoints();
        for (u32 corner = 0u; corner < 4u; ++corner) {
            const u32 index = (u32(y) + u32(points[corner].m_y)) * view.m_width + u32(x) + u32(points[corner].m_x);
            if (view.m_labels[index] == probe.m_material) {
                mask |= 1u << corner;
            }
        }

        u32 component = 0xffffffffu;
        for (const auto &range: receivers.components().subspan(cell.m_offset, cell.m_count)) {
            if ((range.m_corner_mask & mask) != 0u) {
                component = range.m_component;
            }
        }

        const auto score = [&](const lighting::TerrainReceiverFeature &feature) {
            const double dx = double(probe.m_x) - double(feature.m_visual_x);
            const double dy = double(probe.m_y) - double(feature.m_visual_y);
            return dx * dx + dy * dy;
        };

        // Independent double kernel scans ALL component sites, never the pruned
        // CPU candidate lists or a shared shader weight helper.
        double best = std::numeric_limits<double>::infinity();
        u32 winner = 0xffffffffu;
        for (u32 key = 0u; key < field.size(); ++key) {
            if (receivers.features()[key].m_component != component) {
                continue;
            }

            const double distance = score(receivers.features()[key]);
            if (distance < best) {
                best = distance;
                winner = key;
            }
        }

        if (winner == 0xffffffffu) {
            return {};
        }

        if (not interpolate or best == 0.0) {
            return {double(field[winner].m_mean[0]), double(field[winner].m_mean[1]), double(field[winner].m_mean[2])};
        }

        const double radius = std::sqrt(best) + 0.5;
        const auto &anchor = receivers.features()[winner];
        const std::array<double, 2u> anchor_outward{
            double(std::bit_cast<float>(anchor.m_reserved[0])), double(std::bit_cast<float>(anchor.m_reserved[1]))};
        std::array<double, 3u> total{};
        double measure{};
        for (u32 key = 0u; key < field.size(); ++key) {
            if (receivers.features()[key].m_component != component) {
                continue;
            }

            const auto &feature = receivers.features()[key];
            const double facing = double(std::bit_cast<float>(feature.m_reserved[0])) * anchor_outward[0] +
                                  double(std::bit_cast<float>(feature.m_reserved[1])) * anchor_outward[1];
            if (facing <= 0.0) {
                continue;
            }

            const double distance = score(receivers.features()[key]);
            if (distance >= radius * radius) {
                continue;
            }

            const double inverse = best / distance;
            const double taper = (radius * radius - distance) / (radius * radius - best);
            const double weight = inverse * inverse * taper * taper;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                total[channel] += weight * double(field[key].m_mean[channel]);
            }

            measure += weight;
        }

        for (auto &value: total) {
            value /= measure;
        }

        return total;
    }

    // ------------------------------------------------------------------
    // directional diffuse lift against independent elevation quadrature
    // ------------------------------------------------------------------

    double receiverLiftQuadrature_(const ReceiverProbe &probe, const ReceiverDirection direction) {
        const double length = std::hypot(double(direction.m_x), double(direction.m_y));
        const double A = (double(probe.m_normal_x) * double(direction.m_x) +
                             double(probe.m_normal_y) * double(direction.m_y)) /
                         length;
        const double B = double(probe.m_normal_z);
        const auto integrand = [&](const double elevation) {
            const double cosine = std::cos(elevation);
            return 2.0 * std::max(A * cosine + B * std::sin(elevation), 0.0) * cosine;
        };

        double begin = 0.0;
        const double end = std::numbers::pi / 2.0;
        if (A < 0.0) {
            double low = 0.0, high = end;
            for (u32 iteration = 0u; iteration < 80u; ++iteration) {
                const double middle = (low + high) * 0.5;
                if (A * std::cos(middle) + B * std::sin(middle) > 0.0)
                    high = middle;
                else
                    low = middle;
            }

            begin = high;
        }

        constexpr u32 panels = 2048u;
        const double step = (end - begin) / double(panels);
        double sum = integrand(begin) + integrand(end);
        for (u32 panel = 1u; panel < panels; ++panel) {
            sum += (panel % 2u == 0u ? 2.0 : 4.0) * integrand(begin + double(panel) * step);
        }

        return sum * step / 3.0;
    }

    PPR_UNIT_TEST (mapped_diffuse_uses_actual_angular_field_and_elevation_lift) {
        ReceiverLookupFixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        std::array<u16, 49u> labels{};
        for (u32 y = 2u; y < 5u; ++y) {
            for (u32 x = 2u; x < 5u; ++x)
                labels[y * 7u + x] = 17u;
        }

        const auto receivers = receiverLookupShape_(labels, 7u, 7u);
        std::array<ReceiverDirection, 16u> directions{};
        for (u32 bin = 0u; bin < 16u; ++bin) {
            const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / 16.0;
            directions[bin] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }

        std::vector<lighting::TerrainReceiverResult> means(receivers->features().size());
        std::vector<ReceiverProbe> coefficient_probes;
        for (const double B: {0.0, 0.0001, 0.01, 0.125, 0.25, 0.5, 0.75, 1.0}) {
            for (u32 azimuth = 0u; azimuth < 16u; ++azimuth) {
                // Vary the relative phase within one direction bin rather
                // than permuting one offset. Rows0/1 are respectively halfway
                // between uploaded bins and aligned with an uploaded bin.
                const double within_bin = double(azimuth / 2u) / 16.0 + ((azimuth & 1u) != 0u ? 0.5 : 0.0);
                const double angle = 2.0 * std::numbers::pi * (double(azimuth) + within_bin) / 16.0;
                const double radius = std::sqrt(1.0 - B * B);
                for (const auto direction: directions) {
                    ReceiverProbe probe{};
                    probe.m_x = direction.m_x;
                    probe.m_y = direction.m_y;
                    probe.m_normal_x = static_cast<float>(radius * std::cos(angle));
                    probe.m_normal_y = static_cast<float>(radius * std::sin(angle));
                    probe.m_normal_z = static_cast<float>(B);
                    probe.m_operation = 1u;
                    coefficient_probes.push_back(probe);
                }
            }
        }

        const auto coefficients = fixture.lookup(*receivers, means, coefficient_probes, 1u);
        double maximum_error{}, maximum_constant_error{};
        for (u32 normal = 0u; normal < coefficient_probes.size() / 16u; ++normal) {
            double constant{};
            for (u32 bin = 0u; bin < 16u; ++bin) {
                const u32 index = normal * 16u + bin;
                const auto &probe = coefficient_probes[index];
                const double expected = receiverLiftQuadrature_(probe, directions[bin]);
                const double actual = double(coefficients[index][0u]);
                PPR_TEST_ASSERT(std::isfinite(actual) and actual >= 0.0);
                maximum_error = std::max(maximum_error, std::abs(actual - expected));
                PPR_TEST_ASSERT(std::abs(actual - expected) <= 1e-5);
                constant += actual / 16.0;
            }

            const double target = (1.0 + double(coefficient_probes[normal * 16u].m_normal_z)) * 0.5;
            maximum_constant_error = std::max(maximum_constant_error, std::abs(constant - target));
            PPR_TEST_ASSERT(std::abs(constant - target) <= 0.01);
        }

        std::println(
            "MAPPED_DIFFUSE coefficient_abs_error={} constant16_abs_error={} normals={}", maximum_error,
            maximum_constant_error, coefficient_probes.size() / 16u);

        std::vector<lighting::TerrainReceiverAngular> angular(means.size() * 16u);
        for (u32 key = 0u; key < means.size(); ++key) {
            for (u32 bin = 0u; bin < 16u; ++bin) {
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const float value = float(1u + (key * (channel + 3u) + bin * (channel + 1u)) % 17u);
                    angular[key * 16u + bin].m_radiance[channel] = value;
                    means[key].m_mean[channel] += value / 16.0f;
                }
            }
        }

        std::array<ReceiverProbe, 3u> probes{
            ReceiverProbe{receivers->features()[0u].m_visual_x, receivers->features()[0u].m_visual_y}, {3.2f, 3.1f},
            {3.5f, 3.25f}};
        const auto flat = fixture.lookup(*receivers, means, probes, 2u);
        const auto front = fixture.lookup(*receivers, means, probes, 2u, angular, directions);
        for (u32 index = 0u; index < probes.size(); ++index) {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::bit_cast<u32>(front[index][channel]) == std::bit_cast<u32>(flat[index][channel]));
            }
        }

        for (auto &probe: probes) {
            probe.m_normal_x = 0.6f;
            probe.m_normal_z = 0.8f;
        }

        const auto mapped = fixture.lookup(*receivers, means, probes, 2u, angular, directions);
        for (u32 index = 0u; index < probes.size(); ++index) {
            std::array<double, 3u> expected{};
            for (u32 bin = 0u; bin < 16u; ++bin) {
                std::vector<lighting::TerrainReceiverResult> bin_field(means.size());
                for (u32 key = 0u; key < means.size(); ++key) {
                    for (u32 channel = 0u; channel < 3u; ++channel)
                        bin_field[key].m_mean[channel] = angular[key * 16u + bin].m_radiance[channel];
                }

                const auto incident = receiverMeanReference_(*receivers, bin_field, probes[index], true);
                const double coefficient = receiverLiftQuadrature_(probes[index], directions[bin]);
                for (u32 channel = 0u; channel < 3u; ++channel)
                    expected[channel] += incident[channel] * coefficient / 16.0;
            }

            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    std::abs(double(mapped[index][channel]) - expected[channel]) <=
                    2e-4 * std::max(1.0, expected[channel]));
            }
        }

        // Retain the same facing neighbourhood for directional bins as means.
        std::array<u16, 99u> wall_labels{};
        for (u32 y = 1u; y < 8u; ++y)
            wall_labels[y * 11u + 5u] = 17u;
        const auto wall = receiverLookupShape_(wall_labels, 11u, 9u);
        std::vector<lighting::TerrainReceiverResult> wall_means(wall->features().size());
        std::vector<lighting::TerrainReceiverAngular> wall_angular(wall_means.size() * 16u);
        for (u32 key = 0u; key < wall_means.size(); ++key) {
            const u32 channel = std::bit_cast<float>(wall->features()[key].m_reserved[0]) < 0.0f ? 0u : 2u;
            wall_means[key].m_mean[channel] = 1.0f;
            for (u32 bin = 0u; bin < 16u; ++bin)
                wall_angular[key * 16u + bin].m_radiance[channel] = 1.0f;
        }

        const std::array<ReceiverProbe, 2u> sides{
            ReceiverProbe{5.15f, 4.5f, 17u, 0u, 0.6f, 0.0f, 0.8f}, {5.65f, 4.5f, 17u, 0u, 0.6f, 0.0f, 0.8f}};
        const auto side_rows = fixture.lookup(*wall, wall_means, sides, 2u, wall_angular, directions);
        PPR_TEST_ASSERT(side_rows[0u][0u] > 0.0f and side_rows[0u][2u] == 0.0f);
        PPR_TEST_ASSERT(side_rows[1u][2u] > 0.0f and side_rows[1u][0u] == 0.0f);
    };

    PPR_UNIT_TEST (terrain_receiver_continuation_kernel_preserves_sites_and_cell_seams) {
        ReceiverLookupFixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        std::array<u16, 49u> labels{};
        for (u32 y = 2u; y < 5u; ++y) {
            for (u32 x = 2u; x < 5u; ++x) {
                labels[y * 7u + x] = 17u;
            }
        }

        const auto receivers = receiverLookupShape_(labels, 7u, 7u);
        std::vector<lighting::TerrainReceiverResult> field(receivers->features().size());
        std::vector<ReceiverProbe> probes{};
        for (const auto &feature: receivers->features()) {
            probes.push_back({feature.m_visual_x, feature.m_visual_y, 17u, 0u});
        }

        constexpr float step = 1.0f / 65536.0f;
        probes.insert(
            probes.end(), {{3.5f - step, 3.25f}, {3.5f + step, 3.25f}, {3.25f, 3.5f - step}, {3.25f, 3.5f + step}});

        for (auto &row: field) {
            row.m_mean[0u] = 1.0f;
            row.m_mean[1u] = 2.0f;
            row.m_mean[2u] = 4.0f;
        }

        const auto constant = fixture.lookup(*receivers, field, probes, 2u);

        for (const auto &row: constant) {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(std::abs(row[channel] - float(1u << channel)) <= 1e-5f);
            }
        }

        for (u32 key = 0u; key < field.size(); ++key) {
            field[key].m_mean[0u] = float((key * 7u) % 19u);
            field[key].m_mean[1u] = float((key * 3u) % 13u);
            field[key].m_mean[2u] = float((key * 11u) % 23u);
        }

        for (const u32 mode: {1u, 2u}) {
            const auto rows = fixture.lookup(*receivers, field, probes, mode);

            for (u32 index = 0u; index < probes.size(); ++index) {
                const auto expected = receiverMeanReference_(*receivers, field, probes[index], mode == 2u);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(std::isfinite(rows[index][channel]) and rows[index][channel] >= 0.0f);
                    PPR_TEST_ASSERT(
                        std::abs(double(rows[index][channel]) - expected[channel]) <=
                        2e-4 * std::max(1.0, expected[channel]));

                    if (index < field.size()) {
                        PPR_TEST_ASSERT(rows[index][channel] == field[index].m_mean[channel]);
                    }
                }
            }

            if (mode == 2u) {
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        const u32 begin = static_cast<u32>(field.size()) + axis * 2u;
                        PPR_TEST_ASSERT(std::abs(rows[begin][channel] - rows[begin + 1u][channel]) <= 0.01f);
                    }
                }
            }
        }

        // A single solid component has two opposed wall faces. The nearest
        // anchor selects its side even on either side of the medial line;
        // the admitted discontinuity must not mix their distinct RGB data.
        std::array<u16, 99u> wall_labels{};
        for (u32 y = 1u; y < 8u; ++y) {
            wall_labels[y * 11u + 5u] = 17u;
        }

        const auto wall = receiverLookupShape_(wall_labels, 11u, 9u);
        std::vector<lighting::TerrainReceiverResult> wall_field(wall->features().size());
        for (u32 key = 0u; key < wall_field.size(); ++key) {
            const float outward_x = std::bit_cast<float>(wall->features()[key].m_reserved[0]);
            wall_field[key].m_mean[outward_x < 0.0f ? 0u : 2u] = 1.0f;
        }

        const std::array<ReceiverProbe, 4u> side_probes{
            ReceiverProbe{5.15f, 4.5f}, {5.65f, 4.5f}, {5.5f - step, 4.5f}, {5.5f + step, 4.5f}};
        const auto sides = fixture.lookup(*wall, wall_field, side_probes, 2u);
        for (u32 index = 0u; index < side_probes.size(); ++index) {
            const bool left = (index & 1u) == 0u;
            PPR_TEST_ASSERT(std::abs(sides[index][left ? 0u : 2u] - 1.0f) <= 1e-5f);
            PPR_TEST_ASSERT(sides[index][left ? 2u : 0u] == 0.0f);
        }
    };

    PPR_UNIT_TEST (terrain_receiver_continuation_experiment_respects_blocked_side_quality_gate) {
        ReceiverLookupFixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        // Masks/normalization are fixed before observations. The wall is one
        // sample/cell thick, x=5, y=1..7. Back-half probes x>5.5 exclude both
        // endcaps; bright controls lie in its left half. Portal cases use only
        // y=2.5/6.5 blocked surfaces, away from the1x1 opening and its endcaps.
        // All observations are actual Grid lookup LINEAR incident mean, before
        // albedo/S/opacity/exposure. This is a reconstruction experiment.
        bool physical_gate = true;
        std::array<std::array<double, 2u>, 2u> portal_edges{{{-1.0, -1.0}, {-1.0, -1.0}}};
        for (u32 scenario = 0u; scenario < 3u; ++scenario) {
            const bool portal = scenario != 0u, open = scenario == 2u;
            std::array<u16, 99u> labels{};
            for (u32 y = 1u; y < 8u; ++y) {
                labels[y * 11u + 5u] = open and y == 4u ? 0u : 17u;
            }

            if (portal) {
                labels[4u * 11u + 2u] = 23u; // Vacuum-isolated actual opaque source.
            }

            const auto receivers = receiverLookupShape_(labels, 11u, 9u);
            std::vector<lighting::TerrainReceiverResult> field(receivers->features().size());
            for (u32 key = 0u; key < field.size(); ++key) {
                const auto &feature = receivers->features()[key];
                if (not portal) {
                    // Prescribed incident boundary data isolates continuation:
                    // left-facing straight contour sites1, right/caps0.
                    field[key].m_mean[0u] = feature.m_visual_x < 5.5f ? 1.0f : 0.0f;
                    continue;
                }

                const auto point = receivers->point(key);
                PPR_TEST_ASSERT(point.has_value());
                double incident{};
                constexpr u32 directions = 256u;
                for (u32 bin = 0u; bin < directions; ++bin) {
                    const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / double(directions);
                    const std::array<double, 2u> target{
                        (*point)[0] + 32.0 * std::cos(angle), (*point)[1] + 32.0 * std::sin(angle)};
                    const auto traced = lighting::traceTerrainReceiverConnectorReference(*receivers, key, target);
                    PPR_TEST_ASSERT(traced.has_value());
                    incident += traced->m_interval.m_radiance[0u] / double(directions);
                }

                field[key].m_mean[0u] = static_cast<float>(incident);
            }

            std::vector<ReceiverProbe> probes{};
            const std::vector<float> blocked_y = portal ? std::vector<float>{2.5f, 6.5f}
                                                        : std::vector<float>{2.5f, 3.5f, 4.5f, 5.5f, 6.5f};
            for (const float y: blocked_y) {
                for (const float x: {5.55f, 5.65f, 5.75f}) {
                    probes.push_back({x, y});
                }
            }

            const u32 blocked_count = static_cast<u32>(probes.size());
            for (const float y: blocked_y) {
                probes.push_back({5.15f, y});
            }

            const u32 control_count = static_cast<u32>(probes.size()) - blocked_count;
            const u32 edge_begin = static_cast<u32>(probes.size());
            if (portal) {
                // Fixed shadow-edge transect in the same solid lower back-half.
                // An edge is the first y reaching10% of that case's bright mean;
                // absence is explicit, never a fabricated zero displacement.
                for (u32 step = 0u; step <= 64u; ++step) {
                    probes.push_back({5.65f, 2.5f + float(step) / 64.0f});
                }
            }

            std::println(
                "CUT_FACE_MASK scenario={} blocked_x=5.55,5.65,5.75 blocked_y={} bright_x=5.15 bright_count={} endcaps=excluded portal=1x1 oracle=Bezier_witness_256_bins",
                scenario, portal ? "2.5,6.5" : "2.5,3.5,4.5,5.5,6.5", control_count);
            for (const u32 mode: {1u, 2u}) {
                const auto rows = fixture.lookup(*receivers, field, probes, mode);

                double blocked{}, maximum{}, bright{};
                for (u32 index = 0u; index < rows.size(); ++index) {
                    const double actual = double(rows[index][0u]);
                    PPR_TEST_ASSERT(std::isfinite(actual) and actual >= 0.0);

                    if (index < blocked_count) {
                        blocked += actual;
                        maximum = std::max(maximum, actual);
                    } else if (index < edge_begin) {
                        bright += actual;
                    }
                }

                bright /= double(control_count);
                PPR_TEST_ASSERT(bright > 0.0);
                const double mean_leak = (blocked / double(blocked_count)) / bright;
                const double max_leak = maximum / bright;
                std::println(
                    "CUT_FACE_PHYSICAL scenario={} mode={} blocked_count={} bright_mean={:.12g} mean_leak={:.12g} max_leak={:.12g} required_mean_leak=0.01 verdict={}",
                    scenario, mode, blocked_count, bright, mean_leak, max_leak, mean_leak <= 0.01 ? "PASS" : "FAIL");

                physical_gate = physical_gate and mean_leak <= 0.01;

                if (portal) {
                    double edge = -1.0;
                    for (u32 index = edge_begin; index < probes.size(); ++index) {
                        if (double(rows[index][0u]) >= 0.1 * bright) {
                            edge = double(probes[index].m_y);
                            break;
                        }
                    }

                    portal_edges[mode - 1u][open ? 1u : 0u] = edge;
                    std::println(
                        "CUT_FACE_SHADOW_EDGE open={} mode={} threshold_fraction=0.1 edge_y={} absent_value=-1", open,
                        mode, edge);
                }
            }
        }

        for (u32 mode = 0u; mode < 2u; ++mode) {
            const auto edges = portal_edges[mode];
            if (edges[0u] >= 0.0 and edges[1u] >= 0.0) {
                std::println(
                    "CUT_FACE_SHADOW_MOVEMENT mode={} closed={} open={} delta={}", mode + 1u, edges[0u], edges[1u],
                    edges[1u] - edges[0u]);
            } else {
                std::println(
                    "CUT_FACE_SHADOW_MOVEMENT mode={} closed={} open={} delta=unavailable_absent_edge", mode + 1u,
                    edges[0u], edges[1u]);
            }
        }

        // A real failing physical gate is intentionally independent of the
        // implementation-kernel leaf. Failure retains default nearest and routes
        // boundary-side policy to the coordinator; it never authorizes tuning.
        PPR_TEST_ASSERT(physical_gate);
    };

    // ------------------------------------------------------------------
    // shared exact backwall fixture geometry
    // ------------------------------------------------------------------

    std::shared_ptr<const lighting::TerrainReceiverSet> backgroundReceivers_(const bool constant, const bool open) {
        constexpr u32 width = 12u, height = 10u;
        std::vector<u16> labels(width * height, constant ? 17u : 0u);
        if (constant) {
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    if (x == 0u or y == 0u or x + 1u == width or y + 1u == height) {
                        labels[y * width + x] = 0u;
                    }
                }
            }

            labels[3u * width + 3u] = 0u;
        } else {
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    if (((x == 1u or x == width - 2u) and y >= 1u and y <= height - 2u) or
                        ((y == 1u or y == height - 2u) and x >= 1u and x <= width - 2u) or
                        (x == 6u and y >= 1u and y <= height - 2u)) {
                        labels[y * width + x] = 17u;
                    }
                }
            }

            labels[5u * width + 3u] = 23u;
            if (open)
                labels[4u * width + 6u] = 0u;
        }

        const lighting::OpticalCell stone{
            {}, {}, constant ? lighting::OpticalRgb{2.0, 1.0, 0.5} : lighting::OpticalRgb{},
            lighting::OpticalKind::opaque};
        const std::array<lighting::OpticalTerrainMaterial, 3> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {stone, 17u, 0.5f},
            {lighting::OpticalCell{{}, {}, {2.0, 1.0, 0.5}, lighting::OpticalKind::opaque}, 23u, 0.5f}};
        const auto captured = lighting::OpticalTerrainSnapshot::capture(
            {labels, materials, {}, 1.0, width, height, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(captured.has_value());

        auto geometry = std::make_shared<const lighting::OpticalTerrainSnapshot>(*captured);
        const auto receivers = lighting::TerrainReceiverSet::capture(geometry);
        PPR_TEST_ASSERT(receivers.has_value());
        return std::make_shared<const lighting::TerrainReceiverSet>(*receivers);
    }

    // ------------------------------------------------------------------
    // CPU-only fixed-point angular and near-field dependency comparison
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_background_cpu_nearfield_dependency_comparison) {
        using Clock = std::chrono::steady_clock;
        constexpr u32 width = 12u, height = 10u, points = 12u, split = 4u;
        std::array<std::array<double, points>, 2> direct{}, baseline{}, straight{}, candidate{};
        u64 control_queries{}, direct_queries{}, connector_queries{}, local_interval_queries{}, identity_connectors{},
            coarse_reads{};
        double baseline_ms{}, direct_ms{}, control_ms{}, candidate_ms{};

        for (u32 state = 0u; state < 2u; ++state) {
            const bool open = state != 0u;
            const auto receivers = backgroundReceivers_(false, open);
            const auto &geometry = *receivers->geometry();
            const auto optical = geometry.view();
            std::vector<lighting::OpticalCell> cells;
            for (const auto label: optical.m_labels) {
                const auto material = std::ranges::find(
                    optical.m_materials, label, &lighting::OpticalTerrainMaterial::m_material);
                PPR_TEST_ASSERT(material != optical.m_materials.end());

                cells.push_back(material->m_optics);
            }

            const lighting::OpticalGridView grid{cells, {}, 1.0, width, height, lighting::OpticalBoundary::vacuum};
            const auto request = cascadeRequest_(width, height);

            auto started = Clock::now();
            const auto field = solveCpuCascades_(grid, request, false, false, &geometry);
            baseline_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            PPR_TEST_ASSERT(field.m_layout.m_cascades.size() == split + 1u);
            const auto &coarse = field.m_layout.m_cascades[split];
            PPR_TEST_ASSERT(coarse.m_near_cells == 8u and coarse.m_directions == 128u);

            std::println(
                "CPU_NEARFIELD_BASELINE_WORK scene={} connector_queries={} top_interval_queries={} coarse_reads={}",
                open ? "open" : "closed", field.m_connector_queries, field.m_top_interval_queries,
                field.m_coarse_reads);

            for (u32 screen_y = 3u; screen_y <= 6u; ++screen_y) {
                for (u32 x = 7u; x <= 9u; ++x) {
                    const u32 index = (screen_y - 3u) * 3u + x - 7u;
                    const u32 world_y = height - screen_y - 1u;
                    const std::array<double, 2> q{double(x) + 0.5, double(world_y) + 0.5};

                    for (u32 bin = 0u; bin < 8u; ++bin) {
                        baseline[state][index] += field.m_intervals[0][(world_y * width + x) * 8u + bin].m_radiance[0] /
                                                  8.0;
                    }

                    started = Clock::now();
                    for (u32 bin = 0u; bin < 1024u; ++bin) {
                        const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / 1024.0;
                        const auto traced = lighting::traceOpticalTerrainReference(
                            geometry, {q, {std::cos(angle), std::sin(angle)}, 0.0, 32.0});
                        PPR_TEST_ASSERT(traced.has_value());

                        ++direct_queries;
                        direct[state][index] += traced->m_interval.m_radiance[0] / 1024.0;
                    }

                    direct_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();

                    started = Clock::now();
                    for (const auto &direction: field.m_directions[split]) {
                        const auto traced = lighting::traceOpticalTerrainReference(geometry, {q, direction, 0.0, 32.0});
                        PPR_TEST_ASSERT(traced.has_value());

                        ++control_queries;
                        straight[state][index] += traced->m_interval.m_radiance[0] / 128.0;
                    }

                    control_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();

                    const u64 connectors_before = connector_queries;
                    const u64 local_before = local_interval_queries;
                    const u64 reads_before = coarse_reads;
                    started = Clock::now();

                    // Only this point's C3..C0 dependency closure is retained.
                    // Every virtual early level has unit probe spacing, so q
                    // is an exact shared lattice node: floor==ceil and weight1.
                    std::array<std::vector<lighting::RadianceInterval>, split> local;
                    for (u32 level = split; level-- > 0u;) {
                        const auto &level_layout = field.m_layout.m_cascades[level];
                        const auto &parent = field.m_layout.m_cascades[level + 1u];
                        local[level].resize(static_cast<std::size_t>(level_layout.m_directions));

                        for (u64 bin = 0u; bin < level_layout.m_directions; ++bin) {
                            auto &output = local[level][static_cast<std::size_t>(bin)];
                            output.m_transmittance = {};
                            const auto &direction = field.m_directions[level][static_cast<std::size_t>(bin)];
                            const std::array<double, 2> start{
                                q[0] + double(level_layout.m_near_cells) * direction[0],
                                q[1] + double(level_layout.m_near_cells) * direction[1]};

                            for (u64 child = 0u; child < 2u; ++child) {
                                const u64 upper_bin = 2u * bin + child;
                                const auto &child_direction =
                                    field.m_directions[level + 1u][static_cast<std::size_t>(upper_bin)];
                                const u64 parent_spacing = level + 1u == split ? parent.m_spacing_cells : 1u;
                                const double lattice_x = (q[0] - 0.5) / double(parent_spacing);
                                const double lattice_y = (q[1] - 0.5) / double(parent_spacing);
                                const double fraction_x = lattice_x - std::floor(lattice_x);
                                const double fraction_y = lattice_y - std::floor(lattice_y);

                                for (u32 sy = 0u; sy < 2u; ++sy) {
                                    for (u32 sx = 0u; sx < 2u; ++sx) {
                                        const double weight = (sx == 0u ? 1.0 - fraction_x : fraction_x) *
                                                              (sy == 0u ? 1.0 - fraction_y : fraction_y);
                                        if (weight == 0.0) {
                                            continue;
                                        }

                                        const u64 px = static_cast<u64>(
                                            sx == 0u ? std::floor(lattice_x) : std::ceil(lattice_x));
                                        const u64 py = static_cast<u64>(
                                            sy == 0u ? std::floor(lattice_y) : std::ceil(lattice_y));
                                        const std::array<double, 2> parent_probe{
                                            0.5 + double(px * parent_spacing), 0.5 + double(py * parent_spacing)};
                                        const std::array<double, 2> end{
                                            parent_probe[0] + double(parent.m_near_cells) * child_direction[0],
                                            parent_probe[1] + double(parent.m_near_cells) * child_direction[1]};

                                        ++connector_queries;
                                        if (start == end) {
                                            ++identity_connectors;
                                        } else {
                                            ++local_interval_queries;
                                        }

                                        const auto near = connector_(grid, start, end, &geometry);
                                        lighting::RadianceInterval far{};
                                        if (level + 1u == split) {
                                            PPR_TEST_ASSERT(
                                                px < parent.m_padded_probes[0] and py < parent.m_padded_probes[1]);

                                            ++coarse_reads;
                                            far = field.m_intervals[split][static_cast<std::size_t>(
                                                (py * parent.m_padded_probes[0] + px) * parent.m_directions +
                                                upper_bin)];
                                        } else {
                                            PPR_TEST_ASSERT(parent_probe == q);

                                            far = local[level + 1u][static_cast<std::size_t>(upper_bin)];
                                        }

                                        const auto pair = lighting::composeIntervals(near, far);
                                        PPR_TEST_ASSERT(pair.has_value());

                                        for (u32 channel = 0u; channel < 3u; ++channel) {
                                            output.m_radiance[channel] += 0.5 * weight * pair->m_radiance[channel];
                                            output.m_transmittance[channel] += 0.5 *
                                                                               weight *
                                                                               pair->m_transmittance[channel];
                                        }
                                    }
                                }
                            }
                        }
                    }

                    for (const auto &interval: local[0]) {
                        candidate[state][index] += interval.m_radiance[0] / 8.0;
                    }

                    candidate_ms += std::chrono::duration<double, std::milli>(Clock::now() - started).count();

                    std::println(
                        "CPU_NEARFIELD_POINT scene={} q=({},{}) direct_1024={} baseline_c0={} straight_128={} candidate={} formerly_dark={} connector_queries={} positive_local_interval_queries={} coarse_reads={}",
                        open ? "open" : "closed", q[0], q[1], direct[state][index], baseline[state][index],
                        straight[state][index], candidate[state][index],
                        direct[state][index] == 0.0 and baseline[state][index] > 0.0,
                        connector_queries - connectors_before, local_interval_queries - local_before,
                        coarse_reads - reads_before);
                }
            }
        }

        const auto quality = [&](const std::string_view name, const auto &values) {
            double reference{}, measured{}, error{}, blocked{}, bias{};
            for (u32 point = 0u; point < points; ++point) {
                reference += direct[1][point];
                measured += values[1][point];
                error += std::abs(values[1][point] - direct[1][point]);
                bias += values[1][point] - direct[1][point];
                blocked += values[0][point];
            }

            const bool pass = measured > 0.0 and
                              reference > 0.0 and
                              error <= 0.15 * reference and
                              blocked <= 0.01 * measured;
            std::println(
                "CPU_NEARFIELD_QUALITY mode={} reference_sum={} measured_sum={} sum_abs_errors={} signed_bias={} relative_l1={} blocked_ratio={} gate={}",
                name, reference, measured, error, bias, error / reference, measured > 0.0 ? blocked / measured : -1.0,
                pass);
            return pass;
        };

        std::ignore = quality("baseline", baseline);
        const bool control_gate = quality("straight128", straight);
        const bool candidate_gate = quality("same_q_to_near8", candidate);

        std::println(
            "CPU_NEARFIELD_WORK direct_queries={} control_queries={} connector_queries={} positive_local_interval_queries={} identity_connectors={} coarse_reads={} duplicate_cross_point_work_counted=1 baseline_ms={} direct_ms={} control_ms={} candidate_ms={}",
            direct_queries, control_queries, connector_queries, local_interval_queries, identity_connectors,
            coarse_reads, baseline_ms, direct_ms, control_ms, candidate_ms);
        std::ignore = std::fflush(nullptr); // Best-effort preservation before unchanged physical gates.

        PPR_TEST_ASSERT(control_queries == 3072u and direct_queries == 24576u);
        PPR_TEST_ASSERT(connector_queries == 24u * 624u and coarse_reads == 24u * 512u);
        PPR_TEST_ASSERT(connector_queries == local_interval_queries + identity_connectors);
        PPR_TEST_ASSERT(control_gate);
        PPR_TEST_ASSERT(candidate_gate);
    };

    // ------------------------------------------------------------------
    // completed C0 backwall reconstruction and exact-view publication
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_background_view_preserves_world_visibility_samples_and_leases) {
        TerrainFieldScope isolated{};
        auto &field_scope = terrainFieldScope_(isolated);
        PPR_TEST_ASSERT(not field_scope.m_receiver_started or field_scope.m_receiver_completed);

        field_scope.m_background_started = true;
        auto &fixture = field_scope.fixture();
        PPR_DEFER {
            fixture.endDiagnostics();

            isolated.shutdown();
        };
        field_scope.initialize();
        fixture.observePhase("background.scenario.begin");
        auto &device = fixture.m_rhi->getDevice();
        constexpr u32 width = 12u, height = 10u;
        const lighting::TerrainReceiverCohort cohort{3u, 5u, 7u, 11u};
        auto request = cascadeRequest_(width, height);

        const auto make_receivers = backgroundReceivers_;

        if (device.getInfo().deviceType != rhi::DeviceType::Vulkan) {
            const auto receivers = make_receivers(true, false);
            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTerrainReceivers(
                    receivers, cohort, request, fixture.m_fence.completed(), false,
                    lighting::TerrainReceiverOutput::mean_only,
                    lighting::TerrainBackgroundOutput::c0_mean) == std::errc::not_supported);
            std::println("BACKGROUND_PHYSICAL_NOT_RUN reason=Vulkan_terrain_producer_required");
            field_scope.m_background_completed = true;
            return;
        }

        const auto read_rows = [&](rhi::IBuffer *const buffer) {
            fixture.observePhase("background.readback.begin");
            PPR_TEST_ASSERT(buffer->getDesc().size <= 16u * 1024u);
            std::vector<std::array<float, 4>> rows(buffer->getDesc().size / 16u);
            rhi::BufferDesc description{};
            description.size = buffer->getDesc().size;
            description.elementSize = 16u;
            description.memoryType = rhi::MemoryType::ReadBack;
            description.usage = rhi::BufferUsage::CopyDestination;
            description.defaultState = rhi::ResourceState::CopyDestination;
            rhi::ComPtr<rhi::IBuffer> readback{};
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, readback.writeRef())));

            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            encoder->setBufferState(buffer, rhi::ResourceState::CopySource);
            encoder->copyBuffer(readback, 0u, buffer, 0u, description.size);
            encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);
            rhi::ComPtr<rhi::ICommandBuffer> commands{};
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
            rhi::ICommandBuffer *pointer = commands.get();
            rhi::SubmitDesc submit{};
            submit.commandBuffers = &pointer;
            submit.commandBufferCount = 1u;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submit)));
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

            void *mapped{};
            PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
            std::memcpy(rows.data(), mapped, description.size);
            PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

            fixture.observePhase("background.readback.end");
            return rows;
        };

        Camera camera{ECameraProjection::orthographic};
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_origin = float3{0.0f, 0.0f, -0.5f};
        model.m_ortho_scale = 1.0f;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, width, height}});
        const auto camera_snapshot = camera.getSnapshot();
        const int2 extent{int(width), int(height)};

        const std::array<float2, 1> jitter{float2{0.25f, -0.25f}};
        camera.setJitterSamples(CameraJitterSamples{std::span<const float2>{jitter}});
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, width, height}});
        auto raster_camera = camera.getSnapshot();
        raster_camera.m_projection = raster_camera.m_jittered_projection;
        raster_camera.m_invert_projection = inverse(raster_camera.m_projection);
        raster_camera.m_view_projection = raster_camera.m_jittered_view_projection;
        raster_camera.m_invert_view_projection = raster_camera.m_invert_jittered_view_projection;

        std::shared_ptr<const lighting::TerrainBackgroundGpuView> retained{};
        double blocked_mean{}, open_mean{}, physical_error{}, physical_scale{}, physical_bias{};

        for (u32 scene = 0u; scene < 3u; ++scene) {
            fixture.observePhase(
                scene == 0u ? "background.constant.begin"
                            : (scene == 1u ? "background.closed.begin" : "background.open.begin"));
            const bool constant = scene == 0u, open = scene == 2u;
            const auto receivers = make_receivers(constant, open);
            if (scene == 0u) {
                // Establish the prior direct terrain pipeline: a wrong generic
                // entry must reject by batch kind, not by a missing pipeline.
                const std::array<lighting::OpticalRay, 1> rays{lighting::OpticalRay{{0.5, 0.5}, {1.0, 0.0}, 0.0, 0.0}};
                PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                    *receivers->geometry(), rays, request.m_budget_bytes, 0u, request.m_max_resource_bytes,
                    fixture.m_fence.completed()));
                fixture.submit();
                const auto traced = fixture.m_pass.readCompleted(fixture.m_fence.completed());
                PPR_TEST_ASSERT(traced.has_value() and traced->m_intervals.size() == 1u);
                assertClose_(traced->m_intervals.front(), {});
            }

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainReceivers(
                receivers, cohort, request, fixture.m_fence.completed(), false,
                lighting::TerrainReceiverOutput::mean_only, lighting::TerrainBackgroundOutput::c0_mean));
            fixture.submit(true, true);
            const auto acquired = fixture.m_pass.acquireTerrainReceiverView(fixture.m_fence.completed());
            PPR_TEST_ASSERT(acquired.has_value());
            const auto generation = *acquired;
            PPR_TEST_ASSERT(generation->backgroundMeanBuffer() != nullptr);
            PPR_TEST_ASSERT(generation->backgroundMeanBuffer()->getDesc().size == width * height * 16u);

            if (not device.hasFeature("standard-sample-locations-4") or not device.hasFeature("sample-rate-shading")) {
                PPR_TEST_ASSERT(
                    fixture.m_pass.prepareTerrainBackgroundView(
                        generation, camera_snapshot, extent, request.m_budget_bytes, 0u, request.m_max_resource_bytes,
                        fixture.m_fence.completed()) == std::errc::not_supported);
                std::println("BACKGROUND_PHYSICAL_NOT_RUN reason=admitted_standard_MSAA4_sample_pattern_required");
                field_scope.m_background_completed = true;
                return;
            }


            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTerrainBackgroundView(
                    generation, camera_snapshot, extent, 1u, 0u, request.m_max_resource_bytes,
                    fixture.m_fence.completed()) == std::errc::no_buffer_space);
            auto invalid_camera = camera_snapshot;
            invalid_camera.m_invert_view_projection[0][0] = std::numeric_limits<float>::quiet_NaN();
            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTerrainBackgroundView(
                    generation, invalid_camera, extent, request.m_budget_bytes, 0u, request.m_max_resource_bytes,
                    fixture.m_fence.completed()) == std::errc::argument_out_of_domain);

            for (u32 invalid_plane = 0u; invalid_plane < 3u; ++invalid_plane) {
                auto unsupported = camera_snapshot;
                if (invalid_plane < 2u) {
                    unsupported.m_invert_view_projection[2][0] = 1.0f;
                    unsupported.m_invert_view_projection[2][2] = invalid_plane == 0u ? 0.0f : 1.0e-9f;
                } else {
                    unsupported.m_invert_view_projection[3][2] = 1.0f;
                    unsupported.m_invert_view_projection[2][2] = 2.0f;
                }

                PPR_TEST_ASSERT(
                    fixture.m_pass.prepareTerrainBackgroundView(
                        generation, unsupported, extent, request.m_budget_bytes, 0u, request.m_max_resource_bytes,
                        fixture.m_fence.completed()) == std::errc::argument_out_of_domain);
            }

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainBackgroundView(
                generation, camera_snapshot, extent, request.m_budget_bytes, 0u, request.m_max_resource_bytes,
                fixture.m_fence.completed()));
            {
                rhi::ComPtr<rhi::ICommandEncoder> rejected{};
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(rejected.writeRef())));
                const PreRenderContext context{device, *rejected, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(fixture.m_pass.encodeTrace(context) == std::errc::operation_not_permitted);
            }

            fixture.submit(false, false, true);
            const auto wrong_read = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(not wrong_read and wrong_read.error() == std::errc::operation_not_permitted);
            const auto published = fixture.m_pass.acquireTerrainBackgroundView(fixture.m_fence.completed());
            PPR_TEST_ASSERT(published.has_value());
            const auto view = *published;
            const auto statistics_before = view->statistics();
            const auto repeated = fixture.m_pass.acquireTerrainBackgroundView(fixture.m_fence.completed());
            PPR_TEST_ASSERT(repeated.has_value() and *repeated == view);
            const auto &statistics_after = view->statistics();
            PPR_TEST_ASSERT(
                statistics_before.m_query_upper_bound == statistics_after.m_query_upper_bound and
                statistics_before.m_attempted == statistics_after.m_attempted and
                statistics_before.m_qualified == statistics_after.m_qualified and
                statistics_before.m_unresolved == statistics_after.m_unresolved and
                statistics_before.m_bound_error == statistics_after.m_bound_error and
                statistics_before.m_outside_plane == statistics_after.m_outside_plane and
                statistics_before.m_samples == statistics_after.m_samples and
                statistics_before.m_invalid_values == statistics_after.m_invalid_values);

            PPR_TEST_ASSERT(view->matches(camera_snapshot, extent, device));
            PPR_TEST_ASSERT(not view->matches(invalid_camera, extent, device));
            PPR_TEST_ASSERT(not view->matches(camera_snapshot, int2{11, 10}, device));
            PPR_TEST_ASSERT(not view->matches(raster_camera, extent, device));

            PPR_TEST_ASSERT(view->statistics().m_unresolved == 0u and view->statistics().m_invalid_values == 0u);
            PPR_TEST_ASSERT(view->statistics().m_samples == width * height * 5u);
            PPR_TEST_ASSERT(view->statistics().m_outside_plane > 0u);
            const auto rows = read_rows(view->resultsBuffer());
            std::vector<double> direct_red(width * height, -1.0);

            CpuCascadeFixture constant_reference{};
            std::vector<lighting::OpticalCell> constant_cells{};
            if (constant or fixture.m_diagnostic_enabled) {
                const auto optical = receivers->geometry()->view();
                for (const auto label: optical.m_labels) {
                    const auto material = std::ranges::find(
                        optical.m_materials, label, &lighting::OpticalTerrainMaterial::m_material);
                    PPR_TEST_ASSERT(material != optical.m_materials.end());

                    constant_cells.push_back(material->m_optics);
                }

                const lighting::OpticalGridView grid{
                    constant_cells, {}, 1.0, width, height, lighting::OpticalBoundary::vacuum};
                constant_reference = solveCpuCascades_(grid, request, false, false, receivers->geometry().get());
            }

            // Diagnostic-only readback observes the actual published C0 buffer;
            // it does not replace the direct-ray physical acceptance reference.
            const auto gpu_c0 = fixture.m_diagnostic_enabled and not constant
                                    ? read_rows(generation->backgroundMeanBuffer())
                                    : std::vector<std::array<float, 4>>{};

            // Independent camera projection and analytic closed-room constant
            // constrain indexed sample mapping, all four hull edges and far void.
            constexpr std::array<std::array<double, 2>, 5> positions{
                {{0.5, 0.5}, {0.375, 0.125}, {0.875, 0.375}, {0.125, 0.625}, {0.625, 0.875}}};
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    for (u32 sample = 0u; sample < 5u; ++sample) {
                        const std::array<double, 2> q{
                            double(x) + positions[sample][0], double(height - y) - positions[sample][1]};
                        const auto &row = rows[(y * width + x) * 5u + sample];
                        const bool inside = q[0] >= 0.5 and q[0] <= 11.5 and q[1] >= 0.5 and q[1] <= 9.5;
                        const bool analytic_interior = constant and
                                                       q[0] >= 2.5 and
                                                       q[0] <= double(width) - 2.5 and
                                                       q[1] >= 2.5 and
                                                       q[1] <= double(height) - 2.5;
                        if (not inside or analytic_interior) {
                            for (u32 channel = 0u; channel < 3u; ++channel) {
                                const double expected = inside ? std::ldexp(2.0, -static_cast<int>(channel)) : 0.0;
                                PPR_TEST_ASSERT(std::abs(double(row[channel]) - expected) <= 1e-5);
                            }
                        }

                        if (constant and inside and not analytic_interior) {
                            // Preserve independent near-perimeter bucket checks:
                            // the protected vacuum ring has a nonconstant field.
                            const lighting::OpticalGridView grid{
                                constant_cells, {}, 1.0, width, height, lighting::OpticalBoundary::vacuum};
                            const double lx = q[0] - 0.5, ly = q[1] - 0.5;
                            const double fx = lx - std::floor(lx), fy = ly - std::floor(ly);
                            lighting::OpticalRgb expected{};
                            for (u32 sy = 0u; sy < 2u; ++sy) {
                                for (u32 sx = 0u; sx < 2u; ++sx) {
                                    const double weight = (sx == 0u ? 1.0 - fx : fx) * (sy == 0u ? 1.0 - fy : fy);
                                    if (weight == 0.0)
                                        continue;

                                    const auto px = static_cast<u32>(sx == 0u ? std::floor(lx) : std::ceil(lx));
                                    const auto py = static_cast<u32>(sy == 0u ? std::floor(ly) : std::ceil(ly));
                                    const auto local = connector_(
                                        grid, q, {double(px) + 0.5, double(py) + 0.5}, receivers->geometry().get());
                                    for (u32 channel = 0u; channel < 3u; ++channel) {
                                        double mean{};
                                        for (u32 bin = 0u; bin < 8u; ++bin) {
                                            mean += constant_reference.m_intervals[0][(py * width + px) * 8u + bin]
                                                        .m_radiance[channel] /
                                                    8.0;
                                        }

                                        expected[channel] += weight * (local.m_radiance[channel] +
                                                                          local.m_transmittance[channel] * mean);
                                    }
                                }
                            }

                            for (u32 channel = 0u; channel < 3u; ++channel) {
                                PPR_TEST_ASSERT(
                                    std::abs(double(row[channel]) - expected[channel]) <=
                                    1e-5 + 1e-4 * std::abs(expected[channel]));
                            }
                        }

                        if (not constant and inside and sample == 0u and x >= 7u and x <= 9u and y >= 3u and y <= 6u) {
                            double direct{};
                            for (u32 angle = 0u; angle < 1024u; ++angle) {
                                const double a = 2.0 * std::numbers::pi * (double(angle) + 0.5) / 1024.0;
                                const auto ray = lighting::traceOpticalTerrainReference(
                                    *receivers->geometry(), {{q[0], q[1]}, {std::cos(a), std::sin(a)}, 0.0, 32.0});
                                PPR_TEST_ASSERT(ray.has_value());
                                direct += ray->m_interval.m_radiance[0] / 1024.0;
                            }
                            direct_red[y * width + x] = direct;
                            if (not open)
                                PPR_TEST_ASSERT(direct == 0.0);
                        }
                    }
                }
            }

            if (not constant) {
                GridPass grid{};
                PPR_DEFER {
                    PPR_TEST_ASSERT(not grid.shutdown());
                };
                PPR_TEST_ASSERT(
                    not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
                GridStagedFrame stage{};
                stage.m_has_catalog = stage.m_full_refresh = stage.m_reset_caches = stage.m_has_background = true;
                stage.m_cell_size_m = 1.0f;
                stage.m_boundary_material_id = 0u;
                stage.m_content_fingerprint = cohort.m_content_fingerprint;
                stage.m_optical_geometry = receivers->geometry();
                stage.m_optical_revision = cohort.m_geometry_revision;
                const auto optical = receivers->geometry()->view();
                for (const auto &material: optical.m_materials) {
                    if (material.m_material == 0u) {
                        continue;
                    }

                    GridTerrainVisual visual{};
                    visual.m_material_id = material.m_material;
                    visual.m_roundness = material.m_roundness;
                    visual.m_transport_model = kGridTransportOpaqueV1;
                    // Grid stores a bounded linear color and a separate source
                    // strength. The fixture's S=(2,1,.5) is exactly preserved.
                    const double strength = std::max(
                        1.0, *std::ranges::max_element(material.m_optics.m_surface_emission));
                    visual.m_emissive_strength = static_cast<float>(strength);
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        visual.m_emissive_rgb[channel] = static_cast<float>(
                            material.m_optics.m_surface_emission[channel] / strength);
                    }

                    const auto projected = gridMaterialOpticalCell(visual);
                    PPR_TEST_ASSERT(
                        projected.has_value() and
                        projected->m_surface_emission == material.m_optics.m_surface_emission);

                    stage.m_terrain_visuals.push_back(visual);
                }

                const std::array<std::byte, 4> pixel{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
                image::ImageAsset albedo{};
                albedo.m_width = albedo.m_height = 1u;
                albedo.m_storage = mem::SharedBuffer::clone(mem::SharedBufferView{pixel.data(), pixel.size()});
                albedo.m_subresources.push_back({albedo.m_storage, 4u, 4u});
                stage.m_terrain_images.push_back(std::move(albedo));
                u32 order{};
                for (auto &layer: stage.m_background_layers) {
                    layer.m_order = order++;
                    layer.m_texture_index = 0u;
                    layer.m_color_rgba = 0xffffffffu;
                    layer.m_repeat_period_m = 8.0f;
                    layer.m_parallax_factor = 1.0f;
                }

                stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
                GridStagedUpload upload{};
                upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
                upload.m_cell_materials.resize(GridPass::kCellsPerChunk, 0u);
                for (u32 y = 0u; y < height; ++y) {
                    std::copy_n(
                        optical.m_labels.begin() + y * width, width, upload.m_cell_materials.begin() + y * 128u);
                }

                stage.m_uploads.push_back(std::move(upload));

                PPR_TEST_ASSERT(not grid.stageWorldCandidate(cohort.m_job_id, cohort.m_world_generation, stage));
                const auto ready = grid.advanceWorldCandidate(device, fixture.m_fence.completed());
                PPR_TEST_ASSERT(ready.has_value() and *ready);
                PPR_TEST_ASSERT(not grid.activateWorldCandidate(
                    cohort.m_job_id, cohort.m_world_generation, cohort.m_content_fingerprint));
                PPR_TEST_ASSERT(not grid.setTerrainReceiverLighting(generation));
                PPR_TEST_ASSERT(not grid.setTerrainBackgroundLighting(view));
                PPR_TEST_ASSERT(not grid.update(
                    TimeSpan{}, camera_snapshot, false, GridPass::ReceiverContinuation::nearest,
                    GridPass::ReceiverResponse::flat, GridPass::BackgroundResponse::lit_backwall));

                rhi::TextureDesc description{};
                description.size = {width, height, 1u};
                description.format = rhi::Format::RGBA32Float;
                description.usage = static_cast<rhi::TextureUsage>(
                    static_cast<u32>(rhi::TextureUsage::RenderTarget) |
                    static_cast<u32>(rhi::TextureUsage::CopySource));
                description.defaultState = rhi::ResourceState::RenderTarget;
                rhi::ComPtr<rhi::ITexture> target{};
                rhi::ComPtr<rhi::ITextureView> target_view{};
                PPR_TEST_ASSERT(not make_error_code(device.createTexture(description, nullptr, target.writeRef())));
                PPR_TEST_ASSERT(not make_error_code(target->getDefaultView(target_view.writeRef())));
                rhi::ComPtr<rhi::ICommandEncoder> encoder{};
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
                const auto viewport = rhi::Viewport::fromSize(float(width), float(height));
                const auto scissor = rhi::ScissorRect::fromSize(width, height);
                const PreRenderContext prepare{device, *encoder, fixture.m_fence.completed(), {}};
                PPR_TEST_ASSERT(
                    grid.prepareBackground(prepare, raster_camera, viewport, scissor) == std::errc::invalid_argument);

                PPR_TEST_ASSERT(not grid.prepareBackground(prepare, camera_snapshot, viewport, scissor));

                rhi::RenderPassColorAttachment attachment{};
                attachment.view = target_view;
                rhi::RenderPassDesc pass_description{};
                pass_description.colorAttachments = &attachment;
                pass_description.colorAttachmentCount = 1u;
                auto *const pass = encoder->beginRenderPass(pass_description);
                PPR_TEST_ASSERT(pass != nullptr);
                RenderPipelineSignature signature{};
                signature.m_color_formats[0] = rhi::Format::RGBA32Float;
                signature.m_color_format_count = 1u;
                const DrawContext draw{
                    .m_device = device,
                    .m_pass = *pass,
                    .m_render_pipeline_key = RenderPipelineKey{signature},
                    .m_viewport = viewport,
                    .m_scissor = scissor,
                    .m_target_extent = extent,
                    .m_completed_fence_value = fixture.m_fence.completed()};
                PPR_TEST_ASSERT(not grid.renderBackground(draw));
                pass->end();
                rhi::ComPtr<rhi::ICommandBuffer> commands{};
                PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));
                rhi::ICommandBuffer *pointer = commands.get();
                rhi::IFence *signal = fixture.m_fence.fence();
                const auto value = fixture.m_fence.nextValue();
                const u64 raw_value = value;
                rhi::SubmitDesc submit{};
                submit.commandBuffers = &pointer;
                submit.commandBufferCount = 1u;
                submit.signalFences = &signal;
                submit.signalFenceValues = &raw_value;
                submit.signalFenceCount = 1u;
                PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submit)));
                fixture.m_fence.commitSubmitted(value);
                grid.notifyFrameSubmitted(value);
                PPR_TEST_ASSERT(not fixture.m_fence.waitFor(value));

                shader::ComPtr<ISlangBlob> blob{};
                rhi::SubresourceLayout layout{};
                PPR_TEST_ASSERT(not make_error_code(device.readTexture(target, 0u, 0u, blob.writeRef(), &layout)));
                const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer());
                for (u32 y = 0u; y < height; ++y) {
                    for (u32 x = 0u; x < width; ++x) {
                        std::array<float, 4> actual{};
                        std::memcpy(actual.data(), bytes + y * layout.rowPitch + x * 16u, sizeof(actual));
                        const double direct = direct_red[y * width + x];
                        if (direct >= 0.0) {
                            const double measured = double(actual[0]) / (128.0 / 255.0);
                            if (fixture.m_diagnostic_enabled) {
                                const u32 world_y = height - y - 1u;
                                const u32 probe = world_y * width + x;
                                double cpu_c0{};
                                for (u32 bin = 0u; bin < 8u; ++bin) {
                                    cpu_c0 += constant_reference.m_intervals[0][probe * 8u + bin].m_radiance[0] / 8.0;
                                }

                                std::println(
                                    "BACKGROUND_PHYSICAL_SAMPLE scene={} q=({},{}) probe={} direct_1024={} cpu_c0={} gpu_c0={} rendered_over_albedo={}",
                                    open ? "open" : "closed", double(x) + 0.5, double(world_y) + 0.5, probe, direct,
                                    cpu_c0, gpu_c0[probe][0], measured);
                            }

                            if (open) {
                                open_mean += measured;
                                physical_error += std::abs(measured - direct);
                                physical_scale += direct;
                                physical_bias += measured - direct;
                            } else {
                                blocked_mean += measured;
                            }
                        }

                        const auto &incident = rows[(y * width + x) * 5u];
                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            const double expected = double(incident[channel]) * (128.0 / 255.0);
                            // Cached backdrop is RGBA16F; the RGBA32F readback
                            // observes that one declared storage quantization before grading.
                            PPR_TEST_ASSERT(
                                std::abs(double(actual[channel]) - expected) <= 1e-5 + 1e-3 * std::abs(expected));
                        }
                    }
                }
            }

            if (retained) {
                PPR_TEST_ASSERT(retained->resultsBuffer() != view->resultsBuffer());
                PPR_TEST_ASSERT(retained->source()->backgroundMeanBuffer() != generation->backgroundMeanBuffer());
                const auto prior = read_rows(retained->resultsBuffer());
                PPR_TEST_ASSERT(prior.size() == width * height * 5u);
            }

            retained = view;
            fixture.observePhase(
                scene == 0u ? "background.constant.verdict.pass"
                            : (scene == 1u ? "background.closed.verdict.pass" : "background.open.verdict.pass"));
        }

        if (fixture.m_diagnostic_enabled) {
            std::println(
                "BACKGROUND_PHYSICAL_SUMMARY reference_sum={} measured_sum={} sum_abs_errors={} signed_bias={} blocked_sum={}",
                physical_scale, open_mean, physical_error, physical_bias, blocked_mean);
            std::ignore = std::fflush(nullptr); // Best-effort evidence before unchanged physical assertions.
        }

        PPR_TEST_ASSERT(open_mean > 0.0);
        PPR_TEST_ASSERT(blocked_mean <= 0.01 * open_mean);
        PPR_TEST_ASSERT(physical_scale > 0.0 and physical_error <= 0.15 * physical_scale);
        std::println(
            "BACKGROUND_PHYSICAL blocked={} open={} relative_brightness_error={}", blocked_mean, open_mean,
            physical_error / physical_scale);
        field_scope.m_background_lease_check = [retained] {
            PPR_TEST_ASSERT(retained->resultsBuffer()->getDesc().size == width * height * 5u * 16u);
        };

        field_scope.m_background_completed = true;
        fixture.observePhase("background.scenario.verdict.pass");
    };

    // ------------------------------------------------------------------
    // completed receiver field and generation leases
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_receivers_reconstruct_c1_and_retain_completed_generation_leases) {
        const auto precision_setting = config::getenvLookup("PPR_TEST_TERRAIN_PRODUCTION_FP64");
        PPR_TEST_ASSERT(not precision_setting or *precision_setting == "0" or *precision_setting == "1");
        const bool production_refined = precision_setting and *precision_setting == "1";
        lighting::TerrainPrecisionSelection precision{};
        if (production_refined) {
            PPR_TEST_ASSERT(not config::getenvLookup("PPR_TEST_TERRAIN_FP64_PROTOTYPE_DIRECTORY"));
            PPR_TEST_ASSERT(not config::getenvLookup("PPR_TEST_FP64_CORPUS_DIRECTORY"));

            // Explicit qualification caller assertion for the checked device/session.
            // This opt-in is neither runtime RTE discovery nor general admission.
            precision.m_precision = lighting::TerrainPrecision::qualified_fp64_refinement_experimental;
            precision.m_rte64_qualified_for_device_session = true;
        }

        const auto diagnostic_setting = config::getenvLookup("PPR_TEST_RADIANCE_CASCADE_DIAGNOSTIC");
        const bool diagnostic = diagnostic_setting and *diagnostic_setting == "1";
        const auto phase = [diagnostic, production_refined](const string_literal name) {
            if (diagnostic) {
                std::println(
                    "PRODUCTION_RECEIVER_PHASE mode={} phase={}", production_refined ? "refined" : "strict",
                    name.view());
                std::ignore = std::fflush(nullptr); // Best-effort existing watchdog observation.
            }
        };

        TerrainFieldScope isolated{};
        auto &field_scope = terrainFieldScope_(isolated);
        PPR_TEST_ASSERT(not field_scope.m_fixture);

        field_scope.m_receiver_started = true;
        auto &fixture = field_scope.fixture();
        PPR_DEFER {
            fixture.endDiagnostics();

            isolated.shutdown();
        };
        {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.Initialize"};
            phase("initialize.begin");
            field_scope.initialize(precision);
            phase("initialize.end");
        }

        const auto initial_statistics = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
        PPR_TEST_ASSERT(initial_statistics.has_value() and not *initial_statistics);

        const auto spirv_directory = config::getenvLookup("PPR_TEST_TERRAIN_SPIRV_DIRECTORY");
        if (spirv_directory) {
            PPR_TEST_ASSERT(not spirv_directory->empty());
            const auto directory = std::filesystem::path{*spirv_directory} /
                                   (production_refined ? "production-refined" : "production-strict");
            PPR_TEST_ASSERT(not fixture.m_pass.writeTerrainSpirvForQualification(directory, false));
            PPR_TEST_ASSERT(not fixture.m_pass.writeTerrainSpirvForQualification(directory, true));
        }

        if (production_refined) {
            // Snapshot capture rejects 4097 samples before the pass receives a
            // geometry owner. This is capture admission, not pass preparation
            // or execution of the unreachable >UINT32 query-count branch.
            const u32 width = lighting::RadianceCascadePass::kMaxCompleteFixtureCells + 1u;
            const std::vector<u16> oversized_labels(width, 0u);
            const std::array<lighting::OpticalTerrainMaterial, 1u> vacuum_material{
                lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}};
            const auto oversized = lighting::OpticalTerrainSnapshot::capture(
                {oversized_labels, vacuum_material, {}, 1.0, width, 1u, 0u, lighting::OpticalBoundary::vacuum});
            PPR_TEST_ASSERT(not oversized and oversized.error() == std::errc::not_supported);

            const auto absent = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
            PPR_TEST_ASSERT(absent.has_value() and not *absent);
            PPR_TEST_ASSERT(fixture.m_pass.terrainPipelineReadinessForQualification() == (std::array<bool, 3>{}));
        }

        std::array<u16, 49> labels{};
        labels[24u] = 17u;
        const std::array<lighting::OpticalTerrainMaterial, 2> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f},
            {lighting::OpticalCell{{}, {}, {8.0, 2.0, 1.0}, lighting::OpticalKind::opaque}, 17u, 0.6f}};
        const auto captured = lighting::OpticalTerrainSnapshot::capture(
            {labels, materials, {}, 1.0, 7u, 7u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(captured.has_value());
        const auto geometry = std::make_shared<const lighting::OpticalTerrainSnapshot>(*captured);
        const auto captured_receivers = lighting::TerrainReceiverSet::capture(geometry);
        PPR_TEST_ASSERT(captured_receivers.has_value());
        const auto receivers = std::make_shared<const lighting::TerrainReceiverSet>(*captured_receivers);
        const lighting::TerrainReceiverCohort cohort{3u, 5u, 7u, 11u};
        const auto request = cascadeRequest_(7u, 7u);
        std::array<lighting::OpticalCell, 49> cells{};
        cells.fill(vacuum_());
        cells[24u] = materials[1].m_optics;
        const lighting::OpticalGridView grid{cells, {}, 1.0, 7u, 7u, lighting::OpticalBoundary::vacuum};
        const auto reference = [&] {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.CpuC1Reference"};
            phase("cpu-c1-reference.begin");
            auto value = solveCpuCascades_(grid, request, false, false, geometry.get(), 1u);
            phase("cpu-c1-reference.end");
            return value;
        }();

        auto tiny_materials = materials;
        tiny_materials[1].m_optics.m_surface_emission = {
            std::ldexp(1.0, -120), std::ldexp(1.0, -120), std::ldexp(1.0, -120)};
        const auto tiny_shape = lighting::OpticalTerrainSnapshot::capture(
            {labels, tiny_materials, {}, 1.0, 7u, 7u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(tiny_shape.has_value());
        const auto tiny_set = lighting::TerrainReceiverSet::capture(
            std::make_shared<const lighting::OpticalTerrainSnapshot>(*tiny_shape));
        PPR_TEST_ASSERT(tiny_set.has_value());
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainReceivers(
                std::make_shared<const lighting::TerrainReceiverSet>(*tiny_set), cohort, request,
                fixture.m_fence.completed()) == std::errc::argument_out_of_domain);

        auto too_small = request;
        too_small.m_budget_bytes = 1u;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainReceivers(receivers, cohort, too_small, fixture.m_fence.completed()) ==
            std::errc::no_buffer_space);

        {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.Prepare"};
            phase("prepare.begin");
            PPR_TEST_ASSERT(
                not fixture.m_pass.prepareTerrainReceivers(receivers, cohort, request, fixture.m_fence.completed()));
            phase("prepare.end");
        }

        const auto prepared_statistics = fixture.m_pass.readTerrainPrecisionStatistics(fixture.m_fence.completed());
        PPR_TEST_ASSERT(prepared_statistics.has_value() and not *prepared_statistics);

        {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.EncodeSubmitWait"};
            phase("encode-submit-wait.begin");
            fixture.submit(true, true);
            phase("encode-submit-wait.end");
        }

        fixture.observePhase("receiver.readback.begin");
        const auto result = fixture.m_pass.readTerrainReceivers(fixture.m_fence.completed());
        if (not result)
            fixture.observeReadFailure(result.error());
        PPR_TEST_ASSERT(result.has_value() and result->size() == receivers->features().size());

        fixture.observePhase("receiver.readback.end");

        const auto precision_statistics = fixture.m_pass.readTerrainPrecisionStatistics(fixture.m_fence.completed());
        PPR_TEST_ASSERT(precision_statistics.has_value());
        PPR_TEST_ASSERT(precision_statistics->has_value() == production_refined);
        if (production_refined) {
            const auto &statistics = **precision_statistics;
            u64 query_bound = receivers->features().size() * 64u;
            for (std::size_t level = 1u; level < reference.m_layout.m_cascades.size(); ++level) {
                const auto &cascade = reference.m_layout.m_cascades[level];
                const bool top = level + 1u == reference.m_layout.m_cascades.size();
                query_bound += cascade.m_resident_records * (top ? 1u : 8u);
            }

            PPR_TEST_ASSERT(statistics.m_query_upper_bound == query_bound);
            PPR_TEST_ASSERT(query_bound > 0u and query_bound <= std::numeric_limits<u32>::max());
            PPR_TEST_ASSERT(u64{statistics.m_attempted} <= query_bound);
            PPR_TEST_ASSERT(u64{statistics.m_qualified} + statistics.m_unresolved == statistics.m_attempted);
            PPR_TEST_ASSERT(statistics.m_unresolved == 0u and statistics.m_bound_error == 0u);
            std::println(
                "PRODUCTION_PRECISION_RECEIVERS bound={} attempted={} qualified={} unresolved={} bound_error={}",
                query_bound, statistics.m_attempted, statistics.m_qualified, statistics.m_unresolved,
                statistics.m_bound_error);

            // Re-encode the same completed prepared batch before publication.
            // Its new epoch must preserve values/counts instead of accumulating.
            {
                const hal::ProfileScope scope{"Tests.Radiance.Receiver.EncodeSubmitWait"};
                phase("repeat-encode-submit-wait.begin");
                fixture.submit(true, true);
                phase("repeat-encode-submit-wait.end");
            }

            const auto repeated = fixture.m_pass.readTerrainReceivers(fixture.m_fence.completed());
            PPR_TEST_ASSERT(repeated.has_value() and repeated->size() == result->size());
            for (std::size_t index = 0u; index < result->size(); ++index) {
                PPR_TEST_ASSERT((*repeated)[index].m_status == (*result)[index].m_status);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT((*repeated)[index].m_mean[channel] == (*result)[index].m_mean[channel]);
                }
            }

            const auto repeated_statistics = fixture.m_pass.readTerrainPrecisionStatistics(fixture.m_fence.completed());
            PPR_TEST_ASSERT(repeated_statistics.has_value() and repeated_statistics->has_value());
            PPR_TEST_ASSERT((**repeated_statistics).m_query_upper_bound == query_bound);
            PPR_TEST_ASSERT((**repeated_statistics).m_attempted == statistics.m_attempted);
            PPR_TEST_ASSERT((**repeated_statistics).m_qualified == statistics.m_qualified);
            PPR_TEST_ASSERT((**repeated_statistics).m_unresolved == 0u and (**repeated_statistics).m_bound_error == 0u);
        }

        std::vector<lighting::OpticalRgb> angular_expected(result->size() * 16u);
        {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.OracleComparison"};
            phase("oracle-comparison.begin");
            const auto &parent = reference.m_layout.m_cascades[1];
            for (u32 key = 0u; key < result->size(); ++key) {
                const auto point = receivers->point(key);
                PPR_TEST_ASSERT(point.has_value());
                const std::array<double, 2> lattice{((*point)[0] - 0.5) / 2.0, ((*point)[1] - 0.5) / 2.0};
                lighting::OpticalRgb expected{};
                for (u32 bin = 0u; bin < 16u; ++bin) {
                    for (u32 y = 0u; y < 2u; ++y) {
                        for (u32 x = 0u; x < 2u; ++x) {
                            const double fx = lattice[0] - std::floor(lattice[0]),
                                         fy = lattice[1] - std::floor(lattice[1]);
                            const double weight = (x == 0u ? 1.0 - fx : fx) * (y == 0u ? 1.0 - fy : fy);
                            if (weight == 0.0) {
                                continue;
                            }

                            const u32 px = static_cast<u32>(x == 0u ? std::floor(lattice[0]) : std::ceil(lattice[0]));
                            const u32 py = static_cast<u32>(y == 0u ? std::floor(lattice[1]) : std::ceil(lattice[1]));
                            const auto direction = reference.m_directions[1][bin];
                            const std::array<double, 2> target{
                                0.5 + 2.0 * px + direction[0], 0.5 + 2.0 * py + direction[1]};
                            const auto connector = lighting::traceTerrainReceiverConnectorReference(
                                *receivers, key, target);
                            PPR_TEST_ASSERT(connector.has_value());
                            const auto &upper =
                                reference
                                    .m_intervals[1][(std::size_t(py) * parent.m_padded_probes[0] + px) * 16u + bin];
                            const auto pair = lighting::composeIntervals(connector->m_interval, upper);
                            PPR_TEST_ASSERT(pair.has_value());
                            for (u32 channel = 0u; channel < 3u; ++channel) {
                                expected[channel] += (1.0 / 16.0) * weight * pair->m_radiance[channel];
                                angular_expected[key * 16u + bin][channel] += weight * pair->m_radiance[channel];
                            }
                        }
                    }
                }

                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const double actual = static_cast<double>((*result)[key].m_mean[channel]);
                    PPR_TEST_ASSERT(std::abs(actual - expected[channel]) <= 1e-5 + 1e-4 * std::abs(expected[channel]));
                }
            }

            phase("oracle-comparison.end");
        }

        const auto acquired = [&] {
            const hal::ProfileScope scope{"Tests.Radiance.Receiver.PublishGeneration"};
            phase("publish-generation.begin");
            auto value = fixture.m_pass.acquireTerrainReceiverView(fixture.m_fence.completed());
            phase("publish-generation.end");
            return value;
        }();
        PPR_TEST_ASSERT(acquired.has_value());
        const auto generation = *acquired;
        PPR_TEST_ASSERT(generation->receivers() == receivers and generation->cohort() == cohort);
        PPR_TEST_ASSERT(generation->featuresBuffer()->getDesc().elementSize == 32u);
        PPR_TEST_ASSERT(generation->resultsBuffer()->getDesc().elementSize == 16u);
        PPR_TEST_ASSERT(generation->c1Buffer() != nullptr and generation->bufferBytes() > 0u);
        PPR_TEST_ASSERT((generation->precisionStatistics() != nullptr) == production_refined);
        if (production_refined) {
            PPR_TEST_ASSERT(
                generation->precisionStatistics()->m_query_upper_bound == (**precision_statistics).m_query_upper_bound);
            PPR_TEST_ASSERT(generation->precisionStatistics()->m_attempted == (**precision_statistics).m_attempted);
            PPR_TEST_ASSERT(generation->precisionStatistics()->m_qualified == (**precision_statistics).m_qualified);
            PPR_TEST_ASSERT(generation->precisionStatistics()->m_unresolved == 0u);
            PPR_TEST_ASSERT(generation->precisionStatistics()->m_bound_error == 0u);
        }

        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainReceivers(receivers, cohort, too_small, fixture.m_fence.completed()) ==
            std::errc::no_buffer_space);

        PPR_TEST_ASSERT(generation->angularBuffer() == nullptr and generation->angularDirectionCount() == 0u);
        const auto absent_angular = fixture.m_pass.readTerrainReceiverAngular(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not absent_angular and absent_angular.error() == std::errc::operation_not_permitted);

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainReceivers(
            receivers, cohort, request, fixture.m_fence.completed(), false,
            lighting::TerrainReceiverOutput::angular16));

        fixture.submit(true, true);

        const auto angular_mean = fixture.m_pass.readTerrainReceivers(fixture.m_fence.completed());
        const auto angular_rows = fixture.m_pass.readTerrainReceiverAngular(fixture.m_fence.completed());
        PPR_TEST_ASSERT(
            angular_mean.has_value() and angular_rows.has_value() and angular_rows->size() == angular_expected.size());
        for (std::size_t key = 0u; key < result->size(); ++key) {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    std::bit_cast<u32>((*angular_mean)[key].m_mean[channel]) ==
                    std::bit_cast<u32>((*result)[key].m_mean[channel]));
            }
        }

        for (std::size_t row = 0u; row < angular_rows->size(); ++row) {
            PPR_TEST_ASSERT((*angular_rows)[row].m_status == 0u);
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double expected = angular_expected[row][channel];
                PPR_TEST_ASSERT(
                    std::abs(double((*angular_rows)[row].m_radiance[channel]) - expected) <=
                    1e-5 + 1e-4 * std::abs(expected));
            }
        }

        const auto angular_acquired = fixture.m_pass.acquireTerrainReceiverView(fixture.m_fence.completed());
        PPR_TEST_ASSERT(angular_acquired.has_value());
        const auto angular_generation = *angular_acquired;
        PPR_TEST_ASSERT(
            angular_generation->angularDirectionOffset() == 8u and angular_generation->angularDirectionCount() == 16u);
        PPR_TEST_ASSERT(
            angular_generation->angularBuffer()->getDesc().elementSize == sizeof(lighting::TerrainReceiverAngular));
        PPR_TEST_ASSERT(angular_generation->directionsBuffer()->getDesc().elementSize == sizeof(ReceiverDirection));
        PPR_TEST_ASSERT(
            angular_generation->bufferBytes() ==
            generation->bufferBytes() + angular_rows->size() * sizeof(lighting::TerrainReceiverAngular));

        GridPass grid_pass{};
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid_pass.shutdown());
        };
        PPR_TEST_ASSERT(not grid_pass.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        GridStagedFrame stage{};
        stage.m_full_refresh = stage.m_has_catalog = stage.m_reset_caches = true;
        stage.m_boundary_material_id = 0u;
        stage.m_content_fingerprint = cohort.m_content_fingerprint;
        stage.m_optical_geometry = geometry;
        stage.m_optical_revision = cohort.m_geometry_revision;
        GridTerrainVisual visual{};
        visual.m_material_id = 17u;
        visual.m_roundness = 0.6f;
        visual.m_transport_model = kGridTransportOpaqueV1;
        visual.m_emissive_rgb[0] = 1.0f;
        visual.m_emissive_rgb[1] = 0.25f;
        visual.m_emissive_rgb[2] = 0.125f;
        visual.m_emissive_strength = 8.0f;
        stage.m_terrain_visuals.push_back(visual);
        stage.m_tiles.push_back({.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
        GridStagedUpload upload{};
        upload.m_payload.m_rect[2] = upload.m_payload.m_rect[3] = 128.0f;
        upload.m_cell_materials.resize(GridPass::kCellsPerChunk, 0u);
        upload.m_cell_materials[3u * 128u + 3u] = 17u;
        stage.m_uploads.push_back(std::move(upload));

        // A complete optical cohort replaces the lookup, including neighbors
        // outside its draw table. Seed a nonzero resident neighbor first.
        auto neighbor = stage;
        neighbor.m_optical_geometry.reset();
        neighbor.m_optical_revision = 0u;
        neighbor.m_reset_caches = false;
        neighbor.m_boundary_material_id = 17u;
        neighbor.m_tiles[0].m_chunk_id = 1u;
        neighbor.m_tiles[0].m_tile_range = {128, 0, 256, 128};
        neighbor.m_uploads[0].m_chunk_id = 1u;
        neighbor.m_uploads[0].m_payload.m_rect[0] = 128.0f;
        neighbor.m_uploads[0].m_payload.m_rect[2] = 256.0f;
        std::ranges::fill(neighbor.m_uploads[0].m_cell_materials, u16{17u});
        PPR_TEST_ASSERT(not grid_pass.applyStagedFrame(neighbor));
        const auto neighbor_ready = grid_pass.tryPrepareCellUploads(
            fixture.m_rhi->getDevice(), fixture.m_fence.completed());
        PPR_TEST_ASSERT(neighbor_ready.has_value() and *neighbor_ready);
        PPR_TEST_ASSERT(grid_pass.resolveCached(1u).has_value());

        auto missing_reset = stage;
        missing_reset.m_reset_caches = false;
        PPR_TEST_ASSERT(grid_pass.applyStagedFrame(missing_reset) == std::errc::function_not_supported);
        PPR_TEST_ASSERT(grid_pass.resolveCached(1u).has_value() and grid_pass.cachedTileCount() == 1u);

        PPR_TEST_ASSERT(not grid_pass.applyStagedFrame(stage));
        const auto replacement_ready = grid_pass.tryPrepareCellUploads(
            fixture.m_rhi->getDevice(), fixture.m_fence.completed());
        PPR_TEST_ASSERT(replacement_ready.has_value() and *replacement_ready);
        PPR_TEST_ASSERT(grid_pass.resolveCached(0u).has_value() and grid_pass.cachedTileCount() == 1u);
        PPR_TEST_ASSERT(not grid_pass.resolveCached(1u).has_value());

        PPR_TEST_ASSERT(not grid_pass.stageWorldCandidate(cohort.m_job_id, cohort.m_world_generation, stage));
        PPR_TEST_ASSERT(grid_pass.setTerrainReceiverLighting(generation) == std::errc::invalid_argument);
        const auto prepared = grid_pass.advanceWorldCandidate(fixture.m_rhi->getDevice(), fixture.m_fence.completed());
        PPR_TEST_ASSERT(prepared.has_value() and *prepared);
        PPR_TEST_ASSERT(not grid_pass.activateWorldCandidate(
            cohort.m_job_id, cohort.m_world_generation, cohort.m_content_fingerprint));
        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting(generation));
        const CameraSnapshot normal_camera{};
        PPR_TEST_ASSERT(
            grid_pass.update(
                TimeSpan{}, normal_camera, false, GridPass::ReceiverContinuation::nearest,
                GridPass::ReceiverResponse::mapped_diffuse) == std::errc::invalid_argument);

        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting(angular_generation));
        PPR_TEST_ASSERT(not grid_pass.update(
            TimeSpan{}, normal_camera, false, GridPass::ReceiverContinuation::nearest,
            GridPass::ReceiverResponse::mapped_diffuse));

        // Publish the next flat generation before selecting its response, as
        // the real frame boundary does. The previous mapped frame cannot veto
        // valid mean-only data, but consuming that data as mapped still fails.
        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting(generation));
        PPR_TEST_ASSERT(
            grid_pass.update(
                TimeSpan{}, normal_camera, false, GridPass::ReceiverContinuation::nearest,
                GridPass::ReceiverResponse::mapped_diffuse) == std::errc::invalid_argument);

        PPR_TEST_ASSERT(not grid_pass.update(TimeSpan{}, normal_camera));
        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting(generation));


        // Upload-free legacy policy changes cannot relabel a compatible resident
        // owner, and their rejection leaves the current completed view usable.
        auto policy_change = stage;
        policy_change.m_optical_geometry.reset();
        policy_change.m_optical_revision = 0u;
        policy_change.m_reset_caches = false;
        policy_change.m_uploads.clear();
        policy_change.m_tiles.clear();
        policy_change.m_boundary_material_id = 17u;
        PPR_TEST_ASSERT(grid_pass.applyStagedFrame(policy_change) == std::errc::function_not_supported);
        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting(generation));

        // Equal cell words do not authorize a foreign geometry owner/revision.
        // Reset removes admission of the old owner; the checked replacement
        // becomes current only at resident publication. Encoded roots retain
        // their own strong leases independently of this CPU-side admission.
        stage.m_optical_geometry = std::make_shared<const lighting::OpticalTerrainSnapshot>(*geometry);
        ++stage.m_optical_revision;
        PPR_TEST_ASSERT(not grid_pass.applyStagedFrame(stage));
        PPR_TEST_ASSERT(grid_pass.setTerrainReceiverLighting(generation) == std::errc::invalid_argument);
        const auto republished = grid_pass.tryPrepareCellUploads(
            fixture.m_rhi->getDevice(), fixture.m_fence.completed());
        PPR_TEST_ASSERT(republished.has_value() and *republished);
        PPR_TEST_ASSERT(grid_pass.setTerrainReceiverLighting(generation) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not grid_pass.setTerrainReceiverLighting({}));

        const auto angular_bytes = angular_rows->size() * sizeof(lighting::TerrainReceiverAngular);
        const auto mean_bytes = result->size() * sizeof(lighting::TerrainReceiverResult);
        field_scope.m_receiver_lease_check = [angular_generation, generation, angular_bytes, mean_bytes,
                                                 production_refined, precision_statistics] {
            PPR_TEST_ASSERT(angular_generation->angularBuffer()->getDesc().size == angular_bytes);
            PPR_TEST_ASSERT(angular_generation->directionsBuffer() != nullptr);

            PPR_TEST_ASSERT(generation->resultsBuffer()->getDesc().size == mean_bytes);
            if (production_refined) {
                PPR_TEST_ASSERT(generation->precisionStatistics()->m_attempted == (**precision_statistics).m_attempted);
                PPR_TEST_ASSERT(generation->precisionStatistics()->m_qualified == (**precision_statistics).m_qualified);
            }
        };

        field_scope.m_receiver_completed = true;
        fixture.observePhase("receiver.scenario.verdict.pass");
    };

    PPR_UNIT_TEST (fp32_scalar_trace_matches_quantized_optical_oracle) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        std::array<lighting::OpticalCell, 32> cells{};
        cells.fill(vacuum_());
        for (std::size_t row = 0u; row < 4u; ++row) {
            cells[row * 8u + 2u] = lighting::OpticalCell{
                {0.5, 1.0, 0.0}, {0.25, 0.5, 0.125}, {}, lighting::OpticalKind::absorbing};
            cells[row * 8u + 6u] = lighting::OpticalCell{{}, {}, {8.0, 4.0, 2.0}, lighting::OpticalKind::opaque};
        }

        // Binary-exact local coordinates/coefficients/metric ensure the oracle
        // consumes the uploaded numeric field. Large world translation is removed
        // in host double before upload; physical quantization is a separate gate.
        const lighting::OpticalGridView grid{
            cells, {1000000.0, -1000000.0}, 0.25, 8u, 4u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 8> rays{
            lighting::OpticalRay{{1000000.0, -999999.875}, {1.0, 0.0}, 0.0, 2.0},
            lighting::OpticalRay{{1000002.0, -999999.875}, {-1.0, 0.0}, 0.0, 2.0},
            lighting::OpticalRay{{1000000.0, -999999.75}, {1.0, 0.0}, 0.0, 2.0},
            lighting::OpticalRay{{1000000.0, -1000000.0}, {1.0, 1.0}, 0.0, 4.0},
            lighting::OpticalRay{{1000001.0, -999999.5}, {0.0, -1.0}, 0.0, 2.0},
            lighting::OpticalRay{{1000000.5, -999999.5}, {1.0, 0.0}, 0.125, 0.625},
            lighting::OpticalRay{{1000000.0, -999999.0}, {1.0, 0.0}, 0.0, 2.0},
            lighting::OpticalRay{{1000000.0, -999999.875}, {1.0, 0.0}, 0.5, 0.5}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(grid, rays, 1024u * 1024u, 65536u, 65536u, kFenceValueNone));
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, false, false}));

        fixture.submit();
        const auto snapshot = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(snapshot.has_value() and snapshot->m_intervals.size() == rays.size());

        lighting::OpticalGridView quantized_grid = grid;
        quantized_grid.m_min_m = {};
        quantized_grid.m_cell_size_m = double(float(grid.m_cell_size_m));
        for (std::size_t index = 0u; index < rays.size(); ++index) {
            const auto normalized = lighting::normalizeOpticalRay(rays[index]);
            PPR_TEST_ASSERT(normalized.has_value());
            lighting::OpticalRay quantized = *normalized;
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const double local = (rays[index].m_origin_m[axis] - grid.m_min_m[axis]) / grid.m_cell_size_m;
                quantized.m_origin_m[axis] = double(float(local)) * quantized_grid.m_cell_size_m;
                quantized.m_direction[axis] = double(float(normalized->m_direction[axis]));
            }

            quantized.m_near_m = double(float(quantized.m_near_m));
            quantized.m_far_m = double(float(quantized.m_far_m));
            const auto expected = lighting::traceOpticalReference(quantized_grid, quantized);
            PPR_TEST_ASSERT(expected.has_value());
            assertClose_(snapshot->m_intervals[index], *expected);
            PPR_TEST_ASSERT(snapshot->m_segments[index] <= grid.m_width + grid.m_height);
        }

        const lighting::OpticalCell medium{
            {0.5, 0.25, 0.0}, {0.25, 0.125, 0.03125}, {}, lighting::OpticalKind::absorbing};
        const std::vector<lighting::OpticalCell> corridor(513u, medium);
        const lighting::OpticalGridView long_grid{
            corridor, {0.0, 0.0}, 0.25, 513u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> long_rays{
            lighting::OpticalRay{{0.0, 0.125}, {1.0, 0.0}, 0.0, 128.25}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            long_grid, long_rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        fixture.submit();
        const auto long_result = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        const auto long_expected = lighting::traceOpticalReference(long_grid, long_rays[0]);
        PPR_TEST_ASSERT(long_result.has_value() and long_expected.has_value());
        PPR_TEST_ASSERT(long_result->m_segments[0] == 513u);
        assertClose_(long_result->m_intervals[0], *long_expected);
    };

    // ------------------------------------------------------------------
    // immutable GPU vacuum hierarchy and rebuild observations
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (vacuum_hierarchy_preserves_optics_and_rebuilds_captured_edits) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const std::array<lighting::OpticalCell, 1> single{vacuum_()};
        const lighting::OpticalGridView single_grid{single, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> single_ray{lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 1.0}};

        // 40 source +24 ray +32 output +32 readback +4 flags +16 metadata.
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(single_grid, single_ray, 147u, 0u, 65536u, fixture.m_fence.completed()) ==
            std::errc::no_buffer_space);
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(single_grid, single_ray, 148u, 0u, 65536u, fixture.m_fence.completed()));

        // Candidate and previous immutable resources coexist during replacement.
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(single_grid, single_ray, 295u, 0u, 65536u, fixture.m_fence.completed()) ==
            std::errc::no_buffer_space);
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(single_grid, single_ray, 296u, 0u, 65536u, fixture.m_fence.completed()));

        for (const auto dimensions:
            std::array<std::array<u32, 2>, 5>{std::array<u32, 2>{64u, 64u}, {9u, 7u}, {1u, 9u}, {9u, 1u}, {1u, 1u}}) {
            const u32 width = dimensions[0];
            const u32 height = dimensions[1];
            std::vector<lighting::OpticalCell> cells(std::size_t(width) * height, vacuum_());
            const lighting::OpticalGridView grid{cells, {}, 1.0, width, height, lighting::OpticalBoundary::vacuum};
            const std::array<lighting::OpticalRay, 5> rays{
                lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, double(width)},
                lighting::OpticalRay{{double(width), 0.5}, {-1.0, 0.0}, 0.0, double(width)},
                lighting::OpticalRay{{0.5, 0.0}, {0.0, 1.0}, 0.0, double(height)},
                lighting::OpticalRay{{0.5, double(height)}, {0.0, -1.0}, 0.0, double(height)},
                lighting::OpticalRay{{0.0, 0.0}, {1.0, 1.0}, 0.0, 128.0}};
            const auto hierarchy = lighting::OpticalHierarchy::capture(grid);
            PPR_TEST_ASSERT(hierarchy.has_value());
            PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
                grid, rays, 4u * 1024u * 1024u, 65536u, 1024u * 1024u, fixture.m_fence.completed()));
            fixture.submit();
            const auto actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(actual.has_value());
            for (std::size_t index = 0u; index < rays.size(); ++index) {
                const auto reference = lighting::traceOpticalReference(grid, rays[index]);
                const auto accelerated = lighting::traceOpticalAccelerated(*hierarchy, rays[index]);
                PPR_TEST_ASSERT(reference.has_value() and accelerated.has_value());
                assertClose_(actual->m_intervals[index], *reference);
                PPR_TEST_ASSERT(actual->m_segments[index] == accelerated->m_segments);
            }

            if (width == 64u and height == 64u) {
                // One aligned block replaces 64 cell spans, without a timing claim.
                PPR_TEST_ASSERT(actual->m_segments[0] == 1u);
                std::println(
                    "GPU vacuum hierarchy: axis_segments={} plain_cell_spans=64 flags_bytes=21844 metadata_bytes=112",
                    actual->m_segments[0]);
            }
        }

        std::vector<lighting::OpticalCell> cells(64u, vacuum_());
        const lighting::OpticalGridView grid{cells, {}, 1.0, 8u, 8u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 2> rays{
            lighting::OpticalRay{{0.0, 3.5}, {1.0, 0.0}, 0.0, 8.0},
            lighting::OpticalRay{{8.0, 3.5}, {-1.0, 0.0}, 0.0, 8.0}};
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(grid, rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        cells[3u * 8u + 3u] = lighting::OpticalCell{
            {0.5, 0.25, 0.0}, {0.25, 0.125, 0.03125}, {}, lighting::OpticalKind::absorbing};
        cells[3u * 8u + 6u] = lighting::OpticalCell{{}, {}, {8.0, 4.0, 2.0}, lighting::OpticalKind::opaque};

        // Caller edits after prepare cannot mutate the captured GPU source or
        // its subsequently constructed hierarchy. Explicit reprepare rebuilds it.
        fixture.submit();
        const auto captured = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(captured.has_value());
        for (std::size_t index = 0u; index < rays.size(); ++index) {
            assertClose_(captured->m_intervals[index], {});
            PPR_TEST_ASSERT(captured->m_segments[index] == 1u);
        }

        const auto hierarchy = lighting::OpticalHierarchy::capture(grid);
        PPR_TEST_ASSERT(hierarchy.has_value());
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(grid, rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        fixture.submit();
        const auto rebuilt = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(rebuilt.has_value());
        for (std::size_t index = 0u; index < rays.size(); ++index) {
            const auto expected = lighting::traceOpticalReference(grid, rays[index]);
            const auto accelerated = lighting::traceOpticalAccelerated(*hierarchy, rays[index]);
            PPR_TEST_ASSERT(expected.has_value() and accelerated.has_value());
            assertClose_(rebuilt->m_intervals[index], *expected);
            PPR_TEST_ASSERT(rebuilt->m_segments[index] == accelerated->m_segments);
        }

        PPR_TEST_ASSERT(rebuilt->m_intervals[0].m_radiance[0] > 0.0);
        const auto request = cascadeRequest_(8u, 8u);
        const auto expected_rc = solveCpuCascades_(grid, request);
        PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()));
        std::ranges::fill(cells, vacuum_());
        fixture.submit(true);
        const auto captured_rc = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(captured_rc.has_value());
        for (std::size_t record = 0u; record < captured_rc->m_intervals.size(); ++record) {
            assertClose_(captured_rc->m_intervals[record], expected_rc.m_intervals[0][record]);
        }

        PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()));
        fixture.submit(true);
        const auto rebuilt_rc = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(rebuilt_rc.has_value());
        for (const auto &interval: rebuilt_rc->m_intervals) {
            assertClose_(interval, {});
        }
    };

    // ------------------------------------------------------------------
    // admission, arithmetic rejection and completion-safe lifetime
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (admission_quantization_and_abandoned_batches_preserve_safe_reuse) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const std::array<lighting::OpticalCell, 1> cells{vacuum_()};
        const lighting::OpticalGridView grid{cells, {0.0, 0.0}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> rays{lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 1.0}};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(grid, rays, 0u, 0u, 65536u, kFenceValueNone) == std::errc::no_buffer_space);
        const auto absent = fixture.m_pass.readCompleted(kFenceValueNone);
        PPR_TEST_ASSERT(not absent and absent.error() == std::errc::operation_not_permitted);

        auto collapsed = rays;
        collapsed[0].m_near_m = 1.0;
        collapsed[0].m_far_m = 1.0 + 1e-12;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(grid, collapsed, 65536u, 0u, 65536u, kFenceValueNone) ==
            std::errc::result_out_of_range);

        // These rays/coefficients upload exactly, but FP32 plane-event
        // subtraction loses an opaque cell even though the whole ray interval
        // remains positive. Validate crossings, not only scalar conversion.
        const lighting::OpticalCell emitter{{}, {}, {8.0, 4.0, 2.0}, lighting::OpticalKind::opaque};
        const std::array<lighting::OpticalCell, 4> strip{emitter, vacuum_(), vacuum_(), vacuum_()};
        const lighting::OpticalGridView strip_grid{strip, {}, 1.0, 4u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 4> unresolvable{
            lighting::OpticalRay{{-33554432.0, 0.5}, {1.0, 0.0}, 0.0, 33554436.0},
            lighting::OpticalRay{{-33554432.0, 0.5}, {1.0, 0.0}, 33554432.0, 33554436.0},
            lighting::OpticalRay{{33554436.0, 0.5}, {-1.0, 0.0}, 0.0, 33554436.0},
            lighting::OpticalRay{{33554436.0, 0.5}, {-1.0, 0.0}, 33554432.0, 33554436.0}};
        for (const auto &ray: unresolvable) {
            const auto reference = lighting::traceOpticalReference(strip_grid, ray);
            PPR_TEST_ASSERT(reference.has_value());
            assertClose_(*reference, {{8.0, 4.0, 2.0}, {}});

            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTrace(strip_grid, std::span{&ray, 1u}, 65536u, 0u, 65536u, kFenceValueNone) ==
                std::errc::result_out_of_range);
        }

        // The entire one-cell world intersection can collapse despite a
        // positive representable requested interval, in either direction.
        const std::array<lighting::OpticalCell, 1> single{emitter};
        const lighting::OpticalGridView single_grid{single, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        for (const auto &ray: std::array<lighting::OpticalRay, 2>{
                 unresolvable[0], lighting::OpticalRay{{33554432.0, 0.5}, {-1.0, 0.0}, 0.0, 33554432.0}}) {
            const auto reference = lighting::traceOpticalReference(single_grid, ray);
            PPR_TEST_ASSERT(reference.has_value());
            assertClose_(*reference, {{8.0, 4.0, 2.0}, {}});

            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTrace(single_grid, std::span{&ray, 1u}, 65536u, 0u, 65536u, kFenceValueNone) ==
                std::errc::result_out_of_range);
        }

        const lighting::OpticalGridView vertical_grid{strip, {}, 1.0, 1u, 4u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> vertical{
            lighting::OpticalRay{{0.5, -33554432.0}, {0.0, 1.0}, 0.0, 33554436.0}};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(vertical_grid, vertical, 65536u, 0u, 65536u, kFenceValueNone) ==
            std::errc::result_out_of_range);

        // A far-origin ray pointing away has no world crossings to resolve.
        const std::array<lighting::OpticalRay, 1> away{
            lighting::OpticalRay{{-33554432.0, 0.5}, {-1.0, 0.0}, 0.0, 33554436.0}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(single_grid, away, 65536u, 0u, 65536u, kFenceValueNone));

        // Raw FP32 subnormal operands are unsupported even when an empty
        // interval or a world-boundary axis shortcut could otherwise hide them.
        const double tiny = double((std::numeric_limits<float>::denorm_min)());
        const lighting::OpticalRay tiny_direction{{0.0, 0.25}, {-tiny, 1.0}, 0.0, 0.125};
        const auto tiny_reference = lighting::traceOpticalReference(single_grid, tiny_direction);
        PPR_TEST_ASSERT(tiny_reference.has_value());
        assertClose_(*tiny_reference, {});

        for (const double far: {0.125, 0.0}) {
            auto ray = tiny_direction;
            ray.m_far_m = far;
            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTrace(single_grid, std::span{&ray, 1u}, 65536u, 0u, 65536u, kFenceValueNone) ==
                std::errc::argument_out_of_domain);
        }

        const double tiny_coefficient = std::ldexp(1.0, -127);
        const double huge_metric = std::ldexp(1.0, 127);
        const lighting::OpticalRay huge_ray{{0.0, huge_metric * 0.5}, {1.0, 0.0}, 0.0, huge_metric};
        for (const bool volume_source: {false, true}) {
            lighting::OpticalCell medium{};
            medium.m_kind = lighting::OpticalKind::absorbing;
            (volume_source ? medium.m_volume_emission : medium.m_extinction)[0] = tiny_coefficient;
            const std::array<lighting::OpticalCell, 1> medium_cells{medium};
            const lighting::OpticalGridView medium_grid{
                medium_cells, {}, huge_metric, 1u, 1u, lighting::OpticalBoundary::vacuum};
            const auto reference = lighting::traceOpticalReference(medium_grid, huge_ray);
            PPR_TEST_ASSERT(reference.has_value());
            assertClose_(
                *reference, volume_source ? lighting::RadianceInterval{{1.0, 0.0, 0.0}, {1.0, 1.0, 1.0}}
                                          : lighting::RadianceInterval{{}, {std::exp(-1.0), 1.0, 1.0}});

            PPR_TEST_ASSERT(
                fixture.m_pass.prepareTrace(
                    medium_grid, std::span{&huge_ray, 1u}, 65536u, 0u, 65536u, kFenceValueNone) ==
                std::errc::argument_out_of_domain);
        }

        auto tiny_metric_grid = grid;
        tiny_metric_grid.m_cell_size_m = tiny;
        const lighting::OpticalRay tiny_metric_ray{{0.0, tiny * 0.5}, {1.0, 0.0}, 0.0, tiny};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(
                tiny_metric_grid, std::span{&tiny_metric_ray, 1u}, 65536u, 0u, 65536u, kFenceValueNone) ==
            std::errc::argument_out_of_domain);
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareCascades(tiny_metric_grid, cascadeRequest_(1u, 1u), kFenceValueNone) ==
            std::errc::argument_out_of_domain);

        auto weak_surface = emitter;
        weak_surface.m_surface_emission[0] = tiny_coefficient;
        const std::array<lighting::OpticalCell, 1> weak_cells{weak_surface};
        const lighting::OpticalGridView weak_grid{weak_cells, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(weak_grid, rays, 65536u, 0u, 65536u, kFenceValueNone) ==
            std::errc::argument_out_of_domain);

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(grid, rays, 65536u, 0u, 65536u, kFenceValueNone));

        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        PPR_TEST_ASSERT(not fixture.m_pass.encodeTrace({fixture.m_rhi->getDevice(), *encoder, kFenceValueNone, {}}));
        PPR_TEST_ASSERT(fixture.m_pass.shutdown(kFenceValueNone) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(grid, rays, 65536u, 0u, 65536u, kFenceValueNone) ==
            std::errc::device_or_resource_busy);

        encoder.setNull();
        PPR_TEST_ASSERT(not fixture.m_pass.discardUnsubmitted());
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(grid, rays, 65536u, 0u, 65536u, kFenceValueNone));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        PPR_TEST_ASSERT(not fixture.m_pass.encodeTrace({fixture.m_rhi->getDevice(), *encoder, kFenceValueNone, {}}));
        encoder.setNull();

        // Deterministic ambiguous-submit notification exercises quarantine without
        // issuing an unsafe real failing submission or pretending completion.
        PPR_TEST_ASSERT(not fixture.m_pass.notifySubmitFailed());
        PPR_TEST_ASSERT(fixture.m_pass.shutdown(kFenceValueNone) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(fixture.m_pass.shutdown(GpuQuiescenceResult{}) == std::errc::state_not_recoverable);
    };

    // ------------------------------------------------------------------
    // visible rejection of positive FP32 arithmetic underflow
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (generic_cell_positive_underflow_never_returns_a_complete_interval) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        const auto reject_arithmetic = [&](const lighting::OpticalGridView &grid, const lighting::OpticalRay &ray,
                                           const lighting::RadianceInterval &analytic) {
            const auto reference = lighting::traceOpticalReference(grid, ray);
            PPR_TEST_ASSERT(reference.has_value());
            assertClose_(*reference, analytic);
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                if (analytic.m_radiance[channel] > 0.0) {
                    PPR_TEST_ASSERT(
                        std::abs(reference->m_radiance[channel] - analytic.m_radiance[channel]) <=
                        analytic.m_radiance[channel] * 1e-12);
                }

                if (analytic.m_transmittance[channel] > 0.0) {
                    PPR_TEST_ASSERT(
                        std::abs(reference->m_transmittance[channel] - analytic.m_transmittance[channel]) <=
                        analytic.m_transmittance[channel] * 1e-12);
                }
            }

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
                grid, std::span{&ray, 1u}, 1024u * 1024u, 0u, 65536u, fixture.m_fence.completed()));
            fixture.submit();
            const auto actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(not actual and actual.error() == std::errc::result_out_of_range);
        };

        // All inputs are normal FP32. The e^-88 transmission is subnormal,
        // yet the bright opaque source makes its discarded radiance order one.
        lighting::OpticalCell absorber{};
        absorber.m_kind = lighting::OpticalKind::absorbing;
        absorber.m_extinction[0] = 88.0;
        lighting::OpticalCell bright{};
        bright.m_kind = lighting::OpticalKind::opaque;
        bright.m_surface_emission[0] = std::ldexp(1.0, 127);
        const double transmitted_bright = std::exp(-88.0) * bright.m_surface_emission[0];
        PPR_TEST_ASSERT(transmitted_bright > 1.0 and transmitted_bright < 2.0);

        const std::array<lighting::OpticalCell, 2> single_absorber{absorber, bright};
        const lighting::OpticalGridView single_grid{
            single_absorber, {}, 1.0, 2u, 1u, lighting::OpticalBoundary::vacuum};
        reject_arithmetic(single_grid, {{0.0, 0.5}, {1.0, 0.0}, 0.0, 2.0}, {{transmitted_bright, 0.0, 0.0}, {}});

        // Each e^-44 segment is normal; only their cumulative product underflows.
        absorber.m_extinction[0] = 44.0;
        const std::array<lighting::OpticalCell, 3> two_absorbers{absorber, absorber, bright};
        const lighting::OpticalGridView cumulative_grid{
            two_absorbers, {}, 1.0, 3u, 1u, lighting::OpticalBoundary::vacuum};
        reject_arithmetic(cumulative_grid, {{0.0, 0.5}, {1.0, 0.0}, 0.0, 3.0}, {{transmitted_bright, 0.0, 0.0}, {}});

        // Opaque emission still occurs before termination; its positive T*S
        // product cannot silently flush merely because the endpoint then has T=0.
        auto weak = bright;
        weak.m_surface_emission[0] = std::ldexp(1.0, -100);
        const std::array<lighting::OpticalCell, 2> weak_surface{absorber, weak};
        const lighting::OpticalGridView weak_grid{weak_surface, {}, 1.0, 2u, 1u, lighting::OpticalBoundary::vacuum};
        reject_arithmetic(
            weak_grid, {{0.0, 0.5}, {1.0, 0.0}, 0.0, 2.0},
            {{std::exp(-44.0) * weak.m_surface_emission[0], 0.0, 0.0}, {}});

        // The accepted order is (T*j)*integral. Its first product underflows,
        // although the final analytical radiance is a normal FP32 value.
        const double large_metric = std::ldexp(1.0, 100);
        absorber.m_extinction[0] = std::ldexp(1.0, -94);
        lighting::OpticalCell volume{};
        volume.m_kind = lighting::OpticalKind::absorbing;
        volume.m_volume_emission[0] = std::ldexp(1.0, -68);
        const std::array<lighting::OpticalCell, 2> amplified{absorber, volume};
        const lighting::OpticalGridView amplified_grid{
            amplified, {}, large_metric, 2u, 1u, lighting::OpticalBoundary::vacuum};
        const double volume_light = std::exp(-64.0) * std::ldexp(1.0, 32);
        PPR_TEST_ASSERT(volume_light > double((std::numeric_limits<float>::min)()));
        reject_arithmetic(
            amplified_grid, {{0.0, large_metric * 0.5}, {1.0, 0.0}, 0.0, large_metric * 2.0},
            {{volume_light, 0.0, 0.0}, {std::exp(-64.0), 1.0, 1.0}});

        // A normal metric can still produce a subnormal positive in-world
        // distance. A large normal sigma makes that short span consequential.
        const double small_metric = std::ldexp(1.0, -126);
        absorber.m_extinction[0] = std::ldexp(1.0, 126);
        const std::array<lighting::OpticalCell, 1> short_cells{absorber};
        const lighting::OpticalGridView short_grid{
            short_cells, {}, small_metric, 1u, 1u, lighting::OpticalBoundary::vacuum};
        reject_arithmetic(
            short_grid, {{small_metric * 0.5, small_metric * 0.5}, {1.0, 0.0}, 0.0, small_metric},
            {{}, {std::exp(-0.5), 1.0, 1.0}});

        // A needed positive source integral can be subnormal even when j times
        // that integral has a normal result. The integral is not silently zero.
        absorber.m_volume_emission[0] = std::ldexp(1.0, 126);
        const std::array<lighting::OpticalCell, 1> integral_cells{absorber};
        const lighting::OpticalGridView integral_grid{
            integral_cells, {}, small_metric, 1u, 1u, lighting::OpticalBoundary::vacuum};
        reject_arithmetic(
            integral_grid, {{0.0, small_metric * 0.5}, {1.0, 0.0}, 0.0, small_metric},
            {{1.0 - std::exp(-1.0), 0.0, 0.0}, {std::exp(-1.0), 1.0, 1.0}});

        // A complete weak opaque interval is valid before angular weighting,
        // but its positive one-half merge term leaves the normal FP32 domain.
        weak.m_surface_emission[0] = small_metric;
        const std::array<lighting::OpticalCell, 1> weighted_cells{weak};
        const lighting::OpticalGridView weighted_grid{
            weighted_cells, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareCascades(weighted_grid, cascadeRequest_(1u, 1u), fixture.m_fence.completed()));
        fixture.submit(true);
        const auto weighted = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not weighted and weighted.error() == std::errc::result_out_of_range);
    };

    // ------------------------------------------------------------------
    // authored material palette ABI qualification
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (material_optics_palette_uses_actual96_byte_shader_rows) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        auto &device = fixture.m_rhi->getDevice();

        GridPass grid{};
        PPR_DEFER {
            if (make_error_code(fixture.m_queue->waitOnHost())) {
                hal::fatalExit("material-optics-grid-initialization-completion-unknown");
            }

            PPR_TEST_ASSERT(not grid.shutdown());
        };
        // Exercise the real draw and particle program construction/reflection;
        // the isolated compute probe below does not enter a game frame.
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));

        struct ProbeRow {
            u32 m_model{};
            float m_sigma[3]{};
            float m_source[3]{};
            u32 m_status{};
            float m_volume[3]{};
            u32 m_reserved{};
        };
        static_assert(sizeof(ProbeRow) == 48u and alignof(ProbeRow) == 4u);
        static_assert(PPR_OFFSETOF(ProbeRow, m_source) == 16u);
        static_assert(PPR_OFFSETOF(ProbeRow, m_status) == 28u);
        static_assert(PPR_OFFSETOF(ProbeRow, m_volume) == 32u);

        std::array<GridTerrainVisual, 5> palette{};
        palette[1].m_transport_model = kGridTransportVacuumV1;
        palette[2].m_transport_model = kGridTransportOpaqueV1;
        palette[2].m_surface_flags = kGridSurfaceOpticalBlend;
        palette[2].m_opacity = 0.25f;
        palette[2].m_emissive_rgb[0] = 0.5f;
        palette[2].m_emissive_rgb[1] = 0.25f;
        palette[2].m_emissive_rgb[2] = 0.125f;
        palette[2].m_emissive_strength = 4.0f;
        palette[3] = palette[2];
        palette[3].m_transport_model = kGridTransportAbsorbingV1;
        palette[3].m_sigma_per_m[0] = 0.5f;
        palette[3].m_sigma_per_m[1] = 2.0f;
        palette[4].m_transport_model = 0x201u;
        for (u32 index = 0u; index < palette.size(); ++index) {
            palette[index].m_material_id = index;
        }

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "grid_tiles.slang", "grid_tiles", module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        slang::TypeLayoutReflection *visual{};
        slang::TypeLayoutReflection *probe{};
        for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
            auto *const parameter = layout->getParameterByIndex(index);
            const std::string_view name = parameter->getName();
            if (name == "g_terrain_visuals") {
                visual = parameter->getTypeLayout()->getElementTypeLayout();
            } else if (name == "g_material_optics_probe") {
                probe = parameter->getTypeLayout()->getElementTypeLayout();
            }
        }

        PPR_TEST_ASSERT(
            visual != nullptr and visual->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GridTerrainVisual));
        PPR_TEST_ASSERT(probe != nullptr and probe->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ProbeRow));
        for (const auto &[name, offset]: std::array<std::pair<const char *, u32>, 4>{
                 std::pair{"m_transport_model", 80u}, {"m_sigma_r_per_m", 84u}, {"m_sigma_g_per_m", 88u},
                 {"m_sigma_b_per_m", 92u}}) {
            bool found = false;
            for (unsigned index = 0u; index < visual->getFieldCount(); ++index) {
                auto *const field = visual->getFieldByIndex(index);
                if (std::string_view{field->getName()} == name) {
                    found = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                    break;
                }
            }

            PPR_TEST_ASSERT(found);
        }

        rhi::ComPtr<rhi::IBuffer> input, output, readback;
        rhi::ComPtr<rhi::IShaderProgram> program{};
        rhi::ComPtr<rhi::IComputePipeline> pipeline{};
        rhi::ComPtr<rhi::IShaderObject> root{};
        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        rhi::ComPtr<rhi::ICommandBuffer> commands{};
        PPR_DEFER {
            // The queue proof runs before any local command/buffer owner dies,
            // including assertion/partial-encoding exits and ambiguous submit.
            if (make_error_code(fixture.m_queue->waitOnHost())) {
                hal::fatalExit("material-optics-qualification-completion-unknown");
            }
        };

        rhi::BufferDesc description{};
        description.size = sizeof(palette);
        description.elementSize = sizeof(GridTerrainVisual);
        description.memoryType = rhi::MemoryType::Upload;
        description.usage = rhi::BufferUsage::ShaderResource;
        description.defaultState = rhi::ResourceState::ShaderResource;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, palette.data(), input.writeRef())));
        PPR_TEST_ASSERT(input->getDesc().elementSize == 96u and input->getDesc().size == sizeof(palette));

        std::array<ProbeRow, 5> rows{};
        description.size = sizeof(rows);
        description.elementSize = sizeof(ProbeRow);
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, output.writeRef())));

        description.elementSize = 0u;
        description.memoryType = rhi::MemoryType::ReadBack;
        description.usage = rhi::BufferUsage::CopyDestination;
        description.defaultState = rhi::ResourceState::CopyDestination;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, readback.writeRef())));

        shader::ComPtr<slang::IEntryPoint> entry{};
        PPR_TEST_ASSERT(
            not shader::make_error_code(module->findEntryPointByName("qualifyMaterialOptics", entry.writeRef())));
        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc program_description{};
        program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_description.slangGlobalScope = module.get();
        program_description.slangEntryPoints = entries;
        program_description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics{};
        PPR_TEST_ASSERT(not make_error_code(
            device.createShaderProgram(program_description, program.writeRef(), diagnostics.writeRef())));
        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor constants{};
        const std::array<u32, 4> parameters{static_cast<u32>(palette.size()), 0u, 0u, 0u};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_material_optics_probe_params"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(parameters.data(), sizeof(parameters))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_terrain_visuals"].setBinding(
                rhi::Binding(input, {.offset = 0u, .size = input->getDesc().size}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_material_optics_probe"].setBinding(
                rhi::Binding(output, {.offset = 0u, .size = output->getDesc().size}))));

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(input, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(readback, rhi::ResourceState::CopyDestination);
        auto *const compute = encoder->beginComputePass();
        PPR_TEST_ASSERT(compute != nullptr);
        compute->bindPipeline(pipeline, root);
        compute->dispatchCompute(1u, 1u, 1u);
        compute->end();

        encoder->globalBarrier();
        encoder->setBufferState(output, rhi::ResourceState::CopySource);
        encoder->copyBuffer(readback, 0u, output, 0u, sizeof(rows));
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));

        rhi::ICommandBuffer *command_pointer = commands.get();
        rhi::SubmitDesc submission{};
        submission.commandBuffers = &command_pointer;
        submission.commandBufferCount = 1u;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submission)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

        void *mapped{};
        PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
        std::memcpy(rows.data(), mapped, sizeof(rows));
        PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

        for (std::size_t index = 0u; index < rows.size(); ++index) {
            PPR_TEST_ASSERT(rows[index].m_model == palette[index].m_transport_model);
            PPR_TEST_ASSERT((rows[index].m_status == 0u) == (index >= 1u and index <= 3u));
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                const float source = palette[index].m_emissive_rgb[channel] * palette[index].m_emissive_strength;
                PPR_TEST_ASSERT(rows[index].m_sigma[channel] == palette[index].m_sigma_per_m[channel]);
                PPR_TEST_ASSERT(rows[index].m_source[channel] == source);
                PPR_TEST_ASSERT(rows[index].m_volume[channel] == palette[index].m_sigma_per_m[channel] * source);
            }
        }
    };

    // ------------------------------------------------------------------
    // actual production precision admission and canonical source observation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_precision_selection_rejects_unqualified_sessions_before_initialization) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        auto &device = fixture.m_rhi->getDevice();
        const auto missing_root = std::filesystem::current_path() / "precision-admission-must-not-load";
        PPR_TEST_ASSERT(not std::filesystem::exists(missing_root));

        lighting::TerrainPrecisionSelection invalid{};
        invalid.m_precision = static_cast<lighting::TerrainPrecision>(255u);
        invalid.m_rte64_qualified_for_device_session = true;
        PPR_TEST_ASSERT(
            fixture.m_pass.initialize(device, *fixture.m_shaders, missing_root, invalid) ==
            std::errc::invalid_argument);
        const auto after_invalid = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
        PPR_TEST_ASSERT(not after_invalid and after_invalid.error() == std::errc::not_connected);

        lighting::TerrainPrecisionSelection unqualified{};
        unqualified.m_precision = lighting::TerrainPrecision::qualified_fp64_refinement_experimental;
        PPR_TEST_ASSERT(
            fixture.m_pass.initialize(device, *fixture.m_shaders, missing_root, unqualified) ==
            std::errc::not_supported);
        const auto after_unqualified = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
        PPR_TEST_ASSERT(not after_unqualified and after_unqualified.error() == std::errc::not_connected);

        if (device.getInfo().deviceType != rhi::DeviceType::Vulkan or not device.hasFeature(rhi::Feature::Double)) {
            unqualified.m_rte64_qualified_for_device_session = true;
            PPR_TEST_ASSERT(
                fixture.m_pass.initialize(device, *fixture.m_shaders, missing_root, unqualified) ==
                std::errc::not_supported);
            const auto unsupported = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
            PPR_TEST_ASSERT(not unsupported and unsupported.error() == std::errc::not_connected);
        } else {
            std::println(
                "PRODUCTION_PRECISION_ADMISSION: non-Vulkan/missing-Double branches unavailable on actual device");
        }

        // A real missing content root distinguishes admission rejection from
        // shader loading. A subsequent default initialization proves no State
        // remained acquired; no allocation/capability mock is introduced.
        PPR_TEST_ASSERT(not fixture.m_pass.initialize(device, *fixture.m_shaders, std::filesystem::current_path()));
        const auto strict = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
        PPR_TEST_ASSERT(strict.has_value() and not *strict);
        PPR_TEST_ASSERT(fixture.m_pass.terrainPipelineReadinessForQualification() == (std::array<bool, 3>{}));

        PPR_TEST_ASSERT(not fixture.m_pass.shutdown(kFenceValueNone));
        const auto after_shutdown = fixture.m_pass.readTerrainPrecisionStatistics(kFenceValueNone);
        PPR_TEST_ASSERT(not after_shutdown and after_shutdown.error() == std::errc::not_connected);
    };

    struct PrecisionProbeInput {
        u32 m_kind{}, m_flags{};
        std::array<u32, 8u> m_coordinates{};
        u32 m_first_status{}, m_initial_status{}, m_refined_status{}, m_roundness{};
        u32 m_reserved[2]{};
    };

    static_assert(sizeof(PrecisionProbeInput) == 64u and alignof(PrecisionProbeInput) == 4u);
    static_assert(std::is_standard_layout_v<PrecisionProbeInput> and std::is_trivially_copyable_v<PrecisionProbeInput>);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_kind) == 0u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_flags) == 4u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_coordinates) == 8u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_first_status) == 40u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_initial_status) == 44u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_refined_status) == 48u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_roundness) == 52u);
    static_assert(PPR_OFFSETOF(PrecisionProbeInput, m_reserved) == 56u);

    PPR_UNIT_TEST (terrain_precision_source_bridge_preserves_fp32_ownership_and_counter_settlement) {
        const auto qualification = config::getenvLookup("PPR_TEST_TERRAIN_PRODUCTION_FP64");
        PPR_TEST_ASSERT(not qualification or *qualification == "0" or *qualification == "1");
        if (not qualification or *qualification != "1") {
            std::println(
                "PRODUCTION_PRECISION_SOURCE_UNQUALIFIED_NOT_EXECUTED: operator must supply PPR_TEST_TERRAIN_PRODUCTION_FP64=1 after checking this device/session RTE64 evidence; no GPU qualification");
            return;
        }

        // The existing selector is the operator's explicit checked-session
        // assertion, not capability discovery. Vulkan+Double alone is insufficient.
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        auto &device = fixture.m_rhi->getDevice();
        if (device.getInfo().deviceType != rhi::DeviceType::Vulkan or not device.hasFeature(rhi::Feature::Double)) {
            std::println(
                "PRODUCTION_PRECISION_SOURCE_UNSUPPORTED: actual Vulkan+Double unavailable; no source qualification");
            return;
        }

        using ProbeOutput = std::array<u32, 16u>;
        static_assert(sizeof(ProbeOutput) == 64u and alignof(ProbeOutput) == 4u);
        std::vector<PrecisionProbeInput> inputs{};
        std::vector<ProbeOutput> expected{};
        const auto append = [&](const u32 kind, const u32 flags, const std::array<u64, 4u> coordinates,
                                const ProbeOutput output, const u32 first = 0u, const u32 initial = 0u,
                                const u32 refined = 0u, const u32 roundness = 0u) {
            PrecisionProbeInput input{};
            input.m_kind = kind;
            input.m_flags = flags;
            for (std::size_t index = 0u; index < coordinates.size(); ++index) {
                input.m_coordinates[2u * index] = static_cast<u32>(coordinates[index]);
                input.m_coordinates[2u * index + 1u] = static_cast<u32>(coordinates[index] >> 32u);
            }

            input.m_first_status = first;
            input.m_initial_status = initial;
            input.m_refined_status = refined;
            input.m_roundness = roundness;
            inputs.push_back(input);
            expected.push_back(output);
        };

        struct SourceCase {
            std::array<u64, 4u> m_coordinates{};
            std::array<u32, 4u> m_canonical{};
            u32 m_flags{}, m_conversion{}, m_accepted{}, m_red{}, m_blue{};
        };

        // Literal binary64 inputs and independent binary32 endpoint/winner bits.
        // Flags1 selects horizontal,2 reverses first-label order. No expected
        // result calls a production policy or recomputes its predicate.
        const std::array source_cases{
            SourceCase{
                {0x3f90000000000000u, 0x3f90000400000000u, 0x3fd0000000000000u, 0x3fd0000400000000u},
                {0x3c800000u, 0x3c800020u, 0x3e800000u, 0x3e800020u}, 0u, 3u, 1u, 0x3f800000u, 0u},
            SourceCase{
                {0x3fd0000000000000u, 0x3fd0000400000000u, 0x3f90000000000000u, 0x3f90000400000000u},
                {0x3e800000u, 0x3e800020u, 0x3c800000u, 0x3c800020u}, 1u, 3u, 1u, 0x3f800000u, 0u},
            SourceCase{
                {0x3fef7fffe0000000u, 0x3fef800000000000u, 0x3fd0000000000000u, 0x3fd0000400000000u},
                {0x3f7bffffu, 0x3f7c0000u, 0x3e800000u, 0x3e800020u}, 0u, 3u, 1u, 0u, 0x3f800000u},
            SourceCase{
                {0x3fd0000000000000u, 0x3fd0000400000000u, 0x3fef7fffe0000000u, 0x3fef800000000000u},
                {0x3e800000u, 0x3e800020u, 0x3f7bffffu, 0x3f7c0000u}, 1u, 3u, 1u, 0u, 0x3f800000u},
            SourceCase{
                {0x3f90000000000000u, 0x3f90000400000000u, 0x3fd0000000000000u, 0x3fd0000400000000u},
                {0x3c800000u, 0x3c800020u, 0x3e800000u, 0x3e800020u}, 2u, 3u, 1u, 0u, 0x3f800000u},
            SourceCase{
                {0x3fe0000001000000u, 0x3fe0000002000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0x3f000000u, 0x3f000001u, 0x3e800000u, 0x3e800000u}, 0u, 3u, 0u, 0u, 0u},
            SourceCase{
                {0x3fdffffffc000000u, 0x3fdffffffe000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0x3effffffu, 0x3f000000u, 0x3e800000u, 0x3e800000u}, 0u, 3u, 0u, 0u, 0u},
            SourceCase{
                {0x3fd0000000000000u, 0x3fd0000000000000u, 0x3fe0000001000000u, 0x3fe0000002000000u},
                {0x3e800000u, 0x3e800000u, 0x3f000000u, 0x3f000001u}, 1u, 3u, 0u, 0u, 0u},
            SourceCase{
                {0x3fe8000000000000u, 0x3fd0000000000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u},
            SourceCase{
                {0x0000000000000001u, 0x0000000000000001u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u},
            SourceCase{
                {0x0010000000000000u, 0x0010000000000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u},
            SourceCase{
                {0x36a0000000000000u, 0x36a0000000000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u},
            SourceCase{
                {0x47f0000000000000u, 0x47f0000000000000u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u},
            SourceCase{
                {0x7ff8000000000001u, 0x7ff8000000000001u, 0x3fd0000000000000u, 0x3fd0000000000000u},
                {0u, 0u, 0x3e800000u, 0x3e800000u}, 0u, 2u, 0u, 0u, 0u}};

        for (const auto &row: source_cases) {
            ProbeOutput output{
                row.m_conversion, row.m_canonical[0], row.m_canonical[1], row.m_canonical[2], row.m_canonical[3],
                row.m_accepted, row.m_red, 0u, row.m_blue};
            append(0u, row.m_flags, row.m_coordinates, output);
        }

        // Singleton point admission is the actual policy helper; exact native
        // tie points additionally call the unchanged production score/tie rule.
        append(1u, 8u, {0x3fe0000000000000u, 0u, 0x3fd0000000000000u, 0u}, {1u, 1u});
        append(1u, 10u, {0x3fe0000000000000u, 0u, 0x3fd0000000000000u, 0u}, {1u, 2u});
        append(1u, 9u, {0x3fd0000000000000u, 0u, 0x3fe0000000000000u, 0u}, {1u, 1u});
        append(1u, 0u, {0x3fe0000001000000u, 0u, 0x3fd0000000000000u, 0u}, {});
        append(1u, 0u, {0x36a0000000000000u, 0u, 0x3fd0000000000000u, 0u}, {});
        append(1u, 0u, {0x0010000000000000u, 0u, 0x3fd0000000000000u, 0u}, {});
        append(1u, 0u, {0x8000000000000000u, 0u, 0x3fd0000000000000u, 0u}, {1u});

        // The real common-source opaque path does not need a unique label.
        append(3u, 4u, {}, {1u, 0u, 0u, 0x3f800000u});

        // Discriminating production regression: roundness 1 and non-singleton
        // localX=.5+[2^-29,2^-28],Y=.875. The old FP64-only source certificate
        // selected blue; the canonical FP32 tie enclosure must stay unresolved.
        append(
            4u, 0u, {0x3e20000000000000u, 0x3e30000000000000u, 0x3fec000000000000u, 0x3fec000000000000u},
            {0u, 0x2020u, 0u, 0u, 0u, 0x3f800000u}, 0u, 0u, 0u, 0x3f800000u);

        // These rows observe the actual eligibility/settlement functions and
        // GPU atomics only; they do not claim complete-query generation admission.
        append(2u, 0u, {}, {1u, 0u, 0x40e00000u}, 0x120u, 0u, 0u);
        append(2u, 0u, {}, {1u, 0x2220u, 0x40400000u}, 0x220u, 0u, 0x2020u);
        append(2u, 0u, {}, {1u, 0x4420u, 0x40400000u}, 0x420u, 0u, 0x4020u);
        append(2u, 0u, {}, {1u, 0x820u, 0x40400000u}, 0x820u, 0u, 0x820u);
        append(2u, 0u, {}, {1u, 0u, 0x40e00000u}, 0x08001020u, 0u, 0u);
        append(2u, 0u, {}, {0u, 0x120u, 0x40400000u}, 0x120u, 1u, 0u);
        append(2u, 0u, {}, {0u, 0x2020u, 0x40400000u}, 0x2020u);
        append(2u, 0u, {}, {0u, 0x4020u, 0x40400000u}, 0x4020u);
        append(2u, 0u, {}, {0u, 0x101u, 0x40400000u}, 0x101u);
        append(2u, 0u, {}, {0u, 0x00100120u, 0x40400000u}, 0x00100120u);
        append(2u, 0u, {}, {0u, 0x20u, 0x40400000u}, 0x20u);
        append(2u, 0u, {}, {0u, 0x10000120u, 0x40400000u}, 0x10000120u);
        append(2u, 0u, {}, {0u, 0x08000020u, 0x40400000u}, 0x08000020u);

        PPR_TEST_ASSERT(inputs.size() == expected.size() and inputs.size() <= 64u);

        constexpr string_literal source = R"(
#include "radiance_cascades_refined.slang"

struct PrecisionProbeInput {
    uint kind, flags;
    uint coordinates[8];
    uint firstStatus, initialStatus, refinedStatus, roundness;
    uint reserved[2];
};
struct PrecisionProbeOutput { uint words[16]; };
struct PrecisionProbeParameters { uint count, reserved0, reserved1, reserved2; };
ConstantBuffer<PrecisionProbeParameters> g_precision_probe_params;
StructuredBuffer<PrecisionProbeInput> g_precision_probe_inputs;
RWStructuredBuffer<PrecisionProbeOutput> g_precision_probe_outputs;

[shader("compute")]
[numthreads(64, 1, 1)]
void observeProductionPrecision(uint3 id : SV_DispatchThreadID) {
    terrainRequireRte64();
    if (id.x >= g_precision_probe_params.count) return;

    PrecisionProbeInput input = g_precision_probe_inputs[id.x];
    PrecisionProbeOutput output;
    for (uint i = 0; i < 16; ++i) output.words[i] = 0;

    double2 x = double2(asdouble(input.coordinates[0], input.coordinates[1]),
        asdouble(input.coordinates[2], input.coordinates[3]));
    double2 y = double2(asdouble(input.coordinates[4], input.coordinates[5]),
        asdouble(input.coordinates[6], input.coordinates[7]));
    bool horizontal = (input.flags & 1u) != 0;
    uint labels[4] = {1u, horizontal ? 1u : 2u, 2u, horizontal ? 2u : 1u};
    if ((input.flags & 2u) != 0) for (uint i = 0; i < 4; ++i) labels[i] = 3u - labels[i];
    float roundness[4]; float3 sources[4];
    for (uint i = 0; i < 4; ++i) {
        roundness[i] = asfloat(input.roundness);
        sources[i] = (input.flags & 4u) != 0 ? float3(0, 1, 0) :
            labels[i] == 1 ? float3(1, 0, 0) : float3(0, 0, 1);
    }

    if (input.kind == 0) {
        float2 canonicalX, canonicalY;
        bool validX = Terrain64::terrainSourceFp32Interval(x, canonicalX);
        bool validY = Terrain64::terrainSourceFp32Interval(y, canonicalY);
        float3 selected;
        bool accepted = Terrain64::terrainSourceCertificate(labels, roundness, sources, x, y, selected);
        output.words[0] = (validX ? 1u : 0u) | (validY ? 2u : 0u);
        output.words[1] = asuint(canonicalX.x); output.words[2] = asuint(canonicalX.y);
        output.words[3] = asuint(canonicalY.x); output.words[4] = asuint(canonicalY.y);
        output.words[5] = accepted ? 1u : 0u;
        output.words[6] = asuint(selected.x); output.words[7] = asuint(selected.y); output.words[8] = asuint(selected.z);
    } else if (input.kind == 1) {
        float2 nativePoint;
        bool admitted = Terrain64::terrainNativePoint(double2(x.x, y.x), nativePoint);
        output.words[0] = admitted ? 1u : 0u;
        if (admitted && (input.flags & 8u) != 0) {
            uint status = 0;
            output.words[1] = terrainHitLabel(labels, roundness, nativePoint, status);
            output.words[2] = status;
        }
    } else if (input.kind == 2) {
        OpticalResult first = {3, 0, 0, 1, 1, 1, input.firstStatus, 0};
        OpticalResult refined = {7, 0, 0, 1, 1, 1, input.refinedStatus, 0};
        bool eligible = terrainRefinementEligible(first.status, input.initialStatus);
        if (eligible) first = terrainSettleRefinement(first, refined);
        output.words[0] = eligible ? 1u : 0u;
        output.words[1] = first.status; output.words[2] = asuint(first.radianceR);
    } else if (input.kind == 3) {
        TerrainPoint origin = {float2(0.5), 0.0, 0.0};
        TerrainLine line = {origin, origin, float2(1, 0), 0};
        OpticalResult result = {0, 0, 0, 1, 1, 1, 0, 0};
        bool stopped = Terrain64::terrainOpaque(labels, roundness, sources, int2(0),
            line, double2(1), double2(0), double2(0, 1), result);
        output.words[0] = stopped ? 1u : 0u; output.words[1] = result.status;
        output.words[2] = asuint(result.radianceR); output.words[3] = asuint(result.radianceG);
        output.words[4] = asuint(result.radianceB); output.words[5] = asuint(result.transmissionR);
    } else if (input.kind == 4) {
        TerrainPoint origin = {float2(1.0, 1.375), 0.0, 0.0};
        TerrainLine line = {origin, origin, float2(1, 0), 0};
        OpticalResult result = {0, 0, 0, 1, 1, 1, 0, 0};

        bool stopped = Terrain64::terrainOpaque(labels, roundness, sources, int2(0),
            line, double2(1), double2(0), x, result);

        output.words[0] = stopped ? 1u : 0u; output.words[1] = result.status;
        output.words[2] = asuint(result.radianceR); output.words[3] = asuint(result.radianceG);
        output.words[4] = asuint(result.radianceB); output.words[5] = asuint(result.transmissionR);
    }

    g_precision_probe_outputs[id.x] = output;
}
)";
        shader::SharedModule module{};
        shader::ComPtr<slang::IEntryPoint> entry{};
        shader::ComPtr<slang::IComponentType> composite{}, linked{};
        shader::Diagnose diagnostics{};
        rhi::ComPtr<rhi::IShaderProgram> program{};
        rhi::ComPtr<rhi::IComputePipeline> pipeline{};
        rhi::ComPtr<rhi::IShaderObject> root{};
        rhi::ComPtr<rhi::IBuffer> gpu_inputs{}, gpu_outputs{}, gpu_counts{}, readback{}, points{}, cases{};
        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        rhi::ComPtr<rhi::ICommandBuffer> commands{};
        PPR_DEFER {
            if (make_error_code(fixture.m_queue->waitOnHost())) {
                hal::fatalExit("production-precision-observer-completion-unknown");
            }
        };

        // The virtual source path anchors includes to the actually staged tree.
        // No prototype directory, source overlay or copied numerical helper exists.
        const auto source_path =
            (std::filesystem::current_path() / "shaders/production_precision_observer.slang").generic_string();
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromSource(
            "production_precision_observer", source_path.c_str(), source, module.writeRef()));
        PPR_TEST_ASSERT(
            not make_error_code(module->findEntryPointByName("observeProductionPrecision", entry.writeRef())));
        slang::IComponentType *parts[]{module.get(), entry.get()};
        PPR_TEST_ASSERT(not make_error_code(module->getSession()->createCompositeComponentType(
            parts, 2, composite.writeRef(), diagnostics.writeRef())));

        slang::CompilerOptionEntry option{};
        option.name = slang::CompilerOptionName::FloatingPointMode;
        option.value.kind = slang::CompilerOptionValueKind::Int;
        option.value.intValue0 = SLANG_FLOATING_POINT_MODE_PRECISE;
        PPR_TEST_ASSERT(
            not make_error_code(composite->linkWithOptions(linked.writeRef(), 1u, &option, diagnostics.writeRef())));

        auto *const layout = linked->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        for (const auto &[name, bytes]: std::array<std::pair<const char *, std::size_t>, 6u>{
                 std::pair{"g_precision_probe_inputs", sizeof(PrecisionProbeInput)},
                 {"g_precision_probe_outputs", sizeof(ProbeOutput)}, {"g_precision_probe_params", 16u},
                 {"g_terrain_precision_counts", sizeof(u32)}, {"g_contour_points", sizeof(math::ContourPoint)},
                 {"g_contour_cases", sizeof(math::ContourCase)}}) {
            slang::TypeLayoutReflection *element{};
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == name) {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }

            PPR_TEST_ASSERT(element != nullptr and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes);
            if (std::string_view{name} != "g_precision_probe_params") {
                PPR_TEST_ASSERT(element->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes);
                const auto alignment = element->getAlignment(SLANG_PARAMETER_CATEGORY_UNIFORM);
                PPR_TEST_ASSERT(alignment > 0);
                PPR_TEST_ASSERT(bytes % static_cast<std::size_t>(alignment) == 0u);
                if (std::string_view{name} != "g_contour_points" and std::string_view{name} != "g_contour_cases") {
                    PPR_TEST_ASSERT(alignment == 4u);
                }
            }

            if (std::string_view{name} == "g_precision_probe_inputs") {
                constexpr std::array<std::size_t, 7u> offsets{0u, 4u, 8u, 40u, 44u, 48u, 52u};
                PPR_TEST_ASSERT(element->getFieldCount() == 8u);
                for (unsigned index = 0u; index < offsets.size(); ++index) {
                    PPR_TEST_ASSERT(
                        element->getFieldByIndex(index)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offsets[index]);
                }

                PPR_TEST_ASSERT(element->getFieldByIndex(7u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 56u);
            }

            if (std::string_view{name} == "g_precision_probe_outputs") {
                PPR_TEST_ASSERT(element->getFieldCount() == 1u);
                PPR_TEST_ASSERT(element->getFieldByIndex(0u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 0u);
            }

            if (std::string_view{name} == "g_precision_probe_params") {
                PPR_TEST_ASSERT(element->getFieldCount() == 4u);
                for (unsigned index = 0u; index < 4u; ++index) {
                    PPR_TEST_ASSERT(
                        element->getFieldByIndex(index)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) ==
                        index * sizeof(u32));
                }
            }
        }

        const auto dump = config::getenvLookup("PPR_TEST_TERRAIN_SPIRV_DIRECTORY");
        if (dump) {
            PPR_TEST_ASSERT(not dump->empty());
            shader::ComPtr<slang::IBlob> code{};
            PPR_TEST_ASSERT(
                not make_error_code(linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())));
            PPR_TEST_ASSERT(code and code->getBufferSize() >= sizeof(u32));
            PPR_TEST_ASSERT(code->getBufferSize() <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));

            std::error_code error{};
            std::filesystem::create_directories(std::filesystem::path{*dump}, error);
            PPR_TEST_ASSERT(not error);
            std::ofstream stream{
                std::filesystem::path{*dump} / "production-precision-source-bridge.spv",
                std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(stream.is_open());
            stream.write(
                static_cast<const char *>(code->getBufferPointer()),
                static_cast<std::streamsize>(code->getBufferSize()));
            stream.close();
            PPR_TEST_ASSERT(not stream.fail());
        }

        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = linked.get();
        PPR_TEST_ASSERT(
            not make_error_code(device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef())));
        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

        const auto buffer = [&](const u64 bytes, const u32 stride, const rhi::MemoryType memory,
                                const rhi::BufferUsage usage, const rhi::ResourceState state, const void *const data) {
            rhi::BufferDesc descriptor{};
            descriptor.size = bytes;
            descriptor.elementSize = stride;
            descriptor.memoryType = memory;
            descriptor.usage = usage;
            descriptor.defaultState = state;
            rhi::ComPtr<rhi::IBuffer> result{};
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(descriptor, data, result.writeRef())));
            PPR_TEST_ASSERT(result and result->getDesc().size == bytes and result->getDesc().elementSize == stride);
            return result;
        };

        const auto writable = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource) |
            static_cast<u32>(rhi::BufferUsage::CopyDestination));
        const u64 input_bytes = inputs.size() * sizeof(PrecisionProbeInput);
        const u64 output_bytes = inputs.size() * sizeof(ProbeOutput);
        constexpr std::array<u32, 4u> zero_counts{};
        gpu_inputs = buffer(
            input_bytes, sizeof(PrecisionProbeInput), rhi::MemoryType::Upload, rhi::BufferUsage::ShaderResource,
            rhi::ResourceState::ShaderResource, inputs.data());
        gpu_outputs = buffer(
            output_bytes, sizeof(ProbeOutput), rhi::MemoryType::DeviceLocal, writable,
            rhi::ResourceState::UnorderedAccess, nullptr);
        gpu_counts = buffer(
            sizeof(zero_counts), sizeof(u32), rhi::MemoryType::DeviceLocal, writable,
            rhi::ResourceState::UnorderedAccess, zero_counts.data());
        readback = buffer(
            output_bytes + sizeof(zero_counts), 0u, rhi::MemoryType::ReadBack, rhi::BufferUsage::CopyDestination,
            rhi::ResourceState::CopyDestination, nullptr);
        points = buffer(
            math::contourPoints().size_bytes(), sizeof(math::ContourPoint), rhi::MemoryType::Upload,
            rhi::BufferUsage::ShaderResource, rhi::ResourceState::ShaderResource, math::contourPoints().data());
        cases = buffer(
            math::contourCases().size_bytes(), sizeof(math::ContourCase), rhi::MemoryType::Upload,
            rhi::BufferUsage::ShaderResource, rhi::ResourceState::ShaderResource, math::contourCases().data());

        rhi::ShaderCursor cursor{root};
        for (const auto &[name, resource]: std::array<std::pair<const char *, rhi::IBuffer *>, 5u>{
                 std::pair{"g_precision_probe_inputs", gpu_inputs.get()},
                 {"g_precision_probe_outputs", gpu_outputs.get()}, {"g_terrain_precision_counts", gpu_counts.get()},
                 {"g_contour_points", points.get()}, {"g_contour_cases", cases.get()}}) {
            PPR_TEST_ASSERT(not make_error_code(
                cursor[name].setBinding(rhi::Binding(resource, {.offset = 0u, .size = resource->getDesc().size}))));
        }

        const std::array<u32, 4u> parameters{static_cast<u32>(inputs.size()), 0u, 0u, 0u};
        rhi::ShaderCursor constants{};
        PPR_TEST_ASSERT(not make_error_code(cursor["g_precision_probe_params"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(parameters.data(), sizeof(parameters))));

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        for (auto *const resource: {gpu_inputs.get(), points.get(), cases.get()}) {
            encoder->setBufferState(resource, rhi::ResourceState::ShaderResource);
        }

        encoder->setBufferState(gpu_outputs, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(gpu_counts, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(readback, rhi::ResourceState::CopyDestination);
        auto *const compute = encoder->beginComputePass();
        PPR_TEST_ASSERT(compute != nullptr);
        compute->bindPipeline(pipeline, root);
        compute->dispatchCompute(1u, 1u, 1u);
        compute->end();

        encoder->globalBarrier();
        encoder->setBufferState(gpu_outputs, rhi::ResourceState::CopySource);
        encoder->setBufferState(gpu_counts, rhi::ResourceState::CopySource);
        encoder->copyBuffer(readback, 0u, gpu_outputs, 0u, output_bytes);
        encoder->copyBuffer(readback, output_bytes, gpu_counts, 0u, sizeof(zero_counts));
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));

        rhi::ICommandBuffer *command = commands.get();
        rhi::IFence *signal = fixture.m_fence.fence();
        const auto submitted = fixture.m_fence.nextValue();
        const u64 raw_submitted = submitted;
        rhi::SubmitDesc submission{};
        submission.commandBuffers = &command;
        submission.commandBufferCount = 1u;
        submission.signalFences = &signal;
        submission.signalFenceValues = &raw_submitted;
        submission.signalFenceCount = 1u;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submission)));
        fixture.m_fence.commitSubmitted(submitted);
        PPR_TEST_ASSERT(not fixture.m_fence.waitFor(submitted));

        std::vector<ProbeOutput> actual(inputs.size());
        std::array<u32, 4u> counts{};
        void *mapped{};
        PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
        std::memcpy(actual.data(), mapped, static_cast<std::size_t>(output_bytes));
        std::memcpy(counts.data(), static_cast<const std::byte *>(mapped) + output_bytes, sizeof(counts));
        PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

        for (std::size_t index = 0u; index < actual.size(); ++index) {
            for (std::size_t word = 0u; word < actual[index].size(); ++word) {
                if (actual[index][word] != expected[index][word]) {
                    std::println(
                        "PRODUCTION_PRECISION_SOURCE_MISMATCH row={} kind={} word={} actual={:08x} expected={:08x}",
                        index, inputs[index].m_kind, word, actual[index][word], expected[index][word]);
                }

                PPR_TEST_ASSERT(actual[index][word] == expected[index][word]);
            }
        }

        PPR_TEST_ASSERT(counts == (std::array<u32, 4u>{5u, 2u, 3u, 1u}));
        std::println(
            "PRODUCTION_PRECISION_SOURCE_PASS rows={} settlement_only_counts={},{},{},{}; no generation-overflow claim",
            actual.size(), counts[0], counts[1], counts[2], counts[3]);
    };

    // ------------------------------------------------------------------
    // runtime binary64 capability through the actual shader/RHI boundary
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (fp64_runtime_arithmetic_preserves_double_precision) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false);
        auto &device = fixture.m_rhi->getDevice();
        if (not device.hasFeature(rhi::Feature::Double)) {
            std::println("FP64_CAPABILITY_UNSUPPORTED: RHI Double feature absent; no runtime FP64 qualification");
            return;
        }

        const auto prototype_directory = config::getenvLookup("PPR_TEST_TERRAIN_FP64_PROTOTYPE_DIRECTORY");
        const auto corpus_directory = config::getenvLookup("PPR_TEST_FP64_CORPUS_DIRECTORY");
        PPR_TEST_ASSERT(not prototype_directory or not corpus_directory);

        if (corpus_directory) {
            PPR_TEST_ASSERT(not corpus_directory->empty());
            PPR_TEST_ASSERT(device.getInfo().deviceType == rhi::DeviceType::Vulkan);
            PPR_TEST_ASSERT(std::endian::native == std::endian::little);
            const auto export_directory = config::getenvLookup("PPR_TEST_FP64_SPIRV_DIRECTORY");
            PPR_TEST_ASSERT(export_directory and not export_directory->empty());

            struct CorpusInput {
                u32 m_op{};
                std::array<u32, 2> m_a{}, m_b{};
            };

            struct CorpusOutput {
                std::array<u32, 2> m_nearest{}, m_lower{}, m_upper{};
                u32 m_status{}, m_reserved{};
            };

            struct CorpusParameters {
                u32 m_count{}, m_reserved0{}, m_reserved1{}, m_reserved2{};
            };
            static_assert(sizeof(CorpusInput) == 20u and alignof(CorpusInput) == 4u);
            static_assert(sizeof(CorpusOutput) == 32u and alignof(CorpusOutput) == 4u);
            static_assert(sizeof(CorpusParameters) == 16u and alignof(CorpusParameters) == 4u);
            static_assert(std::is_standard_layout_v<CorpusInput> and std::is_trivially_copyable_v<CorpusInput>);
            static_assert(std::is_standard_layout_v<CorpusOutput> and std::is_trivially_copyable_v<CorpusOutput>);
            static_assert(
                std::is_standard_layout_v<CorpusParameters> and std::is_trivially_copyable_v<CorpusParameters>);
            static_assert(PPR_OFFSETOF(CorpusInput, m_op) == 0u);
            static_assert(PPR_OFFSETOF(CorpusInput, m_a) == 4u and PPR_OFFSETOF(CorpusInput, m_b) == 12u);
            static_assert(PPR_OFFSETOF(CorpusOutput, m_nearest) == 0u);
            static_assert(PPR_OFFSETOF(CorpusOutput, m_lower) == 8u and PPR_OFFSETOF(CorpusOutput, m_upper) == 16u);
            static_assert(
                PPR_OFFSETOF(CorpusOutput, m_status) == 24u and PPR_OFFSETOF(CorpusOutput, m_reserved) == 28u);
            static_assert(PPR_OFFSETOF(CorpusParameters, m_count) == 0u);
            static_assert(PPR_OFFSETOF(CorpusParameters, m_reserved0) == 4u);
            static_assert(PPR_OFFSETOF(CorpusParameters, m_reserved1) == 8u);
            static_assert(PPR_OFFSETOF(CorpusParameters, m_reserved2) == 12u);

            constexpr u32 count = 10240u;
            constexpr u64 input_bytes = u64{count} * sizeof(CorpusInput);
            constexpr u64 output_bytes = u64{count} * sizeof(CorpusOutput);
            static_assert(input_bytes <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));
            static_assert(output_bytes <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));
            const std::filesystem::path directory{*corpus_directory};
            const auto input_path = directory / "gpu-inputs-20B-le.bin";
            std::error_code filesystem_error{};
            const auto file_bytes = std::filesystem::file_size(input_path, filesystem_error);
            PPR_TEST_ASSERT(not filesystem_error);
            PPR_TEST_ASSERT(file_bytes % sizeof(CorpusInput) == 0u and file_bytes == input_bytes);

            std::vector<CorpusInput> values(count);
            std::ifstream input_stream{input_path, std::ios::binary};
            PPR_TEST_ASSERT(input_stream.is_open());
            input_stream.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(input_bytes));
            PPR_TEST_ASSERT(
                not input_stream.fail() and input_stream.gcount() == static_cast<std::streamsize>(input_bytes));
            input_stream.close();
            PPR_TEST_ASSERT(not input_stream.fail());

            shader::SharedModule corpus_module{};
            shader::ComPtr<slang::IEntryPoint> corpus_entry{};
            shader::ComPtr<slang::IComponentType> corpus_composite{}, corpus_linked{};
            shader::Diagnose corpus_diagnostics{};
            rhi::ComPtr<rhi::IShaderProgram> corpus_program{};
            rhi::ComPtr<rhi::IComputePipeline> corpus_pipeline{};
            rhi::ComPtr<rhi::IShaderObject> corpus_root{};
            rhi::ComPtr<rhi::IBuffer> corpus_inputs{}, corpus_outputs{}, corpus_readback{};
            rhi::ComPtr<rhi::ICommandEncoder> corpus_encoder{};
            rhi::ComPtr<rhi::ICommandBuffer> corpus_commands{};
            PPR_DEFER {
                // Also covers failed/ambiguous submit and setup assertions;
                // referenced resources are released only after proven idleness.
                if (make_error_code(fixture.m_queue->waitOnHost())) {
                    hal::fatalExit("fp64-corpus-completion-unknown");
                }
            };

            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                directory / "probe-source/arithmetic_corpus.slang", "terrain_fp64_arithmetic_corpus",
                corpus_module.writeRef()));
            PPR_TEST_ASSERT(not make_error_code(
                corpus_module->findEntryPointByName("qualifyFp64TerrainArithmetic", corpus_entry.writeRef())));

            slang::IComponentType *corpus_parts[]{corpus_module.get(), corpus_entry.get()};
            PPR_TEST_ASSERT(not make_error_code(corpus_module->getSession()->createCompositeComponentType(
                corpus_parts, 2, corpus_composite.writeRef(), corpus_diagnostics.writeRef())));

            slang::CompilerOptionEntry corpus_option{};
            corpus_option.name = slang::CompilerOptionName::FloatingPointMode;
            corpus_option.value.kind = slang::CompilerOptionValueKind::Int;
            corpus_option.value.intValue0 = SLANG_FLOATING_POINT_MODE_PRECISE;
            PPR_TEST_ASSERT(not make_error_code(corpus_composite->linkWithOptions(
                corpus_linked.writeRef(), 1u, &corpus_option, corpus_diagnostics.writeRef())));

            auto *const corpus_layout = corpus_linked->getLayout();
            PPR_TEST_ASSERT(corpus_layout != nullptr);
            for (const auto &[name, bytes]: std::array<std::pair<const char *, std::size_t>, 3>{
                     std::pair{"g_fp64_corpus_inputs", sizeof(CorpusInput)},
                     {"g_fp64_corpus_outputs", sizeof(CorpusOutput)},
                     {"g_fp64_corpus_params", sizeof(CorpusParameters)}}) {
                bool found = false;
                for (unsigned index = 0u; index < corpus_layout->getParameterCount(); ++index) {
                    auto *const field = corpus_layout->getParameterByIndex(index);
                    if (std::string_view{field->getName()} != name) {
                        continue;
                    }

                    auto *const element = field->getTypeLayout()->getElementTypeLayout();
                    PPR_TEST_ASSERT(element != nullptr);
                    PPR_TEST_ASSERT(element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes);
                    if (std::string_view{name} != "g_fp64_corpus_params") {
                        PPR_TEST_ASSERT(element->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes);
                        PPR_TEST_ASSERT(element->getAlignment(SLANG_PARAMETER_CATEGORY_UNIFORM) == 4);
                    }

                    found = true;
                    break;
                }

                PPR_TEST_ASSERT(found);
            }

            shader::ComPtr<slang::IBlob> corpus_code{};
            PPR_TEST_ASSERT(not make_error_code(
                corpus_linked->getEntryPointCode(0, 0, corpus_code.writeRef(), corpus_diagnostics.writeRef())));
            PPR_TEST_ASSERT(corpus_code and corpus_code->getBufferSize() >= sizeof(u32));

            u32 corpus_magic{};
            std::memcpy(&corpus_magic, corpus_code->getBufferPointer(), sizeof(corpus_magic));
            PPR_TEST_ASSERT(corpus_magic == 0x07230203u);

            const std::filesystem::path output_directory{*export_directory};
            std::filesystem::create_directories(output_directory, filesystem_error);
            PPR_TEST_ASSERT(not filesystem_error);

            PPR_TEST_ASSERT(
                corpus_code->getBufferSize() <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));
            std::ofstream code_stream{
                output_directory / "terrain-fp64-arithmetic-corpus.spv", std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(code_stream.is_open());
            code_stream.write(
                static_cast<const char *>(corpus_code->getBufferPointer()),
                static_cast<std::streamsize>(corpus_code->getBufferSize()));
            code_stream.close();
            PPR_TEST_ASSERT(not code_stream.fail());

            rhi::ShaderProgramDesc corpus_program_description{};
            corpus_program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            corpus_program_description.slangGlobalScope = corpus_linked.get();
            PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                corpus_program_description, corpus_program.writeRef(), corpus_diagnostics.writeRef())));

            rhi::ComputePipelineDesc corpus_pipeline_description{};
            corpus_pipeline_description.program = corpus_program;
            PPR_TEST_ASSERT(not make_error_code(
                device.createComputePipeline(corpus_pipeline_description, corpus_pipeline.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(corpus_program, corpus_root.writeRef())));

            rhi::BufferDesc corpus_buffer_description{};
            corpus_buffer_description.size = input_bytes;
            corpus_buffer_description.elementSize = sizeof(CorpusInput);
            corpus_buffer_description.memoryType = rhi::MemoryType::Upload;
            corpus_buffer_description.usage = rhi::BufferUsage::ShaderResource;
            corpus_buffer_description.defaultState = rhi::ResourceState::ShaderResource;
            PPR_TEST_ASSERT(not make_error_code(
                device.createBuffer(corpus_buffer_description, values.data(), corpus_inputs.writeRef())));

            corpus_buffer_description.size = output_bytes;
            corpus_buffer_description.elementSize = sizeof(CorpusOutput);
            corpus_buffer_description.memoryType = rhi::MemoryType::DeviceLocal;
            corpus_buffer_description.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
            corpus_buffer_description.defaultState = rhi::ResourceState::UnorderedAccess;
            PPR_TEST_ASSERT(not make_error_code(
                device.createBuffer(corpus_buffer_description, nullptr, corpus_outputs.writeRef())));

            corpus_buffer_description.memoryType = rhi::MemoryType::ReadBack;
            corpus_buffer_description.usage = rhi::BufferUsage::CopyDestination;
            corpus_buffer_description.defaultState = rhi::ResourceState::CopyDestination;
            PPR_TEST_ASSERT(not make_error_code(
                device.createBuffer(corpus_buffer_description, nullptr, corpus_readback.writeRef())));

            PPR_TEST_ASSERT(
                corpus_inputs->getDesc().size == input_bytes and
                corpus_inputs->getDesc().elementSize == sizeof(CorpusInput));
            for (auto *const buffer: {corpus_outputs.get(), corpus_readback.get()}) {
                PPR_TEST_ASSERT(
                    buffer != nullptr and
                    buffer->getDesc().size == output_bytes and
                    buffer->getDesc().elementSize == sizeof(CorpusOutput));
            }

            rhi::ShaderCursor corpus_cursor{corpus_root};
            rhi::ShaderCursor corpus_constants{};
            const CorpusParameters corpus_parameters{count, 0u, 0u, 0u};
            PPR_TEST_ASSERT(
                not make_error_code(corpus_cursor["g_fp64_corpus_params"].getDereferenced(corpus_constants)));
            PPR_TEST_ASSERT(
                not make_error_code(corpus_constants.setData(&corpus_parameters, sizeof(corpus_parameters))));
            PPR_TEST_ASSERT(not make_error_code(
                corpus_cursor["g_fp64_corpus_inputs"].setBinding(
                    rhi::Binding(corpus_inputs, {.offset = 0u, .size = input_bytes}))));
            PPR_TEST_ASSERT(not make_error_code(
                corpus_cursor["g_fp64_corpus_outputs"].setBinding(
                    rhi::Binding(corpus_outputs, {.offset = 0u, .size = output_bytes}))));

            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(corpus_encoder.writeRef())));
            corpus_encoder->setBufferState(corpus_inputs, rhi::ResourceState::ShaderResource);
            corpus_encoder->setBufferState(corpus_outputs, rhi::ResourceState::UnorderedAccess);
            corpus_encoder->setBufferState(corpus_readback, rhi::ResourceState::CopyDestination);
            auto *const corpus_compute = corpus_encoder->beginComputePass();
            PPR_TEST_ASSERT(corpus_compute != nullptr);
            corpus_compute->bindPipeline(corpus_pipeline, corpus_root);
            corpus_compute->dispatchCompute((count + 63u) / 64u, 1u, 1u);
            corpus_compute->end();

            corpus_encoder->globalBarrier();
            corpus_encoder->setBufferState(corpus_outputs, rhi::ResourceState::CopySource);
            corpus_encoder->copyBuffer(corpus_readback, 0u, corpus_outputs, 0u, output_bytes);
            PPR_TEST_ASSERT(not make_error_code(corpus_encoder->finish(corpus_commands.writeRef())));

            rhi::ICommandBuffer *corpus_command_pointer = corpus_commands.get();
            rhi::IFence *corpus_signal = fixture.m_fence.fence();
            const auto corpus_submitted = fixture.m_fence.nextValue();
            const u64 corpus_raw_submitted = corpus_submitted;
            rhi::SubmitDesc corpus_submission{};
            corpus_submission.commandBuffers = &corpus_command_pointer;
            corpus_submission.commandBufferCount = 1u;
            corpus_submission.signalFences = &corpus_signal;
            corpus_submission.signalFenceValues = &corpus_raw_submitted;
            corpus_submission.signalFenceCount = 1u;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(corpus_submission)));

            fixture.m_fence.commitSubmitted(corpus_submitted);
            PPR_TEST_ASSERT(not fixture.m_fence.waitFor(corpus_submitted));

            std::vector<CorpusOutput> rows(count);
            void *corpus_mapped{};
            PPR_TEST_ASSERT(
                not make_error_code(device.mapBuffer(corpus_readback, rhi::CpuAccessMode::Read, &corpus_mapped)));
            std::memcpy(rows.data(), corpus_mapped, static_cast<std::size_t>(output_bytes));
            PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(corpus_readback)));

            std::ofstream result_stream{
                output_directory / "terrain-fp64-arithmetic-results-32B-le.bin", std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(result_stream.is_open());
            result_stream.write(
                reinterpret_cast<const char *>(rows.data()), static_cast<std::streamsize>(output_bytes));
            result_stream.close();
            PPR_TEST_ASSERT(not result_stream.fail());

            // Exact Fraction/square analysis owns numerical qualification. This
            // sentinel reports only a fenced dispatch and complete raw capture.
            std::println("FP64_CORPUS_CAPTURE_COMPLETE: {} raw records; numerical qualification not executed", count);
            return;
        }

        if (prototype_directory) {
            PPR_TEST_ASSERT(not prototype_directory->empty());
            PPR_TEST_ASSERT(device.getInfo().deviceType == rhi::DeviceType::Vulkan);
            const auto export_directory = config::getenvLookup("PPR_TEST_FP64_SPIRV_DIRECTORY");
            PPR_TEST_ASSERT(export_directory and not export_directory->empty());

            shader::SharedModule prototype_module{};
            shader::ComPtr<slang::IEntryPoint> prototype_entry{};
            shader::ComPtr<slang::IComponentType> prototype_composite{}, prototype_linked{};
            shader::Diagnose prototype_diagnostics{};
            rhi::ComPtr<rhi::IShaderProgram> prototype_program{};
            rhi::ComPtr<rhi::IComputePipeline> prototype_pipeline{};
            PPR_DEFER {
                if (make_error_code(fixture.m_queue->waitOnHost())) {
                    hal::fatalExit("fp64-prototype-completion-unknown");
                }
            };

            // This explicit diagnostic reads artifact-local copies. It never
            // dispatches or admits query output and never initializes the pass.
            const std::filesystem::path directory{*prototype_directory};
            std::println("FP64_PROTOTYPE_PHASE module-load-before");
            std::ignore = std::fflush(nullptr); // Best-effort watchdog observation.
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                directory / "radiance_cascades.slang", "terrain_fp64_refinement_prototype",
                prototype_module.writeRef()));
            PPR_TEST_ASSERT(not make_error_code(
                prototype_module->findEntryPointByName("terrainReceivers", prototype_entry.writeRef())));

            slang::IComponentType *prototype_parts[]{prototype_module.get(), prototype_entry.get()};
            PPR_TEST_ASSERT(not make_error_code(prototype_module->getSession()->createCompositeComponentType(
                prototype_parts, 2, prototype_composite.writeRef(), prototype_diagnostics.writeRef())));

            slang::CompilerOptionEntry prototype_option{};
            prototype_option.name = slang::CompilerOptionName::FloatingPointMode;
            prototype_option.value.kind = slang::CompilerOptionValueKind::Int;
            prototype_option.value.intValue0 = SLANG_FLOATING_POINT_MODE_PRECISE;
            PPR_TEST_ASSERT(not make_error_code(prototype_composite->linkWithOptions(
                prototype_linked.writeRef(), 1u, &prototype_option, prototype_diagnostics.writeRef())));

            std::println("FP64_PROTOTYPE_PHASE linked-code-before");
            std::ignore = std::fflush(nullptr); // Best-effort watchdog observation.
            shader::ComPtr<slang::IBlob> prototype_code{};
            PPR_TEST_ASSERT(not make_error_code(prototype_linked->getEntryPointCode(
                0, 0, prototype_code.writeRef(), prototype_diagnostics.writeRef())));
            PPR_TEST_ASSERT(prototype_code and prototype_code->getBufferSize() >= sizeof(u32));

            u32 prototype_magic{};
            std::memcpy(&prototype_magic, prototype_code->getBufferPointer(), sizeof(prototype_magic));
            PPR_TEST_ASSERT(prototype_magic == 0x07230203u);

            const std::filesystem::path output_directory{*export_directory};
            std::error_code filesystem_error{};
            std::filesystem::create_directories(output_directory, filesystem_error);
            PPR_TEST_ASSERT(not filesystem_error);

            PPR_TEST_ASSERT(
                prototype_code->getBufferSize() <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));
            std::ofstream prototype_stream{
                output_directory / "terrain-receivers-fp64-prototype.spv", std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(prototype_stream.is_open());
            prototype_stream.write(
                static_cast<const char *>(prototype_code->getBufferPointer()),
                static_cast<std::streamsize>(prototype_code->getBufferSize()));
            prototype_stream.close();
            PPR_TEST_ASSERT(not prototype_stream.fail());

            rhi::ShaderProgramDesc prototype_program_description{};
            prototype_program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            prototype_program_description.slangGlobalScope = prototype_linked.get();
            PPR_TEST_ASSERT(not make_error_code(device.createShaderProgram(
                prototype_program_description, prototype_program.writeRef(), prototype_diagnostics.writeRef())));

            rhi::ComputePipelineDesc prototype_pipeline_description{};
            prototype_pipeline_description.program = prototype_program;
            std::println("FP64_PROTOTYPE_PHASE native-pipeline-before");
            std::ignore = std::fflush(nullptr); // Best-effort watchdog observation.
            PPR_TEST_ASSERT(not make_error_code(
                device.createComputePipeline(prototype_pipeline_description, prototype_pipeline.writeRef())));

            std::println("FP64_PROTOTYPE_COMPILE_ONLY_PASS: one receiver pipeline; physical assertions not executed");
            std::ignore = std::fflush(nullptr); // Best-effort final diagnostic sentinel.
            return;
        }

        static_assert(sizeof(double) == 8u and alignof(double) == 8u);
        static_assert(std::numeric_limits<double>::is_iec559 and std::numeric_limits<double>::digits == 53);
        static_assert(sizeof(std::array<double, 4>) == 32u and alignof(std::array<double, 4>) == 8u);
        static_assert(std::is_trivially_copyable_v<std::array<double, 4>>);

        shader::SharedModule module{};
        shader::ComPtr<slang::IEntryPoint> entry{};
        shader::ComPtr<slang::IComponentType> composite{}, linked{};
        shader::Diagnose diagnostics{};
        rhi::ComPtr<rhi::IShaderProgram> program{};
        rhi::ComPtr<rhi::IComputePipeline> pipeline{};
        rhi::ComPtr<rhi::IShaderObject> root{};
        rhi::ComPtr<rhi::IBuffer> inputs{}, outputs{}, readback{};
        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        rhi::ComPtr<rhi::ICommandBuffer> commands{};
        PPR_DEFER {
            // This guard runs before any referenced resource is released, also
            // after a failed/ambiguous submit or a test assertion during setup.
            if (make_error_code(fixture.m_queue->waitOnHost())) {
                hal::fatalExit("fp64-capability-completion-unknown");
            }
        };

        constexpr string_literal source = R"(
StructuredBuffer<double> g_fp64_inputs;
RWStructuredBuffer<double> g_fp64_outputs;

[shader("compute")]
[numthreads(1, 1, 1)]
void probeFp64() {
    precise double a = g_fp64_inputs[0];
    precise double b = g_fp64_inputs[1];
    precise double c = g_fp64_inputs[2];
    precise double d = g_fp64_inputs[3];

    precise double sum = a + b;
    g_fp64_outputs[0] = sum;

    precise double difference = sum - a;
    g_fp64_outputs[1] = difference;

    precise double product = sum * c;
    g_fp64_outputs[2] = product;

    precise double quotient = product / d;
    g_fp64_outputs[3] = quotient;
}
)";
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromSource(
            "radiance_fp64_runtime_probe", "radiance_fp64_runtime_probe.slang", source, module.writeRef()));
        PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("probeFp64", entry.writeRef())));

        slang::IComponentType *parts[]{module.get(), entry.get()};
        PPR_TEST_ASSERT(not make_error_code(module->getSession()->createCompositeComponentType(
            parts, 2, composite.writeRef(), diagnostics.writeRef())));

        slang::CompilerOptionEntry option{};
        option.name = slang::CompilerOptionName::FloatingPointMode;
        option.value.kind = slang::CompilerOptionValueKind::Int;
        option.value.intValue0 = SLANG_FLOATING_POINT_MODE_PRECISE;
        PPR_TEST_ASSERT(
            not make_error_code(composite->linkWithOptions(linked.writeRef(), 1u, &option, diagnostics.writeRef())));

        auto *const layout = linked->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        for (const std::string_view name: {"g_fp64_inputs", "g_fp64_outputs"}) {
            bool found = false;
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const field = layout->getParameterByIndex(index);
                if (std::string_view{field->getName()} != name) {
                    continue;
                }

                auto *const element = field->getTypeLayout()->getElementTypeLayout();
                PPR_TEST_ASSERT(element != nullptr);
                PPR_TEST_ASSERT(element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
                PPR_TEST_ASSERT(element->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u);
                PPR_TEST_ASSERT(element->getAlignment(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8);
                found = true;
                break;
            }

            PPR_TEST_ASSERT(found);
        }

        const auto export_directory = config::getenvLookup("PPR_TEST_FP64_SPIRV_DIRECTORY");
        if (export_directory and not export_directory->empty()) {
            PPR_TEST_ASSERT(device.getInfo().deviceType == rhi::DeviceType::Vulkan);
            shader::ComPtr<slang::IBlob> code{};
            PPR_TEST_ASSERT(
                not make_error_code(linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())));
            PPR_TEST_ASSERT(code and code->getBufferSize() >= sizeof(u32));

            u32 magic{};
            std::memcpy(&magic, code->getBufferPointer(), sizeof(magic));
            PPR_TEST_ASSERT(magic == 0x07230203u);

            const std::filesystem::path directory{*export_directory};
            std::error_code filesystem_error{};
            std::filesystem::create_directories(directory, filesystem_error);
            PPR_TEST_ASSERT(not filesystem_error);

            PPR_TEST_ASSERT(code->getBufferSize() <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()));
            std::ofstream stream{directory / "fp64-runtime.spv", std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(stream.is_open());
            stream.write(
                static_cast<const char *>(code->getBufferPointer()),
                static_cast<std::streamsize>(code->getBufferSize()));
            stream.close();
            PPR_TEST_ASSERT(not stream.fail());
        }

        rhi::ShaderProgramDesc program_description{};
        program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_description.slangGlobalScope = linked.get();
        PPR_TEST_ASSERT(not make_error_code(
            device.createShaderProgram(program_description, program.writeRef(), diagnostics.writeRef())));

        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program;
        PPR_TEST_ASSERT(not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

        // The sum/product exceed exact FP32 integer precision. Runtime divisor2
        // keeps this capability check independent of a general division tolerance.
        constexpr std::array<double, 4> values{16777216.0, 1.0, 3.0, 2.0};
        constexpr std::array<double, 4> expected{16777217.0, 1.0, 50331651.0, 25165825.5};
        rhi::BufferDesc description{};
        description.size = sizeof(values);
        description.elementSize = sizeof(double);
        description.memoryType = rhi::MemoryType::Upload;
        description.usage = rhi::BufferUsage::ShaderResource;
        description.defaultState = rhi::ResourceState::ShaderResource;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, values.data(), inputs.writeRef())));

        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, outputs.writeRef())));

        description.memoryType = rhi::MemoryType::ReadBack;
        description.usage = rhi::BufferUsage::CopyDestination;
        description.defaultState = rhi::ResourceState::CopyDestination;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, readback.writeRef())));

        for (auto *const buffer: {inputs.get(), outputs.get(), readback.get()}) {
            PPR_TEST_ASSERT(
                buffer != nullptr and
                buffer->getDesc().size == sizeof(values) and
                buffer->getDesc().elementSize == sizeof(double));
        }

        rhi::ShaderCursor cursor{root};
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_fp64_inputs"].setBinding(rhi::Binding(inputs, {.offset = 0u, .size = sizeof(values)}))));
        PPR_TEST_ASSERT(not make_error_code(
            cursor["g_fp64_outputs"].setBinding(rhi::Binding(outputs, {.offset = 0u, .size = sizeof(values)}))));

        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        encoder->setBufferState(inputs, rhi::ResourceState::ShaderResource);
        encoder->setBufferState(outputs, rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(readback, rhi::ResourceState::CopyDestination);
        auto *const compute = encoder->beginComputePass();
        PPR_TEST_ASSERT(compute != nullptr);
        compute->bindPipeline(pipeline, root);
        compute->dispatchCompute(1u, 1u, 1u);
        compute->end();

        encoder->globalBarrier();
        encoder->setBufferState(outputs, rhi::ResourceState::CopySource);
        encoder->copyBuffer(readback, 0u, outputs, 0u, sizeof(values));
        PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));

        rhi::ICommandBuffer *command_pointer = commands.get();
        rhi::IFence *signal = fixture.m_fence.fence();
        const auto submitted = fixture.m_fence.nextValue();
        const u64 raw_submitted = submitted;
        rhi::SubmitDesc submission{};
        submission.commandBuffers = &command_pointer;
        submission.commandBufferCount = 1u;
        submission.signalFences = &signal;
        submission.signalFenceValues = &raw_submitted;
        submission.signalFenceCount = 1u;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submission)));

        fixture.m_fence.commitSubmitted(submitted);
        PPR_TEST_ASSERT(not fixture.m_fence.waitFor(submitted));

        std::array<double, 4> actual{};
        void *mapped{};
        PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
        std::memcpy(actual.data(), mapped, sizeof(actual));
        PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

        for (std::size_t index = 0u; index < actual.size(); ++index) {
            std::println(
                "FP64_RUNTIME_RESULT index={} value={:.17g} bits={:016x}", index, actual[index],
                std::bit_cast<u64>(actual[index]));
            PPR_TEST_ASSERT(std::isfinite(actual[index]) and actual[index] == expected[index]);
        }

        std::println("FP64_RUNTIME_PASS: four binary64 runtime operations verified; capability probe only");
    };

    // ------------------------------------------------------------------
    // actual raster and terrain canonical coefficient consumers
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (canonical_roundness_bits_match_actual_raster_and_terrain_paths) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(false); // No unrelated radiance program/pipeline families.
        auto &device = fixture.m_rhi->getDevice();

        // Literal dyadic results independently cover midpoint parity, binade
        // carry, large shift/sticky and subnormal encoding. Last word records
        // only the terrain probe's normal-input eligibility, not transport success.
        constexpr std::array<std::array<u32, 4>, 17> cases{{
            {0x3f0ccccdu, 0x3f19999au, 0x3f133334u, 0u},
            {0x3f19999au, 0x3f0ccccdu, 0x3f133334u, 0u},
            {0u, 0x3f800000u, 0x3f000000u, 0u},
            {0x3f800000u, 0x3f800000u, 0x3f800000u, 0u},
            {0u, 0u, 0u, 0u},
            {0x3f000000u, 0x3f000001u, 0x3f000000u, 0u},
            {0x3f000001u, 0x3f000002u, 0x3f000002u, 0u},
            {0x3effffffu, 0x3f000000u, 0x3f000000u, 0u},
            {0x3f7fffffu, 0x3f800000u, 0x3f800000u, 0u},
            {0x3f800000u, 0x2b800000u, 0x3f000000u, 0u},
            {0x3f800000u, 0x33800000u, 0x3f000000u, 0u},
            {0x3f800000u, 0x33800001u, 0x3f000001u, 0u},
            {0u, 1u, 0u, 32u},
            {0u, 3u, 2u, 32u},
            {0x007fffffu, 0x00800000u, 0x00800000u, 32u},
            {0x00800000u, 0u, 0x00400000u, 32u},
            {0x80000000u, 0u, 0u, 0u},
        }};

        struct TerrainVertex {
            float m_roundness{};
            u32 m_material{};
        };
        static_assert(sizeof(TerrainVertex) == 8u and alignof(TerrainVertex) == 4u);
        static_assert(std::is_standard_layout_v<TerrainVertex> and std::is_trivially_copyable_v<TerrainVertex>);
        static_assert(PPR_OFFSETOF(TerrainVertex, m_roundness) == 0u);
        static_assert(PPR_OFFSETOF(TerrainVertex, m_material) == 4u);
        static_assert(sizeof(std::array<u32, 2>) == 8u and sizeof(std::array<u32, 4>) == 16u);

        std::array<GridTerrainVisual, 1u + 2u * cases.size()> palette{};
        std::array<TerrainVertex, 1u + 2u * cases.size()> vertices{};
        std::array<std::array<u32, 2>, cases.size()> pairs{};
        for (std::size_t index = 0u; index < cases.size(); ++index) {
            for (u32 endpoint = 0u; endpoint < 2u; ++endpoint) {
                const u32 id = static_cast<u32>(1u + 2u * index + endpoint);
                palette[id].m_material_id = id;
                palette[id].m_roundness = std::bit_cast<float>(cases[index][endpoint]);
                vertices[id] = {palette[id].m_roundness, id};
                pairs[index][endpoint] = id;
            }
        }

        rhi::ComPtr<rhi::IBuffer> palette_buffer, vertices_buffer, pairs_buffer, points_buffer, cases_buffer;
        rhi::ComPtr<rhi::IBuffer> output, readback;
        const auto upload = [&](const void *const data, const u64 bytes, const u32 stride,
                                rhi::ComPtr<rhi::IBuffer> &buffer) {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::Upload;
            description.usage = rhi::BufferUsage::ShaderResource;
            description.defaultState = rhi::ResourceState::ShaderResource;
            PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, data, buffer.writeRef())));
            PPR_TEST_ASSERT(buffer->getDesc().elementSize == stride and buffer->getDesc().size == bytes);
        };

        upload(palette.data(), sizeof(palette), sizeof(GridTerrainVisual), palette_buffer);
        upload(vertices.data(), sizeof(vertices), sizeof(TerrainVertex), vertices_buffer);
        upload(pairs.data(), sizeof(pairs), sizeof(pairs[0]), pairs_buffer);
        const auto points = math::contourPoints();
        const auto contour_cases = math::contourCases();
        upload(points.data(), points.size_bytes(), sizeof(math::ContourPoint), points_buffer);
        upload(contour_cases.data(), contour_cases.size_bytes(), sizeof(math::ContourCase), cases_buffer);

        std::array<std::array<u32, 4>, cases.size()> rows{};
        rhi::BufferDesc description{};
        description.size = sizeof(rows);
        description.elementSize = sizeof(rows[0]);
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) | static_cast<u32>(rhi::BufferUsage::CopySource));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, output.writeRef())));

        description.elementSize = 0u;
        description.memoryType = rhi::MemoryType::ReadBack;
        description.usage = rhi::BufferUsage::CopyDestination;
        description.defaultState = rhi::ResourceState::CopyDestination;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, nullptr, readback.writeRef())));

        for (const bool terrain: {false, true}) {
            shader::SharedModule module{};
            shader::ComPtr<slang::IEntryPoint> entry{};
            rhi::ComPtr<rhi::IShaderProgram> program{};
            rhi::ComPtr<rhi::IComputePipeline> pipeline{};
            rhi::ComPtr<rhi::IShaderObject> root{};
            rhi::ComPtr<rhi::ICommandEncoder> encoder{};
            rhi::ComPtr<rhi::ICommandBuffer> commands{};
            PPR_DEFER {
                // Every submitted/ambiguous path proves idleness before any
                // command, root, pipeline or buffer referenced here can die.
                if (make_error_code(fixture.m_queue->waitOnHost())) {
                    hal::fatalExit("contour-coefficient-qualification-completion-unknown");
                }
            };

            const char *const module_name = terrain ? "radiance_cascades" : "grid_tiles";
            const char *const entry_name = terrain ? "qualifyTerrainContourRoundness" : "qualifyRasterContourRoundness";
            PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
                std::filesystem::current_path() / "shaders" / (std::string{module_name} + ".slang"), module_name,
                module.writeRef()));
            PPR_TEST_ASSERT(not shader::make_error_code(module->findEntryPointByName(entry_name, entry.writeRef())));

            auto *const layout = module->getLayout();
            PPR_TEST_ASSERT(layout != nullptr);
            for (const auto &[name, stride]: std::array<std::pair<const char *, std::size_t>, 4>{
                     std::pair{terrain ? "g_terrain_vertices" : "g_terrain_visuals", terrain ? 8u : 96u},
                     {"g_contour_roundness_pairs", 8u}, {"g_contour_roundness_probe", 16u},
                     {"g_contour_roundness_probe_params", 16u}}) {
                bool found = false;
                for (unsigned parameter = 0u; parameter < layout->getParameterCount(); ++parameter) {
                    auto *const field = layout->getParameterByIndex(parameter);
                    if (std::string_view{field->getName()} == name) {
                        found = field->getTypeLayout()->getElementTypeLayout()->getSize(
                                    SLANG_PARAMETER_CATEGORY_UNIFORM) == stride;
                        break;
                    }
                }

                PPR_TEST_ASSERT(found);
            }

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc program_description{};
            program_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_description.slangGlobalScope = module.get();
            program_description.slangEntryPoints = entries;
            program_description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics{};
            PPR_TEST_ASSERT(not make_error_code(
                device.createShaderProgram(program_description, program.writeRef(), diagnostics.writeRef())));

            rhi::ComputePipelineDesc pipeline_description{};
            pipeline_description.program = program;
            PPR_TEST_ASSERT(
                not make_error_code(device.createComputePipeline(pipeline_description, pipeline.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

            rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor constants{};
            const std::array<u32, 4> parameters{static_cast<u32>(cases.size()), 0u, 0u, 0u};
            PPR_TEST_ASSERT(not make_error_code(cursor["g_contour_roundness_probe_params"].getDereferenced(constants)));
            PPR_TEST_ASSERT(not make_error_code(constants.setData(parameters.data(), sizeof(parameters))));
            if (not terrain) {
                GridPass::GridFrame frame{};
                frame.m_surface_mode = 2u;
                PPR_TEST_ASSERT(not make_error_code(cursor["g_grid_frame"].getDereferenced(constants)));
                PPR_TEST_ASSERT(not make_error_code(constants.setData(&frame, sizeof(frame))));
            }

            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 5>{
                     std::pair{
                         terrain ? "g_terrain_vertices" : "g_terrain_visuals",
                         terrain ? vertices_buffer.get() : palette_buffer.get()},
                     {"g_contour_roundness_pairs", pairs_buffer.get()}, {"g_contour_points", points_buffer.get()},
                     {"g_contour_cases", cases_buffer.get()}, {"g_contour_roundness_probe", output.get()}}) {
                PPR_TEST_ASSERT(not make_error_code(
                    cursor[name].setBinding(rhi::Binding(buffer, {.offset = 0u, .size = buffer->getDesc().size}))));
            }

            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
            for (auto *const buffer:
                {terrain ? vertices_buffer.get() : palette_buffer.get(), pairs_buffer.get(), points_buffer.get(),
                    cases_buffer.get()}) {
                encoder->setBufferState(buffer, rhi::ResourceState::ShaderResource);
            }

            encoder->setBufferState(output, rhi::ResourceState::UnorderedAccess);
            encoder->setBufferState(readback, rhi::ResourceState::CopyDestination);
            auto *const compute = encoder->beginComputePass();
            PPR_TEST_ASSERT(compute != nullptr);
            compute->bindPipeline(pipeline, root);
            compute->dispatchCompute(1u, 1u, 1u);
            compute->end();

            encoder->globalBarrier();
            encoder->setBufferState(output, rhi::ResourceState::CopySource);
            encoder->copyBuffer(readback, 0u, output, 0u, sizeof(rows));
            PPR_TEST_ASSERT(not make_error_code(encoder->finish(commands.writeRef())));

            rhi::ICommandBuffer *command_pointer = commands.get();
            rhi::SubmitDesc submission{};
            submission.commandBuffers = &command_pointer;
            submission.commandBufferCount = 1u;
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(submission)));
            PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));

            void *mapped{};
            PPR_TEST_ASSERT(not make_error_code(device.mapBuffer(readback, rhi::CpuAccessMode::Read, &mapped)));
            std::memcpy(rows.data(), mapped, sizeof(rows));
            PPR_TEST_ASSERT(not make_error_code(device.unmapBuffer(readback)));

            for (std::size_t index = 0u; index < cases.size(); ++index) {
                PPR_TEST_ASSERT(rows[index][0] == cases[index][2]);
                PPR_TEST_ASSERT(rows[index][1] == cases[index][2]);
                PPR_TEST_ASSERT(rows[index][2] == 1u);
                PPR_TEST_ASSERT(rows[index][3] == (terrain ? cases[index][3] : 0u));
            }
        }
    };

    // ------------------------------------------------------------------
    // noise-free quadratic terrain and explicit numerical ambiguity
    // ------------------------------------------------------------------

    bool terrainBackend_(Fixture &fixture) {
        if (fixture.m_rhi->getDevice().getInfo().deviceType == rhi::DeviceType::Vulkan) {
            return true;
        }

        const std::array<u16, 1> labels{};
        const std::array<lighting::OpticalTerrainMaterial, 1> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}};
        const auto snapshot = lighting::OpticalTerrainSnapshot::capture(
            {labels, materials, {}, 1.0, 1u, 1u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(snapshot.has_value());
        const std::array<lighting::OpticalRay, 1> rays{lighting::OpticalRay{{0.5, 0.5}, {1.0, 0.0}, 0.0, 0.125}};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(
                *snapshot, rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone) ==
            std::errc::not_supported);
        std::println("rounded terrain qualification explicitly unsupported on this backend; no GPU geometry claim");
        return false;
    }

    PPR_UNIT_TEST (rounded_terrain_trace_matches_analytic_sources_and_immutable_rebuilds) {
        const auto export_setting = config::getenvLookup("PPR_TEST_TERRAIN_SPIRV_EXPORT_ONLY");
        const bool export_only = export_setting and *export_setting == "1";
        const auto dump = config::getenvLookup("PPR_TEST_TERRAIN_SPIRV_DIRECTORY");
        PPR_TEST_ASSERT(not export_only or (dump and not dump->empty()));

        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        if (not terrainBackend_(fixture)) {
            PPR_TEST_ASSERT(not export_only);
            return;
        }

        if (dump and not dump->empty()) {
            PPR_TEST_ASSERT(not fixture.m_pass.writeTerrainSpirvForQualification(std::filesystem::path{*dump}, false));

            if (export_only) {
                PPR_TEST_ASSERT(
                    not fixture.m_pass.writeTerrainSpirvForQualification(std::filesystem::path{*dump}, true));

                std::println("IR export diagnostic only; physical assertions not executed");
                return;
            }
        }

        std::array<u16, 4> labels{17u, 0u, 0u, 0u};
        lighting::OpticalCell surface{};
        surface.m_kind = lighting::OpticalKind::opaque;
        surface.m_surface_emission = {8.0, 2.0, 1.0};
        std::array<lighting::OpticalTerrainMaterial, 5> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface, 17u, 1.0f}, {surface, 23u, 0.5f},
            {surface, 31u, 1.0f}, {surface, 47u, 0.0f}};
        materials[2].m_optics.m_surface_emission = {1.0, 9.0, 2.0};
        materials[3].m_optics.m_surface_emission = {2.0, 1.0, 10.0};
        materials[4].m_optics.m_surface_emission = {3.0, 4.0, 5.0};
        lighting::OpticalTerrainView input{labels, materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum};
        const auto retained = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(retained.has_value());

        // Both GPU geometry modes reject FP32-subnormal opaque sources on
        // upload; supported binary16-subnormal sources are normal FP32 values.
        auto weak_materials = materials;
        weak_materials[1].m_optics.m_surface_emission[0] = std::ldexp(1.0, -127);
        auto weak_input = input;
        weak_input.m_materials = weak_materials;
        const auto weak_shape = lighting::OpticalTerrainSnapshot::capture(weak_input);
        PPR_TEST_ASSERT(weak_shape.has_value());
        const std::array<lighting::OpticalRay, 1> weak_ray{
            lighting::OpticalRay{{0.625, 0.625}, {1.0, 0.0}, 0.0, 0.0625}};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(
                *weak_shape, weak_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()) ==
            std::errc::argument_out_of_domain);

        // This source is uploaded only through the halo parameters: material23
        // is absent from every in-world label, so cell-row checks cannot see it.
        weak_materials = materials;
        weak_materials[2].m_optics.m_surface_emission[0] = std::ldexp(1.0, -127);
        weak_input.m_outside_material = 23u;
        PPR_TEST_ASSERT(std::ranges::find(labels, u16{23u}) == labels.end());

        const auto weak_halo = lighting::OpticalTerrainSnapshot::capture(weak_input);
        PPR_TEST_ASSERT(weak_halo.has_value());

        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(
                *weak_halo, weak_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()) ==
            std::errc::argument_out_of_domain);

        const std::array<lighting::OpticalRay, 4> rays{
            lighting::OpticalRay{{1.375, 0.875}, {-1.0, 0.0}, 0.0, 1.0}, {{0.625, 0.625}, {1.0, 0.125}, 0.0, 0.125},
            {{1.375, 1.375}, {-1.0, 0.125}, 0.0, 0.125}, {{1.375, 0.875}, {-1.0, 0.0}, 0.0, 0.25}};
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, false, false}));
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(*retained, rays, 1u, 0u, 32u * 1024u * 1024u, kFenceValueNone) ==
            std::errc::no_buffer_space);
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, false, false}));

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *retained, rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{true, false, false}));
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(*retained, rays, 1u, 0u, 32u * 1024u * 1024u, kFenceValueNone) ==
            std::errc::no_buffer_space);

        labels[0] = 0u;
        materials[1].m_optics.m_surface_emission = {100.0, 100.0, 100.0};

        fixture.submit();
        const auto completed = fixture.m_fence.pollCompleted();
        PPR_TEST_ASSERT(completed.has_value());

        const auto first = fixture.m_pass.readCompleted(*completed);
        PPR_TEST_ASSERT(first.has_value() and first->m_intervals.size() == rays.size());

        for (std::size_t index = 0u; index < rays.size(); ++index) {
            const auto expected = lighting::traceOpticalTerrainReference(*retained, rays[index]);
            PPR_TEST_ASSERT(expected.has_value());
            assertClose_(first->m_intervals[index], expected->m_interval);
        }

        // Exact schedule probe origins on dual planes retain directional
        // half-open ownership, including a zero component. Translation and a
        // smaller metre metric must not turn that identity into uncertainty.
        const std::array<lighting::OpticalRay, 10> half_plane_rays{
            lighting::OpticalRay{{1.5, 0.5}, {-1.0, 0.0}, 0.0, 0.125}, {{1.5, 0.5}, {1.0, 0.0}, 0.0, 0.125},
            {{1.5, 0.5}, {0.0, -1.0}, 0.0, 0.125}, {{1.5, 0.5}, {-1.0, 8.0}, 0.0, 0.125},
            {{0.5, 1.5}, {0.0, -1.0}, 0.0, 0.125}, {{0.5, 1.5}, {0.0, 1.0}, 0.0, 0.125},
            {{0.5, 1.5}, {-1.0, 0.0}, 0.0, 0.125}, {{0.5, 1.5}, {8.0, -1.0}, 0.0, 0.125},
            {{1.5, 1.5}, {1.0, -1.0}, 0.0, 0.125}, {{1.5, 1.5}, {-1.0, 1.0}, 0.0, 0.125}};

        for (const bool translated: {false, true}) {
            auto half_plane_view = retained->view();
            half_plane_view.m_min_m = translated ? std::array{1000000.0, -1000000.0} : std::array{0.0, 0.0};
            half_plane_view.m_cell_size_m = translated ? 0.25 : 1.0;
            const auto shape = lighting::OpticalTerrainSnapshot::capture(half_plane_view);
            PPR_TEST_ASSERT(shape.has_value());

            auto world_rays = half_plane_rays;
            for (auto &ray: world_rays) {
                for (std::size_t axis = 0u; axis < 2u; ++axis) {
                    ray.m_origin_m[axis] = half_plane_view.m_min_m[axis] +
                                           ray.m_origin_m[axis] * half_plane_view.m_cell_size_m;
                }

                ray.m_far_m *= half_plane_view.m_cell_size_m;
            }

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                *shape, world_rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));

            fixture.submit();

            const auto actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(actual.has_value() and actual->m_intervals.size() == world_rays.size());

            for (std::size_t index = 0u; index < world_rays.size(); ++index) {
                const auto expected = lighting::traceOpticalTerrainReference(*shape, world_rays[index]);
                PPR_TEST_ASSERT(expected.has_value() and not expected->m_hit_m);
                assertClose_(actual->m_intervals[index], expected->m_interval);
            }
        }

        const auto edited = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(edited.has_value());
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *edited, rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{true, false, false}));

        fixture.submit();
        const auto updated_completion = fixture.m_fence.pollCompleted();
        PPR_TEST_ASSERT(updated_completion.has_value());

        const auto updated = fixture.m_pass.readCompleted(*updated_completion);
        PPR_TEST_ASSERT(updated.has_value());

        for (const auto &interval: updated->m_intervals) {
            assertClose_(interval, {});
        }

        labels = {17u, 23u, 47u, 31u};
        const auto junction = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(junction.has_value());

        // The x1.375 first-hit points exercise exact reverse-Sterbenz
        // subtraction against the .5 cell anchor, without an input-grid subset.
        const std::array<lighting::OpticalRay, 5> sources{
            lighting::OpticalRay{{0.625, 0.625}, {1.0, 0.0}, 0.0, 0.0625}, {{1.375, 0.625}, {-1.0, 0.0}, 0.0, 0.0625},
            {{0.625, 1.375}, {1.0, 0.0}, 0.0, 0.0625}, {{1.375, 1.375}, {-1.0, 0.0}, 0.0, 0.0625},
            {{1.0, 1.0}, {1.0, 0.0}, 0.0, 0.0625}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *junction, sources, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, *updated_completion));
        fixture.submit();
        const auto junction_completion = fixture.m_fence.pollCompleted();
        PPR_TEST_ASSERT(junction_completion.has_value());

        const auto actual_sources = fixture.m_pass.readCompleted(*junction_completion);
        PPR_TEST_ASSERT(actual_sources.has_value());

        for (std::size_t index = 0u; index < sources.size(); ++index) {
            const auto expected = lighting::traceOpticalTerrainReference(*junction, sources[index]);
            PPR_TEST_ASSERT(expected.has_value());
            assertClose_(actual_sources->m_intervals[index], expected->m_interval);
        }

        assertClose_(actual_sources->m_intervals[1], {{1.0, 9.0, 2.0}, {}});

        u32 ordinary_cases{};
        constexpr std::array<u32, 4> storage{0u, 1u, 3u, 2u};
        const std::array<lighting::OpticalRay, 1> sample_ray{
            lighting::OpticalRay{{0.625, 0.625}, {1.0, 0.0}, 0.0, 0.0625}};
        for (const float roundness: {0.0f, 0.5f, 1.0f}) {
            materials[1].m_roundness = roundness;
            for (u32 mask = 0u; mask < 16u; ++mask) {
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    labels[storage[corner]] = (mask & (1u << corner)) != 0u ? 17u : 0u;
                }

                const auto shape = lighting::OpticalTerrainSnapshot::capture(input);
                PPR_TEST_ASSERT(shape.has_value());
                const auto expected = lighting::traceOpticalTerrainReference(*shape, sample_ray[0]);
                PPR_TEST_ASSERT(expected.has_value());

                PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                    *shape, sample_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
                fixture.submit();
                const auto shape_completion = fixture.m_fence.pollCompleted();
                PPR_TEST_ASSERT(shape_completion.has_value());

                const auto actual = fixture.m_pass.readCompleted(*shape_completion);
                PPR_TEST_ASSERT(actual.has_value());
                assertClose_(actual->m_intervals[0], expected->m_interval);
                ++ordinary_cases;
            }
        }

        const std::array<lighting::OpticalRay, 1> bridge_ray{
            lighting::OpticalRay{{1.0, 1.03125}, {1.0, 0.0}, 0.0, 0.0625}};
        for (const u16 diagonal: {u16{17u}, u16{23u}}) {
            labels = {17u, 0u, 0u, diagonal};
            const auto shape = lighting::OpticalTerrainSnapshot::capture(input);
            PPR_TEST_ASSERT(shape.has_value());
            const auto expected = lighting::traceOpticalTerrainReference(*shape, bridge_ray[0]);
            PPR_TEST_ASSERT(expected.has_value());

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                *shape, bridge_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
            fixture.submit();
            const auto shape_completion = fixture.m_fence.pollCompleted();
            PPR_TEST_ASSERT(shape_completion.has_value());

            const auto actual = fixture.m_pass.readCompleted(*shape_completion);
            PPR_TEST_ASSERT(actual.has_value());
            assertClose_(actual->m_intervals[0], expected->m_interval);
            ++ordinary_cases;
        }

        std::vector<u16> wall_labels(129u * 3u);
        for (u32 y = 0u; y < 3u; ++y) {
            wall_labels[std::size_t(y) * 129u + 127u] = 17u;
        }

        const std::array<lighting::OpticalRay, 2> seam_rays{
            lighting::OpticalRay{{126.25, 1.25}, {1.0, 0.0}, 0.0, 2.5}, {{128.75, 1.25}, {-1.0, 0.0}, 0.0, 2.5}};
        for (const bool doorway: {false, true}) {
            wall_labels[129u + 127u] = doorway ? 0u : 17u;
            const auto shape = lighting::OpticalTerrainSnapshot::capture(
                {wall_labels, materials, {}, 1.0, 129u, 3u, 0u, lighting::OpticalBoundary::vacuum});
            PPR_TEST_ASSERT(shape.has_value());

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                *shape, seam_rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
            fixture.submit();
            const auto shape_completion = fixture.m_fence.pollCompleted();
            PPR_TEST_ASSERT(shape_completion.has_value());

            const auto actual = fixture.m_pass.readCompleted(*shape_completion);
            PPR_TEST_ASSERT(actual.has_value());
            for (std::size_t index = 0u; index < seam_rays.size(); ++index) {
                const auto expected = lighting::traceOpticalTerrainReference(*shape, seam_rays[index]);
                PPR_TEST_ASSERT(expected.has_value());
                assertClose_(actual->m_intervals[index], expected->m_interval);
                ++ordinary_cases;
            }
        }

        // A non-singleton first-hit position stays clearly on one side of the
        // declared straight source partition; its source must match the oracle.
        lighting::OpticalCell first_source{}, second_source{};
        first_source.m_kind = second_source.m_kind = lighting::OpticalKind::opaque;
        first_source.m_surface_emission = {8.0, 2.0, 1.0};
        second_source.m_surface_emission = {1.0, 9.0, 2.0};
        const std::array<u16, 4> partition_labels{17u, 17u, 23u, 23u};
        std::array<lighting::OpticalTerrainMaterial, 3> partition_materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {first_source, 17u, 0.0f},
            {second_source, 23u, 0.0f}};
        lighting::OpticalTerrainView partition_input{
            partition_labels, partition_materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum};
        const auto partition = lighting::OpticalTerrainSnapshot::capture(partition_input);
        PPR_TEST_ASSERT(partition.has_value());
        const std::array<lighting::OpticalRay, 1> owned_side{
            lighting::OpticalRay{{1.375, 0.625}, {1.0, 0.0}, 0.03125, 0.0625}};
        const auto owned_reference = lighting::traceOpticalTerrainReference(*partition, owned_side[0]);
        PPR_TEST_ASSERT(owned_reference.has_value() and owned_reference->m_material == 17u);

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *partition, owned_side, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto owned = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(owned.has_value());
        assertClose_(owned->m_intervals[0], owned_reference->m_interval);
        ++ordinary_cases;

        // The actual earliest point is on the interface, but the complete GPU
        // event/coordinate enclosure spans both differing sources. It must not
        // silently choose a representative point or the later opaque-span source.
        const std::array<lighting::OpticalRay, 1> across_partition{
            lighting::OpticalRay{{1.375, 0.75}, {0.0, 1.0}, 0.25, 0.375}};
        const auto interface_reference = lighting::traceOpticalTerrainReference(*partition, across_partition[0]);
        PPR_TEST_ASSERT(interface_reference.has_value() and interface_reference->m_material == 17u);

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *partition, across_partition, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto uncertain_source = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not uncertain_source and uncertain_source.error() == std::errc::argument_out_of_domain);

        // This uncertainty comes only from subtracting the raw half-cell anchor:
        // exact xlocal is .5 + 2^-27, while FP32 rounds it to .5. Neither reverse
        // witness may certify that rounded value as an exact source coordinate.
        const std::array<u16, 4> anchor_labels{23u, 23u, 23u, 23u};
        auto anchor_materials = partition_materials;
        anchor_materials[1].m_roundness = 0.5f;
        anchor_materials[2].m_roundness = 0.5f;
        const auto anchor_shape = lighting::OpticalTerrainSnapshot::capture(
            {anchor_labels, anchor_materials, {}, 1.0, 2u, 2u, 17u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(anchor_shape.has_value());
        const std::array<lighting::OpticalRay, 1> anchor_ray{
            lighting::OpticalRay{{std::ldexp(1.0, -27), 0.75}, {0.0, 1.0}, 0.0, 0.0625}};
        const auto anchor_reference = lighting::traceOpticalTerrainReference(*anchor_shape, anchor_ray[0]);
        PPR_TEST_ASSERT(anchor_reference.has_value() and anchor_reference->m_material == 23u);
        assertClose_(anchor_reference->m_interval, {second_source.m_surface_emission, {}});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *anchor_shape, anchor_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto anchor_uncertainty = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not anchor_uncertainty and anchor_uncertainty.error() == std::errc::argument_out_of_domain);

        // Unresolved label identity is harmless when all possible winners share
        // the same source. Keep both IDs and the identical interface query.
        partition_materials[2].m_optics = first_source;
        const auto common_source = lighting::OpticalTerrainSnapshot::capture(partition_input);
        PPR_TEST_ASSERT(common_source.has_value());
        const auto common_reference = lighting::traceOpticalTerrainReference(*common_source, across_partition[0]);
        PPR_TEST_ASSERT(common_reference.has_value());

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *common_source, across_partition, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto common = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(common.has_value());
        assertClose_(common->m_intervals[0], common_reference->m_interval);
        ++ordinary_cases;

        // Captured C7 record1457 translated into a compact one-corner field.
        // Its first occupied interval is certified before the later exit box
        // meets the cell far plane. Preserve the actual uploaded direction bits.
        lighting::OpticalCell causal_surface{};
        causal_surface.m_kind = lighting::OpticalKind::opaque;
        causal_surface.m_surface_emission = {8.0, 2.0, 1.0};
        const std::array<u16, 4> causal_labels{17u, 0u, 0u, 0u};
        const std::array<lighting::OpticalTerrainMaterial, 2> causal_materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {causal_surface, 17u, 0.9f}};
        const auto causal_shape = lighting::OpticalTerrainSnapshot::capture(
            {causal_labels, causal_materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(causal_shape.has_value());

        const std::array<double, 2> causal_direction{
            double(std::bit_cast<float>(0xbf62df7bu)), double(std::bit_cast<float>(0x3eed3063u))};
        const double causal_raw_length = std::hypot(causal_direction[0], causal_direction[1]);
        const std::array<lighting::OpticalRay, 1> causal_ray{
            lighting::OpticalRay{{66.5, -33.5}, causal_direction, 64.0 * causal_raw_length, 514.0 * causal_raw_length}};
        const auto causal_normalized = lighting::normalizeOpticalRay(causal_ray[0]);
        PPR_TEST_ASSERT(causal_normalized.has_value());
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(causal_normalized->m_direction[0])) == 0xbf62df7bu);
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(causal_normalized->m_direction[1])) == 0x3eed3063u);
        PPR_TEST_ASSERT(
            static_cast<float>(causal_normalized->m_near_m) == 64.0f and
            static_cast<float>(causal_normalized->m_far_m) == 514.0f);

        const auto causal_reference = lighting::traceOpticalTerrainReference(*causal_shape, causal_ray[0]);
        PPR_TEST_ASSERT(
            causal_reference.has_value() and causal_reference->m_hit_m and causal_reference->m_material == 17u);
        assertClose_(causal_reference->m_interval, {causal_surface.m_surface_emission, {}});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *causal_shape, causal_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto causal_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(causal_actual.has_value() and causal_actual->m_intervals.size() == 1u);
        assertClose_(causal_actual->m_intervals[0], causal_reference->m_interval);
        ++ordinary_cases;

        // Preserve the original shortened query. Cell-local conditioning now
        // separates its endpoint from the first root, certifying the true miss.
        auto causal_short_ray = causal_ray;
        causal_short_ray[0].m_far_m = 73.99 * causal_raw_length;
        const auto causal_short_reference = lighting::traceOpticalTerrainReference(*causal_shape, causal_short_ray[0]);
        PPR_TEST_ASSERT(causal_short_reference.has_value() and not causal_short_reference->m_hit_m);
        assertClose_(causal_short_reference->m_interval, {});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *causal_shape, causal_short_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto causal_short_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(causal_short_actual.has_value() and causal_short_actual->m_intervals.size() == 1u);
        assertClose_(causal_short_actual->m_intervals[0], causal_short_reference->m_interval);
        ++ordinary_cases;

        // A distinct endpoint stays just before the exact entry but its metric
        // enclosure overlaps the rebased root. It must remain a visible error.
        auto causal_near_root_ray = causal_ray;
        causal_near_root_ray[0].m_far_m = 73.9909 * causal_raw_length;
        const auto causal_near_root_reference = lighting::traceOpticalTerrainReference(
            *causal_shape, causal_near_root_ray[0]);
        PPR_TEST_ASSERT(causal_near_root_reference.has_value() and not causal_near_root_reference->m_hit_m);
        assertClose_(causal_near_root_reference->m_interval, {});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *causal_shape, causal_near_root_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto causal_unresolved = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not causal_unresolved and causal_unresolved.error() == std::errc::argument_out_of_domain);

        // Captured C7 record3762 is a strict rounded-corner miss. Retain its
        // physical origin and translate only the immutable compact grid minimum,
        // giving the same cell-relative polynomial at world cell(62,0).
        const std::array<u16, 4> distant_miss_labels{0u, 0u, 17u, 0u};
        const std::array<lighting::OpticalTerrainMaterial, 2> distant_miss_materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {causal_surface, 17u, 1.0f}};
        const auto distant_miss_shape = lighting::OpticalTerrainSnapshot::capture(
            {distant_miss_labels, distant_miss_materials, {62.0, 0.0}, 1.0, 2u, 2u, 0u,
                lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(distant_miss_shape.has_value());

        const std::array<double, 2> distant_miss_direction{
            double(std::bit_cast<float>(0xbeea6681u)), double(std::bit_cast<float>(0xbf6398acu))};
        const double distant_miss_length = std::hypot(distant_miss_direction[0], distant_miss_direction[1]);
        const std::array<lighting::OpticalRay, 1> distant_miss_ray{lighting::OpticalRay{
            {128.5, 128.5}, distant_miss_direction, 64.0 * distant_miss_length, 514.0 * distant_miss_length}};
        const auto distant_miss_normalized = lighting::normalizeOpticalRay(distant_miss_ray[0]);
        PPR_TEST_ASSERT(distant_miss_normalized.has_value());
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(distant_miss_normalized->m_direction[0])) == 0xbeea6681u);
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(distant_miss_normalized->m_direction[1])) == 0xbf6398acu);

        const auto distant_miss_reference = lighting::traceOpticalTerrainReference(
            *distant_miss_shape, distant_miss_ray[0]);
        PPR_TEST_ASSERT(distant_miss_reference.has_value() and not distant_miss_reference->m_hit_m);
        assertClose_(distant_miss_reference->m_interval, {});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *distant_miss_shape, distant_miss_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto distant_miss_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(distant_miss_actual.has_value() and distant_miss_actual->m_intervals.size() == 1u);
        assertClose_(distant_miss_actual->m_intervals[0], distant_miss_reference->m_interval);
        ++ordinary_cases;

        // Captured C7 record1455 crosses y=35.5 just before x=63.5. Their time
        // boxes overlap, but the original-line cross products order them. The
        // vacuum intermediate cell must be visited rather than treated as a tie.
        std::array<u16, 4> dda_labels{};
        const lighting::OpticalTerrainView dda_input{
            dda_labels, distant_miss_materials, {62.0, 34.0}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum};
        const auto dda_vacuum = lighting::OpticalTerrainSnapshot::capture(dda_input);
        PPR_TEST_ASSERT(dda_vacuum.has_value());

        const std::array<double, 2> dda_direction{
            double(std::bit_cast<float>(0xbf61668au)), double(std::bit_cast<float>(0x3ef2bd43u))};
        const double dda_length = std::hypot(dda_direction[0], dda_direction[1]);
        const std::array<lighting::OpticalRay, 1> dda_ray{
            lighting::OpticalRay{{128.5, 0.5}, dda_direction, 64.0 * dda_length, 514.0 * dda_length}};
        const auto dda_normalized = lighting::normalizeOpticalRay(dda_ray[0]);
        PPR_TEST_ASSERT(dda_normalized.has_value());
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(dda_normalized->m_direction[0])) == 0xbf61668au);
        PPR_TEST_ASSERT(std::bit_cast<u32>(static_cast<float>(dda_normalized->m_direction[1])) == 0x3ef2bd43u);

        const auto dda_vacuum_reference = lighting::traceOpticalTerrainReference(*dda_vacuum, dda_ray[0]);
        PPR_TEST_ASSERT(dda_vacuum_reference.has_value() and not dda_vacuum_reference->m_hit_m);
        PPR_TEST_ASSERT(dda_vacuum_reference->m_dual_cells == 3u);
        assertClose_(dda_vacuum_reference->m_interval, {});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *dda_vacuum, dda_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto dda_vacuum_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(dda_vacuum_actual.has_value() and dda_vacuum_actual->m_intervals.size() == 1u);
        PPR_TEST_ASSERT(dda_vacuum_actual->m_segments.size() == 1u and dda_vacuum_actual->m_segments[0] == 3u);
        assertClose_(dda_vacuum_actual->m_intervals[0], dda_vacuum_reference->m_interval);
        ++ordinary_cases;

        // Filling the shared corner also fills a neighbourhood in the preceding
        // cell. The independent first hit legitimately terminates there; this
        // source check does not claim execution of an occupied thin-cell branch.
        dda_labels[3] = 17u;
        const auto dda_occupied = lighting::OpticalTerrainSnapshot::capture(dda_input);
        PPR_TEST_ASSERT(dda_occupied.has_value());
        const auto dda_occupied_reference = lighting::traceOpticalTerrainReference(*dda_occupied, dda_ray[0]);
        PPR_TEST_ASSERT(
            dda_occupied_reference.has_value() and
            dda_occupied_reference->m_hit_m and
            dda_occupied_reference->m_material == 17u);
        PPR_TEST_ASSERT(*dda_occupied_reference->m_hit_m < (35.0 / dda_direction[1]) * dda_length);
        assertClose_(dda_occupied_reference->m_interval, {causal_surface.m_surface_emission, {}});

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *dda_occupied, dda_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, fixture.m_fence.completed()));
        fixture.submit();
        const auto dda_occupied_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(dda_occupied_actual.has_value() and dda_occupied_actual->m_intervals.size() == 1u);
        PPR_TEST_ASSERT(dda_occupied_actual->m_segments.size() == 1u and dda_occupied_actual->m_segments[0] == 1u);
        assertClose_(dda_occupied_actual->m_intervals[0], dda_occupied_reference->m_interval);
        ++ordinary_cases;

        std::println(
            "terrain ordinary cases={} initial dual-cell spans={}",
            ordinary_cases + rays.size() * 2u + sources.size() + half_plane_rays.size() * 2u,
            std::accumulate(first->m_segments.begin(), first->m_segments.end(), u64{}));
    };

    PPR_UNIT_TEST (rounded_terrain_near_tangent_and_collinear_queries_report_ambiguity) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        if (not terrainBackend_(fixture)) {
            return;
        }

        const std::array<u16, 4> labels{17u, 0u, 0u, 0u};
        lighting::OpticalCell surface{};
        surface.m_kind = lighting::OpticalKind::opaque;
        surface.m_surface_emission = {8.0, 2.0, 1.0};
        std::array<lighting::OpticalTerrainMaterial, 2> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface, 17u, 1.0f}};
        lighting::OpticalTerrainView input{labels, materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum};
        const auto snapshot = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(snapshot.has_value());

        // All coordinates are actual uploaded FP32 values. The corresponding
        // unit-quad example is (.375,nextDown(.375)); this world-anchored variant
        // retains the perturbation after the required half-cell origin shift.
        const std::array<double, 3> ys{
            double(std::nextafter(0.875f, 0.0f)), 0.875, double(std::nextafter(0.875f, 1.0f))};
        u32 ambiguity_count{};
        for (const double y: ys) {
            const std::array<lighting::OpticalRay, 1> rays{lighting::OpticalRay{{0.875, y}, {-1.0, 1.0}, 0.0, 0.125}};

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
                *snapshot, rays, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
            fixture.submit();
            const auto completed = fixture.m_fence.pollCompleted();
            PPR_TEST_ASSERT(completed.has_value());

            const auto actual = fixture.m_pass.readCompleted(*completed);
            PPR_TEST_ASSERT(not actual and actual.error() == std::errc::argument_out_of_domain);
            ++ambiguity_count;
        }

        materials[1].m_roundness = 0.0f;
        const auto straight = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(straight.has_value());
        const std::array<lighting::OpticalRay, 1> collinear{
            lighting::OpticalRay{{0.75, 0.75}, {-1.0, 1.0}, 0.0, 0.125}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *straight, collinear, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone));
        fixture.submit();
        const auto completed = fixture.m_fence.pollCompleted();
        PPR_TEST_ASSERT(completed.has_value());

        const auto actual = fixture.m_pass.readCompleted(*completed);
        PPR_TEST_ASSERT(not actual and actual.error() == std::errc::argument_out_of_domain);

        // A negative minimum-subnormal direction at the left world edge must
        // not become an axis-parallel opaque hit after GPU FTZ. Host terrain
        // admission rejects the exact uploaded representation before allocation.
        const std::array<u16, 1> edge_labels{17u};
        const auto edge = lighting::OpticalTerrainSnapshot::capture(
            {edge_labels, materials, {}, 1.0, 1u, 1u, 17u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(edge.has_value());
        const std::array<lighting::OpticalRay, 1> subnormal_ray{
            lighting::OpticalRay{{0.0, 0.25}, {-double(std::numeric_limits<float>::denorm_min()), 1.0}, 0.0, 0.125}};
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainTrace(
                *edge, subnormal_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u, kFenceValueNone) ==
            std::errc::argument_out_of_domain);

        // Canonical coefficient encoding does not admit subnormal geometry
        // arithmetic. Consume that authored coefficient in the actual tracer
        // and require a visible domain failure, never a flushed linear contour.
        materials[1].m_roundness = std::bit_cast<float>(0x00400000u);
        const auto tiny_roundness = lighting::OpticalTerrainSnapshot::capture(input);
        PPR_TEST_ASSERT(tiny_roundness.has_value());
        const std::array<lighting::OpticalRay, 1> tiny_curve_ray{
            lighting::OpticalRay{{0.875, 0.875}, {-1.0, 0.0}, 0.0, 0.125}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *tiny_roundness, tiny_curve_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto unsupported_curve = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not unsupported_curve and unsupported_curve.error() == std::errc::argument_out_of_domain);

        // Full occupancy bypasses the outer contour field. Differing sources
        // still consume the canonical shared coefficient during label scoring:
        // normal and zero authored endpoints produce a subnormal half-minimum mean.
        lighting::OpticalCell second_surface = surface;
        second_surface.m_surface_emission = {1.0, 3.0, 7.0};
        const std::array<u16, 4> shared_labels{17u, 18u, 17u, 18u};
        const std::array<lighting::OpticalTerrainMaterial, 3> shared_materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface, 17u, std::numeric_limits<float>::min()},
            {second_surface, 18u, 0.0f}};
        const auto tiny_shared_roundness = lighting::OpticalTerrainSnapshot::capture(
            {shared_labels, shared_materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(tiny_shared_roundness.has_value());

        const std::array<lighting::OpticalRay, 1> inside_ray{
            lighting::OpticalRay{{0.75, 0.75}, {1.0, 0.0}, 0.0, 0.125}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *tiny_shared_roundness, inside_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto unsupported_shared = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not unsupported_shared and unsupported_shared.error() == std::errc::argument_out_of_domain);

        // Three labels disable shared scoring. The first unique corner instead
        // consumes its authored subnormal directly at the same singleton hit.
        lighting::OpticalCell third_surface = surface;
        third_surface.m_surface_emission = {2.0, 5.0, 1.0};
        const std::array<u16, 4> separate_labels{17u, 18u, 19u, 18u};
        const std::array<lighting::OpticalTerrainMaterial, 4> separate_materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface, 17u, std::bit_cast<float>(0x00400000u)},
            {second_surface, 18u, 0.0f}, {third_surface, 19u, 0.5f}};
        const auto tiny_separate_roundness = lighting::OpticalTerrainSnapshot::capture(
            {separate_labels, separate_materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(tiny_separate_roundness.has_value());

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainTrace(
            *tiny_separate_roundness, inside_ray, 64u * 1024u * 1024u, 0u, 32u * 1024u * 1024u,
            fixture.m_fence.completed()));
        fixture.submit();
        const auto unsupported_separate = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not unsupported_separate and unsupported_separate.error() == std::errc::argument_out_of_domain);

        std::println("terrain explicit ambiguity fixtures={} collinear=1", ambiguity_count);
    };

    PPR_UNIT_TEST (rounded_terrain_fp32_cascades_reuse_the_declared_rc_schedule) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        if (not terrainBackend_(fixture)) {
            return;
        }

        const std::array<u16, 4> labels{17u, 0u, 0u, 0u};
        lighting::OpticalCell surface{};
        surface.m_kind = lighting::OpticalKind::opaque;
        surface.m_surface_emission = {8.0, 2.0, 1.0};
        const std::array<lighting::OpticalTerrainMaterial, 2> materials{
            lighting::OpticalTerrainMaterial{vacuum_(), 0u, 0.0f}, {surface, 17u, 0.5f}};
        const auto snapshot = lighting::OpticalTerrainSnapshot::capture(
            {labels, materials, {}, 1.0, 2u, 2u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(snapshot.has_value());
        const std::array<lighting::OpticalCell, 4> cells{surface, vacuum_(), vacuum_(), vacuum_()};
        const lighting::OpticalGridView grid{cells, {}, 1.0, 2u, 2u, lighting::OpticalBoundary::vacuum};
        const auto request = cascadeRequest_(2u, 2u);
        const auto expected = solveCpuCascades_(grid, request, false, false, &*snapshot);

        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, false, false}));
        auto rejected = request;
        rejected.m_budget_bytes = 1u;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTerrainCascades(*snapshot, rejected, kFenceValueNone) == std::errc::no_buffer_space);
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, false, false}));

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainCascades(*snapshot, request, kFenceValueNone));
        PPR_TEST_ASSERT((fixture.m_pass.terrainPipelineReadinessForQualification() == std::array{false, true, true}));

        fixture.submit(true);
        const auto completed = fixture.m_fence.pollCompleted();
        PPR_TEST_ASSERT(completed.has_value());

        const auto actual = fixture.m_pass.readCompleted(*completed);
        fixture.observeReadFailure(actual ? std::error_code{} : actual.error());
        PPR_TEST_ASSERT(actual.has_value());
        PPR_TEST_ASSERT(actual->m_intervals.size() == expected.m_intervals[0].size());

        for (std::size_t index = 0u; index < actual->m_intervals.size(); ++index) {
            assertClose_(actual->m_intervals[index], expected.m_intervals[0][index]);
        }

        PPR_TEST_ASSERT(actual->m_c1_probe0_bin0.has_value());
        assertClose_(*actual->m_c1_probe0_bin0, expected.m_intervals[1][0]);

        // The same local raw-direction schedule must survive world translation
        // and a different metre metric without changing any endpoint expression.
        for (const double metric: {1.0, 0.25}) {
            auto variant_input = snapshot->view();
            variant_input.m_min_m = {1000000.0, -1000000.0};
            variant_input.m_cell_size_m = metric;
            const auto variant = lighting::OpticalTerrainSnapshot::capture(variant_input);
            PPR_TEST_ASSERT(variant.has_value());

            auto variant_grid = grid;
            variant_grid.m_min_m = variant_input.m_min_m;
            variant_grid.m_cell_size_m = metric;
            const auto variant_expected = solveCpuCascades_(variant_grid, request, false, false, &*variant);
            PPR_TEST_ASSERT(not fixture.m_pass.prepareTerrainCascades(*variant, request, fixture.m_fence.completed()));
            fixture.submit(true);
            const auto observed = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            fixture.observeReadFailure(observed ? std::error_code{} : observed.error());
            PPR_TEST_ASSERT(
                observed.has_value() and observed->m_intervals.size() == variant_expected.m_intervals[0].size());

            for (std::size_t record = 0u; record < observed->m_intervals.size(); ++record) {
                assertClose_(observed->m_intervals[record], variant_expected.m_intervals[0][record]);
                assertClose_(observed->m_intervals[record], actual->m_intervals[record]);
            }

            PPR_TEST_ASSERT(observed->m_c1_probe0_bin0.has_value());
            assertClose_(*observed->m_c1_probe0_bin0, variant_expected.m_intervals[1][0]);
        }

        // The top high guard is beyond the 4x1 world. This complete fixture
        // keeps its far emitter under the boundary-complete lambda schedule.
        const std::array<u16, 4> guard_labels{17u, 0u, 0u, 0u};
        const auto guard_snapshot = lighting::OpticalTerrainSnapshot::capture(
            {guard_labels, materials, {}, 1.0, 4u, 1u, 0u, lighting::OpticalBoundary::vacuum});
        PPR_TEST_ASSERT(guard_snapshot.has_value());
        const std::array<lighting::OpticalCell, 4> guard_cells{surface, vacuum_(), vacuum_(), vacuum_()};
        const lighting::OpticalGridView guard_grid{guard_cells, {}, 1.0, 4u, 1u, lighting::OpticalBoundary::vacuum};
        const auto guard_request = cascadeRequest_(4u, 1u);
        const auto guard_expected = solveCpuCascades_(guard_grid, guard_request, false, false, &*guard_snapshot);
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTerrainCascades(*guard_snapshot, guard_request, fixture.m_fence.completed()));
        fixture.submit(true);
        const auto guard_actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        fixture.observeReadFailure(guard_actual ? std::error_code{} : guard_actual.error());
        PPR_TEST_ASSERT(
            guard_actual.has_value() and guard_actual->m_intervals.size() == guard_expected.m_intervals[0].size());

        for (std::size_t record = 0u; record < guard_actual->m_intervals.size(); ++record) {
            assertClose_(guard_actual->m_intervals[record], guard_expected.m_intervals[0][record]);
        }

        PPR_TEST_ASSERT(guard_actual->m_c1_probe0_bin0.has_value());
        assertClose_(*guard_actual->m_c1_probe0_bin0, guard_expected.m_intervals[1][0]);
    };

    // ------------------------------------------------------------------
    // independent RGB binary16 storage qualification
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (packed_half_rounding_endpoints_admission_and_visible_overflow) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        constexpr auto storage = lighting::GpuIntervalStorage::rgb16f;

        const std::array<lighting::OpticalCell, 1> vacuum{vacuum_()};
        const lighting::OpticalGridView tiny{vacuum, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> one_ray{lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 1.0}};
        // 40 source +24 ray +16 stored interval +4 segment +20 readback
        // +4 hierarchy flags +16 metadata. No hidden 32-byte GPU/readback row.
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(tiny, one_ray, 123u, 0u, 65536u, fixture.m_fence.completed(), storage) ==
            std::errc::no_buffer_space);
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(tiny, one_ray, 124u, 0u, 65536u, fixture.m_fence.completed(), storage));
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(tiny, one_ray, 247u, 0u, 65536u, fixture.m_fence.completed(), storage) ==
            std::errc::no_buffer_space);
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(tiny, one_ray, 248u, 0u, 65536u, fixture.m_fence.completed(), storage));
        fixture.submit();
        const auto identity = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(identity.has_value());
        PPR_TEST_ASSERT(identity->m_intervals[0].m_radiance == lighting::OpticalRgb{});
        PPR_TEST_ASSERT(identity->m_intervals[0].m_transmittance == (lighting::OpticalRgb{1.0, 1.0, 1.0}));

        const std::array<double, 13> values{
            0.0, std::ldexp(1.0, -25), 3.0 * std::ldexp(1.0, -25), std::ldexp(1.0, -24),
            std::ldexp(1.0, -14) - std::ldexp(1.0, -25), std::ldexp(1.0, -14), 1.0 + std::ldexp(1.0, -11),
            1.0 + 3.0 * std::ldexp(1.0, -11), 0.0001, 17.123, 2.0 - std::ldexp(1.0, -11), 32768.0, 65504.0};
        std::vector<lighting::OpticalCell> cells{};
        std::vector<lighting::OpticalRay> rays{};
        for (std::size_t index = 0u; index < values.size(); ++index) {
            const double value = double(float(values[index]));
            cells.push_back(
                lighting::OpticalCell{
                    {}, {}, {value, value * 0.5, std::ldexp(1.0, -24)}, lighting::OpticalKind::opaque});
            rays.push_back({{double(index) + 0.5, 0.5}, {1.0, 0.0}, 0.0, 0.25});
        }

        const lighting::OpticalGridView grid{
            cells, {}, 1.0, static_cast<u32>(cells.size()), 1u, lighting::OpticalBoundary::vacuum};
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareTrace(grid, rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        fixture.submit();
        const auto fp32 = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(fp32.has_value());
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            grid, rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage));
        fixture.submit();
        const auto packed = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(packed.has_value());
        bool conversion_rte = true;
        bool quantum_bound = true;
        bool subnormal_bound = true;
        bool endpoints_and_counts = true;
        for (std::size_t index = 0u; index < values.size(); ++index) {
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                const double source = fp32->m_intervals[index].m_radiance[channel];
                const double stored = packed->m_intervals[index].m_radiance[channel];
                const auto &words = packed->m_packed_words[index];
                const u32 actual_bits = channel == 0u   ? words[0] & 65535u
                                        : channel == 1u ? words[0] >> 16u
                                                        : words[1] & 65535u;
                if (stored != roundHalf_(source) or actual_bits != expectedHalfBits_(source)) {
                    std::println(
                        "PACK DIAG DIRECT record={} channel={} source={} source_fp32={:08x} actual={} actual_half={:04x} expected={} expected_half={:04x} raw={:08x},{:08x},{:08x},{:08x}",
                        index, channel, source, std::bit_cast<u32>(static_cast<float>(source)), stored, actual_bits,
                        roundHalf_(source), expectedHalfBits_(source), words[0], words[1], words[2], words[3]);
                }

                // Verify both stored bits and decoded values against the independent
                // arithmetic reference, without depending on native conversion mode.
                conversion_rte = conversion_rte and
                                 stored == roundHalf_(source) and
                                 actual_bits == expectedHalfBits_(source);
                quantum_bound = quantum_bound and std::abs(stored - source) <= halfQuantum_(source);
                if (source < std::ldexp(1.0, -14)) {
                    subnormal_bound = subnormal_bound and std::abs(stored - source) <= std::ldexp(1.0, -25);
                }

                endpoints_and_counts = endpoints_and_counts and
                                       packed->m_intervals[index].m_transmittance[channel] == 0.0;
            }

            endpoints_and_counts = endpoints_and_counts and packed->m_segments[index] == fp32->m_segments[index];
        }

        PPR_TEST_ASSERT(conversion_rte and quantum_bound and subnormal_bound and endpoints_and_counts);

        PPR_TEST_ASSERT(packed->m_intervals.back().m_radiance[0] == 65504.0);
        PPR_TEST_ASSERT(packed->m_intervals.back().m_radiance[2] == std::ldexp(1.0, -24));
        std::println(
            "RGB16F deterministic RTE bit qualification: ties, carry, independent weak channels and subnormal error <=2^-25");

        std::array<lighting::OpticalCell, 1> invalid{
            lighting::OpticalCell{{}, {}, {65505.0, 1.0, 0.0}, lighting::OpticalKind::opaque}};
        const lighting::OpticalGridView invalid_grid{invalid, {}, 1.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            invalid_grid, one_ray, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage));
        fixture.submit();
        const auto overflow = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not overflow and overflow.error() == std::errc::result_out_of_range);

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            invalid_grid, one_ray, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        fixture.submit();
        const auto explicit_fp32 = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(explicit_fp32.has_value() and explicit_fp32->m_intervals[0].m_radiance[0] == 65505.0);

        invalid[0].m_surface_emission[0] = std::numeric_limits<double>::infinity();
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(
                invalid_grid, one_ray, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage) ==
            std::errc::invalid_argument);

        invalid[0].m_surface_emission[0] = -1.0;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareTrace(
                invalid_grid, one_ray, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage) ==
            std::errc::invalid_argument);

        // Cascade storage failures remain visible through the same completion
        // API; upper interval status is retained separately from local span counts.
        invalid[0].m_surface_emission[0] = 70000.0;
        auto request = cascadeRequest_(1u, 1u);
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareCascades(invalid_grid, request, fixture.m_fence.completed(), storage) ==
            std::errc::function_not_supported);

        request.m_interval_bytes = 16u;
        PPR_TEST_ASSERT(
            not fixture.m_pass.prepareCascades(invalid_grid, request, fixture.m_fence.completed(), storage));
        fixture.submit(true);
        const auto cascade_failure = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not cascade_failure and cascade_failure.error() == std::errc::result_out_of_range);

        const std::array<lighting::OpticalCell, 1> huge_source{
            lighting::OpticalCell{{}, {1e38, 0.0, 0.0}, {}, lighting::OpticalKind::absorbing}};
        const lighting::OpticalGridView huge_grid{huge_source, {}, 1024.0, 1u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 1> huge_ray{lighting::OpticalRay{{0.0, 512.0}, {1.0, 0.0}, 0.0, 1024.0}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            huge_grid, huge_ray, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage));
        fixture.submit();
        const auto nonfinite = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(not nonfinite and nonfinite.error() == std::errc::result_out_of_range);
    };

    PPR_UNIT_TEST (packed_cascades_match_per_level_half_reference_and_report_storage_error) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();
        constexpr auto storage = lighting::GpuIntervalStorage::rgb16f;

        for (const auto dimensions:
            std::array<std::array<u32, 2>, 3>{std::array<u32, 2>{8u, 8u}, {64u, 1u}, {1u, 17u}}) {
            const u32 width = dimensions[0];
            const u32 height = dimensions[1];
            std::vector<lighting::OpticalCell> cells(std::size_t(width) * height);
            for (auto &cell: cells) {
                cell = lighting::OpticalCell{
                    {std::ldexp(1.0, -10), std::ldexp(1.0, -12), 0.25},
                    {std::ldexp(1.0, -16), std::ldexp(1.0, -20), std::ldexp(1.0, -24)}, {},
                    lighting::OpticalKind::absorbing};
            }

            cells.back() = lighting::OpticalCell{
                {}, {}, {32768.0, 0.125, std::ldexp(1.0, -14)}, lighting::OpticalKind::opaque};
            const lighting::OpticalGridView grid{cells, {}, 1.0, width, height, lighting::OpticalBoundary::vacuum};
            auto request = cascadeRequest_(width, height);
            PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()));
            fixture.submit(true);
            const auto fp32 = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(fp32.has_value());

            request.m_interval_bytes = 16u;
            const auto expected = solveCpuCascades_(grid, request, false, true);
            PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed(), storage));
            fixture.submit(true);
            const auto packed = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(packed.has_value() and packed->m_intervals.size() == expected.m_intervals[0].size());
            double maximum_l_error{};
            double maximum_t_error{};
            for (std::size_t index = 0u; index < packed->m_intervals.size(); ++index) {
                assertStoredClose_(
                    packed->m_intervals[index], expected.m_intervals[0][index], &packed->m_packed_words[index], index);
                PPR_TEST_ASSERT(packed->m_segments[index] == fp32->m_segments[index]);
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    maximum_l_error = std::max(
                        maximum_l_error, std::abs(
                                             packed->m_intervals[index].m_radiance[channel] -
                                             fp32->m_intervals[index].m_radiance[channel]));
                    maximum_t_error = std::max(
                        maximum_t_error, std::abs(
                                             packed->m_intervals[index].m_transmittance[channel] -
                                             fp32->m_intervals[index].m_transmittance[channel]));
                }
            }

            PPR_TEST_ASSERT(packed->m_c1_probe0_bin0.has_value());
            assertStoredClose_(
                *packed->m_c1_probe0_bin0, expected.m_intervals[1][0], &packed->m_packed_words.back(),
                packed->m_intervals.size());
            std::println(
                "RGB16F storage diagnostic {}x{} levels={}: max_abs_L={} max_abs_T={}", width, height,
                request.m_cascade_count, maximum_l_error, maximum_t_error);
        }

        // Many weak medium segments test storage after FP32 integration.
        // The unchanged high-depth queries now expose previously silent FTZ
        // as domain errors in both formats, not production-media admission.
        for (const double extinction: std::array{std::ldexp(1.0, -12), 0.25, 32.0}) {
            std::vector<lighting::OpticalCell> cells(
                513u, lighting::OpticalCell{
                          {extinction, extinction * 0.5, extinction * 0.25}, {}, {}, lighting::OpticalKind::absorbing});
            cells.back() = lighting::OpticalCell{
                {}, {}, {32768.0, 0.125, std::ldexp(1.0, -14)}, lighting::OpticalKind::opaque};
            const lighting::OpticalGridView grid{cells, {}, 1.0, 513u, 1u, lighting::OpticalBoundary::vacuum};
            const std::array<lighting::OpticalRay, 2> rays{
                lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 512.0},
                lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 513.0}};
            PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
                grid, rays, 4u * 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
            fixture.submit();
            const auto fp32 = fixture.m_pass.readCompleted(fixture.m_fence.completed());

            PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
                grid, rays, 4u * 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage));
            fixture.submit();
            const auto packed = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            if (extinction != std::ldexp(1.0, -12)) {
                PPR_TEST_ASSERT(not fp32 and fp32.error() == std::errc::result_out_of_range);
                PPR_TEST_ASSERT(not packed and packed.error() == std::errc::result_out_of_range);
                std::println("RGB16F qualification domain rejects unchanged 512-cell extinction={}", extinction);
                continue;
            }

            PPR_TEST_ASSERT(fp32.has_value());
            PPR_TEST_ASSERT(packed.has_value() and packed->m_segments[0] == 512u);
            for (std::size_t index = 0u; index < rays.size(); ++index) {
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(
                        packed->m_intervals[index].m_radiance[channel] ==
                        roundHalf_(fp32->m_intervals[index].m_radiance[channel]));
                    PPR_TEST_ASSERT(
                        packed->m_intervals[index].m_transmittance[channel] ==
                        roundHalf_(fp32->m_intervals[index].m_transmittance[channel]));
                }
            }

            const double t_error = std::abs(
                packed->m_intervals[0].m_transmittance[0] - fp32->m_intervals[0].m_transmittance[0]);
            const double after_t_error = t_error * 32768.0;
            const double stored_source_error = std::abs(
                packed->m_intervals[1].m_radiance[0] - fp32->m_intervals[1].m_radiance[0]);
            std::println(
                "RGB16F storage diagnostic extinction={}: T_error={} bright_after_T_error={} stored_far_L_error={}",
                extinction, t_error, after_t_error, stored_source_error);
        }

        // A short high-absorption path retains successful near-opaque storage
        // coverage: depth32 has normal FP32 T, which may quantize to half zero.
        const std::array<lighting::OpticalCell, 2> short_medium{
            lighting::OpticalCell{{32.0, 16.0, 8.0}, {}, {}, lighting::OpticalKind::absorbing},
            lighting::OpticalCell{{}, {}, {32768.0, 0.125, std::ldexp(1.0, -14)}, lighting::OpticalKind::opaque}};
        const lighting::OpticalGridView short_grid{short_medium, {}, 1.0, 2u, 1u, lighting::OpticalBoundary::vacuum};
        const std::array<lighting::OpticalRay, 2> short_rays{
            lighting::OpticalRay{{0.0, 0.5}, {1.0, 0.0}, 0.0, 1.0}, {{0.0, 0.5}, {1.0, 0.0}, 0.0, 2.0}};
        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            short_grid, short_rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed()));
        fixture.submit();
        const auto short_fp32 = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(short_fp32.has_value());
        assertClose_(short_fp32->m_intervals[0], {{}, {std::exp(-32.0), std::exp(-16.0), std::exp(-8.0)}});

        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            const double expected_t = std::exp(-short_medium[0].m_extinction[channel]);
            PPR_TEST_ASSERT(
                std::abs(short_fp32->m_intervals[0].m_transmittance[channel] - expected_t) <= expected_t * 1e-4);
        }

        PPR_TEST_ASSERT(not fixture.m_pass.prepareTrace(
            short_grid, short_rays, 1024u * 1024u, 65536u, 65536u, fixture.m_fence.completed(), storage));
        fixture.submit();
        const auto short_packed = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(short_packed.has_value());
        for (std::size_t index = 0u; index < short_rays.size(); ++index) {
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                PPR_TEST_ASSERT(
                    short_packed->m_intervals[index].m_radiance[channel] ==
                    roundHalf_(short_fp32->m_intervals[index].m_radiance[channel]));
                PPR_TEST_ASSERT(
                    short_packed->m_intervals[index].m_transmittance[channel] ==
                    roundHalf_(short_fp32->m_intervals[index].m_transmittance[channel]));
            }
        }
    };

    // ------------------------------------------------------------------
    // numerical RC equivalence and separate physical diagnostics
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (fp32_cascades_match_double_schedule_and_degenerate_axis_support) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        for (const auto dimensions:
            std::array<std::array<u32, 2>, 5>{std::array<u32, 2>{1u, 1u}, {1u, 7u}, {7u, 1u}, {9u, 7u}, {8u, 8u}}) {
            const u32 width = dimensions[0];
            const u32 height = dimensions[1];
            std::vector<lighting::OpticalCell> cells(std::size_t(width) * height);
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    cells[std::size_t(y) * width + x] = lighting::OpticalCell{
                        {0.25, 0.5, 0.125}, {0.125 * double(x + 1u), 0.0625 * double(y + 1u), 0.125}, {},
                        lighting::OpticalKind::absorbing};
                }
            }

            const lighting::OpticalGridView grid{
                cells, {0.0, 0.0}, 0.25, width, height, lighting::OpticalBoundary::vacuum};
            const auto request = cascadeRequest_(width, height);
            const auto expected = solveCpuCascades_(grid, request);
            PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()));
            fixture.submit(true);
            const auto actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
            PPR_TEST_ASSERT(actual.has_value() and actual->m_intervals.size() == expected.m_intervals[0].size());
            for (std::size_t record = 0u; record < actual->m_intervals.size(); ++record) {
                assertClose_(actual->m_intervals[record], expected.m_intervals[0][record]);
            }

            PPR_TEST_ASSERT(actual->m_c1_probe0_bin0.has_value());
            assertClose_(*actual->m_c1_probe0_bin0, expected.m_intervals[1][0]);
            if (width == 8u and height == 8u) {
                const auto wrong = solveCpuCascades_(grid, request, true);
                const double delta = std::abs(
                    wrong.m_intervals[1][0].m_radiance[0] - expected.m_intervals[1][0].m_radiance[0]);
                PPR_TEST_ASSERT(delta > 1e-4);
            }
        }

        std::vector<lighting::OpticalCell> oversized(
            lighting::RadianceCascadePass::kMaxCompleteFixtureCells + 1u, vacuum_());
        const lighting::OpticalGridView too_large{
            oversized, {}, 1.0, lighting::RadianceCascadePass::kMaxCompleteFixtureCells + 1u, 1u,
            lighting::OpticalBoundary::vacuum};
        const auto large_request = cascadeRequest_(too_large.m_width, too_large.m_height);
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareCascades(too_large, large_request, fixture.m_fence.completed()) ==
            std::errc::function_not_supported);

        const std::array<lighting::OpticalCell, 4> small{vacuum_(), vacuum_(), vacuum_(), vacuum_()};
        const lighting::OpticalGridView grid{small, {}, 1.0, 2u, 2u, lighting::OpticalBoundary::vacuum};
        auto request = cascadeRequest_(2u, 2u);
        request.m_c0_tile_width = 1u;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()) ==
            std::errc::function_not_supported);

        request = cascadeRequest_(2u, 2u);
        ++request.m_cascade_count;
        PPR_TEST_ASSERT(
            fixture.m_pass.prepareCascades(grid, request, fixture.m_fence.completed()) ==
            std::errc::function_not_supported);
    };

    PPR_UNIT_TEST (bin_normalization_and_independent_direct_ray_error_are_distinct) {
        Fixture fixture{};
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        std::array<lighting::OpticalCell, 16> cells{};
        cells.fill(lighting::OpticalCell{{}, {}, {2.0, 4.0, 8.0}, lighting::OpticalKind::opaque});
        const lighting::OpticalGridView uniform{cells, {}, 1.0, 4u, 4u, lighting::OpticalBoundary::vacuum};
        const auto uniform_request = cascadeRequest_(4u, 4u);
        PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(uniform, uniform_request, fixture.m_fence.completed()));
        fixture.submit(true);
        const auto constant = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(constant.has_value());
        for (std::size_t probe = 0u; probe < cells.size(); ++probe) {
            lighting::OpticalRgb mean{};
            for (std::size_t bin = 0u; bin < 8u; ++bin) {
                const auto &interval = constant->m_intervals[probe * 8u + bin];
                assertClose_(interval, {{2.0, 4.0, 8.0}, {}});
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    mean[channel] += interval.m_radiance[channel] / 8.0;
                }
            }

            PPR_TEST_ASSERT(mean == lighting::OpticalRgb{2.0, 4.0, 8.0});
        }

        std::array<lighting::OpticalCell, 128> room{};
        room.fill(vacuum_());
        for (u32 y = 0u; y < 8u; ++y) {
            room[y * 16u + 8u] = lighting::OpticalCell{{}, {}, {}, lighting::OpticalKind::opaque};
            room[y * 16u + 14u] = lighting::OpticalCell{{}, {}, {8.0, 4.0, 2.0}, lighting::OpticalKind::opaque};
        }

        const lighting::OpticalGridView world{room, {}, 1.0, 16u, 8u, lighting::OpticalBoundary::opaque};
        const auto request = cascadeRequest_(16u, 8u);
        PPR_TEST_ASSERT(not fixture.m_pass.prepareCascades(world, request, fixture.m_fence.completed()));
        fixture.submit(true);
        const auto actual = fixture.m_pass.readCompleted(fixture.m_fence.completed());
        PPR_TEST_ASSERT(actual.has_value());

        std::array<double, 2> direct_mean{};
        std::array<double, 2> rc_mean{};
        for (std::size_t side = 0u; side < 2u; ++side) {
            const u32 x = side == 0u ? 3u : 11u;
            const std::array<double, 2> origin{double(x) + 0.5, 4.5};
            // Independent 1024-angle quadrature, no RC directions/probes/merge.
            for (u32 sample = 0u; sample < 1024u; ++sample) {
                const double angle = 2.0 * std::numbers::pi * (double(sample) + 0.5) / 1024.0;
                const auto ray = lighting::traceOpticalReference(
                    world, {origin, {std::cos(angle), std::sin(angle)}, 0.0, 32.0});
                PPR_TEST_ASSERT(ray.has_value());
                direct_mean[side] += ray->m_radiance[0] / 1024.0;
            }

            for (std::size_t bin = 0u; bin < 8u; ++bin) {
                rc_mean[side] += actual->m_intervals[(4u * 16u + x) * 8u + bin].m_radiance[0] / 8.0;
            }
        }

        PPR_TEST_ASSERT(direct_mean[0] == 0.0 and rc_mean[0] == 0.0);
        PPR_TEST_ASSERT(direct_mean[1] > 0.0 and std::isfinite(rc_mean[1]));
        std::println(
            "RC physical diagnostic red: blocked_direct={} blocked_rc={} open_direct={} open_rc={} abs_error={}",
            direct_mean[0], rc_mean[0], direct_mean[1], rc_mean[1], std::abs(rc_mean[1] - direct_mean[1]));
    };

}

namespace pP::tests {
    // ------------------------------------------------------------------
    // GPU optical qualification suite
    // ------------------------------------------------------------------

    const UnitTest radiance_gpu = UnitTest::Named("radiance_gpu") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::RadianceGpu::fp32_scalar_trace_matches_quantized_optical_oracle,
                detail::RadianceGpu::vacuum_hierarchy_preserves_optics_and_rebuilds_captured_edits,
                detail::RadianceGpu::admission_quantization_and_abandoned_batches_preserve_safe_reuse,
                detail::RadianceGpu::generic_cell_positive_underflow_never_returns_a_complete_interval,
                detail::RadianceGpu::material_optics_palette_uses_actual96_byte_shader_rows,
                detail::RadianceGpu::fp64_runtime_arithmetic_preserves_double_precision,
                detail::RadianceGpu::terrain_precision_selection_rejects_unqualified_sessions_before_initialization,
                detail::RadianceGpu::terrain_precision_source_bridge_preserves_fp32_ownership_and_counter_settlement,
                detail::RadianceGpu::terrain_receiver_continuation_kernel_preserves_sites_and_cell_seams,
                detail::RadianceGpu::mapped_diffuse_uses_actual_angular_field_and_elevation_lift,
                detail::RadianceGpu::terrain_background_cpu_nearfield_dependency_comparison,
                detail::RadianceGpu::terrain_receiver_continuation_experiment_respects_blocked_side_quality_gate,
                detail::RadianceGpu::canonical_roundness_bits_match_actual_raster_and_terrain_paths,
                detail::RadianceGpu::rounded_terrain_trace_matches_analytic_sources_and_immutable_rebuilds,
                detail::RadianceGpu::rounded_terrain_near_tangent_and_collinear_queries_report_ambiguity,
                detail::RadianceGpu::rounded_terrain_fp32_cascades_reuse_the_declared_rc_schedule,
                detail::RadianceGpu::packed_half_rounding_endpoints_admission_and_visible_overflow,
                detail::RadianceGpu::packed_cascades_match_per_level_half_reference_and_report_storage_error,
                detail::RadianceGpu::fp32_cascades_match_double_schedule_and_degenerate_axis_support,
                detail::RadianceGpu::bin_normalization_and_independent_direct_ray_error_are_distinct});

        // A separate recurse call per scenario fixes pristine receiver-first
        // order even when the unrelated leaves use the runner's shuffle mode.
        detail::RadianceGpu::TerrainFieldScope field_scope{};
        auto *const previous_scope = detail::RadianceGpu::s_terrain_field_scope;
        detail::RadianceGpu::s_terrain_field_scope = &field_scope;
        PPR_DEFER {
            detail::RadianceGpu::s_terrain_field_scope = previous_scope;
            field_scope.shutdown();
        };

        _.recurse(detail::RadianceGpu::terrain_receivers_reconstruct_c1_and_retain_completed_generation_leases);
        _.recurse(detail::RadianceGpu::terrain_background_view_preserves_world_visibility_samples_and_leases);
    };

    const UnitTest &radiance_gpuTests() noexcept {
        return radiance_gpu;
    }
}
