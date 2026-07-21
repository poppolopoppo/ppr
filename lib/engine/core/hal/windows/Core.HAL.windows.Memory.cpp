module;

#include "Core.HAL.windows.include.hpp"

#include <Memoryapi.h>

#pragma comment(lib, "mincore.lib")

module engine.core;

import :assert;
import :hal;
import :memory;
import :memory.poison;

import std;

namespace pP::hal {
    const std::size_t page_size = []() noexcept -> std::size_t {
        SYSTEM_INFO sys_info;
        ::GetSystemInfo(&sys_info);
        return checked_cast<std::size_t>(sys_info.dwPageSize);
    }();

    const std::align_val_t page_granularity = []() noexcept -> std::align_val_t {
        SYSTEM_INFO sys_info;
        ::GetSystemInfo(&sys_info);
        return std::align_val_t{checked_cast<std::size_t>(sys_info.dwAllocationGranularity)};
    }();

    [[nodiscard]] PPR_FORCE_INLINE static constexpr ::DWORD pageProtectionFlags_(const PageProtection protect) noexcept {
        if (protect.read) [[likely]] {
            if (protect.write) [[likely]] {
                return protect.execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            }
            return protect.execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
        }
        if (protect.write) {
            return protect.execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
        }
        return protect.execute ? PAGE_EXECUTE : PAGE_NOACCESS;
    }

#if WINVER < 0x0A00
    [[nodiscard]] static PVOID WINAPI alignedVirtualAllocFallback_(
        [[maybe_unused]] HANDLE process,
        PVOID p_base_address,
        SIZE_T size,
        ULONG allocation_type,
        ULONG protection_flags,
        MEM_EXTENDED_PARAMETER *p_params,
        ULONG num_params) {
        void *p = ::VirtualAlloc(p_base_address, size, allocation_type, protection_flags);

        std::size_t alignment_v = static_cast<std::size_t>(page_granularity);
        for (ULONG i = 0u; i < num_params; ++i) {
            if (p_params[i].Type == MemExtendedParameterAddressRequirements) {
                const auto *const p_address_requirements = static_cast<const MEM_ADDRESS_REQUIREMENTS *>(p_params[i].Pointer);
                if (p_address_requirements->Alignment) {
                    alignment_v = static_cast<std::size_t>(p_address_requirements->Alignment);
                }
            }
        }

        if (alignForward(p, std::align_val_t{alignment_v}) != p) {
            if (std::bit_cast<std::uintptr_t>(p) < 16 * 1024 * 1024)
                ::VirtualAlloc(p, alignment_v - (std::bit_cast<std::uintptr_t>(p) & (alignment_v - 1u)), MEM_RESERVE, PAGE_NOACCESS);

            do {
                p = ::VirtualAlloc(nullptr, size + alignment_v - static_cast<std::size_t>(page_granularity), MEM_RESERVE, PAGE_NOACCESS);
                if (nullptr == p)
                    return nullptr;

                ::VirtualFree(p, 0, MEM_RELEASE);

                p = ::VirtualAlloc(
                    std::bit_cast<void *>(std::bit_cast<std::uintptr_t>(p) + (alignment_v - 1u) & ~(alignment_v - 1u)),
                    size, allocation_type, protection_flags);
            } while (nullptr == p);
        }

        PPR_ASSERT(alignForward(p, std::align_val_t{alignment_v}) == p);
        return p;
    }
#endif

