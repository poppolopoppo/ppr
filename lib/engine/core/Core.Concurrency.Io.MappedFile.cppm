module;
#include "pP/Macros.h"
export module engine.core:io_mapped;

import :assert;
import :containers;
import :hal;

import std;

export namespace pP {

    // ------------------------------------------------------------------
    // MappedFile — memory-mapped file (RAII move-only, standalone)
    // ------------------------------------------------------------------

    class MappedFile {
        hal::io::MapHandle m_map{nullptr};

    public:
        MappedFile() noexcept = default;

        MappedFile(MappedFile &&other) noexcept
            : m_map(std::exchange(other.m_map, nullptr)) {
        }

        MappedFile &operator=(MappedFile &&other) noexcept {
            if (this != &other) {
                unmap_();
                m_map = std::exchange(other.m_map, nullptr);
            }
            return *this;
        }

        MappedFile(const MappedFile &) = delete;
        MappedFile &operator=(const MappedFile &) = delete;

        ~MappedFile() noexcept {
            unmap_();
        }

        [[nodiscard]] std::span<const std::byte> span() const noexcept {
            if (m_map == nullptr) {
                return {};
            }
            return std::span(
                static_cast<const std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map));
        }

        [[nodiscard]] std::span<std::byte> span() noexcept {
            if (m_map == nullptr) {
                return {};
            }
            return std::span(
                static_cast<std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map));
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return m_map != nullptr ? hal::io::mapSize(m_map) : 0u;
        }

        [[nodiscard]] bool isValid() const noexcept {
            return m_map != nullptr;
        }

        explicit MappedFile(hal::io::MapHandle map) noexcept
            : m_map(map) {
        }

    private:
        void unmap_() noexcept {
            if (m_map != nullptr) {
                hal::io::unmapFile(m_map);
                m_map = nullptr;
            }
        }
    };

    template<> struct details::relocatable<MappedFile> : std::true_type {};

}

export namespace pP::io {

    [[nodiscard]] MappedFile mapFile(const std::filesystem::path &path,
                                      const hal::io::OpenFlags flags = {}) noexcept(false) {
        return MappedFile(hal::io::mapFile(path, flags));
    }

}
