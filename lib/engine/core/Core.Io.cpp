module;
#include "pP/Macros.h"
module engine.core;

import :assert;
import :concurrency.event;
import :hal;
import :io;

import std;

namespace pP {

namespace {
    constexpr std::size_t kMaxCompletionBatch = 64;
}

// ------------------------------------------------------------------
// IoFile
// ------------------------------------------------------------------

IoFile &IoFile::operator=(IoFile &&other) noexcept {
    if (this != &other) {
        close_();
        m_port_handle = std::exchange(other.m_port_handle, nullptr);
        m_file = std::exchange(other.m_file, nullptr);
    }
    return *this;
}

void IoFile::close_() noexcept {
    if (m_file != nullptr) {
        hal::io::closeFile(m_port_handle, m_file);
        m_file = nullptr;
        m_port_handle = nullptr;
    }
}

// ------------------------------------------------------------------
// IoRequest
// ------------------------------------------------------------------

void IoRequest::complete_(const u64 bytes, const std::error_code ec) noexcept {
    u8 expected = 1u;
    if (m_state.compare_exchange_strong(expected, 2u, std::memory_order_acq_rel)) {
        m_bytes = bytes;
        m_error = ec;
        m_completed.emitEvent();
    }
}

IoRequest::~IoRequest() noexcept {
    if (isPending()) {
        (void)cancel();
    }
    PPR_VERIFY(not isPending());
}

// ------------------------------------------------------------------
// IoPort
// ------------------------------------------------------------------

IoPort::IoPort()
    : m_handle(hal::io::init()) {
}

IoPort::~IoPort() noexcept {
    hal::io::deinit(m_handle);
}

std::expected<IoFile, std::error_code>
IoPort::open(const std::filesystem::path &path, const hal::io::OpenFlags flags) noexcept {
    try {
        return IoFile(m_handle, hal::io::openFile(m_handle, path, flags));
    } catch (const std::system_error &e) {
        return std::unexpected(e.code());
    } catch (const std::invalid_argument &) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    } catch (const std::bad_alloc &) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

void IoPort::read(IoRequest &req, const IoFile &file,
                   std::span<std::byte> buffer, const u64 file_offset) noexcept {
    PPR_ASSERT(req.m_state.load() == 0u && "IoRequest already in-flight");

    req.m_active_file = file.m_file;

    hal::io::SubmitEntry entry;
    entry.m_file = file.m_file;
    entry.m_buffer = buffer.data();
    entry.m_buffer_size = buffer.size();
    entry.m_file_offset = file_offset;
    entry.m_opcode = hal::io::Opcode::read;
    entry.m_user_data = &req;
    entry.m_overlapped = req.m_overlapped_storage;

    req.m_state.store(1u, std::memory_order_release);
    req.m_completed.resetEvent();

    [[maybe_unused]] const std::size_t submitted = hal::io::submit(m_handle, std::span(&entry, 1u));
    PPR_ASSERT(submitted == 1u);
}

void IoPort::write(IoRequest &req, const IoFile &file,
                    std::span<std::byte> buffer, const u64 file_offset) noexcept {
    PPR_ASSERT(req.m_state.load() == 0u && "IoRequest already in-flight");

    req.m_active_file = file.m_file;

    hal::io::SubmitEntry entry;
    entry.m_file = file.m_file;
    entry.m_buffer = buffer.data();
    entry.m_buffer_size = buffer.size();
    entry.m_file_offset = file_offset;
    entry.m_opcode = hal::io::Opcode::write;
    entry.m_user_data = &req;
    entry.m_overlapped = req.m_overlapped_storage;

    req.m_state.store(1u, std::memory_order_release);
    req.m_completed.resetEvent();

    [[maybe_unused]] const std::size_t submitted = hal::io::submit(m_handle, std::span(&entry, 1u));
    PPR_ASSERT(submitted == 1u);
}

std::size_t IoPort::pollCompletions() noexcept {
    std::array<hal::io::CompletionEntry, kMaxCompletionBatch> entries{};
    const std::size_t n = hal::io::poll(m_handle, entries);
    for (std::size_t i = 0u; i < n; ++i) {
        auto &ce = entries[i];
        if (ce.m_user_data != nullptr) [[likely]] {
            auto *req = static_cast<IoRequest *>(ce.m_user_data);
            req->complete_(ce.m_bytes_transferred, ce.m_error);
        }
    }
    return n;
}

std::size_t IoPort::waitForCompletions() noexcept {
    std::array<hal::io::CompletionEntry, kMaxCompletionBatch> entries{};
    const std::size_t n = hal::io::wait(m_handle, entries);
    for (std::size_t i = 0u; i < n; ++i) {
        auto &ce = entries[i];
        if (ce.m_user_data != nullptr) [[likely]] {
            auto *req = static_cast<IoRequest *>(ce.m_user_data);
            req->complete_(ce.m_bytes_transferred, ce.m_error);
        }
    }
    return n;
}

// ------------------------------------------------------------------
// IoRequest
// ------------------------------------------------------------------

bool IoRequest::cancel() noexcept {
    u8 expected = 1u;
    const bool was_inflight = m_state.compare_exchange_strong(expected, 0u, std::memory_order_acq_rel);
    if (was_inflight) {
        if (m_active_file != nullptr) {
            hal::io::cancelIo(m_active_file, m_overlapped_storage);
        }
        m_active_file = nullptr;
        m_completed.resetEvent();
    }
    return was_inflight;
}

} // namespace pP

namespace pP::io {

IoPort createPort() {
    return IoPort();
}

} // namespace pP::io
