module;
#include "pP/Macros.h"
module game.colony;

import engine.core;
import engine.sim;
import game.colony.worldgen;
import game.colony.metric;

import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyWorld, info, none);
    Colony::Colony() = default;
    Colony::Colony(Colony &&) noexcept = default;
    Colony &Colony::operator=(Colony &&) noexcept = default;
    Colony::~Colony() = default;
    const sim::ChunkGrid &Colony::grid() const noexcept {
        if (m_prepared) {
            return m_prepared->generated().m_grid;
        }
        return m_grid;
    }

    sim::ChunkGrid &Colony::grid() noexcept {
        if (m_prepared) {
            return m_prepared->grid();
        }
        return m_grid;
    }

    bool Colony::isInitialized() const noexcept {
        return m_initialized;
    }

    std::error_code Colony::initLegacyFixture(const ColonyDesc &desc, const sim::GenerationExecution *const execution) {
        m_initialized = false;
        m_prepared.reset();
        m_grid = sim::ChunkGrid{};
        PPR_RETURN_ERROR_ON_FAIL(ColonyWorld, generateLegacyFixture(desc.m_seed, m_grid, execution));

        m_initialized = true;
        return {};
    }

    std::error_code Colony::shutdown() {
        m_prepared.reset();
        m_grid = sim::ChunkGrid{};
        m_initialized = false;
        return {};
    }

    std::error_code Colony::install(PreparedWorld world) {
        const auto &descriptor = world.descriptor();
        const auto &recipe = world.generated().m_plan.recipe();
        PPR_RETURN_ERROR_ON_FAIL(ColonyWorld, validateWorldMetric(descriptor.m_metric));
        if (descriptor.m_pack_schema_version != 1u or
            descriptor.m_generator_version != sim::kGeneratorAlgorithmVersion or
            descriptor.m_content_fingerprint != world.catalog().contentFingerprint() or
            world.catalog().findWorld(descriptor.m_profile) == nullptr or
            descriptor.m_seed != world.generated().m_plan.seed() or
            recipe.m_cell_size_m != kFineCellMetres or
            recipe.m_reference_thickness_m != descriptor.m_metric.m_reference_thickness_m) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyWorld, std::errc::invalid_argument);
        }
        auto prepared = std::make_unique<PreparedWorld>(std::move(world));
        m_grid = sim::ChunkGrid{};
        m_prepared = std::move(prepared);
        m_initialized = true;
        return {};
    }

    const PreparedWorld *Colony::preparedWorld() const noexcept {
        return m_prepared.get();
    }
}
