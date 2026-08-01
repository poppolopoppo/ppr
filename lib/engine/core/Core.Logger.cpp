module;
#include "../../../out/build/msvc-dev/vcpkg_installed/x64-windows/include/fmt/base.h"
#include "pP/Macros.h"
module engine.core;
import :hal;
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
    // asynchronous log handler
    // ------------------------------------------------------------------

    class Log::Handler {
#ifdef PPR_LOG_BUFFER_SIZE
        static constexpr std::size_t buffer_size_v = PPR_LOG_BUFFER_SIZE;
#else
        static constexpr std::size_t buffer_size_v = 2ull << 20u;
#endif

        void defaultWriter_(const Entry &entry) const noexcept;

        static void backgroundWorkerLoop_(Handler &handler) noexcept;

        alignas(hal::cacheline_size_v) RawChannel m_messages;

        alignas(hal::cacheline_size_v) std::mutex m_writer_barrier{};
        Policy m_writer_policy;

        alignas(hal::cacheline_size_v) std::jthread m_background_worker{};
        const TimePoint m_started_at{};

        Handler() noexcept;

    public:
        ~Handler() noexcept;

        [[nodiscard]] static Handler &get() noexcept;

        Policy setWriterPolicy(Policy writer_policy) noexcept;

        void flush() noexcept;

        void log(const Emitter &emitter, string_literal message, opaque::Dict params = {}) noexcept;

        void logRaw(const Emitter &emitter, std::string_view copy_message, opaque::Dict params = {}) noexcept;
    };

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
        std::ignore = m_messages.flush();
    }

    void Log::Handler::log(const Emitter &emitter, const string_literal message, const opaque::Dict params) noexcept {
        const TimePoint timestamp = std::chrono::steady_clock::now();

        const std::size_t block_size_bytes = opaque::Block::sizeOf(params);
        const std::size_t entry_size_bytes = sizeof(Entry) + block_size_bytes;

        const auto hdr = m_messages.producerReserve(entry_size_bytes, RawChannel::wait_if_full);
        if (not PPR_ENSURE(hdr.has_value())) [[unlikely]] {
            return;
        }

        auto *const slot = static_cast<std::byte *>(const_cast<void *>(hdr->data()));
        auto *const entry = new(slot) Entry{
            .m_message{message.view()},
            .m_site{emitter},
            .m_timestamp{timestamp},
            .m_thread_id{hal::currentThreadId()},
        };

        mem::Slab slab{slot + sizeof(Entry), block_size_bytes};
        entry->m_params.resetAssumeEmpty(params, slab);

        m_messages.producerSubmit(*hdr);
    }

    void Log::Handler::logRaw(const Emitter &emitter, const std::string_view message, const opaque::Dict params) noexcept {
        const TimePoint timestamp = std::chrono::steady_clock::now();

        const std::size_t block_size_bytes = opaque::Block::sizeOf(params);
        const std::size_t message_size_bytes = alignForward(message.size() * sizeof(message[0]), max_align_v);
        const std::size_t entry_size_bytes = sizeof(Entry) + message_size_bytes + block_size_bytes;

        const auto hdr = m_messages.producerReserve(entry_size_bytes, RawChannel::wait_if_full);
        if (not PPR_ENSURE(hdr.has_value())) [[unlikely]] {
            return;
        }

        auto *const slot = static_cast<std::byte *>(const_cast<void *>(hdr->data()));

        auto *const embedded_message = reinterpret_cast<char *>(slot + sizeof(Entry));
        std::memcpy(embedded_message, message.data(), message.size() * sizeof(message[0]));

        auto *const entry = new(slot) Entry{
            .m_message{embedded_message, message.size()},
            .m_site{emitter},
            .m_timestamp{timestamp},
            .m_thread_id{hal::currentThreadId()},
        };

        mem::Slab slab{slot + sizeof(Entry) + message_size_bytes, block_size_bytes};
        entry->m_params.resetAssumeEmpty(params, slab);

        m_messages.producerSubmit(*hdr);
    }

    void Log::Handler::defaultWriter_(const Entry &entry) const noexcept {
        using namespace std::chrono;
        const auto elapsed_seconds = duration_cast<nanoseconds>(entry.m_timestamp - m_started_at).count() / 1e9;

#if 0
        hal::outputDebugFmt("{} [{:08.3f}][{}][{}] -- {} {}\n",
            toString_<char>(entry.m_site.m_verbosity),
            elapsed_seconds, entry.m_thread_id, entry.m_site.m_category.m_name.view(),
            entry.m_message, entry.m_params);
#else
        std::println(std::cout, "{:08.3f} {} {} [{:16}] -- {} {}",
            elapsed_seconds, entry.m_thread_id,
            toString_<char>(entry.m_site.m_verbosity),
            entry.m_site.m_category.m_name.view(),
            entry.m_message, entry.m_params);

        if (entry.m_site.m_verbosity > ELevel::verbose) {
            std::cout.flush();
        }
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

    // ------------------------------------------------------------------
    // public api for the logger
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

    void Log::logRaw(const Emitter &emitter, const std::string_view copy_message, const opaque::Dict params) noexcept {
        Handler::get().logRaw(emitter, copy_message, params);
    }
}
