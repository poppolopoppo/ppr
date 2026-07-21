module;
#include "pP/Macros.h"

module engine.core;
import :memory.page_pool;
import std;

namespace pP::mem {
    // ------------------------------------------------------------------
    // OS page pooling allocator
    // ------------------------------------------------------------------

    void *PagePool::pageAt_(const u32 page_index) const noexcept {
        PPR_ASSERT(page_index < m_tree_infos.m_desired_size);
        PPR_ASSUME(m_reserved_space != nullptr);
        // ReSharper disable once CppDFANullDereference
        return m_reserved_space + page_index * m_page_size;
    }

    u32 PagePool::pageIndex_(const void *const ptr) const noexcept {
        const auto p = std::bit_cast<std::uintptr_t>(ptr);
        const auto r = std::bit_cast<std::uintptr_t>(m_reserved_space);
        PPR_ASSERT(p >= r && p + m_page_size <= r + m_tree_infos.m_desired_size * m_page_size);
        return checked_cast<u32>((p - r) / m_page_size);
    }

    // the full bundle is always sorted
    bool PagePool::hasFullBundle_() const noexcept {
        return m_full_bundle[0u] < m_tree_infos.m_desired_size;
    }

    void PagePool::decommitFullBundle_() {
        for (auto [page_first, page_count]: runListAssumeSorted_(m_full_bundle)) {
            if (page_first < m_tree_infos.m_desired_size) {
                bool can_decommit_pages = false;
                for (u32 i = 0u; i < page_count; i++) {
                    can_decommit_pages |= m_committed_pages.deallocate(m_tree_infos, page_first + i);
                }

                if (can_decommit_pages) [[unlikely]] {
                    void *const free_bucket = pageAt_(alignBackward(page_first, BitmapTree::word_bit_count));
                    hal::pageDecommit(free_bucket, m_page_size * BitmapTree::word_bit_count);
                }
            } else {
                break;
            }
        }

        m_full_bundle.fill(umax_v);
    }

    std::allocation_result<void *> PagePool::reclaimFullBundle_() {
        void *free_page = nullptr;
        for (u32 i = 0u; i < bundle_max_count; i++) {
            const u32 page_index = m_full_bundle[i];
            m_full_bundle[i] = umax_v;

            if (page_index < m_tree_infos.m_desired_size) [[likely]] {
                if (free_page) [[likely]] {
                    m_partial_bundle.pushFront(page_index);
                } else {
                    free_page = pageAt_(page_index);
                }
            } else {
                break;
            }
        }

        return {free_page, m_page_size};
    }

    std::allocation_result<void *> PagePool::allocateRawFallback_() {
        PPR_ASSERT(m_partial_bundle.isEmpty());

        if (hasFullBundle_()) [[unlikely]] {
            return reclaimFullBundle_();
        }

        bool must_commit_pages = false;
        const auto [page_first, page_count] = m_committed_pages.allocateContiguous(m_tree_infos, bundle_max_count, must_commit_pages);
        if (page_first >= m_tree_infos.m_desired_size || page_count == 0u) {
            throw std::bad_alloc{};
        }
        PPR_ASSERT(page_first + page_count <= m_tree_infos.m_desired_size);

        if (must_commit_pages) [[unlikely]] {
            void *const free_bucket = pageAt_(alignBackward(page_first, BitmapTree::word_bit_count));
            const std::size_t bucket_size_bytes{m_page_size * BitmapTree::word_bit_count};

            hal::pageCommit(free_bucket, bucket_size_bytes);

            m_highest_committed_space = std::max(
                m_highest_committed_space,
                static_cast<std::byte *>(free_bucket) + bucket_size_bytes);
        }

        for (u32 i = 1u; i < page_count; i++) {
            m_partial_bundle.pushFront(page_first + i);
        }

        return {pageAt_(page_first), m_page_size};
    }

    void PagePool::deallocateRawFallback_(const void *const ptr, [[maybe_unused]] const std::size_t bytes) {
        PPR_ASSUME(ptr != nullptr);
        PPR_ASSERT(m_partial_bundle.isFull());

        if (hasFullBundle_()) [[unlikely]] {
            decommitFullBundle_();
        }

        const u32 page_index = pageIndex_(ptr);

        PPR_ASSERT(!hasFullBundle_());
        std::ranges::copy(m_partial_bundle.m_arr | std::views::take(m_partial_bundle.m_count), m_full_bundle.begin());
        m_full_bundle[m_partial_bundle.m_count] = page_index;
        m_partial_bundle.m_count = 0u;

        sort::inplaceShell(m_full_bundle);
    }

