module;

#include "pP/Macros.h"
#include <mango/math/math.hpp>

export module engine.math;

import engine.core;
import std;

namespace pP::math::details {
    template<std::floating_point T>
    [[nodiscard]] constexpr T roundHalfAwayFromZero(const T value) noexcept {
        return std::round(value);
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr mango::math::Vector<T, DimV> roundHalfAwayFromZero(const mango::math::Vector<T, DimV> &value) noexcept {
        return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> mango::math::Vector<T, DimV> {
            mango::math::Vector<T, DimV> result{};
            ((result[idx] = std::round(value[idx])), ...);
            return result;
        });
    }

    template<typename ToT, typename FromT, u32 DimV>
    [[nodiscard]] constexpr mango::math::Vector<ToT, DimV> vectorCast(const mango::math::Vector<FromT, DimV> &value) noexcept {
        return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> mango::math::Vector<ToT, DimV> {
            mango::math::Vector<ToT, DimV> result{};
            ((result[idx] = static_cast<ToT>(value[idx])), ...);
            return result;
        });
    }

    template<>
    [[nodiscard]] constexpr mango::math::Vector<i32, 4u> vectorCast<i32, float, 4u>(const mango::math::Vector<float, 4u> &value) noexcept {
        return mango::math::truncate<mango::math::Vector<i32, 4u> >(value);
    }

    template<>
    [[nodiscard]] constexpr mango::math::Vector<float, 4u> vectorCast<float, i32, 4u>(const mango::math::Vector<i32, 4u> &value) noexcept {
        return mango::math::convert<mango::math::Vector<float, 4u> >(value);
    }

    template<>
    [[nodiscard]] constexpr mango::math::Vector<float, 4u> vectorCast<float, u32, 4u>(const mango::math::Vector<u32, 4u> &value) noexcept {
        return mango::math::convert<mango::math::Vector<float, 4u> >(value);
    }

    template<>
    [[nodiscard]] constexpr mango::math::Vector<u32, 4u> vectorCast<u32, float, 4u>(const mango::math::Vector<float, 4u> &value) noexcept {
        return mango::math::convert<mango::math::Vector<u32, 4u> >(value);
    }

    template<typename ToT, typename FromT>
    [[nodiscard]] constexpr ToT scalarCast(const FromT value) noexcept {
        return static_cast<ToT>(value);
    }
}

export namespace pP {
    using namespace mango::math;

    using int2 = int32x2;
    using int3 = int32x3;
    using int4 = int32x4;

    using uint2 = uint32x2;
    using uint3 = uint32x3;
    using uint4 = uint32x4;

    using float2 = float32x2;
    using float3 = float32x3;
    using float4 = float32x4;

    using double2 = float64x2;
    using double3 = float64x3;
    using double4 = float64x4;

    using float3x3 = Matrix3x3;
    using float4x4 = Matrix4x4;

    using mango::math::abs;
    using mango::math::acos;
    using mango::math::asin;
    using mango::math::atan;
    using mango::math::atan2;
    using mango::math::ceil;
    using mango::math::clamp;
    using mango::math::cos;
    using mango::math::cross;
    using mango::math::distance;
    using mango::math::dot;
    using mango::math::exp;
    using mango::math::exp2;
    using mango::math::floor;
    using mango::math::fract;
    using mango::math::hmax;
    using mango::math::hmin;
    using mango::math::inverse;
    using mango::math::length;
    using mango::math::lerp;
    using mango::math::log;
    using mango::math::log2;
    using mango::math::max;
    using mango::math::min;
    using mango::math::mod;
    using mango::math::normalize;
    using mango::math::pow;
    using mango::math::rcp;
    using mango::math::reflect;
    using mango::math::refract;
    using mango::math::round;
    using mango::math::rsqrt;
    using mango::math::sign;
    using mango::math::sin;
    using mango::math::smoothstep;
    using mango::math::sqrt;
    using mango::math::square;
    using mango::math::tan;
    using mango::math::transpose;
    using mango::math::trunc;

    using mango::math::AngleAxis;
    using mango::math::EulerAngles;
    using mango::math::Quaternion;

    using mango::math::conjugate;
    using mango::math::slerp;
    using mango::math::squad;

    using mango::math::packQuaternion;
    using mango::math::unpackQuaternion;

