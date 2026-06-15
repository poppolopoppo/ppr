module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    const std::size_t page_size = 4096u;
    const std::align_val_t page_granularity{4096u};

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        const bool commit,
        const PageProtection allowed,
        std::align_val_t alignment) noexcept(false) {
        (void)size;
        (void)commit;
        (void)allowed;
        (void)alignment;
        throw std::bad_alloc();
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        (void)ptr;
        (void)size;
        (void)allowed;
        throw std::bad_alloc();
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        (void)ptr;
        (void)size;
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) noexcept(false) {
        (void)ptr;
        (void)size;
        (void)allowed;
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept(false) {
        (void)ptr;
        (void)size;
    }

    [[nodiscard]] bool pageReclaimFromOS(const void *const ptr, const std::size_t size) noexcept {
        (void)ptr;
        (void)size;
        return false;
    }

    void pageFree(void *const ptr, const std::size_t size) noexcept(false) {
        (void)ptr;
        (void)size;
    }
}
