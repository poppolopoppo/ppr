module;

#include "pP/Macros.h"

export module engine.core:utility;

import :types;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // trivial math aliases
    // ------------------------------------------------------------------

    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T clamp(T value, T vmin, T vmax) noexcept {
        return std::max(std::min(value, vmax), vmin);
    }

    template<typename T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T saturate(T value) noexcept {
        return std::max(std::min(value, T(1)), T(0));
    }

    // ------------------------------------------------------------------
    // alignment helpers
    // ------------------------------------------------------------------

    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto divideRoundUp(T value, T div) noexcept {
        return (value + div - 1) / div;
    }

    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto alignBackward(T value, T mod) noexcept {
        return (value / mod) * mod;
    }

    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto alignForward(T value, T mod) noexcept {
        return divideRoundUp(value, mod) * mod;
    }

    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto alignForward(T value, std::align_val_t alignment) noexcept {
        return alignForward(value, static_cast<T>(alignment));
    }

    template<typename T>
    [[nodiscard]] T *alignBackward(T *ptr, std::align_val_t alignment) noexcept {
        return std::bit_cast<T *>(alignBackward(std::bit_cast<std::uintptr_t>(ptr), static_cast<std::uintptr_t>(alignment)));
    }

    template<typename T>
    [[nodiscard]] T *alignForward(T *ptr, std::align_val_t alignment) noexcept {
        return std::bit_cast<T *>(alignForward(std::bit_cast<std::uintptr_t>(ptr), static_cast<std::uintptr_t>(alignment)));
    }

    // ------------------------------------------------------------------
    // alignment traits
    // ------------------------------------------------------------------

    template<typename T>
    inline constexpr std::align_val_t alignof_v{alignof(T)};
    inline constexpr std::align_val_t max_align_v = alignof_v<std::max_align_t>;

    // ------------------------------------------------------------------
    // bit count needed to store any type
    // ------------------------------------------------------------------

    template<typename T>
    inline constexpr std::size_t bit_count_v = sizeof(std::unwrap_ref_decay_t<T>) * 8u;

    // ------------------------------------------------------------------
    // general purpose hasFailed() check (ADL target for hasSucceeded)
    // ------------------------------------------------------------------

    template<typename T = void>
    using Expected = std::expected<T, std::error_code>;

    [[nodiscard]] constexpr const std::error_code &make_error_code(const std::error_code &err) noexcept {
        return err;
    }

    // return the first error which actually failed
    [[nodiscard]] constexpr std::error_code make_error_code(const std::initializer_list<std::error_code> ilist) noexcept {
        for (const std::error_code &err : ilist) {
            if (err) [[unlikely]] {
                return err;
            }
        }
        return default_value_v;
    }

    template<typename T>
    [[nodiscard]] constexpr std::error_code make_error_code(const std::expected<T, std::error_code> &expected) noexcept {
        return expected.error_or(default_value_v);
    }

    [[nodiscard]] constexpr bool hasFailed(const bool result) noexcept {
        return not result;
    }

    [[nodiscard]] constexpr bool hasFailed(const std::error_code &err) noexcept {
        return static_cast<bool>(err);
    }

    template<typename T>
    [[nodiscard]] constexpr bool hasFailed(const std::expected<T, std::error_code> &expected) noexcept {
        return not expected.has_value();
    }

    template<typename T>
        requires requires (const T &result)
    {
        { hasFailed(result) } -> std::convertible_to<bool>;
    }
    PPR_FORCE_INLINE [[nodiscard]] constexpr bool hasSucceeded(const T &result) noexcept {
        return not hasFailed(result);
    }

    // ------------------------------------------------------------------
    // expand a callable over an index sequence to perform compile-time unrolling
    // ------------------------------------------------------------------

    namespace details {
        template<typename T, class F, T... Is>
        PPR_FORCE_INLINE constexpr decltype(auto) static_iota_expand(F &&f, std::integer_sequence<T, Is...>)
            noexcept(noexcept(std::declval<F>()(std::integral_constant<T, Is>{}...)))
            requires std::invocable<F, std::integral_constant<T, Is>...> {
            return std::forward<F>(f)(std::integral_constant<T, Is>{}...);
        }
    }

    template<std::size_t N, class F>
    PPR_FORCE_INLINE constexpr decltype(auto) static_iota(F &&f)
        noexcept(noexcept(details::static_iota_expand<std::size_t>(
            std::forward<F>(f),
            std::make_integer_sequence<std::size_t, N>{}))) {
        return details::static_iota_expand<std::size_t>(
            std::forward<F>(f),
            std::make_integer_sequence<std::size_t, N>{});
    }

    template<typename T, T N, class F>
    PPR_FORCE_INLINE constexpr decltype(auto) static_iota(F &&f)
        noexcept(noexcept(details::static_iota_expand<T>(
            std::forward<F>(f),
            std::make_integer_sequence<T, N>{}))) {
        return details::static_iota_expand<T>(
            std::forward<F>(f),
            std::make_integer_sequence<T, N>{});
    }

}
