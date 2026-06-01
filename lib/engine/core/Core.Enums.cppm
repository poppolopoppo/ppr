module;
#include "pP/Macros.h"
export module engine.core:enums;

import :assert;

import std;

export namespace pP {
    template<typename EnumT> requires std::is_enum_v<EnumT>
    [[nodiscard]] constexpr std::underlying_type_t<EnumT> enumOrd(const EnumT value) noexcept {
        return std::underlying_type_t<EnumT>(value);
    }

    // ------------------------------------------------------------------
    // enum flags
    // ------------------------------------------------------------------

    namespace details {
        template<typename EnumT>
        concept TEnumFlags = requires()
        {
            { EnumT::none } -> std::convertible_to<EnumT>;
            { EnumT::all } -> std::convertible_to<EnumT>;
        } && std::is_enum_v<EnumT>;
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr bool any(const EnumT flags) noexcept {
        return enumOrd(flags) != 0;
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr bool all(const EnumT flags) noexcept {
        return enumOrd(flags) == enumOrd(EnumT::all);
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr EnumT operator ~(const EnumT flags) noexcept {
        return EnumT{~enumOrd(flags)};
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr EnumT operator &(const EnumT lhs, const EnumT rhs) noexcept {
        return EnumT{enumOrd(lhs) & enumOrd(rhs)};
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr EnumT operator |(const EnumT lhs, const EnumT rhs) noexcept {
        return EnumT{enumOrd(lhs) | enumOrd(rhs)};
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr EnumT operator ^(const EnumT lhs, const EnumT rhs) noexcept {
        return EnumT{enumOrd(lhs) ^ enumOrd(rhs)};
    }

    template<details::TEnumFlags EnumT>
    constexpr EnumT &operator &=(EnumT &lhs, const EnumT rhs) noexcept {
        return lhs = lhs & rhs;
    }

    template<details::TEnumFlags EnumT>
    constexpr EnumT &operator |=(EnumT &lhs, const EnumT rhs) noexcept {
        return lhs = lhs | rhs;
    }

    template<details::TEnumFlags EnumT>
    constexpr EnumT &operator ^=(EnumT &lhs, const EnumT rhs) noexcept {
        return lhs = lhs ^ rhs;
    }
}
