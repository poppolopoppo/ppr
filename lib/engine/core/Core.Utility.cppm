module;

#include "pP/Macros.h"

export module engine.core:utility;

import :types;
export import :types;
import std;

export namespace pP {

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
    inline constexpr std::size_t bit_count_v = sizeof(std::unwrap_ref_decay_t<T>) * 8;

    // ------------------------------------------------------------------
    // expand a callable over an index sequence to perform compile-time unrolling
    // ------------------------------------------------------------------

    namespace details {
        template<typename T, class F, T... Is>
        constexpr decltype(auto) static_iota_expand(F &&f, std::integer_sequence<T, Is...>)
            noexcept(noexcept(std::declval<F>()(std::integral_constant<T, Is>{}...)))
            requires std::invocable<F, std::integral_constant<T, Is>...> {
            return std::forward<F>(f)(std::integral_constant<T, Is>{}...);
        }
    }

    template<std::size_t N, class F>
    constexpr decltype(auto) static_iota(F &&f)
        noexcept(noexcept(details::static_iota_expand<std::size_t>(
            std::forward<F>(f),
            std::make_integer_sequence<std::size_t, N>{}))) {
        return details::static_iota_expand<std::size_t>(
            std::forward<F>(f),
            std::make_integer_sequence<std::size_t, N>{});
    }

    template<typename T, T N, class F>
    constexpr decltype(auto) static_iota(F &&f)
        noexcept(noexcept(details::static_iota_expand<T>(
            std::forward<F>(f),
            std::make_integer_sequence<T, N>{}))) {
        return details::static_iota_expand<T>(
            std::forward<F>(f),
            std::make_integer_sequence<T, N>{});
    }

}
