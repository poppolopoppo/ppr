module;
#include "pP/Macros.h"
module engine.core;
import :logger;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // logger level formatting
    // ------------------------------------------------------------------

    namespace {
        template<details::TChar CharT>
        [[nodiscard]] constexpr std::basic_string_view<CharT> toString_(const Log::ELevel level) noexcept {
            switch (level) {
                using enum Log::ELevel;
            case debug:
                return PPR_LITERAL_FOR(CharT, "👾");
            case verbose:
                return PPR_LITERAL_FOR(CharT, "👁️");
            case info:
                return PPR_LITERAL_FOR(CharT, "ℹ️");
            case emphasis:
                return PPR_LITERAL_FOR(CharT, "👉");
            case warning:
                return PPR_LITERAL_FOR(CharT, "⚠️");
            case error:
                return PPR_LITERAL_FOR(CharT, "❌");
            case fatal:
                return PPR_LITERAL_FOR(CharT, "💀");
            }
            std::unreachable();
        }
    }

    [[nodiscard]] std::basic_string_view<char> Log::toString(std::type_identity_t<char>, const ELevel level) noexcept {
        return toString_<char>(level);
    }

    [[nodiscard]] std::basic_string_view<char8_t> Log::toString(std::type_identity_t<char8_t>, const ELevel level) noexcept {
        return toString_<char8_t>(level);
    }

    [[nodiscard]] std::basic_string_view<wchar_t> Log::toString(std::type_identity_t<wchar_t>, const ELevel level) noexcept {
        return toString_<wchar_t>(level);
    }

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
        const TimePoint timestamp = std::chrono::steady_clock::now();

        mem::GrowingSlab slab;
        opaque::Block(params, slab);

        const std::span<const std::byte> params_block = slab.consumed();
        const std::size_t entry_size = sizeof(Entry) + params_block.size_bytes();
        const auto hdr = m_messages.producerReserve(entry_size, RawChannel::wait_if_full);
        PPR_ASSERT(hdr.has_value());

        auto *const slot = static_cast<std::byte *>(const_cast<void *>(hdr->data()));
        auto *const entry = new (slot) Entry{
            .m_message{message.view()},
            .m_params{},
            .m_site{emitter},
            .m_timestamp{timestamp},
            .m_thread_id{std::this_thread::get_id()},
        };

        std::memcpy(slot + sizeof(Entry), params_block.data(), params_block.size_bytes());
        entry->m_params.m_data = reinterpret_cast<opaque::Block::Dict *>(slot + sizeof(Entry));

        m_messages.producerSubmit(*hdr);
    }

    void Log::Handler::defaultWriter_(const Entry &entry) const noexcept {
        using namespace std::chrono;
        const auto elapsed_seconds = static_cast<double>(duration_cast<nanoseconds>(entry.m_timestamp - m_started_at).count()) / 1e9;

#if 0
        hal::outputDebugFmt("[{:08.3f}][{}][{}] {} -- {} {}\n", elapsed_seconds, entry.m_thread_id, entry.m_site.m_category.m_name.view(),
            toString_<char>(entry.m_site.m_verbosity), entry.m_message, entry.m_params);
#else
        std::println(std::cout, "[{:08.3f}][{}][{}] {} -- {} {}", elapsed_seconds, entry.m_thread_id, entry.m_site.m_category.m_name.view(),
            toString_<char>(entry.m_site.m_verbosity), entry.m_message, entry.m_params);
        std::cout.flush();
#endif
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
