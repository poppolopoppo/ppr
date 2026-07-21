module;

#include "pP/Macros.h"
#include "mango/math/math.hpp"

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
        return [&value]() noexcept -> pP::opaque::TransformView {
            return pP::opaque::TransformView(std::span<const T, DimV>(value.data(), DimV));
        };
    }
}

