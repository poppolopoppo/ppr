module;
#include "pP/Macros.h"
module game.colony.worldgen;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // world plan compilation
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(ColonyWorldPlan, info, none)

    Expected<sim::GenerationPlan> compileWorldPlan(
        const ContentCatalog &catalog, const std::string_view profile_key, const u64 seed) {
        if (not catalog.contentFingerprint() or catalog.findWorld(profile_key) == nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyWorldPlan, std::errc::invalid_argument);
        }

        config::Diagnostics diagnostics{64u};
        const sim::definition::GenerationCompileParameters parameters{
            .m_cell_size_m = 1.0 / 3.0,
            .m_reference_thickness_m = 1.0,
            .m_actor_half_width_m = 0.4,
            .m_actor_half_height_m = 0.9};

        auto compiled = sim::definition::compileRecipe(
            catalog.definition(), {std::string{profile_key}}, parameters, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyWorldPlan, compiled);
        return sim::compileGenerationPlan(std::move(compiled->m_recipe), seed);
    }
}
