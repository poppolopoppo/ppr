module;

#include "Core.HAL.windows.include.hpp"

#include <Memoryapi.h>

#pragma comment(lib, "mincore.lib")

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    struct Win32LastError {
        ::DWORD m_errno{0};

        Win32LastError() noexcept
            : m_errno(::GetLastError()) {
        }

        explicit Win32LastError(const errno_t errno) noexcept
            : m_errno(errno) {
        }

        [[nodiscard]] std::size_t format(char *const buffer, const std::size_t capacity) const noexcept {
            PPR_ASSERT(buffer && capacity > 0);
            if (!buffer || capacity == 0) [[unlikely]] {
                return 0u;
            }

            ::DWORD len = ::FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, m_errno,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buffer, checked_cast<DWORD>(capacity) - 1, nullptr);

            if (len <= 0) [[unlikely]] {
                constexpr char fallback[] = "unknown win32 error";
                if (std::size(fallback) < capacity) {
                    std::memcpy(buffer, fallback, sizeof(fallback));
                    len = static_cast<::DWORD>(std::size(fallback) - 1);
                }
            }

            PPR_ASSERT(len < capacity);
            buffer[len] = '\0';
            return checked_cast<std::size_t>(len);
        }

        [[nodiscard]] std::string message() const {
            char buffer[512];
            const std::size_t len = format(buffer, std::size(buffer));
            return {buffer, len};
        }
    };

    class [[nodiscard]] Win32Exception : public std::runtime_error {
        Win32LastError m_last_error;

    public:
        Win32Exception() noexcept
            : Win32Exception(Win32LastError{}) {
        }

        explicit Win32Exception(const Win32LastError last_error) noexcept
            : std::runtime_error(last_error.message()),
              m_last_error(last_error) {
        }

        [[nodiscard]] Win32LastError getLastError() const noexcept {
            return m_last_error;
        }
    };

    void *ringBufferAlloc(const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        ::HANDLE section = nullptr;
        void *ringBuffer = nullptr;
        void *placeholder1 = nullptr;
        void *placeholder2 = nullptr;
        void *view1 = nullptr;
        void *view2 = nullptr;

        placeholder1 = static_cast<PCHAR>(::VirtualAlloc2(
            nullptr,
            nullptr,
            2u * buffer_size,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS,
            nullptr, 0
        ));

        if (placeholder1 == nullptr) {
            throw Win32Exception();
        }

        const BOOL result = ::VirtualFree(
            placeholder1,
            buffer_size,
            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER
        );

        if (result == FALSE) {
            throw Win32Exception();
        }

        placeholder2 = static_cast<std::byte *>(placeholder1) + buffer_size;

        section = ::CreateFileMapping(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            safe_narrowing{buffer_size}, nullptr
        );

        if (section == nullptr) {
            throw Win32Exception();
        }

        view1 = ::MapViewOfFile3(
            section,
            nullptr,
            placeholder1,
            0,
            buffer_size,
            MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE,
            nullptr, 0
        );

        if (view1 == nullptr) {
            throw Win32Exception();
        }

        placeholder1 = nullptr;

        view2 = ::MapViewOfFile3(
            section,
            nullptr,
            placeholder2,
            0,
            buffer_size,
            MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE,
            nullptr, 0
        );

        if (view2 == nullptr) {
            throw Win32Exception();
        }

        ringBuffer = view1;

        ::CloseHandle(section);
        return ringBuffer;
    }

    void ringBufferFree(const void *ring_buffer, const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(ring_buffer != nullptr);
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        if (not::UnmapViewOfFile(ring_buffer)) {
            throw Win32Exception();
        }

        if (const auto *secondary_view = static_cast<const std::byte *>(ring_buffer) + buffer_size;
            not::UnmapViewOfFile(secondary_view)) {
            throw Win32Exception();
        }
    }
}
