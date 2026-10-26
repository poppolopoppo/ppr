module;
#include "pP/Macros.h"
module game.colony.driver;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyCheckpoint, info, none)

    // ------------------------------------------------------------------
    // checkpoint capture and restore
    // ------------------------------------------------------------------

    u32 ColonyDriver::tickHz() const noexcept {
        return m_tick_hz;
    }

    bool ColonyDriver::captureReady() const noexcept {
        return not m_matter_terminal and
               not m_matter_request and
               m_snapshot_phase == ESnapshotPhase::idle and
               not m_edit and
               not m_matter_pending and
               m_held_slices == 0u and
               m_pending_single_steps == 0u;
    }

    Expected<sim::Snapshot> ColonyDriver::captureFullWithBodies(const IContext *const context) const {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyCheckpoint, m_timestep.has_value() and captureReady()
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::device_or_resource_busy));

        auto snapshot = sim::captureFull(grid(), m_registry, m_seed, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyCheckpoint, snapshot);

        for (auto &record: snapshot->m_bodies.m_records) {
            const auto body = std::ranges::find(m_bodies, record.m_entity, &AgentBody::m_entity);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyCheckpoint,
                body != m_bodies.end() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            if (m_capture_awake_valid) {
                const auto awake = std::ranges::find(
                    m_capture_awake, record.m_entity, &std::pair<sim::Entity, bool>::first);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyCheckpoint, awake != m_capture_awake.end()
                                          ? std::error_code{}
                                          : std::make_error_code(std::errc::invalid_argument));
                record.m_awake = awake->second;
            } else {
                const auto awake = m_scene.isAwake(body->m_handle);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyCheckpoint, awake);
                record.m_awake = *awake;
            }
        }

        snapshot->m_bodies.m_replay_ticks = m_tick_count;
        return snapshot;
    }

    std::error_code ColonyDriver::initRestored(
        const ColonyDriverDesc &desc, PreparedWorld world, const sim::Snapshot &snapshot,
        const sim::MatterParticleSnapshot &particles, MatterState matter, const u64 tick,
        const bool initial_matter_valid, const std::optional<sim::Entity> controlled,
        const sim::GenerationExecution *const execution) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyCheckpoint, not m_timestep and
                                      desc.m_tick_hz != 0u and
                                      desc.m_seed == world.descriptor().m_seed and
                                      snapshot.m_seed == desc.m_seed
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        const auto period = TimeSpan{std::chrono::seconds{1}} / desc.m_tick_hz;
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyCheckpoint,
            period > TimeSpan{} ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyCheckpoint, validateMatterState(
                                  matter, world.generated().m_plan.recipe().m_materials,
                                  1.0 / world.descriptor().m_metric.m_cells_per_metre,
                                  world.descriptor().m_metric.m_reference_thickness_m, kGravityY));

        PPR_RETURN_ERROR_ON_FAIL(
            ColonyCheckpoint,
            m_colony.install(std::move(world), &particles, execution ? execution->m_context : nullptr));

        bool initialized = false;
        PPR_DEFER {
            if (not initialized) {
                // Best-effort cleanup of an unpublished candidate, including unwinding.
                PPR_LOG_WARNING_ON_FAIL(ColonyCheckpoint, shutdown());
            }
        };

        PPR_RETURN_ERROR_ON_FAIL(ColonyCheckpoint, initializeRuntime(desc, period, execution, &snapshot));
        PPR_RETURN_ERROR_ON_FAIL(ColonyCheckpoint, assignControl(controlled));

        m_tick_count = tick;
        m_snapshot_tick = tick;
        matter.m_identity.m_world = m_world_generation;
        m_matter = std::move(matter);
        m_initial_matter_baseline_valid = initial_matter_valid;
        m_counts = pP::colony::pathCounts(m_registry);
        m_errands = liveErrandCount(m_registry);
        clearPlayerCommand();
        initialized = true;
        return {};
    }
}
