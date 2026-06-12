module;
#include "pP/Macros.h"

#include <cstdint>

#if PPR_ENABLE_SANITIZER_ADDRESS
#   include <sanitizer/asan_interface.h>
#endif

module engine.core;
import :memory_poison;
import std;

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

#define PPR_ENABLE_MEMORY_POISONING \
    defined(PPR_ENABLE_SANITIZER_ADDRESS) || PPR_ENABLE_DEBUG

#if PPR_ENABLE_MEMORY_POISONING

namespace pP::mem::details {

[[nodiscard]] u64 poisonSeed(const PoisonPattern pattern, const void *const ptr, const std::size_t size_bytes) noexcept {
    if consteval {
        return hash::combine(std::bit_cast<std::uintptr_t>(ptr), static_cast<u64>(pattern) ^ size_bytes);
    } else {
        static const u64 g_process_salt{randomNumberGenerator()()};
        return hash::combine(std::bit_cast<std::uintptr_t>(ptr) ^ static_cast<u64>(pattern), g_process_salt ^ size_bytes);
    }
}

void poisonFlood(const PoisonPattern pattern, const void *const ptr, const std::size_t size_bytes) noexcept {
    if (!ptr || size_bytes == 0) [[unlikely]] {
        return;
    }

    auto next_pattern = [rng{poisonSeed(pattern, ptr, size_bytes)}]() mutable noexcept -> u64 {
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

    PPR_COMPILER_READWRITE_BARRIER();
}

}

#endif

namespace pP::mem {

// ------------------------------------------------------------------
// memory poisoning and ASAN integration to detect memory bugs early
// ------------------------------------------------------------------

void poisonReserved(void *const ptr, const std::size_t size_bytes) noexcept {
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

void unpoisonUninitialized(void *const ptr, const std::size_t size_bytes) noexcept {
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

void poisonDestroyed(void *const ptr, const std::size_t size_bytes) noexcept {
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

void annotateContiguousContainer(
    const void *const beg,
    const void *const end,
    const void *const old_mid,
    const void *const new_mid) noexcept {
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
    const void *const storage_beg,
    const void *const storage_end,
    const void *const old_container_beg,
    const void *const old_container_end,
    const void *const new_container_beg,
    const void *const new_container_end) noexcept {
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

}

#if PPR_ENABLE_SANITIZER_ADDRESS
extern "C" const char* __asan_default_options()
{
    return "abort_on_error=1:print_stats=1";
}
#endif
