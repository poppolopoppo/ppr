module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>
#include <slang.h>

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.app;
import engine.image;
import engine.matter;
import engine.shader;
import engine.rhi;
import game.colony.matter;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.digtool;
import game.colony.worldgen;
import game.colony.sim_packet;
import game.application;
import game.colony.panel;
import game.ui.model;
import std;

#include "Checkpoint.Tests.Support.h"

namespace pP::game::tests::detail::checkpoint {
    // ------------------------------------------------------------------
    // attached GPU edit and cold restore continuation
    // ------------------------------------------------------------------

    class GpuOwner {
        decltype(IShaderService::get()) m_shaders{IShaderService::get()};
        decltype(IRhiService::get()) m_rhi{IRhiService::get()};
        std::shared_ptr<const ContentCatalog> m_catalog;
        std::optional<WorldCheckpoint> m_before_admission;

        void rememberAdmission() {
            const auto before = captureWorldCheckpoint(*m_driver, m_driver->preparedWorld()->generated());
            PPR_TEST_ASSERT(before.has_value());

            m_before_admission = *before;
        }

    public:
        void reportFailure(const std::error_code error, const char *const operation) {
            if (not error or m_failure_reported)
                return;

            m_failure_reported = true;

            // Failure-only evidence precedes all fixture teardown. These are
            // reported fields, not a chronological stage or failed intermediates.
            const auto reported = m_runtime.failure();
            std::fprintf(
                stderr,
                "[checkpoint-gpu-failure] operation=%s error=%s:%d raw_claim=unavailable predicted_rows=unavailable failed_candidate=unavailable\n",
                operation, error.category().name(), error.value());
            if (reported) {
                std::fprintf(
                    stderr,
                    "[checkpoint-gpu-reported] cause=%s:%d flags=%08x node=%08x field=%08x particle=%08x body=%08x\n",
                    reported->m_cause.category().name(), reported->m_cause.value(),
                    static_cast<unsigned>(reported->m_flags), static_cast<unsigned>(reported->m_node),
                    static_cast<unsigned>(reported->m_field), static_cast<unsigned>(reported->m_particle),
                    static_cast<unsigned>(reported->m_body));
            } else {
                std::fprintf(
                    stderr, "[checkpoint-gpu-reported] accessor_error=%s:%d\n", reported.error().category().name(),
                    reported.error().value());
            }

            if (m_before_admission) {
                const auto &before = *m_before_admission;
                const auto &state = before.m_current_matter;
                std::fprintf(
                    stderr,
                    "[checkpoint-gpu-before-prefix] world=%llu publication=%llu terrain_revision=%llu tick=%llu next_id=%llu h=%.17g thickness=%.17g profile_words=",
                    static_cast<unsigned long long>(state.m_identity.m_world),
                    static_cast<unsigned long long>(state.m_identity.m_publication),
                    static_cast<unsigned long long>(state.m_identity.m_terrain_revision),
                    static_cast<unsigned long long>(before.m_tick), static_cast<unsigned long long>(state.m_next_id),
                    state.m_cell_metres, state.m_thickness_metres);
                for (const u32 word: std::bit_cast<std::array<u32, 16u>>(state.m_profile))
                    std::fprintf(stderr, "%08x ", static_cast<unsigned>(word));
                std::fprintf(stderr, "\n");

                for (std::size_t row = 0u; row < state.m_particles.size(); ++row) {
                    std::fprintf(
                        stderr, "[checkpoint-gpu-before-prefix-particle] id=%llu words=",
                        static_cast<unsigned long long>(state.m_records[row].m_id));
                    for (const u32 word: std::bit_cast<std::array<u32, 22u>>(state.m_particles[row]))
                        std::fprintf(stderr, "%08x ", static_cast<unsigned>(word));
                    std::fprintf(stderr, "\n");
                }

                // The v5 file includes exact physical ledger/gas, complete cells,
                // ECS body definitions/state, content identity and carried columns.
                // It is the before-prefix checkpoint, not a GPU candidate dump.
                const auto bytes = encodeWorldCheckpoint(before, *m_catalog);
                std::error_code artifact_error;
                const auto parent = std::filesystem::current_path(artifact_error);
                std::filesystem::path directory;
                if (bytes and not artifact_error) {
                    for (u32 ordinal = 0u; ordinal < 4096u; ++ordinal) {
                        auto candidate = parent / ("checkpoint-gpu-failure-" + std::to_string(ordinal));
                        if (std::filesystem::create_directory(candidate, artifact_error)) {
                            directory = std::move(candidate);
                            break;
                        }

                        if (artifact_error)
                            break;
                    }
                }

                if (not directory.empty()) {
                    std::ofstream stream{directory / "before-prefix-v5.bin", std::ios::binary};
                    stream.write(
                        reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
                    stream.close();
                    std::fprintf(
                        stderr,
                        "[checkpoint-gpu-artifact] fixture=checkpoint::Fixture path=%s bytes=%zu write_ok=%u terrain=before-prefix-v5-cells bodies=before-prefix-v5-ECS\n",
                        directory.string().c_str(), bytes->size(), static_cast<unsigned>(not stream.fail()));
                } else {
                    const auto cause = bytes ? artifact_error : bytes.error();
                    std::fprintf(
                        stderr, "[checkpoint-gpu-artifact] unavailable error=%s:%d directory_slots_exhausted=%u\n",
                        cause.category().name(), cause.value(), static_cast<unsigned>(bytes and not artifact_error));
                }
            }

            if (m_driver and m_driver->matterState() != nullptr) {
                const auto &published = *m_driver->matterState();
                std::fprintf(
                    stderr,
                    "[checkpoint-gpu-published] publication=%llu terrain_revision=%llu rows=%zu records=%zu gas=%zu next_id=%llu; "
                    "current published state, not a failed candidate or original intermediate; profile_words=",
                    static_cast<unsigned long long>(published.m_identity.m_publication),
                    static_cast<unsigned long long>(published.m_identity.m_terrain_revision),
                    published.m_particles.size(), published.m_records.size(), published.m_gas.size(),
                    static_cast<unsigned long long>(published.m_next_id));
                for (const u32 word: std::bit_cast<std::array<u32, 16u>>(published.m_profile))
                    std::fprintf(stderr, "%08x ", static_cast<unsigned>(word));
                std::fprintf(stderr, "\n");

                for (std::size_t row = 0u; row < published.m_particles.size(); ++row) {
                    const auto *const record = row < published.m_records.size() ? &published.m_records[row] : nullptr;
                    std::fprintf(
                        stderr, "[checkpoint-gpu-published-row] row=%zu id=%llu metadata_available=%u words=", row,
                        static_cast<unsigned long long>(record ? record->m_id : 0u),
                        static_cast<unsigned>(record != nullptr));
                    for (const u32 word: std::bit_cast<std::array<u32, 22u>>(published.m_particles[row]))
                        std::fprintf(stderr, "%08x ", static_cast<unsigned>(word));

                    std::fprintf(stderr, "\n");
                    if (record != nullptr) {
                        std::fprintf(
                            stderr,
                            "[checkpoint-gpu-published-ledger] row=%zu position_m=(%.17g,%.17g) V_m3=%.17g "
                            "mass_kg=%.17g temperature_K=%.17g density=%.17g material=%u phase=%u\n",
                            row, record->m_x_m, record->m_y_m, record->m_volume_m3, record->m_mass_kg,
                            record->m_temperature_k, record->m_bulk_density_kg_m3,
                            static_cast<unsigned>(record->m_material), static_cast<unsigned>(record->m_phase));
                    }
                }
            }

            const auto accounting = m_runtime.accounting();
            if (accounting) {
                std::fprintf(
                    stderr,
                    "[checkpoint-gpu-accounting] epoch=%llu dispatches=%llu device_bytes=%llu readback_capacity=%llu "
                    "last_readback=%llu particles=%u shapes=%u bodies=%u substeps=%u\n",
                    static_cast<unsigned long long>(accounting->m_epoch),
                    static_cast<unsigned long long>(accounting->m_dispatches),
                    static_cast<unsigned long long>(accounting->m_device_bytes),
                    static_cast<unsigned long long>(accounting->m_readback_capacity_bytes),
                    static_cast<unsigned long long>(accounting->m_last_readback_bytes),
                    static_cast<unsigned>(accounting->m_particle_count),
                    static_cast<unsigned>(accounting->m_shape_count), static_cast<unsigned>(accounting->m_body_count),
                    static_cast<unsigned>(accounting->m_substeps));
            } else {
                std::fprintf(
                    stderr, "[checkpoint-gpu-accounting] unavailable=%s:%d\n", accounting.error().category().name(),
                    accounting.error().value());
            }

            std::fflush(stderr);
        }

    public:
        // owned requests on the injected inline or dedicated lane:
        std::error_code completeOwned(Expected<MatterRequest> request) {
            PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, request);

            for (;;) {
                auto completion = m_runtime.advance(*request);
                PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, completion);
                if (not *completion) {
                    std::this_thread::yield();
                    continue;
                }

                PPR_TEST_ASSERT((**completion).m_execution.m_ran);
                PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, (**completion).m_execution.m_operation_error);
                PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, (**completion).m_execution.m_transport_error);
                return {};
            }
        }

    private:
        void requireSuccess(const std::error_code error, const char *const operation) {
            if (not error)
                return;

            reportFailure(error, operation);
            PPR_TEST_ASSERT(not error);
        }

    public:
        MatterRuntime m_runtime;
        std::unique_ptr<ColonyDriver> m_driver;

    private:
        // Borrowed execution dependency outlives inverse GPU fixture teardown.
        RenderLane *m_lane{};
        bool m_shaders_attempted{}, m_rhi_attempted{}, m_failure_reported{};

    public:
        ~GpuOwner() {
            if (m_driver) {
                for (;;) {
                    const auto detached = m_driver->advanceDetachMatter();
                    PPR_TEST_ASSERT(detached.has_value());
                    if (*detached) {
                        break;
                    }

                    std::this_thread::yield();
                }
            }

            const auto driver_error = m_driver ? m_driver->shutdown() : std::error_code{};
            const auto runtime_error = completeOwned(m_runtime.beginShutdown());
            if (runtime_error)
                hal::fatalExit("checkpoint-test-gpu-completion-unknown");

            const auto rhi_error = m_rhi_attempted ? m_rhi->shutdown() : std::error_code{};
            const auto shader_error = m_shaders_attempted ? m_shaders->shutdown() : std::error_code{};
            if (driver_error or rhi_error or shader_error)
                hal::fatalExit("checkpoint-test-dependent-cleanup-failure");
        }

        void initialize(RenderLane *const lane = nullptr) {
            m_lane = lane;

            m_shaders_attempted = true;
            PPR_TEST_ASSERT(not m_shaders->initialize());

            m_rhi->setCoreValidation(true);
            m_rhi->setGpuAssistedValidation(true);
            m_rhi_attempted = true;
            PPR_TEST_ASSERT(not m_rhi->initialize(rhi::DeviceType::D3D12, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == rhi::DeviceType::D3D12);

            PPR_TEST_ASSERT(not completeOwned(
                m_runtime.beginInitialize(lane, m_rhi->getDevice(), *m_shaders, std::filesystem::current_path())));
        }

        void restore(
            const std::shared_ptr<const ContentCatalog> &catalog, const WorldCheckpoint &checkpoint, const u64 world) {
            if (m_driver)
                PPR_TEST_ASSERT(not m_driver->shutdown());

            auto restored = restoreWorldCheckpoint(catalog, checkpoint, world);
            PPR_TEST_ASSERT(restored.has_value());
            m_driver = std::move(*restored);
            m_catalog = catalog;
            rememberAdmission();

            Array<std::array<u32, 22u>> words;
            words.reserve(m_driver->matterState()->m_particles.size());

            for (const auto &particle: m_driver->matterState()->m_particles)
                words.push_back(std::bit_cast<std::array<u32, 22u>>(particle));

            const auto identity = m_driver->matterState()->m_identity;

            for (;;) {
                const auto prepared = m_driver->advancePrepareMatter(m_runtime);
                PPR_TEST_ASSERT(prepared.has_value());
                if (*prepared) {
                    break;
                }

                std::this_thread::yield();
            }

            for (;;) {
                const auto attached = m_driver->advanceAttachMatter(m_runtime);
                PPR_TEST_ASSERT(attached.has_value());
                if (*attached) {
                    break;
                }

                std::this_thread::yield();
            }

            PPR_TEST_ASSERT(m_driver->matterState()->m_identity == identity);
            PPR_TEST_ASSERT(m_driver->matterState()->m_particles.size() == words.size());

            for (std::size_t row = 0u; row < words.size(); ++row)
                PPR_TEST_ASSERT(
                    std::bit_cast<std::array<u32, 22u>>(m_driver->matterState()->m_particles[row]) == words[row]);

            const auto accounting = m_runtime.accounting();
            PPR_TEST_ASSERT(accounting.has_value());
            std::fprintf(
                stderr,
                "[checkpoint-gpu] G=4103 substeps=8 K=2 device_bytes=%llu readback_capacity=%llu owner=%zu/%zu\n",
                static_cast<unsigned long long>(accounting->m_device_bytes),
                static_cast<unsigned long long>(accounting->m_readback_capacity_bytes), sizeof(GpuOwner),
                alignof(GpuOwner));
        }

        void completeTick() {
            PPR_TEST_ASSERT(m_driver->physicsPending());
            drain();
            update(TimeSpan{});
            PPR_TEST_ASSERT(not m_driver->physicsPending());

            const auto accounting = m_runtime.accounting();
            PPR_TEST_ASSERT(accounting and accounting->m_last_readback_bytes == 24u);
            PPR_TEST_ASSERT(accounting->m_last_particle_upload_bytes == 0u);
            PPR_TEST_ASSERT(not m_driver->particleSnapshotCurrent() and m_driver->matterState() == nullptr);
            const auto unavailable = m_driver->captureParticles();
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::resource_unavailable_try_again);

            captureCurrent();

            for (const auto &particle: m_driver->matterState()->m_particles) {
                for (const float field: std::bit_cast<std::array<float, 22u>>(particle))
                    PPR_TEST_ASSERT(std::isfinite(field));

                PPR_TEST_ASSERT(particle.m_liquid_density > 0.0f);
            }
        }

        void captureCurrent() {
            const auto tick = m_driver->tickCount();

            for (;;) {
                const auto ready = m_driver->advanceMatterSnapshot();
                PPR_TEST_ASSERT(ready.has_value());
                if (*ready) {
                    break;
                }

                drain();
                std::this_thread::yield();
            }

            PPR_TEST_ASSERT(m_driver->particleSnapshotCurrent());
            PPR_TEST_ASSERT(m_driver->particleSnapshotTick() == tick and m_driver->tickCount() == tick);
        }

        void update(const TimeSpan elapsed) {
            if (m_driver->captureReady() and m_driver->particleSnapshotCurrent())
                rememberAdmission();

            requireSuccess(m_driver->update(elapsed), "update/poll");

            while (not m_driver->physicsPending() and (m_driver->editPending() or m_driver->matterServicePending())) {
                if (not m_runtime.requestPending()) {
                    drain();
                }

                requireSuccess(m_driver->update(TimeSpan{}), "update/edit-snapshot");
                std::this_thread::yield();
            }

            // After the fixture has drained GPU work, zero elapsed time still
            // needs the dedicated publish ACK before the old assertion point.
            if (m_lane and elapsed == TimeSpan{}) {
                while (m_driver->physicsPending()) {
                    if (not m_runtime.requestPending()) {
                        drain();
                    }

                    requireSuccess(m_driver->update(TimeSpan{}), "update/owned-physics-ack");
                    std::this_thread::yield();
                }
            }
        }

        void step() {
            rememberAdmission();
            requireSuccess(m_driver->stepOne(), "stepOne");
        }

        void drain() {
            // Attached tick/snapshot requests can use ordinary Driver service.
            // Detached transition requests must be taken by their exact advance.
            while (m_runtime.requestPending()) {
                if (not m_driver or not m_driver->matterAttached()) {
                    std::this_thread::yield();
                    return;
                }

                requireSuccess(m_driver->update(TimeSpan{}), "update/pending-owned-request");
                std::this_thread::yield();
            }

            requireSuccess(completeOwned(m_runtime.beginDrain()), "drain");
        }
    };

    // ------------------------------------------------------------------
    // temporary full-frontier frozen-shader differential diagnostic
    // ------------------------------------------------------------------

    u64 pairedFingerprint(const std::span<const std::byte> bytes) noexcept {
        u64 hash = 14695981039346656037ull;

        for (const auto byte: bytes)
            hash = (hash ^ std::to_integer<u8>(byte)) * 1099511628211ull;

        return hash;
    }

    std::string pairedRead(const std::filesystem::path &path) {
        std::ifstream stream{path, std::ios::binary};
        PPR_TEST_ASSERT(stream.good());
        return {std::istreambuf_iterator<char>{stream}, {}};
    }

    void pairedWrite(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        PPR_TEST_ASSERT(not stream.fail());
    }

    class FrozenPairShader final : public IShaderService {
        IShaderService &m_service;
        std::filesystem::path m_source;
        std::string m_name;

    public:
        shader::IModule *m_loaded{}; // Session-owned, never released by this observer.

        FrozenPairShader(IShaderService &service, std::filesystem::path source, std::string name)
            : m_service(service), m_source(std::move(source)), m_name(std::move(name)) {
        }

        std::error_code initialize() override {
            return m_service.initialize();
        }

        std::error_code shutdown() override {
            return m_service.shutdown();
        }

        std::error_code setTargetFormat(const SlangCompileTarget target) override {
            return m_service.setTargetFormat(target);
        }

        shader::IGlobalSession *getGlobalSession() const noexcept override {
            return m_service.getGlobalSession();
        }

        std::error_code loadModuleFromFile(
            const std::filesystem::path &path, const char *name, shader::IModule **module) override {
            if (std::string_view{name} != "pbmpm_solver")
                return m_service.loadModuleFromFile(path, name, module);

            PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, m_service.loadModuleFromFile(m_source, m_name.c_str(), module));
            m_loaded = *module;
            return {};
        }

        std::error_code loadModuleFromSource(
            const char *name, const char *path, const string_literal source, shader::IModule **module) override {
            return m_service.loadModuleFromSource(name, path, source, module);
        }
    };

    struct PairedSubmission final : IMatterSubmissionObserverForTest {
        MatterIdentity m_source{};
        matter::GpuParameters m_profile{};
        Array<matter::GpuBodyBoundary> m_bodies;
        std::unique_ptr<MatterSubmissionJournal> m_journal;
        u32 m_body_count{}, m_calls{};

        void observeAdmission(
            const MatterState &source, const std::span<const u32> terrain, const u32 width, const u32 height,
            const float origin_x, const float origin_y, const u32 tick_hz) override {
            m_journal->observeAdmission(source, terrain, width, height, origin_x, origin_y, tick_hz);
        }

        void observe(
            const MatterIdentity source, const matter::GpuParameters &profile,
            const std::span<const matter::GpuBodyBoundary> bodies, const u32 body_count) override {
            PPR_TEST_ASSERT(bodies.size() == static_cast<std::size_t>(body_count) * matter::kGameSubsteps);
            m_journal->observe(source, profile, bodies, body_count);
            m_source = source;
            m_profile = profile;
            m_body_count = body_count;
            PPR_TEST_ASSERT(m_bodies.size() == bodies.size());
            std::ranges::copy(bodies, m_bodies.begin());
            ++m_calls;
        }
    };

    PPR_UNIT_TEST (full_frontier_frozen_fused_budget_pair) {
        const auto configured_root = config::getenvLookup("PPR_TEST_FUSED_PAIRED_ROOT");
        PPR_TEST_ASSERT(configured_root.has_value());
        const auto root = std::filesystem::canonical(*configured_root);
        std::filesystem::path output;
        for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
            const auto path = root / ("paired-output-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) {
                output = path;
                break;
            }
        }

        PPR_TEST_ASSERT(not output.empty());
        const auto baseline_path = root.parent_path() /
                                   "fused-iteration-specialization-1/baseline-adapted/solver.slang";
        const auto candidate_path = root.parent_path() /
                                    "fused-iteration-specialization-1/candidate-specialized/solver.slang";
        const auto baseline_source = pairedRead(baseline_path), candidate_source = pairedRead(candidate_path);
        PPR_TEST_ASSERT(baseline_source != candidate_source);
        PPR_TEST_ASSERT(pairedFingerprint(std::as_bytes(std::span{baseline_source})) == 0xeb85aef7cc955569ull);
        PPR_TEST_ASSERT(pairedFingerprint(std::as_bytes(std::span{candidate_source})) == 0xb7ad09e86daf1509ull);

        for (const auto &[name, expected]: std::array<std::pair<const char *, u64>, 4u>{
                 {{"types.slang", 0xf0e204e41068e245ull}, {"fork_core.slang", 0xe6b0ca7c8cbffb65ull},
                     {"environment.slang", 0xe6cd80d26b734b17ull}, {"sparse_address.slang", 0xf4053454cae2f284ull}}}) {
            const auto baseline = pairedRead(baseline_path.parent_path() / name);
            const auto candidate = pairedRead(candidate_path.parent_path() / name);
            PPR_TEST_ASSERT(
                baseline == candidate and candidate == pairedRead("shaders/pbmpm" / std::filesystem::path{name}));
            PPR_TEST_ASSERT(pairedFingerprint(std::as_bytes(std::span{baseline})) == expected);
        }

        struct Clock final : ITimerClock {
            TimePoint now() const noexcept override {
                return time::now();
            }
        } clock;

        // One fixed-size current record survives termination after flush. Completed
        // host intervals exclude journal work; the currently open interval is not complete.
        const auto progress_origin = clock.now();
        std::ofstream progress{output / "progress.txt", std::ios::binary | std::ios::trunc};
        PPR_TEST_ASSERT(progress.good());

        constexpr std::array<const char *, 17u> phase_names{
            "setup_other", "baseline_init", "baseline_prepare", "baseline_attach", "candidate_init",
            "candidate_prepare", "baseline_submit", "baseline_poll", "baseline_snapshot", "candidate_submit",
            "candidate_poll", "candidate_publish", "candidate_snapshot_begin", "candidate_snapshot_poll", "compare",
            "between_phases", "cleanup"};
        std::array<u64, phase_names.size()> phase_counts{}, phase_microseconds{};
        auto progress_previous = progress_origin;
        u64 progress_sequence{}, journal_completed{}, journal_microseconds{};
        u32 active_pair{}, completed_pairs{}, previous_category{};
        bool progress_valid{true}, setup_finished{};

        const auto category = [&](const std::string_view phase) -> u32 {
            if (phase == "baseline.tick.begin") {
                setup_finished = true;
                return 6u;
            }

            if (phase == "baseline.initialize.begin")
                return 1u;
            if (phase == "baseline.prepare.begin")
                return 2u;
            if (phase == "baseline.attach.begin")
                return 3u;
            if (phase == "candidate.initialize.begin")
                return 4u;
            if (phase == "candidate.prepare.begin")
                return 5u;
            if (phase == "baseline.tick.poll")
                return 7u;
            if (phase == "baseline.snapshot.begin")
                return 8u;
            if (phase == "candidate.submit.begin")
                return 9u;
            if (phase == "candidate.tick.poll")
                return 10u;
            if (phase == "candidate.publish.begin")
                return 11u;
            if (phase == "candidate.snapshot.begin")
                return 12u;
            if (phase == "candidate.snapshot.poll")
                return 13u;
            if (phase == "compare.begin")
                return 14u;
            if (phase.starts_with("cleanup."))
                return 16u;

            return setup_finished ? 15u : 0u;
        };

        const auto journal = [&](const char *const phase) noexcept {
            if (not progress_valid)
                return;

            const auto begin = clock.now();
            const auto interval = std::chrono::duration_cast<std::chrono::microseconds>(begin - progress_previous);
            phase_microseconds[previous_category] += static_cast<u64>(interval.count());
            ++phase_counts[previous_category];
            previous_category = category(phase);

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(begin - progress_origin);
            const u64 sequence = ++progress_sequence;
            std::array<char, 2048u> record;
            record.fill(' ');

            int length = std::snprintf(
                record.data(), record.size(), "seq=%llu phase=%-48s pair=%u completed=%u elapsed_us=%lld open=%s",
                static_cast<unsigned long long>(sequence), phase, active_pair, completed_pairs,
                static_cast<long long>(elapsed.count()), phase_names[previous_category]);
            bool formatted = length >= 0 and static_cast<std::size_t>(length) < record.size() - 1u;
            std::size_t used = formatted ? static_cast<std::size_t>(length) : 0u;

            for (u32 index = 0u; formatted and index < phase_names.size(); ++index) {
                length = std::snprintf(
                    record.data() + used, record.size() - used, " %s=%llu/%llu", phase_names[index],
                    static_cast<unsigned long long>(phase_counts[index]),
                    static_cast<unsigned long long>(phase_microseconds[index]));
                formatted = length >= 0 and static_cast<std::size_t>(length) < record.size() - used - 1u;
                if (formatted)
                    used += static_cast<std::size_t>(length);
            }

            if (formatted) {
                length = std::snprintf(
                    record.data() + used, record.size() - used,
                    " journal_completed=%llu journal_us=%llu journal_bytes=%llu end=%llu",
                    static_cast<unsigned long long>(journal_completed),
                    static_cast<unsigned long long>(journal_microseconds),
                    static_cast<unsigned long long>(journal_completed * record.size()),
                    static_cast<unsigned long long>(sequence));
                formatted = length >= 0 and static_cast<std::size_t>(length) < record.size() - used - 1u;
                if (formatted)
                    used += static_cast<std::size_t>(length);
            }

            if (formatted) {
                record[used] = ' ';
                record.back() = '\n';
                progress.seekp(0);
                progress.write(record.data(), static_cast<std::streamsize>(record.size()));
                progress.flush();
            }

            // Best effort during rejection and teardown: never replace the first
            // solver error or bypass cleanup because observability failed.
            if (not formatted or not progress.good()) {
                progress_valid = false;
                std::fputs("[fused-pair-progress] journal invalid; further writes disabled\n", stderr);
                std::fflush(stderr);
            }

            // The next record persists this write's cost. No per-poll write is added.
            progress_previous = clock.now();
            journal_microseconds += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(progress_previous - begin).count());
            ++journal_completed;
        };

        journal("setup.begin");
        ParallelScheduler workers{
            "Fused pair preparation", std::max(1u, std::min(8u, std::thread::hardware_concurrency())),
            4u * static_cast<std::size_t>(hal::page_granularity)};
        const sim::GenerationExecution execution{.m_scheduler = &workers};
        auto shaders = IShaderService::get();
        auto rhi_service = IRhiService::get();
        FrozenPairShader baseline_shader{*shaders, baseline_path, "pbmpm_solver_paired_dynamic_baseline_eb64443e"};
        FrozenPairShader candidate_shader{
            *shaders, candidate_path, "pbmpm_solver_paired_specialized_candidate_e8fcdc90"};
        PairedSubmission observed;
        MatterRuntime runtime;
        std::unique_ptr<ColonyDriver> driver;
        matter::GpuSolver candidate;
        rhi::ComPtr<rhi::ICommandQueue> queue;
        bool shader_started{}, rhi_started{};

        const auto complete = [&](Expected<MatterRequest> request) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, request);
            for (;;) {
                auto result = runtime.advance(*request);
                PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, result);
                if (*result) {
                    const auto &execution_result = (**result).m_execution;
                    PPR_RETURN_ERROR_ON_FAIL(
                        CheckpointTests, execution_result.m_ran ? std::error_code{}
                                                                : std::make_error_code(std::errc::operation_canceled));
                    PPR_RETURN_ERROR_ON_FAIL(CheckpointTests, execution_result.m_operation_error);
                    return execution_result.m_transport_error;
                }

                std::this_thread::yield();
            }
        };

        PPR_DEFER {
            // Stop the borrowed observer before draining either numerical owner.
            journal("cleanup.baseline.begin");
            if (driver) {
                driver->setMatterSubmissionObserverForTest(nullptr);
                std::error_code original;
                for (;;) {
                    const auto detached = driver->advanceTerminalDetachMatter(original);
                    if (not detached)
                        hal::fatalExit("paired-baseline-completion-unknown");
                    if (*detached)
                        break;

                    std::this_thread::yield();
                }
            }

            journal("cleanup.baseline.complete");

            journal("cleanup.candidate.begin");
            std::fputs("[fused-pair-timing-owner] candidate\n", stderr);
            std::fflush(stderr);

            if (candidate.drain() or candidate.shutdown())
                hal::fatalExit("paired-candidate-completion-unknown");
            journal("cleanup.candidate.complete");

            journal("cleanup.runtime.begin");
            std::fputs("[fused-pair-timing-owner] baseline\n", stderr);
            std::fflush(stderr);

            if (complete(runtime.beginShutdown()))
                hal::fatalExit("paired-runtime-completion-unknown");
            journal("cleanup.runtime.complete");

            journal("cleanup.driver.begin");
            if (driver and driver->shutdown())
                hal::fatalExit("paired-driver-cleanup-failure");
            journal("cleanup.driver.complete");

            // Solver fences prove execution; this owner must also retire queue-held commands.
            journal("cleanup.queue.begin");
            std::error_code queue_error{};
            if (queue) {
                const hal::ProfileScope queue_wait{
                    "Checkpoint.FusedPair.QueueRetirement", hal::profile_blocking_category};
                PPR_RETAIN_ERROR_ON_FAIL(CheckpointTests, queue_error, make_error_code(queue->waitOnHost()));
            }

            if (queue_error)
                hal::fatalExit("paired-queue-completion-unknown");

            journal("cleanup.queue.complete");
            queue.setNull();
            journal("cleanup.rhi.begin");
            if (rhi_started and rhi_service->shutdown())
                hal::fatalExit("paired-rhi-cleanup-failure");
            journal("cleanup.rhi.complete");

            journal("cleanup.shader.begin");
            if (shader_started and shaders->shutdown())
                hal::fatalExit("paired-shader-cleanup-failure");
            journal("cleanup.complete");

            const auto inputs_error = observed.m_journal ? observed.m_journal->write(output / "controlled-inputs.bin")
                                                         : std::error_code{};
            if (inputs_error) {
                std::fprintf(
                    stderr, "[fused-pair-inputs] write failed: %s:%d\n", inputs_error.category().name(),
                    inputs_error.value());
                std::fflush(stderr);
            }

            // Deferred permits throwing on ordinary return. Preserve an already
            // unwinding test failure; report lost observation only after cleanup.
            if (std::uncaught_exceptions() == 0) {
                PPR_TEST_ASSERT(not inputs_error);
                PPR_TEST_ASSERT(progress_valid);
            }
        };

        config::Diagnostics diagnostics;
        journal("world.prepare.begin");
        auto prepared = prepareWorld(
            std::filesystem::current_path() / "worldgen/default", "frontier", 0x2aull, clock, diagnostics, execution);
        PPR_TEST_ASSERT(prepared.has_value());
        journal("world.prepare.complete");

        const auto catalog = prepared->catalogOwner();
        const auto descriptor = prepared->descriptor();
        const auto terrain_checksum = prepared->generated().m_report.m_terrain_checksum;
        const auto output_checksum = prepared->generated().m_report.m_output_checksum;
        driver = std::make_unique<ColonyDriver>();
        journal("driver.initialize.begin");
        PPR_TEST_ASSERT(not driver->initPrepared({.m_seed = 0x2aull}, std::move(*prepared), &execution));
        journal("driver.initialize.complete");

        journal("initial.capture.begin");
        auto captured = driver->captureMatterState();
        PPR_TEST_ASSERT(captured.has_value());
        MatterState initial = std::move(*captured);
        PPR_TEST_ASSERT(descriptor.m_seed == 0x2aull and driver->seed() == 0x2aull);
        PPR_TEST_ASSERT(initial.m_particles.size() == 13044u);
        PPR_TEST_ASSERT(initial.m_profile.m_grid_width == 4103u and initial.m_profile.m_grid_height == 4103u);
        PPR_TEST_ASSERT(initial.m_profile.m_delta_time == 1.0f / 480.0f and initial.m_profile.m_shape_count == 0u);
        journal("initial.capture.complete");

        Array<u16> solids;
        for (const auto &material: catalog->materials()) {
            if (material.m_representation == EMaterialRepresentation::terrain and
                material.m_phase == EMaterialPhase::solid)
                solids.push_back(material.m_numeric_id);
        }

        journal("terrain.capture.begin");
        auto terrain = makeMatterTerrain(driver->grid(), solids);
        PPR_TEST_ASSERT(terrain.has_value());
        journal("terrain.capture.complete");

        journal("initial.artifacts.begin");
        pairedWrite(output / "initial-particles.bin", std::as_bytes(std::span{initial.m_particles}));
        pairedWrite(output / "terrain.bin", std::as_bytes(std::span{*terrain}));
        pairedWrite(output / "profile.bin", std::as_bytes(std::span{&initial.m_profile, 1u}));
        std::fprintf(
            stderr,
            "[fused-pair-identity] seed=%llu profile=%s content=%016llx terrain=%016llx output=%016llx "
            "particle_hash=%016llx terrain_input_hash=%016llx N=%zu G=4103 S=8 K=2 artifacts=%s\n",
            static_cast<unsigned long long>(descriptor.m_seed), descriptor.m_profile.c_str(),
            static_cast<unsigned long long>(descriptor.m_content_fingerprint),
            static_cast<unsigned long long>(terrain_checksum), static_cast<unsigned long long>(output_checksum),
            static_cast<unsigned long long>(pairedFingerprint(std::as_bytes(std::span{initial.m_particles}))),
            static_cast<unsigned long long>(pairedFingerprint(std::as_bytes(std::span{*terrain}))),
            initial.m_particles.size(), output.string().c_str());
        std::fprintf(
            stderr, "[fused-pair-layout] driver_size=%zu driver_alignment=%zu observer_size=%zu body_stride=%zu\n",
            sizeof(ColonyDriver), alignof(ColonyDriver), sizeof(PairedSubmission), sizeof(matter::GpuBodyBoundary));
        std::fflush(stderr);
        journal("initial.artifacts.complete");

        shader_started = true;
        journal("shader.initialize.begin");
        PPR_TEST_ASSERT(not shaders->initialize());
        journal("shader.initialize.complete");

        rhi_service->setCoreValidation(true);
        rhi_service->setGpuAssistedValidation(true);
        rhi_started = true;
        journal("rhi.initialize.begin");
        PPR_TEST_ASSERT(not rhi_service->initialize(rhi::DeviceType::D3D12, *shaders));
        PPR_TEST_ASSERT(
            not make_error_code(rhi_service->getDevice().getQueue(rhi::QueueType::Graphics, queue.writeRef())));
        journal("rhi.initialize.complete");

        journal("baseline.initialize.begin");
        PPR_TEST_ASSERT(not complete(runtime.beginInitialize(
            nullptr, rhi_service->getDevice(), baseline_shader, std::filesystem::current_path(), &clock)));
        journal("baseline.initialize.complete");

        observed.m_journal = std::make_unique<MatterSubmissionJournal>(std::format(
            "role=controlled profile={} seed={} content={:016x} terrain={:016x} output={:016x}", descriptor.m_profile,
            descriptor.m_seed, descriptor.m_content_fingerprint, terrain_checksum, output_checksum));
        observed.m_bodies.resize(matter::kGameSubsteps);
        driver->setMatterSubmissionObserverForTest(&observed);

        journal("baseline.prepare.begin");
        for (;;) {
            const auto ready = driver->advancePrepareMatter(runtime);
            PPR_TEST_ASSERT(ready.has_value());
            if (*ready)
                break;

            std::this_thread::yield();
        }

        journal("baseline.prepare.complete");

        journal("baseline.attach.begin");
        for (;;) {
            const auto ready = driver->advanceAttachMatter(runtime);
            PPR_TEST_ASSERT(ready.has_value());
            if (*ready)
                break;

            std::this_thread::yield();
        }

        journal("baseline.attach.complete");

        PPR_TEST_ASSERT(driver->matterState() != nullptr);
        const auto &admitted = *driver->matterState();
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 16u>>(admitted.m_profile) ==
            std::bit_cast<std::array<u32, 16u>>(initial.m_profile));
        PPR_TEST_ASSERT(
            pairedFingerprint(std::as_bytes(std::span{admitted.m_particles})) ==
            pairedFingerprint(std::as_bytes(std::span{initial.m_particles})));
        initial.m_identity = admitted.m_identity;

        journal("candidate.initialize.begin");
        PPR_TEST_ASSERT(not candidate.initialize(
            rhi_service->getDevice(), *queue, candidate_shader, std::filesystem::current_path(), initial.m_profile,
            &clock));
        journal("candidate.initialize.complete");

        journal("candidate.prepare.begin");
        PPR_TEST_ASSERT(not candidate.prepareReplacement(
            initial.m_particles, {}, candidate.accounting().m_epoch,
            {*terrain, kMatterTerrainEdge, kMatterTerrainEdge, 2.5f, 2.5f}));
        candidate.publishReplacement();
        journal("candidate.prepare.complete");
        PPR_TEST_ASSERT(baseline_shader.m_loaded != nullptr and candidate_shader.m_loaded != nullptr);
        PPR_TEST_ASSERT(baseline_shader.m_loaded != candidate_shader.m_loaded);
        std::fprintf(
            stderr, "[fused-pair-modules] baseline=%p candidate=%p hashes=eb85aef7cc955569/b7ad09e86daf1509\n",
            static_cast<void *>(baseline_shader.m_loaded), static_cast<void *>(candidate_shader.m_loaded));

        driver->setMatterSubmissionObserverForTest(&observed);
        driver->setPaused(true);
        Array<matter::GpuParticle> entering = std::move(initial.m_particles);
        Array<matter::GpuParticle> baseline_rows;
        Array<matter::GpuParticle> candidate_rows;
        const auto stop = [&](const char *reason, const u32 pair, const std::error_code error) {
            journal("failure.artifacts.begin");
            pairedWrite(output / "failed-entering-particles.bin", std::as_bytes(std::span{entering}));
            pairedWrite(output / "failed-bodies.bin", std::as_bytes(std::span{observed.m_bodies}));
            pairedWrite(output / "failed-profile.bin", std::as_bytes(std::span{&observed.m_profile, 1u}));
            pairedWrite(output / "failed-baseline.bin", std::as_bytes(std::span{baseline_rows}));
            pairedWrite(output / "failed-candidate.bin", std::as_bytes(std::span{candidate_rows}));
            pairedWrite(output / "failed-identity.bin", std::as_bytes(std::span{&observed.m_source, 1u}));

            const auto baseline_failure = runtime.failure();
            const auto candidate_failure = candidate.failure();
            std::fprintf(
                stderr,
                "[fused-pair-STOP] reason=%s pair=%u source_publication=%llu terrain_revision=%llu bodies=%u "
                "error=%s:%d baseline_flags=%u candidate_flags=%u candidate_node=%u field=%u row=%u\n",
                reason, pair, static_cast<unsigned long long>(observed.m_source.m_publication),
                static_cast<unsigned long long>(observed.m_source.m_terrain_revision), observed.m_body_count,
                error.category().name(), error.value(), baseline_failure ? baseline_failure->m_flags : 0u,
                candidate_failure.m_flags, candidate_failure.m_node, candidate_failure.m_field,
                candidate_failure.m_particle);
            if (baseline_failure) {
                std::fprintf(
                    stderr,
                    "[fused-pair-baseline-failure] tick=%llu cause=%s:%d flags=%u node=%u field=%u row=%u body=%u\n",
                    static_cast<unsigned long long>(driver->tickCount()), baseline_failure->m_cause.category().name(),
                    baseline_failure->m_cause.value(), baseline_failure->m_flags, baseline_failure->m_node,
                    baseline_failure->m_field, baseline_failure->m_particle, baseline_failure->m_body);
            }

            std::fprintf(
                stderr,
                "[fused-pair-candidate-failure] epoch=%llu cause=%s:%d flags=%u node=%u field=%u row=%u body=%u\n",
                static_cast<unsigned long long>(candidate.accounting().m_epoch),
                candidate_failure.m_cause.category().name(), candidate_failure.m_cause.value(),
                candidate_failure.m_flags, candidate_failure.m_node, candidate_failure.m_field,
                candidate_failure.m_particle, candidate_failure.m_body);
            std::fflush(stderr);
            journal("failure.artifacts.complete");
            PPR_TEST_ASSERT(false);
        };

        const auto pause_pending = [] {
            const hal::ProfileScope pending_wait{"Checkpoint.FusedPair.PendingPoll", hal::profile_blocking_category};
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        };

        // Temporary diagnostic horizon: at most 512 exact pairs within the existing deadline.
        for (u32 pair = 0u; pair < 512u; ++pair) {
            active_pair = pair;
            baseline_rows.clear();
            candidate_rows.clear();
            const u32 calls = observed.m_calls;

            journal("baseline.tick.begin");
            auto error = driver->stepOne();
            journal("baseline.tick.poll");
            while (not error and driver->physicsPending()) {
                error = driver->update(TimeSpan{});

                if (not error and driver->physicsPending())
                    pause_pending();
            }

            if (error)
                stop("baseline-rejected", pair, error);

            journal("baseline.tick.complete");

            PPR_TEST_ASSERT(observed.m_calls == calls + 1u and observed.m_body_count == 1u);
            PPR_TEST_ASSERT(observed.m_source.m_terrain_revision == initial.m_identity.m_terrain_revision);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 16u>>(observed.m_profile) ==
                std::bit_cast<std::array<u32, 16u>>(initial.m_profile));

            journal("baseline.snapshot.begin");
            for (;;) {
                const auto ready = driver->advanceMatterSnapshot(&workers);
                if (not ready)
                    stop("baseline-snapshot", pair, ready.error());
                if (*ready)
                    break;

                pause_pending();
            }

            PPR_TEST_ASSERT(driver->matterState() != nullptr);
            baseline_rows = driver->matterState()->m_particles;
            journal("baseline.snapshot.complete");

            journal("candidate.submit.begin");
            error = candidate.submitResidentTick(
                candidate.accounting().m_epoch, matter::kGameSubsteps, matter::kSourceIterations, observed.m_bodies,
                observed.m_body_count);
            if (error)
                stop("candidate-submit", pair, error);

            journal("candidate.tick.poll");
            for (;;) {
                const auto ready = candidate.pollTick();
                if (not ready)
                    stop("candidate-rejected", pair, ready.error());
                if (*ready)
                    break;

                pause_pending();
            }

            journal("candidate.tick.complete");

            journal("candidate.publish.begin");
            candidate.publishTick();
            journal("candidate.snapshot.begin");
            PPR_TEST_ASSERT(not candidate.beginSnapshot(candidate.accounting().m_epoch));
            journal("candidate.snapshot.poll");
            for (;;) {
                auto ready = candidate.pollSnapshot();
                if (not ready)
                    stop("candidate-snapshot", pair, ready.error());
                if (*ready) {
                    candidate_rows = std::move((**ready).m_particles);
                    break;
                }

                pause_pending();
            }

            journal("candidate.snapshot.complete");

            journal("compare.begin");
            PPR_TEST_ASSERT(baseline_rows.size() == entering.size() and candidate_rows.size() == entering.size());
            for (u32 row = 0u; row < entering.size(); ++row) {
                const auto baseline_words = std::bit_cast<std::array<u32, 22u>>(baseline_rows[row]);
                const auto candidate_words = std::bit_cast<std::array<u32, 22u>>(candidate_rows[row]);

                for (u32 field = 0u; field < 22u; ++field) {
                    if (baseline_words[field] != candidate_words[field]) {
                        std::fprintf(
                            stderr, "[fused-pair-divergence] pair=%u row=%u field=%u baseline=%08x candidate=%08x\n",
                            pair, row, field, baseline_words[field], candidate_words[field]);
                        stop("exact-semantic-divergence", pair, std::make_error_code(std::errc::illegal_byte_sequence));
                    }
                }
            }

            if (pair == 0u) {
                pairedWrite(output / "first-pair-bodies.bin", std::as_bytes(std::span{observed.m_bodies}));
                pairedWrite(output / "first-pair-baseline.bin", std::as_bytes(std::span{baseline_rows}));
                pairedWrite(output / "first-pair-candidate.bin", std::as_bytes(std::span{candidate_rows}));
                pairedWrite(output / "first-pair-source.bin", std::as_bytes(std::span{&observed.m_source, 1u}));
            }

            entering = std::move(candidate_rows);
            completed_pairs = pair + 1u;
            journal("pair.complete");
            if (completed_pairs == 1u or completed_pairs % 32u == 0u) {
                std::fprintf(
                    stderr, "[fused-pair] completed=%u baseline_tick=%llu candidate_epoch=%llu\n", completed_pairs,
                    static_cast<unsigned long long>(driver->tickCount()),
                    static_cast<unsigned long long>(candidate.accounting().m_epoch));
                std::fflush(stderr);
            }
        }
    };

    // ------------------------------------------------------------------
    // attached GPU edit and cold restore continuation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (attached_dig_preparation_retry_publishes_inventory_once_without_render_ack) {
        constexpr DigRect second_dig{{255u, 127u}, 2u, 2u};
        static_assert(second_dig.m_width > 0u and second_dig.m_width <= kMaxDigEdge);
        static_assert(second_dig.m_height > 0u and second_dig.m_height <= kMaxDigEdge);
        static_assert(second_dig.m_width * second_dig.m_height <= kMaxDigCells);
        constexpr std::array<sim::GlobalCellPos, 3u> second_dig_cells{{{255u, 127u}, {256u, 127u}, {255u, 128u}}};

        PPR_TEST_ASSERT(sim::chunkOf(second_dig_cells[0u]) != sim::chunkOf(second_dig_cells[1u]));
        PPR_TEST_ASSERT(sim::chunkOf(second_dig_cells[0u]) != sim::chunkOf(second_dig_cells[2u]));
        PPR_TEST_ASSERT(sim::chunkOf(second_dig_cells[1u]) != sim::chunkOf(second_dig_cells[2u]));

        Fixture fixture;
        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        checkpoint.m_current_matter.m_particles[0u].m_displacement_x = 0.125f;
        checkpoint.m_current_matter.m_particles[0u].m_deformation_gradient = {1.0f, 0.125f, 0.0f, 1.0f};

        auto physical = *checkpoint.m_particles.m_chunks[0u].m_particles;
        physical[0u].m_x_m = std::nextafter(physical[0u].m_x_m, std::numeric_limits<double>::infinity());
        checkpoint.m_current_matter.m_records[0u] = physical[0u];
        checkpoint.m_particles.m_chunks[0u].m_particles = std::make_shared<const Array<sim::MatterParticle>>(
            std::move(physical));

        // A second unblocked edit spans three actual work chunks. It must finish
        // inside prepareEdit's headless loop, not only update's retry branch.
        sim::ChunkGrid terrain_grid;
        PPR_TEST_ASSERT(not sim::apply(terrain_grid, checkpoint.m_snapshot));

        for (const auto position: second_dig_cells)
            PPR_TEST_ASSERT(
                not terrain_grid.setCell(position, {fixture.m_catalog->findMaterial("basalt")->m_numeric_id, 310.0f}));

        PPR_TEST_ASSERT(terrain_grid.getCell({256u, 128u})->m_element == 0u);

        auto terrain_snapshot = sim::captureFull(terrain_grid, checkpoint.m_descriptor.m_seed);
        PPR_TEST_ASSERT(terrain_snapshot.has_value());
        checkpoint.m_snapshot.m_deltas = std::move(terrain_snapshot->m_deltas);

        GpuOwner owner;
        owner.initialize();
        owner.restore(fixture.m_catalog, checkpoint, 3u);
        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        const auto original = *driver.matterState();

        Array<u16> solids;
        for (const auto &material: fixture.m_catalog->materials()) {
            if (material.m_representation == EMaterialRepresentation::terrain and
                material.m_phase == EMaterialPhase::solid)
                solids.push_back(material.m_numeric_id);
        }

        auto terrain = makeMatterTerrain(driver.grid(), solids);
        PPR_TEST_ASSERT(terrain.has_value());
        PPR_TEST_ASSERT(
            not owner.completeOwned(owner.m_runtime.beginPrepareResidentWorld(original, std::move(*terrain))));

        PPR_TEST_ASSERT(not driver.requestDig({127u, 40u}, 2u, 1u));
        std::error_code rejected{};
        for (u32 attempt = 0u; attempt < 32u and not rejected; ++attempt)
            rejected = driver.update(TimeSpan{});

        PPR_TEST_ASSERT(rejected == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(driver.editPending() and not driver.editReadyForPresentation());
        PPR_TEST_ASSERT(driver.matterState()->m_identity == original.m_identity);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == original.m_next_id);
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element != 0u);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 22u>>(driver.matterState()->m_particles[0u]) ==
            std::bit_cast<std::array<u32, 22u>>(original.m_particles[0u]));

        PPR_TEST_ASSERT(not owner.completeOwned(owner.m_runtime.beginCancelWorld()));

        for (u32 attempt = 0u; attempt < 32u and driver.editPending(); ++attempt)
            owner.update(TimeSpan{});

        PPR_TEST_ASSERT(not driver.editPending() and driver.tickCount() == checkpoint.m_tick);
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({128u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.particleCount() == 3u and driver.matterState()->m_next_id == original.m_next_id + 2u);
        PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == original.m_identity.m_publication + 1u);
        PPR_TEST_ASSERT(
            driver.matterState()->m_identity.m_terrain_revision == original.m_identity.m_terrain_revision + 1u);

        const auto append_accounting = owner.m_runtime.accounting();
        PPR_TEST_ASSERT(append_accounting.has_value());
        PPR_TEST_ASSERT(
            append_accounting->m_last_particle_upload_bytes ==
            2u * (sizeof(matter::ParticleWrite) + sizeof(matter::ParticleReadonly)));
        PPR_TEST_ASSERT(
            append_accounting->m_last_particle_copy_bytes ==
            sizeof(matter::ParticleWrite) + sizeof(matter::ParticleReadonly));

        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 22u>>(driver.matterState()->m_particles[0u]) ==
            std::bit_cast<std::array<u32, 22u>>(original.m_particles[0u]));

        const auto edited_capture = captureWorldCheckpoint(driver, driver.preparedWorld()->generated());
        PPR_TEST_ASSERT(edited_capture.has_value());
        PPR_TEST_ASSERT(not validateWorldCheckpoint(*edited_capture, *fixture.m_catalog));
        PPR_TEST_ASSERT(driver.matterState()->m_records[0u] == original.m_records[0u]);

        const auto committed_id = driver.matterState()->m_next_id;
        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == committed_id and driver.particleCount() == 3u);

        PPR_TEST_ASSERT(not driver.requestDig(second_dig.m_min, second_dig.m_width, second_dig.m_height));
        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(not driver.editPending() and driver.tickCount() == checkpoint.m_tick);
        PPR_TEST_ASSERT(driver.digTotal() == 5u and driver.particleCount() == 6u);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == committed_id + 3u);

        for (const auto position: second_dig_cells)
            PPR_TEST_ASSERT(driver.grid().getCell(position)->m_element == 0u);

        driver.setPaused(false);
        owner.update(TimeSpan{std::chrono::seconds{1}} / 60u);
        owner.completeTick();
        PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 1u and driver.particleCount() == 6u);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == committed_id + 3u);
    };

    PPR_UNIT_TEST (coverage_deferral_keeps_cohort_then_evolution_allows_a_later_tick_retry) {
        Fixture fixture;
        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        // Explicit admitted carried motion takes this isolated particle below
        // the proposed wall during the first eight-substep tick.
        checkpoint.m_current_matter.m_particles[0u].m_displacement_y = -1.0f;

        const Footprint wall{{129u, 39u}, 9u, 3u, fixture.m_catalog->findMaterial("basalt")->m_numeric_id};
        {
            sim::ChunkGrid grid;
            PPR_TEST_ASSERT(not sim::apply(grid, checkpoint.m_snapshot));

            sim::Registry registry;
            PPR_TEST_ASSERT(not registerColonyComponents(registry));
            PPR_TEST_ASSERT(not sim::restore(registry, checkpoint.m_snapshot));

            PPR_TEST_ASSERT(not validateConstruction(grid, registry, wall, *fixture.m_catalog));
        }

        const auto &particle = checkpoint.m_current_matter.m_particles[0u];
        const float particle_x = particle.m_position_x - kMatterOrigin;
        const float particle_y = particle.m_position_y - kMatterOrigin;
        PPR_TEST_ASSERT(
            particle_x > static_cast<float>(wall.m_min.m_x) and
            particle_x < static_cast<float>(wall.m_min.m_x + wall.m_width));
        PPR_TEST_ASSERT(
            particle_y > static_cast<float>(wall.m_min.m_y) and
            particle_y < static_cast<float>(wall.m_min.m_y + wall.m_height));

        GpuOwner owner;
        owner.initialize();
        owner.restore(fixture.m_catalog, checkpoint, 3u);
        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        driver.requireEditPublication();
        const auto original = *driver.matterState();

        const auto errand = driver.buildWall(wall);
        PPR_TEST_ASSERT(errand.has_value());

        owner.step();
        PPR_TEST_ASSERT(driver.physicsPending() and not driver.editPending());
        PPR_TEST_ASSERT(driver.takeEditRejection() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not driver.takeEditRejection());
        PPR_TEST_ASSERT(driver.matterState()->m_identity == original.m_identity);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == original.m_next_id);
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({130u, 40u})->m_element == 0u);

        owner.completeTick();
        PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 1u);
        PPR_TEST_ASSERT(driver.matterState()->m_records[0u].m_y_m < 38.0 * original.m_cell_metres);

        owner.step();
        for (u32 attempt = 0u; attempt < 32u and driver.tickCount() != checkpoint.m_tick + 2u; ++attempt) {
            if (driver.physicsPending())
                owner.drain();

            owner.update(TimeSpan{});
        }

        PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 2u and not driver.physicsPending());
        owner.captureCurrent();

        PPR_TEST_ASSERT(not driver.editPending() and driver.captureReady());
        PPR_TEST_ASSERT(driver.grid().getCell({130u, 40u})->m_element == driver.boundaryMaterial());
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 27u);
        PPR_TEST_ASSERT(driver.particleCount() == 1u and driver.matterState()->m_next_id == original.m_next_id);
        PPR_TEST_ASSERT(
            driver.matterState()->m_identity.m_terrain_revision == original.m_identity.m_terrain_revision + 1u);
        PPR_TEST_ASSERT(not driver.takeEditRejection());
    };

    PPR_UNIT_TEST (identically_restored_attached_owners_preserve_columns_and_resume_the_same_tick) {
        Fixture fixture;
        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto &carried = checkpoint.m_current_matter;
        carried.m_identity = {17u, 8u, 4u};
        carried.m_particles[0u].m_displacement_x = 0.125f;
        carried.m_particles[0u].m_deformation_gradient = {1.0f, 0.125f, 0.0f, 1.0f};
        carried.m_particles[0u].m_log_jp = 0.75f;

        const auto bytes = encodeWorldCheckpoint(checkpoint, *fixture.m_catalog);
        PPR_TEST_ASSERT(bytes.has_value());
        const auto saved = decodeWorldCheckpoint(*bytes, *fixture.m_catalog);
        PPR_TEST_ASSERT(saved.has_value());

        sim::Registry saved_registry;
        PPR_TEST_ASSERT(not registerColonyComponents(saved_registry));
        PPR_TEST_ASSERT(not sim::restore(saved_registry, saved->m_snapshot));
        PPR_TEST_ASSERT(saved->m_controlled.has_value());
        const auto *const saved_definition = saved_registry.get<sim::BodyDefinition>(*saved->m_controlled);
        PPR_TEST_ASSERT(saved_definition != nullptr);

        const auto definition_values = [](const sim::BodyDefinition &value) {
            return std::tuple{
                value.m_motion, value.m_geometry, value.m_half_width, value.m_half_height, value.m_density,
                value.m_friction, value.m_restitution, value.m_category_bits, value.m_mask_bits, value.m_group_index,
                value.m_enable_sleep, value.m_is_awake, value.m_is_bullet, value.m_fixed_rotation};
        };

        std::binary_semaphore frame_entered{0}, release_frame{0};
        RenderLane lane;
        PPR_TEST_ASSERT(not lane.start(
            {},
            [&](const RenderFrameRequest &, RenderFrameResult &) {
                frame_entered.release();
                release_frame.acquire();
                return std::error_code{};
            },
            {}, {}));
        PPR_DEFER {
            lane.requestStop();
            PPR_TEST_ASSERT(not lane.join());
        };

        // The lane and semaphore owners outlive GpuOwner's inverse teardown.
        GpuOwner owner;
        owner.initialize(&lane);

        Array<std::array<u32, 22u>> first;
        std::optional<sim::BodiesSection> first_bodies;
        PPR_TEST_ASSERT(not saved->m_snapshot.m_bodies.m_records.empty());
        PPR_TEST_ASSERT(saved->m_snapshot.m_bodies.m_records[0u].m_state.m_velocity_y != 0.0f);

        std::unique_ptr<ColonyDriver> replacement;
        PPR_DEFER {
            if (replacement) {
                for (;;) {
                    const auto detached = replacement->advanceDetachMatter();
                    PPR_TEST_ASSERT(detached.has_value());
                    if (*detached) {
                        break;
                    }

                    std::this_thread::yield();
                }

                PPR_TEST_ASSERT(not replacement->shutdown());
            }
        };

        game::SessionWorldTransition transition{};

        const auto advance_transition = [&](const game::SessionWorldTransitionInput &input) {
            const auto step = game::advanceSessionWorldTransition(
                transition, owner.m_driver.get(), replacement.get(), owner.m_runtime, input);
            PPR_TEST_ASSERT(step.has_value());
            return *step;
        };

        for (u64 replay = 0u; replay < 2u; ++replay) {
            if (replay == 0u) {
                owner.restore(fixture.m_catalog, *saved, 30u);
            } else {
                // Reuse the cancelled CPU candidate. GPU preparation leaves
                // the exact old active owner valid until admitted activation.
                transition = {};
                transition.m_phase = game::ESessionWorldPhase::detaching;
                transition.m_reservation_epoch = 77u;
                const GridWorldIdentity requested{502u, 31u, saved->m_descriptor.m_content_fingerprint};
                auto old_view = owner.m_driver->particleView();
                PPR_TEST_ASSERT(old_view and old_view->current());

                while (advance_transition({.m_world = requested}) != game::ESessionWorldStep::activate) {
                    owner.drain();
                    std::this_thread::yield();
                }

                PPR_TEST_ASSERT(old_view->current() and not owner.m_driver->matterAttached());
                PPR_TEST_ASSERT(not replacement->matterAttached() and not transition.m_expected);
                PPR_TEST_ASSERT(game::canSelectSessionWorldView(transition));
                PPR_TEST_ASSERT(advance_transition({.m_activation_admitted = true}) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(not game::canSelectSessionWorldView(transition));
                PPR_TEST_ASSERT(advance_transition({.m_cancel = true}) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(transition.m_phase == game::ESessionWorldPhase::activating and old_view->current());

                // The same production advance settles the actual attachment;
                // cancel after commit intent cannot resurrect the old bundle.
                while (advance_transition({.m_world = requested, .m_cancel = true, .m_activated = true}) !=
                       game::ESessionWorldStep::publish) {
                    owner.drain();
                    std::this_thread::yield();
                }

                PPR_TEST_ASSERT(not old_view->current() and replacement->matterAttached());
                PPR_TEST_ASSERT(transition.m_expected and game::canSelectSessionWorldView(transition));
                owner.m_driver.swap(replacement);
                PPR_TEST_ASSERT(not replacement->shutdown());
                replacement.reset();
                PPR_TEST_ASSERT(advance_transition({.m_activated = true}) == game::ESessionWorldStep::wait);

                RenderFrameResult presented{};
                presented.m_presented = true;
                presented.m_reservation_epoch = 77u;
                presented.m_presented_grid = requested;
                presented.m_presented_particles = transition.m_expected;
                PPR_TEST_ASSERT(advance_transition({.m_frame = &presented}) == game::ESessionWorldStep::finish);
                PPR_TEST_ASSERT(advance_transition({.m_frame = &presented}) == game::ESessionWorldStep::wait);
                old_view.reset();
            }

            auto &driver = *owner.m_driver;
            PPR_TEST_ASSERT(driver.matterState()->m_identity == MatterIdentity{30u + replay, 8u, 4u});

            owner.update(TimeSpan{std::chrono::seconds{1}} / 60u);
            owner.drain();
            owner.update(TimeSpan{});
            PPR_TEST_ASSERT(not driver.physicsPending() and not driver.particleSnapshotCurrent());
            PPR_TEST_ASSERT(driver.matterState() == nullptr);

            if (replay == 0u) {
                const GridWorldIdentity previous{501u, 30u, saved->m_descriptor.m_content_fingerprint};
                driver.bindParticlePresentation(previous);
                auto old_view = driver.particleView();
                PPR_TEST_ASSERT(old_view and old_view->current());

                auto candidate = restoreWorldCheckpoint(fixture.m_catalog, *saved, 31u);
                PPR_TEST_ASSERT(candidate.has_value());
                replacement = std::move(*candidate);
                transition = {};
                transition.m_previous = {previous, true, true, true, true, false};
                transition.m_phase = game::ESessionWorldPhase::detaching;

                // This begins with genuinely stale CPU coordinates after the
                // resident tick. Snapshot/detach must precede candidate prepare.
                while (advance_transition({}) != game::ESessionWorldStep::activate) {
                    owner.drain();
                    std::this_thread::yield();
                }

                PPR_TEST_ASSERT(driver.particleSnapshotCurrent() and not driver.matterAttached());
                PPR_TEST_ASSERT(driver.particleSnapshotTick() == checkpoint.m_tick + 1u);
                PPR_TEST_ASSERT(old_view->current() and old_view->m_world == previous);
                PPR_TEST_ASSERT(not replacement->matterAttached() and driver.paused());

                // GPU candidate cleanup alone cannot resume old ownership while
                // graphics still holds an unacknowledged reversible candidate.
                PPR_TEST_ASSERT(advance_transition({.m_cancel = true}) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(transition.m_phase == game::ESessionWorldPhase::cancelling);
                PPR_TEST_ASSERT(not driver.matterAttached() and old_view->current());
                while (advance_transition({.m_cancel = true, .m_renderer_idle = true}) !=
                       game::ESessionWorldStep::release_candidate) {
                    owner.drain();
                }

                PPR_TEST_ASSERT(not replacement->matterDeviceOwned());

                // A real executing frame delays an ADMITTED resume in the same
                // owned slot. This is not an admission-busy-only witness.
                bool frame_released = false;
                PPR_DEFER {
                    if (not frame_released) {
                        release_frame.release();
                    }
                };

                auto packet = std::make_shared<FramePacket>();
                packet->m_frame_id = 1u;
                packet->m_framebuffer_size = int2{64, 64};
                RenderFrameRequest frame_request{};
                frame_request.m_packet = std::move(packet);

                PPR_TEST_ASSERT(not lane.submitFrame(frame_request));
                PPR_TEST_ASSERT(frame_entered.try_acquire_for(std::chrono::seconds{30}));
                PPR_TEST_ASSERT(lane.operationAvailability().m_available);

                bool close_requested = false;
                u32 transition_turns = 0u, ordinary_updates = 0u;

                const auto service_turn = [&](const bool close) {
                    close_requested = close_requested or close;

                    if (game::sessionTransitionOwnsDriverService(transition)) {
                        ++transition_turns;
                        return advance_transition({.m_cancel = close_requested});
                    }

                    ++ordinary_updates;
                    PPR_TEST_ASSERT(not driver.update({}));
                    return game::ESessionWorldStep::wait;
                };

                PPR_TEST_ASSERT(service_turn(false) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(owner.m_runtime.requestPending() and not owner.m_runtime.serviceReady());
                PPR_TEST_ASSERT(not lane.operationAvailability().m_available);
                PPR_TEST_ASSERT(not driver.matterAttached() and driver.matterServicePending());
                PPR_TEST_ASSERT(transition.m_phase == game::ESessionWorldPhase::resuming);

                PPR_TEST_ASSERT(service_turn(true) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(close_requested and transition_turns == 2u and ordinary_updates == 0u);
                PPR_TEST_ASSERT(not driver.matterAttached() and driver.matterServicePending());
                PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 1u and old_view->current());

                release_frame.release();
                frame_released = true;

                while (not owner.m_runtime.serviceReady()) {
                    std::this_thread::yield();
                }

                // Completed but unconsumed is exactly the next-service R3
                // trigger: ordinary service would still see the detached owner.
                PPR_TEST_ASSERT(owner.m_runtime.requestPending() and not driver.matterAttached());
                PPR_TEST_ASSERT(driver.matterServicePending() and not lane.operationAvailability().m_available);
                PPR_TEST_ASSERT(game::sessionTransitionOwnsDriverService(transition));
                PPR_TEST_ASSERT(service_turn(false) == game::ESessionWorldStep::restore);
                PPR_TEST_ASSERT(transition_turns == 3u and ordinary_updates == 0u and close_requested);
                PPR_TEST_ASSERT(not game::sessionTransitionOwnsDriverService(transition));
                PPR_TEST_ASSERT(transition.m_previous.m_world == previous and transition.m_previous.m_ready);
                PPR_TEST_ASSERT(transition.m_previous.m_presented and not transition.m_previous.m_paused);

                // The host may now handle its latched close. Without close,
                // subsequent ordinary service is eligible again and adds no tick.
                close_requested = false;
                PPR_TEST_ASSERT(service_turn(false) == game::ESessionWorldStep::wait);
                PPR_TEST_ASSERT(ordinary_updates == 1u and driver.tickCount() == checkpoint.m_tick + 1u);
                PPR_TEST_ASSERT(driver.matterAttached() and not driver.paused());
                PPR_TEST_ASSERT(driver.particleView()->m_world == previous and old_view->current());
                PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 1u);
                old_view.reset();
            }

            // Main retains the source through copy/reconstruction and captures
            // body awake values before the existing worker receives the driver.
            WorldJob capture{
                parallel::backgroundPool(),
                [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                    config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                    // Capture/override-only fixtures must never admit a default generation.
                    return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
                }};
            PPR_TEST_ASSERT(not capture.requestCheckpoint(owner.m_driver));
            PPR_TEST_ASSERT(not owner.m_driver);

            PPR_TEST_ASSERT(not capture.update(false));

            if (replay != 0u) {
                capture.cancel(); // The admitted copy must drain before return.
            }

            while (capture.view().m_state != EWorldJobState::prepared) {
                PPR_TEST_ASSERT(not capture.update(true));
                std::this_thread::yield();
            }

            if (replay == 0u) {
                PPR_TEST_ASSERT(not capture.view().m_error);

                const auto frozen = capture.takeCheckpoint();
                PPR_TEST_ASSERT(frozen.m_tick == checkpoint.m_tick + 1u);
                PPR_TEST_ASSERT(not validateWorldCheckpoint(frozen, *fixture.m_catalog));
            } else {
                PPR_TEST_ASSERT(capture.view().m_error == std::errc::operation_canceled);
            }

            owner.m_driver = capture.takeCaptured();
            PPR_TEST_ASSERT(owner.m_driver.get() == &driver and driver.particleSnapshotCurrent());
            PPR_TEST_ASSERT(driver.particleSnapshotTick() == checkpoint.m_tick + 1u);

            while (capture.busy()) {
                PPR_TEST_ASSERT(not capture.update(true));
                std::this_thread::yield();
            }

            transition = {};
            transition.m_phase = game::ESessionWorldPhase::resuming;
            transition.m_returning = true;
            transition.m_previous.m_paused = false;
            transition.m_reservation_epoch = 78u;
            const GridWorldIdentity restored{
                503u + replay, driver.worldGeneration(), saved->m_descriptor.m_content_fingerprint};
            while (advance_transition({.m_world = restored}) != game::ESessionWorldStep::render_return) {
                owner.drain();
            }

            PPR_TEST_ASSERT(driver.paused() and driver.matterAttached());
            PPR_TEST_ASSERT(transition.m_phase == game::ESessionWorldPhase::awaiting_presentation);
            PPR_TEST_ASSERT(advance_transition({.m_activated = true}) == game::ESessionWorldStep::wait);

            RenderFrameResult returned{};
            returned.m_reservation_epoch = 78u;
            returned.m_presented = true;
            returned.m_presented_grid = restored;
            returned.m_presented_particles = transition.m_expected;
            returned.m_dropped = true;
            PPR_TEST_ASSERT(advance_transition({.m_frame = &returned}) == game::ESessionWorldStep::wait);
            PPR_TEST_ASSERT(driver.paused() and driver.tickCount() == checkpoint.m_tick + 1u);
            returned.m_dropped = false;
            PPR_TEST_ASSERT(advance_transition({.m_frame = &returned}) == game::ESessionWorldStep::finish);
            PPR_TEST_ASSERT(not driver.paused());
            PPR_TEST_ASSERT(advance_transition({.m_frame = &returned}) == game::ESessionWorldStep::wait);

            PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + 1u);
            PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == 9u);
            PPR_TEST_ASSERT(driver.matterState()->m_identity.m_terrain_revision == 4u);
            PPR_TEST_ASSERT(driver.matterState()->m_next_id == saved->m_current_matter.m_next_id);

            for (std::size_t row = 0u; row < driver.matterState()->m_particles.size(); ++row) {
                const auto words = std::bit_cast<std::array<u32, 22u>>(driver.matterState()->m_particles[row]);
                if (replay == 0u)
                    first.push_back(words);
                else
                    PPR_TEST_ASSERT(words == first[row]);
            }

            const auto recaptured = captureWorldCheckpoint(driver, driver.preparedWorld()->generated());
            PPR_TEST_ASSERT(recaptured.has_value());
            PPR_TEST_ASSERT(recaptured->m_current_matter.m_identity == driver.matterState()->m_identity);

            const auto &bodies = recaptured->m_snapshot.m_bodies;
            PPR_TEST_ASSERT(not bodies.m_records.empty());
            PPR_TEST_ASSERT(bodies.m_records[0u].m_state.m_y != saved->m_snapshot.m_bodies.m_records[0u].m_state.m_y);
            if (replay == 0u)
                first_bodies = bodies;
            else
                PPR_TEST_ASSERT(bodies == *first_bodies);

            const auto *const definition = driver.registry().get<sim::BodyDefinition>(*saved->m_controlled);
            PPR_TEST_ASSERT(definition != nullptr);
            PPR_TEST_ASSERT(definition_values(*definition) == definition_values(*saved_definition));

            for (;;) {
                const auto detached = driver.advanceDetachMatter();
                PPR_TEST_ASSERT(detached.has_value());
                if (*detached) {
                    break;
                }

                owner.drain();
                std::this_thread::yield();
            }
        }
    };

    // ------------------------------------------------------------------
    // sustained actual dug-sand integration
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (dug_sand_sustains_sixty_runtime_ticks_on_shared_terrain) {
        constexpr DigRect dig{{127u, 40u}, 2u, 1u};
        static_assert(dig.m_width <= kMaxDigEdge and dig.m_height <= kMaxDigEdge);
        static_assert(dig.m_width * dig.m_height <= kMaxDigCells);

        Fixture fixture;
        const auto *const basalt = fixture.m_catalog->findMaterial("basalt");
        const auto *const sandstone = fixture.m_catalog->findMaterial("sandstone");
        PPR_TEST_ASSERT(basalt != nullptr and sandstone != nullptr);
        PPR_TEST_ASSERT(basalt->m_reference_density_kg_m3 == 2000.0 and sandstone->m_reference_density_kg_m3 == 2000.0);

        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        checkpoint.m_matter = {};
        checkpoint.m_particles = {.m_next_id = 12u};
        auto empty = makeMatterState(
            checkpoint.m_particles, kFineCellMetres, checkpoint.m_descriptor.m_metric.m_reference_thickness_m,
            kMatterGravityY, 1u);
        PPR_TEST_ASSERT(empty.has_value());

        empty->m_identity.m_terrain_revision = 1u;
        checkpoint.m_current_matter = std::move(*empty);
        PPR_TEST_ASSERT(checkpoint.m_current_matter.m_records.empty() and checkpoint.m_current_matter.m_gas.empty());

        sim::ChunkGrid terrain_grid;
        PPR_TEST_ASSERT(not sim::apply(terrain_grid, checkpoint.m_snapshot));

        for (u32 y = 32u; y <= 35u; ++y) {
            for (u32 x = 120u; x <= 135u; ++x)
                PPR_TEST_ASSERT(not terrain_grid.setCell({x, y}, {basalt->m_numeric_id, 300.0f}));
        }

        auto terrain_snapshot = sim::captureFull(terrain_grid, checkpoint.m_descriptor.m_seed);
        PPR_TEST_ASSERT(terrain_snapshot.has_value());

        checkpoint.m_snapshot.m_deltas = std::move(terrain_snapshot->m_deltas);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, *fixture.m_catalog));

        // The actual lower-left mask samples and origin2.5 give top36/bottom32
        // in world fine cells (solver39/35), with flat top x120.5..135.5.
        // No nearest-boundary solver or hidden contact count is reconstructed.
        const std::array<u16, 2u> materials{basalt->m_numeric_id, sandstone->m_numeric_id};
        const std::array<double, 2u> temperatures{321.0, 345.0};
        const double h = checkpoint.m_current_matter.m_cell_metres;
        const double volume = h * h * checkpoint.m_current_matter.m_thickness_metres;
        const double mass = 2000.0 * volume;

        std::array<u32, 2u> top_overlap_endpoints{}, top_overlap_runs{}, longest_top_overlap_runs{},
            span_exit_endpoints{};
        u32 observed{};
        bool passed = false;

        GpuOwner owner;
        PPR_DEFER {
            if (not passed)
                owner.reportFailure(
                    std::make_error_code(std::errc::state_not_recoverable), "sand-first-failed-boundary");

            std::fprintf(
                stderr,
                "[dug-sand-summary] observed_game_tick_endpoints=%u expected_executed_source_substeps=%u "
                "fixed_row_top_overlap_endpoints=[%u,%u] longest_runs=[%u,%u] span_exit_endpoints=[%u,%u]; "
                "late_particle_contact=%s internal_grid_guardian=UNAVAILABLE SVD_plastic_safeguard_counts=UNAVAILABLE "
                "first_internal_substep_divergence=UNAVAILABLE; no sand/body-impact or settled-quality claim\n",
                static_cast<unsigned>(observed), static_cast<unsigned>(observed * matter::kGameSubsteps),
                static_cast<unsigned>(top_overlap_endpoints[0u]), static_cast<unsigned>(top_overlap_endpoints[1u]),
                static_cast<unsigned>(longest_top_overlap_runs[0u]),
                static_cast<unsigned>(longest_top_overlap_runs[1u]), static_cast<unsigned>(span_exit_endpoints[0u]),
                static_cast<unsigned>(span_exit_endpoints[1u]),
                top_overlap_endpoints[0u] + top_overlap_endpoints[1u] == 0u ? "UNPROVEN"
                                                                            : "geometric_endpoint_overlap_only");
            std::fflush(stderr);
        };

        owner.initialize();

        owner.restore(fixture.m_catalog, checkpoint, 60u);

        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        PPR_TEST_ASSERT(driver.matterState()->m_particles.empty() and driver.particleCount() == 0u);

        const auto original_identity = driver.matterState()->m_identity;

        PPR_TEST_ASSERT(not driver.requestDig(dig.m_min, dig.m_width, dig.m_height));

        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(not driver.editPending() and driver.captureReady());
        PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick and driver.digTotal() == 2u);
        PPR_TEST_ASSERT(
            driver.grid().getCell({127u, 40u})->m_element == 0u and
            driver.grid().getCell({128u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.matterState()->m_identity.m_publication == original_identity.m_publication + 1u);
        PPR_TEST_ASSERT(
            driver.matterState()->m_identity.m_terrain_revision == original_identity.m_terrain_revision + 1u);

        const auto emitted = *driver.matterState();
        PPR_TEST_ASSERT(
            emitted.m_next_id == 14u and emitted.m_records.size() == 2u and emitted.m_particles.size() == 2u);
        PPR_TEST_ASSERT(emitted.m_gas.empty());

        for (std::size_t row = 0u; row < 2u; ++row) {
            const auto &record = emitted.m_records[row];
            PPR_TEST_ASSERT(record.m_id == 12u + row and record.m_material == materials[row]);
            PPR_TEST_ASSERT(
                record.m_phase == sim::EGenerationMatter::solid and record.m_temperature_k == temperatures[row]);
            PPR_TEST_ASSERT(
                record.m_bulk_density_kg_m3 == 2000.0 and record.m_mass_kg == mass and record.m_volume_m3 == volume);
            PPR_TEST_ASSERT(record.m_x_m == (127.5 + static_cast<double>(row)) * h and record.m_y_m == 40.5 * h);

            matter::GpuParticle seed{};
            seed.m_position_x = 130.5f + static_cast<float>(row);
            seed.m_position_y = 43.5f;
            seed.m_mass = 2.0f;
            seed.m_material = static_cast<float>(matter::ESourceMaterial::sand);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 22u>>(emitted.m_particles[row]) ==
                std::bit_cast<std::array<u32, 22u>>(seed));
        }

        std::fprintf(
            stderr,
            "[dug-sand-input] G=4103 K=2 source_substeps_per_game_tick=8 endpoints=60 "
            "dig=(127,40,2,1) IDs=[12,13] temperatures_K=[321,345] density=2000 h=%.17g thickness=%.17g "
            "physical_volume_each=%.17g physical_mass_each=%.17g kernel_mass_each=2 kernel_volume_each=1 "
            "flat_top_world_fine=36 bottom=32 flat_x=[120.5,135.5] actor=distant; no sand/body-impact claim\n",
            h, emitted.m_thickness_metres, volume, mass);
        std::fflush(stderr);

        Array<matter::GpuParticle> previous = emitted.m_particles;

        for (u32 tick = 1u; tick <= 60u; ++tick) {
            owner.step();
            PPR_TEST_ASSERT(driver.physicsPending());

            owner.drain();

            owner.update(TimeSpan{});

            owner.captureCurrent();

            const auto &state = *driver.matterState();
            std::fprintf(
                stderr, "[dug-sand-collected] endpoint=%u tick=%llu publication=%llu terrain=%llu rows=%zu\n",
                static_cast<unsigned>(tick), static_cast<unsigned long long>(driver.tickCount()),
                static_cast<unsigned long long>(state.m_identity.m_publication),
                static_cast<unsigned long long>(state.m_identity.m_terrain_revision), state.m_particles.size());
            std::fflush(stderr);
            PPR_TEST_ASSERT(state.m_particles.size() == 2u and state.m_records.size() == 2u);

            double kernel_mass{}, nominal_volume{}, physical_mass{}, elastic_volume_proxy{}, mass_y{};
            bool finite = true, metadata = true, guardian = true, bottom = true;

            for (std::size_t row = 0u; row < 2u; ++row) {
                const auto &particle = state.m_particles[row];
                const auto &record = state.m_records[row];
                const auto &original = emitted.m_records[row];

                for (const float field: std::bit_cast<std::array<float, 22u>>(particle))
                    finite = finite and std::isfinite(field);

                metadata = metadata and
                           record.m_id == original.m_id and
                           record.m_material == original.m_material and
                           record.m_phase == original.m_phase and
                           record.m_temperature_k == original.m_temperature_k and
                           record.m_bulk_density_kg_m3 == original.m_bulk_density_kg_m3 and
                           record.m_mass_kg == original.m_mass_kg and
                           record.m_volume_m3 == original.m_volume_m3;

                const auto words = std::bit_cast<std::array<u32, 22u>>(particle);
                const auto original_words = std::bit_cast<std::array<u32, 22u>>(emitted.m_particles[row]);
                for (const u32 word: {13u, 14u, 15u, 18u, 19u, 20u, 21u})
                    metadata = metadata and words[word] == original_words[word];

                const double x = static_cast<double>(particle.m_position_x) - kMatterOrigin;
                const double y = static_cast<double>(particle.m_position_y) - kMatterOrigin;
                const bool in_span = x >= 120.5 and x <= 135.5;
                const bool overlap = in_span and y < 36.0 and y > 32.0;
                top_overlap_endpoints[row] += static_cast<u32>(overlap);
                top_overlap_runs[row] = overlap ? top_overlap_runs[row] + 1u : 0u;
                longest_top_overlap_runs[row] = std::max(longest_top_overlap_runs[row], top_overlap_runs[row]);
                span_exit_endpoints[row] += static_cast<u32>(not in_span);

                bottom = bottom and (not in_span or y >= 32.0);
                guardian = guardian and
                           particle.m_position_x > 3.0f and
                           particle.m_position_y > 3.0f and
                           particle.m_position_x < static_cast<float>(kMatterGridEdge - 4u) and
                           particle.m_position_y < static_cast<float>(kMatterGridEdge - 4u);

                const auto f = std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient);
                const auto d = std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement);
                const double det_f = static_cast<double>(f[0u]) * f[3u] - static_cast<double>(f[2u]) * f[1u];
                const double dx = static_cast<double>(particle.m_position_x) - previous[row].m_position_x;
                const double dy = static_cast<double>(particle.m_position_y) - previous[row].m_position_y;
                const double applied = std::sqrt(dx * dx + dy * dy);
                const double carried = std::sqrt(
                    static_cast<double>(particle.m_displacement_x) * particle.m_displacement_x +
                    static_cast<double>(particle.m_displacement_y) * particle.m_displacement_y);

                kernel_mass += particle.m_mass;
                nominal_volume += particle.m_volume;
                physical_mass += record.m_mass_kg;
                mass_y += record.m_mass_kg * y;
                elastic_volume_proxy += record.m_volume_m3 * det_f;

                std::fprintf(
                    stderr,
                    "[dug-sand-row] endpoint=%u row=%zu id=%llu world_fine=(%.9g,%.9g) applied_delta_cells=%.9g "
                    "carried_d_cells=%.9g F_columns=[%.9g,%.9g,%.9g,%.9g] detF=%.9g "
                    "D_columns=[%.9g,%.9g,%.9g,%.9g] logJp=%.9g unused_liquid_J=%.9g "
                    "in_flat_span=%u flat_top_depth=%.9g geometric_overlap_endpoint=%u words=",
                    static_cast<unsigned>(tick), row, static_cast<unsigned long long>(record.m_id), x, y, applied,
                    carried, static_cast<double>(f[0u]), static_cast<double>(f[1u]), static_cast<double>(f[2u]),
                    static_cast<double>(f[3u]), det_f, static_cast<double>(d[0u]), static_cast<double>(d[1u]),
                    static_cast<double>(d[2u]), static_cast<double>(d[3u]), static_cast<double>(particle.m_log_jp),
                    static_cast<double>(particle.m_liquid_density), static_cast<unsigned>(in_span),
                    in_span ? std::max(36.0 - y, 0.0) : 0.0, static_cast<unsigned>(overlap));
                for (const u32 word: words)
                    std::fprintf(stderr, "%08x ", static_cast<unsigned>(word));

                std::fprintf(stderr, "\n");
            }

            PPR_TEST_ASSERT(checkpoint.m_controlled.has_value());
            const auto *const body = driver.registry().get<sim::BodyState>(*checkpoint.m_controlled);
            PPR_TEST_ASSERT(body != nullptr);

            std::fprintf(stderr, "[dug-sand-body] endpoint=%u fields=", static_cast<unsigned>(tick));
            for (const float field: std::bit_cast<std::array<float, 6u>>(*body)) {
                std::fprintf(stderr, "%.9g ", static_cast<double>(field));
                finite = finite and std::isfinite(field);
            }

            const double com_y = mass_y / physical_mass;
            const auto accounting = owner.m_runtime.accounting();
            std::fprintf(
                stderr,
                "\n[dug-sand-observation] endpoint=%u COM_y_world_fine=%.9g kernel_mass=%.9g nominal_volume=%.9g "
                "physical_mass=%.17g elastic_deformation_only_volume_proxy_m3=%.17g; "
                "proxy_excludes_plastic_volume_and_is_not_occupied_volume sand_J_is_not_volume\n",
                static_cast<unsigned>(tick), com_y, kernel_mass, nominal_volume, physical_mass, elastic_volume_proxy);
            std::fflush(stderr);

            ++observed;

            PPR_TEST_ASSERT(finite and metadata and guardian and bottom);
            PPR_TEST_ASSERT(kernel_mass == 4.0 and nominal_volume == 2.0 and physical_mass == 2.0 * mass);
            PPR_TEST_ASSERT(state.m_gas.empty() and state.m_next_id == 14u and driver.particleCount() == 2u);
            PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + tick and driver.captureReady());
            PPR_TEST_ASSERT(state.m_identity.m_publication == emitted.m_identity.m_publication + tick);
            PPR_TEST_ASSERT(state.m_identity.m_terrain_revision == emitted.m_identity.m_terrain_revision);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 16u>>(state.m_profile) ==
                std::bit_cast<std::array<u32, 16u>>(emitted.m_profile));
            PPR_TEST_ASSERT(accounting.has_value() and accounting->m_substeps == matter::kGameSubsteps);

            if (tick == 1u or tick == 60u)
                PPR_TEST_ASSERT(com_y < 40.5);

            previous = state.m_particles;
        }

        owner.update(TimeSpan{});
        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(observed == 60u and driver.tickCount() == checkpoint.m_tick + 60u and driver.captureReady());
        PPR_TEST_ASSERT(driver.digTotal() == 2u and driver.matterState()->m_next_id == 14u);

        passed = true;
    };
}

namespace pP::game::tests {
    const UnitTest checkpoint_gpu = UnitTest::Named("checkpoint_gpu") / [](UnitTest::IRun &_) {
        _.recurse({
            detail::checkpoint::attached_dig_preparation_retry_publishes_inventory_once_without_render_ack,
            detail::checkpoint::coverage_deferral_keeps_cohort_then_evolution_allows_a_later_tick_retry,
            detail::checkpoint::identically_restored_attached_owners_preserve_columns_and_resume_the_same_tick,
            detail::checkpoint::dug_sand_sustains_sixty_runtime_ticks_on_shared_terrain,
            detail::checkpoint::full_frontier_frozen_fused_budget_pair,
        });
    };

    const UnitTest &gameCheckpointGpuTests() noexcept {
        return checkpoint_gpu;
    }
}
