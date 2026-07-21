module;
#include "pP/Macros.h"
export module engine.core:io.mapped_file;

import :assert;
import :containers;
import :hal;
import :memory.poison;

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

        MappedFile &operator=(MappedFile &&other) noexcept;

        MappedFile(const MappedFile &) = delete;
        MappedFile &operator=(const MappedFile &) = delete;

        ~MappedFile() noexcept {
            unmap_();
        }

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return m_map != nullptr;
        }

        [[nodiscard]] const char *c_str() const noexcept;

        [[nodiscard]] std::span<const std::byte> span() const noexcept;

        [[nodiscard]] std::span<std::byte> span() noexcept;

        [[nodiscard]] std::size_t size() const noexcept;

        explicit MappedFile(hal::io::MapHandle map) noexcept;

    private:
        void unmap_() noexcept;
    };

    template<> struct details::relocatable<MappedFile> : std::true_type {};

}

export namespace pP::io {

    [[nodiscard]] std::expected<MappedFile, std::error_code>
    mapFile(const std::filesystem::path &path,
            const hal::io::OpenFlags flags = {}) noexcept;

}
