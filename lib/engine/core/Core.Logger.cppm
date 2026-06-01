module;
#include "pP/Macros.h"
export module engine.core:logger;

import :allocator;
import :channel;
import :enums;
import :opaque;
import :strings;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // asynchronous logger
    // ------------------------------------------------------------------

    PPR_PRAGMA_WARNING_PUSH()
    PPR_PRAGMA_WARNING_DISABLE_MSVC(4324) // 'pP::Log::Handler': structure was padded due to alignment specifier

    struct Log {
        enum class ELevel : u8 {
            debug,
            verbose,
            info,
            emphasis,
            warning,
            error,
            fatal,
        };

        template<details::TChar CharT>
        [[nodiscard]] static constexpr std::basic_string_view<CharT> toString(const ELevel level) noexcept;

        struct Category {
            enum EFlags : u8 {
                none = 0,

                immediate = 1 << 0,
                break_on_error = 1 << 1,
                break_on_warning = 1 << 2,

                all = immediate | break_on_error | break_on_warning,
            };

            string_literal m_name;
            EFlags m_flags;
            ELevel m_verbosity;

            constexpr explicit Category(
                const string_literal name,
                const ELevel verbosity = ELevel::debug,
                const EFlags flags = none) noexcept
                : m_name(name),
                  m_flags(flags),
                  m_verbosity(verbosity) {
            }
        };

        struct Emitter {
            const Category &m_category;
            std::source_location m_location;
            ELevel m_verbosity;
        };

        struct Entry {
            std::string_view m_message;
            opaque::Block m_params;
            const Emitter &m_site;

            std::chrono::steady_clock::time_point m_timestamp;
            std::thread::id m_thread_id;
        };

        using Policy = std23::function_ref<void(const Entry &)>;

        static Policy setWriterPolicy(Policy writer_policy) noexcept {
            return Handler::get().setWriterPolicy(writer_policy);
        }

        static void flush() noexcept {
            Handler::get().flush();
        }

        static void log(const Emitter &emitter, const string_literal message, const opaque::Dict params = {}) noexcept {
            Handler::get().log(emitter, message, params);
        }

        class Handler {
            static constexpr std::size_t buffer_size_v = 2ull << 20u;

            void defaultWriter_(const Entry &entry) const noexcept;

            static void backgroundWorkerLoop_(Handler &handler) noexcept;

            alignas(hal::cacheline_size_v) RawChannel m_messages;

            alignas(hal::cacheline_size_v) std::mutex m_writer_barrier{};
            Policy m_writer_policy;

            alignas(hal::cacheline_size_v) std::jthread m_background_worker;
            const std::chrono::steady_clock::time_point m_started_at;


            Handler() noexcept
                : m_messages(buffer_size_v),
                  m_writer_policy{std23::nontype<&Handler::defaultWriter_>, this},
                  m_background_worker(&Handler::backgroundWorkerLoop_, std::ref(*this)),
                  m_started_at(std::chrono::steady_clock::now()) {
            }

        public:
            ~Handler() noexcept {
                PPR_VERIFY(m_messages.close().has_value());
            }

            [[nodiscard]] static Handler &get() noexcept {
                static Handler g_instance;
                return g_instance;
            }

            Policy setWriterPolicy(Policy writer_policy) noexcept {
                const std::lock_guard scope_lock{m_writer_barrier};
                std::swap(writer_policy, m_writer_policy);
                return writer_policy;
            }

            void flush() noexcept {
                PPR_VERIFY(m_messages.flush().has_value());
            }

            void log(const Emitter &emitter, const string_literal message, const opaque::Dict params = {}) noexcept {
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
        };
    };

    PPR_PRAGMA_WARNING_POP()

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

    template<details::TChar CharT>
    [[nodiscard]] constexpr std::basic_string_view<CharT> Log::toString(const ELevel level) noexcept {
        switch (level) {
            case ELevel::debug: return PPR_LITERAL_FOR(CharT, "👾");
            case ELevel::verbose: return PPR_LITERAL_FOR(CharT, "👁️");
            case ELevel::info: return PPR_LITERAL_FOR(CharT, "ℹ️");
            case ELevel::emphasis: return PPR_LITERAL_FOR(CharT, "👉");
            case ELevel::warning: return PPR_LITERAL_FOR(CharT, "⚠️");
            case ELevel::error: return PPR_LITERAL_FOR(CharT, "❌");
            case ELevel::fatal: return PPR_LITERAL_FOR(CharT, "💀");
        }
        std::unreachable();
    }
}
