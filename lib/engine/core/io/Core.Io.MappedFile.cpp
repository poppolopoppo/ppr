module;
#include "pP/Macros.h"
module engine.core;

import :assert;
import :containers;
import :hal;
import :io.mapped_file;
import :memory.poison;

import std;

namespace pP {
    MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
        if (this != &other) {
            unmap_();
            m_map = std::exchange(other.m_map, nullptr);
        }
        return *this;
    }

    const char *MappedFile::c_str() const noexcept {
        return m_map != nullptr ? static_cast<const char *>(hal::io::mapData(m_map)) : nullptr;
    }

    std::span<const std::byte> MappedFile::span() const noexcept {
        if (m_map != nullptr) [[likely]] {
            return {
                static_cast<const std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map)
            };
        }
        return default_value_v;
    }

    std::span<std::byte> MappedFile::span() noexcept {
        if (m_map != nullptr) [[likely]] {
            return {
                static_cast<std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map)
            };
        }
        return default_value_v;
    }

    std::size_t MappedFile::size() const noexcept {
        return m_map != nullptr ? hal::io::mapSize(m_map) : 0u;
    }

    void MappedFile::unmap_() noexcept {
        if (m_map != nullptr) {
            hal::io::unmapFile(m_map);
            m_map = nullptr;
        }
    }

    MappedFile::MappedFile(const hal::io::MapHandle map) noexcept
        : m_map(map) {
        if (m_map != nullptr) {
            mem::unpoisonUninitialized(
                static_cast<std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map));
        }
    }
} // namespace pP

namespace pP::io {
    std::expected<MappedFile, std::error_code> mapFile(const std::filesystem::path &path,
                                                       const hal::io::OpenFlags flags) noexcept {
        try {
            return MappedFile(hal::io::mapFile(path, flags));
        } catch (const std::system_error &e) {
            return std::unexpected(e.code());
        } catch (const std::bad_alloc &) {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
    }
} // namespace pP::io
