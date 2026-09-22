module;
#include "pP/Macros.h"

export module engine.core:memory.page_pool;

import :assert;
import :containers;
import :hal;

import std;

namespace pP::mem {
    // ------------------------------------------------------------------
    // bit tree used to handle all page allocations and deallocations in O(1)
    // with high cache locality
    // ------------------------------------------------------------------
    class BitmapTree {
        using word_t = std::size_t;
        using mask_t = Bitmask<word_t>;
        using ref_mask_t = BitmaskRef<word_t>;

    public:
        static constexpr u32 word_bit_count = mask_t::bit_count_v;

        struct BuildInfos {
            u32 m_tree_depth{0};
            u32 m_desired_size{0};
            u32 m_leaves_num_words{0};
            u32 m_leaves_first_word{0};

            [[nodiscard]] constexpr u32 getNumTotalWords() const noexcept { return m_leaves_first_word + m_leaves_num_words; }
            [[nodiscard]] constexpr std::size_t getAllocationSize() const noexcept { return getNumTotalWords() * sizeof(word_t); }

            explicit constexpr BuildInfos(u32 desired_size) noexcept;
        };

    private:
        word_t *m_bits{nullptr};

        [[nodiscard]] PPR_FORCE_INLINE constexpr word_t &wordAt_(const u32 at) noexcept {
            PPR_ASSUME(m_bits);
            return m_bits[at];
        }

        [[nodiscard]] PPR_FORCE_INLINE constexpr const word_t &wordAt_(const u32 at) const noexcept {
            PPR_ASSUME(m_bits);
            return m_bits[at];
        }

        [[nodiscard]] constexpr u32 countOnes_(const BuildInfos &infos, u32 up_to) const noexcept {
            PPR_ASSERT(up_to <= infos.m_desired_size && "Count up to size exceeds desired size");
            const word_t *p_word = std::addressof(wordAt_(infos.m_leaves_first_word));

            u32 count = 0;
            for (; up_to >= word_bit_count; up_to -= word_bit_count, ++p_word) {
                count += static_cast<u32>(std::popcount(*p_word));
            }

            if (up_to > 0) {
                count += static_cast<u32>(std::popcount(*p_word << (word_bit_count - up_to)));
            }

            return count;
        }

        constexpr void allocateBitAtDepth_(u32 bit, u32 d, u32 offset) noexcept {
            u32 r = bit % word_bit_count;
            u32 w = offset + bit / word_bit_count;

            ref_mask_t m = {wordAt_(w)};
            PPR_ASSERT(!m.test(r));
            m.set(r);

            if (d > 0 && m.all()) [[unlikely]] {
                do {
                    r = (w - 1) % word_bit_count;
                    w = (w - 1) / word_bit_count;

                    m = {wordAt_(w)};
                    PPR_ASSERT(not m.test(r));
                    m.set(r);

                    if (not m.all()) [[likely]] {
                        break;
                    }

                    d--;
                } while (d);
            }
        }

        PPR_NO_INLINE constexpr void allocateBubbleUpIsFull_(u32 d, u32 offset, ref_mask_t m) noexcept;

        PPR_NO_INLINE constexpr void deallocateBubbleUpWasFull_(u32 d, u32 offset, ref_mask_t m) noexcept {
            do {
                const u32 r = (offset - 1) % word_bit_count;
                offset = (offset - 1) / word_bit_count;

                m = {wordAt_(offset)};
                const bool wasFull = m.all();
                PPR_ASSERT(m.test(r));
                m.reset(r);

                if (not wasFull) [[likely]] {
                    break;
                }

                d--;
            } while (d);
        }

    public:
        // ReSharper disable once CppDFAConstantFunctionResult
        [[nodiscard]] constexpr void *getAllocationPtr() const noexcept {
            return m_bits;
        }

#if PPR_ENABLE_ASSERTIONS
        [[nodiscard]] constexpr bool isEmpty_forAssert(const BuildInfos &infos) const noexcept {
            return countOnes_(infos, infos.m_desired_size) == 0;
        }
#endif
        [[nodiscard]] constexpr bool isFull() const noexcept {
            return wordAt_(0) == mask_t::all_v;
        }

        [[nodiscard]] constexpr std::span<const word_t> leaves(const BuildInfos &infos) const noexcept {
            return std::span(m_bits + infos.m_leaves_first_word, infos.m_leaves_num_words);
        }

        [[nodiscard]] constexpr std::span<const word_t> nodesAtDepth(const BuildInfos &infos, std::size_t depth) const noexcept;

        // must call setupMemoryRequirements() before
        constexpr void initialize(const BuildInfos &infos, std::allocation_result<void *> storage, bool enabled_by_default) noexcept;

        [[nodiscard]] constexpr bool isAllocated(const BuildInfos &infos, u32 bit) const noexcept;

        PPR_FORCE_INLINE constexpr void allocateBit(const BuildInfos &infos, const u32 bit) noexcept {
            return allocateBitAtDepth_(bit, infos.m_tree_depth - 1, infos.m_leaves_first_word);
        }

        [[nodiscard]] constexpr u32 allocate(const BuildInfos &infos, bool &out_was_empty) noexcept;

        struct AllocRange {
            u32 m_first_bit{max_v};
            u32 m_bit_count{0u};
        };

        [[nodiscard]] constexpr AllocRange allocateContiguous(const BuildInfos &infos, u32 requested_count, bool &out_was_empty) noexcept;

