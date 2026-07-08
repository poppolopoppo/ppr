module;

#include "pP/Macros.h"
#include "mango/math/math.hpp"

export module engine.math;

import engine.core;
import std;

#if 1
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

    using float3x3 = Matrix3x3;
    using float4x4 = Matrix4x4;

    using mango::math::distance;
    using mango::math::dot;
    using mango::math::lerp;
    using mango::math::normalize;
    using mango::math::sqrt;
    using mango::math::rsqrt;

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
        requires requires (T x) { mango::math::dot(x, x); }
    [[nodiscard]] constexpr auto dot2(T x) noexcept {
        return mango::math::dot(x, x);
    }

    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr auto dot2(T x) noexcept {
        return x * x;
    }

    template<typename ToT, typename FromT, u32 DimV>
        requires std::convertible_to<FromT, ToT>
    [[nodiscard]] constexpr Vector<ToT, DimV> vector_cast(const Vector<FromT, DimV> &value) noexcept {
        return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> Vector<ToT, DimV> {
            return Vector<ToT, DimV>(
                static_cast<ToT>(value[idx])...);
        });
    }

    // checked_cast integration
    template<std::integral ToT, std::integral FromT, u32 DimV>
    [[nodiscard]] constexpr Vector<ToT, DimV> checked_cast(const Vector<FromT, DimV> &value) noexcept {
        return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> Vector<ToT, DimV> {
            return Vector<ToT, DimV>(
                checked_cast<ToT>(value[idx])...);
        });
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
        return [&value]() noexcept -> pP::opaque::TransformView {
            return pP::opaque::TransformView(std::span<const T, DimV>(value.data(), DimV));
        };
    }
}
#else
// warning C5304: a declaration designated by the using-declaration 'XXX' exported from this module has internal linkage
// and using such a name outside the module is ill-formed
PPR_PRAGMA_WARNING_DISABLE_MSVC(5304)

export namespace pP {
    using namespace mango::math;

    using float2 = mango::math::float32x2;
    using float3 = mango::math::float32x3;
    using float4 = mango::math::float32x4;

    using float3x3 = mango::math::Matrix3x3;
    using float4x4 = mango::math::Matrix4x4;

    using mango::math::AngleAxis;
    using mango::math::EulerAngles;
    using mango::math::Quaternion;

    using mango::math::Box;
    using mango::math::FastRay;
    using mango::math::Frustum;
    using mango::math::LineSegment;
    using mango::math::Plane;
    using mango::math::Quadratic;
    using mango::math::Ray;
    using mango::math::RayFrustum;
    using mango::math::Rectangle;
    using mango::math::Sphere;
    using mango::math::Triangle;

    using mango::math::Intersect;
    using mango::math::IntersectBarycentric;
    using mango::math::IntersectBarycentricTwosided;
    using mango::math::IntersectRange;
    using mango::math::IntersectSolid;

    [[nodiscard]] inline float4x4 operator*(const float4x4 &a, const float4x4 &b) {
        return mango::math::operator*(a, b);
    }

    [[nodiscard]] inline float4 operator*(const float4 &v, const float4x4 &m) { return mango::math::operator*(v, m); }

    [[nodiscard]] inline float2 operator+(const float2 &a, const float2 &b) { return mango::math::operator+(a, b); }
    [[nodiscard]] inline float2 operator-(const float2 &a, const float2 &b) { return mango::math::operator-(a, b); }
    [[nodiscard]] inline float2 operator*(const float2 &a, const float2 &b) { return mango::math::operator*(a, b); }
    [[nodiscard]] inline float2 operator/(const float2 &a, const float2 &b) { return mango::math::operator/(a, b); }

    [[nodiscard]] inline float3 operator+(const float3 &a, const float3 &b) { return mango::math::operator+(a, b); }
    [[nodiscard]] inline float3 operator-(const float3 &a, const float3 &b) { return mango::math::operator-(a, b); }
    [[nodiscard]] inline float3 operator*(const float3 &a, const float3 &b) { return mango::math::operator*(a, b); }
    [[nodiscard]] inline float3 operator/(const float3 &a, const float3 &b) { return mango::math::operator/(a, b); }

