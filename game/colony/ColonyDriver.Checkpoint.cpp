module;
#include "pP/Macros.h"
module game.colony.driver;
import std;
namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyCheckpoint, info, none);
    u32 ColonyDriver::tickHz() const noexcept {
        return m_tick_hz;
    }
    bool ColonyDriver::captureReady() const noexcept {
        return not m_edit and m_held_slices == 0u;
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
            const auto awake = m_scene.isAwake(body->m_handle);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyCheckpoint, awake);
            record.m_awake = *awake;
        }
        snapshot->m_bodies.m_replay_ticks = m_tick_count;
        return snapshot;
    }
    std::error_code ColonyDriver::initRestored(
        const ColonyDriverDesc &desc, PreparedWorld world, const sim::Snapshot &snapshot, const u64 tick,
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
        PPR_RETURN_ERROR_ON_FAIL(ColonyCheckpoint, m_colony.install(std::move(world)));
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
        m_initial_matter_baseline_valid = initial_matter_valid;
        m_counts = pP::colony::pathCounts(m_registry);
        m_errands = liveErrandCount(m_registry);
        m_agent_count = static_cast<u32>(m_agents.size());
        clearPlayerCommand();
        initialized = true;
        return {};
    }
}
