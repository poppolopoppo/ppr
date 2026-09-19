module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace FilteredAnalogTests {
        constexpr float kEps = 1e-4f;

        PPR_UNIT_TEST (initial_state) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            PPR_TEST_ASSERT(std::abs(analog.raw()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.filtered()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.sensitivity() - 2.0f) < kEps);
        };

        PPR_UNIT_TEST (add_accumulation) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.add(1.0f);
            analog.add(2.0f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 3.0f) < kEps);
            analog.add(-1.0f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 2.0f) < kEps);
        };

        PPR_UNIT_TEST (first_update_snapshots_raw) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            // First update: filtered snaps to raw, delta is zero.
            PPR_TEST_ASSERT(std::abs(analog.filtered() - 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
        };

        PPR_UNIT_TEST (update_lerps_toward_raw) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.update(std::chrono::milliseconds{16}); // filtered = 0
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            // Rate contract: alpha = 1 - exp(-lambda * dt).
            const float expected = 1.0f - std::exp(-2.0f * 0.016f);
            PPR_TEST_ASSERT(std::abs(analog.filtered() - expected) < 1e-6f);
            PPR_TEST_ASSERT(std::abs(analog.delta() - expected) < 1e-6f);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 1.0f) < kEps);
        };

        PPR_UNIT_TEST (dt_clamped_to_150ms) {
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
            const float unclamped = 1.0f - std::exp(-2.0f * 0.2f);
            PPR_TEST_ASSERT(clamped.filtered() < unclamped - 1e-3f);
        };

        PPR_UNIT_TEST (zero_rate_freezes_filter) {
            FilteredAnalog<float> analog{0.0f, 0.0f};
            analog.update(std::chrono::milliseconds{16}); // prime: filtered = 0
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            PPR_TEST_ASSERT(std::abs(analog.filtered()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.delta()) < kEps);
            PPR_TEST_ASSERT(std::abs(analog.raw() - 1.0f) < kEps);
        };

        PPR_UNIT_TEST (huge_sensitivity_snaps_to_raw) {
            FilteredAnalog<float> analog{0.0f, 100000.0f};
            analog.update(std::chrono::milliseconds{16}); // prime: filtered = 0
            analog.add(1.0f);
            analog.update(std::chrono::milliseconds{16});
            PPR_TEST_ASSERT(std::abs(analog.filtered() - 1.0f) < 1e-4f);
        };

        // Primed controller, equal wall time, constant raw: 30/60/120Hz must
        // converge within tolerance. Closed form gives exact invariance:
        // residual after T is exp(-lambda * T) regardless of partitioning.
        PPR_UNIT_TEST (primed_equal_wall_time_converges) {
            const auto run_at = [](const int steps, const std::chrono::microseconds step) {
                FilteredAnalog<float> analog{0.0f, 2.0f};
                analog.update(std::chrono::milliseconds{16}); // prime: filtered = 0
                analog.add(1.0f); // constant raw = 1 from here on
                for (int i = 0; i < steps; ++i) {
                    analog.update(step);
                }
                return analog.filtered();
            };
            // 1s wall time at 30/60/120Hz.
            const float at30 = run_at(30, std::chrono::microseconds{33333});
            const float at60 = run_at(60, std::chrono::microseconds{16667});
            const float at120 = run_at(120, std::chrono::microseconds{8333});
            const float expected = 1.0f - std::exp(-2.0f * 1.0f);
            PPR_TEST_ASSERT(std::abs(at30 - expected) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at60 - expected) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at120 - expected) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at30 - at60) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at60 - at120) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at30 - at120) < 2e-3f);
        };

        // Same invariance for vector state: post-priming motion must not stall
        // at small dt (the old pow(dt, 1/s) contract was ~1e-12 at 16ms/0.15).
        PPR_UNIT_TEST (primed_float3_equal_wall_time_converges) {
            const auto run_at = [](const int steps, const std::chrono::microseconds step) {
                FilteredAnalog<float3> analog{float3{zero_v}, 8.0f};
                analog.update(std::chrono::milliseconds{16}); // prime
                analog.add(float3{1.0f, 0.0f, 0.0f});
                for (int i = 0; i < steps; ++i) {
                    analog.update(step);
                }
                return analog.filtered();
            };
            const float3 at30 = run_at(30, std::chrono::microseconds{33333});
            const float3 at60 = run_at(60, std::chrono::microseconds{16667});
            const float3 at120 = run_at(120, std::chrono::microseconds{8333});
            const float expected = 1.0f - std::exp(-8.0f * 1.0f);
            PPR_TEST_ASSERT(std::abs(at30.x - expected) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at60.x - expected) < 2e-3f);
            PPR_TEST_ASSERT(std::abs(at120.x - expected) < 2e-3f);
            PPR_TEST_ASSERT(distance(at30, at60) < 2e-3f);
            PPR_TEST_ASSERT(distance(at60, at120) < 2e-3f);
            // Motion is meaningful after 1s, not stalled near zero.
            PPR_TEST_ASSERT(at60.x > 0.9f);
        };

        PPR_UNIT_TEST (reset_restores_init) {
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

        PPR_UNIT_TEST (set_sensitivity) {
            FilteredAnalog<float> analog{0.0f, 2.0f};
            analog.setSensitivity(4.0f);
            PPR_TEST_ASSERT(std::abs(analog.sensitivity() - 4.0f) < kEps);
        };

        PPR_UNIT_TEST (float3_accumulation) {
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
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest filtered_analog = UnitTest::Named("filtered_analog") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::FilteredAnalogTests::initial_state,
            detail::FilteredAnalogTests::add_accumulation,
            detail::FilteredAnalogTests::first_update_snapshots_raw,
            detail::FilteredAnalogTests::update_lerps_toward_raw,
            detail::FilteredAnalogTests::dt_clamped_to_150ms,
            detail::FilteredAnalogTests::zero_rate_freezes_filter,
            detail::FilteredAnalogTests::huge_sensitivity_snaps_to_raw,
            detail::FilteredAnalogTests::primed_equal_wall_time_converges,
            detail::FilteredAnalogTests::primed_float3_equal_wall_time_converges,
            detail::FilteredAnalogTests::reset_restores_init,
            detail::FilteredAnalogTests::set_sensitivity,
            detail::FilteredAnalogTests::float3_accumulation,
        });
    };

    const UnitTest &filtered_analogTests() noexcept {
        return filtered_analog;
    }
} // namespace pP::tests
