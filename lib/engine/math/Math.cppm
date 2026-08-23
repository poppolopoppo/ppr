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

    using mango::math::Quaternion;

// PPR's lookAt(eye, target, up) follows the standard (non-mirrored) convention: the camera is
// placed at `eye` looking toward `target`, with +X to the right and -Z forward in view space.
// It is built directly from the basis vectors rather than mango::Matrix4x4::lookat, whose
// (target, viewer) argument order and mirrored basis make the standard matrix awkward to
// express. mango's Matrix4x4 is row-major (m[0..3] are rows); the layout below matches it.
    [[nodiscard]] float4x4 lookAt(const float3 &eye, const float3 &target, const float3 &up) noexcept {
        const float3 zaxis = normalize(eye - target);
        const float3 xaxis = normalize(cross(up, zaxis));
        const float3 yaxis = cross(zaxis, xaxis);
        return float4x4{
            float4{xaxis.x, yaxis.x, zaxis.x, 0.0f},
            float4{xaxis.y, yaxis.y, zaxis.y, 0.0f},
            float4{xaxis.z, yaxis.z, zaxis.z, 0.0f},
            float4{-dot(xaxis, eye), -dot(yaxis, eye), -dot(zaxis, eye), 1.0f},
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

    [[nodiscard]] float4x4 makeLookAtMatrix(const float3 &eye, const float3 &target, const float3 &up) noexcept {
        return lookAt(eye, target, up);
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

