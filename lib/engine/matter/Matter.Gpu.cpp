module;
#include "pP/Macros.h"
#include <slang.h>

module engine.matter;

import :gpu;
import :gpu.sparse;
import :gpu.types;
import std;
import engine.core;
import engine.shader;
import engine.rhi;
import engine.math;

namespace pP::matter {
    PPR_DEFINE_LOG_CATEGORY(MatterGpu, debug, none)

    using ::pP::make_error_code;

    // ------------------------------------------------------------------
    // storage admission and explicit resource bindings
    // ------------------------------------------------------------------

    namespace {
        constexpr u64 kErrorBytes = 6u * sizeof(u32);
        constexpr std::array<const char *, 2u> kEntries{"fused0", "fused1"};
        constexpr std::array<const char *, 2u> kWaveEntries{"fused0Wave", "fused1Wave"};

        rhi::BufferRange fullRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        bool finiteMatrix(const SourceMatrix &m) noexcept {
            return std::isfinite(m.m_c0x) and
                   std::isfinite(m.m_c0y) and
                   std::isfinite(m.m_c1x) and
                   std::isfinite(m.m_c1y);
        }

        bool validParticle(const GpuParticle &p, const GpuParameters &profile) noexcept {
            const bool supported = p.m_material == 0.0f or
                                   p.m_material == 1.0f or
                                   p.m_material == 2.0f or
                                   p.m_material == 3.0f;
            return supported and
                   p.m_enabled == 1.0f and
                   std::isfinite(p.m_position_x) and
                   std::isfinite(p.m_position_y) and
                   p.m_position_x >= 3.0f and
                   p.m_position_x <= static_cast<float>(profile.m_grid_width) - 4.0f and
                   p.m_position_y >= 3.0f and
                   p.m_position_y <= static_cast<float>(profile.m_grid_height) - 4.0f and
                   std::isfinite(p.m_displacement_x) and
                   std::isfinite(p.m_displacement_y) and
                   finiteMatrix(p.m_deformation_gradient) and
                   finiteMatrix(p.m_deformation_displacement) and
                   std::isfinite(p.m_liquid_density) and
                   p.m_liquid_density > 0.0f and
                   std::isfinite(p.m_mass) and
                   p.m_mass > 0.0f and
                   std::isfinite(p.m_volume) and
                   p.m_volume > 0.0f and
                   std::isfinite(p.m_lambda) and
                   std::isfinite(p.m_log_jp) and
                   std::isfinite(p.m_color_r) and
                   std::isfinite(p.m_color_g) and
                   std::isfinite(p.m_color_b);
        }

