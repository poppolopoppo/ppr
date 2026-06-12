module;
#include "pP/Macros.h"

module engine.core;
import :memory;
import std;

namespace pP::mem {

// ------------------------------------------------------------------
// general purpose allocator use stl default allocator
// ------------------------------------------------------------------

std::allocation_result<void *>
GPA::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
    void *const ptr = (alignment > max_align_v ? operator new(bytes, alignment, std::nothrow) : operator new(bytes, std::nothrow));
    PPR_ASSERT(!ptr || alignForward(ptr, alignment) == ptr);
    return {ptr, ptr ? bytes : 0u};
}

void GPA::deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
    if (alignment > max_align_v) {
        operator delete(ptr, bytes, alignment);
    } else {
        operator delete(ptr, bytes);
    }
}

// ------------------------------------------------------------------
// os virtual memory allocator
// ------------------------------------------------------------------

std::allocation_result<void *>
OS::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
    const auto [ptr, reserved] = hal::pageAlloc(bytes);
    PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % static_cast<std::size_t>(alignment) == 0);
    return {ptr, reserved};
}

void OS::deallocateRaw(void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) {
    hal::pageFree(ptr, bytes);
}

}