    using mango::math::Box;
    using mango::math::Cone;
    using mango::math::FastRay;
    using mango::math::Frustum;
    using mango::math::Plane;
    using mango::math::Quadratic;
    using mango::math::Ray;
    using mango::math::RayFrustum;
    using mango::math::Rectangle;
    using mango::math::Sphere;
    using mango::math::Triangle;

    namespace math {
        inline const float3 axis_x{1, 0, 0};
        inline const float3 axis_y{0, 1, 0};
        inline const float3 axis_z{0, 0, 1};

        inline const float3 right{axis_x};
        inline const float3 left{-axis_x};

        inline const float3 up{axis_y};
        inline const float3 down{-axis_y};

        inline const float3 forward{axis_z};
        inline const float3 backward{-axis_z};

        using mango::math::Intersect;
        using mango::math::IntersectBarycentric;
        using mango::math::IntersectBarycentricTwosided;
        using mango::math::IntersectRange;
        using mango::math::IntersectSolid;

        using mango::math::easeInBack;
        using mango::math::easeInBounce;
        using mango::math::easeInCircular;
        using mango::math::easeInCubic;
        using mango::math::easeInElastic;
        using mango::math::easeInExponential;
        using mango::math::easeInQuadratic;
        using mango::math::easeInQuartic;
        using mango::math::easeInSine;

        using mango::math::easeOutBack;
        using mango::math::easeOutBounce;
        using mango::math::easeOutCircular;
        using mango::math::easeOutCubic;
        using mango::math::easeOutElastic;
        using mango::math::easeOutExponential;
        using mango::math::easeOutQuadratic;
        using mango::math::easeOutQuartic;
        using mango::math::easeOutSine;

        using mango::math::easeInOutBack;
        using mango::math::easeInOutBounce;
        using mango::math::easeInOutCircular;
        using mango::math::easeInOutCubic;
        using mango::math::easeInOutElastic;
        using mango::math::easeInOutExponential;
        using mango::math::easeInOutQuadratic;
        using mango::math::easeInOutQuartic;
        using mango::math::easeInOutSine;

        namespace details {
            template<typename T>
            concept TArithmetic = std::is_arithmetic_v<T>;

            template<auto ValueGeneratorV, typename T>
            concept TValueGenerator = requires
            {
                ValueGeneratorV.template operator()<T>();
            };

            // Accept any callable object as a Non-Type Template Parameter
            template<auto ValueGeneratorV>
            struct PolymorphicConstant {
                // Constrain it to floating point types to match std::numbers
                template<typename T>
                    requires TValueGenerator<ValueGeneratorV, T>
                // ReSharper disable once CppNonExplicitConversionOperator
                [[nodiscard]] constexpr operator T() const noexcept {
                    // We use .template operator()<T>() to explicitly call the lambda's template
                    return ValueGeneratorV.template operator()<T>();
                }

                template<typename T>
                    requires TValueGenerator<ValueGeneratorV, T>
                [[nodiscard]] constexpr bool operator ==(const T value) const noexcept {
                    return ValueGeneratorV.template operator()<T>() == value;
                }

                template<typename T>
                    requires TValueGenerator<ValueGeneratorV, T>
                [[nodiscard]] constexpr auto operator <=>(const T value) const noexcept {
                    return ValueGeneratorV.template operator()<T>() <=> value;
                }
            };

            template<class T>
            struct InvalidConstantType {
                static_assert(!sizeof(T *),
                    "A program that instantiates a primary template of a mathematical constant variable template is ill-formed. (N4950 [math.constants]/3)");
            };

            template<typename T, auto>
            struct Number {
                static constexpr InvalidConstantType<T> value{};
            };

            template<typename T, auto ValueGeneratorV>
                requires TValueGenerator<ValueGeneratorV, T>
            struct Number<T, ValueGeneratorV> {
                static constexpr T value{ValueGeneratorV.template operator()<T>()};
            };

            template<auto ValueGeneratorV>
            struct Number<void, ValueGeneratorV> {
                static constexpr PolymorphicConstant<ValueGeneratorV> value{};
            };

            struct IdentityValue {
                template<typename IdentityT>
                    requires requires
                    {
                        { IdentityT::identity() } -> std::convertible_to<IdentityT>;
                    }
                // ReSharper disable once CppNonExplicitConversionOperator
                [[nodiscard]] constexpr operator IdentityT() const noexcept {
                    return IdentityT::identity();
                }
            };
        }

