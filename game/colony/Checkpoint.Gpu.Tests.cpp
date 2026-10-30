module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import engine.asset;
import engine.core;
import engine.math;
import engine.sim;
import engine.app;
import engine.image;
import engine.matter;
import engine.shader;
import engine.rhi;
import game.colony.matter;
import game.colony.metric;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.tools;
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
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
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

                // The current checkpoint includes exact physical ledger/gas, complete cells,
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
                    std::ofstream stream{directory / "before-prefix-current.bin", std::ios::binary};
                    stream.write(
                        reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
                    stream.close();
                    std::fprintf(
                        stderr,
                        "[checkpoint-gpu-artifact] fixture=checkpoint::Fixture path=%s bytes=%zu write_ok=%u terrain=before-prefix-current-cells bodies=before-prefix-current-ECS\n",
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

            // Solver fences prove execution; the queue must also retire its
            // retained command buffers before releasing the device dependency.
            std::error_code queue_error{};
            if (m_queue) {
                const hal::ProfileScope queue_wait{
                    "Checkpoint.GpuOwner.QueueRetirement", hal::profile_blocking_category};
                PPR_RETAIN_ERROR_ON_FAIL(CheckpointTests, queue_error, make_error_code(m_queue->waitOnHost()));
            }

            if (queue_error)
                hal::fatalExit("checkpoint-test-queue-completion-unknown");

            m_queue.setNull();

            const auto rhi_error = m_rhi_attempted ? m_rhi->shutdown() : std::error_code{};
            const auto shader_error = m_shaders_attempted ? m_shaders->shutdown() : std::error_code{};
            if (driver_error or rhi_error or shader_error)
                hal::fatalExit("checkpoint-test-dependent-cleanup-failure");
        }

        void initialize(RenderLane *const lane = nullptr, const bool validation = true) {
            m_lane = lane;

            m_shaders_attempted = true;
            PPR_TEST_ASSERT(not m_shaders->initialize());

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
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == backend);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            PPR_TEST_ASSERT(not completeOwned(
                m_runtime.beginInitialize(lane, m_rhi->getDevice(), *m_shaders, std::filesystem::current_path())));
        }

        void admitPrepared(PreparedWorld prepared, const sim::GenerationExecution &execution) {
            m_catalog = prepared.catalogOwner();
            m_driver = std::make_unique<ColonyDriver>();
            PPR_TEST_ASSERT(not m_driver->initPrepared({.m_seed = 0x2aull}, std::move(prepared), &execution));

            for (;;) {
                const auto ready = m_driver->advancePrepareMatter(m_runtime);
                PPR_TEST_ASSERT(ready.has_value());
                if (*ready)
                    break;

                std::this_thread::yield();
            }

            for (;;) {
                const auto ready = m_driver->advanceAttachMatter(m_runtime);
                PPR_TEST_ASSERT(ready.has_value());
                if (*ready)
                    break;

                std::this_thread::yield();
            }

            captureCurrent();
            rememberAdmission();
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
            if (m_driver->captureReady() and m_driver->particleSnapshotCurrent())
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
    // attached GPU edit and cold restore continuation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (paused_particle_tools_preserve_prefix_ids_and_terrain_then_destroy_to_zero) {
        Fixture fixture{"tool_inventory", "frontier", 1024u};
        const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        const auto water = fixture.m_catalog->findMaterial("water")->m_numeric_id;

        GpuOwner owner;
        owner.initialize();
        owner.restore(fixture.m_catalog, checkpoint, 3u);
        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        const auto original = *driver.matterState();
        const auto tick = driver.tickCount();
        PPR_TEST_ASSERT(original.m_records.size() == 1u);

        PPR_TEST_ASSERT(driver.setRadialForce({20.0, 20.0, 1.0, -20.0}) == std::error_code{});
        PPR_TEST_ASSERT(driver.setRadialForce({20.0, 20.0, 1.0, 20.0}) == std::error_code{});
        PPR_TEST_ASSERT(driver.setRadialForce({20.0, 20.0, 0.0, 20.0}) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not driver.setRadialForce({}));

        const ParticleToolCommand spawn{20.0, 20.0, 1.0, 3u, water, EParticleToolKind::spawn};
        PPR_TEST_ASSERT(not driver.requestParticleTool(spawn));
        PPR_TEST_ASSERT(not driver.captureReady() and driver.editPending() and not driver.admitsElapsed());
        PPR_TEST_ASSERT(driver.requestParticleTool(spawn) == std::errc::device_or_resource_busy);
        owner.update(TimeSpan{});

        const auto spawned = *driver.matterState();
        PPR_TEST_ASSERT(driver.captureReady() and driver.tickCount() == tick);
        PPR_TEST_ASSERT(spawned.m_records.size() == original.m_records.size() + 3u);
        PPR_TEST_ASSERT(spawned.m_next_id == original.m_next_id + 3u);
        PPR_TEST_ASSERT(spawned.m_identity.m_publication == original.m_identity.m_publication + 1u);
        PPR_TEST_ASSERT(spawned.m_identity.m_terrain_revision == original.m_identity.m_terrain_revision);
        PPR_TEST_ASSERT(spawned.m_records.front() == original.m_records.front());
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 22u>>(spawned.m_particles.front()) ==
            std::bit_cast<std::array<u32, 22u>>(original.m_particles.front()));
        for (std::size_t row = 1u; row < spawned.m_records.size(); ++row) {
            PPR_TEST_ASSERT(spawned.m_records[row].m_id == original.m_next_id + row - 1u);
            PPR_TEST_ASSERT(spawned.m_records[row].m_material == water);
        }

        PPR_TEST_ASSERT(not driver.requestParticleTool({100.0, 100.0, 1.0, 0u, 0u, EParticleToolKind::destroy}));
        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(driver.matterState()->m_identity == spawned.m_identity);
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == spawned.m_next_id);

        const auto &old = original.m_records.front();
        PPR_TEST_ASSERT(
            not driver.requestParticleTool({old.m_x_m, old.m_y_m, 0.5, 0u, 0u, EParticleToolKind::destroy}));
        owner.update(TimeSpan{});
        const auto survivors = *driver.matterState();
        PPR_TEST_ASSERT(survivors.m_records.size() == 3u and survivors.m_next_id == spawned.m_next_id);
        PPR_TEST_ASSERT(survivors.m_identity.m_publication == spawned.m_identity.m_publication + 1u);
        for (std::size_t row = 0u; row < survivors.m_records.size(); ++row) {
            PPR_TEST_ASSERT(survivors.m_records[row] == spawned.m_records[row + 1u]);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 22u>>(survivors.m_particles[row]) ==
                std::bit_cast<std::array<u32, 22u>>(spawned.m_particles[row + 1u]));
        }

        PPR_TEST_ASSERT(not driver.requestParticleTool({20.0, 20.0, 1.0, 0u, 0u, EParticleToolKind::destroy}));
        owner.update(TimeSpan{});
        const auto &empty = *driver.matterState();
        PPR_TEST_ASSERT(empty.m_records.empty() and empty.m_particles.empty() and empty.m_gas.empty());
        PPR_TEST_ASSERT(empty.m_next_id == spawned.m_next_id and empty.m_profile.m_particle_count == 0u);
        PPR_TEST_ASSERT(empty.m_identity.m_publication == survivors.m_identity.m_publication + 1u);
        PPR_TEST_ASSERT(empty.m_identity.m_terrain_revision == original.m_identity.m_terrain_revision);
        PPR_TEST_ASSERT(driver.captureReady() and driver.tickCount() == tick and driver.particleCount() == 0u);

        // Demand the actual committed replacement buffer: an empty CPU candidate
        // alone cannot prove GPU count/metadata replacement was published.
        const auto identity = empty.m_identity;
        PPR_TEST_ASSERT(not owner.completeOwned(owner.m_runtime.beginSnapshot(identity)));
        for (;;) {
            auto request = owner.m_runtime.beginPollSnapshot(identity);
            PPR_TEST_ASSERT(request.has_value());
            auto completion = owner.m_runtime.advance(*request);
            PPR_TEST_ASSERT(completion.has_value() and completion->has_value());
            PPR_TEST_ASSERT(not (**completion).m_execution.m_operation_error);
            if ((**completion).m_readback) {
                PPR_TEST_ASSERT((**completion).m_readback->m_particles.empty());
                break;
            }

            owner.drain();
        }

        PPR_TEST_ASSERT(not owner.completeOwned(owner.m_runtime.beginEndSnapshot(identity)));

        // One update prepares the owned append but does not poll/publish it.
        // Terminal discard must drain that candidate without consuming IDs.
        PPR_TEST_ASSERT(not driver.requestParticleTool(spawn));
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.editPending() and driver.matterState()->m_identity == identity);

        std::error_code cleanup{};
        for (;;) {
            const auto detached = driver.advanceTerminalDetachMatter(cleanup);
            PPR_TEST_ASSERT(detached.has_value());
            if (*detached) {
                break;
            }

            owner.drain();
        }

        PPR_TEST_ASSERT(not cleanup and not driver.editPending() and not driver.captureReady());
        PPR_TEST_ASSERT(driver.matterState()->m_next_id == spawned.m_next_id);
        PPR_TEST_ASSERT(driver.requestParticleTool(spawn) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.setRadialForce({}));
    };

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

    void exerciseDenseDigDiagnostic(const DigRect dig, const bool confined) {
        PPR_TEST_ASSERT(dig.m_width > 0u and dig.m_width <= 16u);
        PPR_TEST_ASSERT(dig.m_height > 0u and dig.m_height <= 16u);

        const u32 particle_count = dig.m_width * dig.m_height;

        Fixture fixture;
        const auto *const basalt = fixture.m_catalog->findMaterial("basalt");
        PPR_TEST_ASSERT(basalt != nullptr and basalt->m_reference_density_kg_m3 == 2000.0);

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
        PPR_TEST_ASSERT(dig.m_min.m_x >= 8u and dig.m_min.m_y >= 8u);
        PPR_TEST_ASSERT(dig.m_min.m_x + dig.m_width + 8u <= sim::kWorldEdge);
        PPR_TEST_ASSERT(dig.m_min.m_y + dig.m_height + 4u <= sim::kWorldEdge);

        if (confined) {
            // Four solid cells enclose every edge of the actual dug cavity.
            for (u32 y = dig.m_min.m_y - 4u; y < dig.m_min.m_y + dig.m_height + 4u; ++y) {
                for (u32 x = dig.m_min.m_x - 4u; x < dig.m_min.m_x + dig.m_width + 4u; ++x)
                    PPR_TEST_ASSERT(not terrain_grid.setCell({x, y}, {basalt->m_numeric_id, 300.0f}));
            }
        } else {
            // The emitted block falls through four empty cell rows onto a
            // four-cell-thick floor, without containing side walls or a roof.
            for (u32 y = dig.m_min.m_y - 8u; y < dig.m_min.m_y - 4u; ++y) {
                for (u32 x = dig.m_min.m_x - 8u; x < dig.m_min.m_x + dig.m_width + 8u; ++x)
                    PPR_TEST_ASSERT(not terrain_grid.setCell({x, y}, {basalt->m_numeric_id, 300.0f}));
            }

            for (u32 y = dig.m_min.m_y; y < dig.m_min.m_y + dig.m_height; ++y) {
                for (u32 x = dig.m_min.m_x; x < dig.m_min.m_x + dig.m_width; ++x)
                    PPR_TEST_ASSERT(not terrain_grid.setCell({x, y}, {basalt->m_numeric_id, 300.0f}));
            }
        }

        auto terrain_snapshot = sim::captureFull(terrain_grid, checkpoint.m_descriptor.m_seed);
        PPR_TEST_ASSERT(terrain_snapshot.has_value());

        checkpoint.m_snapshot.m_deltas = std::move(terrain_snapshot->m_deltas);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, *fixture.m_catalog));

        GpuOwner owner;
        owner.initialize();
        owner.restore(fixture.m_catalog, checkpoint, 61u);

        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        PPR_TEST_ASSERT(driver.particleCount() == 0u);
        PPR_TEST_ASSERT(not driver.requestDig(dig.m_min, dig.m_width, dig.m_height));

        owner.update(TimeSpan{});
        PPR_TEST_ASSERT(not driver.editPending() and driver.captureReady());
        PPR_TEST_ASSERT(driver.digTotal() == particle_count and driver.tickCount() == checkpoint.m_tick);

        const auto emitted = *driver.matterState();
        PPR_TEST_ASSERT(emitted.m_particles.size() == particle_count and emitted.m_records.size() == particle_count);
        PPR_TEST_ASSERT(emitted.m_gas.empty() and emitted.m_next_id == 12u + particle_count);

        std::fprintf(
            stderr,
            "[dense-dug-sand-input] dig=(%u,%u,%u,%u) confined=%u rows=%u endpoints=120 "
            "source_substeps_per_tick=%u density=2000 h=%.17g thickness=%.17g; "
            "bounded dig reproducer, not full-world parity\n",
            static_cast<unsigned>(dig.m_min.m_x), static_cast<unsigned>(dig.m_min.m_y),
            static_cast<unsigned>(dig.m_width), static_cast<unsigned>(dig.m_height), static_cast<unsigned>(confined),
            static_cast<unsigned>(particle_count), static_cast<unsigned>(matter::kGameSubsteps), emitted.m_cell_metres,
            emitted.m_thickness_metres);
        std::fflush(stderr);

        for (u32 tick = 1u; tick <= 120u; ++tick) {
            std::fprintf(stderr, "[dense-dug-sand-begin] endpoint=%u\n", static_cast<unsigned>(tick));
            std::fflush(stderr);

            owner.step();
            owner.drain();
            owner.update(TimeSpan{});
            owner.captureCurrent();

            const auto &state = *driver.matterState();
            PPR_TEST_ASSERT(state.m_particles.size() == particle_count and state.m_records.size() == particle_count);
            PPR_TEST_ASSERT(driver.tickCount() == checkpoint.m_tick + tick and driver.captureReady());

            float min_x = std::numeric_limits<float>::max(), min_y = min_x;
            float max_x = std::numeric_limits<float>::lowest(), max_y = max_x;
            float max_displacement{}, max_deformation{}, max_gradient{};

            for (const auto &particle: state.m_particles) {
                min_x = std::min(min_x, particle.m_position_x);
                min_y = std::min(min_y, particle.m_position_y);
                max_x = std::max(max_x, particle.m_position_x);
                max_y = std::max(max_y, particle.m_position_y);

                max_displacement = std::max(
                    {max_displacement, std::abs(particle.m_displacement_x), std::abs(particle.m_displacement_y)});

                for (const float field: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement))
                    max_deformation = std::max(max_deformation, std::abs(field));

                for (const float field: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient))
                    max_gradient = std::max(max_gradient, std::abs(field));
            }

            std::fprintf(
                stderr,
                "[dense-dug-sand-collected] endpoint=%u tick=%llu publication=%llu rows=%zu "
                "solver_x=[%.9g,%.9g] solver_y=[%.9g,%.9g] max_abs_d=%.9g max_abs_D=%.9g max_abs_F=%.9g\n",
                static_cast<unsigned>(tick), static_cast<unsigned long long>(driver.tickCount()),
                static_cast<unsigned long long>(state.m_identity.m_publication), state.m_particles.size(),
                static_cast<double>(min_x), static_cast<double>(max_x), static_cast<double>(min_y),
                static_cast<double>(max_y), static_cast<double>(max_displacement), static_cast<double>(max_deformation),
                static_cast<double>(max_gradient));
            std::fflush(stderr);

            const auto validity = validateMatterState(
                state, driver.preparedWorld()->generated().m_plan.recipe().m_materials, kFineCellMetres,
                checkpoint.m_descriptor.m_metric.m_reference_thickness_m, kMatterGravityY);
            if (validity)
                owner.reportFailure(validity, "dense-dug-sand-published-state");

            PPR_TEST_ASSERT(not validity);
            PPR_TEST_ASSERT(state.m_identity.m_publication == emitted.m_identity.m_publication + tick);
            PPR_TEST_ASSERT(state.m_identity.m_terrain_revision == emitted.m_identity.m_terrain_revision);
        }

        PPR_TEST_ASSERT(driver.digTotal() == particle_count and driver.matterState()->m_next_id == emitted.m_next_id);
    }

    // ------------------------------------------------------------------
    // authored frontier pressure stability
    // ------------------------------------------------------------------

    void exerciseFrontierPressureStability(const bool onset_only) {
        struct Clock final : ITimerClock {
            TimePoint now() const noexcept override {
                return time::now();
            }
        } clock;

        ParallelScheduler workers{
            "Frontier pressure regression preparation", std::max(1u, std::min(8u, std::thread::hardware_concurrency())),
            4u * static_cast<std::size_t>(hal::page_granularity)};
        const sim::GenerationExecution execution{.m_scheduler = &workers};
        config::Diagnostics diagnostics;
        auto prepared = prepareWorld(
            std::filesystem::current_path() / "worldgen/default", "frontier", 0x2aull, clock, diagnostics, execution);
        PPR_TEST_ASSERT(prepared.has_value());

        GpuOwner owner;
        owner.initialize();
        owner.admitPrepared(std::move(*prepared), execution);
        auto &driver = *owner.m_driver;
        driver.setPaused(true);
        PPR_TEST_ASSERT(driver.matterState() != nullptr);
        PPR_TEST_ASSERT(driver.matterState()->m_particles.size() == 13044u);
        const auto initial = *driver.matterState();
        const double thickness = initial.m_thickness_metres;
        PPR_TEST_ASSERT(initial.m_profile.m_liquid_relaxation == 1.0f);

        u32 endpoint{}, removed_cells{};
        bool dig_requested{}, passed{};
        PPR_DEFER {
            if (not passed) {
                std::fprintf(
                    stderr, "[frontier-late-dig-failed] endpoint=%u phase=%s\n", static_cast<unsigned>(endpoint),
                    dig_requested ? "after-dig-request" : "before-dig");
                std::fflush(stderr);
                owner.reportFailure(std::make_error_code(std::errc::state_not_recoverable), "frontier-late-dig");
            }
        };

        const auto observe = [&] {
            owner.captureCurrent();
            const auto &state = *driver.matterState();
            PPR_TEST_ASSERT(state.m_particles.size() == initial.m_particles.size() + removed_cells);
            PPR_TEST_ASSERT(state.m_particles.size() == state.m_records.size());
            PPR_TEST_ASSERT(state.m_next_id == initial.m_next_id + removed_cells);
            PPR_TEST_ASSERT(state.m_gas == initial.m_gas);
            PPR_TEST_ASSERT(
                state.m_identity.m_publication ==
                initial.m_identity.m_publication + driver.tickCount() + static_cast<u64>(dig_requested));
            PPR_TEST_ASSERT(
                state.m_identity.m_terrain_revision ==
                initial.m_identity.m_terrain_revision + static_cast<u64>(dig_requested));

            for (std::size_t row = 0u; row < initial.m_particles.size(); ++row) {
                const auto &particle = state.m_particles[row];
                const auto &source = initial.m_particles[row];
                PPR_TEST_ASSERT(particle.m_mass == source.m_mass and particle.m_volume == source.m_volume);
                PPR_TEST_ASSERT(particle.m_material == source.m_material);
                PPR_TEST_ASSERT(state.m_records[row].m_id == initial.m_records[row].m_id);
            }

            float max_d{}, max_D{}, max_F{};
            std::size_t worst_row{};

            for (std::size_t row = 0u; row < state.m_particles.size(); ++row) {
                const auto &particle = state.m_particles[row];
                const float displacement = std::max(
                    std::abs(particle.m_displacement_x), std::abs(particle.m_displacement_y));
                if (displacement > max_d) {
                    max_d = displacement;
                    worst_row = row;
                }

                for (const float field: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_displacement)) {
                    max_D = std::max(max_D, std::abs(field));
                }

                for (const float field: std::bit_cast<std::array<float, 4u>>(particle.m_deformation_gradient))
                    max_F = std::max(max_F, std::abs(field));
            }

            std::fprintf(
                stderr,
                "[frontier-late-dig-collected] endpoint=%u tick=%llu phase=%s rows=%zu max_abs_d=%.9g max_abs_D=%.9g max_abs_F=%.9g worst_d_row=%zu",
                static_cast<unsigned>(endpoint), static_cast<unsigned long long>(driver.tickCount()),
                dig_requested ? "after-dig-request" : "before-dig", state.m_particles.size(),
                static_cast<double>(max_d), static_cast<double>(max_D), static_cast<double>(max_F), worst_row);
            if (not state.m_particles.empty()) {
                const auto &particle = state.m_particles[worst_row];
                std::fprintf(
                    stderr, " material=%u solver_position=(%.9g,%.9g)",
                    static_cast<unsigned>(state.m_records[worst_row].m_material),
                    static_cast<double>(particle.m_position_x), static_cast<double>(particle.m_position_y));
            }

            std::fprintf(stderr, "\n");
            std::fflush(stderr);

            const auto validity = validateMatterState(
                state, driver.preparedWorld()->generated().m_plan.recipe().m_materials, kFineCellMetres, thickness,
                kMatterGravityY);
            if (validity)
                owner.reportFailure(validity, "frontier-late-dig-published-state");

            PPR_TEST_ASSERT(not validity);
            // Healthy default-world motion is about 0.003 cells/substep. These
            // scene-specific bounds leave room for impacts while detecting the
            // former pressure runaway (displacement > 5 and affine D > 16).
            PPR_TEST_ASSERT(max_d < 0.1f and max_D < 0.1f);
        };

        observe();
        const u32 last_endpoint = onset_only ? 320u : 3700u;
        for (endpoint = 1u; endpoint <= last_endpoint; ++endpoint) {
            if (not onset_only and endpoint == 3601u) {
                // Choose a nearby 5x7 selection with the same 23 emitted cells
                // as the reported failure, using only the authored terrain.
                std::optional<DigRect> selected;
                for (u32 y = 458u; y <= 498u and not selected; ++y) {
                    for (u32 x = 3256u; x <= 3296u and not selected; ++x) {
                        u32 diggable{};
                        bool valid = true;
                        for (u32 row = 0u; row < 7u; ++row) {
                            for (u32 col = 0u; col < 5u; ++col) {
                                const auto cell = driver.grid().getCell({x + col, y + row});
                                PPR_TEST_ASSERT(cell.has_value());
                                if (cell->m_element == 0u)
                                    continue;

                                const auto *const material = driver.contentCatalog()->findMaterial(cell->m_element);
                                PPR_TEST_ASSERT(material != nullptr);
                                if (material->m_representation != EMaterialRepresentation::terrain or
                                    material->m_phase != EMaterialPhase::solid) {
                                    valid = false;
                                    continue;
                                }

                                diggable += material->m_diggable.value_or(false) ? 1u : 0u;
                            }
                        }

                        if (valid and diggable == 23u)
                            selected = DigRect{{x, y}, 5u, 7u};
                    }
                }

                PPR_TEST_ASSERT(selected.has_value());
                const DigRect dig = *selected;
                const auto before = driver.digTotal();
                dig_requested = true;
                PPR_TEST_ASSERT(not driver.requestDig(dig.m_min, dig.m_width, dig.m_height));

                owner.update(TimeSpan{});
                PPR_TEST_ASSERT(not driver.editPending());
                removed_cells = static_cast<u32>(driver.digTotal() - before);
                PPR_TEST_ASSERT(removed_cells == 23u);
                std::fprintf(
                    stderr, "[frontier-late-dig-edit] tick=%llu dig=(%u,%u,5,7) removed=%u total=%llu\n",
                    static_cast<unsigned long long>(driver.tickCount()), static_cast<unsigned>(dig.m_min.m_x),
                    static_cast<unsigned>(dig.m_min.m_y), static_cast<unsigned>(removed_cells),
                    static_cast<unsigned long long>(driver.digTotal()));
                std::fflush(stderr);
                observe();
            }

            owner.step();
            owner.drain();
            owner.update(TimeSpan{});
            PPR_TEST_ASSERT(driver.tickCount() == endpoint);

            if (endpoint % 60u == 0u or (dig_requested and endpoint % 10u == 0u) or endpoint == last_endpoint)
                observe();
        }

        passed = true;
    }

    PPR_UNIT_TEST (frontier_seed_42_five_by_seven_dig_stays_stable_for_3700_ticks) {
        exerciseFrontierPressureStability(false);
    };

    PPR_UNIT_TEST (frontier_seed_42_liquids_stay_stable_past_prior_explosion) {
        exerciseFrontierPressureStability(true);
    };

    // ------------------------------------------------------------------
    // sustained actual dug-sand integration
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (dense_dug_sand_cavity_preserves_valid_resident_state_for_120_ticks) {
        exerciseDenseDigDiagnostic({{120u, 40u}, 16u, 16u}, true);
    };

    PPR_UNIT_TEST (dense_dug_sand_drop_preserves_valid_resident_state_for_120_ticks) {
        exerciseDenseDigDiagnostic({{3274u, 482u}, 16u, 16u}, false);
    };

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

                const double x = static_cast<double>(particle.m_position_x) - static_cast<double>(kMatterOrigin);
                const double y = static_cast<double>(particle.m_position_y) - static_cast<double>(kMatterOrigin);
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
                const double det_f = static_cast<double>(f[0u]) * static_cast<double>(f[3u]) -
                                     static_cast<double>(f[2u]) * static_cast<double>(f[1u]);
                const double dx = static_cast<double>(particle.m_position_x) -
                                  static_cast<double>(previous[row].m_position_x);
                const double dy = static_cast<double>(particle.m_position_y) -
                                  static_cast<double>(previous[row].m_position_y);
                const double applied = std::sqrt(dx * dx + dy * dy);
                const double carried = std::sqrt(
                    static_cast<double>(particle.m_displacement_x) * static_cast<double>(particle.m_displacement_x) +
                    static_cast<double>(particle.m_displacement_y) * static_cast<double>(particle.m_displacement_y));

                kernel_mass += static_cast<double>(particle.m_mass);
                nominal_volume += static_cast<double>(particle.m_volume);
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
            detail::checkpoint::paused_particle_tools_preserve_prefix_ids_and_terrain_then_destroy_to_zero,
            detail::checkpoint::attached_dig_preparation_retry_publishes_inventory_once_without_render_ack,
            detail::checkpoint::coverage_deferral_keeps_cohort_then_evolution_allows_a_later_tick_retry,
            detail::checkpoint::identically_restored_attached_owners_preserve_columns_and_resume_the_same_tick,
            detail::checkpoint::dug_sand_sustains_sixty_runtime_ticks_on_shared_terrain,
            detail::checkpoint::frontier_seed_42_five_by_seven_dig_stays_stable_for_3700_ticks,
            detail::checkpoint::frontier_seed_42_liquids_stay_stable_past_prior_explosion,
            detail::checkpoint::dense_dug_sand_cavity_preserves_valid_resident_state_for_120_ticks,
            detail::checkpoint::dense_dug_sand_drop_preserves_valid_resident_state_for_120_ticks,
        });
    };

    const UnitTest &gameCheckpointGpuTests() noexcept {
        return checkpoint_gpu;
    }
}
