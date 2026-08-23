module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"

export module engine.tests.app:filtered_analog;

import engine.app;
import engine.core;
import engine.math;
import std;

export namespace pP::tests {
    namespace FilteredAnalogTests {
        constexpr float kEps = 1e-4f;

        PPR_UNIT_TEST(initial_state) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            PPR_TEST_ASSERT(std::abs(analog.raw()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.filtered()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.sensitivity() - 2.0f) < kEps);
        };

        PPR_UNIT_TEST(add_accumulation) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.add(1.0f);
            analog.add(2.0f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 3.0f) < kEps);
            analog.add(-1.0f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 2.0f) < kEps);
        };

        PPR_UNIT_TEST(first_update_snapshots_raw) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            // First update: filtered snaps to raw, delta is zero.
            PPR_TEST_ASSERT(std::abs(analog.filtered() - 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
        };

        PPR_UNIT_TEST(update_lerps_toward_raw) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.update(std::chrono::milliseconds{16}); // filtered = 0
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            // t = pow(0.016, 1/2) = sqrt(0.016) ≈ 0.1265
            const float expected = std::sqrt(0.016f);
            PPR_TEST_ASSERT(std::abs(analog.filtered() - expected) < 1e-3f);
            PPR_TEST_ASSERT(std::abs(analog.delta() - expected) < 1e-3f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 1.0f) < kEps);
        };

        PPR_UNIT_TEST(dt_clamped_to_150ms) {
            FilteredAnalog<float> clamped{0.0f, 2.0f};
            clamped.update(std::chrono::milliseconds{16});
            clamped.add(1.0f);
            clamped.update(std::chrono::milliseconds{200}); // clamped to 150ms

            FilteredAnalog<float> reference{0.0f, 2.0f};
            reference.update(std::chrono::milliseconds{16});
            reference.add(1.0f);
            reference.update(std::chrono::milliseconds{150});

            PPR_TEST_ASSERT(std::abs(clamped.filtered() - reference.filtered()) < 1e-4f);
            // Unclamped 200ms would converge further than 150ms.
            const float unclamped = std::sqrt(0.2f);
            PPR_TEST_ASSERT(clamped.filtered() < unclamped - 1e-3f);
        };

        PPR_UNIT_TEST(reset_restores_init) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.update(std::chrono::milliseconds{16});
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            PPR_TEST_ASSERT(std::abs(analog.filtered()) > kEps);

            analog.reset(5.0f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 5.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.filtered() - 5.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);

            // Next update re-snapshots raw; delta stays zero.
            analog.update(std::chrono::milliseconds{16});
            PPR_TEST_ASSERT(std::abs(analog.filtered() - 5.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
        };

        PPR_UNIT_TEST(set_sensitivity) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.setSensitivity(4.0f);
            PPR_TEST_ASSERT(std::abs(analog.sensitivity() - 4.0f) < kEps);
        };

        PPR_UNIT_TEST(float3_accumulation) {
            FilteredAnalog<float3> analog{float3{zero_v}, 2.0f};
            analog.add(float3{1.0f, 2.0f, 3.0f});
            analog.add(float3{1.0f, 1.0f, 1.0f});
            PPR_TEST_ASSERT(distance(analog.raw(), float3{2.0f, 3.0f, 4.0f}) < kEps);
            analog.update(std::chrono::milliseconds{16});
            PPR_TEST_ASSERT(std::isfinite(analog.filtered().x));
            PPR_TEST_ASSERT(std::isfinite(analog.filtered().y));
            PPR_TEST_ASSERT(std::isfinite(analog.filtered().z));
        };
    }

    PPR_UNIT_TEST(app_filtered_analog) {
        _.recurse({
            FilteredAnalogTests::initial_state,
            FilteredAnalogTests::add_accumulation,
            FilteredAnalogTests::first_update_snapshots_raw,
            FilteredAnalogTests::update_lerps_toward_raw,
            FilteredAnalogTests::dt_clamped_to_150ms,
            FilteredAnalogTests::reset_restores_init,
            FilteredAnalogTests::set_sensitivity,
            FilteredAnalogTests::float3_accumulation,
        });
    };
}
