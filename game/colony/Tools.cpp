module;
#include "pP/Macros.h"

module game.colony.tools;

import engine.core;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyTools, info, none)

    // ------------------------------------------------------------------
    // settings validation
    // ------------------------------------------------------------------

    std::error_code validateToolSettings(const ToolSettings &settings) {
        if (settings.m_spawn_material.empty() or
            not std::isfinite(settings.m_radius_m) or
            settings.m_radius_m < 1.0 / 3.0 or
            settings.m_radius_m > 32.0 or
            not std::isfinite(settings.m_acceleration_m_s2) or
            settings.m_acceleration_m_s2 < 0.0 or
            settings.m_acceleration_m_s2 > 1000.0 or
            settings.m_spawn_count == 0u or
            settings.m_spawn_count > 4096u) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyTools, std::errc::invalid_argument);
        }

        return {};
    }
}
