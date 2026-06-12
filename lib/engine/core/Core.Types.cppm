module;

#include "pP/Macros.h"

export module engine.core:types;

import std;

export namespace pP {
    static_assert(__cplusplus >= 202302L, "C++ version too low: 2023 C++ standard is required");

#ifdef PPR_ENABLE_DEBUG
    inline constexpr bool enable_debug = true;
#else
    inline constexpr bool enable_debug = false;
#endif

    // ------------------------------------------------------------------
    // misc. traits
    // ------------------------------------------------------------------

    template<typename... T>
    inline constexpr bool always_false_v = false; // for conditional static asserts

    template<typename T>
    using unwrap_ref_decay_t = std::decay_t<std::unwrap_reference_t<T> >; // this is weird.

    // ------------------------------------------------------------------
    // integer types
    // ------------------------------------------------------------------

    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    // ------------------------------------------------------------------
    // string character types
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        struct string_character : std::bool_constant<
                    std::is_same_v<char, T> ||
                    std::is_same_v<wchar_t, T> ||
                    std::is_same_v<char8_t, T>> {
        };

        template<typename T>
        constexpr bool is_string_character_v = string_character<T>::value;

        // list of allowed string character types
        template<typename T>
        concept TChar = is_string_character_v<T>;
    }

    // ------------------------------------------------------------------
    // impostor types
    // ------------------------------------------------------------------

    struct DefaultValue final {
        // Constrain both conversion and comparison to default-constructible types.
        template<typename T> requires std::is_default_constructible_v<T>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator T() const
            noexcept(std::is_nothrow_default_constructible_v<T>) {
            return T{};
        }

        template<typename T> requires std::is_default_constructible_v<T>
        [[nodiscard]] friend constexpr bool operator==(DefaultValue, T rhs)
            noexcept(std::is_nothrow_default_constructible_v<T> &&
                     noexcept(T{} == rhs)) {
            return T{} == rhs;
        }
    };

    struct ZeroValue final {
        // Constrain both conversion and comparison to int-constructible types.
        template<typename T> requires std::is_constructible_v<T, int>
        [[nodiscard]] constexpr operator T() const
            noexcept(std::is_nothrow_constructible_v<T, int>) {
            return T{0};
        }

        template<typename T> requires std::is_constructible_v<T, int>
        [[nodiscard]] friend constexpr bool operator==(ZeroValue, T rhs)
            noexcept(std::is_nothrow_constructible_v<T, int> &&
                     noexcept(T{0} == rhs)) {
            return T{} == rhs;
        }
    };

    struct UnsignedMax final {
        // Constrain to unsigned integral types only — the ~T(0) trick is
        // undefined behavior on signed types, so reject them at the constraint level.
        template<std::unsigned_integral T>
        [[nodiscard]] constexpr operator T() const noexcept {
            return ~T{0};
        }

        template<std::unsigned_integral T>
        [[nodiscard]] friend constexpr bool operator==(UnsignedMax lhs, T rhs) noexcept {
            return T{lhs} == rhs;
        }

        template<std::unsigned_integral T>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(UnsignedMax lhs, T rhs) noexcept {
            return T{lhs} <=> rhs;
        }
    };

    inline constexpr DefaultValue default_value_v;
    inline constexpr ZeroValue zero_v;
    inline constexpr UnsignedMax none_v;
    inline constexpr UnsignedMax umax_v;
}
