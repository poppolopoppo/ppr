module;

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct MapHandleData {
        void       *m_data{nullptr};
        std::size_t m_size{0};
    };

    MapHandle mapFile(const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        int prot = PROT_READ;
        int oflags = O_RDONLY;

        if (flags.m_bits & OpenFlags::write) {
            prot |= PROT_WRITE;
            oflags = O_RDWR;
        }

        const int fd = ::open(path.c_str(), oflags);
        if (fd < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile open failed");
        }

        PPR_DEFER { ::close(fd); };

        struct ::stat st {};
        if (::fstat(fd, &st) < 0) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile fstat failed");
        }

        if (st.st_size == 0) {
            auto *md = new MapHandleData();
            return static_cast<MapHandle>(md);
        }

        void *const data = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                                  prot, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) [[unlikely]] {
            throw std::system_error(errno, std::generic_category(), "pP::io: mapFile mmap failed");
        }

        auto *md = new MapHandleData();
        md->m_data = data;
        md->m_size = static_cast<std::size_t>(st.st_size);
        return static_cast<MapHandle>(md);
    }

    void unmapFile(const MapHandle map) noexcept {
        auto *data = static_cast<MapHandleData *>(map);
        if (data != nullptr) {
            if (data->m_data != nullptr) {
                ::munmap(data->m_data, data->m_size);
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
