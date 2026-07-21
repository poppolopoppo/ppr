module;

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal::io {
    struct IoHandleData {
        int m_ring_fd{-1};
    };

    struct FileHandleData {
        int m_fd{-1};
    };

    IoHandle init() noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: io_uring not yet implemented on Linux");
    }

    void deinit(const IoHandle handle) noexcept {
        delete static_cast<IoHandleData *>(handle);
    }

    FileHandle openFile(const IoHandle, const std::filesystem::path &, const OpenFlags) noexcept(false) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "pP::io: openFile not yet implemented on Linux");
    }

    void closeFile(const IoHandle, const FileHandle file) noexcept {
        delete static_cast<FileHandleData *>(file);
    }

    std::size_t submit(const IoHandle, const std::span<SubmitEntry>) noexcept {
        return 0u;
    }

    std::size_t poll(const IoHandle, const std::span<CompletionEntry>) noexcept {
        return 0u;
    }

    std::size_t wait(const IoHandle, const std::span<CompletionEntry>) noexcept {
        return 0u;
    }

    void wake(const IoHandle) noexcept {
    }
}
