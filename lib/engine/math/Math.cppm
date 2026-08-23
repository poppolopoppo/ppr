module;

#include "pP/Macros.h"
#include <mango/math/math.hpp>

export module engine.math;

import engine.core;
import std;

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
    using mango::math::length;
    using mango::math::lerp;
    using mango::math::normalize;
    using mango::math::sqrt;
    using mango::math::rsqrt;
    using mango::math::inverse;
    using mango::math::transpose;
    using mango::math::cross;
    using mango::math::clamp;
    using mango::math::slerp;

    using mango::math::Box;
    using mango::math::Cone;
    using mango::math::Frustum;
    using mango::math::Plane;
    using mango::math::Quadratic;
    using mango::math::FastRay;
    using mango::math::Ray;
    using mango::math::Rectangle;
    using mango::math::Sphere;
    using mango::math::Triangle;

    using mango::math::Quaternion;

    namespace math {
        using mango::math::Intersect;
        using mango::math::IntersectBarycentric;
        using mango::math::IntersectBarycentricTwosided;
        using mango::math::IntersectRange;
        using mango::math::IntersectSolid;
    }

// PPR's lookAt(eye, target, up) follows the standard (non-mirrored) convention: the camera is
// placed at `eye` looking toward `target`, with +X to the right and -Z forward in view space.
// It is built directly from the basis vectors rather than mango::Matrix4x4::lookat, whose
// (target, viewer) argument order and mirrored basis make the standard matrix awkward to
// express. mango's Matrix4x4 is row-major (m[0..3] are rows); the layout below matches it.
    [[nodiscard]] float4x4 makeLookAtMatrix(const float3 &eye, const float3 &target, const float3 &up) noexcept {
        const float3 z_axis = normalize(eye - target);
        const float3 x_axis = normalize(cross(up, z_axis));
        const float3 y_axis = cross(z_axis, x_axis);
        return float4x4{
            float4{x_axis.x, y_axis.x, z_axis.x, 0.0f},
            float4{x_axis.y, y_axis.y, z_axis.y, 0.0f},
            float4{x_axis.z, y_axis.z, z_axis.z, 0.0f},
            float4{-dot(x_axis, eye), -dot(y_axis, eye), -dot(z_axis, eye), 1.0f},
        };
    }

    // Quaternion helpers (mango is row-major; rotateXYZ applies X→Y→Z: pitch→x, yaw→y, roll→z).
    [[nodiscard]] Quaternion makeYawPitchRollQuaternion(float yaw, float pitch, float roll) noexcept {
        return mango::math::Quaternion::rotateXYZ(pitch, yaw, roll);
    }

    [[nodiscard]] Quaternion makeQuaternionFromRotationMatrix(const float3x3 &m) noexcept {
        return Quaternion(m);
    }

    [[nodiscard]] Quaternion makeQuaternionFromRotationMatrix(const float4x4 &m) noexcept {
        return Quaternion(m);
    }

    [[nodiscard]] float3 quaternionTransform(const Quaternion &q, const float3 &v) noexcept {
        return mango::math::operator*(v, q);
    }

    [[nodiscard]] Quaternion conjugate(const Quaternion &q) noexcept {
        return mango::math::conjugate(q);
    }

    [[nodiscard]] float quaternionDot(const Quaternion &a, const Quaternion &b) noexcept {
        return mango::math::dot(a, b);
    }

    [[nodiscard]] Quaternion normalizeQuaternion(const Quaternion &q) noexcept {
        return mango::math::normalize(q);
    }

    // Extracts yaw (around Y) and pitch (around X) from a quaternion built with
    // makeYawPitchRollQuaternion(yaw, pitch, 0) = qy(yaw) * qx(pitch). Round-trips
    // with that construction for pitch within (-pi/2, pi/2).
    [[nodiscard]] float2 quaternionToYawPitch(const Quaternion &q) noexcept {
        const Quaternion n = normalize(q);
        const float sinp = 2.0f * (n.w * n.x - n.y * n.z);
        const float pitch = std::asin(clamp(sinp, -1.0f, 1.0f));
        const float siny_cosp = 2.0f * (n.x * n.z + n.w * n.y);
        const float cosy_cosp = 1.0f - 2.0f * (n.x * n.x + n.y * n.y);
        const float yaw = std::atan2(siny_cosp, cosy_cosp);
        return float2{yaw, pitch};
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
    [[nodiscard]] constexpr auto dot2(T x) noexcept {
        return mango::math::dot(x, x);
    }

    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr auto dot2(T x) noexcept {
        return x * x;
    }

    template<typename T, u32 DimV>
    [[nodiscard]] constexpr bool operator ==(const Vector<T, DimV> &lhs, const Vector<T, DimV> &rhs) noexcept {
        if constexpr (requires(const Vector<T, DimV> &v)
        {
            { mango::simd::compare_eq(v, v) } -> std::same_as<bool>;
        }) {
            return all_of(mango::math::operator==(lhs, rhs));
        } else {
            return pP::static_iota<u32, DimV>([&](auto... idx) constexpr noexcept -> bool {
                return ((lhs[idx] == rhs[idx]) && ...);
            });
        }
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
        // Capture the span by value: it references value.data() (alive for the whole
        // log expression), but the span object itself must outlive the TransformView
        // construction inside the lambda, otherwise the returned TransformView dangles.
        return pP::opaqueValue(std::span<const T, DimV>(value.data(), DimV));
    }
}

