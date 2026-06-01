module;

#include "pP/Macros.h"

#include <cstdint>

#if PPR_ENABLE_SANITIZER_ADDRESS
#   include <sanitizer/asan_interface.h>
#endif

export module engine.core:memory_poison;

import :assert;
import :hal;

import std;

#define PPR_ENABLE_MEMORY_POISONING \
    defined(PPR_ENABLE_SANITIZER_ADDRESS) || PPR_ENABLE_DEBUG

#if defined(PPR_ENABLE_SANITIZER_ADDRESS)
#   include <sanitizer/common_interface_defs.h>
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

    // ------------------------------------------------------------------
    // memory poisoning and ASAN integration to detect memory bugs early
    // ------------------------------------------------------------------

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
                static const u64 g_process_salt{randomNumberGenerator()()};
                return hash::combine(std::bit_cast<std::uintptr_t>(ptr) ^ static_cast<u64>(pattern), g_process_salt ^ size_bytes);
            }
        }

        constexpr void poisonFlood(const PoisonPattern pattern, const void *const ptr, const std::size_t size_bytes) noexcept {
            if (!ptr || size_bytes == 0) [[unlikely]] {
                return;
            }

            auto next_pattern = [rng{poisonSeed(pattern, ptr, size_bytes)}]() mutable constexpr noexcept -> u64 {
                rng = hash::mix(rng);
                return rng;
            };

            auto *const p_start = static_cast<std::byte *>(const_cast<void *>(ptr));
            auto *const p_end = p_start + size_bytes;

            std::byte *const p_start_aligned = std::min(alignForward(p_start, alignof_v<u64>), p_end);
            for (std::byte *p_bytes = p_start; p_bytes != p_start_aligned; ++p_bytes) {
                const auto lo = static_cast<std::byte>(next_pattern() & 0x0Fu);
                *p_bytes = static_cast<std::byte>(static_cast<u64>(pattern) & 0xF0u) | lo;
            }

            auto *const p_end_aligned = reinterpret_cast<u64 *>(std::max(alignBackward(p_end, alignof_v<u64>), p_start_aligned));
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
#if PPR_ENABLE_SANITIZER_ADDRESS
        if (ptr && size_bytes > 0u) [[likely]] {
            const auto a = std::bit_cast<std::uintptr_t>(ptr);
            const auto asan_start = a & ~7ULL;
            const auto asan_end = (a + size_bytes + 7) & ~7ULL;
            PPR_ASAN_POISON_MEMORY(std::bit_cast<void *>(asan_start), asan_end - asan_start);
        }
#else
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::reserved, ptr, size_bytes));
#endif
    }

    void unpoisonUninitialized([[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t size_bytes) noexcept {
#if PPR_ENABLE_SANITIZER_ADDRESS
        if (ptr && size_bytes > 0u) [[likely]] {
            const auto a = std::bit_cast<std::uintptr_t>(ptr);
            const auto asan_start = a & ~7ULL;
            const auto asan_end = (a + size_bytes + 7) & ~7ULL;
            PPR_ASAN_UNPOISON_MEMORY(std::bit_cast<void *>(asan_start), asan_end - asan_start);
        }
#else
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::uninitialized, ptr, size_bytes));
#endif
    }

    void poisonDestroyed([[maybe_unused]] void *const ptr, [[maybe_unused]] const std::size_t size_bytes) noexcept {
#if PPR_ENABLE_SANITIZER_ADDRESS
        if (ptr && size_bytes > 0u) [[likely]] {
            const auto a = std::bit_cast<std::uintptr_t>(ptr);
            const auto asan_start = (a + 7) & ~7ULL;
            const auto asan_end = (a + size_bytes) & ~7ULL;
            if (asan_end > asan_start) {
                PPR_ASAN_POISON_MEMORY(std::bit_cast<void *>(asan_start), asan_end - asan_start);
            }
        }
#else
        PPR_EXPR_IF_DEBUG(details::poisonFlood(details::PoisonPattern::destroyed, ptr, size_bytes));
#endif
    }

    // ------------------------------------------------------------------
    // contiguous container annotation helpers
    // ------------------------------------------------------------------

    void annotateContiguousContainer(
        [[maybe_unused]] const void *const beg,
        [[maybe_unused]] const void *const end,
        [[maybe_unused]] const void *const old_mid,
        [[maybe_unused]] const void *const new_mid) noexcept {
#if PPR_ENABLE_SANITIZER_ADDRESS
        __sanitizer_annotate_contiguous_container(beg, end, old_mid, new_mid);
#elif PPR_ENABLE_DEBUG
        if (old_mid == new_mid) [[unlikely]] {
            return;
        }
        auto *const old_bytes = static_cast<const std::byte *>(old_mid);
        auto *const new_bytes = static_cast<const std::byte *>(new_mid);
        if (old_bytes < new_bytes) {
            const auto sz = static_cast<std::size_t>(new_bytes - old_bytes);
            details::poisonFlood(details::PoisonPattern::uninitialized, old_mid, sz);
        } else {
            const auto sz = static_cast<std::size_t>(old_bytes - new_bytes);
            details::poisonFlood(details::PoisonPattern::destroyed, new_mid, sz);
        }
#endif
    }

    void annotateDoubleEndedContiguousContainer(
        [[maybe_unused]] const void *const storage_beg,
        [[maybe_unused]] const void *const storage_end,
        [[maybe_unused]] const void *const old_container_beg,
        [[maybe_unused]] const void *const old_container_end,
        [[maybe_unused]] const void *const new_container_beg,
        [[maybe_unused]] const void *const new_container_end) noexcept {
#if PPR_ENABLE_SANITIZER_ADDRESS
        __sanitizer_annotate_double_ended_contiguous_container(
            storage_beg, storage_end,
            old_container_beg, old_container_end,
            new_container_beg, new_container_end);
#elif PPR_ENABLE_DEBUG
        {
            auto *const old_beg = static_cast<const std::byte *>(old_container_beg);
            auto *const new_beg = static_cast<const std::byte *>(new_container_beg);
            if (new_beg < old_beg) {
                const auto sz = static_cast<std::size_t>(old_beg - new_beg);
                details::poisonFlood(details::PoisonPattern::uninitialized, new_container_beg, sz);
            } else if (new_beg > old_beg) {
                const auto sz = static_cast<std::size_t>(new_beg - old_beg);
                details::poisonFlood(details::PoisonPattern::destroyed, old_container_beg, sz);
            }
        }
        {
            auto *const old_end = static_cast<const std::byte *>(old_container_end);
            auto *const new_end = static_cast<const std::byte *>(new_container_end);
            if (new_end > old_end) {
                const auto sz = static_cast<std::size_t>(new_end - old_end);
                details::poisonFlood(details::PoisonPattern::uninitialized, old_container_end, sz);
            } else if (new_end < old_end) {
                const auto sz = static_cast<std::size_t>(old_end - new_end);
                details::poisonFlood(details::PoisonPattern::destroyed, new_container_end, sz);
            }
        }
#endif
    }

