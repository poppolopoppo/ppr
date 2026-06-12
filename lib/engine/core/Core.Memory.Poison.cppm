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

        [[nodiscard]] u64 poisonSeed(PoisonPattern pattern, const void *ptr, std::size_t size_bytes) noexcept;
        void poisonFlood(PoisonPattern pattern, const void *ptr, std::size_t size_bytes) noexcept;
    }

    void poisonReserved(void *const ptr, const std::size_t size_bytes) noexcept;
    void unpoisonUninitialized(void *const ptr, const std::size_t size_bytes) noexcept;
    void poisonDestroyed(void *const ptr, const std::size_t size_bytes) noexcept;
    void annotateContiguousContainer(const void *const beg, const void *const end, const void *const old_mid, const void *const new_mid) noexcept;
    void annotateDoubleEndedContiguousContainer(const void *const storage_beg, const void *const storage_end, const void *const old_container_beg, const void *const old_container_end, const void *const new_container_beg, const void *const new_container_end) noexcept;

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
