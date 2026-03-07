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
        template<typename EnumT> requires std::is_enum_v<EnumT>
        [[nodiscard]] constexpr bool is_enum_flags(std::type_identity<EnumT>) noexcept {
            return false;
        }

        template<typename EnumT> requires std::is_enum_v<EnumT>
        inline constexpr bool is_enum_flags_v = is_enum_flags(std::type_identity<EnumT>{}) &&
                                                std::is_same_v<EnumT, std::decay_t<EnumT> >;

        template<typename EnumT>
        concept TEnumFlags = std::is_enum_v<EnumT> && is_enum_flags_v<EnumT>;
    }

    template<details::TEnumFlags EnumT>
    [[nodiscard]] constexpr bool any(const EnumT flags) noexcept {
        return enumOrd(flags) != 0;
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
}