#else

    PPR_FORCE_INLINE constexpr void poisonReserved(void *const, const std::size_t) noexcept {
    }
    PPR_FORCE_INLINE constexpr void unpoisonUninitialized(void *const, const std::size_t) noexcept {
    }
    PPR_FORCE_INLINE constexpr void poisonDestroyed(void *const, const std::size_t) noexcept {
    }
    PPR_FORCE_INLINE constexpr void annotateContiguousContainer(
        const void *, const void *, const void *, const void *) noexcept {
    }
    PPR_FORCE_INLINE constexpr void annotateDoubleEndedContiguousContainer(
        const void *, const void *, const void *, const void *,
        const void *, const void *) noexcept {
    }

#endif

    // ------------------------------------------------------------------
    // type-safe overloads
    // ------------------------------------------------------------------

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void poisonReserved(T *const ptr, const std::size_t n = 1u) noexcept {
        poisonReserved(static_cast<void *>(ptr), sizeof(T) * n);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void unpoisonUninitialized(T *const ptr, const std::size_t n = 1u) noexcept {
        unpoisonUninitialized(static_cast<void *>(ptr), sizeof(T) * n);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void poisonDestroyed(T *const ptr, const std::size_t n = 1u) noexcept {
        poisonDestroyed(static_cast<void *>(ptr), sizeof(T) * n);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void annotateContiguousContainer(
        const T *storage,
        const std::size_t capacity,
        const std::size_t old_live_count,
        const std::size_t new_live_count) noexcept {
        annotateContiguousContainer(
            static_cast<const void *>(storage),
            static_cast<const void *>(storage + capacity),
            static_cast<const void *>(storage + old_live_count),
            static_cast<const void *>(storage + new_live_count));
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void annotateDoubleEndedContiguousContainer(
        const T *storage,
        const std::size_t storage_count,
        const std::size_t old_container_beg_idx,
        const std::size_t old_container_end_idx,
        const std::size_t new_container_beg_idx,
        const std::size_t new_container_end_idx) noexcept {
        annotateDoubleEndedContiguousContainer(
            static_cast<const void *>(storage),
            static_cast<const void *>(storage + storage_count),
            static_cast<const void *>(storage + old_container_beg_idx),
            static_cast<const void *>(storage + old_container_end_idx),
            static_cast<const void *>(storage + new_container_beg_idx),
            static_cast<const void *>(storage + new_container_end_idx));
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void annotateEmptyContiguousContainer(
        const T *storage,
        const std::size_t capacity) noexcept {
        annotateContiguousContainer(storage, capacity, 0, capacity);
        annotateContiguousContainer(storage, capacity, capacity, 0);
    }

    template<typename T> requires (not std::is_void_v<T>)
    PPR_FORCE_INLINE constexpr void annotateEmptyDoubleEndedContiguousContainer(
        const T *storage,
        const std::size_t capacity) noexcept {
        annotateDoubleEndedContiguousContainer(storage, capacity, 0, 0, 0, capacity);
        annotateDoubleEndedContiguousContainer(storage, capacity, 0, capacity, 0, 0);
    }

    // ------------------------------------------------------------------
    // contiguous range overloads
    // ------------------------------------------------------------------

    template<std::ranges::contiguous_range RangeT>
    PPR_FORCE_INLINE constexpr void poisonReserved(RangeT &&values) noexcept {
        using value_type = std::ranges::range_value_t<RangeT>;
        poisonReserved(static_cast<void *>(values.data()), values.size() * sizeof(value_type));
    }

    template<std::ranges::contiguous_range RangeT>
    PPR_FORCE_INLINE constexpr void unpoisonUninitialized(RangeT &&values) noexcept {
        using value_type = std::ranges::range_value_t<RangeT>;
        unpoisonUninitialized(static_cast<void *>(values.data()), values.size() * sizeof(value_type));
    }

    template<std::ranges::contiguous_range RangeT>
    PPR_FORCE_INLINE constexpr void poisonDestroyed(RangeT &&values) noexcept {
        using value_type = std::ranges::range_value_t<RangeT>;
        poisonDestroyed(static_cast<void *>(values.data()), values.size() * sizeof(value_type));
    }

    // ------------------------------------------------------------------
    // poison iterators for non-trivial range operations
    // ------------------------------------------------------------------

    template<std::input_iterator IteratorT>
    struct UnpoisonUninitializedIterator : IteratorT {
        PPR_FORCE_INLINE explicit constexpr UnpoisonUninitializedIterator(const IteratorT &it) noexcept
            : IteratorT(it) {
        }

        PPR_FORCE_INLINE explicit constexpr UnpoisonUninitializedIterator(IteratorT &&it) noexcept
            : IteratorT(std::move(it)) {
        }

        PPR_FORCE_INLINE constexpr decltype(auto) operator*() const noexcept {
            auto &ref = IteratorT::operator*();
            unpoisonUninitialized(std::addressof(ref));
            return ref;
        }

        PPR_FORCE_INLINE constexpr decltype(auto) operator->() const noexcept {
            auto *const ptr = IteratorT::operator->();
            unpoisonUninitialized(ptr);
            return ptr;
        }
    };

    template<std::input_iterator IteratorT>
    UnpoisonUninitializedIterator(IteratorT &&) -> UnpoisonUninitializedIterator<IteratorT>;

    template<std::input_iterator IteratorT>
    UnpoisonUninitializedIterator(const IteratorT &) -> UnpoisonUninitializedIterator<IteratorT>;
}

#if PPR_ENABLE_SANITIZER_ADDRESS
extern "C" const char* __asan_default_options()
{
    return "abort_on_error=1:print_stats=1";
}
#endif