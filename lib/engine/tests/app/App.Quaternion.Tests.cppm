module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"

export module engine.tests.app:quaternion;

import engine.app;
import engine.core;
import engine.math;
import std;

export namespace pP::tests {
    namespace QuaternionTests {
        constexpr float kEps = 1e-4f;

        PPR_UNIT_TEST(identity) {
            const auto q = pP::Quaternion::identity();
            PPR_TEST_ASSERT(std::abs(dot(q, q) - 1.0f) < kEps);

            const float3 v(1.0f, 2.0f, 3.0f);
            const auto rotated = pP::quaternionTransform(q, v);
            PPR_TEST_ASSERT(std::abs(rotated.x - v.x) < kEps);
            PPR_TEST_ASSERT(std::abs(rotated.y - v.y) < kEps);
            PPR_TEST_ASSERT(std::abs(rotated.z - v.z) < kEps);
        };

        PPR_UNIT_TEST(yaw_pitch_roll) {
            constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;

            // yaw = +90° about Y: forward (-Z) maps to (-X)
            const auto yaw_q = Quaternion::rotateXYZ(0.0f, kHalfPi, 0.0f);
            const auto yawed = pP::quaternionTransform(yaw_q, float3(0.0f, 0.0f, -1.0f));
            PPR_TEST_ASSERT(std::abs(yawed.x - (-1.0f)) < kEps);
            PPR_TEST_ASSERT(std::abs(yawed.y) < kEps);
            PPR_TEST_ASSERT(std::abs(yawed.z) < kEps);

            // pitch = +90° about X: forward (-Z) maps to (+Y)
            const auto pitch_q = pP::Quaternion::rotateXYZ(kHalfPi, 0.0f, 0.0f);
            const auto pitched = pP::quaternionTransform(pitch_q, float3(0.0f, 0.0f, -1.0f));
            PPR_TEST_ASSERT(std::abs(pitched.x) < kEps);
            PPR_TEST_ASSERT(std::abs(pitched.y - 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(pitched.z) < kEps);
        };

        PPR_UNIT_TEST(matrix_round_trip) {
            const auto q = pP::Quaternion::rotateXYZ(0.3f, 0.2f, 0.1f);
            const auto q2 = Quaternion(float3x3(q));

            const float3 v(1.0f, 2.0f, 3.0f);
            const auto a = pP::quaternionTransform(q, v);
            const auto b = pP::quaternionTransform(q2, v);
            PPR_TEST_ASSERT(std::abs(a.x - b.x) < kEps);
            PPR_TEST_ASSERT(std::abs(a.y - b.y) < kEps);
            PPR_TEST_ASSERT(std::abs(a.z - b.z) < kEps);
        };

        PPR_UNIT_TEST(length_preserved) {
            const auto q = pP::Quaternion::rotateXYZ(0.5f, -0.25f, 0.125f);
            const float3 v(1.0f, 2.0f, 3.0f);
            const auto rotated = pP::quaternionTransform(q, v);
            PPR_TEST_ASSERT(std::abs(pP::dot2(rotated) - pP::dot2(v)) < kEps);
        };
    }

    PPR_UNIT_TEST(app_quaternion) {
        _.recurse({
            QuaternionTests::identity,
            QuaternionTests::yaw_pitch_roll,
            QuaternionTests::matrix_round_trip,
            QuaternionTests::length_preserved,
        });
    };
}
