module;
#include "pP/Macros.h"
module game.colony.driver;
import game.colony.digtool;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyEdits, info, none)

    // ------------------------------------------------------------------
    // edit publication
    // ------------------------------------------------------------------

    void ColonyDriver::requireEditPublication() noexcept {
        m_require_edit_publication = true;
    }

    bool ColonyDriver::editPending() const noexcept {
        return m_edit.has_value() or m_pending_particle_tool.has_value();
    }

    bool ColonyDriver::editReadyForPresentation() const noexcept {
        return m_edit and m_edit->m_colliders_ready and (not m_edit->m_matter or m_edit->m_matter_copied);
    }

    std::span<const sim::ChunkGrid::CapturedChunk> ColonyDriver::editChunks() const noexcept {
        return editReadyForPresentation() ? std::span<const sim::ChunkGrid::CapturedChunk>{m_edit->m_chunks}
                                          : std::span<const sim::ChunkGrid::CapturedChunk>{};
    }

    std::span<const ColonyDriver::DirtyChunk> ColonyDriver::editManifest() const noexcept {
        return editReadyForPresentation() ? std::span<const DirtyChunk>{m_edit->m_manifest}
                                          : std::span<const DirtyChunk>{};
    }

    // ------------------------------------------------------------------
    // bounded service-bay publication
    // ------------------------------------------------------------------

    Expected<bool> ColonyDriver::prepareServiceDoorSnapshot() {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, preparedWorld() and
                                 preparedWorld()->lightingShowcase() and
                                 not m_service_door and
                                 not m_edit and
                                 not m_pending_particle_tool and
                                 not m_matter_terminal
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        m_door_quiescing = true;
        clearPlayerCommand();

        if (not m_door_backlog) {
            // Only the submitted slice has execution authority. Completing it
            // must not run cached elapsed-time slices as update(0) would do.
            if (m_matter_pending) {
                const auto finished = finishMatterPhysics();
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, finished);
                if (not *finished) {
                    return false;
                }

                PPR_ASSERT(m_held_slices != 0u);
                --m_held_slices;
                ++m_tick_count;
                m_resume_edit_tick = false;
                if (m_pending_single_steps != 0u) {
                    --m_pending_single_steps;
                }

                refreshAgents();
            }

            m_door_backlog = std::array{std::exchange(m_held_slices, 0u), std::exchange(m_pending_single_steps, 0u)};
        }

        const auto captured = advanceMatterSnapshot();
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, captured);
        return *captured;
    }

    std::error_code ColonyDriver::releaseServiceDoorSnapshot() {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, not m_service_door and
                                 m_door_quiescing and
                                 m_door_backlog and
                                 m_snapshot_phase == ESnapshotPhase::idle and
                                 not m_matter_request and
                                 not m_matter_pending
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        PPR_ASSERT(m_held_slices == 0u and m_pending_single_steps == 0u);
        m_held_slices = (*m_door_backlog)[0u];
        m_pending_single_steps = (*m_door_backlog)[1u];
        m_door_backlog.reset();
        m_door_quiescing = false;
        return {};
    }

    Expected<u64> ColonyDriver::requestServiceDoor(const bool closed, const u64 expected_revision) {
        return requestServiceBay_(EServiceBayTarget::door, closed, expected_revision);
    }

    Expected<u64> ColonyDriver::requestServiceLamp(const bool enabled, const u64 expected_revision) {
        return requestServiceBay_(EServiceBayTarget::amber_lamp, enabled, expected_revision);
    }

    Expected<u64> ColonyDriver::requestServiceLampPosition(
        const EServiceLampPosition position, const u64 expected_revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, position == EServiceLampPosition::original or position == EServiceLampPosition::alternate
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        return requestServiceBay_(
            EServiceBayTarget::amber_lamp_position, position == EServiceLampPosition::alternate, expected_revision);
    }

    Expected<u64> ColonyDriver::requestServiceBay_(
        const EServiceBayTarget target, const bool value, const u64 expected_revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits,
            preparedWorld() and preparedWorld()->lightingShowcase() and m_timestep and not m_matter_terminal
                ? std::error_code{}
                : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, not m_service_door and
                                 not m_edit and
                                 not m_pending_particle_tool and
                                 m_pending_digs.empty() and
                                 captureReady() and
                                 not m_matter_request and
                                 not m_matter_pending
                             ? std::error_code{}
                             : std::make_error_code(std::errc::device_or_resource_busy));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, expected_revision == m_lighting_revision and
                                 value != (target == EServiceBayTarget::door ? m_service_door_closed
                                              : target == EServiceBayTarget::amber_lamp
                                                  ? m_service_lamp_enabled
                                                  : m_service_lamp_position == EServiceLampPosition::alternate)
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        const auto *const material = contentCatalog()->findMaterial("showcase_housing");
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, material and material->m_representation == EMaterialRepresentation::terrain
                             ? std::error_code{}
                             : std::make_error_code(std::errc::invalid_argument));

        const float cell = 1.0f / static_cast<float>(cellsPerMetre());
        const bool moving = target == EServiceBayTarget::amber_lamp_position;
        const u32 destination_x = value ? 37u : 30u;
        if ((target == EServiceBayTarget::door and value) or moving) {
            for (const auto &[entity, pose]: m_registry.view<sim::BodyState>()) {
                const auto *const body = m_registry.get<sim::BodyDefinition>(entity);
                if (not body) {
                    continue;
                }

                const bool overlap = pose.m_x + body->m_half_width > float(moving ? destination_x : 36u) * cell and
                                     pose.m_x - body->m_half_width < float(moving ? destination_x + 2u : 37u) * cell and
                                     pose.m_y + body->m_half_height > float(moving ? 16u : 7u) * cell and
                                     pose.m_y - body->m_half_height < float(moving ? 18u : 14u) * cell;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyEdits,
                    not overlap ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, m_door_revision_highwater != std::numeric_limits<u64>::max()
                             ? std::error_code{}
                             : std::make_error_code(std::errc::value_too_large));

        const hal::ProfileScope scope{"Colony.ServiceDoor.Prepare"};
        ServiceDoorPublication binding{};
        binding.m_revision = ++m_door_revision_highwater;
        binding.m_target = target;
        binding.m_closed = target == EServiceBayTarget::door ? value : m_service_door_closed;
        binding.m_lamp_enabled = target == EServiceBayTarget::amber_lamp ? value : m_service_lamp_enabled;
        binding.m_lamp_position = moving ? (value ? EServiceLampPosition::alternate : EServiceLampPosition::original)
                                         : m_service_lamp_position;

        EditCandidate candidate{};
        auto chunk = grid().captureChunk({0u, 0u});
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, chunk);

        grid().exchangeChunk(*chunk);
        bool restore = true;
        PPR_DEFER {
            if (restore) {
                grid().exchangeChunk(*chunk);
            }
        };

        if (target == EServiceBayTarget::door) {
            for (u32 y = 7u; y <= 13u; ++y) {
                const auto original = grid().getCell({36u, y});
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, original);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyEdits, original->m_element == (m_service_door_closed ? material->m_numeric_id : 0u)
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::state_not_recoverable));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyEdits,
                    grid().setCell({36u, y}, {value ? material->m_numeric_id : u16{0u}, original->m_temperature}));
            }
        } else {
            // Both switching and relocation require the dedicated row to
            // represent exactly the four cells at the committed slot.
            const auto *const lamp = contentCatalog()->findMaterial("showcase_amber_lamp");
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyEdits, lamp and lamp->m_representation == EMaterialRepresentation::terrain
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::invalid_argument));

            const auto cells = grid().residentCells({0u, 0u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyEdits, grid().residentChunkCount() == 1u and cells and cells->size() == sim::kCellsPerChunk
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::not_supported));

            const u32 source_x = m_service_lamp_position == EServiceLampPosition::original ? 30u : 37u;
            u32 count{};
            for (u32 y = 0u; y < sim::kChunkEdge; ++y) {
                for (u32 x = 0u; x < sim::kChunkEdge; ++x) {
                    if ((*cells)[y * sim::kChunkEdge + x].m_element != lamp->m_numeric_id) {
                        continue;
                    }

                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyEdits, x >= source_x and x < source_x + 2u and y >= 16u and y <= 17u
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::not_supported));

                    ++count;
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyEdits, count == 4u ? std::error_code{} : std::make_error_code(std::errc::not_supported));
        }

        if (moving) {
            const u32 source_x = m_service_lamp_position == EServiceLampPosition::original ? 30u : 37u;
            for (u32 y = 16u; y <= 17u; ++y) {
                for (u32 dx = 0u; dx < 2u; ++dx) {
                    const auto source = grid().getCell({source_x + dx, y});
                    const auto destination = grid().getCell({destination_x + dx, y});
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, source);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, destination);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyEdits, destination->m_element == 0u
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::device_or_resource_busy));

                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, grid().setCell({source_x + dx, y}, *destination));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, grid().setCell({destination_x + dx, y}, *source));
                }
            }
        }

        grid().exchangeChunk(*chunk);
        restore = false;
        candidate.m_chunks.push_back(std::move(*chunk));
        candidate.m_touched.push_back({0u, 0u});

        u64 chunk_revision = 1u;
        for (const auto &row: m_present_revision_history) {
            if (row.m_pos == sim::ChunkPos{0u, 0u}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyEdits, row.m_revision != std::numeric_limits<u64>::max()
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::value_too_large));
                chunk_revision = row.m_revision + 1u;
                break;
            }
        }

        candidate.m_manifest.push_back({{0u, 0u}, chunk_revision});
        if (m_matter_runtime) {
            // Supported bay changes belong to the opaque/vacuum showcase.
            // The lamp retains the same conservative chunk/collider cohort;
            // these edits create no particles and discard no moving inventory.
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyEdits, m_matter and
                                     m_cpu_particles_current and
                                     m_matter->m_records.empty() and
                                     m_matter->m_gas.empty() and
                                     m_matter->m_profile.m_particle_count == 0u
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::not_supported));

            const auto source = m_matter->m_identity;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyEdits, source.m_publication != std::numeric_limits<u64>::max() and
                                     source.m_terrain_revision != std::numeric_limits<u64>::max()
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::value_too_large));

            candidate.m_matter = *m_matter;
            ++candidate.m_matter->m_identity.m_publication;
            ++candidate.m_matter->m_identity.m_terrain_revision;
            binding.m_matter = std::array{source, candidate.m_matter->m_identity};
        }

        if (not m_door_backlog) {
            m_door_backlog = std::array{std::exchange(m_held_slices, 0u), std::exchange(m_pending_single_steps, 0u)};
        }
        m_door_quiescing = true;

        m_edit.emplace(std::move(candidate));
        m_service_door.emplace(std::move(binding));
        clearPlayerCommand();
        invalidateColliderCache();
        return m_service_door->m_revision;
    }

    Expected<bool> ColonyDriver::advanceServiceDoor(const u64 revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, m_service_door and
                                 m_service_door->m_revision == revision and
                                 not m_service_door->m_cancelling and
                                 not m_service_door->m_activation_sent and
                                 m_edit
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, prepareEditColliders());
        return editReadyForPresentation();
    }

    std::error_code ColonyDriver::markServiceDoorActivationSent(const u64 revision) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, m_service_door and
                                 m_service_door->m_revision == revision and
                                 not m_service_door->m_cancelling and
                                 not m_service_door->m_activation_sent and
                                 editReadyForPresentation()
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        m_service_door->m_activation_sent = true;
        return {};
    }

    std::error_code ColonyDriver::acknowledgeServiceDoorActivation(const u64 revision, const bool applied) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, m_service_door and
                                 m_service_door->m_revision == revision and
                                 m_service_door->m_activation_sent and
                                 not m_service_door->m_activated
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        m_service_door->m_activation_sent = applied;
        m_service_door->m_activated = applied;
        return {};
    }

    Expected<bool> ColonyDriver::commitServiceDoor(const u64 revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, m_service_door and m_service_door->m_revision == revision and m_service_door->m_activated
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));
        if (m_service_door->m_committed) {
            return true;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, m_edit and (not m_service_door->m_matter or
                                        (m_matter and
                                            m_edit->m_matter and
                                            m_matter->m_identity == (*m_service_door->m_matter)[0u] and
                                            m_edit->m_matter->m_identity == (*m_service_door->m_matter)[1u]))
                             ? std::error_code{}
                             : std::make_error_code(std::errc::state_not_recoverable));

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, settleEdit_(m_edit->m_manifest, revision));
        return m_service_door->m_committed;
    }

    Expected<bool> ColonyDriver::cancelServiceDoor(const u64 revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyEdits, m_service_door and
                                 m_service_door->m_revision == revision and
                                 not m_service_door->m_activation_sent and
                                 not m_service_door->m_activated
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        auto &binding = *m_service_door;
        binding.m_cancelling = true;

        if (m_matter_request and m_matter_operation != EMatterOperation::edit_cancel) {
            // Never reuse an internal target until the old operation actually
            // settled, even if the outer attempt is already logically canceled.
            auto completed = m_matter_runtime->advance(*m_matter_request);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, completed);
            if (not *completed) {
                return false;
            }

            PPR_RETAIN_ERROR_ON_FAIL(
                ColonyEdits, binding.m_cancel_error,
                (**completed).m_execution.m_ran ? (**completed).m_execution.m_operation_error
                                                : std::make_error_code(std::errc::operation_canceled));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, (**completed).m_execution.m_transport_error);
            m_matter_request.reset();
            m_matter_operation = EMatterOperation::idle;
        }

        if (m_matter_runtime and not binding.m_matter_cancelled) {
            auto completed = advanceMatterRequest(
                *m_matter_runtime, EMatterOperation::edit_cancel, [&] { return m_matter_runtime->beginDiscard(); });
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, completed);
            if (not *completed) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, (**completed).m_execution.m_operation_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, (**completed).m_execution.m_transport_error);
            binding.m_matter_cancelled = true;
        }

        if (m_edit->m_colliders_started) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, m_colliders.cancelCohort(m_scene));
            m_edit->m_colliders_started = false;
        }

        const auto error = binding.m_cancel_error;
        m_edit.reset();
        m_service_door.reset();
        invalidateColliderCache();
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, releaseServiceDoorSnapshot());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, error);
        return true;
    }

    std::error_code ColonyDriver::finishServiceDoor(const u64 revision) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, m_service_door and
                                 m_service_door->m_revision == revision and
                                 m_service_door->m_committed and
                                 m_lighting_revision == revision
                             ? std::error_code{}
                             : std::make_error_code(std::errc::operation_not_permitted));

        m_service_door.reset();
        clearPlayerCommand();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, releaseServiceDoorSnapshot());
        return {};
    }

    void ColonyDriver::commitDoorBinding_() noexcept {
        if (m_service_door) {
            m_service_door_closed = m_service_door->m_closed;
            m_service_lamp_enabled = m_service_door->m_lamp_enabled;
            m_service_lamp_position = m_service_door->m_lamp_position;
            m_lighting_revision = m_service_door->m_revision;
            m_service_door->m_committed = true;
        }
    }

    // ------------------------------------------------------------------
    // physical tool admission
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::setRadialForce(const RadialForce force) {
        if (force.m_acceleration_m_s2 == 0.0) {
            m_radial_force = {};
            return {};
        }

        if (m_matter_terminal or not m_timestep or not m_matter_runtime or not m_matter) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::operation_not_permitted);
        }

        const double extent = static_cast<double>(grid().edge()) / cellsPerMetre();
        if (not std::isfinite(force.m_x_m) or
            not std::isfinite(force.m_y_m) or
            not std::isfinite(force.m_radius_m) or
            not std::isfinite(force.m_acceleration_m_s2) or
            force.m_x_m < 0.0 or
            force.m_y_m < 0.0 or
            force.m_x_m >= extent or
            force.m_y_m >= extent or
            force.m_radius_m < 1.0 / 3.0 or
            force.m_radius_m > 32.0 or
            std::abs(force.m_acceleration_m_s2) > 1000.0) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::invalid_argument);
        }

        m_radial_force = force;
        return {};
    }

    std::error_code ColonyDriver::requestParticleTool(const ParticleToolCommand command) {
        if (m_matter_terminal or not m_timestep or not m_matter_runtime or not m_matter or not preparedWorld()) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::operation_not_permitted);
        }

        const double extent = static_cast<double>(grid().edge()) / cellsPerMetre();
        const bool spawn = command.m_kind == EParticleToolKind::spawn;
        if ((not spawn and command.m_kind != EParticleToolKind::destroy) or
            not std::isfinite(command.m_x_m) or
            not std::isfinite(command.m_y_m) or
            not std::isfinite(command.m_radius_m) or
            command.m_x_m < 0.0 or
            command.m_y_m < 0.0 or
            command.m_x_m >= extent or
            command.m_y_m >= extent or
            command.m_radius_m < 1.0 / 3.0 or
            command.m_radius_m > 32.0 or
            (spawn and (command.m_count == 0u or command.m_count > 4096u))) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::invalid_argument);
        }

        if (m_matter->m_identity.m_publication == std::numeric_limits<u64>::max()) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::value_too_large);
        }

        if (spawn) {
            const auto count = m_matter->m_records.size() + m_matter->m_gas.size();
            if (command.m_count > sim::kMaxMatterParticles - count or
                m_matter->m_next_id == 0u or
                command.m_count > std::numeric_limits<u64>::max() - m_matter->m_next_id) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::value_too_large);
            }

            const auto *const material = contentCatalog()->findMaterial(command.m_material);
            if (material == nullptr or
                material->m_representation != EMaterialRepresentation::particle or
                (material->m_phase != EMaterialPhase::solid and material->m_phase != EMaterialPhase::liquid)) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::invalid_argument);
            }
        }

        if (not captureReady() or
            m_pending_particle_tool or
            not m_pending_digs.empty() or
            (not m_cpu_particles_current and m_snapshot_workers == nullptr)) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::device_or_resource_busy);
        }

        m_pending_particle_tool = command;
        // An explicit tool action is a state mutation that can change the frozen
        // verdict, so the next admitted tick is evaluated from the new inventory.
        clearMatterStall();
        return {};
    }

    std::error_code ColonyDriver::prepareParticleTool() {
        PPR_ASSERT(m_pending_particle_tool and m_matter_runtime and m_matter and preparedWorld());
        if (not m_cpu_particles_current) {
            const auto captured = advanceMatterSnapshot();
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, captured);
            if (not *captured) {
                return {};
            }
        }

        const hal::ProfileScope scope{"Colony.Tools.PrepareInventory"};
        const auto command = *m_pending_particle_tool;
        MatterState working = *m_matter;
        const double radius_squared = command.m_radius_m * command.m_radius_m;
        const auto inside = [&](const double x, const double y) {
            const double dx = x - command.m_x_m, dy = y - command.m_y_m;
            return dx * dx + dy * dy <= radius_squared;
        };

        const auto before_count = working.m_records.size() + working.m_gas.size();
        if (command.m_kind == EParticleToolKind::destroy) {
            Array<sim::MatterParticle> records;
            Array<matter::GpuParticle> particles;
            records.reserve(working.m_records.size());
            particles.reserve(working.m_particles.size());

            for (std::size_t row = 0u; row < working.m_records.size(); ++row) {
                const auto &record = working.m_records[row];
                if (inside(record.m_x_m, record.m_y_m)) {
                    continue;
                }

                records.push_back(record);
                particles.push_back(working.m_particles[row]);
            }

            working.m_records = std::move(records);
            working.m_particles = std::move(particles);
            std::erase_if(
                working.m_gas, [&](const sim::MatterParticle &record) { return inside(record.m_x_m, record.m_y_m); });
        } else {
            const auto *const material = contentCatalog()->findMaterial(command.m_material);
            PPR_ASSERT(material != nullptr);
            const double cell = working.m_cell_metres;
            const auto first_x = static_cast<u32>(
                std::max(0.0, std::floor((command.m_x_m - command.m_radius_m) / cell)));
            const auto first_y = static_cast<u32>(
                std::max(0.0, std::floor((command.m_y_m - command.m_radius_m) / cell)));
            const auto end_x = static_cast<u32>(std::min(
                static_cast<double>(grid().edge()), std::floor((command.m_x_m + command.m_radius_m) / cell) + 1.0));
            const auto end_y = static_cast<u32>(std::min(
                static_cast<double>(grid().edge()), std::floor((command.m_y_m + command.m_radius_m) / cell) + 1.0));
            const u32 width = end_x - first_x, height = end_y - first_y;
            Array<u8> occupied(static_cast<std::size_t>(width) * height, u8{});

            for (const auto &record: working.m_records) {
                const auto x = std::min(static_cast<u32>(record.m_x_m / cell), grid().edge() - 1u);
                const auto y = std::min(static_cast<u32>(record.m_y_m / cell), grid().edge() - 1u);
                if (x >= first_x and y >= first_y and x < end_x and y < end_y) {
                    occupied[static_cast<std::size_t>(y - first_y) * width + x - first_x] = 1u;
                }
            }

            const double volume = cell * cell * working.m_thickness_metres;
            const double mass = material->m_reference_density_kg_m3 * volume;
            if (not std::isfinite(mass) or mass <= 0.0) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::invalid_argument);
            }

            u32 spawned{};
            for (u32 y = first_y; y < end_y and spawned < command.m_count; ++y) {
                for (u32 x = first_x; x < end_x and spawned < command.m_count; ++x) {
                    if (occupied[static_cast<std::size_t>(y - first_y) * width + x - first_x] != 0u or
                        not inside((x + 0.5) * cell, (y + 0.5) * cell)) {
                        continue;
                    }

                    // Marching terrain interpolates neighboring samples. Keep
                    // spawned centres and their cell support clear of that contour.
                    bool terrain_clear = true;
                    for (u32 sy = y == 0u ? 0u : y - 1u; sy <= std::min(y + 1u, grid().edge() - 1u); ++sy) {
                        for (u32 sx = x == 0u ? 0u : x - 1u; sx <= std::min(x + 1u, grid().edge() - 1u); ++sx) {
                            const auto terrain = grid().getCell({sx, sy});
                            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, terrain);
                            terrain_clear = terrain_clear and terrain->m_element == 0u;
                        }
                    }

                    if (not terrain_clear) {
                        continue;
                    }

                    if (working.m_next_id == 0u or
                        working.m_next_id == std::numeric_limits<u64>::max() or
                        working.m_records.size() + working.m_gas.size() == sim::kMaxMatterParticles) {
                        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::value_too_large);
                    }

                    sim::MatterParticle record{
                        working.m_next_id++, (x + 0.5) * cell, (y + 0.5) * cell, volume, mass, 300.0,
                        material->m_reference_density_kg_m3, command.m_material,
                        material->m_phase == EMaterialPhase::liquid ? sim::EGenerationMatter::liquid
                                                                    : sim::EGenerationMatter::solid};
                    matter::GpuParticle particle{};
                    particle.m_position_x = static_cast<float>(x) + 0.5f + kMatterOrigin;
                    particle.m_position_y = static_cast<float>(y) + 0.5f + kMatterOrigin;
                    particle.m_mass = static_cast<float>(mass / (1000.0 * volume));
                    particle.m_material = static_cast<float>(
                        material->m_phase == EMaterialPhase::liquid ? matter::ESourceMaterial::liquid
                                                                    : matter::ESourceMaterial::sand);
                    working.m_records.push_back(record);
                    working.m_particles.push_back(particle);
                    ++spawned;
                }
            }
        }

        if (working.m_records.size() + working.m_gas.size() == before_count) {
            m_pending_particle_tool.reset();
            return {};
        }

        if (working.m_identity.m_publication == std::numeric_limits<u64>::max()) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::value_too_large);
        }

        ++working.m_identity.m_publication;
        working.m_profile.m_particle_count = static_cast<u32>(working.m_particles.size());
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, validateMatterState(
                             working, preparedWorld()->generated().m_plan.recipe().m_materials, working.m_cell_metres,
                             working.m_thickness_metres, kGravityY));

        auto snapshot = captureMovingMatter(working, working.m_particles);
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, snapshot);
        EditCandidate candidate{};
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits, candidate.m_moving_particles.restoreMoving(
                             *snapshot, preparedWorld()->generated().m_plan.recipe().m_materials, working.m_cell_metres,
                             working.m_thickness_metres, working.m_world_edge));
        candidate.m_matter = std::move(working);
        candidate.m_inventory_only = true;
        candidate.m_replace_inventory = command.m_kind == EParticleToolKind::destroy;
        candidate.m_colliders_ready = true;

        m_edit.emplace(std::move(candidate));
        m_pending_particle_tool.reset();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());
        return {};
    }

    // ------------------------------------------------------------------
    // held ticks
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::runHeldTicks() {
        while (m_held_slices != 0u) {
            if (m_matter_pending) {
                const auto finished = finishMatterPhysics();
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, finished);
                if (not *finished) {
                    if (m_matter_stalled) {
                        // The rejected candidate ended this slice without a
                        // publication. The frozen state owns the joint tick
                        // identity, so the slice and every queued catch-up slice
                        // are dropped instead of replaying an identical tick. An
                        // explicit single-step request was attempted once and is
                        // consumed, so it cannot retry every frame.
                        m_held_slices = 0u;
                        if (m_pending_single_steps != 0u) {
                            --m_pending_single_steps;
                        }
                    }

                    return {};
                }
            } else {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_steps.runSystems(m_registry));
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_step_error);
                if (m_matter_pending or m_snapshot_phase != ESnapshotPhase::idle) {
                    return {};
                }
            }

            if (m_edit) {
                m_resume_edit_tick = true;
                // Prefix stopped before search/planning/movement. Retain this
                // slice and every already-admitted catch-up slice exactly once.
                return {};
            }

            m_resume_edit_tick = false;
            --m_held_slices;
            ++m_tick_count;
            m_counts = pP::colony::pathCounts(m_registry);
            m_errands = liveErrandCount(m_registry);
            refreshAgents();

            if (m_pending_single_steps != 0u) {
                --m_pending_single_steps;
                return {};
            }

            if (m_paused) {
                // Finish the already-admitted in-flight slice, then retain all
                // later catch-up slices until an explicit resume/single-step.
                return {};
            }
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit preparation
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEdit(const bool advance_build) {
        if (m_pending_particle_tool) {
            return prepareParticleTool();
        }

        if (m_matter_runtime != nullptr and
            not m_cpu_particles_current and
            (not m_pending_digs.empty() or (advance_build and liveErrandCount(m_registry) != 0u))) {
            const auto captured = advanceMatterSnapshot();
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, captured);
            if (not *captured) {
                return {};
            }
        }

        if (m_matter_runtime != nullptr and
            contentCatalog() == nullptr and
            (not m_pending_digs.empty() or (advance_build and liveErrandCount(m_registry) != 0u))) {
            // Numeric fixtures have no authoritative emission density.
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, std::errc::not_supported);
        }

        EditCandidate candidate{};
        auto capture = [&](const sim::ChunkPos pos) -> std::error_code {
            if (std::ranges::any_of(candidate.m_chunks, [pos](const auto &chunk) { return chunk.position() == pos; })) {
                return {};
            }
            auto chunk = grid().captureChunk(pos);
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, chunk);
            candidate.m_chunks.push_back(std::move(*chunk));
            return {};
        };

        if (advance_build) {
            for (const auto &[entity, errand]: m_registry.view<BuildErrand>()) {
                if (errand.m_progress < errand.m_total) {
                    candidate.m_errands.push_back({entity, errand.m_progress, errand.m_progress});
                }
            }
        }

        bool restore = true;
        PPR_DEFER {
            // Includes errors and fatal allocation unwinding. No partially
            // prepared CPU cells or errand progress escape this exclusive call.
            if (restore) {
                for (auto &chunk: candidate.m_chunks) {
                    grid().exchangeChunk(chunk);
                }
                for (const auto &row: candidate.m_errands) {
                    m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_original;
                }
            }
        };

        if (not m_pending_digs.empty()) {
            const auto &dig = m_pending_digs.front();
            candidate.m_has_dig = true;
            for (u32 y = dig.m_min.m_y / sim::kChunkEdge; y <= (dig.m_min.m_y + dig.m_height - 1u) / sim::kChunkEdge;
                ++y) {
                for (u32 x = dig.m_min.m_x / sim::kChunkEdge; x <= (dig.m_min.m_x + dig.m_width - 1u) / sim::kChunkEdge;
                    ++x) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, capture({x, y}));
                }
            }
            const auto dug = digCells(grid(), {dig.m_min, dig.m_width, dig.m_height}, contentCatalog());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, dug);
            candidate.m_dug = dug->m_dug;
            candidate.m_touched = dug->m_touched;
        }

        if (advance_build) {
            Pathfinder scratch{};
            // Capture only chunks actually written, bounded by this tick's
            // 64-cell budget, regardless of the number of queued errands.
            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, advanceErrands(
                                 grid(), m_registry, scratch, candidate.m_touched, kErrandCellsPerTick,
                                 contentCatalog(), std23::function_ref<std::error_code(sim::ChunkPos)>{capture}));
        }

        for (auto &row: candidate.m_errands) {
            row.m_candidate = m_registry.get<BuildErrand>(row.m_entity)->m_progress;
            m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_original;
        }

        for (auto &chunk: candidate.m_chunks) {
            grid().exchangeChunk(chunk);
        }

        restore = false;
        if (candidate.m_touched.empty()) {
            for (const auto &row: candidate.m_errands) {
                m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;
            }
            if (candidate.m_has_dig) {
                m_pending_digs.erase(m_pending_digs.begin());
            }
            return {};
        }

        std::erase_if(candidate.m_chunks, [&](const auto &chunk) {
            return std::ranges::find(candidate.m_touched, chunk.position()) == candidate.m_touched.end();
        });

        if (m_matter_runtime != nullptr) {
            auto matter = prepareMatterTerrainEdit(*m_matter, grid(), candidate.m_chunks, *contentCatalog());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, matter);
            if (not matter->has_value()) {
                // Expected coverage deferral retains the whole tool cohort.
                // Continue physics on committed terrain; retry on a later tick.
                m_edit_rejection = std::make_error_code(std::errc::device_or_resource_busy);
                return {};
            }

            auto &working = **matter;
            // An edit does not integrate position. Preserve the current physical
            // ledger exactly instead of reconstructing it through FP32 readback.
            std::array<Array<sim::MatterParticle>, sim::kChunkCount> rows{};
            for (const auto records:
                {std::span<const sim::MatterParticle>{working.m_records},
                    std::span<const sim::MatterParticle>{working.m_gas}}) {
                for (const auto &record: records) {
                    const sim::GlobalCellPos position{
                        std::min(static_cast<u32>(record.m_x_m / working.m_cell_metres), working.m_world_edge - 1u),
                        std::min(static_cast<u32>(record.m_y_m / working.m_cell_metres), working.m_world_edge - 1u)};
                    rows[sim::chunkIndexOf(sim::chunkOf(position))].push_back(record);
                }
            }

            sim::MatterParticleSnapshot snapshot{.m_next_id = working.m_next_id};
            for (u32 index = 0u; index < sim::kChunkCount; ++index) {
                if (rows[index].empty())
                    continue;

                std::ranges::sort(rows[index], {}, &sim::MatterParticle::m_id);
                snapshot.m_chunks.push_back(
                    {{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge},
                        std::make_shared<const Array<sim::MatterParticle>>(std::move(rows[index]))});
            }

            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, candidate.m_moving_particles.restoreMoving(
                                 snapshot, preparedWorld()->generated().m_plan.recipe().m_materials,
                                 working.m_cell_metres, working.m_thickness_metres, working.m_world_edge));

            candidate.m_matter = std::move(working);
        } else {
            auto particles = m_colony.particles().prepareTerrainEdit(grid(), candidate.m_chunks, 1.0 / cellsPerMetre());
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, particles);
            candidate.m_particles = std::move(*particles);
        }

        for (const auto pos: candidate.m_touched) {
            u64 revision = 1u;
            for (const auto &row: m_present_revision_history) {
                if (row.m_pos == pos) {
                    revision = row.m_revision + 1u;
                    break;
                }
            }
            PPR_RETURN_ERROR_ON_FAIL(
                ColonyEdits, revision != 0u ? std::error_code{} : std::make_error_code(std::errc::value_too_large));
            candidate.m_manifest.push_back({pos, revision});
        }

        m_edit.emplace(std::move(candidate));
        // The admitted command belongs to the candidate, so later bounded
        // FIFO overflow cannot evict it or make settlement pop another dig.
        if (m_edit->m_has_dig) {
            m_pending_digs.erase(m_pending_digs.begin());
        }

        invalidateColliderCache();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());

        if (not m_edit)
            return {}; // Attached simulation settlement has no render prerequisite.

        if (not m_require_edit_publication) {
            // Headless authored sessions publish the same prepared transaction
            // synchronously, without depending on a render acknowledgement.
            while (m_edit and not editReadyForPresentation()) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, prepareEditColliders());
            }

            if (m_edit)
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, settleEdit(m_edit->m_manifest));
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit colliders
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::prepareEditColliders() {
        PPR_ASSERT(m_edit.has_value());
        if (m_edit->m_colliders_ready and not m_edit->m_matter) {
            return {};
        }

        if (not m_edit->m_colliders_ready) {
            Array<sim::ChunkPos> needed{};
            collectColliderInterest(needed);

            if (not m_edit->m_colliders_started) {
                if (m_colliders.pendingCount() != 0u) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks({}, needed, 2u));
                    if (m_colliders.pendingCount() != 0u) {
                        return {};
                    }
                }

                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_colliders.beginCohort(m_scene));
                m_edit->m_colliders_started = true;

                // Candidate cells are exposed only inside this main-thread call;
                // cached collider views own their numeric copies. UI and rendering
                // keep the original grid throughout every inter-frame wait.
                for (auto &chunk: m_edit->m_chunks) {
                    grid().exchangeChunk(chunk);
                }
                PPR_DEFER {
                    for (auto &chunk: m_edit->m_chunks) {
                        grid().exchangeChunk(chunk);
                    }
                };

                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks(m_edit->m_touched, needed, 2u));
            } else {
                // The frozen candidate cache already owns the complete neighbor closure.
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, rebuildChunks({}, needed, 2u));
            }

            m_edit->m_colliders_ready = m_colliders.pendingCount() == 0u;
        }

        if (m_edit->m_colliders_ready and m_edit->m_matter) {
            // Reserve all bookkeeping before the first irreversible publication.
            m_present_dirty.reserve(m_present_dirty.size() + m_edit->m_manifest.size());
            m_present_revision_history.reserve(m_present_revision_history.size() + m_edit->m_manifest.size());

            if (m_edit->m_matter_prepared) {
                if (not m_edit->m_matter_copied) {
                    auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::edit_poll, [&] {
                        return m_matter_runtime->beginPollEdit(m_edit->m_matter->m_identity);
                    });
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
                    if (not *completion) {
                        return {};
                    }

                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_operation_error);
                    PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);
                    if (not (**completion).m_readback) {
                        return {};
                    }

                    m_edit->m_matter_copied = true;
                }

                return m_service_door ? std::error_code{} : settleEdit(m_edit->m_manifest);
            }

            auto completion = advanceMatterRequest(
                *m_matter_runtime, EMatterOperation::edit_prepare, [&]() -> Expected<MatterRequest> {
                    if (m_edit->m_inventory_only) {
                        return m_matter_runtime->beginPrepareEdit(
                            m_matter->m_identity, *m_edit->m_matter, m_matter->m_profile.m_particle_count, {},
                            m_edit->m_replace_inventory ? EMatterEditKind::particle_replace
                                                        : EMatterEditKind::particle_append);
                    }

                    Array<u16> solids;
                    for (const auto &material: contentCatalog()->materials()) {
                        if (material.m_representation == EMaterialRepresentation::terrain and
                            material.m_phase == EMaterialPhase::solid) {
                            solids.push_back(material.m_numeric_id);
                        }
                    }

                    for (auto &chunk: m_edit->m_chunks) {
                        grid().exchangeChunk(chunk);
                    }

                    auto terrain = [&] {
                        PPR_DEFER {
                            for (auto &chunk: m_edit->m_chunks) {
                                grid().exchangeChunk(chunk);
                            }
                        };

                        return makeMatterTerrain(grid(), solids);
                    }();
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyEdits, terrain);
                    return m_matter_runtime->beginPrepareEdit(
                        m_matter->m_identity, *m_edit->m_matter, m_matter->m_profile.m_particle_count,
                        std::move(*terrain),
                        m_edit->m_inventory_only ? (m_edit->m_replace_inventory ? EMatterEditKind::particle_replace
                                                                                : EMatterEditKind::particle_append)
                                                 : EMatterEditKind::terrain_append);
                });
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
            if (not *completion) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_operation_error);
            m_edit->m_matter_prepared = true;
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);

            // Acknowledged upload submission is distinct from its copy fence.
            // The next poll alone can admit joint publication.
        }
        return {};
    }

    // ------------------------------------------------------------------
    // edit settlement
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::settleEdit(const std::span<const DirtyChunk> manifest) {
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, settleEdit_(manifest, 0u));
        return {};
    }

    std::error_code ColonyDriver::settleEdit_(const std::span<const DirtyChunk> manifest, const u64 door_revision) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyEdits,
            not m_service_door or (m_service_door->m_revision == door_revision and m_service_door->m_activated)
                ? std::error_code{}
                : std::make_error_code(std::errc::operation_not_permitted));
        if (not editReadyForPresentation()) {
            return {};
        }
        const bool matches = std::ranges::all_of(m_edit->m_manifest, [&](const DirtyChunk &expected) {
            return std::ranges::any_of(manifest, [&](const DirtyChunk &row) {
                return row.m_pos == expected.m_pos and row.m_revision == expected.m_revision;
            });
        });
        if (not matches) {
            return {};
        }

        if (m_edit->m_matter) {
            PPR_ASSERT(m_matter_runtime != nullptr and m_edit->m_matter_prepared);
            // Prepared CPU/GPU state and collider cohort are frozen together.
            // Either publication may be irreversible even on an unexpected error.
            auto completion = advanceMatterRequest(*m_matter_runtime, EMatterOperation::edit_publish, [&] {
                return m_matter_runtime->beginPublishEdit(m_edit->m_matter->m_identity);
            });
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, completion);
            if (not *completion) {
                return {};
            }

            if ((**completion).m_execution.m_operation_error or
                (not m_edit->m_inventory_only and m_colliders.commitCohort(m_scene))) {
                hal::fatalExit("matter-terrain-joint-publication-failure");
            }

            m_edit->m_colliders_started = false;
            m_edit->m_matter_prepared = false;

            for (auto &chunk: m_edit->m_chunks)
                grid().exchangeChunk(chunk);

            m_matter = std::move(m_edit->m_matter);
            m_colony.particles() = std::move(m_edit->m_moving_particles);
            const auto presentation_world = m_particle_view->m_world;
            m_particle_view = (**completion).m_view;
            PPR_ASSERT(m_particle_view.has_value());
            m_particle_view->m_world = presentation_world;
            m_particle_view->m_joint_tick = m_tick_count;
            m_cpu_particles_current = true;
            m_snapshot_tick = m_tick_count;
            m_capture_awake_valid = false;
            m_initial_matter_baseline_valid = false;

            if (m_edit->m_inventory_only) {
                m_edit.reset();
                // A committed inventory mutation can change the frozen verdict.
                clearMatterStall();
                PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);
                return {};
            }

            for (const auto &row: m_edit->m_errands)
                m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;

            for (const auto &row: m_edit->m_manifest) {
                auto present = std::ranges::find(m_present_dirty, row.m_pos, &DirtyChunk::m_pos);
                if (present == m_present_dirty.end())
                    m_present_dirty.push_back(row);
                else
                    *present = row;

                auto history = std::ranges::find(m_present_revision_history, row.m_pos, &DirtyChunk::m_pos);
                if (history == m_present_revision_history.end())
                    m_present_revision_history.push_back(row);
                else
                    *history = row;
            }

            m_collider_dirty.clear();
            m_finder = Pathfinder{};
            m_dig_stats.m_dug_total += m_edit->m_dug;
            m_initial_matter_baseline_valid = false;
            invalidateColliderCache();

            if (wakeBodiesNearEdit(m_edit->m_touched))
                hal::fatalExit("matter-terrain-postcommit-wake-failure");

            m_dig_stats.m_last_chunks = std::move(m_edit->m_touched);
            commitDoorBinding_();
            m_edit.reset();
            // The committed terrain edit changes both rows and coverage, so the
            // freeze ends and the next tick evaluates a new verdict.
            clearMatterStall();
            PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, (**completion).m_execution.m_transport_error);
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, m_colliders.commitCohort(m_scene));
        m_edit->m_colliders_started = false;

        for (auto &chunk: m_edit->m_chunks) {
            grid().exchangeChunk(chunk);
        }

        m_colony.particles().commitTerrainEdit(m_edit->m_particles);

        for (const auto &row: m_edit->m_errands) {
            m_registry.get<BuildErrand>(row.m_entity)->m_progress = row.m_candidate;
        }

        fanOutEditChunks(m_edit->m_touched);
        m_collider_dirty.clear(); // Already prepared and committed as one cohort.
        m_finder = Pathfinder{};
        m_dig_stats.m_dug_total += m_edit->m_dug;

        const auto wake_error = wakeBodiesNearEdit(m_edit->m_touched);
        // The transaction is committed even if the postcommit physics wake fails.
        commitDoorBinding_();
        m_edit.reset();
        // A committed terrain edit can change the frozen verdict.
        clearMatterStall();
        PPR_RETURN_ERROR_ON_FAIL(ColonyEdits, wake_error);
        return {};
    }
}
