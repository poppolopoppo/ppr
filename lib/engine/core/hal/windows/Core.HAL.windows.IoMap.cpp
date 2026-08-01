module;

#include "Core.HAL.windows.include.hpp"

#include <Memoryapi.h>

#pragma comment(lib, "mincore.lib")

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct MapHandleData {
        ::HANDLE m_mapping{nullptr};
        void    *m_data{nullptr};
        std::size_t m_size{0};
    };

    MapHandle mapFile(const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        ::DWORD desired_access = GENERIC_READ;
        ::DWORD share = FILE_SHARE_READ;
        ::DWORD protection = PAGE_READONLY;
        ::DWORD map_access = FILE_MAP_READ;

        if (flags.m_bits & OpenFlags::write) {
            desired_access = GENERIC_READ | GENERIC_WRITE;
            protection = PAGE_READWRITE;
            map_access = FILE_MAP_READ | FILE_MAP_WRITE;
            share = FILE_SHARE_READ;
        }

        const ::HANDLE file = ::CreateFileW(
            path.c_str(),
            desired_access,
            share,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        PPR_DEFER { ::CloseHandle(file); };

        ::LARGE_INTEGER file_size{};
        if (not::GetFileSizeEx(file, &file_size)) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        if (file_size.QuadPart == 0) [[unlikely]] {
            auto *md = new MapHandleData();
            md->m_data = nullptr;
            md->m_size = 0;
            return static_cast<MapHandle>(md);
        }

        const ::HANDLE mapping = ::CreateFileMappingW(
            file,
            nullptr,
            protection,
            static_cast<::DWORD>(file_size.QuadPart >> 32u),
            static_cast<::DWORD>(file_size.QuadPart & 0xFFFFFFFFu),
            nullptr);

        if (mapping == nullptr) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        void *const data = ::MapViewOfFile(
            mapping,
            map_access,
            0, 0,
            static_cast<std::size_t>(file_size.QuadPart));

        if (data == nullptr) [[unlikely]] {
            ::CloseHandle(mapping);
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()));
        }

        auto *md = new MapHandleData();
        md->m_mapping = mapping;
        md->m_data = data;
        md->m_size = static_cast<std::size_t>(file_size.QuadPart);
        return static_cast<MapHandle>(md);
    }

    void unmapFile(const MapHandle map) noexcept {
        auto *data = static_cast<MapHandleData *>(map);
        if (data != nullptr) {
            if (data->m_data != nullptr) {
                ::UnmapViewOfFile(data->m_data);
            }
            if (data->m_mapping != nullptr) {
                ::CloseHandle(data->m_mapping);
            }
            delete data;
        }
    }

    void *mapData(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_data : nullptr;
    }

    std::size_t mapSize(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_size : 0u;
    }
}
