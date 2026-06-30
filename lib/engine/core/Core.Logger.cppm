module;
#include "pP/Macros.h"
export module engine.core:logger;

import :memory.allocator;
import :concurrency.channel;
import :enums;
import :opaque;
import :strings;
import :timer;

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

        [[nodiscard]] static std::basic_string_view<char> toString(std::type_identity_t<char>, const ELevel level) noexcept;
        [[nodiscard]] static std::basic_string_view<char8_t> toString(std::type_identity_t<char8_t>, const ELevel level) noexcept;
        [[nodiscard]] static std::basic_string_view<wchar_t> toString(std::type_identity_t<wchar_t>, const ELevel level) noexcept;

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

            constexpr explicit Category(const string_literal name, const ELevel verbosity = ELevel::debug, const EFlags flags = none) noexcept
                : m_name(name),
                  m_flags(flags),
                  m_verbosity(verbosity) {
            }
        };

        struct Emitter {
            const Category &m_category;
            std::source_location m_location;
            ELevel m_verbosity;

            constexpr Emitter(
                const Category &category,
                const ELevel verbosity,
                const std::source_location &location = std::source_location::current() ) noexcept
                : m_category(category),
                  m_location(location),
                  m_verbosity(verbosity) {
            }
        };

        struct Entry {
            std::string_view m_message;
            opaque::Block m_params;
            Emitter m_site;

            TimePoint m_timestamp;
            std::thread::id m_thread_id;
        };

        using Policy = std23::function_ref<void(const Entry &)>;

        static Policy setWriterPolicy(Policy writer_policy) noexcept;

        static void log(const Emitter &emitter, const string_literal message, const opaque::Dict params = {}) noexcept;

        static void flush() noexcept;

        class Handler {
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

            void log(const Emitter &emitter, const string_literal message, const opaque::Dict params = {}) noexcept;
        };
    };

    PPR_PRAGMA_WARNING_POP()
}
