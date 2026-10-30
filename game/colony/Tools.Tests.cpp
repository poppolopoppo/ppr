module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import game.colony.tools;
import game.colony.driver;
import std;

namespace pP::game::tests::detail::tools {
    // ------------------------------------------------------------------
    // physical brush settings and lifecycle admission
    // ------------------------------------------------------------------

    using namespace colony;

    PPR_UNIT_TEST (settings_domains_and_disabled_driver_cleanup) {
        ToolSettings settings{};
        PPR_TEST_ASSERT(not validateToolSettings(settings));

        settings.m_radius_m = 1.0 / 3.0;
        settings.m_acceleration_m_s2 = 0.0;
        settings.m_spawn_count = 1u;
        PPR_TEST_ASSERT(not validateToolSettings(settings));

        settings.m_radius_m = 32.0;
        settings.m_acceleration_m_s2 = 1000.0;
        settings.m_spawn_count = 4096u;
        PPR_TEST_ASSERT(not validateToolSettings(settings));

        for (const double value:
            {0.0, 32.001, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            auto invalid = settings;
            invalid.m_radius_m = value;
            PPR_TEST_ASSERT(validateToolSettings(invalid) == std::errc::invalid_argument);
        }

        for (const double value: {-1.0, 1000.001, std::numeric_limits<double>::quiet_NaN()}) {
            auto invalid = settings;
            invalid.m_acceleration_m_s2 = value;
            PPR_TEST_ASSERT(validateToolSettings(invalid) == std::errc::invalid_argument);
        }

        for (const u32 count: {0u, 4097u}) {
            auto invalid = settings;
            invalid.m_spawn_count = count;
            PPR_TEST_ASSERT(validateToolSettings(invalid) == std::errc::invalid_argument);
        }

        settings.m_spawn_material.clear();
        PPR_TEST_ASSERT(validateToolSettings(settings) == std::errc::invalid_argument);

        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.setRadialForce({}));
        PPR_TEST_ASSERT(driver.setRadialForce({1.0, 1.0, 2.0, 20.0}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(driver.requestParticleTool({}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.shutdown());
        PPR_TEST_ASSERT(not driver.setRadialForce({}));
    };
}

namespace pP::game::tests {
    const UnitTest tools = UnitTest::Named("tools") / [](UnitTest::IRun &_) {
        _.recurse({detail::tools::settings_domains_and_disabled_driver_cleanup});
    };

    const UnitTest &gameToolsTests() noexcept {
        return tools;
    }
}
