module;
#include "pP/Macros.h"

module engine.core;
import :memory.page_pool;
import std;

namespace pP::mem {

// ------------------------------------------------------------------
// OS page pooling allocator
// ------------------------------------------------------------------

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

std::allocation_result<void *>
PagePool::reclaimFullBundle_() {
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

std::allocation_result<void *>
PagePool::allocateRawFallback_() {
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
        hal::pageCommit(free_bucket, m_page_size * BitmapTree::word_bit_count);
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
    hal::pageFree(m_reserved_space, static_cast<std::size_t>(m_tree_infos.m_desired_size) * static_cast<std::size_t>(m_page_size));
}

bool PagePool::owns(const void *const ptr, const std::size_t size) const noexcept {
    return static_cast<const std::byte *>(ptr) >= m_reserved_space &&
           static_cast<const std::byte *>(ptr) + size < m_reserved_space + m_page_size * m_tree_infos.m_desired_size;
}

std::allocation_result<void *>
PagePool::allocateRaw(const std::size_t bytes,
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

}
