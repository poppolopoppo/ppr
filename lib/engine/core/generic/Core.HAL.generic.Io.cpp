module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct IoHandleData { int dummy{}; };
    struct FileHandleData { int dummy{}; };

    IoHandle init() noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "IoPort not supported on this platform");
    }

    void deinit(const IoHandle) noexcept {}

    FileHandle openFile(const IoHandle, const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "IoPort not supported on this platform");
    }

    void closeFile(const IoHandle, const FileHandle) noexcept {}

    std::size_t submit(const IoHandle, const std::span<SubmitEntry>) noexcept { return 0u; }
    std::size_t poll(const IoHandle, const std::span<CompletionEntry>) noexcept { return 0u; }
    std::size_t wait(const IoHandle, const std::span<CompletionEntry>) noexcept { return 0u; }
    void wake(const IoHandle) noexcept {}
}
