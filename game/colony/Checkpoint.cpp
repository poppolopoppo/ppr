module;
#include "pP/Macros.h"
module game.colony.checkpoint;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import std;
namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldCheckpointLog, info, none);
    namespace {
        std::error_code canceled(const IContext *const context) noexcept {
            return context ? context->error() : std::error_code{};
        }
        Array<CheckpointMaterial> materialManifest(const ContentCatalog &catalog) {
            Array<CheckpointMaterial> result{{0u, "vacuum"}};
            for (const auto &material: catalog.materials()) {
                result.push_back({material.m_numeric_id, material.m_key});
            }
            std::ranges::sort(result, {}, &CheckpointMaterial::m_id);
            return result;
        }
    }
    Expected<WorldCheckpoint> captureWorldCheckpoint(
        const ColonyDriver &driver, const sim::GeneratedWorld &generated, const IContext *const context) {
        const auto *const prepared = driver.preparedWorld();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldCheckpointLog, prepared and driver.captureReady()
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, canceled(context));
        WorldCheckpoint checkpoint;
        checkpoint.m_descriptor = prepared->descriptor();
        checkpoint.m_materials = materialManifest(*driver.contentCatalog());
        checkpoint.m_sites.assign(generated.m_layout.sites().begin(), generated.m_layout.sites().end());
        checkpoint.m_geometry = generated.m_geometry;
        checkpoint.m_matter = generated.m_matter;
        checkpoint.m_particles = driver.captureParticles();
        checkpoint.m_start = generated.m_start;
        checkpoint.m_report = generated.m_report;
        checkpoint.m_controlled = driver.controlledEntity();
        checkpoint.m_initial_matter_valid = driver.initialMatterBaselineValid();
        checkpoint.m_tick = driver.tickCount();
        checkpoint.m_tick_hz = driver.tickHz();
        auto snapshot = driver.captureFullWithBodies(context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, snapshot);
        checkpoint.m_snapshot = std::move(*snapshot);
        // Retain the established v4 column layouts/IDs, but no transient rows.
        for (const auto id:
            {driver.registry().componentId<Plan>(), driver.registry().componentId<PathReq>(),
                driver.registry().componentId<PathComp>()}) {
            PPR_ASSERT(id.has_value());
            auto &column = checkpoint.m_snapshot.m_ecs.m_columns[*id];
            column.m_owners.clear();
            column.m_bytes.clear();
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, canceled(context));
        return checkpoint;
    }

    Expected<std::unique_ptr<ColonyDriver>> restoreWorldCheckpoint(
        ContentCatalog catalog, WorldCheckpoint checkpoint, const u64 world_generation,
        const sim::GenerationExecution &execution) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldCheckpointLog,
            world_generation != 0u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldCheckpointLog, validateWorldCheckpoint(checkpoint, catalog, execution.m_context));
        auto plan = compileWorldPlan(catalog, checkpoint.m_descriptor.m_profile, checkpoint.m_descriptor.m_seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, plan);
        auto layout = sim::restoreBiomeLayout(*plan, checkpoint.m_sites);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, layout);
        sim::ChunkGrid grid;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, sim::apply(grid, checkpoint.m_snapshot));
        sim::GeneratedWorld world{
            std::move(*plan), std::move(*layout), std::move(checkpoint.m_geometry), std::move(grid),
            std::move(checkpoint.m_matter), std::move(checkpoint.m_start), std::move(checkpoint.m_report)};
        auto prepared = restorePreparedWorld(std::move(catalog), checkpoint.m_descriptor, std::move(world));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldCheckpointLog, prepared);
        auto driver = std::make_unique<ColonyDriver>();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldCheckpointLog,
            driver->initRestored(
                {.m_seed = checkpoint.m_descriptor.m_seed,
                    .m_tick_hz = checkpoint.m_tick_hz,
                    .m_agent_count = 1u,
                    .m_world_generation = world_generation},
                std::move(*prepared), checkpoint.m_snapshot, checkpoint.m_particles, checkpoint.m_tick,
                checkpoint.m_initial_matter_valid, checkpoint.m_controlled, &execution));
        return driver;
    }
}
