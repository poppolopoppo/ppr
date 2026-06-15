module;
#include "pP/Macros.h"

module engine.core;
import :memory.arena;
import std;

namespace pP::mem {

// ------------------------------------------------------------------
// slab allocator has a single chunk of fixed size, which it does not own
// ------------------------------------------------------------------

std::allocation_result<void *>
Slab::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept(false) {
    std::size_t space = m_capacity - m_offset;
    void *aligned_ptr = m_data + m_offset;

    if (std::align(static_cast<std::size_t>(alignment), bytes, aligned_ptr, space) == nullptr) [[unlikely]] {
        throw std::bad_alloc{};
    }

    const u32 old_offset = m_offset;
    m_offset = checked_cast<u32>(static_cast<std::ptrdiff_t>(bytes) +
                                 static_cast<std::byte *>(aligned_ptr) - m_data);

    annotateContiguousContainer(m_data, m_capacity, old_offset, m_offset);
    return {aligned_ptr, bytes};
}

bool Slab::resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
    PPR_ASSERT(owns(ptr, old_size) && "Trying to resize a pointer outside of the arena");
    if (old_size == new_size) {
        return true;
    }

    const auto byte_ptr = static_cast<std::byte *>(ptr);
    if (byte_ptr + old_size != m_data + m_offset) [[unlikely]] {
        return false;
    }

    const u32 new_offset = checked_cast<u32>(
        static_cast<std::byte *>(byte_ptr) - m_data +
        static_cast<std::ptrdiff_t>(new_size));
    if (new_offset > m_capacity) [[unlikely]] {
        return false;
    }

    annotateContiguousContainer(m_data, m_capacity, m_offset, new_offset);
    m_offset = new_offset;
    return true;
}

bool Slab::deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) noexcept {
    PPR_ASSERT(owns(ptr, bytes) && "Trying to deallocate a pointer outside of the arena");

    if (const std::byte *byte_ptr = static_cast<std::byte *>(ptr);
        byte_ptr + bytes == m_data + m_offset) [[likely]] {
        const u32 old_offset = m_offset;
        m_offset = checked_cast<u32>(byte_ptr - m_data);

        annotateContiguousContainer(m_data, m_capacity, old_offset, m_offset);
        return true;
    }

    return false;
}

void Slab::reset() noexcept {
    if (m_data != nullptr) [[likely]] {
        annotateContiguousContainer(m_data, m_capacity, m_offset, 0u);
        m_offset = 0u;
    }
}

void Slab::restore(const void *const mark) noexcept {
    if (mark == nullptr) [[likely]] {
        reset();
    }

    PPR_ASSERT(overlap(m_data, m_capacity, mark));
    const u32 prev_offset = m_offset;
    m_offset = static_cast<u32>(static_cast<const std::byte *>(mark) - m_data);

    annotateContiguousContainer(m_data, m_capacity, prev_offset, m_offset);
}

// ------------------------------------------------------------------
// default Arena for for small transient allocations (ex: log formatting, serialization, etc)
// ------------------------------------------------------------------

Arena<SmallPage> &ScratchPad::getArenaTLS_() noexcept {
    alignas(hal::cacheline_size_v) thread_local Arena<SmallPage> g_instance_tls{};
    return g_instance_tls;
}

}

template class pP::mem::Arena<pP::mem::HugePage>;
template class pP::mem::Arena<pP::mem::SmallPage>;
