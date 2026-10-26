module;
#include "pP/Macros.h"

module game.colony.driver;

import engine.matter;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyMatter, info, none)

    // ------------------------------------------------------------------
    // device attachment and durable ownership
    // ------------------------------------------------------------------

    Expected<std::optional<MatterCompletion>> ColonyDriver::advanceMatterRequest(
        MatterRuntime &runtime, const EMatterOperation operation,
        const std23::function_ref<Expected<MatterRequest>()> begin) {
        if (not m_matter_request) {
            auto request = begin();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, request);
            m_matter_request = *request;
            m_matter_operation = operation;
        }

        if (m_matter_operation != operation) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::errc::operation_not_permitted);
        }

        auto result = runtime.advance(*m_matter_request);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, result);
        if (*result) {
            m_matter_request.reset();
            m_matter_operation = EMatterOperation::idle;
            if (not (**result).m_execution.m_ran) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::errc::operation_canceled);
            }
        }

        return result;
    }

    Expected<bool> ColonyDriver::advancePrepareMatter(MatterRuntime &runtime) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter,
            not m_matter_terminal ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        if (m_matter_request) {
            auto completion = advanceMatterRequest(
                runtime, EMatterOperation::prepare, [&] { return runtime.beginCancelWorld(); });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
            return true;
        }

        const hal::ProfileScope scope{"Colony.Matter.Prepare"};
        if (m_matter_runtime != nullptr or not captureReady() or tickHz() != 60u) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::errc::operation_not_permitted);
        }

        if (not m_matter) {
            auto particles = captureParticles();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, particles);

            auto state = makeMatterState(
                *particles, 1.0 / cellsPerMetre(), referenceThicknessMetres(), kGravityY, worldGeneration());
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, state);
            state->m_identity.m_terrain_revision = 1u;
            m_matter = std::move(*state);
        }

        Array<u16> solids;
        if (const auto *const catalog = contentCatalog()) {
            for (const auto &material: catalog->materials()) {
                if (material.m_representation == EMaterialRepresentation::terrain and
                    material.m_phase == EMaterialPhase::solid) {
                    solids.push_back(material.m_numeric_id);
                }
            }
        } else {
            // The explicit legacy fixture uses the same nonzero-solid contract
            // as copyColliderChunk; it has no material catalog or matter seeds.
            for (u32 id = 1u; id <= std::numeric_limits<u16>::max(); ++id) {
                solids.push_back(static_cast<u16>(id));
            }
        }

        auto terrain = makeMatterTerrain(grid(), solids);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, terrain);
        m_preparing_runtime = &runtime;
        auto completion = advanceMatterRequest(runtime, EMatterOperation::prepare, [&] {
            if (m_submission_observer) {
                m_submission_observer->observeAdmission(
                    *m_matter, *terrain, kMatterTerrainEdge, kMatterTerrainEdge, 2.5f, 2.5f, tickHz());
            }

            return runtime.beginPrepareResidentWorld(*m_matter, std::move(*terrain));
        });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        return true;
    }

    Expected<bool> ColonyDriver::advanceAttachMatter(MatterRuntime &runtime) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter,
            not m_matter_terminal ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        if (not m_matter or m_matter_runtime != nullptr or (not m_matter_request and not captureReady())) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::errc::operation_not_permitted);
        }

        m_preparing_runtime = &runtime;
        auto completion = advanceMatterRequest(
            runtime, EMatterOperation::attach, [&] { return runtime.beginPublishWorld(m_matter->m_identity); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);

        // Publication/validation already ran. Settle CPU attachment before
        // reporting an independent terminal transport failure.
        m_matter_runtime = &runtime;
        m_preparing_runtime = nullptr;
        const auto presentation_world = m_particle_view ? std::optional{m_particle_view->m_world} : std::nullopt;
        m_particle_view = (**completion).m_view;
        if (m_particle_view and presentation_world) {
            m_particle_view->m_world = *presentation_world;
        }

        PPR_ASSERT(m_particle_view.has_value());
        m_particle_view->m_joint_tick = m_tick_count;
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        return true;
    }

    Expected<bool> ColonyDriver::advanceResumeMatter(MatterRuntime &runtime) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter,
            not m_matter_terminal ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        if (not m_matter or m_matter_runtime != nullptr or (not m_matter_request and not captureReady())) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::errc::operation_not_permitted);
        }

        m_preparing_runtime = &runtime;
        auto completion = advanceMatterRequest(
            runtime, EMatterOperation::resume, [&] { return runtime.beginResumeWorld(m_matter->m_identity); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);

        // Publication/validation already ran. Settle CPU attachment before
        // reporting an independent terminal transport failure.
        m_matter_runtime = &runtime;
        m_preparing_runtime = nullptr;
        const auto presentation_world = m_particle_view ? std::optional{m_particle_view->m_world} : std::nullopt;
        m_particle_view = (**completion).m_view;
        if (m_particle_view and presentation_world) {
            m_particle_view->m_world = *presentation_world;
        }

        PPR_ASSERT(m_particle_view.has_value());
        m_particle_view->m_joint_tick = m_tick_count;
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        return true;
    }

    Expected<bool> ColonyDriver::advanceDetachMatter() {
        if (m_matter_terminal) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyMatter, not matterDeviceOwned() and not m_matter_request and not m_matter_pending
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::operation_not_permitted));
            return true;
        }

        // Complete the already-mutated prefix. No newly admitted tick or tool
        // work may cross this state-preserving ownership boundary.
        if (m_matter_pending) {
            auto finished = finishMatterPhysics();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, finished);
            if (not *finished) {
                return false;
            }

            --m_held_slices;
            ++m_tick_count;
            m_resume_edit_tick = false;
            if (m_pending_single_steps != 0u) {
                --m_pending_single_steps;
            }

            refreshAgents();
        }

        if (m_edit and m_matter_runtime) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, prepareEditColliders());
            if (m_edit) {
                return false;
            }
        }

        // A preparation or irreversible attachment already accepted must first
        // acknowledge against the original owner before any cancellation.
        if (m_preparing_runtime and m_matter_request and m_matter_operation != EMatterOperation::cancel) {
            auto *const runtime = m_preparing_runtime;
            Expected<bool> settled = m_matter_operation == EMatterOperation::prepare  ? advancePrepareMatter(*runtime)
                                     : m_matter_operation == EMatterOperation::attach ? advanceAttachMatter(*runtime)
                                                                                      : advanceResumeMatter(*runtime);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, settled);
            if (not *settled) {
                return false;
            }
        }

        if (m_preparing_runtime) {
            auto &runtime = *m_preparing_runtime;
            auto completion = advanceMatterRequest(
                runtime, EMatterOperation::cancel, [&] { return runtime.beginCancelWorld(); });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);
            m_preparing_runtime = nullptr;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        }

        if (m_matter_runtime != nullptr) {
            const auto captured = advanceMatterSnapshot();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, captured);
            if (not *captured) {
                return false;
            }
        }

        if (not m_capture_awake_valid and m_physics_ready) {
            m_capture_awake.clear();
            for (const auto &body: m_bodies) {
                const auto awake = m_scene.isAwake(body.m_handle);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, awake);
                m_capture_awake.emplace_back(body.m_entity, *awake);
            }

            m_capture_awake_valid = true;
        }

        m_matter_runtime = nullptr;
        return true;
    }

    Expected<bool> ColonyDriver::advanceTerminalDetachMatter(std::error_code &first_error) {
        m_matter_terminal = true;
        PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, first_error, m_step_error);

        // A retained request must settle on its exact original owner, including
        // publication callbacks already admitted before terminal admission stopped.
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter, not m_preparing_runtime or not m_matter_runtime or m_preparing_runtime == m_matter_runtime
                              ? std::error_code{}
                              : std::make_error_code(std::errc::state_not_recoverable));
        auto *const runtime = m_preparing_runtime ? m_preparing_runtime : m_matter_runtime;
        if (not runtime) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyMatter, not m_matter_request and not m_matter_pending
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::state_not_recoverable));
            return true;
        }

        const auto failure = runtime->failure();
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, failure);
        PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, first_error, failure->m_cause);

        if (m_matter_request and m_matter_operation != EMatterOperation::terminal_discard) {
            auto completion = runtime->advance(*m_matter_request);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            const auto &execution = (**completion).m_execution;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyMatter,
                execution.m_ran ? std::error_code{} : std::make_error_code(std::errc::operation_canceled));
            PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, first_error, execution.m_operation_error);
            PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, first_error, execution.m_transport_error);
            m_matter_request.reset();
            m_matter_operation = EMatterOperation::idle;
        }

        if (not m_matter_request) {
            auto request = runtime->beginDiscard();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, request);
            m_matter_request = *request;
            m_matter_operation = EMatterOperation::terminal_discard;
        }

        auto completion = runtime->advance(*m_matter_request);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        const auto &execution = (**completion).m_execution;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter, execution.m_ran ? std::error_code{} : std::make_error_code(std::errc::operation_canceled));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, execution.m_operation_error);
        PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, first_error, execution.m_transport_error);

        // Only successful drain/discard plus its matching acknowledgement release
        // readers and publication state. The abandoned Scene never commits/steps.
        m_matter_request.reset();
        m_matter_operation = EMatterOperation::idle;
        m_matter_publication.reset();
        m_matter_bodies.clear();
        m_particle_view.reset();
        m_snapshot_ready.reset();
        m_snapshot_work.reset();
        m_snapshot_phase = ESnapshotPhase::idle;
        m_matter_pending = false;
        m_held_slices = 0u;
        m_pending_single_steps = 0u;
        m_preparing_runtime = nullptr;
        m_matter_runtime = nullptr;
        return true;
    }

    bool ColonyDriver::matterServicePending() const noexcept {
        return m_matter_request.has_value() or m_matter_pending or m_snapshot_phase != ESnapshotPhase::idle;
    }

    bool ColonyDriver::matterAttachmentPending() const noexcept {
        return m_matter_operation == EMatterOperation::attach;
    }

    bool ColonyDriver::matterDeviceOwned() const noexcept {
        return m_matter_runtime != nullptr or m_preparing_runtime != nullptr;
    }

    std::error_code ColonyDriver::prepareMatter(MatterRuntime &runtime) {
        const auto ready = advancePrepareMatter(runtime);
        PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, ready);
        if (not *ready) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, std::errc::operation_in_progress);
        }

        return {};
    }

    std::error_code ColonyDriver::attachMatter(MatterRuntime &runtime) {
        const auto ready = advanceAttachMatter(runtime);
        PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, ready);
        if (not *ready) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, std::errc::operation_in_progress);
        }

        return {};
    }

    std::error_code ColonyDriver::resumeMatter(MatterRuntime &runtime) {
        const auto ready = advanceResumeMatter(runtime);
        PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, ready);
        if (not *ready) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, std::errc::operation_in_progress);
        }

        return {};
    }

    std::error_code ColonyDriver::detachMatter() {
        const auto ready = advanceDetachMatter();
        PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, ready);
        if (not *ready) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyMatter, std::errc::operation_in_progress);
        }

        return {};
    }

    bool ColonyDriver::matterAttached() const noexcept {
        return m_matter_runtime != nullptr;
    }

    bool ColonyDriver::physicsPending() const noexcept {
        return m_matter_pending;
    }

    bool ColonyDriver::admitsElapsed() const noexcept {
        return not m_matter_terminal and
               m_timestep.has_value() and
               not m_paused and
               not m_edit and
               not m_matter_pending and
               m_snapshot_phase == ESnapshotPhase::idle and
               m_held_slices == 0u and
               m_pending_single_steps == 0u;
    }

    const MatterState *ColonyDriver::matterState() const noexcept {
        return m_matter and m_cpu_particles_current ? &*m_matter : nullptr;
    }

    std::optional<GridParticleGpuView> ColonyDriver::particleView() const noexcept {
        return m_particle_view;
    }

    void ColonyDriver::bindParticlePresentation(const GridWorldIdentity world) noexcept {
        if (m_particle_view) {
            m_particle_view->m_world = world;
        }
    }

    bool ColonyDriver::particleSnapshotCurrent() const noexcept {
        return m_cpu_particles_current;
    }

    u64 ColonyDriver::particleSnapshotTick() const noexcept {
        return m_cpu_particles_current ? m_tick_count : m_snapshot_tick;
    }

    Expected<MatterState> ColonyDriver::captureMatterState() const {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter,
            not m_matter_terminal ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter, m_cpu_particles_current ? std::error_code{}
                                                  : std::make_error_code(std::errc::resource_unavailable_try_again));

        if (m_matter)
            return *m_matter;

        auto particles = captureParticles();
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, particles);

        auto initialized = makeMatterState(
            *particles, 1.0 / cellsPerMetre(), referenceThicknessMetres(), kGravityY, worldGeneration());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, initialized);
        initialized->m_identity.m_terrain_revision = 1u;
        return initialized;
    }

    std::error_code ColonyDriver::takeEditRejection() noexcept {
        return std::exchange(m_edit_rejection, std::error_code{});
    }

    // ------------------------------------------------------------------
    // exact joint-tick demand snapshots
    // ------------------------------------------------------------------

    Expected<bool> ColonyDriver::advanceMatterSnapshot(ParallelScheduler *const workers) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyMatter,
            not m_matter_terminal ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        if (workers != nullptr) {
            m_snapshot_workers = workers;
        }

        if (m_snapshot_phase == ESnapshotPhase::idle) {
            if (m_cpu_particles_current) {
                m_snapshot_tick = m_tick_count;
                return true;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyMatter, m_matter_runtime != nullptr and
                                      m_snapshot_workers != nullptr and
                                      not m_matter_pending and
                                      not m_edit and
                                      not m_matter_request
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::device_or_resource_busy));

            m_snapshot_error = {};
            m_snapshot_phase = ESnapshotPhase::begin;
        }

        if (m_snapshot_phase == ESnapshotPhase::begin or m_snapshot_phase == ESnapshotPhase::poll) {
            const bool begin = m_snapshot_phase == ESnapshotPhase::begin;
            auto completion = advanceMatterRequest(
                *m_matter_runtime, begin ? EMatterOperation::snapshot_begin : EMatterOperation::snapshot_poll, [&] {
                    return begin ? m_matter_runtime->beginSnapshot(m_matter->m_identity)
                                 : m_matter_runtime->beginPollSnapshot(m_matter->m_identity);
                });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            // A failed admitted copy still owns its source. Release that freeze
            // through the same lane before exposing the first terminal error.
            PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, m_snapshot_error, (**completion).m_execution.m_operation_error);
            PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, m_snapshot_error, (**completion).m_execution.m_transport_error);
            if (m_snapshot_error) {
                m_snapshot_phase = ESnapshotPhase::end;
            } else if (begin) {
                m_snapshot_phase = ESnapshotPhase::poll;
                return false;
            } else if (not (**completion).m_readback) {
                return false;
            } else {
                m_snapshot_work = std::make_shared<DemandSnapshot>();
                m_snapshot_work->m_matter = *m_matter;
                m_snapshot_work->m_matter.m_particles = std::move((**completion).m_readback->m_particles);
                m_snapshot_phase = ESnapshotPhase::reconstruct;
            }
        }

        if (m_snapshot_phase == ESnapshotPhase::reconstruct) {
            if (not m_snapshot_task.valid()) {
                const auto work = m_snapshot_work;
                Array<sim::GenerationMaterial> materials;
                if (const auto *const prepared = preparedWorld()) {
                    const auto &source = prepared->generated().m_plan.recipe().m_materials;
                    materials.assign(source.begin(), source.end());
                }

                auto task = m_snapshot_workers->schedule(
                    [work, materials = std::move(materials)](const IContext &context) -> Expected<DemandSnapshot> {
                        const hal::ProfileScope scope{"Colony.Matter.Snapshot.Reconstruct"};
                        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, context.error());

                        auto &source = work->m_matter;
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyMatter, source.m_particles.size() == source.m_records.size()
                                              ? std::error_code{}
                                              : std::make_error_code(std::errc::invalid_argument));

                        for (std::size_t row = 0u; row < source.m_records.size(); ++row) {
                            source.m_records[row].m_x_m = (static_cast<double>(source.m_particles[row].m_position_x) -
                                                              kMatterOrigin) *
                                                          source.m_cell_metres;
                            source.m_records[row].m_y_m = (static_cast<double>(source.m_particles[row].m_position_y) -
                                                              kMatterOrigin) *
                                                          source.m_cell_metres;
                        }

                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyMatter,
                            validateMatterState(
                                source, materials, source.m_cell_metres, source.m_thickness_metres, kGravityY));

                        auto particles = captureMovingMatter(source, source.m_particles);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, particles);

                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            ColonyMatter, work->m_particles.restoreMoving(
                                              *particles, materials, source.m_cell_metres, source.m_thickness_metres));

                        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, context.error());
                        return std::move(*work);
                    },
                    std::nullopt, ParallelScheduler::drop_if_full);
                if (not task and task.error() == make_error_code(RawChannel::error_full)) {
                    return false;
                }

                if (not task) {
                    m_snapshot_error = task.error();
                    m_snapshot_phase = ESnapshotPhase::end;
                } else {
                    m_snapshot_task = std::move(*task);
                }
            }

            if (m_snapshot_task.valid()) {
                if (m_snapshot_task.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                    return false;
                }

                auto reconstructed = m_snapshot_task.get();
                if (reconstructed) {
                    m_snapshot_ready = std::move(*reconstructed);
                } else {
                    m_snapshot_error = reconstructed.error();
                }

                m_snapshot_work.reset();
                m_snapshot_phase = ESnapshotPhase::end;
            }
        }

        auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::snapshot_end, [&] {
            return m_matter_runtime->beginEndSnapshot(m_matter->m_identity);
        });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, m_snapshot_error, (**completion).m_execution.m_operation_error);
        PPR_RETAIN_ERROR_ON_FAIL(ColonyMatter, m_snapshot_error, (**completion).m_execution.m_transport_error);
        m_snapshot_phase = ESnapshotPhase::idle;
        m_snapshot_work.reset();
        if (m_snapshot_error) {
            m_snapshot_ready.reset();
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, std::exchange(m_snapshot_error, std::error_code{}));
        }

        PPR_ASSERT(m_snapshot_ready.has_value());
        m_matter = std::move(m_snapshot_ready->m_matter);
        m_colony.particles() = std::move(m_snapshot_ready->m_particles);
        m_snapshot_ready.reset();
        m_cpu_particles_current = true;
        m_snapshot_tick = m_tick_count;
        return true;
    }

    // ------------------------------------------------------------------
    // pending final physics phase
    // ------------------------------------------------------------------

    Expected<bool> ColonyDriver::finishMatterPhysics() {
        const hal::ProfileScope scope{"Colony.Matter.Complete"};
        if (m_matter_operation == EMatterOperation::submit) {
            auto completion = advanceMatterRequest(
                *m_matter_runtime, EMatterOperation::submit, [&] { return m_matter_runtime->beginDiscard(); });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        }

        if (not m_matter_publication) {
            auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::poll, [&] {
                return m_matter_runtime->beginPoll(m_matter->m_identity);
            });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
            if (not *completion) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
            if (not (**completion).m_readback) {
                // An acknowledged query may still observe an unsignalled GPU fence.
                return false;
            }

            auto &candidate = (**completion).m_readback.value();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyMatter, candidate.m_source == m_matter->m_identity and candidate.m_particles.empty()
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::state_not_recoverable));

            const physics::ParticleCouplingEpoch epoch{
                m_world_generation, m_tick_count, m_matter->m_identity.m_terrain_revision};
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, m_scene.validateParticleImpulses(epoch, {}));

            // The one-way route still rejects a body mutation across the owner
            // lane wait, independently of the coupling frontier and terrain tag.
            for (const auto &body: m_matter_bodies) {
                const auto current = m_scene.state(body.m_handle);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, current);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyMatter, *current == body.m_state ? std::error_code{}
                                                           : std::make_error_code(std::errc::state_not_recoverable));
            }

            MatterPublication publication{};
            publication.m_candidate = std::move(candidate);
            m_matter_publication = std::move(publication);
        }

        auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::publish, [&] {
            return m_matter_runtime->beginPublish(m_matter->m_identity);
        });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, completion);
        if (not *completion) {
            return false;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_operation_error);

        const physics::ParticleCouplingEpoch epoch{
            m_world_generation, m_tick_count, m_matter->m_identity.m_terrain_revision};
        // Main owns this pending tick exclusively across the lane wait. Any
        // pumped controls/results are deferred by the application's service guard.
        // A post-publication rejection cannot become an ordinary retry.
        if (m_scene.commitParticleCoupling(epoch) or m_scene.step()) {
            hal::fatalExit("matter-dependent-physics-commit-failure");
        }

        mirrorBodyStates();

        ++m_matter->m_identity.m_publication;
        const auto presentation_world = m_particle_view ? std::optional{m_particle_view->m_world} : std::nullopt;
        m_particle_view = (**completion).m_view;
        if (m_particle_view and presentation_world) {
            m_particle_view->m_world = *presentation_world;
        }

        PPR_ASSERT(m_particle_view.has_value());
        m_particle_view->m_joint_tick = m_tick_count + 1u;
        // A checked zero moving inventory has no dynamic coordinates to become
        // stale; unsimulated gas remains exact without any particle readback.
        m_cpu_particles_current = m_matter->m_profile.m_particle_count == 0u;
        if (m_cpu_particles_current) {
            m_snapshot_tick = m_tick_count + 1u;
        }
        m_capture_awake_valid = false;
        // Release the old demand snapshot once. Ordinary resident ticks retain
        // immutable inventory metadata and perform no particle traversal.
        m_matter->m_particles = {};
        m_matter_publication.reset();
        m_matter_pending = false;
        m_matter_bodies.clear();
        m_initial_matter_baseline_valid = false;
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyMatter, (**completion).m_execution.m_transport_error);
        return true;
    }
}
