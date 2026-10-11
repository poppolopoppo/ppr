module;
#include "pP/Macros.h"
module game.colony.worldgen;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldPreparation, info, none);

    PreparedWorld::PreparedWorld(ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world)
        : m_catalog(std::move(catalog)), m_descriptor(std::move(descriptor)), m_world(std::move(world)) {
    }
    PreparedWorld::PreparedWorld(PreparedWorld &&) noexcept = default;
    PreparedWorld &PreparedWorld::operator=(PreparedWorld &&) noexcept = default;
    PreparedWorld::~PreparedWorld() = default;
    const ContentCatalog &PreparedWorld::catalog() const noexcept {
        return m_catalog;
    }
    const WorldDescriptor &PreparedWorld::descriptor() const noexcept {
        return m_descriptor;
    }
    const sim::GeneratedWorld &PreparedWorld::generated() const noexcept {
        return m_world;
    }
    sim::ChunkGrid &PreparedWorld::grid() noexcept {
        return m_world.m_grid;
    }

    Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution) {
        auto catalog = loadContentCatalog(root, diagnostics, execution.m_context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, catalog);
        return prepareWorld(std::move(*catalog), profile, seed, clock, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution) {
        if (execution.m_context != nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, execution.m_context->error());
        }
        if (not catalog.contentFingerprint()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, std::errc::invalid_argument);
        }
        auto plan = compileWorldPlan(catalog, profile, seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, plan);
        auto world = sim::generateWorld(std::move(*plan), clock, execution);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, world);
        const WorldDescriptor descriptor{
            .m_profile = std::string{profile}, .m_seed = seed, .m_content_fingerprint = *catalog.contentFingerprint()};
        return PreparedWorld{std::move(catalog), descriptor, std::move(*world)};
    }

    std::error_code generateWorld(
        const u64 seed, sim::ChunkGrid &grid, const sim::GenerationExecution *const execution) {
        if (execution != nullptr) {
            return sim::generate(seed, grid, *execution);
        }
        sim::generate(seed, grid);
        return {};
    }
}
