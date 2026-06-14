module;
#include "pP/Macros.h"
module engine.core;
import :channel;
import std;

namespace pP {

// ------------------------------------------------------------------
// a channel is a lock-free MPSC circular buffer for fast message passing between threads
// ------------------------------------------------------------------

RawChannel::RawChannel(const std::size_t buffer_size) noexcept
    : m_data{hal::ringBufferAlloc(buffer_size)},
      m_capacity(buffer_size) {
    PPR_ASSERT(std::has_single_bit(m_capacity) && "buffer size must be a power of 2");
    mem::poisonReserved(m_data, m_capacity);
}

RawChannel::RawChannel() noexcept
    : RawChannel(static_cast<std::size_t>(hal::page_granularity)) {
}

RawChannel::RawChannel(const std::size_t num_elements, const std::size_t element_size) noexcept
    : RawChannel(std::bit_ceil(alignForward(
        num_elements * alignSize(element_size), hal::page_granularity))) {
}

RawChannel::~RawChannel() noexcept {
    if (m_data == nullptr) [[unlikely]] {
        return;
    }
    if (not isClosedOrClosing()) {
        [[maybe_unused]] auto _ = close();
    }
    mem::poisonDestroyed(m_data, m_capacity);
    hal::ringBufferFree(m_data, m_capacity);
}

bool RawChannel::isOpened() const noexcept {
    return m_status.load(std::memory_order_acquire) == status_opened;
}

bool RawChannel::isClosed() const noexcept {
    return m_status.load(std::memory_order_acquire) == status_closed;
}

bool RawChannel::isClosedOrClosing() const noexcept {
    return m_status.load(std::memory_order_acquire) != status_opened;
}

std::size_t RawChannel::capacity() const noexcept {
    return m_capacity;
}

TagPtr<ISignal> RawChannel::subscribeEvent(const TagPtr<ISignal> signal) noexcept {
    return m_on_produced.subscribeEvent(signal);
}

void RawChannel::unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept {
    return m_on_produced.unsubscribeEvent(signal, restore);
}

bool RawChannel::pollEvent() noexcept {
    return m_on_produced.pollEvent();
}

void RawChannel::resetEvent() noexcept {
    m_on_produced.resetEvent();
}

auto RawChannel::flush() noexcept -> std::expected<void, EError> {
    auto hdr = producerReserve(sizeof(std::atomic_flag), wait_if_full);
    if (not hdr.has_value()) [[unlikely]] {
        return std::unexpected(hdr.error());
    }

    alignas(hal::cacheline_size_v) std::atomic_flag flush_signal{};

    *std::start_lifetime_as<std::atomic_flag*>(hdr->data()) = &flush_signal;
    hdr->m_header.get().m_flags |= RecordHeader::flag_flush;

    producerSubmit(*hdr);

    flush_signal.wait(false, std::memory_order_acquire);
    return {};
}

auto RawChannel::close() noexcept -> std::expected<void, EError> {
    if (int expected_status = status_opened;
        not m_status.compare_exchange_strong(expected_status, status_closing,
                                              std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return std::unexpected(error_closed);
    }

    auto hdr = producerReserveAssumeNotClosed_(0u, wait_if_full);
    if (not hdr.has_value()) [[unlikely]] {
        m_status.store(status_closed, std::memory_order_release);

        m_commit.notify_one();
        m_on_produced.emitEvent();

        return std::unexpected(hdr.error());
    }

    hdr->m_header.get().m_flags = RecordHeader::flag_close;
    producerSubmit(*hdr);
    return {};
}

auto RawChannel::producerReserve(const std::size_t size_bytes, const EBackPressure policy) noexcept
    -> std::expected<Record, EError> {
    if (isClosedOrClosing()) [[unlikely]] {
        return std::unexpected(error_closed);
    }
    return producerReserveAssumeNotClosed_(size_bytes, policy);
}

auto RawChannel::producerReserveAssumeNotClosed_(const std::size_t size_bytes, const EBackPressure policy) noexcept
    -> std::expected<Record, EError> {
    const std::size_t record_size = alignSize(size_bytes);
    if (not PPR_ENSURE(record_size <= m_capacity)) [[unlikely]] {
        return std::unexpected(error_full);
    }

    std::unique_lock lock(m_producer_mutex);
    while (true) {
        if (const std::size_t used = m_write - m_read.load(std::memory_order_acquire);
            used + record_size > m_capacity) [[unlikely]] {
            switch (policy) {
                case drop_if_full:
                    return std::unexpected(error_full);
                case wait_if_full:
                    m_read.wait(m_read.load(std::memory_order_acquire), std::memory_order_acquire);
                    continue;
                case yield_if_full:
                    lock.unlock();
                    std::this_thread::yield();
                    lock.lock();
                    continue;
            }
        }

        const std::size_t offset = m_write & (m_capacity - 1u);
        void *const p_reserved_block = static_cast<std::byte *>(m_data) + offset;
        m_write += record_size;

        mem::unpoisonUninitialized(p_reserved_block, record_size);

        return Record{
            *new(p_reserved_block) RecordHeader{
                .m_flags = RecordHeader::flag_busy,
                .m_available_size = safe_narrowing{record_size - sizeof(RecordHeader)}
            }
        };
    }
}

void RawChannel::advanceCommit_() noexcept {
    std::size_t commit = m_commit.load(std::memory_order_relaxed);

    while (commit < m_write) {
        const std::size_t offset = commit & (m_capacity - 1u);
        const auto *hdr = std::start_lifetime_as<RecordHeader>(
            static_cast<std::byte *>(m_data) + offset);

        if (hdr->m_flags & RecordHeader::flag_busy) {
            break;
        }

        commit += alignSize(hdr->m_available_size);
    }

    m_commit.store(commit, std::memory_order_release);
    m_commit.notify_one();

    m_on_produced.emitEvent();
}

void RawChannel::producerSubmit(RecordHeader &written) noexcept {
    const std::lock_guard lock(m_producer_mutex);
    written.m_flags &= ~RecordHeader::flag_busy;
    advanceCommit_();
}

void RawChannel::producerDiscard(RecordHeader &written) noexcept {
    const std::lock_guard lock(m_producer_mutex);
    written.m_flags = RecordHeader::flag_discard;
    advanceCommit_();
}

auto RawChannel::consumerAcquire(const EPolling policy) noexcept
    -> std::expected<Record, EError> {
    while (true) {
        const std::size_t read_pos = m_read.load(std::memory_order_relaxed);
        const std::size_t commit_pos = m_commit.load(std::memory_order_acquire);

        if (read_pos == commit_pos) {
            switch (policy) {
                case block_until_available:
                    m_commit.wait(commit_pos, std::memory_order_acquire);
                    continue;
                case peek_without_blocking:
                    return std::unexpected(error_empty);
            }
        }

        const std::size_t offset = read_pos & (m_capacity - 1u);
        auto *const hdr = std::start_lifetime_as<RecordHeader>(
            static_cast<std::byte *>(m_data) + offset);

        const std::uint32_t f = hdr->m_flags;
        PPR_ASSERT(!(f & RecordHeader::flag_busy));

        if (f & (RecordHeader::flag_close | RecordHeader::flag_discard | RecordHeader::flag_flush)) [[unlikely]] {
            PPR_ASSERT(not (f & RecordHeader::flag_close) || hdr->m_available_size == 0u);
            const std::size_t record_size = alignSize(hdr->m_available_size);

            if (f & RecordHeader::flag_flush) {
                PPR_ASSERT(hdr->m_available_size >= sizeof(std::atomic_flag));
                auto *const p_atomic_signal = *std::start_lifetime_as<std::atomic_flag *>(
                    hdr->data());

                p_atomic_signal->test_and_set(std::memory_order_release);
                p_atomic_signal->notify_all();
            }

            mem::poisonDestroyed(static_cast<void *>(hdr), record_size);

            m_read.fetch_add(record_size, std::memory_order_release);
            m_read.notify_all();

            if (f & RecordHeader::flag_close) {
                m_status.store(status_closed, std::memory_order_release);
                return std::unexpected(error_closed);
            }
            continue;
        }

        return Record{*hdr};
    }
}

void RawChannel::consumerRelease(const RecordHeader &read) noexcept {
    const std::size_t record_size = alignSize(read.m_available_size);
    mem::poisonDestroyed(static_cast<void *>(const_cast<RecordHeader *>(&read)), record_size);

    m_read.fetch_add(record_size, std::memory_order_release);
    m_read.notify_all();
}

}