    [[nodiscard]] inline float4 operator+(const float4 &a, const float4 &b) { return mango::math::operator+(a, b); }
    [[nodiscard]] inline float4 operator-(const float4 &a, const float4 &b) { return mango::math::operator-(a, b); }
    [[nodiscard]] inline float4 operator*(const float4 &a, const float4 &b) { return mango::math::operator*(a, b); }
    [[nodiscard]] inline float4 operator/(const float4 &a, const float4 &b) { return mango::math::operator/(a, b); }

    namespace math {
        using std::log;
        using std::max;
        using std::min;
        using std::sqrt;

        using mango::math::lerp;
        using mango::math::intersect;

        template<typename A, typename B>
            requires requires(A a, B b) { mango::math::distance(a, b); }
        [[nodiscard]] inline auto distance(A a, B b) {
            return mango::math::distance(a, b);
        }

        template<typename T>
            requires requires(T x) { mango::math::dot(x, x); }
        [[nodiscard]] inline auto dot(T x, T y) {
            return mango::math::dot(x, y);
        }

        template<typename T>
            requires requires(T x) { mango::math::dot(x, x); }
        [[nodiscard]] inline auto dot2(T x) {
            return mango::math::dot(x, x);
        }

        [[nodiscard]] inline float4x4 translate(const float4x4 &matrix, const float x, const float y, const float z) {
            return mango::math::translate(matrix, x, y, z);
        }

        [[nodiscard]] inline float4x4 scale(const float4x4 &matrix, const float s) {
            return mango::math::scale(matrix, s);
        }

        [[nodiscard]] inline float4x4 scale(const float4x4 &matrix, const float x, const float y, const float z) {
            return mango::math::scale(matrix, x, y, z);
        }

        [[nodiscard]] inline float4x4 rotate(const float4x4 &matrix, const float angle, const float3 &axis) {
            return mango::math::rotate(matrix, angle, axis);
        }

        [[nodiscard]] inline float4x4 rotateX(const float4x4 &matrix, const float angle) {
            return mango::math::rotateX(matrix, angle);
        }

        [[nodiscard]] inline float4x4 rotateY(const float4x4 &matrix, const float angle) {
            return mango::math::rotateY(matrix, angle);
        }

        [[nodiscard]] inline float4x4 rotateZ(const float4x4 &matrix, const float angle) {
            return mango::math::rotateZ(matrix, angle);
        }

        [[nodiscard]] inline float4x4 rotateXYZ(const float4x4 &matrix, const float x, const float y, const float z) {
            return mango::math::rotateXYZ(matrix, x, y, z);
        }

        [[nodiscard]] inline float4x4 mirror(const float4x4 &matrix, const float4 &plane) {
            return mango::math::mirror(matrix, plane);
        }

        [[nodiscard]] inline float4x4 affineInverse(const float4x4 &matrix) {
            return mango::math::affineInverse(matrix);
        }

        [[nodiscard]] inline float4x4 adjoint(const float4x4 &matrix) { return mango::math::adjoint(matrix); }

        [[nodiscard]] inline float4x4 oblique(const float4x4 &matrix, const float4 &nearClip) {
            return mango::math::obliqueD3D(matrix, nearClip);
        }

        template<typename T>
        [[nodiscard]] auto perspectiveD3D(T fovY, T aspect, T nearZ, T farZ) {
            return mango::math::Matrix4x4::perspectiveD3D(fovY * aspect, fovY, nearZ, farZ);
        }

        [[nodiscard]] inline float4x4 inverse(const float4x4 &m) { return mango::math::inverse(m); }

        template<typename T>
        [[nodiscard]] inline auto normalize(const T &v) {
            return mango::math::normalize(v);
        }

        [[nodiscard]] inline float4x4 lookAt(const float3 &viewer, const float3 &target, const float3 &up) {
            return mango::math::Matrix4x4::lookat(target, viewer, up);
        }
    } // namespace math
} // namespace pP
#endif
