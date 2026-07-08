module;
#include "pP/Macros.h"
#include "rapidhash.h"

export module engine.core:hashing;

import :assert;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // hashing
    // ------------------------------------------------------------------

    struct hash_t {
        std::size_t m_value{0u};

        [[nodiscard]] friend constexpr bool operator==(const hash_t lhs, const hash_t rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const hash_t lhs, const hash_t rhs) noexcept {
            return lhs.m_value <=> rhs.m_value;
        }

        friend constexpr void swap(hash_t &lhs, hash_t &rhs) noexcept {
            std::swap(lhs.m_value, rhs.m_value);
        }

        [[nodiscard]] friend constexpr hash_t hashValue(const hash_t value) noexcept {
            return value; // pass-through
        }
    };

    namespace hash {
        [[nodiscard]] constexpr hash_t combine(const hash_t seed, const hash_t hash_value) noexcept {
            return hash_t{combine(seed.m_value, hash_value.m_value)};
        }

        // duplicated from rapidhash to solve linker issues due to modules
        inline constexpr u64 rapid_secret_v[8] = {
            0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull,
            0xa0761d6478bd642full, 0xe7037ed1a0b428dbull, 0x90ed1765281c388cull, 0xaaaaaaaaaaaaaaaaull
        };

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t memory(const void *const key, const std::size_t size_bytes, const u64 seed) noexcept {
            return hash_t{rapidhash_internal(key, size_bytes, seed, rapid_secret_v)};
        }

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t small(const void *const key, const std::size_t size_bytes, const u64 seed) noexcept {
            return hash_t{rapidhashMicro_internal(key, size_bytes, seed, rapid_secret_v)};
        }

        // Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others
        template<typename T>
            requires std::is_trivially_copyable_v<T>
        [[nodiscard]] RAPIDHASH_ALWAYS_INLINE hash_t trivial(const T *const trivial, const u64 seed) noexcept {
            return hash_t{::rapidhashNano_internal(trivial, sizeof(T), seed, rapid_secret_v)};
        }

        template<typename T, typename ValueT>
        concept THasher = requires(const std::remove_cvref_t<T> &hasher, const ValueT &value)
        {
            { hasher(value) } -> std::same_as<hash_t>;
        };

        inline constexpr u64 default_seed_v = 0xA64E'B204'80BD'0F29ull;

        template<typename T>
        [[nodiscard]] PPR_FLATTEN RAPIDHASH_ALWAYS_INLINE hash_t ptr(const T *const p) noexcept {
            const std::uintptr_t x = std::bit_cast<std::uintptr_t>(p);
            // `x + (x >> 3)` adjustment by Alberto Barbati and Dave Harris.
            return hash_t{mix(x + (x >> 3))};
        }
    }

    template<typename EnumT>
        requires std::is_enum_v<EnumT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const EnumT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    template<std::integral IntegralT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const IntegralT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    template<std::floating_point FloatingPointT>
    [[nodiscard]] PPR_FLATTEN constexpr hash_t hashValue(const FloatingPointT value) noexcept {
        return hash::trivial(&value, hash::default_seed_v);
    }

    namespace hash {
        template<typename T>
        concept THashable = requires(const std::remove_cvref_t<T> &value)
        {
            { hashValue(value) } -> std::same_as<hash_t>;
        };

        template<THashable T>
        struct DefaultHash {
            constexpr hash_t operator()(const T &value) const noexcept {
                return hashValue(value);
            }
        };

        template<THashable HashableValueT>
        [[nodiscard]] constexpr hash_t combine(const hash_t seed, const HashableValueT &value) noexcept {
            return combine(seed, hashValue(value));
        }

        template<std::ranges::sized_range SizedRangeT>
        [[nodiscard]] PPR_FLATTEN constexpr hash_t sizedRange(SizedRangeT &&values) noexcept
            requires THashable<std::ranges::range_value_t<SizedRangeT> > {
            hash_t H = hashValue(std::ranges::size(values));
            for (const auto &value: values) {
                H = hash::combine(H, value);
            }
            return H;
        }

        template<std::ranges::range UnorderedRangeT>
        [[nodiscard]] PPR_FLATTEN constexpr hash_t unorderedRange(UnorderedRangeT &&values) noexcept
            requires THashable<std::ranges::range_value_t<UnorderedRangeT> > {
            hash_t H{default_seed_v};
            for (const auto &value: values) {
                // use an associative hash combine, so the final result is not order-dependant
                H.m_value += hashValue(value).m_value;
            }
            return H;
        }

        template<std::ranges::contiguous_range ContiguousRangeT>
        [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t contiguousRange(ContiguousRangeT &&values) noexcept
            requires std::is_trivially_copyable_v<std::ranges::range_value_t<ContiguousRangeT> > {
            return memory(
                std::ranges::data(values), std::ranges::size(values) * sizeof(std::ranges::range_value_t<ContiguousRangeT>), default_seed_v);
        }

        template<std::ranges::sized_range RangeT>
        [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t anyRange(RangeT &&values) noexcept
            requires hash::THashable<std::ranges::range_value_t<RangeT> > ||
                     std::is_trivially_copyable_v<std::ranges::range_value_t<RangeT> > {
            if constexpr (std::ranges::contiguous_range<RangeT> && std::is_trivially_copyable_v<std::ranges::range_value_t<RangeT> >) {
                return hash::contiguousRange(std::forward<RangeT>(values));
            } else {
                return hash::sizedRange(std::forward<RangeT>(values));
            }
        }

        // ------------------------------------------------------------------
        // constexpr FNV-1a hash for compile-time string hashing
        // ------------------------------------------------------------------

        [[nodiscard]] consteval u32 fnv1a32(const std::string_view str) noexcept {
            u32 hash = 2166136261u;
            for (const auto ch: str) {
                hash ^= static_cast<u8>(ch);
                hash *= 16777619u;
            }
            return hash;
        }

        [[nodiscard]] consteval u64 fnv1a64(const std::string_view str) noexcept {
            u64 hash = 14695981039346656037ull;
            for (const auto ch: str) {
                hash ^= static_cast<u8>(ch);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        [[nodiscard]] consteval auto fnv1a(const std::string_view str) noexcept {
            return PPR_32BIT_OR_64BIT(fnv1a32(str), fnv1a64(str));
        }
    }

    template<std::ranges::contiguous_range ContiguousRangeT>
    [[nodiscard]] PPR_FORCE_INLINE constexpr hash_t hashValue(ContiguousRangeT &&values) noexcept
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<ContiguousRangeT> > {
        return hash::contiguousRange(std::forward<ContiguousRangeT>(values));
    }

    // ------------------------------------------------------------------
    // hash memoizer records the hashValue() to avoid computing it more than once
    // ------------------------------------------------------------------

    namespace hash {
        template<THashable T>
        struct Memoizer {
            T m_value;
            hash_t m_hash{};

            explicit constexpr Memoizer(const T &value) noexcept
                : m_value(value),
                  m_hash(hashValue(value)) {
            }

            explicit constexpr Memoizer(T &&value) noexcept
                : m_value(std::move(value)),
                  m_hash(hashValue(value)) {
            }

            [[nodiscard]] friend constexpr bool operator==(const Memoizer &a, const Memoizer &b) noexcept
                requires std::equality_comparable<T> {
                return a.m_hash == b.m_hash /* short-circuit */ &&
                       a.m_value == b.m_value;
            }

            [[nodiscard]] friend constexpr hash_t hashValue(const Memoizer &memoized) noexcept {
                return memoized.m_hash;
            }
        };

        template<THashable T>
        Memoizer(const T &) -> Memoizer<T>;

        template<THashable T>
        Memoizer(T &&) -> Memoizer<T>;
    }
}