    PagePool::PagePool(const std::size_t page_size,
                       const std::size_t num_reserved_pages)
        : m_reserved_space(static_cast<std::byte *>(hal::pageAlloc(
              num_reserved_pages * page_size,
              false, {},
              std::max(hal::page_granularity, std::align_val_t{page_size})).ptr)),
          m_highest_committed_space{m_reserved_space},
          m_page_size(page_size),
          m_tree_infos(checked_cast<u32>(num_reserved_pages)) {
        PPR_ASSERT(page_size % hal::page_size == 0u);
        PPR_ASSERT(alignBackward(m_reserved_space, std::align_val_t{hal::page_size}) == m_reserved_space);
        PPR_ASSERT(m_tree_infos.m_desired_size == num_reserved_pages);

        m_full_bundle.fill(umax_v);

        const std::size_t metadata_size_bytes = alignForward(
            m_tree_infos.getAllocationSize(),
            hal::page_granularity);

        m_committed_pages.initialize(
            m_tree_infos,
            hal::pageAlloc(metadata_size_bytes),
            false);
    }

    PagePool::~PagePool() {
        shrinkToFit();

        m_barrier.lock();

        const std::size_t metadata_size_bytes = alignForward(
            m_tree_infos.getAllocationSize(),
            hal::page_granularity);
        hal::pageFree(m_committed_pages.getAllocationPtr(), metadata_size_bytes);

        const std::size_t reserved_size_bytes = static_cast<std::size_t>(m_tree_infos.m_desired_size) * static_cast<std::size_t>(m_page_size);
        const std::size_t committed_size_bytes = m_highest_committed_space - m_reserved_space;
        hal::pageFree(m_reserved_space, std::min(reserved_size_bytes, committed_size_bytes));
    }

    bool PagePool::owns(const void *const ptr, const std::size_t size) const noexcept {
        return static_cast<const std::byte *>(ptr) >= m_reserved_space &&
               static_cast<const std::byte *>(ptr) + size < m_reserved_space + m_page_size * m_tree_infos.m_desired_size;
    }

    std::allocation_result<void *> PagePool::allocateRaw(const std::size_t bytes,
                                                         const std::align_val_t alignment) {
        PPR_ASSERT(bytes > 0u && bytes <= m_page_size && static_cast<std::size_t>(alignment) <= hal::page_size);

        const std::lock_guard scope_guard{m_barrier};

        if (!m_partial_bundle.isEmpty()) [[likely]] {
            const u32 page_index = m_partial_bundle.popFront();
            return {pageAt_(page_index), m_page_size};
        }

        return allocateRawFallback_();
    }

    void PagePool::deallocateRaw(const void *const ptr, const std::size_t bytes,
                                 const std::align_val_t alignment) {
        PPR_ASSERT(bytes > 0u && bytes <= m_page_size && static_cast<std::size_t>(alignment) <= hal::page_size);
        PPR_ASSUME(ptr != nullptr);

        const std::lock_guard scope_guard{m_barrier};

        if (!m_partial_bundle.isFull()) [[likely]] {
            const u32 page_index = pageIndex_(ptr);
            m_partial_bundle.pushFront(page_index);
            return;
        }

        deallocateRawFallback_(ptr, bytes);
    }

    void PagePool::shrinkToFit() {
        const std::lock_guard scope_guard{m_barrier};

        decommitFullBundle_();

        if (!m_partial_bundle.isEmpty()) {
            std::ranges::copy(m_partial_bundle.m_arr | std::views::take(m_partial_bundle.m_count), m_full_bundle.begin());
            m_partial_bundle.m_count = 0u;

            sort::inplaceShell(m_full_bundle);

            decommitFullBundle_();
        }
    }

    // ------------------------------------------------------------------
    // bitmap tree for O(1) alloc and dealloc
    // ------------------------------------------------------------------

    constexpr BitmapTree::BuildInfos::BuildInfos(const u32 desired_size) noexcept {
        PPR_ASSERT(desired_size % word_bit_count == 0u && "non multiple of word stride are not handled properly");

        u32 depth = 1;
        u32 row_size_in_words = 1;
        u32 row_offset_in_words = 0;
        u32 total_capacity = word_bit_count;

        while (total_capacity < desired_size) {
            row_offset_in_words += row_size_in_words;
            row_size_in_words *= word_bit_count;
            total_capacity *= word_bit_count;
            depth++;
        }

        m_tree_depth = depth;
        m_desired_size = desired_size;
        m_leaves_num_words = divideRoundUp(m_desired_size, +word_bit_count); // clamp leaves to desired size
        m_leaves_first_word = row_offset_in_words;
    }

