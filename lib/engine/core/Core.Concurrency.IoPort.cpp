module;
#include "pP/Macros.h"
module engine.core;

import :assert;
import :event;
import :hal;
import :io_port;

import std;

namespace pP {

// ------------------------------------------------------------------
// IoPort
// ------------------------------------------------------------------

IoPort::IoPort()
    : m_handle(hal::io::init()),
      m_poller(&IoPort::pollerLoop_, std::ref(*this)) {
}

IoPort::~IoPort() noexcept {
    m_poller.request_stop();
    hal::io::wake(m_handle);
    m_poller.join();
    hal::io::deinit(m_handle);
}

void IoPort::pollerLoop_(std::stop_token stop, IoPort &self) noexcept {
    while (not stop.stop_requested()) {
        hal::io::CompletionEntry entries[64];
        const std::size_t n = hal::io::wait(self.m_handle, entries);

        for (std::size_t i = 0u; i < n; ++i) {
            auto &ce = entries[i];
            if (ce.m_user_data != nullptr) [[likely]] {
                auto *req = static_cast<IoRequest *>(ce.m_user_data);
                req->complete_(ce.m_bytes_transferred, ce.m_error);
            }
        }
    }
}

IoFile IoPort::open(const std::filesystem::path &path, const hal::io::OpenFlags flags) noexcept(false) {
    return IoFile(m_handle, hal::io::openFile(m_handle, path, flags));
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
                    std::span<const std::byte> buffer, const u64 file_offset) noexcept {
    PPR_ASSERT(req.m_state.load() == 0u && "IoRequest already in-flight");

    req.m_active_file = file.m_file;

    hal::io::SubmitEntry entry;
    entry.m_file = file.m_file;
    entry.m_buffer = const_cast<std::byte *>(buffer.data());
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

MappedFile IoPort::map(const std::filesystem::path &path, const hal::io::OpenFlags flags) noexcept(false) {
    return MappedFile(m_handle, hal::io::mapFile(m_handle, path, flags));
}

// ------------------------------------------------------------------
// IoRequest
// ------------------------------------------------------------------

IoRequest::IoRequest() noexcept = default;

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