        std::error_code createBuffer(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial, const char *const label,
            rhi::ComPtr<rhi::IBuffer> &result, const bool readback = false, const bool indirect = false) {
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.elementSize = readback ? 0u : stride;
            desc.memoryType = readback ? rhi::MemoryType::ReadBack : rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::CopyDestination) |
                (readback ? 0u
                          : static_cast<u32>(rhi::BufferUsage::CopySource) |
                                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                (indirect ? static_cast<u32>(rhi::BufferUsage::IndirectArgument) : 0u)));
            desc.defaultState = readback ? rhi::ResourceState::CopyDestination : rhi::ResourceState::UnorderedAccess;
            desc.label = label;

            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, device.createBuffer(desc, initial, result.writeRef()));
            return {};
        }

        struct Kernel {
            rhi::ComPtr<rhi::IShaderProgram> m_program;
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline;
        };

        struct ParticleStorage {
            SparseGridStorage m_sparse;
            rhi::ComPtr<rhi::IBuffer> m_committed_hot;
            rhi::ComPtr<rhi::IBuffer> m_committed_cold;
            rhi::ComPtr<rhi::IBuffer> m_working_hot;
            rhi::ComPtr<rhi::IBuffer> m_working_cold;
            rhi::ComPtr<rhi::IBuffer> m_shapes;
            rhi::ComPtr<rhi::IBuffer> m_readback;
            rhi::ComPtr<rhi::IBuffer> m_terrain_masks;
            rhi::ComPtr<rhi::IBuffer> m_terrain_boundary_tiles;
            u64 m_hot_bytes{};
            u64 m_cold_bytes{};
            u32 m_particle_count{};
            u32 m_shape_count{};
            GpuEnvironment m_environment{};
        };

        struct BoundaryStorage {
            rhi::ComPtr<rhi::IBuffer> m_bodies;
            u32 m_row_capacity{};
            u32 m_body_count{};
        };
    }

    // ------------------------------------------------------------------
    // opt-in bounded device and fence observations
    // ------------------------------------------------------------------

    namespace {
        constexpr u32 kTimingQueries = 44u;
        constexpr std::array<const char *, 8u> kTimingStages{
            "particle_copy", "source_metadata_reset", "sparse_discovery", "initialize_b_and_budget", "fused0",
            "remap_a_and_budget", "fused1", "status_copy"};

        struct TickTimingRecord {
            std::array<u64, kTimingStages.size()> m_ticks{};
            TimePoint m_submit_begin{}, m_submit_end{}, m_first_poll{}, m_last_pending{}, m_ready{}, m_previous_poll{};
            TimeSpan m_poll_work{}, m_max_poll_gap{};
            u64 m_epoch{}, m_signal{}, m_first_gpu{}, m_last_gpu{}, m_polls{};
            u32 m_substeps{}, m_particles{};
            bool m_host_drain{};
        };

        struct TickTiming {
            rhi::ComPtr<rhi::IQueryPool> m_queries;
            // Injected by the application; its clock outlives runtime shutdown.
            const ITimerClock *m_clock{};
            std::array<TickTimingRecord, 60u> m_recent{};
            TickTimingRecord m_pending{};
            std::array<u64, kTimingStages.size()> m_sum{}, m_max{};
            std::array<u64, kTimingQueries> m_ticks{};
            TimePoint m_origin{}, m_first_submit{}, m_last_ready{};
            std::error_code m_error{};
            u64 m_samples{}, m_skipped{}, m_first_epoch{}, m_first_signal{};
            u64 m_last_epoch{}, m_last_signal{}, m_last_first_gpu{}, m_last_gpu{};
            double m_frequency{};
            u32 m_written{}, m_expected{};
            bool m_recording{}, m_calibration_supported{}, m_reported{};

            void fail(const std::error_code error) {
                if (not m_error) {
                    // Best-effort diagnostics never replace the solver's error policy.
                    m_error = error;
                    PPR_LOG(MatterGpu, error, "GPU tick timing invalidated", {{"error", error.message()}});
                }
                m_recording = false;
            }

            double milliseconds(const TimeSpan span) const noexcept {
                return time::seconds(span) * 1000.0;
            }

            void calibrate(rhi::ICommandQueue &queue, const bool final) {
                if (not m_calibration_supported or m_error)
                    return;

                rhi::TimestampCalibration calibration{};
                const auto before = m_clock->now();
                const auto error = make_error_code(queue.getTimestampCalibration(&calibration));
                const auto after = m_clock->now();
                if (error) {
                    fail(error);
                    return;
                }

                if (calibration.gpuFrequency == 0u or calibration.cpuFrequency == 0u or after < before) {
                    fail(std::make_error_code(std::errc::invalid_argument));
                    return;
                }

                // These are measured bridge intervals, not an assumed QPC/steady
                // epoch identity. Offline alignment must preserve both uncertainties.
                PPR_LOG(
                    MatterGpu, emphasis, "GPU tick timing calibration",
                    {{"final", final}, {"cpu_domain", static_cast<u32>(calibration.cpuDomain)},
                        {"clock_origin_ns",
                            std::chrono::duration_cast<std::chrono::nanoseconds>(m_origin.time_since_epoch()).count()},
                        {"cpu_ticks", calibration.cpuTimestamp}, {"cpu_hz", calibration.cpuFrequency},
                        {"gpu_ticks", calibration.gpuTimestamp}, {"gpu_hz", calibration.gpuFrequency},
                        {"max_deviation_ns", calibration.maxDeviationNs},
                        {"clock_before_ms", milliseconds(before - m_origin)},
                        {"clock_after_ms", milliseconds(after - m_origin)}});
            }

            void mark(rhi::ICommandEncoder &encoder) {
                if (not m_recording)
                    return;

                PPR_ASSERT(m_written < m_expected and m_expected <= kTimingQueries);
                encoder.writeTimestamp(m_queries, m_written++);
            }

            void observePoll(const TimePoint before, const TimePoint after, const u64 completed) {
                if (m_error or m_pending.m_signal == 0u or m_pending.m_ready != TimePoint{})
                    return;

                auto &record = m_pending;
                if (record.m_polls == 0u)
                    record.m_first_poll = before;
                else
                    record.m_max_poll_gap = std::max(record.m_max_poll_gap, before - record.m_previous_poll);

                ++record.m_polls;
                record.m_previous_poll = before;
                record.m_poll_work += after - before;
                if (completed < record.m_signal)
                    record.m_last_pending = after;
                else
                    record.m_ready = after;
            }

            void collect(const u64 completed) {
                if (m_error or m_pending.m_signal == 0u or completed < m_pending.m_signal)
                    return;

                rhi::QueryResultState readiness{};
                auto error = make_error_code(m_queries->getResultState(0u, m_expected, &readiness));
                if (not error and readiness != rhi::QueryResultState::Resolved)
                    return;

                if (not error)
                    error = make_error_code(m_queries->getResult(0u, m_expected, m_ticks.data()));
                if (error) {
                    fail(error);
                    return;
                }

                if (m_written != m_expected or not std::is_sorted(m_ticks.begin(), m_ticks.begin() + m_expected)) {
                    fail(std::make_error_code(std::errc::illegal_byte_sequence));
                    return;
                }

                auto &record = m_pending;
                record.m_first_gpu = m_ticks[0u];
                record.m_last_gpu = m_ticks[m_expected - 1u];
                record.m_ticks[0u] = m_ticks[1u] - m_ticks[0u];
                record.m_ticks[1u] = m_ticks[2u] - m_ticks[1u];
                for (u32 substep = 0u; substep < record.m_substeps; ++substep) {
                    for (u32 stage = 0u; stage < 5u; ++stage) {
                        const u32 at = 3u + substep * 5u + stage;
                        record.m_ticks[stage + 2u] += m_ticks[at] - m_ticks[at - 1u];
                    }
                }
                record.m_ticks[7u] = m_ticks[m_expected - 1u] - m_ticks[m_expected - 2u];

                if (m_samples == 0u) {
                    m_first_submit = record.m_submit_begin;
                    m_first_epoch = record.m_epoch;
                    m_first_signal = record.m_signal;
                }
                for (u32 stage = 0u; stage < kTimingStages.size(); ++stage) {
                    m_sum[stage] += record.m_ticks[stage];
                    m_max[stage] = std::max(m_max[stage], record.m_ticks[stage]);
                }

                m_recent[m_samples % m_recent.size()] = record;
                ++m_samples;
                m_last_ready = record.m_ready;
                m_last_epoch = record.m_epoch;
                m_last_signal = record.m_signal;
                m_last_first_gpu = record.m_first_gpu;
                m_last_gpu = record.m_last_gpu;
                // Only matching-fence AND resolved collection clears this range.
                m_pending = {};
            }

            void begin(rhi::ICommandEncoder &encoder, const u64 epoch, const u32 substeps, const u32 particles) {
                m_recording = false;
                if (m_error)
                    return;
                if (m_pending.m_signal != 0u) {
                    ++m_skipped;
                    return;
                }
                if (substeps > kGameSubsteps) {
                    fail(std::make_error_code(std::errc::not_supported));
                    return;
                }

                m_expected = 5u * substeps + 4u;
                const auto error = make_error_code(m_queries->reset(0u, m_expected));
                if (error) {
                    fail(error);
                    return;
                }

                m_pending = {};
                m_pending.m_epoch = epoch;
                m_pending.m_substeps = substeps;
                m_pending.m_particles = particles;
                m_written = 0u;
                m_recording = true;
                mark(encoder);
            }

            void report(rhi::ICommandQueue &queue) {
                if (m_reported)
                    return;
                m_reported = true;
                calibrate(queue, true);

                const u64 recent_count = std::min<u64>(m_samples, m_recent.size());
                std::array<u64, kTimingStages.size()> recent_sum{}, recent_max{};
                u64 polls{};
                TimeSpan poll_work{}, max_gap{}, submit_work{}, observed_latency{};
                for (u64 index = 0u; index < recent_count; ++index) {
                    const auto &record = m_recent[index];
                    polls += record.m_polls;
                    poll_work += record.m_poll_work;
                    max_gap = std::max(max_gap, record.m_max_poll_gap);
                    submit_work += record.m_submit_end - record.m_submit_begin;
                    observed_latency += record.m_ready - record.m_submit_begin;
                    for (u32 stage = 0u; stage < kTimingStages.size(); ++stage) {
                        recent_sum[stage] += record.m_ticks[stage];
                        recent_max[stage] = std::max(recent_max[stage], record.m_ticks[stage]);
                    }
                }

                const auto &oldest = m_recent[m_samples >= m_recent.size() ? m_samples % m_recent.size() : 0u];
                const double frequency = m_frequency > 0.0 ? m_frequency : 1.0;
                const u64 total_ticks = std::accumulate(m_sum.begin(), m_sum.end(), u64{});
                const u64 recent_ticks = std::accumulate(recent_sum.begin(), recent_sum.end(), u64{});
                PPR_LOG(
                    MatterGpu, emphasis, "GPU tick timing summary",
                    {{"valid", not m_error}, {"samples", m_samples}, {"recent_samples", recent_count},
                        {"skipped_pending_query", m_skipped}, {"pending_signal", m_pending.m_signal},
                        {"query_slots", kTimingQueries}, {"query_readback_payload_bytes", kTimingQueries * sizeof(u64)},
                        {"cpu_storage_bytes", sizeof(TickTiming)}, {"gpu_hz", m_frequency},
                        {"clock_origin_ns",
                            std::chrono::duration_cast<std::chrono::nanoseconds>(m_origin.time_since_epoch()).count()},
                        {"gpu_total_ms", static_cast<double>(total_ticks) * 1000.0 / frequency},
                        {"recent_gpu_total_ms", static_cast<double>(recent_ticks) * 1000.0 / frequency},
                        {"calibration_supported", m_calibration_supported}, {"first_epoch", m_first_epoch},
                        {"first_signal", m_first_signal}, {"last_epoch", m_last_epoch}, {"last_signal", m_last_signal},
                        {"recent_first_epoch", oldest.m_epoch}, {"recent_first_signal", oldest.m_signal},
                        {"first_submit_ms", m_samples ? milliseconds(m_first_submit - m_origin) : 0.0},
                        {"recent_first_submit_ms", recent_count ? milliseconds(oldest.m_submit_begin - m_origin) : 0.0},
                        {"last_ready_ms", m_samples ? milliseconds(m_last_ready - m_origin) : 0.0},
                        {"recent_polls", polls}, {"recent_poll_elapsed_ms", milliseconds(poll_work)},
                        {"recent_max_poll_gap_ms", milliseconds(max_gap)},
                        {"recent_submit_elapsed_ms", milliseconds(submit_work)},
                        {"recent_observed_latency_ms", milliseconds(observed_latency)}});

                for (u32 stage = 0u; stage < kTimingStages.size(); ++stage) {
                    PPR_LOG(
                        MatterGpu, emphasis, "GPU tick timing stage",
                        {{"stage", kTimingStages[stage]}, {"samples", m_samples}, {"recent_samples", recent_count},
                            {"total_ms", static_cast<double>(m_sum[stage]) * 1000.0 / frequency},
                            {"max_tick_ms", static_cast<double>(m_max[stage]) * 1000.0 / frequency},
                            {"recent_total_ms", static_cast<double>(recent_sum[stage]) * 1000.0 / frequency},
                            {"recent_max_tick_ms", static_cast<double>(recent_max[stage]) * 1000.0 / frequency}});
                }

                if (m_samples != 0u) {
                    const auto &last = m_recent[(m_samples - 1u) % m_recent.size()];
                    PPR_LOG(
                        MatterGpu, emphasis, "GPU tick timing last fence observation",
                        {{"epoch", last.m_epoch}, {"signal", last.m_signal}, {"substeps", last.m_substeps},
                            {"particles", last.m_particles}, {"first_gpu_ticks", last.m_first_gpu},
                            {"last_gpu_ticks", last.m_last_gpu}, {"completed_after_host_drain", last.m_host_drain},
                            {"submit_begin_ms", milliseconds(last.m_submit_begin - m_origin)},
                            {"submit_end_ms", milliseconds(last.m_submit_end - m_origin)},
                            {"first_poll_ms", milliseconds(last.m_first_poll - m_origin)},
                            {"last_pending_poll_ms", last.m_last_pending != TimePoint{}
                                                         ? milliseconds(last.m_last_pending - m_origin)
                                                         : -1.0},
                            {"ready_poll_ms", milliseconds(last.m_ready - m_origin)}});
                }
            }
        };
    }

    // ------------------------------------------------------------------
    // solver-owned resources and dispatch bindings
    // ------------------------------------------------------------------

    struct GpuSolver::State {
        std::array<Kernel, 2u> m_kernels;
        ParticleStorage m_particles;
        std::optional<ParticleStorage> m_replacement;
        BoundaryStorage m_boundaries;
        rhi::ComPtr<rhi::IBuffer> m_contour_points;
        rhi::ComPtr<rhi::IBuffer> m_contour_cases;
        rhi::ComPtr<rhi::IBuffer> m_empty_terrain;
        // Retained with the command buffer through completion/discard. No root
        // is rewritten after its dispatch has been encoded.
        Array<rhi::ComPtr<rhi::IShaderObject>> m_dispatch_roots;
        rhi::ComPtr<rhi::IFence> m_fence;
        rhi::ComPtr<rhi::ICommandBuffer> m_commands;
        rhi::ComPtr<rhi::IBuffer> m_append_upload_hot;
        rhi::ComPtr<rhi::IBuffer> m_append_upload_cold;
        std::unique_ptr<TickTiming> m_timing;
        std::shared_ptr<std::atomic<u64>> m_live_epoch{std::make_shared<std::atomic<u64>>(0u)};
        rhi::IDevice *m_device{};
        rhi::ICommandQueue *m_queue{};
        IShaderService *m_shaders{};
        std::filesystem::path m_content_root;
        GpuParameters m_profile;
        GpuAccounting m_accounting;
        GpuFailure m_failure;
        std::error_code m_injected_collection_error;
        u64 m_next_signal{1u};
        u64 m_pending_signal{};
        u64 m_snapshot_signal{};
        u64 m_replacement_signal{};
        u32 m_substep{};
        bool m_inflight{};
        bool m_complete{};
        bool m_valid{};
        bool m_readback_mapped{};
        bool m_sparse_encoding_open{};

        std::error_code ensureBoundaries(const u32 rows) {
            if (m_boundaries.m_row_capacity >= std::max(1u, rows))
                return {};

            BoundaryStorage replacement;
            replacement.m_row_capacity = std::max({1u, rows, m_boundaries.m_row_capacity});
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu,
                createBuffer(
                    *m_device, static_cast<u64>(replacement.m_row_capacity) * sizeof(GpuBodyBoundary),
                    sizeof(GpuBodyBoundary), nullptr, "PBMPM immutable predicted boundaries", replacement.m_bodies));

            m_boundaries = std::move(replacement);
            return {};
        }

        std::error_code bindAndDispatch(rhi::ICommandEncoder &encoder, const u32 iteration) {
            Kernel &kernel = m_kernels[iteration];
            const u32 dispatch_index = m_substep * 2u + iteration;
            rhi::IShaderObject *const root = m_dispatch_roots[dispatch_index].get();
            rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor parameters{};
            GpuParameters constants = m_profile;
            constants.m_reserved0 = iteration;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor["g_parameters"].getDereferenced(parameters));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, parameters.setData(&constants, sizeof(constants)));

            const auto sparse = m_particles.m_sparse.constants(iteration != 0u);
            const auto &buffers = m_particles.m_sparse.buffers();
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor["g_sparse"].getDereferenced(parameters));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, parameters.setData(&sparse, sizeof(sparse)));

            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor[name].setBinding(rhi::Binding(buffer, fullRange(buffer))));
                return {};
            };

            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_hot", m_particles.m_working_hot));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_cold", m_particles.m_working_cold));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_errors", buffers.m_errors));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_rows", buffers.m_rows));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_buckets", buffers.m_groups));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_sparseCounters", buffers.m_counters));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_pageIndexA", buffers.m_page_index[0u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_pageIndexB", buffers.m_page_index[1u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_payloadA", buffers.m_payload[0u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_payloadB", buffers.m_payload[1u]));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_sparseBudget", buffers.m_budget));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_shapes", m_particles.m_shapes));

            GpuEnvironment environment = m_particles.m_environment;
            environment.m_body_count = m_boundaries.m_body_count;
            environment.m_body_base = m_substep * m_boundaries.m_body_count;
            rhi::ShaderCursor environment_cursor{};
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, cursor["g_environment"].getDereferenced(environment_cursor));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, environment_cursor.setData(&environment, sizeof(environment)));

            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, bind(
                               "g_terrain_masks", m_particles.m_terrain_masks ? m_particles.m_terrain_masks.get()
                                                                              : m_empty_terrain.get()));
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, bind(
                               "g_terrain_boundary_tiles", m_particles.m_terrain_boundary_tiles
                                                               ? m_particles.m_terrain_boundary_tiles.get()
                                                               : m_empty_terrain.get()));

            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_contour_points", m_contour_points));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_contour_cases", m_contour_cases));
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, bind("g_bodies", m_boundaries.m_bodies));

            rhi::IComputePassEncoder *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(kernel.m_pipeline, root);
                pass->dispatchComputeIndirect({buffers.m_arguments.get(), 32u});

                ++m_accounting.m_dispatches;
            }

            encoder.globalBarrier();
            return {};
        }
    };

    // ------------------------------------------------------------------
    // lifecycle and program creation
    // ------------------------------------------------------------------

    GpuSolver::GpuSolver() noexcept = default;

    GpuSolver::GpuSolver(GpuSolver &&) noexcept = default;

    GpuSolver::~GpuSolver() {
        // Unknown GPU completion must not turn into normal resource destruction.
        // Explicit shutdown reports the error; a missed fatal boundary terminates.
        if (const std::error_code error = shutdown()) {
            PPR_LOG(MatterGpu, error, "GPU solver destruction could not establish completion");
            std::terminate();
        }
    }

    std::error_code GpuSolver::initialize(
        rhi::IDevice &device, rhi::ICommandQueue &queue, IShaderService &shaders,
        const std::filesystem::path &content_root, const GpuParameters profile,
        const ITimerClock *const observation_clock) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, not m_state ? std::error_code{} : std::make_error_code(std::errc::already_connected));

        const u64 nodes = static_cast<u64>(profile.m_grid_width) * profile.m_grid_height;
        const bool valid = profile.m_grid_width >= 9u and
                           profile.m_grid_height >= 9u and
                           nodes <= std::numeric_limits<u32>::max() / 4u and
                           profile.m_fixed_point_multiplier != 0u and
                           std::isfinite(profile.m_delta_time) and
                           profile.m_delta_time > 0.0f and
                           std::isfinite(profile.m_elasticity_ratio) and
                           std::isfinite(profile.m_gravity_strength) and
                           std::isfinite(profile.m_liquid_relaxation) and
                           std::isfinite(profile.m_elastic_relaxation) and
                           std::isfinite(profile.m_liquid_viscosity) and
                           std::isfinite(profile.m_friction_angle) and
                           std::isfinite(profile.m_plasticity) and
                           profile.m_use_grid_volume_for_liquid <= 1u;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, valid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        const hal::ProfileScope scope{"Matter.Gpu.Initialize"};
        auto state = std::make_unique<State>();
        state->m_device = &device;
        state->m_queue = &queue;
        state->m_shaders = &shaders;
        state->m_content_root = content_root;
        state->m_profile = profile;
        state->m_profile.m_particle_count = 0u;
        state->m_profile.m_shape_count = 0u;

        const bool use_wave = device.hasFeature(rhi::Feature::WaveOps);

        shader::SharedModule module;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, shaders.loadModuleFromFile(
                           content_root / "shaders" / "pbmpm" / "solver.slang", "pbmpm_solver", module.writeRef()));

        for (u32 index = 0u; index < state->m_kernels.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu,
                module->findEntryPointByName(use_wave ? kWaveEntries[index] : kEntries[index], entry.writeRef()));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entries;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            Kernel &kernel = state->m_kernels[index];
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, device.createShaderProgram(desc, kernel.m_program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = kernel.m_program;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, device.createComputePipeline(pipeline, kernel.m_pipeline.writeRef()));
        }

        rhi::FenceDesc fence{};
        fence.label = "PBMPM completion";
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, device.createFence(fence, state->m_fence.writeRef()));
        const auto tables = packContourTables(math::contourPoints(), math::contourCases());
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, tables);

        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           device, sizeof(u32), sizeof(u32), &tables->m_points, "PBMPM compact contour points",
                           state->m_contour_points));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           device, sizeof(tables->m_cases), sizeof(PackedContourCase), tables->m_cases.data(),
                           "PBMPM compact contour cases", state->m_contour_cases));
        const u32 empty_mask{};
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            createBuffer(
                device, sizeof(u32), sizeof(u32), &empty_mask, "PBMPM absent terrain binding", state->m_empty_terrain));
        state->m_accounting.m_device_bytes += sizeof(PackedContourTables) + sizeof(u32);
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state->ensureBoundaries(0u));

        m_state = std::move(state);
        // Create the same valid dummy resources used by the empty-world path.
        if (const std::error_code error = prepareReplacement({}, {}, 0u)) {
            m_state.reset();
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, error);
        }

        publishReplacement();
        m_state->m_accounting.m_epoch = 0u;
        m_state->m_live_epoch->store(0u, std::memory_order_release);

        if (observation_clock) {
            auto timing = std::make_unique<TickTiming>();
            timing->m_clock = observation_clock;
            timing->m_origin = observation_clock->now();
            timing->m_frequency = static_cast<double>(device.getInfo().timestampFrequency);
            timing->m_calibration_supported = device.hasFeature(rhi::Feature::TimestampCalibration);
            rhi::QueryPoolDesc queries{};
            queries.type = rhi::QueryType::Timestamp;
            queries.count = kTimingQueries;
            queries.label = "PBMPM diagnostic stage timestamps";
            const auto error = make_error_code(device.createQueryPool(queries, timing->m_queries.writeRef()));
            if (error or timing->m_frequency <= 0.0)
                timing->fail(error ? error : std::make_error_code(std::errc::not_supported));
            else
                timing->calibrate(queue, false);

            m_state->m_timing = std::move(timing);
        }

        return {};
    }

    // ------------------------------------------------------------------
    // prepared replacement transaction
    // ------------------------------------------------------------------

    std::error_code GpuSolver::prepareReplacement(
        const std::span<const GpuParticle> particles, const std::span<const GpuStaticShape> shapes,
        const u64 expected_epoch, const TerrainView terrain) {
        return prepareReplacement_(particles, shapes, expected_epoch, terrain, false);
    }

    std::error_code GpuSolver::prepareAppend(
        const std::span<const GpuParticle> appended, const u64 expected_epoch, const TerrainView terrain) {
        return prepareReplacement_(appended, {}, expected_epoch, terrain, true);
    }

    std::error_code GpuSolver::prepareReplacement_(
        const std::span<const GpuParticle> particles, const std::span<const GpuStaticShape> shapes,
        const u64 expected_epoch, const TerrainView terrain, const bool append) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            m_state and not busy() ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            expected_epoch == state.m_accounting.m_epoch and expected_epoch != std::numeric_limits<u64>::max()
                ? std::error_code{}
                : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            particles.size() <= std::numeric_limits<u32>::max() and shapes.size() <= std::numeric_limits<u32>::max()
                ? std::error_code{}
                : std::make_error_code(std::errc::value_too_large));

        const u32 old_count = append ? state.m_profile.m_particle_count : 0u;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, particles.size() <= std::numeric_limits<u32>::max() - old_count
                           ? std::error_code{}
                           : std::make_error_code(std::errc::value_too_large));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, not append or state.m_next_signal != std::numeric_limits<u64>::max()
                           ? std::error_code{}
                           : std::make_error_code(std::errc::value_too_large));

        for (const GpuParticle &particle: particles) {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, validParticle(particle, state.m_profile)
                               ? std::error_code{}
                               : std::make_error_code(std::errc::invalid_argument));
        }

        for (const GpuStaticShape &shape: shapes) {
            const bool valid = (shape.m_shape == EStaticShape::box or shape.m_shape == EStaticShape::circle) and
                               std::isfinite(shape.m_position_x) and
                               std::isfinite(shape.m_position_y) and
                               std::isfinite(shape.m_rotation_degrees) and
                               std::isfinite(shape.m_half_size_x) and
                               std::isfinite(shape.m_half_size_y) and
                               std::isfinite(shape.m_radius) and
                               shape.m_half_size_x >= 0.0f and
                               shape.m_half_size_y >= 0.0f and
                               shape.m_radius >= 0.0f;
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, valid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        const u64 terrain_cells = static_cast<u64>(terrain.m_width) * terrain.m_height;
        const bool terrain_present = not terrain.m_masks.empty();
        const bool terrain_dimensions = terrain_present ? terrain.m_width != 0u and
                                                              terrain.m_height != 0u and
                                                              terrain_cells == terrain.m_masks.size() and
                                                              terrain.m_width <= (1u << 22u) and
                                                              terrain.m_height <= (1u << 22u) and
                                                              terrain_cells <= std::numeric_limits<u32>::max()
                                                        : terrain.m_width == 0u and terrain.m_height == 0u;
        const bool terrain_origin = std::isfinite(terrain.m_origin_x) and
                                    std::isfinite(terrain.m_origin_y) and
                                    terrain.m_origin_x >= 0.0f and
                                    terrain.m_origin_y >= 0.0f and
                                    static_cast<double>(terrain.m_origin_x) + terrain.m_width <= (1u << 22u) and
                                    static_cast<double>(terrain.m_origin_y) + terrain.m_height <= (1u << 22u) and
                                    std::floor(terrain.m_origin_x * 2.0f) == terrain.m_origin_x * 2.0f and
                                    std::floor(terrain.m_origin_y * 2.0f) == terrain.m_origin_y * 2.0f;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, terrain_dimensions and terrain_origin ? std::error_code{}
                                                             : std::make_error_code(std::errc::invalid_argument));

        const hal::ProfileScope scope{"Matter.Gpu.PrepareReplacement"};
        const auto packed_terrain = packTerrain(terrain);
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, packed_terrain);

        ParticleStorage replacement;
        replacement.m_particle_count = old_count + static_cast<u32>(particles.size());
        replacement.m_hot_bytes = std::max<u64>(1u, replacement.m_particle_count) * sizeof(ParticleWrite);
        replacement.m_cold_bytes = std::max<u64>(1u, replacement.m_particle_count) * sizeof(ParticleReadonly);
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, replacement.m_sparse.initialize(
                           *state.m_device, *state.m_shaders, state.m_content_root, replacement.m_particle_count,
                           state.m_profile.m_grid_width, state.m_profile.m_grid_height));

        replacement.m_shape_count = static_cast<u32>(shapes.size());
        replacement.m_environment.m_terrain_width = terrain.m_width;
        replacement.m_environment.m_terrain_height = terrain.m_height;
        replacement.m_environment.m_terrain_origin_x = terrain.m_origin_x;
        replacement.m_environment.m_terrain_origin_y = terrain.m_origin_y;
        replacement.m_environment.m_has_terrain = terrain_present ? 1u : 0u;
        // Canonical masks and this existence witness publish/cancel together.
        replacement.m_environment.m_reserved =
            std::ranges::any_of(packed_terrain->m_boundary_tiles, [](const u32 word) { return word != 0u; }) ? 1u : 0u;

        // Named-field packing is the only crossing between the semantic record and
        // the split device rows; all six compact cold fields remain FP32.
        Array<ParticleWrite> hot_rows;
        Array<ParticleReadonly> cold_rows;
        if (not particles.empty()) {
            hot_rows.resize(particles.size());
            cold_rows.resize(particles.size());
            for (std::size_t row = 0u; row < particles.size(); ++row) {
                hot_rows[row] = packWrite(particles[row]);
                cold_rows[row] = packReadonly(particles[row]);
            }
        }

        const ParticleWrite empty_hot{};
        const ParticleReadonly empty_cold{};
        const GpuStaticShape empty_shape{};
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           *state.m_device, replacement.m_hot_bytes, sizeof(ParticleWrite),
                           append             ? nullptr
                           : hot_rows.empty() ? &empty_hot
                                              : hot_rows.data(),
                           "PBMPM committed hot rows", replacement.m_committed_hot));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           *state.m_device, replacement.m_cold_bytes, sizeof(ParticleReadonly),
                           append              ? nullptr
                           : cold_rows.empty() ? &empty_cold
                                               : cold_rows.data(),
                           "PBMPM committed cold rows", replacement.m_committed_cold));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           *state.m_device, replacement.m_hot_bytes, sizeof(ParticleWrite), nullptr,
                           "PBMPM working hot rows", replacement.m_working_hot));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           *state.m_device, replacement.m_cold_bytes, sizeof(ParticleReadonly), nullptr,
                           "PBMPM working cold rows", replacement.m_working_cold));
        if (append) {
            replacement.m_shapes = state.m_particles.m_shapes;
            replacement.m_shape_count = state.m_particles.m_shape_count;
        } else {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu,
                createBuffer(
                    *state.m_device, std::max<u64>(1u, shapes.size()) * sizeof(GpuStaticShape), sizeof(GpuStaticShape),
                    shapes.empty() ? &empty_shape : shapes.data(), "PBMPM static shapes", replacement.m_shapes));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, createBuffer(
                           *state.m_device, replacement.m_hot_bytes + replacement.m_cold_bytes + kErrorBytes, 0u,
                           nullptr, "PBMPM tick readback", replacement.m_readback, true));

        if (terrain_present) {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, createBuffer(
                               *state.m_device, packed_terrain->m_masks.size() * sizeof(u32), sizeof(u32),
                               packed_terrain->m_masks.data(), "PBMPM contour cell bins", replacement.m_terrain_masks));
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, createBuffer(
                               *state.m_device, packed_terrain->m_boundary_tiles.size() * sizeof(u32), sizeof(u32),
                               packed_terrain->m_boundary_tiles.data(), "PBMPM contour boundary tiles",
                               replacement.m_terrain_boundary_tiles));
        }

        const u64 row_bytes = sizeof(ParticleWrite) + sizeof(ParticleReadonly);
        state.m_accounting.m_last_particle_upload_bytes = particles.size() * row_bytes;
        state.m_accounting.m_last_particle_copy_bytes = static_cast<u64>(old_count) * row_bytes;
        if (not append) {
            state.m_replacement = std::move(replacement);
            return {};
        }

        // Finish all fallible local preparation before admitting a frozen GPU
        // cohort. Its old prefix stays resident; only the deterministic new
        // suffix crosses the CPU boundary.
        rhi::ComPtr<rhi::IBuffer> appended_upload_hot;
        rhi::ComPtr<rhi::IBuffer> appended_upload_cold;
        if (not particles.empty()) {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, createBuffer(
                               *state.m_device, particles.size() * sizeof(ParticleWrite), sizeof(ParticleWrite),
                               hot_rows.data(), "PBMPM appended hot rows", appended_upload_hot));
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, createBuffer(
                               *state.m_device, particles.size() * sizeof(ParticleReadonly), sizeof(ParticleReadonly),
                               cold_rows.data(), "PBMPM appended cold rows", appended_upload_cold));
        }

        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.m_queue->createCommandEncoder(encoder.writeRef()));
        auto *const hot_destination = replacement.m_committed_hot.get();
        auto *const cold_destination = replacement.m_committed_cold.get();
        encoder->setBufferState(hot_destination, rhi::ResourceState::CopyDestination);
        encoder->setBufferState(cold_destination, rhi::ResourceState::CopyDestination);
        const u64 old_hot_bytes = static_cast<u64>(old_count) * sizeof(ParticleWrite);
        const u64 old_cold_bytes = static_cast<u64>(old_count) * sizeof(ParticleReadonly);
        if (old_hot_bytes != 0u) {
            encoder->setBufferState(state.m_particles.m_committed_hot, rhi::ResourceState::CopySource);
            encoder->setBufferState(state.m_particles.m_committed_cold, rhi::ResourceState::CopySource);
            encoder->copyBuffer(hot_destination, 0u, state.m_particles.m_committed_hot, 0u, old_hot_bytes);
            encoder->copyBuffer(cold_destination, 0u, state.m_particles.m_committed_cold, 0u, old_cold_bytes);
        }

        if (not particles.empty()) {
            encoder->setBufferState(appended_upload_hot, rhi::ResourceState::CopySource);
            encoder->setBufferState(appended_upload_cold, rhi::ResourceState::CopySource);
            encoder->copyBuffer(
                hot_destination, old_hot_bytes, appended_upload_hot, 0u, particles.size() * sizeof(ParticleWrite));
            encoder->copyBuffer(
                cold_destination, old_cold_bytes, appended_upload_cold, 0u,
                particles.size() * sizeof(ParticleReadonly));
        }

        rhi::ComPtr<rhi::ICommandBuffer> prepared_commands;
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, encoder->finish(prepared_commands.writeRef()));

        state.m_replacement = std::move(replacement);
        state.m_append_upload_hot = std::move(appended_upload_hot);
        state.m_append_upload_cold = std::move(appended_upload_cold);
        state.m_commands = std::move(prepared_commands);
        rhi::ICommandBuffer *commands[]{state.m_commands};
        rhi::IFence *fences[]{state.m_fence};
        state.m_replacement_signal = state.m_next_signal++;
        rhi::SubmitDesc submit{};
        submit.commandBuffers = commands;
        submit.commandBufferCount = 1u;
        submit.signalFences = fences;
        submit.signalFenceValues = &state.m_replacement_signal;
        submit.signalFenceCount = 1u;
        state.m_failure = {};
        state.m_inflight = true;
        if (const std::error_code error = make_error_code(state.m_queue->submit(submit))) {
            state.m_failure.m_cause = error;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, error);
        }

        return {};
    }

    Expected<bool> GpuSolver::pollReplacement() {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, m_state and m_state->m_replacement ? std::error_code{}
                                                          : std::make_error_code(std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        if (state.m_replacement_signal == 0u) {
            return true;
        }

        u64 completed{};
        const std::error_code error = make_error_code(state.m_fence->getCurrentValue(&completed));
        if (error or completed == std::numeric_limits<u64>::max()) {
            state.m_failure.m_cause = error ? error : std::make_error_code(std::errc::no_such_device_or_address);
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        }

        if (completed < state.m_replacement_signal) {
            return false;
        }

        state.m_inflight = false;
        state.m_replacement_signal = 0u;
        state.m_append_upload_hot.setNull();
        state.m_append_upload_cold.setNull();
        state.m_commands.setNull();
        state.m_dispatch_roots.clear();
        return true;
    }

    std::error_code GpuSolver::cancelPreparedReplacement() {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, not m_state or (m_state->m_pending_signal == 0u and m_state->m_snapshot_signal == 0u)
                           ? std::error_code{}
                           : std::make_error_code(std::errc::device_or_resource_busy));

        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, drain());
        if (m_state) {
            m_state->m_replacement_signal = 0u;
            m_state->m_append_upload_hot.setNull();
            m_state->m_append_upload_cold.setNull();
            m_state->m_commands.setNull();
            m_state->m_replacement.reset();
            m_state->m_failure = {};
        }

        return {};
    }

    void GpuSolver::publishReplacement() noexcept {
        PPR_ASSERT(
            m_state and
            m_state->m_replacement.has_value() and
            not m_state->m_inflight and
            m_state->m_replacement_signal == 0u);
        State &state = *m_state;
        state.m_particles = std::move(*state.m_replacement);
        state.m_replacement.reset();
        state.m_profile.m_particle_count = state.m_particles.m_particle_count;
        state.m_profile.m_shape_count = state.m_particles.m_shape_count;
        state.m_boundaries.m_body_count = 0u;
        ++state.m_accounting.m_epoch;
        state.m_live_epoch->store(state.m_accounting.m_epoch, std::memory_order_release);
    }

    void GpuSolver::cancelReplacement() noexcept {
        PPR_ASSERT(not m_state or m_state->m_replacement_signal == 0u);
        if (m_state)
            m_state->m_replacement.reset();
    }

    // ------------------------------------------------------------------
    // asynchronous working tick and completion proof
    // ------------------------------------------------------------------

    std::error_code GpuSolver::submitResidentTick(
        const u64 expected_epoch, const u32 substeps, const u32 iterations,
        const std::span<const GpuBodyBoundary> predicted_boundaries, const u32 body_count) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            m_state and not busy() ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            expected_epoch == state.m_accounting.m_epoch and expected_epoch != std::numeric_limits<u64>::max()
                ? std::error_code{}
                : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, substeps != 0u and
                               iterations == kSourceIterations and
                               substeps <= std::numeric_limits<u32>::max() / 2u and
                               state.m_next_signal != std::numeric_limits<u64>::max()
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        const u64 boundary_rows = static_cast<u64>(body_count) * substeps;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, boundary_rows == predicted_boundaries.size() and boundary_rows <= std::numeric_limits<u32>::max()
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));
        for (const GpuBodyBoundary &boundary: predicted_boundaries) {
            const auto &shape = boundary.m_shape;
            const bool valid = (shape.m_shape == EStaticShape::box or shape.m_shape == EStaticShape::circle) and
                               std::isfinite(shape.m_position_x) and
                               std::isfinite(shape.m_position_y) and
                               std::isfinite(shape.m_rotation_degrees) and
                               std::isfinite(shape.m_half_size_x) and
                               std::isfinite(shape.m_half_size_y) and
                               std::isfinite(shape.m_radius) and
                               shape.m_half_size_x >= 0.0f and
                               shape.m_half_size_y >= 0.0f and
                               shape.m_radius >= 0.0f and
                               std::isfinite(boundary.m_com_x) and
                               std::isfinite(boundary.m_com_y) and
                               std::isfinite(boundary.m_velocity_x) and
                               std::isfinite(boundary.m_velocity_y) and
                               std::isfinite(boundary.m_angular_velocity);
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, valid ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        }

        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.ensureBoundaries(static_cast<u32>(boundary_rows)));
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, state.m_particles.m_sparse.canAdmit(state.m_profile.m_particle_count)
                           ? std::error_code{}
                           : std::make_error_code(std::errc::value_too_large));

        Array<rhi::ComPtr<rhi::IShaderObject>> dispatch_roots;
        dispatch_roots.resize(static_cast<std::size_t>(substeps) * 2u);

        for (std::size_t index = 0u; index < dispatch_roots.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                MatterGpu, state.m_device->createRootShaderObject(
                               state.m_kernels[index % 2u].m_program, dispatch_roots[index].writeRef()));
        }

        // Every fused dispatch owns immutable selector/body-base constants.
        state.m_dispatch_roots = std::move(dispatch_roots);
        PPR_ASSERT(state.m_dispatch_roots.size() == static_cast<std::size_t>(substeps) * 2u);

        state.m_boundaries.m_body_count = body_count;
        state.m_accounting.m_substeps = substeps;

        const hal::ProfileScope scope{"Matter.Gpu.EncodeTick"};
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        if (const auto error = make_error_code(state.m_queue->createCommandEncoder(encoder.writeRef()))) {
            state.m_dispatch_roots.clear();
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, error);
        }

        auto *const timing = state.m_timing.get();

        auto encode_error = [&]() -> std::error_code {
            {
                encoder->pushDebugGroup("PBMPM.Tick", {});
                PPR_DEFER {
                    encoder->popDebugGroup();
                };

                if (timing) {
                    if (timing->m_pending.m_signal != 0u and not timing->m_error) {
                        u64 completed{};
                        const auto before = timing->m_clock->now();
                        const auto error = make_error_code(state.m_fence->getCurrentValue(&completed));
                        const auto after = timing->m_clock->now();
                        if (error or completed == std::numeric_limits<u64>::max())
                            timing->fail(error ? error : std::make_error_code(std::errc::no_such_device_or_address));
                        else {
                            timing->observePoll(before, after, completed);
                            timing->collect(completed);
                        }
                    }

                    timing->begin(*encoder, expected_epoch, substeps, state.m_profile.m_particle_count);
                }

                encoder->setBufferState(state.m_particles.m_committed_hot, rhi::ResourceState::CopySource);
                encoder->setBufferState(state.m_particles.m_committed_cold, rhi::ResourceState::CopySource);
                encoder->setBufferState(state.m_particles.m_working_hot, rhi::ResourceState::CopyDestination);
                encoder->setBufferState(state.m_particles.m_working_cold, rhi::ResourceState::CopyDestination);
                encoder->copyBuffer(
                    state.m_particles.m_working_hot, 0u, state.m_particles.m_committed_hot, 0u,
                    state.m_particles.m_hot_bytes);
                encoder->copyBuffer(
                    state.m_particles.m_working_cold, 0u, state.m_particles.m_committed_cold, 0u,
                    state.m_particles.m_cold_bytes);
                state.m_accounting.m_last_particle_upload_bytes = 0u;
                state.m_accounting.m_last_particle_copy_bytes = static_cast<u64>(state.m_profile.m_particle_count) *
                                                                (sizeof(ParticleWrite) + sizeof(ParticleReadonly));
                if (timing)
                    timing->mark(*encoder);

                encoder->setBufferState(state.m_particles.m_working_hot, rhi::ResourceState::UnorderedAccess);
                // The cold row is only ever read by the kernels, so it holds a
                // shader-resource state rather than the hot row's unordered access.
                encoder->setBufferState(state.m_particles.m_working_cold, rhi::ResourceState::ShaderResource);
                encoder->setBufferState(state.m_particles.m_shapes, rhi::ResourceState::ShaderResource);
                if (state.m_particles.m_terrain_masks) {
                    encoder->setBufferState(state.m_particles.m_terrain_masks, rhi::ResourceState::ShaderResource);
                    encoder->setBufferState(
                        state.m_particles.m_terrain_boundary_tiles, rhi::ResourceState::ShaderResource);
                }

                encoder->setBufferState(state.m_contour_points, rhi::ResourceState::ShaderResource);
                encoder->setBufferState(state.m_contour_cases, rhi::ResourceState::ShaderResource);
                encoder->setBufferState(state.m_empty_terrain, rhi::ResourceState::ShaderResource);
                if (not predicted_boundaries.empty()) {
                    encoder->setBufferState(state.m_boundaries.m_bodies, rhi::ResourceState::CopyDestination);
                    PPR_RETURN_ERROR_ON_FAIL(
                        MatterGpu, encoder->uploadBufferData(
                                       state.m_boundaries.m_bodies, 0u, predicted_boundaries.size_bytes(),
                                       predicted_boundaries.data()));
                }

                encoder->setBufferState(state.m_boundaries.m_bodies, rhi::ResourceState::ShaderResource);
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.m_particles.m_sparse.beginTick(*encoder, substeps));
                state.m_sparse_encoding_open = true;
                if (timing)
                    timing->mark(*encoder);

                for (u32 substep = 0u; substep < substeps; ++substep) {
                    encoder->pushDebugGroup("PBMPM.Substep", {});
                    PPR_DEFER {
                        encoder->popDebugGroup();
                    };

                    state.m_substep = substep;
                    {
                        encoder->pushDebugGroup("PBMPM.Sparse.Discovery", {});
                        PPR_DEFER {
                            encoder->popDebugGroup();
                        };

                        PPR_RETURN_ERROR_ON_FAIL(
                            MatterGpu, state.m_particles.m_sparse.discover(
                                           *encoder, *state.m_particles.m_working_hot,
                                           *state.m_particles.m_working_cold, state.m_profile, state.m_dispatch_roots));
                        state.m_accounting.m_dispatches += 9u;
                        if (timing)
                            timing->mark(*encoder);
                    }

                    for (u32 iteration = 0u; iteration < 2u; ++iteration) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            MatterGpu, state.m_particles.m_sparse.initializeDestination(
                                           *encoder, iteration != 0u, state.m_dispatch_roots));
                        ++state.m_accounting.m_dispatches;
                        if (timing)
                            timing->mark(*encoder);

                        encoder->pushDebugGroup(iteration == 0u ? "PBMPM.Fused0" : "PBMPM.Fused1", {});
                        PPR_DEFER {
                            encoder->popDebugGroup();
                        };

                        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.bindAndDispatch(*encoder, iteration));
                        if (timing)
                            timing->mark(*encoder);
                    }
                }

                {
                    encoder->pushDebugGroup("PBMPM.StatusCopy", {});
                    PPR_DEFER {
                        encoder->popDebugGroup();
                    };

                    encoder->setBufferState(
                        state.m_particles.m_sparse.buffers().m_errors, rhi::ResourceState::CopySource);
                    encoder->setBufferState(state.m_particles.m_readback, rhi::ResourceState::CopyDestination);
                    encoder->copyBuffer(
                        state.m_particles.m_readback, 0u, state.m_particles.m_sparse.buffers().m_errors, 0u,
                        kErrorBytes);
                    if (timing)
                        timing->mark(*encoder);
                }
            } // All debug groups close before finishing the encoder.

            {
                const hal::ProfileScope finish_scope{"Matter.Gpu.FinishCommands"};
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, encoder->finish(state.m_commands.writeRef()));
            }

            return {};
        }();
        if (encode_error) {
            // No queue handoff occurred: destroy recording references before
            // classifying this as definite abandonment, never as execution.
            encoder.setNull();
            state.m_commands.setNull();
            if (state.m_sparse_encoding_open) {
                const auto settlement = state.m_particles.m_sparse.settleEncoding(
                    SparseEncodingDisposition::not_submitted);
                if (not settlement)
                    state.m_sparse_encoding_open = false;
                PPR_RETAIN_ERROR_ON_FAIL(MatterGpu, encode_error, settlement);
            }

            if (not state.m_sparse_encoding_open)
                state.m_dispatch_roots.clear();

            if (timing)
                timing->m_recording = false;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, encode_error);
        }

        rhi::ICommandBuffer *commands[]{state.m_commands};
        rhi::IFence *fences[]{state.m_fence};
        state.m_pending_signal = state.m_next_signal++;
        rhi::SubmitDesc submit{};
        submit.commandBuffers = commands;
        submit.commandBufferCount = 1u;
        submit.signalFences = fences;
        submit.signalFenceValues = &state.m_pending_signal;
        submit.signalFenceCount = 1u;
        state.m_failure = {};
        // Even an unsuccessful submit may have handed work to the device.
        state.m_inflight = true;

        const bool observed = timing and timing->m_recording;
        if (observed) {
            PPR_ASSERT(timing->m_written == timing->m_expected);
            timing->m_pending.m_signal = state.m_pending_signal;
            timing->m_pending.m_submit_begin = timing->m_clock->now();
        }

        {
            const hal::ProfileScope submit_scope{"Matter.Gpu.SubmitCommands"};
            const auto error = make_error_code(state.m_queue->submit(submit));
            if (observed) {
                timing->m_pending.m_submit_end = timing->m_clock->now();
                timing->m_recording = false;
                if (error)
                    timing->fail(error); // Retain the possibly submitted query range.
            }

            if (error) {
                state.m_failure.m_cause = error;
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, error);
            }
        }

        return {};
    }

    Expected<bool> GpuSolver::pollTick() {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, m_state and m_state->m_pending_signal != 0u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        if (state.m_complete)
            return true;

        u64 completed{};
        auto *const timing = state.m_timing.get();
        const auto poll_begin = timing and not timing->m_error ? timing->m_clock->now() : TimePoint{};
        if (const std::error_code error = make_error_code(state.m_fence->getCurrentValue(&completed))) {
            if (timing)
                timing->fail(error);

            state.m_failure.m_cause = error;
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, error);
        }

        if (completed == std::numeric_limits<u64>::max()) {
            if (timing)
                timing->fail(std::make_error_code(std::errc::no_such_device_or_address));

            state.m_failure.m_cause = std::make_error_code(std::errc::no_such_device_or_address);
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        }

        if (timing and not timing->m_error) {
            timing->observePoll(poll_begin, timing->m_clock->now(), completed);
            timing->collect(completed);
        }

        if (completed < state.m_pending_signal)
            return false;

        const hal::ProfileScope scope{"Matter.Gpu.CollectTick"};
        if (state.m_sparse_encoding_open) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                MatterGpu, state.m_particles.m_sparse.settleEncoding(SparseEncodingDisposition::executed));
            state.m_sparse_encoding_open = false;
        }
        state.m_inflight = false;

        void *mapped{};
        const std::error_code map_error = state.m_injected_collection_error
                                              ? state.m_injected_collection_error
                                              : make_error_code(state.m_device->mapBuffer(
                                                    state.m_particles.m_readback, rhi::CpuAccessMode::Read, &mapped));
        if (map_error) {
            state.m_failure.m_cause = map_error;
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, map_error);
        }

        state.m_readback_mapped = true;
        std::array<u32, 6u> errors{};
        std::memcpy(errors.data(), mapped, kErrorBytes);

        if (const std::error_code error = make_error_code(state.m_device->unmapBuffer(state.m_particles.m_readback))) {
            state.m_failure.m_cause = error;
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, error);
        }

        state.m_readback_mapped = false;
        state.m_accounting.m_last_readback_bytes = kErrorBytes;
        state.m_failure.m_flags = errors[0];
        if (errors[1] != 0u) {
            state.m_failure.m_node = errors[2];
            state.m_failure.m_field = errors[3];
            state.m_failure.m_particle = errors[4];
            state.m_failure.m_body = errors[5];
        }

        state.m_valid = errors[0] == 0u;
        if (not state.m_valid) {
            state.m_failure.m_cause = std::make_error_code(std::errc::result_out_of_range);

            PPR_LOG(
                MatterGpu, error, "GPU tick candidate rejected",
                {
                    {"flags", errors[0]},
                    {"claimed", errors[1]},
                    {"node", errors[2]},
                    {"field", errors[3]},
                    {"particle_row", errors[4]},
                    {"body", errors[5]},
                    {"epoch", state.m_accounting.m_epoch},
                    {"particle_count", state.m_profile.m_particle_count},
                    {"grid_width", state.m_profile.m_grid_width},
                    {"grid_height", state.m_profile.m_grid_height},
                    {"fixed_point_multiplier", state.m_profile.m_fixed_point_multiplier},
                });

            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        }

        state.m_complete = true;
        return true;
    }

    void GpuSolver::publishTick() noexcept {
        PPR_ASSERT(
            m_state and
            m_state->m_complete and
            m_state->m_valid and
            not m_state->m_inflight and
            not m_state->m_sparse_encoding_open);
        State &state = *m_state;
        std::swap(state.m_particles.m_committed_hot, state.m_particles.m_working_hot);
        std::swap(state.m_particles.m_committed_cold, state.m_particles.m_working_cold);
        ++state.m_accounting.m_epoch;
        state.m_live_epoch->store(state.m_accounting.m_epoch, std::memory_order_release);

        state.m_complete = false;
        state.m_valid = false;
        state.m_pending_signal = 0u;
        state.m_commands.setNull();
        state.m_dispatch_roots.clear();
    }

    std::error_code GpuSolver::discardTick() {
        PPR_ASSERT(m_state and not m_state->m_inflight);
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, m_state->m_snapshot_signal == 0u and not m_state->m_sparse_encoding_open
                           ? std::error_code{}
                           : std::make_error_code(std::errc::device_or_resource_busy));

        if (m_state->m_readback_mapped) {
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_state->m_device->unmapBuffer(m_state->m_particles.m_readback));
            m_state->m_readback_mapped = false;
        }

        m_state->m_complete = false;
        m_state->m_valid = false;
        m_state->m_pending_signal = 0u;
        m_state->m_commands.setNull();
        m_state->m_dispatch_roots.clear();
        return {};
    }

    // ------------------------------------------------------------------
    // committed inspection and safe teardown
    // ------------------------------------------------------------------

    std::error_code GpuSolver::beginSnapshot(const u64 expected_epoch) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu,
            m_state and not busy() ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, expected_epoch == state.m_accounting.m_epoch
                           ? std::error_code{}
                           : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, state.m_next_signal != std::numeric_limits<u64>::max()
                           ? std::error_code{}
                           : std::make_error_code(std::errc::value_too_large));

        const hal::ProfileScope scope{"Matter.Gpu.SnapshotSubmit"};
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, state.m_queue->createCommandEncoder(encoder.writeRef()));
        encoder->setBufferState(state.m_particles.m_committed_hot, rhi::ResourceState::CopySource);
        encoder->setBufferState(state.m_particles.m_committed_cold, rhi::ResourceState::CopySource);
        encoder->setBufferState(state.m_particles.m_readback, rhi::ResourceState::CopyDestination);
        encoder->copyBuffer(
            state.m_particles.m_readback, 0u, state.m_particles.m_committed_hot, 0u, state.m_particles.m_hot_bytes);
        encoder->copyBuffer(
            state.m_particles.m_readback, state.m_particles.m_hot_bytes, state.m_particles.m_committed_cold, 0u,
            state.m_particles.m_cold_bytes);
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, encoder->finish(state.m_commands.writeRef()));

        rhi::ICommandBuffer *commands[]{state.m_commands};
        rhi::IFence *fences[]{state.m_fence};
        state.m_snapshot_signal = state.m_next_signal++;
        rhi::SubmitDesc submit{};
        submit.commandBuffers = commands;
        submit.commandBufferCount = 1u;
        submit.signalFences = fences;
        submit.signalFenceValues = &state.m_snapshot_signal;
        submit.signalFenceCount = 1u;
        state.m_failure = {};
        state.m_inflight = true;

        if (const std::error_code error = make_error_code(state.m_queue->submit(submit))) {
            state.m_failure.m_cause = error;
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, error);
        }

        return {};
    }

    Expected<std::optional<GpuSnapshot>> GpuSolver::pollSnapshot() {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, m_state and m_state->m_snapshot_signal != 0u
                           ? std::error_code{}
                           : std::make_error_code(std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);

        u64 completed{};
        const std::error_code fence_error = make_error_code(state.m_fence->getCurrentValue(&completed));
        if (fence_error or completed == std::numeric_limits<u64>::max()) {
            state.m_failure.m_cause = fence_error ? fence_error
                                                  : std::make_error_code(std::errc::no_such_device_or_address);
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, state.m_failure.m_cause);
        }

        if (completed < state.m_snapshot_signal) {
            return std::optional<GpuSnapshot>{};
        }

        const hal::ProfileScope scope{"Matter.Gpu.SnapshotCollect"};
        state.m_inflight = false;
        GpuSnapshot snapshot{};
        snapshot.m_epoch = state.m_accounting.m_epoch;
        snapshot.m_particles.resize(state.m_profile.m_particle_count);
        void *mapped{};
        const std::error_code map_error = state.m_injected_collection_error
                                              ? state.m_injected_collection_error
                                              : make_error_code(state.m_device->mapBuffer(
                                                    state.m_particles.m_readback, rhi::CpuAccessMode::Read, &mapped));
        if (map_error) {
            state.m_failure.m_cause = map_error;
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, map_error);
        }

        state.m_readback_mapped = true;
        const u64 hot_bytes = static_cast<u64>(snapshot.m_particles.size()) * sizeof(ParticleWrite);
        const u64 cold_bytes = static_cast<u64>(snapshot.m_particles.size()) * sizeof(ParticleReadonly);
        if (hot_bytes != 0u) {
            const auto *const hot = static_cast<const ParticleWrite *>(mapped);
            const auto *const cold = reinterpret_cast<const ParticleReadonly *>(
                static_cast<const std::byte *>(mapped) + hot_bytes);
            for (std::size_t row = 0u; row < snapshot.m_particles.size(); ++row) {
                snapshot.m_particles[row] = unpack(hot[row], cold[row]);
            }
        }

        const std::error_code unmap_error = make_error_code(state.m_device->unmapBuffer(state.m_particles.m_readback));
        if (unmap_error) {
            state.m_failure.m_cause = unmap_error;
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, unmap_error);
        }

        state.m_readback_mapped = false;
        state.m_accounting.m_last_readback_bytes = hot_bytes + cold_bytes;
        state.m_snapshot_signal = 0u;
        state.m_commands.setNull();
        state.m_dispatch_roots.clear();
        return std::optional<GpuSnapshot>{std::move(snapshot)};
    }

    std::error_code GpuSolver::cancelSnapshot() {
        if (not m_state or m_state->m_snapshot_signal == 0u) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, drain());
        if (m_state->m_readback_mapped) {
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_state->m_device->unmapBuffer(m_state->m_particles.m_readback));
            m_state->m_readback_mapped = false;
        }

        m_state->m_snapshot_signal = 0u;
        m_state->m_commands.setNull();
        return {};
    }

    GpuParameters GpuSolver::profile() const noexcept {
        return m_state ? m_state->m_profile : GpuParameters{};
    }

    GpuAccounting GpuSolver::accounting() const noexcept {
        if (not m_state)
            return {};

        GpuAccounting result = m_state->m_accounting;
        const ParticleStorage &particles = m_state->m_particles;
        result.m_device_bytes += 2u * (particles.m_hot_bytes + particles.m_cold_bytes);
        result.m_device_bytes += particles.m_shapes ? particles.m_shapes->getDesc().size : 0u;
        result.m_device_bytes += particles.m_sparse.capacity().m_lookup_bytes +
                                 particles.m_sparse.capacity().m_dynamic_bytes;
        result.m_readback_capacity_bytes = particles.m_hot_bytes + particles.m_cold_bytes + kErrorBytes;
        if (particles.m_terrain_masks)
            result.m_device_bytes += particles.m_terrain_masks->getDesc().size;

        if (particles.m_terrain_boundary_tiles)
            result.m_device_bytes += particles.m_terrain_boundary_tiles->getDesc().size;

        if (m_state->m_replacement) {
            const ParticleStorage &replacement = *m_state->m_replacement;
            result.m_device_bytes += 2u * (replacement.m_hot_bytes + replacement.m_cold_bytes);
            result.m_device_bytes += replacement.m_sparse.capacity().m_lookup_bytes +
                                     replacement.m_sparse.capacity().m_dynamic_bytes;
            if (replacement.m_shapes != particles.m_shapes) {
                result.m_device_bytes += replacement.m_shapes->getDesc().size;
            }

            result.m_readback_capacity_bytes += replacement.m_hot_bytes + replacement.m_cold_bytes + kErrorBytes;
            if (replacement.m_terrain_masks)
                result.m_device_bytes += replacement.m_terrain_masks->getDesc().size;

            if (replacement.m_terrain_boundary_tiles)
                result.m_device_bytes += replacement.m_terrain_boundary_tiles->getDesc().size;
        }

        if (m_state->m_append_upload_hot) {
            result.m_device_bytes += m_state->m_append_upload_hot->getDesc().size;
        }

        if (m_state->m_append_upload_cold) {
            result.m_device_bytes += m_state->m_append_upload_cold->getDesc().size;
        }

        const BoundaryStorage &boundaries = m_state->m_boundaries;
        result.m_device_bytes += static_cast<u64>(boundaries.m_row_capacity) * sizeof(GpuBodyBoundary);

        result.m_particle_count = m_state->m_profile.m_particle_count;
        result.m_shape_count = m_state->m_profile.m_shape_count;
        result.m_body_count = boundaries.m_body_count;
        return result;
    }

    const GpuFailure &GpuSolver::failure() const noexcept {
        PPR_ASSERT(m_state != nullptr);
        return m_state->m_failure;
    }

    bool GpuSolver::busy() const noexcept {
        return m_state and (m_state->m_pending_signal != 0u or
                               m_state->m_snapshot_signal != 0u or
                               m_state->m_replacement.has_value() or
                               m_state->m_sparse_encoding_open);
    }

    void GpuSolver::injectCollectionFailureForTest(const std::error_code cause) noexcept {
        PPR_ASSERT(m_state != nullptr);
        m_state->m_injected_collection_error = cause;
    }

    GpuTimingInspection GpuSolver::inspectTimingForTest() const noexcept {
        if (not m_state or not m_state->m_timing)
            return {};

        const auto &timing = *m_state->m_timing;
        return {
            .m_error = timing.m_error,
            .m_completed_samples = timing.m_samples,
            .m_epoch = timing.m_last_epoch,
            .m_signal = timing.m_last_signal,
            .m_first_gpu_timestamp = timing.m_last_first_gpu,
            .m_last_gpu_timestamp = timing.m_last_gpu,
            .m_query_count = timing.m_queries ? kTimingQueries : 0u};
    }

    std::error_code GpuSolver::drain() {
        if (not m_state)
            return {};

        if (m_state->m_inflight) {
            const hal::ProfileScope scope{"Matter.Gpu.Drain", hal::profile_blocking_category};
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_state->m_queue->waitOnHost());
            // Queue idleness alone cannot classify an ambiguous failed submit.
            // Sparse generations become reusable only after this tick's fence.
            if (m_state->m_sparse_encoding_open) {
                u64 completed{};
                PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_state->m_fence->getCurrentValue(&completed));
                PPR_RETURN_ERROR_ON_FAIL(
                    MatterGpu, completed != std::numeric_limits<u64>::max() and
                                       m_state->m_pending_signal != 0u and
                                       completed >= m_state->m_pending_signal
                                   ? std::error_code{}
                                   : std::make_error_code(std::errc::state_not_recoverable));
                PPR_RETURN_ERROR_ON_FAIL(
                    MatterGpu, m_state->m_particles.m_sparse.settleEncoding(SparseEncodingDisposition::executed));
                m_state->m_sparse_encoding_open = false;
            }

            m_state->m_inflight = false;
            if (m_state->m_timing and m_state->m_timing->m_pending.m_signal != 0u)
                m_state->m_timing->m_pending.m_host_drain = true;
        }

        PPR_RETURN_ERROR_ON_FAIL(
            MatterGpu, not m_state->m_sparse_encoding_open ? std::error_code{}
                                                           : std::make_error_code(std::errc::operation_in_progress));

        // Query readiness is still checked independently after this existing wait.
        // Diagnostic collection never adds a wait or invents fence completion.
        auto *const timing = m_state->m_timing.get();
        if (timing and timing->m_pending.m_signal != 0u and not timing->m_error) {
            u64 completed{};
            const auto before = timing->m_clock->now();
            const auto error = make_error_code(m_state->m_fence->getCurrentValue(&completed));
            const auto after = timing->m_clock->now();
            if (error or completed == std::numeric_limits<u64>::max())
                timing->fail(error ? error : std::make_error_code(std::errc::no_such_device_or_address));
            else {
                timing->observePoll(before, after, completed);
                timing->collect(completed);
            }
        }

        return {};
    }

    Expected<GpuCommittedView> GpuSolver::committedView() const {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterGpu, m_state ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterGpu, m_state->m_failure.m_cause);

        return GpuCommittedView{
            m_state->m_particles.m_committed_hot, m_state->m_particles.m_committed_cold, m_state->m_live_epoch,
            m_state->m_accounting.m_epoch, m_state->m_profile.m_particle_count};
    }

    std::error_code GpuSolver::shutdown() {
        if (not m_state)
            return {};

        m_state->m_live_epoch->store(std::numeric_limits<u64>::max(), std::memory_order_release);
        PPR_RETURN_ERROR_ON_FAIL(MatterGpu, drain());
        if (m_state->m_readback_mapped) {
            PPR_RETURN_ERROR_ON_FAIL(MatterGpu, m_state->m_device->unmapBuffer(m_state->m_particles.m_readback));
            m_state->m_readback_mapped = false;
        }

        if (m_state->m_timing) {
            auto &timing = *m_state->m_timing;
            if (timing.m_pending.m_signal != 0u and not timing.m_error)
                timing.fail(std::make_error_code(std::errc::resource_unavailable_try_again));

            timing.report(*m_state->m_queue);
        }

        m_state.reset();
        return {};
    }
}