    constexpr u32 BitmapTree::countOnes_(const BuildInfos &infos, u32 up_to) const noexcept {
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

    constexpr void BitmapTree::allocateBitAtDepth_(const u32 bit, u32 d, const u32 offset) noexcept {
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

    constexpr void BitmapTree::allocateBubbleUpIsFull_(u32 d, u32 offset, ref_mask_t m) noexcept {
        do {
            const u32 r = (offset - 1) % word_bit_count;
            offset = (offset - 1) / word_bit_count;

            m = {wordAt_(offset)};
            PPR_ASSERT(not m.all());
            m.set(r);

            if (not m.all()) [[likely]] {
                break;
            }

            d--;
        } while (d);
    }

    constexpr void BitmapTree::deallocateBubbleUpWasFull_(u32 d, u32 offset, ref_mask_t m) noexcept {
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

    constexpr auto BitmapTree::nodesAtDepth(const BuildInfos &infos, const std::size_t depth) const noexcept -> std::span<const word_t> {
        PPR_ASSERT(depth < infos.m_tree_depth);

        u32 offset = infos.m_leaves_first_word;
        u32 count = infos.m_leaves_num_words;
        for (u32 d = infos.m_tree_depth - 1u; d > depth; d--) {
            offset /= word_bit_count;
            count = divideRoundUp(count, word_bit_count);
        }

        return std::span(m_bits + offset, count);
    }

    // must call setupMemoryRequirements() before
    constexpr void BitmapTree::initialize(const BuildInfos &infos, const std::allocation_result<void *> storage, const bool enabled_by_default) noexcept {
        PPR_ASSERT(storage.ptr && storage.count >= infos.getAllocationSize() &&
            "Storage must be properly allocated and sized according to setupMemoryRequirements()");
        m_bits = static_cast<word_t *>(storage.ptr);

        auto bits_span = std::span{static_cast<word_t *>(storage.ptr), infos.getNumTotalWords()};

        // initialize the bits in the array, clamped to desired size
        const word_t default_value_word = enabled_by_default ? mask_t::all_v : 0u;
        std::ranges::fill(bits_span, default_value_word);

        if (!enabled_by_default) {
            u32 width = 1u;
            for (u32 d = 1u; d < infos.m_tree_depth; d++)
                width *= word_bit_count;

            width /= word_bit_count;
            u32 offset = infos.m_leaves_first_word - width;
            u32 granularity = word_bit_count;
            for (int d = static_cast<int>(infos.m_tree_depth) - 2; d >= 0; d--) {
                u32 trueBits = width * word_bit_count - divideRoundUp(infos.m_desired_size, granularity);
                const u32 trueWords = trueBits / word_bit_count;
                trueBits %= word_bit_count;

                std::ranges::fill(bits_span.subspan(offset + width - trueWords, trueWords), default_value_word);

                if (trueBits) {
                    m_bits[offset + width - trueWords - 1] = mask_t::unsetFirstN(word_bit_count - trueBits).m_bits;
                }

                granularity *= word_bit_count;
                width /= word_bit_count;
                offset -= width;
            }

            if (infos.m_desired_size % word_bit_count) {
                m_bits[infos.getNumTotalWords() - 1] = mask_t::unsetFirstN(infos.m_desired_size % word_bit_count).m_bits;
            }
        }
    }

    constexpr bool BitmapTree::isAllocated(const BuildInfos &infos, u32 bit) const noexcept {
        PPR_ASSERT(bit < infos.m_desired_size && "Bit index out of bounds");

        bit += infos.m_leaves_first_word * word_bit_count;
        const u32 w = bit / word_bit_count;
        const u32 r = bit % word_bit_count;

        return mask_t{wordAt_(w)}.test(r);
    }

    constexpr u32 BitmapTree::allocate(const BuildInfos &infos, bool &out_was_empty) noexcept {
        // returns leaf index or UMax if full
        if (isFull()) [[unlikely]] {
            return umax_v;
        }

        u32 d = 0u;
        u32 bit = 0u;
        u32 offset = 0u;
        for (;;) {
            ref_mask_t m = {wordAt_(offset)};
            const u32 jmp = m.countTrailingOnes();
            bit = bit * word_bit_count + jmp;

            if (infos.m_tree_depth - 1u == d) {
                PPR_ASSERT(!m.test(jmp));
                out_was_empty = m.none();
                m.set(jmp);

                if (d > 0u && m.all()) [[unlikely]] {
                    allocateBubbleUpIsFull_(d, offset, m);
                }
                return bit;
            }

            d++;
            offset = offset * word_bit_count + 1u + jmp;
        }
    }

    constexpr auto BitmapTree::allocateContiguous(const BuildInfos &infos, const u32 requested_count, bool &out_was_empty) noexcept -> AllocRange {
        PPR_ASSERT(requested_count > 0u);
        // returns leaf index or UMax if full
        if (isFull()) [[unlikely]] {
            return {};
        }

        u32 d = 0u;
        u32 bit = 0u;
        u32 offset = 0u;
        for (;;) {
            ref_mask_t m = {wordAt_(offset)};
            const u32 jmp = m.countTrailingOnes();
            bit = bit * word_bit_count + jmp;

            if (infos.m_tree_depth - 1u == d) {
                const u32 n_contiguous = std::min(requested_count, std::min((m >> jmp).countTrailingZeros(), mask_t::bit_count_v - jmp));
                PPR_ASSERT(n_contiguous > 0u);
                mask_t alloc_m = mask_t::setFirstN(n_contiguous) << jmp;
                PPR_ASSERT((m & alloc_m).none());

                out_was_empty = m.none();

                alloc_m &= ~m;
                m |= alloc_m;

                if (d > 0u && m.all()) [[unlikely]] {
                    allocateBubbleUpIsFull_(d, offset, m);
                }
                return {.m_first_bit = bit, .m_bit_count = n_contiguous};
            }

            d++;
            offset = offset * word_bit_count + 1u + jmp;
        }
    }

    constexpr bool BitmapTree::deallocate(const BuildInfos &infos, const u32 bit) noexcept {
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

    constexpr u32 BitmapTree::nextAllocateBit(const BuildInfos &infos) const noexcept {
        // returns leaf index or UMax if full
        if (isFull()) [[unlikely]] {
            return umax_v;
        }

        u32 bit = 0u;
        u32 offset = 0u;
        for (u32 d = 0u; d < infos.m_tree_depth; ++d) [[likely]]{
            const mask_t m{wordAt_(offset)};
            const u32 jmp = m.countTrailingOnes();

            bit = bit * word_bit_count + jmp;
            offset = offset * word_bit_count + 1u + jmp;
        }

        return bit;
    }

    constexpr u32 BitmapTree::nextAllocateBit(const BuildInfos &infos, const u32 after) const noexcept {
        // returns leaf index or UMax if full
        if (after >= infos.m_desired_size || isFull()) [[unlikely]] {
            return umax_v;
        }

        u32 d = infos.m_tree_depth - 1u;
        u32 bit = after;
        u32 r = bit % word_bit_count;
        u32 offset = infos.m_leaves_first_word + bit / word_bit_count;

        mask_t m{wordAt_(offset)};
        if (not m.test(r)) {
            return bit; // start was unallocated
        }

        m |= mask_t::setFirstN(r);
        if (not m.all()) [[likely]] {
            const u32 jmp = m.countTrailingOnes();
            return bit - r + jmp;
        }

        // ReSharper disable once CppDFALoopConditionNotUpdated
        while (d) {
            d--;
            r = (offset - 1u) % word_bit_count;
            offset = (offset - 1u) / word_bit_count;

            m = {wordAt_(offset)};
            m |= mask_t::setFirstN(r);
            if (not m.all()) [[likely]] {
                for (;;) {
                    const u32 jmp = m.countTrailingZeros();
                    if (d == infos.m_tree_depth - 1u) {
                        PPR_ASSERT(not m.test(jmp));
                        bit = (offset - infos.m_leaves_first_word) * word_bit_count + jmp;
                        PPR_ASSERT(bit < infos.m_desired_size);
                        return bit;
                    }

                    d++;
                    offset = offset * word_bit_count + 1u + jmp;
                    m = mask_t{wordAt_(offset)};
                }
            }
            std::unreachable();
        }

        return umax_v;
    }

    void UnitTest::bit_tree_mechanics() {
        BitmapTree tree;
        constexpr BitmapTree::BuildInfos infos(128u);

        std::byte storage[1024];
        tree.initialize(infos, {storage, infos.getAllocationSize()}, false);
        PPR_ASSERT(!tree.isFull());

        bool was_empty = false;
        const u32 b1 = tree.allocate(infos, was_empty);

        PPR_ASSERT(b1 == 0u);
        PPR_ASSERT(was_empty);
        PPR_ASSERT(tree.isAllocated(infos, 0u));

        const auto [m_first_bit, m_bit_count] =
            tree.allocateContiguous(infos, 10u, was_empty);
        PPR_ASSERT(m_first_bit == 1u);
        PPR_ASSERT(m_bit_count == 10u);
        PPR_ASSERT(!was_empty);

        tree.deallocate(infos, 0u);
        PPR_ASSERT(!tree.isAllocated(infos, 0u));

        const u32 next = tree.nextAllocateBit(infos, 0u);
        PPR_ASSERT(next == 0u);
    }
}