        [[maybe_unused]] PPR_FORCE_INLINE constexpr bool deallocate(const BuildInfos &infos, u32 bit) noexcept {
            PPR_ASSERT(bit < infos.m_desired_size);

            const u32 d = infos.m_tree_depth - 1u;
            const u32 r = bit % word_bit_count;
            const u32 offset = infos.m_leaves_first_word + bit / word_bit_count;

            ref_mask_t m = {wordAt_(offset)};
            PPR_ASSERT(m.test(r));

            const bool was_full = m.all();
            m.reset(r);
            const bool is_empty = m.none();

            if (was_full && d > 0u) [[unlikely]] {
                deallocateBubbleUpWasFull_(d, offset, m);
            }

            return is_empty;
        }

        [[nodiscard]] constexpr u32 nextAllocateBit(const BuildInfos &infos) const noexcept;

        [[nodiscard]] constexpr u32 nextAllocateBit(const BuildInfos &infos, u32 after) const noexcept;
    };

    namespace UnitTest {
        export void bit_tree_mechanics();
    }

    // ------------------------------------------------------------------
    // OS page pooling allocator
    // ------------------------------------------------------------------
    export class PagePool {
        static constexpr u32 bundle_max_count = 16u;

        using FullBundle = std::array<u32, bundle_max_count>;

        struct PartialBundle { // NOLINT(*-pro-type-member-init)
            std::array<u32, bundle_max_count - 1u> m_arr;
            u32 m_count{0u};

#if PPR_ENABLE_ASSERTIONS
            ~PartialBundle() noexcept {
                PPR_ASSERT(m_count == 0u);
            }
#endif

            [[nodiscard]] bool isEmpty() const noexcept {
                return m_count == 0u;
            }

            [[nodiscard]] bool isFull() const noexcept {
                return m_count == std::size(m_arr);
            }

            void pushFront(const u32 page_index) noexcept {
                PPR_ASSERT(m_count < std::size(m_arr));
                m_arr[m_count++] = page_index;
            }

            [[nodiscard]] u32 popFront() noexcept {
                PPR_ASSERT(m_count > 0u);
                return m_arr[--m_count];
            }

            [[nodiscard]] auto view(this auto &&self) noexcept {
                return self.m_arr | std::views::take(self.m_count);
            }
        };

        std::mutex m_barrier{};

        // cold storage, never modified and same cache-line than mutex
        std::byte *const m_reserved_space{};
        std::byte *m_highest_committed_space{};
        const std::size_t m_page_size;
        const BitmapTree::BuildInfos m_tree_infos;
        BitmapTree m_committed_pages{};

        [[maybe_unused]]
        const std::byte m_padding_for_alignment[
            hal::cacheline_size_v - (
                sizeof(m_barrier) + sizeof(m_reserved_space) + sizeof(m_highest_committed_space) + sizeof(m_page_size) +
                sizeof(m_tree_infos) + sizeof(m_committed_pages)) % hal::cacheline_size_v]{};

        // hot storage, bundles are voluntarily isolated inside their respective cache lines
        alignas(hal::cacheline_size_v) PartialBundle m_partial_bundle{};
        alignas(hal::cacheline_size_v) FullBundle m_full_bundle{};

        [[nodiscard]] PPR_FORCE_INLINE void *pageAt_(u32 page_index) const noexcept {
            PPR_ASSERT(page_index < m_tree_infos.m_desired_size);
            PPR_ASSUME(m_reserved_space != nullptr);
            // ReSharper disable once CppDFANullDereference
            return m_reserved_space + page_index * m_page_size;
        }

        [[nodiscard]] PPR_FORCE_INLINE u32 pageIndex_(const void *ptr) const noexcept {
            const auto p = std::bit_cast<std::uintptr_t>(ptr);
            const auto r = std::bit_cast<std::uintptr_t>(m_reserved_space);
            PPR_ASSERT(p >= r && p + m_page_size <= r + m_tree_infos.m_desired_size * m_page_size);
            return checked_cast<u32>((p - r) / m_page_size);
        }

        // the full bundle is always sorted
        [[nodiscard]] PPR_FORCE_INLINE bool hasFullBundle_() const noexcept {
            return m_full_bundle[0u] < m_tree_infos.m_desired_size;
        }

        PPR_FORCE_INLINE static constexpr auto runListAssumeSorted_(std::span<const u32> indices) noexcept {
            return indices
               | std::views::chunk_by([](const u32 a, const u32 b) constexpr noexcept { return b == a + 1; })
               | std::views::transform([](auto run) constexpr noexcept {
                   return std::pair{run.front(), static_cast<u32>(std::ranges::distance(run))};
               });
        }

        void decommitFullBundle_();

        [[nodiscard]] std::allocation_result<void *> reclaimFullBundle_();

        [[nodiscard]] std::allocation_result<void *> allocateRawFallback_();

        void deallocateRawFallback_(const void *ptr, std::size_t bytes);

    public:
        PagePool(std::size_t page_size, std::size_t num_reserved_pages);

        ~PagePool();

        [[nodiscard]] std::size_t getCommittedSize() const noexcept {
            PPR_ASSUME(m_highest_committed_space && m_reserved_space);
            // ReSharper disable line CppDFANullDereference
            return safe_narrowing(m_highest_committed_space - m_reserved_space);
        }

        [[nodiscard]] bool owns(const void *ptr, std::size_t size) const noexcept;

        [[nodiscard]] std::allocation_result<void *>
        allocateRaw(std::size_t bytes = hal::page_size,
                    std::align_val_t alignment = std::align_val_t{hal::page_size});

        void deallocateRaw(const void *ptr, std::size_t bytes,
                           std::align_val_t alignment = std::align_val_t{hal::page_size});

        void shrinkToFit();
    };
}
