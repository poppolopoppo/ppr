module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace MathTransform {
        using pP::math::Transform;

        using pP::isFinite;
        using pP::isInf;
        using pP::isInfOrNan;
        using pP::isNan;

        constexpr float kEps = 1e-4f;
        constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;

        [[nodiscard]] bool nearEqual(const float expected, const float actual, const float eps = kEps) noexcept {
            return std::abs(expected - actual) < eps;
        }

        [[nodiscard]] bool nearEqual(const float3 &expected, const float3 &actual, const float eps = kEps) noexcept {
            return nearEqual(expected.x, actual.x, eps) and
                nearEqual(expected.y, actual.y, eps) and
                nearEqual(expected.z, actual.z, eps);
        }

        // Quaternions double-cover SO(3): compare the rotation, never the raw components.
        [[nodiscard]] bool nearEqual(const Quaternion &expected, const Quaternion &actual, const float eps = kEps) noexcept {
            return nearEqual(std::abs(dot(expected, actual)), 1.0f, eps);
        }

        [[nodiscard]] bool nearEqual(const float4x4 &expected, const float4x4 &actual, const float eps = kEps) noexcept {
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    if (not nearEqual(expected(row, column), actual(row, column), eps)) {
                        return false;
                    }
                }
            }

            return true;
        }

        [[nodiscard]] bool isUnit(const Quaternion &value, const float eps = kEps) noexcept {
            return nearEqual(std::abs(dot(value, value)), 1.0f, eps);
        }

        [[nodiscard]] bool rowEquals(const float4x4 &matrix, const std::size_t row, const float x, const float y, const float z, const float w) noexcept {
            return matrix(row, 0) == x and matrix(row, 1) == y and matrix(row, 2) == z and matrix(row, 3) == w;
        }

        [[nodiscard]] float3 rowOf(const float4x4 &matrix, const std::size_t row) noexcept {
            return float3{matrix(row, 0), matrix(row, 1), matrix(row, 2)};
        }

        // Row-vector point application: p * M includes the translation row.
        [[nodiscard]] float3 applyToPoint(const float3 &point, const float4x4 &matrix) noexcept {
            return point * matrix;
        }

        [[nodiscard]] bool allFinite(const Transform &value) noexcept {
            return std::isfinite(value.m_rotate.x) and
                std::isfinite(value.m_rotate.y) and
                std::isfinite(value.m_rotate.z) and
                std::isfinite(value.m_rotate.w) and
                std::isfinite(value.m_translate.x) and
                std::isfinite(value.m_translate.y) and
                std::isfinite(value.m_translate.z) and
                std::isfinite(value.m_scale.x) and
                std::isfinite(value.m_scale.y) and
                std::isfinite(value.m_scale.z);
        }

        // isInfOrNan/isFinite take a non-deduced param_lvref_t<T>: the template
        // argument is explicit, and const T keeps const test values acceptable
        // (a plain T would demand a mutable lvalue for SIMD/aggregate types).
        template<typename T>
        void expectFinitePolarity(const T &value) {
            PPR_TEST_ASSERT(not isInf(value));
            PPR_TEST_ASSERT(not isNan(value));
            PPR_TEST_ASSERT(not isInfOrNan<const T>(value));
            PPR_TEST_ASSERT(isFinite<const T>(value));
            PPR_TEST_ASSERT(isInfOrNan<const T>(value) == (isInf(value) or isNan(value)));
            PPR_TEST_ASSERT(isFinite<const T>(value) == (not isInfOrNan<const T>(value)));
        }

        template<typename T>
        void expectInfPolarity(const T &value) {
            PPR_TEST_ASSERT(isInf(value));
            PPR_TEST_ASSERT(not isNan(value));
            PPR_TEST_ASSERT(isInfOrNan<const T>(value));
            PPR_TEST_ASSERT(not isFinite<const T>(value));
            PPR_TEST_ASSERT(isInfOrNan<const T>(value) == (isInf(value) or isNan(value)));
            PPR_TEST_ASSERT(isFinite<const T>(value) == (not isInfOrNan<const T>(value)));
        }

        template<typename T>
        void expectNanPolarity(const T &value) {
            PPR_TEST_ASSERT(not isInf(value));
            PPR_TEST_ASSERT(isNan(value));
            PPR_TEST_ASSERT(isInfOrNan<const T>(value));
            PPR_TEST_ASSERT(not isFinite<const T>(value));
            PPR_TEST_ASSERT(isInfOrNan<const T>(value) == (isInf(value) or isNan(value)));
            PPR_TEST_ASSERT(isFinite<const T>(value) == (not isInfOrNan<const T>(value)));
        }

        // fromMatrix(toMatrix(t)) must reproduce t field-for-field (TRS, shear-free).
        void expectDecomposeRoundTrip(const Transform &source) {
            const float4x4 matrix = source.toMatrix();
            const Transform round_trip = Transform::fromMatrix(matrix);

            PPR_TEST_ASSERT(nearEqual(round_trip.toMatrix(), matrix));
            PPR_TEST_ASSERT(nearEqual(round_trip.m_translate, source.m_translate));
            PPR_TEST_ASSERT(nearEqual(round_trip.m_scale, source.m_scale));
            PPR_TEST_ASSERT(nearEqual(round_trip.m_rotate, source.m_rotate));
        }

        PPR_UNIT_TEST (identity) {
            const Transform value;

            PPR_TEST_ASSERT(value.m_rotate.x == 0.0f and value.m_rotate.y == 0.0f and value.m_rotate.z == 0.0f and value.m_rotate.w == 1.0f);
            PPR_TEST_ASSERT(nearEqual(value.m_translate, float3{0.0f, 0.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(value.m_scale, float3{1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(value.getDeterminant() == 1.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);

            PPR_TEST_ASSERT(nearEqual(Transform::identity().m_rotate, value.m_rotate));
            PPR_TEST_ASSERT(nearEqual(Transform::identity().m_translate, value.m_translate));
            PPR_TEST_ASSERT(nearEqual(Transform::identity().m_scale, value.m_scale));

            const float3 probe{1.5f, -2.0f, 0.75f};

            PPR_TEST_ASSERT(nearEqual(value.transformPosition(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.transformPositionNoScale(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.transformVector(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.transformVectorNoScale(probe), probe));

            PPR_TEST_ASSERT(nearEqual(value.invertTransformPosition(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformPositionNoScale(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVector(probe), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVectorNoScale(probe), probe));

            PPR_TEST_ASSERT(nearEqual(value.toMatrix(), float4x4::identity()));
            PPR_TEST_ASSERT(nearEqual(value.toMatrixNoScale(), float4x4::identity()));

            const Transform decomposed = Transform::fromMatrix(float4x4::identity());
            PPR_TEST_ASSERT(nearEqual(decomposed.m_rotate, value.m_rotate));
            PPR_TEST_ASSERT(nearEqual(decomposed.m_translate, value.m_translate));
            PPR_TEST_ASSERT(nearEqual(decomposed.m_scale, value.m_scale));

            const Transform inverted = value.invert();
            PPR_TEST_ASSERT(nearEqual(inverted.m_rotate, value.m_rotate));
            PPR_TEST_ASSERT(nearEqual(inverted.m_translate, value.m_translate));
            PPR_TEST_ASSERT(nearEqual(inverted.m_scale, value.m_scale));
        };

        PPR_UNIT_TEST (to_matrix_exact_values) {
            // Scale only: exact diagonal, no rotation terms, no translation row.
            Transform scaled;
            scaled.m_scale = float3{2.0f, 3.0f, 4.0f};

            const float4x4 scale_matrix = scaled.toMatrix();
            PPR_TEST_ASSERT(rowEquals(scale_matrix, 0, 2.0f, 0.0f, 0.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(scale_matrix, 1, 0.0f, 3.0f, 0.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(scale_matrix, 2, 0.0f, 0.0f, 4.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(scale_matrix, 3, 0.0f, 0.0f, 0.0f, 1.0f));

            // Translation only: row 3 carries (tx, ty, tz, 1), basis stays identity.
            Transform moved;
            moved.m_translate = float3{5.0f, -6.0f, 7.0f};

            const float4x4 move_matrix = moved.toMatrix();
            PPR_TEST_ASSERT(rowEquals(move_matrix, 0, 1.0f, 0.0f, 0.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(move_matrix, 1, 0.0f, 1.0f, 0.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(move_matrix, 2, 0.0f, 0.0f, 1.0f, 0.0f));
            PPR_TEST_ASSERT(rowEquals(move_matrix, 3, 5.0f, -6.0f, 7.0f, 1.0f));

            // Rotation 90 deg about +Z: rows are the rotated basis and agree with the quaternion path.
            Transform spun;
            spun.m_rotate = Quaternion::rotateZ(kHalfPi);

            const float4x4 spin_matrix = spun.toMatrix();
            PPR_TEST_ASSERT(nearEqual(rowOf(spin_matrix, 0), float3{0.0f, 1.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(rowOf(spin_matrix, 1), float3{-1.0f, 0.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(rowOf(spin_matrix, 2), float3{0.0f, 0.0f, 1.0f}));
            PPR_TEST_ASSERT(nearEqual(rowOf(spin_matrix, 0), quaternionTransform(spun.m_rotate, float3{1.0f, 0.0f, 0.0f})));
            PPR_TEST_ASSERT(rowEquals(spin_matrix, 3, 0.0f, 0.0f, 0.0f, 1.0f));

            // Combined S*R*T: the translation row is untouched by scale, and the matrix
            // path must agree with the quaternion point path.
            Transform combined;
            combined.m_scale = float3{2.0f, 3.0f, 4.0f};
            combined.m_rotate = Quaternion::rotateXYZ(0.3f, -0.2f, 0.5f);
            combined.m_translate = float3{1.0f, -2.0f, 3.0f};

            const float4x4 combined_matrix = combined.toMatrix();
            PPR_TEST_ASSERT(nearEqual(rowOf(combined_matrix, 3), combined.m_translate));
            PPR_TEST_ASSERT(combined_matrix(3, 3) == 1.0f);

            const float3 probe{0.75f, -1.5f, 2.25f};
            PPR_TEST_ASSERT(nearEqual(applyToPoint(probe, combined_matrix), combined.transformPosition(probe)));
        };

        PPR_UNIT_TEST (to_matrix_no_scale_is_rotation_times_translation) {
            Transform value;
            value.m_scale = float3{5.0f, 6.0f, 7.0f}; // must be ignored by toMatrixNoScale
            value.m_rotate = Quaternion::rotateZ(kHalfPi);
            value.m_translate = float3{3.0f, -4.0f, 5.0f};

            const float4x4 no_scale = value.toMatrixNoScale();

            // Independent of m_scale: any other scale yields the identical matrix.
            Transform other_scale = value;
            other_scale.m_scale = float3{-2.0f, 0.5f, 100.0f};
            PPR_TEST_ASSERT(nearEqual(other_scale.toMatrixNoScale(), no_scale));

            // Unit-length basis rows and an unscaled translation row.
            PPR_TEST_ASSERT(nearEqual(length(rowOf(no_scale, 0)), 1.0f));
            PPR_TEST_ASSERT(nearEqual(length(rowOf(no_scale, 1)), 1.0f));
            PPR_TEST_ASSERT(nearEqual(length(rowOf(no_scale, 2)), 1.0f));
            PPR_TEST_ASSERT(nearEqual(rowOf(no_scale, 3), value.m_translate));
            PPR_TEST_ASSERT(no_scale(3, 3) == 1.0f);

            // The matrix path and the quaternion point path agree.
            const float3 probe{1.5f, -2.5f, 0.5f};
            PPR_TEST_ASSERT(nearEqual(applyToPoint(probe, no_scale), value.transformPositionNoScale(probe)));

            // toMatrix applies the scale, toMatrixNoScale does not.
            PPR_TEST_ASSERT(not nearEqual(value.toMatrix(), no_scale));
        };

        PPR_UNIT_TEST (from_matrix_to_matrix_round_trip) {
            const Quaternion spin = Quaternion::rotateXYZ(0.3f, -0.2f, 0.5f);
            const float3 translation{1.5f, -2.5f, 3.5f};

            Transform uniform;
            uniform.m_scale = float3{2.0f, 2.0f, 2.0f};
            uniform.m_rotate = spin;
            uniform.m_translate = translation;
            expectDecomposeRoundTrip(uniform);

            Transform non_uniform;
            non_uniform.m_scale = float3{2.0f, 3.0f, 4.0f};
            non_uniform.m_rotate = spin;
            non_uniform.m_translate = translation;
            expectDecomposeRoundTrip(non_uniform);

            // Mirror convention: the negative component survives on X.
            Transform mirrored;
            mirrored.m_scale = float3{-2.0f, 3.0f, 4.0f};
            mirrored.m_rotate = spin;
            mirrored.m_translate = translation;
            expectDecomposeRoundTrip(mirrored);
            PPR_TEST_ASSERT(Transform::fromMatrix(mirrored.toMatrix()).m_scale.x < 0.0f);

            Transform translated_only;
            translated_only.m_translate = translation;
            expectDecomposeRoundTrip(translated_only);
        };

        PPR_UNIT_TEST (to_matrix_from_matrix_round_trip) {
            const float4x4 rotation = float4x4(Quaternion::rotateXYZ(-0.4f, 0.6f, 0.2f));
            const float4x4 translation = float4x4::translate(4.0f, -5.0f, 6.0f);

            // Shear-free positive TRS: exact round trip with a positive determinant.
            const float4x4 positive = float4x4::scale(2.0f, 3.0f, 4.0f) * rotation * translation;
            const Transform from_positive = Transform::fromMatrix(positive);

            PPR_TEST_ASSERT(nearEqual(from_positive.toMatrix(), positive));
            PPR_TEST_ASSERT(from_positive.getDeterminantSign() == 1.0f);
            PPR_TEST_ASSERT(from_positive.m_scale.x > 0.0f);
            PPR_TEST_ASSERT(nearEqual(from_positive.m_translate, float3{4.0f, -5.0f, 6.0f}));

            // Mirroring upper-3x3: recomposes to the same matrix through the neg-X convention.
            const float4x4 mirrored = float4x4::scale(-2.0f, 3.0f, 4.0f) * rotation * translation;
            const Transform from_mirrored = Transform::fromMatrix(mirrored);

            PPR_TEST_ASSERT(nearEqual(from_mirrored.toMatrix(), mirrored));
            PPR_TEST_ASSERT(from_mirrored.getDeterminantSign() == -1.0f);
            PPR_TEST_ASSERT(from_mirrored.m_scale.x < 0.0f);
            PPR_TEST_ASSERT(nearEqual(from_mirrored.m_translate, float3{4.0f, -5.0f, 6.0f}));
        };

        PPR_UNIT_TEST (mirror_fold_into_negative_x) {
            const float4x4 rotation = float4x4(Quaternion::rotateXYZ(0.25f, -0.5f, 0.75f));

            // A mirror on Y alone has negative determinant; the convention folds it onto X.
            const float4x4 mirrored_y = float4x4::scale(1.0f, -1.0f, 1.0f) * rotation;
            const Transform folded = Transform::fromMatrix(mirrored_y);

            PPR_TEST_ASSERT(folded.getDeterminant() < 0.0f);
            PPR_TEST_ASSERT(folded.getDeterminantSign() == -1.0f);
            PPR_TEST_ASSERT(nearEqual(folded.m_scale, float3{-1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(isUnit(folded.m_rotate));

            // Fields were re-conventioned, but the matrix is unchanged.
            PPR_TEST_ASSERT(nearEqual(folded.toMatrix(), mirrored_y));
            PPR_TEST_ASSERT(nearEqual(folded.m_translate, float3{0.0f, 0.0f, 0.0f}));

            // A proper (positive determinant) upper-3x3 keeps a positive X scale.
            const float4x4 proper = float4x4::scale(2.0f, 3.0f, 4.0f) * rotation;
            const Transform kept = Transform::fromMatrix(proper);

            PPR_TEST_ASSERT(kept.getDeterminantSign() == 1.0f);
            PPR_TEST_ASSERT(kept.m_scale.x > 0.0f);
            PPR_TEST_ASSERT(isUnit(kept.m_rotate));
            PPR_TEST_ASSERT(nearEqual(kept.toMatrix(), proper));
        };

        PPR_UNIT_TEST (mirror_z_folds_and_double_mirror_stays_proper) {
            // A mirror on Z alone has negative determinant; the convention folds it onto X.
            const float4x4 mirrored_z = float4x4::scale(1.0f, 1.0f, -1.0f);
            const Transform folded = Transform::fromMatrix(mirrored_z);

            PPR_TEST_ASSERT(nearEqual(folded.m_scale, float3{-1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(folded.getDeterminant() < 0.0f);
            PPR_TEST_ASSERT(folded.getDeterminantSign() == -1.0f);
            PPR_TEST_ASSERT(isUnit(folded.m_rotate));
            PPR_TEST_ASSERT(nearEqual(folded.toMatrix(), mirrored_z));

            // A double mirror has positive determinant: no fold, the sign pair is a rotation.
            const float4x4 double_mirror = float4x4::scale(-1.0f, -1.0f, 1.0f);
            const Transform proper = Transform::fromMatrix(double_mirror);

            PPR_TEST_ASSERT(proper.m_scale.x > 0.0f);
            PPR_TEST_ASSERT(proper.getDeterminantSign() == 1.0f);
            PPR_TEST_ASSERT(isUnit(proper.m_rotate));
            PPR_TEST_ASSERT(nearEqual(proper.toMatrix(), double_mirror));
        };

        PPR_UNIT_TEST (zero_scale_saturates_without_nan) {
            // Every row collapsed to a basis axis: identity rotation, exact zero scale.
            const Transform from_zero_x = Transform::fromMatrix(float4x4::scale(0.0f, 3.0f, 4.0f));
            PPR_TEST_ASSERT(allFinite(from_zero_x) and not isNan(from_zero_x));
            PPR_TEST_ASSERT(nearEqual(from_zero_x.m_scale, float3{0.0f, 3.0f, 4.0f}));
            PPR_TEST_ASSERT(nearEqual(from_zero_x.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(from_zero_x.getDeterminant() == 0.0f);
            PPR_TEST_ASSERT(from_zero_x.getDeterminantSign() == 1.0f);

            const Transform from_zero_y = Transform::fromMatrix(float4x4::scale(2.0f, 0.0f, 4.0f));
            PPR_TEST_ASSERT(allFinite(from_zero_y) and not isNan(from_zero_y));
            PPR_TEST_ASSERT(nearEqual(from_zero_y.m_scale, float3{2.0f, 0.0f, 4.0f}));
            PPR_TEST_ASSERT(nearEqual(from_zero_y.m_rotate, Quaternion::identity()));

            const Transform from_zero_z = Transform::fromMatrix(float4x4::scale(2.0f, 3.0f, 0.0f));
            PPR_TEST_ASSERT(allFinite(from_zero_z) and not isNan(from_zero_z));
            PPR_TEST_ASSERT(nearEqual(from_zero_z.m_scale, float3{2.0f, 3.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(from_zero_z.m_rotate, Quaternion::identity()));

            const Transform from_zero_all = Transform::fromMatrix(float4x4::scale(0.0f, 0.0f, 0.0f));
            PPR_TEST_ASSERT(allFinite(from_zero_all) and not isNan(from_zero_all));
            PPR_TEST_ASSERT(nearEqual(from_zero_all.m_scale, float3{0.0f, 0.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(from_zero_all.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(from_zero_all.getDeterminant() == 0.0f);
            PPR_TEST_ASSERT(from_zero_all.getDeterminantSign() == 1.0f);

            // A collapsed row under a real rotation and translation still saturates to 0.
            const float4x4 with_rotation = float4x4::scale(0.0f, 3.0f, 4.0f) * float4x4(Quaternion::rotateXYZ(0.3f, 0.4f, 0.5f)) * float4x4::translate(2.0f, -3.0f,
                                               4.0f);
            const Transform under_rotation = Transform::fromMatrix(with_rotation);

            PPR_TEST_ASSERT(allFinite(under_rotation) and not isNan(under_rotation));
            PPR_TEST_ASSERT(under_rotation.m_scale.x == 0.0f);
            PPR_TEST_ASSERT(nearEqual(std::abs(under_rotation.m_scale.y), 3.0f));
            PPR_TEST_ASSERT(nearEqual(std::abs(under_rotation.m_scale.z), 4.0f));
            PPR_TEST_ASSERT(nearEqual(under_rotation.m_translate, float3{2.0f, -3.0f, 4.0f}));
            PPR_TEST_ASSERT(isUnit(under_rotation.m_rotate));
        };

        PPR_UNIT_TEST (two_lane_collapse_saturates_without_nan) {
            // Two collapsed lanes at once: each saturates to an exact 0 with a basis-axis
            // fallback, so the multi-bit maskToInt path (0x1 | 0x2, 0x1 | 0x4) stays finite.
            const Transform collapsed_xy = Transform::fromMatrix(float4x4::scale(0.0f, 0.0f, 4.0f));

            PPR_TEST_ASSERT(allFinite(collapsed_xy) and not isNan(collapsed_xy));
            PPR_TEST_ASSERT(collapsed_xy.m_scale.x == 0.0f and collapsed_xy.m_scale.y == 0.0f);
            PPR_TEST_ASSERT(nearEqual(collapsed_xy.m_scale, float3{0.0f, 0.0f, 4.0f}));
            PPR_TEST_ASSERT(nearEqual(collapsed_xy.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(isUnit(collapsed_xy.m_rotate));
            PPR_TEST_ASSERT(collapsed_xy.getDeterminant() == 0.0f);

            const Transform collapsed_xz = Transform::fromMatrix(float4x4::scale(0.0f, 3.0f, 0.0f));

            PPR_TEST_ASSERT(allFinite(collapsed_xz) and not isNan(collapsed_xz));
            PPR_TEST_ASSERT(collapsed_xz.m_scale.x == 0.0f and collapsed_xz.m_scale.z == 0.0f);
            PPR_TEST_ASSERT(nearEqual(collapsed_xz.m_scale, float3{0.0f, 3.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(collapsed_xz.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(isUnit(collapsed_xz.m_rotate));
            PPR_TEST_ASSERT(collapsed_xz.getDeterminant() == 0.0f);
        };

        PPR_UNIT_TEST (epsilon_band_collapses_near_zero_extent) {
            // fromMatrix collapses rows at or below epsilon_v<float> (10 * eps = 1.1920929e-6):
            // 1e-7 is inside the band, 1e-5 is outside it.
            const Transform inside = Transform::fromMatrix(float4x4::scale(1e-7f, 1.0f, 1.0f));

            PPR_TEST_ASSERT(inside.m_scale.x == 0.0f);
            PPR_TEST_ASSERT(nearEqual(inside.m_scale.y, 1.0f) and nearEqual(inside.m_scale.z, 1.0f));
            PPR_TEST_ASSERT(nearEqual(inside.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(isUnit(inside.m_rotate));
            PPR_TEST_ASSERT(allFinite(inside) and not isNan(inside));

            const Transform outside = Transform::fromMatrix(float4x4::scale(1e-5f, 1.0f, 1.0f));

            PPR_TEST_ASSERT(outside.m_scale.x != 0.0f);
            PPR_TEST_ASSERT(nearEqual(outside.m_scale.x, 1e-5f));
            PPR_TEST_ASSERT(nearEqual(outside.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(isUnit(outside.m_rotate));
            PPR_TEST_ASSERT(allFinite(outside) and not isNan(outside));
        };

        PPR_UNIT_TEST (shear_dropped_to_orthonormal_rotation) {
            const float4x4 sheared{
                float4{1.0f, 0.0f, 0.0f, 0.0f},
                float4{0.5f, 1.0f, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{2.0f, -3.0f, 4.0f, 1.0f},
            };

            const Transform value = Transform::fromMatrix(sheared);

            // The rotation part is unit length and orthonormal once the shear is gone.
            PPR_TEST_ASSERT(isUnit(value.m_rotate));

            const float4x4 basis = float4x4(value.m_rotate);
            const float3 x_axis = rowOf(basis, 0);
            const float3 y_axis = rowOf(basis, 1);
            const float3 z_axis = rowOf(basis, 2);

            PPR_TEST_ASSERT(nearEqual(length(x_axis), 1.0f));
            PPR_TEST_ASSERT(nearEqual(length(y_axis), 1.0f));
            PPR_TEST_ASSERT(nearEqual(length(z_axis), 1.0f));
            PPR_TEST_ASSERT(std::abs(dot(x_axis, y_axis)) < kEps);
            PPR_TEST_ASSERT(std::abs(dot(x_axis, z_axis)) < kEps);
            PPR_TEST_ASSERT(std::abs(dot(y_axis, z_axis)) < kEps);

            // Translation survives; the shear itself does not.
            PPR_TEST_ASSERT(nearEqual(value.m_translate, float3{2.0f, -3.0f, 4.0f}));
            PPR_TEST_ASSERT(not nearEqual(value.toMatrix(), sheared));
        };

        PPR_UNIT_TEST (multiply_matches_matrix_product) {
            const Quaternion a_spin = Quaternion::rotateXYZ(0.4f, -0.3f, 0.7f);
            const Quaternion b_spin = Quaternion::rotateXYZ(-0.6f, 0.2f, 0.1f);
            const float3 probe{0.75f, -1.25f, 2.5f};

            // Uniform scale keeps the scalar fast path exact against the matrix product.
            Transform a;
            a.m_scale = float3{2.0f, 2.0f, 2.0f};
            a.m_rotate = a_spin;
            a.m_translate = float3{1.0f, 2.0f, -3.0f};

            Transform b;
            b.m_scale = float3{0.5f, 0.5f, 0.5f};
            b.m_rotate = b_spin;
            b.m_translate = float3{-4.0f, 5.0f, 6.0f};

            const float4x4 expected = a.toMatrix() * b.toMatrix();
            const Transform composed = Transform::multiply(a, b);

            PPR_TEST_ASSERT(nearEqual(composed.toMatrix(), expected));
            PPR_TEST_ASSERT(nearEqual(composed.transformPosition(probe), applyToPoint(probe, expected)));

            // Apply-a-then-b point order.
            PPR_TEST_ASSERT(nearEqual(composed.transformPosition(probe), b.transformPosition(a.transformPosition(probe))));

            // Non-uniform scale with identity rotations is exact too (S commutes with I).
            Transform plain_a;
            plain_a.m_scale = float3{2.0f, 3.0f, 4.0f};

            Transform plain_b;
            plain_b.m_scale = float3{0.5f, 2.0f, 1.5f};
            plain_b.m_translate = float3{1.0f, -1.0f, 1.0f};

            const Transform plain = Transform::multiply(plain_a, plain_b);

            PPR_TEST_ASSERT(nearEqual(plain.m_scale, float3{1.0f, 6.0f, 6.0f}));
            PPR_TEST_ASSERT(nearEqual(plain.toMatrix(), plain_a.toMatrix() * plain_b.toMatrix()));
            PPR_TEST_ASSERT(nearEqual(plain.transformPosition(probe), plain_b.transformPosition(plain_a.transformPosition(probe))));
        };

        PPR_UNIT_TEST (multiply_mirror_uses_matrix_fallback) {
            Transform a;
            a.m_scale = float3{-2.0f, 3.0f, 4.0f};
            a.m_rotate = Quaternion::rotateXYZ(0.3f, 0.5f, -0.2f);
            a.m_translate = float3{1.0f, -2.0f, 3.0f};

            Transform b;
            b.m_scale = float3{2.0f, 2.0f, 2.0f};
            b.m_rotate = Quaternion::rotateXYZ(-0.1f, 0.4f, 0.6f);
            b.m_translate = float3{-1.0f, 0.5f, 2.0f};

            const float4x4 expected = a.toMatrix() * b.toMatrix();
            const Transform composed = Transform::multiply(a, b);
            const float3 probe{-0.5f, 1.75f, 2.0f};

            // The fallback is exactly the matrix product (b is not inverted).
            PPR_TEST_ASSERT(nearEqual(composed.toMatrix(), expected));
            PPR_TEST_ASSERT(nearEqual(composed.transformPosition(probe), applyToPoint(probe, expected)));
            PPR_TEST_ASSERT(composed.getDeterminantSign() == -1.0f);

            // Two mirrored operands compose back to a positive determinant. Uniform
            // magnitude on the second operand keeps the product free of shear, so the
            // matrix round trip still holds (non-uniform S does not commute with R).
            Transform mirror_b = b;
            mirror_b.m_scale = float3{-2.0f, -2.0f, -2.0f};

            const float4x4 both_expected = a.toMatrix() * mirror_b.toMatrix();
            const Transform both = Transform::multiply(a, mirror_b);

            PPR_TEST_ASSERT(both.getDeterminantSign() == 1.0f);
            PPR_TEST_ASSERT(nearEqual(both.toMatrix(), both_expected));
            PPR_TEST_ASSERT(nearEqual(both.transformPosition(probe), applyToPoint(probe, both_expected)));
        };

        PPR_UNIT_TEST (multiply_b_only_mirror_uses_matrix_fallback) {
            // A healthy `a` with a mirrored `b` still routes through the matrix fallback.
            Transform a;
            a.m_scale = float3{2.0f, 2.0f, 2.0f};
            a.m_rotate = Quaternion::rotateXYZ(0.3f, -0.2f, 0.1f);
            a.m_translate = float3{1.0f, -2.0f, 3.0f};

            Transform b;
            b.m_scale = float3{2.0f, -2.0f, 2.0f};
            b.m_rotate = Quaternion::rotateXYZ(-0.1f, 0.4f, 0.6f);
            b.m_translate = float3{-1.0f, 0.5f, 2.0f};

            const float4x4 expected = a.toMatrix() * b.toMatrix();
            const Transform composed = Transform::multiply(a, b);
            const float3 probe{-0.5f, 1.75f, 2.0f};

            PPR_TEST_ASSERT(composed.getDeterminantSign() == -1.0f);
            PPR_TEST_ASSERT(nearEqual(composed.toMatrix(), expected));
            PPR_TEST_ASSERT(nearEqual(composed.transformPosition(probe), applyToPoint(probe, expected)));
        };

        PPR_UNIT_TEST (relative_transform_matches_from_times_to_inverse) {
            Transform from;
            from.m_scale = float3{2.0f, 2.0f, 2.0f};
            from.m_rotate = Quaternion::rotateXYZ(0.35f, -0.25f, 0.55f);
            from.m_translate = float3{3.0f, -1.0f, 2.0f};

            // Uniform `to` scale keeps to.invert().toMatrix() equal to the full inverse.
            Transform to;
            to.m_scale = float3{0.5f, 0.5f, 0.5f};
            to.m_rotate = Quaternion::rotateXYZ(-0.45f, 0.15f, -0.35f);
            to.m_translate = float3{-2.0f, 4.0f, 1.0f};

            const float4x4 expected = from.toMatrix() * inverse(to.toMatrix());
            const Transform relative = Transform::relativeTransform(from, to);
            const float3 probe{1.25f, -0.75f, 2.75f};

            PPR_TEST_ASSERT(nearEqual(relative.toMatrix(), expected));
            PPR_TEST_ASSERT(nearEqual(relative.transformPosition(probe), applyToPoint(probe, expected)));
            PPR_TEST_ASSERT(nearEqual(relative.transformPosition(probe), to.invert().transformPosition(from.transformPosition(probe))));
            PPR_TEST_ASSERT(relative.getDeterminantSign() == 1.0f);

            // Mirrored `from` routes through the matrix fallback with the same semantics.
            Transform mirrored = from;
            mirrored.m_scale = float3{-2.0f, 2.0f, 2.0f};

            const float4x4 mirrored_expected = mirrored.toMatrix() * inverse(to.toMatrix());
            const Transform mirrored_relative = Transform::relativeTransform(mirrored, to);

            PPR_TEST_ASSERT(nearEqual(mirrored_relative.toMatrix(), mirrored_expected));
            PPR_TEST_ASSERT(nearEqual(mirrored_relative.transformPosition(probe), applyToPoint(probe, mirrored_expected)));
            PPR_TEST_ASSERT(mirrored_relative.getDeterminantSign() == -1.0f);
        };

        PPR_UNIT_TEST (invert_round_trip) {
            const float3 probe{1.5f, -2.0f, 0.75f};

            // Uniform scale: point and matrix round trips are both exact.
            Transform uniform;
            uniform.m_scale = float3{2.0f, 2.0f, 2.0f};
            uniform.m_rotate = Quaternion::rotateXYZ(0.45f, -0.35f, 0.65f);
            uniform.m_translate = float3{3.0f, -4.0f, 5.0f};

            const Transform uniform_inverse = uniform.invert();

            PPR_TEST_ASSERT(nearEqual(uniform_inverse.m_scale, float3{0.5f, 0.5f, 0.5f}));
            PPR_TEST_ASSERT(nearEqual(uniform_inverse.transformPosition(uniform.transformPosition(probe)), probe));
            PPR_TEST_ASSERT(nearEqual(uniform.toMatrix() * uniform_inverse.toMatrix(), float4x4::identity()));
            PPR_TEST_ASSERT(nearEqual(uniform_inverse.invert().m_rotate, uniform.m_rotate));

            // invertNoScale leaves m_scale at its default and inverts the rigid part.
            Transform no_scale_source = uniform;
            no_scale_source.m_scale = float3{-4.0f, 0.25f, 9.0f};

            const Transform no_scale_inverse = no_scale_source.invertNoScale();

            PPR_TEST_ASSERT(nearEqual(no_scale_inverse.m_scale, float3{1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(nearEqual(no_scale_inverse.transformPositionNoScale(no_scale_source.transformPositionNoScale(probe)), probe));
            PPR_TEST_ASSERT(nearEqual(no_scale_inverse.invertNoScale().m_rotate, no_scale_source.m_rotate));

            // Non-uniform scale: the per-call point inverse stays exact.
            Transform non_uniform;
            non_uniform.m_scale = float3{10.0f, 1.0f, 1.0f};
            non_uniform.m_rotate = Quaternion::rotateZ(kHalfPi);
            non_uniform.m_translate = float3{1.0f, -2.0f, 3.0f};

            PPR_TEST_ASSERT(nearEqual(non_uniform.invertTransformPosition(non_uniform.transformPosition(probe)), probe));

            // ...but S and R do not commute, so a naive TRS inverse is only approximate.
            // invert() asserts uniform scale, so replicate its construction locally here.
            Transform naive_inverse;
            naive_inverse.m_rotate = conjugate(non_uniform.m_rotate);
            naive_inverse.m_scale = rcp(non_uniform.m_scale);
            naive_inverse.m_translate = quaternionTransform(naive_inverse.m_rotate, -non_uniform.m_translate * naive_inverse.m_scale);

            const float3 axis_probe{1.0f, 0.0f, 0.0f};
            const float3 residue = applyToPoint(axis_probe, non_uniform.toMatrix() * naive_inverse.toMatrix());
            PPR_TEST_ASSERT(nearEqual(residue, float3{10.0f, 0.0f, 0.0f}));
        };

        PPR_UNIT_TEST (point_and_vector_variants) {
            const Quaternion spin = Quaternion::rotateXYZ(0.3f, -0.55f, 0.2f);
            const float3 translation{2.0f, -3.5f, 1.25f};
            const float3 scale{2.0f, 3.0f, 4.0f};
            const float3 probe{0.75f, -1.5f, 2.25f};

            Transform value;
            value.m_rotate = spin;
            value.m_translate = translation;
            value.m_scale = scale;

            // Forward: scale, then rotate, then translate (points) / scale+rotate (vectors).
            PPR_TEST_ASSERT(nearEqual(value.transformPosition(probe), quaternionTransform(spin, probe * scale) + translation));
            PPR_TEST_ASSERT(nearEqual(value.transformPositionNoScale(probe), quaternionTransform(spin, probe) + translation));
            PPR_TEST_ASSERT(nearEqual(value.transformVector(probe), quaternionTransform(spin, probe * scale)));
            PPR_TEST_ASSERT(nearEqual(value.transformVectorNoScale(probe), quaternionTransform(spin, probe)));

            // Inverse: undo the translation, rotate back, then undo the scale.
            const Quaternion back = inverse(spin);

            PPR_TEST_ASSERT(nearEqual(value.invertTransformPosition(probe), quaternionTransform(back, probe - translation) * rcp(scale)));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformPositionNoScale(probe), quaternionTransform(back, probe - translation)));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVector(probe), quaternionTransform(back, probe) * rcp(scale)));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVectorNoScale(probe), quaternionTransform(back, probe)));

            // conjugate() (the fast path) agrees with the general inverse on a unit quaternion.
            PPR_TEST_ASSERT(nearEqual(conjugate(spin), back));

            // Every inverse variant round-trips its matching forward variant.
            PPR_TEST_ASSERT(nearEqual(value.invertTransformPosition(value.transformPosition(probe)), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformPositionNoScale(value.transformPositionNoScale(probe)), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVector(value.transformVector(probe)), probe));
            PPR_TEST_ASSERT(nearEqual(value.invertTransformVectorNoScale(value.transformVectorNoScale(probe)), probe));

            // With a uniform scale, hoisting invert() once per node matches the per-call helpers.
            Transform uniform = value;
            uniform.m_scale = float3{2.0f, 2.0f, 2.0f};

            PPR_TEST_ASSERT(nearEqual(uniform.invert().transformPosition(probe), uniform.invertTransformPosition(probe)));
            PPR_TEST_ASSERT(nearEqual(uniform.invertNoScale().transformPositionNoScale(probe), uniform.invertTransformPositionNoScale(probe)));
            PPR_TEST_ASSERT(nearEqual(uniform.invert().transformVector(probe), uniform.invertTransformVector(probe)));
            PPR_TEST_ASSERT(nearEqual(uniform.invertNoScale().transformVectorNoScale(probe), uniform.invertTransformVectorNoScale(probe)));
        };

        PPR_UNIT_TEST (accumulate_variants) {
            const Quaternion base = Quaternion::rotateXYZ(0.2f, 0.3f, -0.1f);
            const Quaternion delta_spin = Quaternion::rotateXYZ(-0.4f, 0.15f, 0.5f);

            Transform delta;
            delta.m_rotate = delta_spin;
            delta.m_translate = float3{-0.5f, 0.25f, 1.5f};
            delta.m_scale = float3{0.5f, 2.0f, 1.0f};

            // accumulate: rotation composes as delta * current, translate adds, scale multiplies.
            Transform value;
            value.m_rotate = base;
            value.m_translate = float3{1.0f, 2.0f, 3.0f};
            value.m_scale = float3{2.0f, 3.0f, 4.0f};

            Transform &returned = value.accumulate(delta);

            PPR_TEST_ASSERT(&returned == &value);
            PPR_TEST_ASSERT(nearEqual(value.m_rotate, delta_spin * base));
            PPR_TEST_ASSERT(nearEqual(value.m_translate, float3{0.5f, 2.25f, 4.5f}));
            PPR_TEST_ASSERT(nearEqual(value.m_scale, float3{1.0f, 6.0f, 4.0f}));
            PPR_TEST_ASSERT(isUnit(value.m_rotate));

            // An identity-rotation delta is skipped by the guard; translate/scale still apply.
            Transform guarded;
            guarded.m_rotate = base;
            guarded.m_translate = float3{1.0f, 0.0f, 0.0f};
            guarded.m_scale = float3{2.0f, 2.0f, 2.0f};
            PPR_TEST_ASSERT(&guarded.accumulate(Transform::identity()) == &guarded);

            PPR_TEST_ASSERT(nearEqual(guarded.m_rotate, base));
            PPR_TEST_ASSERT(nearEqual(guarded.m_translate, float3{1.0f, 0.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(guarded.m_scale, float3{2.0f, 2.0f, 2.0f}));

            // accumulateWithAdditiveScale: scale *= (1 + delta.scale).
            Transform additive;
            additive.m_rotate = base;
            additive.m_translate = float3{1.0f, 1.0f, 1.0f};
            additive.m_scale = float3{2.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(&additive.accumulateWithAdditiveScale(delta) == &additive);

            PPR_TEST_ASSERT(nearEqual(additive.m_rotate, delta_spin * base));
            PPR_TEST_ASSERT(nearEqual(additive.m_translate, float3{0.5f, 1.25f, 2.5f}));
            PPR_TEST_ASSERT(nearEqual(additive.m_scale, float3{3.0f, 9.0f, 8.0f}));

            // Same guard: an identity delta leaves the rotation alone (scale still doubles).
            Transform additive_guarded;
            additive_guarded.m_rotate = base;
            additive_guarded.m_scale = float3{2.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(&additive_guarded.accumulateWithAdditiveScale(Transform::identity()) == &additive_guarded);

            PPR_TEST_ASSERT(nearEqual(additive_guarded.m_rotate, base));
            PPR_TEST_ASSERT(nearEqual(additive_guarded.m_scale, float3{4.0f, 6.0f, 8.0f}));

            // accumulateWithShortestRotation: flip an antipodal delta, then renormalize.
            const Quaternion small = Quaternion::rotateXYZ(0.05f, -0.02f, 0.03f);

            Transform positive_side;
            positive_side.m_rotate = base;

            Transform negative_side;
            negative_side.m_rotate = base;

            Transform positive_delta;
            positive_delta.m_rotate = small;

            Transform negative_delta;
            negative_delta.m_rotate = -small;

            PPR_TEST_ASSERT(&positive_side.accumulateWithShortestRotation(positive_delta) == &positive_side);
            PPR_TEST_ASSERT(&negative_side.accumulateWithShortestRotation(negative_delta) == &negative_side);

            PPR_TEST_ASSERT(nearEqual(positive_side.m_rotate, negative_side.m_rotate));
            PPR_TEST_ASSERT(nearEqual(positive_side.m_rotate, normalize(base + small)));
            PPR_TEST_ASSERT(isUnit(positive_side.m_rotate));
            PPR_TEST_ASSERT(isUnit(negative_side.m_rotate));
        };

        PPR_UNIT_TEST (blend_shortest_arc) {
            Transform a;
            a.m_rotate = Quaternion::identity();
            a.m_translate = float3{0.0f, 0.0f, 0.0f};
            a.m_scale = float3{1.0f, 1.0f, 1.0f};

            Transform b;
            b.m_rotate = Quaternion::rotateZ(kHalfPi);
            b.m_translate = float3{10.0f, 20.0f, 30.0f};
            b.m_scale = float3{3.0f, 5.0f, 7.0f};

            // Alpha at or below epsilon returns a unchanged.
            const Transform low = Transform::blend(a, b, 0.0f);
            PPR_TEST_ASSERT(nearEqual(low.m_rotate, a.m_rotate));
            PPR_TEST_ASSERT(nearEqual(low.m_translate, a.m_translate));
            PPR_TEST_ASSERT(nearEqual(low.m_scale, a.m_scale));

            // Alpha at or above 1 - epsilon returns b unchanged.
            const Transform high = Transform::blend(a, b, 1.0f);
            PPR_TEST_ASSERT(nearEqual(high.m_rotate, b.m_rotate));
            PPR_TEST_ASSERT(nearEqual(high.m_translate, b.m_translate));
            PPR_TEST_ASSERT(nearEqual(high.m_scale, b.m_scale));

            // Midpoint: linear translation/scale, renormalized rotation on the short arc.
            const Transform mid = Transform::blend(a, b, 0.5f);
            PPR_TEST_ASSERT(nearEqual(mid.m_translate, float3{5.0f, 10.0f, 15.0f}));
            PPR_TEST_ASSERT(nearEqual(mid.m_scale, float3{2.0f, 3.0f, 4.0f}));
            PPR_TEST_ASSERT(isUnit(mid.m_rotate));
            PPR_TEST_ASSERT(mid.m_rotate.z > 0.0f);

            // An antipodal b is flipped back onto a's arc before lerping.
            Transform antipodal_b = b;
            antipodal_b.m_rotate = -b.m_rotate;

            const Transform flipped = Transform::blend(a, antipodal_b, 0.5f);
            PPR_TEST_ASSERT(nearEqual(flipped.m_rotate, mid.m_rotate));
            PPR_TEST_ASSERT(flipped.m_rotate.z > 0.0f);

            // Fully antipodal endpoints still normalize to a valid rotation.
            Transform mirror_a = a;
            mirror_a.m_rotate = -a.m_rotate;

            const Transform degenerate = Transform::blend(a, mirror_a, 0.5f);
            PPR_TEST_ASSERT(isUnit(degenerate.m_rotate));
            PPR_TEST_ASSERT(nearEqual(degenerate.m_rotate, Quaternion::identity()));
        };

        PPR_UNIT_TEST (determinant_signs) {
            Transform value;

            PPR_TEST_ASSERT(value.getDeterminant() == 1.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);

            value.m_scale = float3{2.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == 24.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);

            value.m_scale = float3{-2.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == -24.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == -1.0f);

            value.m_scale = float3{2.0f, -3.0f, 4.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == -24.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == -1.0f);

            value.m_scale = float3{-2.0f, -3.0f, 4.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == 24.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);

            value.m_scale = float3{0.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == 0.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);

            value.m_scale = float3{0.0f, 0.0f, 0.0f};
            PPR_TEST_ASSERT(value.getDeterminant() == 0.0f);
            PPR_TEST_ASSERT(value.getDeterminantSign() == 1.0f);
        };

        PPR_UNIT_TEST (has_uniform_scale) {
            // Signed range compare: hmax - hmin <= hmax(abs) * epsilon_v<float>.
            Transform value;

            value.m_scale = float3{1.0f, 1.0f, 1.0f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            value.m_scale = float3{2.0f, 2.0f, 2.0f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            value.m_scale = float3{2.0f, 3.0f, 4.0f};
            PPR_TEST_ASSERT(not value.hasUniformScale());

            value.m_scale = float3{2.0f, 1.0f, 1.0f};
            PPR_TEST_ASSERT(not value.hasUniformScale());

            // Scalar mirror (-s,-s,-s) is a true scalar matrix and commutes with R.
            value.m_scale = float3{-2.0f, -2.0f, -2.0f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            // Odd mirror is magnitude-uniform but does not commute with R.
            value.m_scale = float3{2.0f, 2.0f, -2.0f};
            PPR_TEST_ASSERT(not value.hasUniformScale());

            // Degenerate but equal: callers must still guard rcp(S).
            value.m_scale = float3{0.0f, 0.0f, 0.0f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            // Relative epsilon band at ref 1: 1e-7 sits inside, 1e-5 sits outside.
            value.m_scale = float3{1.0f, 1.0f, 1.0f + 1e-7f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            value.m_scale = float3{1.0f, 1.0f, 1.0f + 1e-5f};
            PPR_TEST_ASSERT(not value.hasUniformScale());

            // Tight band both sides of epsilon_v<float> * ref.
            const float eps = epsilon_v<float>;
            value.m_scale = float3{1.0f, 1.0f, 1.0f + eps * 0.5f};
            PPR_TEST_ASSERT(value.hasUniformScale());

            value.m_scale = float3{1.0f, 1.0f, 1.0f + eps * 2.0f};
            PPR_TEST_ASSERT(not value.hasUniformScale());
        };

        PPR_UNIT_TEST (transform_normal_matches_inverse_transpose) {
            // Row-vector spelling of transpose(inverse(M)) applied to the normal (w = 0):
            // direction * transpose(inverse(M)). The transpose matters: plain inverse(M)
            // yields the conjugate rotation, which is a sign flip away under rotation.
            // (2,1,1) plus rotation discriminates against rotate/scale ordering shortcuts.
            Transform value;
            value.m_scale = float3{2.0f, 1.0f, 1.0f};
            value.m_rotate = Quaternion::rotateXYZ(0.3f, -0.55f, 0.2f);

            const float4x4 inverse_transpose = transpose(inverse(value.toMatrix()));

            const float3 probes[] = {
                float3{1.0f, 0.0f, 0.0f},
                float3{0.0f, 1.0f, 0.0f},
                float3{0.0f, 0.0f, 1.0f},
                float3{0.5f, -0.75f, 0.25f},
            };

            for (const float3 probe: probes) {
                const float3 direction = normalize(probe);
                const float3 expected = normalize((float4(direction, 0.0f) * inverse_transpose).xyz);
                PPR_TEST_ASSERT(nearEqual(value.transformNormal(direction), expected));
            }

            // Under non-uniform scale the normal path differs from the plain vector path.
            const float3 axis = normalize(float3{1.0f, 1.0f, 0.0f});
            PPR_TEST_ASSERT(not nearEqual(value.transformNormal(axis), normalize(value.transformVector(axis))));

            // Uniform scale degenerates to the rotated normal.
            Transform uniform = value;
            uniform.m_scale = float3{3.0f, 3.0f, 3.0f};

            const float3 spin_probe = normalize(float3{0.25f, 1.0f, -0.5f});
            PPR_TEST_ASSERT(nearEqual(uniform.transformNormal(spin_probe), quaternionTransform(uniform.m_rotate, spin_probe)));
        };

        PPR_UNIT_TEST (invert_transform_normal_round_trip) {
            Transform value;
            value.m_scale = float3{2.0f, 1.0f, 0.5f};
            value.m_rotate = Quaternion::rotateXYZ(-0.4f, 0.6f, 0.2f);
            value.m_translate = float3{3.0f, -4.0f, 5.0f}; // must not affect normals

            const float3 probes[] = {
                float3{1.0f, 0.0f, 0.0f},
                float3{0.0f, 1.0f, 0.0f},
                float3{0.0f, 0.0f, 1.0f},
                float3{0.5f, -0.75f, 0.25f},
            };

            for (const float3 probe: probes) {
                const float3 direction = normalize(probe);
                PPR_TEST_ASSERT(nearEqual(value.invertTransformNormal(value.transformNormal(direction)), direction));
                PPR_TEST_ASSERT(nearEqual(value.transformNormal(value.invertTransformNormal(direction)), direction));
            }

            // Ground truth: conjugate rotation as a transposed basis matrix, then scale.
            const float4x4 back_basis = transpose(float4x4(value.m_rotate)) * float4x4::scale(value.m_scale.x, value.m_scale.y, value.m_scale.z);

            for (const float3 probe: probes) {
                const float3 direction = normalize(probe);
                const float3 expected = normalize((float4(direction, 0.0f) * back_basis).xyz);
                PPR_TEST_ASSERT(nearEqual(value.invertTransformNormal(direction), expected));
            }
        };

        PPR_UNIT_TEST (orthonormalize_variants) {
            // Already orthonormal: the tangent survives unchanged.
            PPR_TEST_ASSERT(nearEqual(orthonormalize(float3{0.0f, 1.0f, 0.0f}, float3{1.0f, 0.0f, 0.0f}), float3{1.0f, 0.0f, 0.0f}));

            // Non-unit normal, skewed tangent: Gram-Schmidt projection, then normalize.
            const float3 skewed = orthonormalize(float3{0.0f, 2.0f, 0.0f}, float3{1.0f, 1.0f, 0.0f});

            PPR_TEST_ASSERT(nearEqual(skewed, float3{1.0f, 0.0f, 0.0f}));
            PPR_TEST_ASSERT(nearEqual(length(skewed), 1.0f));
            PPR_TEST_ASSERT(std::abs(dot(float3{0.0f, 1.0f, 0.0f}, skewed)) < kEps);

            // Degenerate (tangent parallel to normal): falls back to a perpendicular unit axis.
            const float3 fallback = orthonormalize(float3{0.0f, 1.0f, 0.0f}, float3{0.0f, 5.0f, 0.0f});

            PPR_TEST_ASSERT(nearEqual(length(fallback), 1.0f));
            PPR_TEST_ASSERT(std::abs(dot(float3{0.0f, 1.0f, 0.0f}, fallback)) < kEps);

            // Degenerate normal: safeNormalize substitutes +Y before orthogonalizing.
            PPR_TEST_ASSERT(nearEqual(orthonormalize(float3{0.0f, 0.0f, 0.0f}, float3{1.0f, 0.0f, 0.0f}), float3{1.0f, 0.0f, 0.0f}));
        };

        PPR_UNIT_TEST (fallback_perpendicular_variants) {
            // Axis-aligned inputs: the result is unit length and perpendicular.
            const float3 axes[] = {
                float3{1.0f, 0.0f, 0.0f},
                float3{0.0f, 1.0f, 0.0f},
                float3{0.0f, 0.0f, 1.0f},
            };

            for (const float3 axis: axes) {
                const float3 result = fallbackPerpendicular(axis);
                PPR_TEST_ASSERT(nearEqual(length(result), 1.0f));
                PPR_TEST_ASSERT(std::abs(dot(axis, result)) < kEps);
            }

            // Skewed input stays perpendicular after the cross, never near-parallel.
            const float3 skewed_input = normalize(float3{1.0f, 2.0f, 3.0f});
            const float3 skewed_result = fallbackPerpendicular(skewed_input);

            PPR_TEST_ASSERT(nearEqual(length(skewed_result), 1.0f));
            PPR_TEST_ASSERT(std::abs(dot(skewed_input, skewed_result)) < kEps);

            // Zero input: the cross is zero, so safeNormalize returns the +X fallback exactly.
            const float3 zero_result = fallbackPerpendicular(float3{0.0f, 0.0f, 0.0f});

            PPR_TEST_ASSERT(zero_result.x == 1.0f and zero_result.y == 0.0f and zero_result.z == 0.0f);
        };

        PPR_UNIT_TEST (swap_transform_exchanges_fields) {
            Transform a{Quaternion::rotateZ(kHalfPi), float3{1.0f, 2.0f, 3.0f}, float3{2.0f, 2.0f, 2.0f}};
            Transform b{Quaternion::identity(), float3{-4.0f, 5.0f, -6.0f}, float3{1.0f, 3.0f, 5.0f}};

            const Transform expected_a = a;
            const Transform expected_b = b;

            swap(a, b);

            PPR_TEST_ASSERT(nearEqual(a.m_rotate, expected_b.m_rotate));
            PPR_TEST_ASSERT(nearEqual(a.m_translate, expected_b.m_translate));
            PPR_TEST_ASSERT(nearEqual(a.m_scale, expected_b.m_scale));
            PPR_TEST_ASSERT(nearEqual(b.m_rotate, expected_a.m_rotate));
            PPR_TEST_ASSERT(nearEqual(b.m_translate, expected_a.m_translate));
            PPR_TEST_ASSERT(nearEqual(b.m_scale, expected_a.m_scale));
        };

        PPR_UNIT_TEST (angular_velocity_variants) {
            // No rotation change: zero velocity.
            PPR_TEST_ASSERT(nearEqual(angularVelocity(1.0f, Quaternion::identity(), Quaternion::identity()), float3{0.0f, 0.0f, 0.0f}));

            // 90 deg about +Z in one second: pi/2 rad/s about +Z.
            PPR_TEST_ASSERT(nearEqual(angularVelocity(1.0f, Quaternion::identity(), Quaternion::rotateZ(kHalfPi)), float3{0.0f, 0.0f, kHalfPi}));

            // Half the time budget doubles the rate.
            PPR_TEST_ASSERT(
                nearEqual(angularVelocity(0.5f, Quaternion::identity(), Quaternion::rotateZ(kHalfPi)), float3{0.0f, 0.0f, 2.0f * kHalfPi}));

            // Antipodal target flips onto the shortest arc: -identity is the same rotation.
            PPR_TEST_ASSERT(nearEqual(angularVelocity(1.0f, Quaternion::identity(), -Quaternion::identity()), float3{0.0f, 0.0f, 0.0f}));
        };

        PPR_UNIT_TEST (is_nan_and_free_inverse) {
            // Uniform scale keeps the free inverse() an exact point inverse.
            const Transform value{Quaternion::rotateXYZ(0.2f, 0.4f, 0.6f), float3{1.0f, 2.0f, 3.0f}, float3{2.0f, 2.0f, 2.0f}};
            const float3 probe{1.0f, -2.0f, 3.0f};
            const float nan = std::numeric_limits<float>::quiet_NaN();

            PPR_TEST_ASSERT(not isNan(value));
            PPR_TEST_ASSERT(not isNan(Transform::identity()));

            Transform broken = value;
            broken.m_rotate.x = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_translate.y = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_scale.z = nan;
            PPR_TEST_ASSERT(isNan(broken));

            // The free inverse overload delegates to invert().
            const Transform inverted = inverse(value);

            PPR_TEST_ASSERT(nearEqual(inverted.m_rotate, value.invert().m_rotate));
            PPR_TEST_ASSERT(nearEqual(inverted.m_translate, value.invert().m_translate));
            PPR_TEST_ASSERT(nearEqual(inverted.m_scale, value.invert().m_scale));
            PPR_TEST_ASSERT(nearEqual(inverted.transformPosition(value.transformPosition(probe)), probe));
        };

        PPR_UNIT_TEST (is_nan_remaining_lanes) {
            // The remaining 7 of 10 lanes (mirrors the 3 covered by is_nan_and_free_inverse).
            const Transform value{Quaternion::rotateXYZ(0.2f, 0.4f, 0.6f), float3{1.0f, 2.0f, 3.0f}, float3{2.0f, 2.0f, 2.0f}};
            const float nan = std::numeric_limits<float>::quiet_NaN();

            Transform broken = value;
            broken.m_rotate.y = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_rotate.z = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_rotate.w = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_translate.x = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_translate.z = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_scale.x = nan;
            PPR_TEST_ASSERT(isNan(broken));

            broken = value;
            broken.m_scale.y = nan;
            PPR_TEST_ASSERT(isNan(broken));
        };

        PPR_UNIT_TEST (is_inf_scalar_polarity) {
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            expectFinitePolarity(1.5f);

            expectInfPolarity(inf);
            expectInfPolarity(-inf);

            expectNanPolarity(nan);
        };

        PPR_UNIT_TEST (is_inf_quaternion_all_lanes) {
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            expectFinitePolarity(Quaternion::identity());

            Quaternion broken = Quaternion::identity();
            broken.x = inf;
            expectInfPolarity(broken);

            broken = Quaternion::identity();
            broken.y = inf;
            expectInfPolarity(broken);

            broken = Quaternion::identity();
            broken.z = inf;
            expectInfPolarity(broken);

            broken = Quaternion::identity();
            broken.w = inf;
            expectInfPolarity(broken);

            broken = Quaternion::identity();
            broken.x = nan;
            expectNanPolarity(broken);

            broken = Quaternion::identity();
            broken.y = nan;
            expectNanPolarity(broken);

            broken = Quaternion::identity();
            broken.z = nan;
            expectNanPolarity(broken);

            broken = Quaternion::identity();
            broken.w = nan;
            expectNanPolarity(broken);
        };

        PPR_UNIT_TEST (is_inf_vector_all_lanes) {
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            expectFinitePolarity(float3{1.0f, 2.0f, 3.0f});
            expectFinitePolarity(float4{1.0f, 2.0f, 3.0f, 4.0f});

            float3 broken3{1.0f, 2.0f, 3.0f};
            broken3.x = inf;
            expectInfPolarity(broken3);

            broken3 = float3{1.0f, 2.0f, 3.0f};
            broken3.y = inf;
            expectInfPolarity(broken3);

            broken3 = float3{1.0f, 2.0f, 3.0f};
            broken3.z = inf;
            expectInfPolarity(broken3);

            broken3 = float3{1.0f, 2.0f, 3.0f};
            broken3.x = nan;
            expectNanPolarity(broken3);

            broken3 = float3{1.0f, 2.0f, 3.0f};
            broken3.y = nan;
            expectNanPolarity(broken3);

            broken3 = float3{1.0f, 2.0f, 3.0f};
            broken3.z = nan;
            expectNanPolarity(broken3);

            float4 broken4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.x = inf;
            expectInfPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.y = inf;
            expectInfPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.z = inf;
            expectInfPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.w = inf;
            expectInfPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.x = nan;
            expectNanPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.y = nan;
            expectNanPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.z = nan;
            expectNanPolarity(broken4);

            broken4 = float4{1.0f, 2.0f, 3.0f, 4.0f};
            broken4.w = nan;
            expectNanPolarity(broken4);
        };

        PPR_UNIT_TEST (is_inf_matrix_all_columns) {
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            expectFinitePolarity(float4x4::identity());

            // Each injection targets row 0 of a distinct column, exercising the column path.
            float4x4 broken = float4x4::identity();
            broken(0, 0) = inf;
            expectInfPolarity(broken);

            broken = float4x4::identity();
            broken(0, 1) = inf;
            expectInfPolarity(broken);

            broken = float4x4::identity();
            broken(0, 2) = inf;
            expectInfPolarity(broken);

            broken = float4x4::identity();
            broken(0, 3) = inf;
            expectInfPolarity(broken);

            broken = float4x4::identity();
            broken(0, 0) = nan;
            expectNanPolarity(broken);

            broken = float4x4::identity();
            broken(0, 1) = nan;
            expectNanPolarity(broken);

            broken = float4x4::identity();
            broken(0, 2) = nan;
            expectNanPolarity(broken);

            broken = float4x4::identity();
            broken(0, 3) = nan;
            expectNanPolarity(broken);
        };

        PPR_UNIT_TEST (is_inf_transform_first_lanes) {
            // First 3 of 10 lanes (mirrors the 3 covered by is_nan_and_free_inverse).
            const Transform value{Quaternion::rotateXYZ(0.2f, 0.4f, 0.6f), float3{1.0f, 2.0f, 3.0f}, float3{2.0f, 2.0f, 2.0f}};
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            expectFinitePolarity(value);
            expectFinitePolarity(Transform::identity());

            Transform broken = value;
            broken.m_rotate.x = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_translate.y = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_scale.z = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_rotate.x = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_translate.y = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_scale.z = nan;
            expectNanPolarity(broken);
        };

        PPR_UNIT_TEST (is_inf_transform_remaining_lanes) {
            // The remaining 7 of 10 lanes (mirrors is_nan_remaining_lanes).
            const Transform value{Quaternion::rotateXYZ(0.2f, 0.4f, 0.6f), float3{1.0f, 2.0f, 3.0f}, float3{2.0f, 2.0f, 2.0f}};
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();

            Transform broken = value;
            broken.m_rotate.y = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_rotate.z = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_rotate.w = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_translate.x = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_translate.z = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_scale.x = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_scale.y = inf;
            expectInfPolarity(broken);

            broken = value;
            broken.m_rotate.y = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_rotate.z = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_rotate.w = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_translate.x = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_translate.z = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_scale.x = nan;
            expectNanPolarity(broken);

            broken = value;
            broken.m_scale.y = nan;
            expectNanPolarity(broken);
        };

        PPR_UNIT_TEST (healthy_matrix_collapses_no_lanes) {
            // Baseline: a healthy matrix collapses no lanes (collapsed_i & 0x7 == 0), so
            // every scale component survives positive and the rotation is untouched.
            const Transform value = Transform::fromMatrix(float4x4::identity());

            PPR_TEST_ASSERT(nearEqual(value.m_scale, float3{1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(value.m_scale.x > 0.0f and value.m_scale.y > 0.0f and value.m_scale.z > 0.0f);
            PPR_TEST_ASSERT(nearEqual(value.m_rotate, Quaternion::identity()));
            PPR_TEST_ASSERT(nearEqual(value.m_translate, float3{0.0f, 0.0f, 0.0f}));
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest transform = UnitTest::Named("transform") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::MathTransform::identity,
            detail::MathTransform::to_matrix_exact_values,
            detail::MathTransform::to_matrix_no_scale_is_rotation_times_translation,
            detail::MathTransform::from_matrix_to_matrix_round_trip,
            detail::MathTransform::to_matrix_from_matrix_round_trip,
            detail::MathTransform::mirror_fold_into_negative_x,
            detail::MathTransform::mirror_z_folds_and_double_mirror_stays_proper,
            detail::MathTransform::zero_scale_saturates_without_nan,
            detail::MathTransform::two_lane_collapse_saturates_without_nan,
            detail::MathTransform::epsilon_band_collapses_near_zero_extent,
            detail::MathTransform::shear_dropped_to_orthonormal_rotation,
            detail::MathTransform::multiply_matches_matrix_product,
            detail::MathTransform::multiply_mirror_uses_matrix_fallback,
            detail::MathTransform::multiply_b_only_mirror_uses_matrix_fallback,
            detail::MathTransform::relative_transform_matches_from_times_to_inverse,
            detail::MathTransform::invert_round_trip,
            detail::MathTransform::point_and_vector_variants,
            detail::MathTransform::accumulate_variants,
            detail::MathTransform::blend_shortest_arc,
            detail::MathTransform::determinant_signs,
            detail::MathTransform::has_uniform_scale,
            detail::MathTransform::transform_normal_matches_inverse_transpose,
            detail::MathTransform::invert_transform_normal_round_trip,
            detail::MathTransform::orthonormalize_variants,
            detail::MathTransform::fallback_perpendicular_variants,
            detail::MathTransform::swap_transform_exchanges_fields,
            detail::MathTransform::angular_velocity_variants,
            detail::MathTransform::is_nan_and_free_inverse,
            detail::MathTransform::is_nan_remaining_lanes,
            detail::MathTransform::is_inf_scalar_polarity,
            detail::MathTransform::is_inf_quaternion_all_lanes,
            detail::MathTransform::is_inf_vector_all_lanes,
            detail::MathTransform::is_inf_matrix_all_columns,
            detail::MathTransform::is_inf_transform_first_lanes,
            detail::MathTransform::is_inf_transform_remaining_lanes,
            detail::MathTransform::healthy_matrix_collapses_no_lanes,
        });
    };

    const UnitTest &transformTests() noexcept {
        return transform;
    }
} // namespace pP::tests
