module;
#include "pP/Macros.h"
module engine.core;
import :logger;
import std;

namespace pP {

// ------------------------------------------------------------------
// asynchronous logger
// ------------------------------------------------------------------

Log::Policy Log::setWriterPolicy(Policy writer_policy) noexcept {
    return Handler::get().setWriterPolicy(writer_policy);
}

void Log::flush() noexcept {
    Handler::get().flush();
}

void Log::log(const Emitter &emitter, const string_literal message, const opaque::Dict params) noexcept {
    Handler::get().log(emitter, message, params);
}

Log::Handler::Handler() noexcept
    : m_messages(buffer_size_v),
      m_writer_policy{std23::nontype<&Handler::defaultWriter_>, this},
      m_background_worker(&Handler::backgroundWorkerLoop_, std::ref(*this)),
      m_started_at(std::chrono::steady_clock::now()) {
}

Log::Handler::~Handler() noexcept {
    PPR_VERIFY(m_messages.close().has_value());
}

Log::Handler &Log::Handler::get() noexcept {
    static Handler g_instance;
    return g_instance;
}

Log::Policy Log::Handler::setWriterPolicy(Policy writer_policy) noexcept {
    const std::lock_guard scope_lock{m_writer_barrier};
    std::swap(writer_policy, m_writer_policy);
    return writer_policy;
}

void Log::Handler::flush() noexcept {
    PPR_VERIFY(m_messages.flush().has_value());
}

void Log::Handler::log(const Emitter &emitter, const string_literal message, const opaque::Dict params) noexcept {
    const std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();

    std::size_t entry_size = sizeof(Entry);
    entry_size += opaque::Block::sizeOf(params);

    const auto hdr = m_messages.producerReserve(entry_size, RawChannel::wait_if_full);
    PPR_ASSERT(hdr.has_value());

    mem::Slab local_slab{hdr->allocation()};
    new(local_slab) Entry{
        .m_message{message.view()},
        .m_params{params, local_slab},
        .m_site{emitter},
        .m_timestamp{timestamp},
        .m_thread_id{std::this_thread::get_id()},
    };

    m_messages.producerSubmit(*hdr);
}

void Log::Handler::defaultWriter_(const Entry &entry) const noexcept {
    using namespace std::chrono;
    const auto elapsed_seconds = duration_cast<nanoseconds>(entry.m_timestamp - m_started_at).count() / 1e-9;

    hal::outputDebugFmt("[{:08.3}][{}][{}] {} -- {} {}\n",
                        elapsed_seconds,
                        entry.m_thread_id,
                        entry.m_site.m_category.m_name.view(),
                        toString<char>(entry.m_site.m_verbosity),
                        entry.m_message,
                        entry.m_params);
}

void Log::Handler::backgroundWorkerLoop_(Handler &handler) noexcept {
    while (true) {
        const auto hdr = handler.m_messages.consumerAcquire();

        if (not hdr.has_value()) [[unlikely]] {
            PPR_ASSERT(hdr.error() == RawChannel::error_closed);
            return;
        }

        PPR_DEFER {
            handler.m_messages.consumerRelease(*hdr);
        };

        const std::lock_guard scope_lock(handler.m_writer_barrier);
        auto *const p_entry = static_cast<const Entry *>(hdr->data());
        handler.m_writer_policy(*p_entry);
    }
}

}