        template<details::TArithmetic T, std::size_t DimV>
        using Vector = mango::math::Vector<T, DimV>;

        template<details::TArithmetic T, std::size_t WidthV, std::size_t HeightV>
        using Matrix = mango::math::Matrix<T, WidthV, HeightV>;

        // 128 bit vector masks
        using mango::math::mask8x16;
        using mango::math::mask16x8;
        using mango::math::mask32x4;
        using mango::math::mask64x2;

        // 256 bit vector masks
        using mango::math::mask8x32;
        using mango::math::mask16x16;
        using mango::math::mask32x8;
        using mango::math::mask64x4;

        // 512 bit vector masks
        using mango::math::mask8x64;
        using mango::math::mask16x32;
        using mango::math::mask32x16;
        using mango::math::mask64x8;

        using mango::math::maskToInt;

        using mango::math::add;
        using mango::math::sub;
        using mango::math::mul;
        using mango::math::div;

        using mango::math::all_of;
        using mango::math::any_of;
        using mango::math::none_of;
    }

#define PPR_POLYMORPHIC_BASIC_NUMBER(_CONCEPT, _NAME, ...) \
    template<typename ValueT = void> \
    constexpr auto _NAME = math::details::Number<ValueT, \
        []<_CONCEPT T>() constexpr noexcept { \
            return __VA_ARGS__; \
        }>::value

#define PPR_POLYMORPHIC_ARITHMETIC(_NAME, ...) \
    PPR_POLYMORPHIC_BASIC_NUMBER(math::details::TArithmetic, _NAME, __VA_ARGS__)
#define PPR_POLYMORPHIC_FLOAT(_NAME, ...) \
    PPR_POLYMORPHIC_BASIC_NUMBER(std::floating_point, _NAME, __VA_ARGS__)
#define PPR_POLYMORPHIC_STD_NUMBER(_NAME) \
    PPR_POLYMORPHIC_FLOAT(_NAME, std::numbers::_NAME<T>)

    PPR_POLYMORPHIC_STD_NUMBER(e_v);
    PPR_POLYMORPHIC_STD_NUMBER(log2e_v);
    PPR_POLYMORPHIC_STD_NUMBER(log10e_v);
    PPR_POLYMORPHIC_STD_NUMBER(pi_v);
    PPR_POLYMORPHIC_STD_NUMBER(inv_pi_v);
    PPR_POLYMORPHIC_STD_NUMBER(inv_sqrtpi_v);
    PPR_POLYMORPHIC_STD_NUMBER(ln2_v);
    PPR_POLYMORPHIC_STD_NUMBER(ln10_v);
    PPR_POLYMORPHIC_STD_NUMBER(sqrt2_v);
    PPR_POLYMORPHIC_STD_NUMBER(sqrt3_v);
    PPR_POLYMORPHIC_STD_NUMBER(inv_sqrt3_v);
    PPR_POLYMORPHIC_STD_NUMBER(egamma_v);
    PPR_POLYMORPHIC_STD_NUMBER(phi_v);

    PPR_POLYMORPHIC_FLOAT(pi_over_2_v, pi_v<T> / 2);
    PPR_POLYMORPHIC_FLOAT(pi_over_3_v, pi_v<T> / 3);
    PPR_POLYMORPHIC_FLOAT(pi_over_4_v, pi_v<T> / 4);

    // Type-relative zero-detection band: 10x eps for floats (covers float32
    // normalize() output, 3.58e-7 measured over a 20k-direction sweep), 0 for
    // ints (== today's static_cast<int>(1e-8), so int asserts keep `> 0`).
    // Single expression: the macro wraps this in `return ...;`.
    PPR_POLYMORPHIC_ARITHMETIC(epsilon_v,
        std::floating_point<T> ? 10 * std::numeric_limits<T>::epsilon() : T{0});
    PPR_POLYMORPHIC_ARITHMETIC(infinity_v, std::numeric_limits<T>::infinity());

#undef PPR_POLYMORPHIC_STD_NUMBER
#undef PPR_POLYMORPHIC_FLOAT
#undef PPR_POLYMORPHIC_ARITHMETIC
#undef PPR_POLYMORPHIC_BASIC_NUMBER

    constexpr math::details::IdentityValue identity_v;

