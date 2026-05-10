module;

#include <cstdint>

#include "pP/Macros.h"

export module engine.core:memory_poison;

import :assert;
import :hal;

import std;

#define PPR_ENABLE_MEMORY_POISONING \
    defined(PPR_ENABLE_SANITIZER_ADDRESS) || PPR_ENABLE_DEBUG

#if defined(PPR_ENABLE_SANITIZER_ADDRESS)
#   if defined(ASAN_POISON_MEMORY_REGION)
#        define PPR_ASAN_POISON_MEMORY(_ADDR, _SIZE) ASAN_POISON_MEMORY_REGION(_ADDR, _SIZE)
#   else
extern "C" void __asan_poison_memory_region(void const volatile *, ::std::size_t) noexcept;

#        define PPR_ASAN_POISON_MEMORY(_ADDR, _SIZE) __asan_poison_memory_region(_ADDR, _SIZE)
#   endif
#   if defined(ASAN_UNPOISON_MEMORY_REGION)
#        define PPR_ASAN_UNPOISON_MEMORY(_ADDR, _SIZE) ASAN_UNPOISON_MEMORY_REGION(_ADDR, _SIZE)
#   else
extern "C" void __asan_unpoison_memory_region(void const volatile *, ::std::size_t) noexcept;

#        define PPR_ASAN_UNPOISON_MEMORY(_ADDR, _SIZE) __asan_unpoison_memory_region(_ADDR, _SIZE)
#   endif
#else
#   define PPR_ASAN_POISON_MEMORY(_ADDR, _SIZE) ((void)(_ADDR), (void)(_SIZE))
#   define PPR_ASAN_UNPOISON_MEMORY(_ADDR, _SIZE) ((void)(_ADDR), (void)(_SIZE))
#endif

export namespace pP::mem {
#if defined(PPR_ENABLE_SANITIZER_ADDRESS)
    inline constexpr bool is_asan_enabled_v = true;
#else
    inline constexpr bool is_asan_enabled_v = false;
#endif

#if PPR_ENABLE_MEMORY_POISONING

    namespace details {
        enum class PoisonPattern : u64 {
            reserved = UINT64_C(0xAAAAAAAAAAAAAAAA), // memory carved out but not yet given to a user
            uninitialized = UINT64_C(0xCCCCCCCCCCCCCCCC), // freshly allocated, never written by caller
            destroyed = UINT64_C(0xDDDDDDDDDDDDDDDD), // freed / destructed — use-after-free bait
        };

        [[nodiscard]] constexpr u64 poisonSeed(
            const PoisonPattern pattern,
            const void *const ptr,
            const std::size_t size_bytes) noexcept {
            if consteval {
                return hash::combine(std::bit_cast<std::uintptr_t>(ptr), static_cast<u64>(pattern) ^ size_bytes);
            } else {
                const thread_local u64 g_pattern_salt_tls{randomNumberGenerator()()};
                return hash::combine(std::bit_cast<std::uintptr_t>(ptr) ^ static_cast<u64>(pattern), g_pattern_salt_tls ^ size_bytes);
            }
        }

        constexpr void poisonFlood(const PoisonPattern pattern, void *const ptr, const std::size_t size_bytes) noexcept {
            if (!ptr || size_bytes == 0) [[unlikely]] return;

            // Initial RNG state: mix the allocation address with a pattern-specific
            // salt so that (a) two patterns on the same region diverge immediately,
            // and (b) two allocations at the same recycled address also diverge.
            auto next_pattern = [rng{poisonSeed(pattern, ptr, size_bytes)}]() mutable constexpr noexcept -> u64 {
                rng = hash::mix(rng);
                return rng;
            };

            auto *const p_start = static_cast<std::byte *>(ptr);
            auto *const p_end = p_start + size_bytes;

            std::byte *const p_start_aligned = alignForward(p_start, alignof_v<u64>);
            for (std::byte *p_bytes = p_start; p_bytes != p_start_aligned; ++p_bytes) {
                const auto lo = static_cast<std::byte>(next_pattern() & 0x0Fu);
                *p_bytes = static_cast<std::byte>(static_cast<u64>(pattern) & 0xF0u) | lo;
            }

            auto *const p_end_aligned = reinterpret_cast<u64 *>(alignBackward(p_end, alignof_v<u64>));
            for (auto *p_words = reinterpret_cast<u64 *>(p_start_aligned); p_words != p_end_aligned; ++p_words) {
                constexpr u64 high_mask_v = 0xF0F0'F0F0'F0F0'F0F0ULL;
                *p_words = (static_cast<u64>(pattern) & high_mask_v) | (next_pattern() & ~high_mask_v);
            }

            for (auto *p_bytes = reinterpret_cast<std::byte *>(p_end_aligned); p_bytes != p_end; ++p_bytes) {
                const auto lo = static_cast<std::byte>(next_pattern() & 0x0Fu);
                *p_bytes = static_cast<std::byte>(static_cast<u64>(pattern) & 0xF0u) | lo;
            }

            PPR_COMPILER_READWRITE_BARRIER(); // avoid dead-store elimination
        }
    }

    void poisonReserved([[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t size_bytes) noexcept {
        // don't flood memory when reserving with ASAN enabled: this is faster and can inspect previous memory content
#if PPR_ENABLE_SANITIZER_ADDRESS
        PPR_ASAN_POISON_MEMORY(ptr, size_bytes);
#else
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::reserved, ptr, size_bytes));
#endif
    }

    void unpoisonUninitialized([[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t size_bytes) noexcept {
        PPR_ASAN_UNPOISON_MEMORY(ptr, size_bytes);
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::uninitialized, ptr, size_bytes));
    }

    void poisonDestroyed([[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t size_bytes) noexcept {
        // don't flood memory when destroying with ASAN enabled: this is faster and can inspect previous memory content
#if PPR_ENABLE_SANITIZER_ADDRESS
        PPR_ASAN_POISON_MEMORY(ptr, size_bytes);
#else
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::destroyed, ptr, size_bytes));
#endif
    }

#else

    PPR_FORCE_INLINE constexpr void poisonReserved(void *const, const std::size_t) noexcept {
    }
    PPR_FORCE_INLINE constexpr void unpoisonUninitialized(void *const, const std::size_t) noexcept {
    }
    PPR_FORCE_INLINE constexpr void poisonDestroyed(void *const, const std::size_t) noexcept {
    }

#endif

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void poisonReserved([[maybe_unused]] T *const ptr, [[maybe_unused]] const std::size_t n = 1u) noexcept {
        poisonReserved(static_cast<void *>(ptr), sizeof(T) * n);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void unpoisonUninitialized([[maybe_unused]] T *const ptr, [[maybe_unused]] const std::size_t n = 1u) noexcept {
        unpoisonUninitialized(static_cast<void *>(ptr), sizeof(T) * n);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void poisonDestroyed([[maybe_unused]] T *const ptr, [[maybe_unused]] const std::size_t n = 1u) noexcept {
        poisonDestroyed(static_cast<void *>(ptr), sizeof(T) * n);
    }
}