    [[nodiscard]] PPR_FORCE_INLINE static void *alignedVirtualAlloc_(
        const std::size_t size,
        const std::align_val_t alignment,
        const ::DWORD allocation_type,
        const ::DWORD protection_flags) {
        using virtual_alloc_2_f = PVOID (WINAPI*)(
            HANDLE, PVOID, SIZE_T, ULONG, ULONG,
            MEM_EXTENDED_PARAMETER *, ULONG);
#if WINVER >= 0x0A00
        static constexpr virtual_alloc_2_f g_virtual_alloc_2 = &::VirtualAlloc2;
#else
        static const auto g_virtual_alloc_2 = []() noexcept -> virtual_alloc_2_f {
            if (const ::HMODULE h_kernel = ::GetModuleHandleW(L"KernelBase.dll")) [[likely]] {
                const auto fn_virtual_alloc_2 = reinterpret_cast<virtual_alloc_2_f>(
                    ::GetProcAddress(h_kernel, "VirtualAlloc2"));

                if (fn_virtual_alloc_2) {
                    return fn_virtual_alloc_2;
                }
            }
            return &alignedVirtualAllocFallback_;
        }();
#endif

        ::MEM_ADDRESS_REQUIREMENTS address_requirements{};
        address_requirements.Alignment = static_cast<std::size_t>(alignment);

        ::MEM_EXTENDED_PARAMETER params{
            .Type = MemExtendedParameterAddressRequirements,
            .Pointer = &address_requirements
        };

        return g_virtual_alloc_2(
            nullptr,
            nullptr,
            size,
            allocation_type | MEM_64K_PAGES,
            protection_flags,
            &params, 1u);
    }

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        const bool commit,
        const PageProtection allowed,
        std::align_val_t alignment) {
        PPR_ASSERT((static_cast<std::size_t>(alignment) % static_cast<std::size_t>(page_granularity)) == 0u);
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));

        void *const p_result = alignedVirtualAlloc_(
            aligned_size,
            alignment,
            MEM_RESERVE | (commit ? MEM_COMMIT : 0),
            pageProtectionFlags_(allowed));

        if (!p_result) [[unlikely]] {
            throw std::bad_alloc();
        }

        PPR_ASSERT(alignForward(p_result, alignment) == p_result);

        if (commit) {
            mem::unpoisonUninitialized(p_result, aligned_size);
        }

#if PPR_ENABLE_ASSERTIONS
        ::MEMORY_BASIC_INFORMATION info;
        if (PPR_ENSURE(::VirtualQuery(p_result, &info, sizeof(info)))) {
            PPR_ASSERT(info.BaseAddress == p_result && "Allocate memory with an invalid pointer");
            PPR_ASSERT((info.State & (MEM_COMMIT|MEM_RESERVE)) && "Allocate unreserved memory");
            PPR_ASSERT(info.RegionSize == aligned_size && "Allocate with unmatching region size");
        }
#endif

        return std::allocation_result(p_result, aligned_size);
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::VirtualAlloc(
                ptr, size,
                MEM_COMMIT,
                pageProtectionFlags_(allowed)) == nullptr) [[unlikely]] {
            throw std::bad_alloc();
        }

        mem::unpoisonUninitialized(ptr, size);
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        mem::poisonDestroyed(ptr, size);

        if (::VirtualFree(ptr, size, MEM_DECOMMIT) == FALSE) {
            throw std::bad_alloc();
        }
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) {
        ::DWORD old_protect;
        if (::VirtualProtect(ptr, size, pageProtectionFlags_(allowed), &old_protect) == FALSE) {
            throw std::bad_alloc();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept(false) {
        mem::poisonDestroyed(ptr, size);

        if (::OfferVirtualMemory(ptr, size, VmOfferPriorityNormal) != ERROR_SUCCESS) {
            throw std::bad_alloc();
        }
    }

    [[nodiscard]] bool pageReclaimFromOS(void *const ptr, const std::size_t size) noexcept {
        switch (::ReclaimVirtualMemory(ptr, size)) {
            case ERROR_SUCCESS:
            case ERROR_BUSY:
                mem::unpoisonUninitialized(ptr, size);
                return true;
            default:
                return false;
        }
    }

    void pageFree(void *const ptr, [[maybe_unused]] const std::size_t size) {
        mem::poisonDestroyed(ptr, size);

        if (!::VirtualFree(ptr, 0u, MEM_RELEASE)) {
            throw std::bad_alloc();
        }
    }
}
