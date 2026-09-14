module;

#include <limits>

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


    /// See also: https://www.agner.org/optimize/calling_conventions.pdf
    template<class T>
    using param_by_value_or_lvalue_reference = std::conditional_t<
        std::is_trivially_copyable_v<T>,
        std::type_identity<T>,
        std::add_lvalue_reference<T> >;

    template<class T>
    using param_lvref_t = param_by_value_or_lvalue_reference<T>::type;

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

    [[nodiscard]] consteval u8 operator ""_u8(const unsigned long long value) { return static_cast<u8>(value); }
    [[nodiscard]] consteval u16 operator ""_u16(const unsigned long long value) { return static_cast<u16>(value); }
    [[nodiscard]] consteval u32 operator ""_u32(const unsigned long long value) { return static_cast<u32>(value); }
    [[nodiscard]] consteval u64 operator ""_u64(const unsigned long long value) { return static_cast<u64>(value); }

    [[nodiscard]] consteval i8 operator ""_i8(const unsigned long long value) { return static_cast<i8>(value); }
    [[nodiscard]] consteval i16 operator ""_i16(const unsigned long long value) { return static_cast<i16>(value); }
    [[nodiscard]] consteval i32 operator ""_i32(const unsigned long long value) { return static_cast<i32>(value); }
    [[nodiscard]] consteval i64 operator ""_i64(const unsigned long long value) { return static_cast<i64>(value); }

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
        template<typename T>
            requires std::is_default_constructible_v<T>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator T() const
            noexcept(std::is_nothrow_default_constructible_v<T>) {
            return T{};
        }

        template<typename T>
            requires std::is_default_constructible_v<T>
        [[nodiscard]] friend constexpr bool operator==(DefaultValue, T rhs)
            noexcept(std::is_nothrow_default_constructible_v<T> &&
                     noexcept(T{} == rhs)) {
            return T{} == rhs;
        }
    };

    struct ZeroValue final {
        // Constrain both conversion and comparison to int-constructible types.
        template<typename IntT>
            requires std::is_constructible_v<IntT, int>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator IntT() const
            noexcept(std::is_nothrow_constructible_v<IntT, int>) {
            return IntT{0};
        }

        // Constrain both conversion and comparison to int-constructible types.
        template<typename T>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator T *() const noexcept {
            return nullptr;
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator std::nullptr_t() const noexcept {
            return nullptr;
        }

        template<typename IntT>
            requires std::is_constructible_v<IntT, int>
        [[nodiscard]] friend constexpr bool operator==(ZeroValue, IntT rhs)
            noexcept(std::is_nothrow_constructible_v<IntT, int> &&
                     noexcept(IntT{0} == rhs)) {
            return IntT{} == rhs;
        }

        template<typename T>
        [[nodiscard]] friend constexpr bool operator==(ZeroValue, T *rhs) noexcept {
            return rhs == nullptr;
        }
    };

    struct MaxValue final {
        template<std::integral IntT>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator IntT() const noexcept {
            return std::numeric_limits<IntT>::max();
        }

        template<std::floating_point FloatT>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator FloatT() const noexcept {
            return std::numeric_limits<FloatT>::max();
        }

        template<typename T> requires std::convertible_to<const MaxValue &, T>
        [[nodiscard]] friend constexpr bool operator==(MaxValue lhs, T rhs) noexcept {
            return T{lhs} == rhs;
        }

        template<typename T> requires std::convertible_to<const MaxValue &, T>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(MaxValue lhs, T rhs) noexcept {
            return T{lhs} <=> rhs;
        }
    };

    struct MinValue final {
        template<std::integral IntT>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator IntT() const noexcept {
            return std::numeric_limits<IntT>::lowest();
        }

        template<std::floating_point FloatT>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator FloatT() const noexcept {
            return std::numeric_limits<FloatT>::lowest();
        }

        template<typename T> requires std::convertible_to<const MinValue &, T>
        [[nodiscard]] friend constexpr bool operator==(MinValue lhs, T rhs) noexcept {
            return T{lhs} == rhs;
        }

        template<typename T> requires std::convertible_to<const MinValue &, T>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(MinValue lhs, T rhs) noexcept {
            return T{lhs} <=> rhs;
        }
    };

    struct Epsilon final {
        template<std::floating_point FloatT>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator FloatT() const noexcept {
            return std::numeric_limits<FloatT>::epsilon();
        }

        template<std::floating_point FloatT>
        [[nodiscard]] friend constexpr bool operator==(Epsilon lhs, FloatT rhs) noexcept {
            return FloatT{lhs} == rhs;
        }

        template<std::floating_point FloatT>
        [[nodiscard]] friend constexpr auto operator<=>(Epsilon lhs, FloatT rhs) noexcept {
            return FloatT{lhs} <=> rhs;
        }
    };

    inline constexpr DefaultValue default_value_v;
    inline constexpr MaxValue none_v;
    inline constexpr MaxValue max_v;
    inline constexpr MinValue min_v;
    inline constexpr ZeroValue zero_v;

    // ------------------------------------------------------------------
    // strongly-typed numeric types
    // ------------------------------------------------------------------

    template<typename T, typename TagT>
        requires std::equality_comparable<T> && std::three_way_comparable<T>
    struct Numeric {
        using tag_type = TagT;

        [[nodiscard]] friend constexpr T defaultValue(std::type_identity<Numeric>) noexcept {
            return default_value_v;
        }

        T m_value{defaultValue(std::type_identity<Numeric>{})};

        constexpr Numeric() noexcept = default;

        explicit constexpr Numeric(param_lvref_t<const T> value) noexcept
            requires std::is_trivially_copyable_v<T>
            : m_value{value} {
        }

        explicit constexpr Numeric(const char (&magick)[sizeof(T) + 1/*'\0'*/]) noexcept
            requires std::is_integral_v<T> and std::is_trivially_copyable_v<T> {
            std::memcpy(&m_value, &magick[0], sizeof(T));
            if constexpr (std::endian::native == std::endian::little) {
                m_value = std::byteswap(m_value); // make it readable in the debugger
            }
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Numeric(const DefaultValue) noexcept
            : m_value{default_value_v} {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Numeric(const ZeroValue) noexcept
            : m_value{zero_v} {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Numeric(const MinValue) noexcept
            : m_value{min_v} {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Numeric(const MaxValue) noexcept
            : m_value{max_v} {
        }

        [[nodiscard]] constexpr T operator*() const noexcept {
            return m_value;
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator T() const noexcept {
            return m_value;
        }

        constexpr Numeric &operator++() noexcept requires std::is_integral_v<T> {
            ++m_value;
            return *this;
        }

        [[nodiscard]] constexpr Numeric operator++(int) noexcept requires std::is_integral_v<T> {
            const auto old_value = *this;
            ++m_value;
            return old_value;
        }

        [[nodiscard]] constexpr bool operator==(const Numeric &other) const {
            return m_value == other.m_value;
        }

        [[nodiscard]] constexpr std::strong_ordering operator<=>(const Numeric &other) const {
            return m_value <=> other.m_value;
        }

        friend constexpr void swap(Numeric &lhs, Numeric &rhs) noexcept {
            using std::swap;
            swap(lhs.m_value, rhs.m_value);
        }
    };

    // ------------------------------------------------------------------
    // traits to extract properties from a function signature (since std::result_of was deprecated)
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        struct FunctionTraits;

        template<typename ReturnT, typename... ArgsT>
        struct FunctionTraits<ReturnT(ArgsT...)> {
            static constexpr bool is_const_v = false;
            static constexpr bool is_noexcept_v = false;

            using return_type = ReturnT;
            using params_type = std::tuple<ArgsT...>;

            template<typename FirstArgT>
            using prefix_with = ReturnT(FirstArgT, ArgsT...);

            template<class ClassT>
            using member_func = ReturnT (ClassT::*)(ArgsT...);

            template<class ClassT>
            using const_member_func = ReturnT (ClassT::*)(ArgsT...) const;
        };

        template<typename ReturnT, typename... ArgsT>
        struct FunctionTraits<ReturnT(ArgsT...) noexcept> {
            static constexpr bool is_const_v = false;
            static constexpr bool is_noexcept_v = true;

            using return_type = ReturnT;
            using params_type = std::tuple<ArgsT...>;

            template<typename FirstArgT>
            using prefix_args_with = ReturnT(FirstArgT, ArgsT...) noexcept;

            template<class ClassT>
            using member_func = ReturnT (ClassT::*)(ArgsT...) noexcept;

            template<class ClassT>
            using const_member_func = ReturnT (ClassT::*)(ArgsT...) noexcept;
        };

        template<typename ReturnT, typename... ArgsT>
        struct FunctionTraits<ReturnT(ArgsT...) const>
                : FunctionTraits<ReturnT(ArgsT...)> {
            static constexpr bool is_const_v = true;
        };

        template<typename ReturnT, typename... ArgsT>
        struct FunctionTraits<ReturnT(ArgsT...) const noexcept>
                : FunctionTraits<ReturnT(ArgsT...) noexcept> {
            static constexpr bool is_const_v = true;
        };

        template<typename T>
        using function_result_t = FunctionTraits<T>::return_type;

        template<typename FunctionT>
        concept TFunction = std::is_function_v<FunctionT>;

        template<typename FunctionT, typename ReturnT>
        concept TFunctionReturning =
                std::conjunction_v<
                    std::is_function<FunctionT>,
                    std::is_same<function_result_t<FunctionT>, ReturnT>
                >;
    }
}