    [[nodiscard]] constexpr float4x4 makeJitterMatrix(const float2 &jitter) noexcept {
        float4x4 result{float4x4::identity()};
        result[3][0] = jitter.x;
        result[3][1] = jitter.y;
        return result;
    }

    [[nodiscard]] Frustum makeZeroToOneFrustum(const float4x4 &viewProjection) noexcept {
        const float4x4 depth_transform{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 2.0f, 0.0f},
            float4{0.0f, 0.0f, -1.0f, 1.0f},
        };
        return Frustum{viewProjection * depth_transform};
    }

    [[nodiscard]] float3 quaternionTransform(const Quaternion &q, const float3 &v) noexcept {
        return mango::math::operator*(v, q);
    }

    [[nodiscard]] float3 angularVelocity(const float seconds, const Quaternion &from, const Quaternion &to) noexcept {
        PPR_ASSUME(seconds > 0);

        const Quaternion actual_to{dot(from, to) < 0 ? -to : to};
        const Quaternion angular_delta = normalize(actual_to * conjugate(from));

        const float3 v(angular_delta.x, angular_delta.y, angular_delta.z);
        const float sin_half = length(v);
        const float half_angle = std::atan2(sin_half, angular_delta.w);
        const float3 axis = sin_half > epsilon_v<> ? v / sin_half : math::axis_z;

        // FP: mango vector-scalar operator* via `using` + ADL resolves under MSVC 19.52 /WX-clean;
        // IDE CL-262.9437.136 cannot consume MSVC BMIs (Math.cppm:317).
        // Scope: next line only (clang-diagnostic-error); re-check after toolchain/BMI refresh.
        // NOLINTNEXTLINE(clang-diagnostic-error)
        return axis * (2.0f * half_angle / seconds);
    }

    using mango::math::operator!=;
    using mango::math::operator&;
    using mango::math::operator*;
    using mango::math::operator*=;
    using mango::math::operator+;
    using mango::math::operator+=;
    using mango::math::operator-;
    using mango::math::operator-=;
    using mango::math::operator/;
    using mango::math::operator/=;
    using mango::math::operator<;
    using mango::math::operator<=;
    using mango::math::operator<<;
    using mango::math::operator==;
    using mango::math::operator>;
    using mango::math::operator>=;
    using mango::math::operator>>;
    using mango::math::operator^;
    using mango::math::operator|;
    using mango::math::operator~;

    template<typename T>
        requires requires(T x) { mango::math::dot(x, x); }
    [[nodiscard]] constexpr auto dot2(const T &x) noexcept {
        return mango::math::dot(x, x);
    }

    template<math::details::TArithmetic T>
    [[nodiscard]] constexpr auto dot2(T x) noexcept {
        return x * x;
    }

    // checked_cast integration
    template<std::integral ToT, std::integral FromT, u32 DimV>
    [[nodiscard]] constexpr Vector<ToT, DimV> checked_cast(const Vector<FromT, DimV> &value) noexcept {
        return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> Vector<ToT, DimV> {
            return Vector<ToT, DimV>(
                checked_cast<ToT>(value[idx])...);
        });
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr i32 ceilToInt(const T value) noexcept {
        return math::details::scalarCast<i32>(ceil(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<i32, DimV> ceilToInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<i32, T, DimV>(ceil(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr i32 floorToInt(const T value) noexcept {
        return math::details::scalarCast<i32>(floor(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<i32, DimV> floorToInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<i32, T, DimV>(floor(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr i32 roundToInt(const T value) noexcept {
        return math::details::scalarCast<i32>(math::details::roundHalfAwayFromZero(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<i32, DimV> roundToInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<i32, T, DimV>(math::details::roundHalfAwayFromZero<T, DimV>(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr i32 truncToInt(const T value) noexcept {
        return math::details::scalarCast<i32>(trunc(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<i32, DimV> truncToInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<i32, T, DimV>(trunc(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr u32 ceilToUInt(const T value) noexcept {
        return math::details::scalarCast<u32>(ceil(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<u32, DimV> ceilToUInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<u32, T, DimV>(ceil(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr u32 floorToUInt(const T value) noexcept {
        return math::details::scalarCast<u32>(floor(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<u32, DimV> floorToUInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<u32, T, DimV>(floor(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr u32 roundToUInt(const T value) noexcept {
        return math::details::scalarCast<u32>(math::details::roundHalfAwayFromZero(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<u32, DimV> roundToUInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<u32, T, DimV>(math::details::roundHalfAwayFromZero<T, DimV>(value));
    }

    template<std::floating_point T>
    [[nodiscard]] constexpr u32 truncToUInt(const T value) noexcept {
        return math::details::scalarCast<u32>(trunc(value));
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<u32, DimV> truncToUInt(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<u32, T, DimV>(trunc(value));
    }

    template<std::integral T>
    [[nodiscard]] constexpr float toFloat(const T value) noexcept {
        return math::details::scalarCast<float>(value);
    }

    template<std::integral T, u32 DimV>
    [[nodiscard]] constexpr Vector<float, DimV> toFloat(const Vector<T, DimV> &value) noexcept {
        return math::details::vectorCast<float, T, DimV>(value);
    }

    template<typename T, u32 DimV>
    [[nodiscard]] constexpr Vector<T, DimV> saturate(const Vector<T, DimV> &value) noexcept {
        return clamp(value, Vector<T, DimV>(0), Vector<T, DimV>(1));
    }

    // check if inf
    template<std::floating_point T>
    [[nodiscard]] constexpr bool isInf(const T value) noexcept {
        return std::isinf(value);
    }

    // check if any component isInf
    [[nodiscard]] constexpr bool isInf(const Quaternion &quat) noexcept {
        return isInf(quat.x) or isInf(quat.y) or isInf(quat.z) or isInf(quat.w);
    }

    // check if any component isInf
    template<std::floating_point T, std::size_t DimV>
    [[nodiscard]] constexpr bool isInf(const Vector<T, DimV> &value) noexcept {
        return pP::static_iota<std::size_t, DimV>([&](auto... idx) constexpr noexcept -> bool {
            return (isInf(value[idx]) or ...);
        });
    }

    // check if any column isInf
    template<std::floating_point T, std::size_t WidthV, std::size_t HeightV>
    [[nodiscard]] constexpr bool isInf(const Matrix<T, WidthV, HeightV> &value) noexcept {
        return pP::static_iota<u32, WidthV>([&](auto... idx) constexpr noexcept -> bool {
            return (isInf(value.template column<idx>()) or ...);
        });
    }

    // check if nan
    template<std::floating_point T>
    [[nodiscard]] constexpr bool isNan(const T value) noexcept {
        return std::isnan(value);
    }

    // check if any component isnan
    [[nodiscard]] constexpr bool isNan(const Quaternion &quat) noexcept {
        return isNan(quat.x) or isNan(quat.y) or isNan(quat.z) or isNan(quat.w);
    }

    // check if any component isnan
    template<std::floating_point T, std::size_t DimV>
    [[nodiscard]] constexpr bool isNan(const Vector<T, DimV> &value) noexcept {
        return pP::static_iota<std::size_t, DimV>([&](auto... idx) constexpr noexcept -> bool {
            return (isNan(value[idx]) or ...);
        });
    }

    // check if any column isnan
    template<std::floating_point T, std::size_t WidthV, std::size_t HeightV>
    [[nodiscard]] constexpr bool isNan(const Matrix<T, WidthV, HeightV> &value) noexcept {
        return pP::static_iota<u32, WidthV>([&](auto... idx) constexpr noexcept -> bool {
            return (isNan(value.template column<idx>()) or ...);
        });
    }

    // Single source of truth is epsilon_v above (10x eps for floats, 0 for ints).
    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr bool isNormalized(const Vector<T, DimV> &value, const T epsilon = epsilon_v<T>) noexcept {
        return std::abs(1 - dot2(value)) < epsilon;
    }

    [[nodiscard]] constexpr bool isNormalized(const Quaternion &value, const float epsilon = epsilon_v<>) noexcept {
        return std::abs(1 - dot2(value)) < epsilon;
    }

    template<std::floating_point T, u32 DimV>
    [[nodiscard]] constexpr Vector<T, DimV> safeNormalize(const Vector<T, DimV> &value, const Vector<T, DimV> &fallback, const T epsilon = epsilon_v<T>) noexcept {
        const T norm_sq = dot(value, value);
        return norm_sq > epsilon ? value / std::sqrt(norm_sq) : fallback;
    }

    namespace math {
        // Row-vector TRS (scale, then rotate, then translate). Per-node guidance:
        // compose with multiply(); keep one cached invert() per dirty node with the
        // parent inverse hoisted (never invert per child); materialize toMatrix() once
        // per node at the draw leaf; never call invertTransform* per vertex -- hoist
        // invert() once per node and use transformPosition() on the cached result.
        struct Transform {
            Quaternion m_rotate = Quaternion::identity();
            float3 m_translate{0};
            float3 m_scale{1};

            [[nodiscard]] static Transform identity() noexcept {
                return Transform{};
            }

            [[nodiscard]] float getDeterminant() const noexcept {
                return m_scale.x * m_scale.y * m_scale.z;
            }

            // Scalar gate for the rare per-node matrix fallback; never a per-point path.
            [[nodiscard]] float getDeterminantSign() const noexcept {
                return getDeterminant() < 0.0f ? -1.0f : 1.0f;
            }

            [[nodiscard]] bool hasUniformScale() const noexcept {
                // Signed compare, not abs: (2,2,-2) is magnitude-uniform but does NOT
                // commute with R, so TRS invert stays approximate; (-s,-s,-s) is a true
                // scalar matrix and commutes, so it counts. Zero (0,0,0) reports uniform
                // (degenerate but equal) — callers must still guard rcp(S).
                const float4 scale4 = m_scale.xyzx;
                return (maskToInt(hmax(scale4) - hmin(scale4) <= hmax(abs(scale4)) * epsilon_v<float>) & 0x1u) != 0u;
            }


            Transform &accumulate(const Transform &delta) noexcept {
                // delta is in the parent frame: rotation composes as delta * current.
                if (dot2(delta.m_rotate.w) < 1.0f - epsilon_v<float>) {
                    m_rotate = delta.m_rotate * m_rotate;
                }

                m_translate += delta.m_translate;
                m_scale *= delta.m_scale;
                return *this;
            }

            Transform &accumulateWithAdditiveScale(const Transform &delta) noexcept {
                if (dot2(delta.m_rotate.w) < 1.0f - epsilon_v<float>) {
                    m_rotate = delta.m_rotate * m_rotate;
                }

                m_translate += delta.m_translate;
                m_scale *= 1.0f + delta.m_scale;
                return *this;
            }

            Transform &accumulateWithShortestRotation(const Transform &delta) noexcept {
                if (dot(m_rotate, delta.m_rotate) < 0.0f) {
                    m_rotate -= delta.m_rotate;
                } else {
                    m_rotate += delta.m_rotate;
                }

                // Component-wise add/sub drifts off the unit sphere: restore it.
                m_rotate = normalize(m_rotate);

                m_translate += delta.m_translate;
                m_scale *= delta.m_scale;
                return *this;
            }


            [[nodiscard]] float3 transformPosition(const float3 &position) const noexcept {
                return quaternionTransform(m_rotate, position * m_scale) + m_translate;
            }

            [[nodiscard]] float3 transformPositionNoScale(const float3 &position) const noexcept {
                return quaternionTransform(m_rotate, position) + m_translate;
            }

            [[nodiscard]] float3 transformNormal(const float3 &normal) const noexcept {
                return normalize(quaternionTransform(m_rotate, normal * rcp(m_scale)));
            }

            [[nodiscard]] float3 transformVector(const float3 &vector) const noexcept {
                return quaternionTransform(m_rotate, vector * m_scale);
            }

            [[nodiscard]] float3 transformVectorNoScale(const float3 &vector) const noexcept {
                return quaternionTransform(m_rotate, vector);
            }


            [[nodiscard]] float3 invertTransformPosition(const float3 &position) const noexcept {
                return quaternionTransform(conjugate(m_rotate), position - m_translate) * rcp(m_scale);
            }

            [[nodiscard]] float3 invertTransformPositionNoScale(const float3 &position) const noexcept {
                return quaternionTransform(conjugate(m_rotate), position - m_translate);
            }

            [[nodiscard]] float3 invertTransformNormal(const float3 &normal) const noexcept {
                return normalize(quaternionTransform(conjugate(m_rotate), normal) * m_scale);
            }

            [[nodiscard]] float3 invertTransformVector(const float3 &vector) const noexcept {
                return quaternionTransform(conjugate(m_rotate), vector) * rcp(m_scale);
            }

            [[nodiscard]] float3 invertTransformVectorNoScale(const float3 &vector) const noexcept {
                return quaternionTransform(conjugate(m_rotate), vector);
            }


            // m_rotate stays unit, so conjugate() is its inverse (3 negations, no divides).
            // Non-uniform scale + rotation: inverse/composition is approximate (S,R do not commute); exact for uniform scale.
            [[nodiscard]] Transform invert() const noexcept {
                PPR_ASSERT(hasUniformScale() && "Transform::invert() only approximates for non-uniform scale");

                Transform result;
                result.m_rotate = conjugate(m_rotate);
                result.m_scale = rcp(m_scale);
                result.m_translate = quaternionTransform(result.m_rotate, -m_translate * result.m_scale);
                return result;
            }

            [[nodiscard]] Transform invertNoScale() const noexcept {
                Transform result;
                result.m_rotate = conjugate(m_rotate);
                result.m_translate = quaternionTransform(result.m_rotate, -m_translate);
                return result;
            }


            // Row-vector contract: v * M applies scale, then rotation, then translation.
            // Batch rule: compose Transforms per node, materialize this matrix once per node
            // at the draw leaf, and push point clouds through the float4 x Matrix4x4 SIMD
            // path -- no custom batch kernel.
            [[nodiscard]] PPR_FLATTEN float4x4 toMatrix() const noexcept {
                return float4x4::scale(m_scale.x, m_scale.y, m_scale.z) * toMatrixNoScale();
            }

            [[nodiscard]] float4x4 toMatrixNoScale() const noexcept {
                return float4x4(m_rotate) * float4x4::translate(m_translate.x, m_translate.y, m_translate.z);
            }

            // Shear-free TRS only: rows 0..2 are the scaled basis (|row| = scale
            // component, unit rows = rotation); shear is dropped because a TRS
            // cannot represent it.
            [[nodiscard]] static Transform fromMatrix(const float4x4 &transform) noexcept {
                const float3 basis_x = transform[0].xyz;
                const float3 basis_y = transform[1].xyz;
                const float3 basis_z = transform[2].xyz;
                const float3 extent{length(basis_x), length(basis_y), length(basis_z)};

                // A near-zero row carries no direction: substitute a basis axis,
                // saturate that scale component to 0 (never inf/NaN) and leave the
                // rotation around the collapsed axis undefined.
                const mask32x4 collapsed = float4(extent.xyz, 0) <= epsilon_v<float>;
                float4 scale = select(collapsed, float4(0), float4(extent, 0));
                const float4 safe_scale = rcp(select(collapsed, float4(1), float4(extent, 1)));
                const u32 collapsed_i = maskToInt(collapsed);
                float3 rotation_x = collapsed_i & 0x1u ? axis_x : basis_x * safe_scale.x;
                const float3 rotation_y = collapsed_i & 0x2u ? axis_y : basis_y * safe_scale.y;
                const float3 rotation_z = collapsed_i & 0x4u ? axis_z : basis_z * safe_scale.z;

                // Fold a mirroring upper-3x3 (det < 0) into a negative X scale plus
                // the matching row flip so the quaternion always sees a proper
                // (det = +1) orthonormal basis. Cold scalar path: mirrors are rare,
                // so folding stays scalar and gated by [[unlikely]].
                if (dot(cross(rotation_x, rotation_y), rotation_z) < 0.0f) [[unlikely]] {
                    scale.x = -scale.x;
                    rotation_x = -rotation_x;
                }

                return Transform{
                    .m_rotate = normalize(Quaternion(float3x3(rotation_x, rotation_y, rotation_z))),
                    .m_translate = transform[3].xyz,
                    .m_scale = scale.xyz,
                };
            }

            [[nodiscard]] static Transform blend(const Transform &a, const Transform &b, const float alpha) noexcept {
                if (alpha < epsilon_v<float>) [[unlikely]] {
                    return a;
                }
                if (alpha > 1.0f - epsilon_v<float>) [[unlikely]] {
                    return b;
                }

                // Flip the antipodal partner onto the shortest arc; mango lerp is
                // component-wise and unnormalized, so restore unit length after.
                const Quaternion target = dot(a.m_rotate, b.m_rotate) < 0.0f ? -b.m_rotate : b.m_rotate;

                return Transform{
                    .m_rotate = normalize(lerp(a.m_rotate, target, alpha)),
                    .m_translate = lerp(a.m_translate, b.m_translate, alpha),
                    .m_scale = lerp(a.m_scale, b.m_scale, alpha),
                };
            }

            // Scalar quaternion composition is deliberate: tens of flops per node are
            // cheaper than materializing two matrices per compose.
            [[nodiscard]] static Transform multiply(const Transform &a, const Transform &b) noexcept {
                if (a.getDeterminantSign() < 0 or b.getDeterminantSign() < 0) [[unlikely]] {
                    return fromMatrix(a.toMatrix() * b.toMatrix());
                }

                Transform result;
                result.m_scale = b.m_scale * a.m_scale;
                result.m_rotate = b.m_rotate * a.m_rotate;
                result.m_translate = quaternionTransform(b.m_rotate, b.m_scale * a.m_translate) + b.m_translate;
                return result;
            }

            [[nodiscard]] static Transform relativeTransform(const Transform &from, const Transform &to) noexcept {
                if (from.getDeterminantSign() < 0 or to.getDeterminantSign() < 0) [[unlikely]] {
                    return fromMatrix(from.toMatrix() * inverse(to.toMatrix()));
                }

                // to.m_rotate stays unit, so conjugate() is its inverse; hoisted once here.
                const Quaternion inv_rotate = conjugate(to.m_rotate);
                const float3 inv_scale = rcp(to.m_scale);

                Transform result;
                result.m_scale = from.m_scale * inv_scale;
                result.m_rotate = inv_rotate * from.m_rotate;
                result.m_translate = quaternionTransform(inv_rotate, from.m_translate - to.m_translate) * inv_scale;
                return result;
            }
        };

        [[nodiscard]] bool isInf(const Transform &transform) noexcept {
            using pP::isInf;
            return isInf(transform.m_rotate) or isInf(transform.m_translate) or isInf(transform.m_scale);
        }

        [[nodiscard]] bool isNan(const Transform &transform) noexcept {
            using pP::isNan;
            return isNan(transform.m_rotate) or isNan(transform.m_translate) or isNan(transform.m_scale);
        }

        [[nodiscard]] Transform inverse(const Transform &transform) noexcept {
            return transform.invert();
        }

        void swap(Transform &a, Transform &b) noexcept {
            using std::swap;
            swap(a.m_rotate, b.m_rotate);
            swap(a.m_translate, b.m_translate);
            swap(a.m_scale, b.m_scale);
        }
    }

    template<typename T>
        requires requires(param_lvref_t<const T> value)
        {
            { isInf(value) } -> std::same_as<bool>;
            { isNan(value) } -> std::same_as<bool>;
        }
    [[nodiscard]] constexpr bool isInfOrNan(param_lvref_t<const T> value) noexcept {
        return isInf(value) or isNan(value);
    }

    template<typename T>
        requires requires(param_lvref_t<const T> value)
        {
            { isInf(value) } -> std::same_as<bool>;
            { isNan(value) } -> std::same_as<bool>;
        }
    [[nodiscard]] constexpr bool isFinite(param_lvref_t<const T> value) noexcept {
        return not isInf(value) and not isNan(value);
    }

    [[nodiscard]] float3 fallbackPerpendicular(const float3 &n) noexcept {
        // smallest-component axis ⇒ cross is never near-parallel
        const float3 a = abs(n);
        const float3 axis = a.x <= a.y
            ? (a.x <= a.z ? math::axis_x : math::axis_z)
            : (a.y <= a.z ? math::axis_y : math::axis_z);
        return safeNormalize(cross(n, axis), math::axis_x);
    }

    [[nodiscard]] float3 orthonormalize(const float3 &n, const float3 &t) noexcept {
        const float3 nn = safeNormalize(n, math::axis_y); // bake N is unit; guard anyway
        return safeNormalize(t - nn * dot(t, nn), fallbackPerpendicular(nn));
    }
}

export namespace mango::math {
    // hashing infrastructure integration
    template<typename T, u32 DimV>
    [[nodiscard]] constexpr pP::hash_t hashValue(const Vector<T, DimV> &value) noexcept {
        return pP::hash::contiguousRange(std::span<const T, DimV>(value.data(), DimV));
    }

    // opaque infrastructure integration
    template<typename T, u32 DimV>
    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const Vector<T, DimV> &value) noexcept {
        // Capture the span by value: it references value.data() (alive for the whole
        // log expression), but the span object itself must outlive the TransformView
        // construction inside the lambda, otherwise the returned TransformView dangles.
        return pP::opaqueValue(std::span<const T, DimV>(value.data(), DimV));
    }
}
