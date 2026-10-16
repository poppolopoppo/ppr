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
        const auto *compiled = catalog.selectedRecipe();
        if (not catalog.contentFingerprint() or catalog.findWorld(profile_key) == nullptr or compiled == nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyWorldPlan, std::errc::invalid_argument);
        }

        return sim::compileGenerationPlan(compiled->m_recipe, seed);
    }
}
