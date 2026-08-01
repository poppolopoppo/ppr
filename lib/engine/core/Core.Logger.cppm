module;
#include "pP/Macros.h"
export module engine.core:logger;

import :concurrency.channel;
import :enums;
import :hal;
import :memory.allocator;
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

        [[nodiscard]] static std::basic_string_view<char> toString(std::type_identity_t<char>, ELevel level) noexcept;
        [[nodiscard]] static std::basic_string_view<char8_t> toString(std::type_identity_t<char8_t>, ELevel level) noexcept;
        [[nodiscard]] static std::basic_string_view<wchar_t> toString(std::type_identity_t<wchar_t>, ELevel level) noexcept;

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
            hal::ThreadId m_thread_id;
        };

        using Policy = std23::function_ref<void(const Entry &)>;

        static Policy setWriterPolicy(Policy writer_policy) noexcept;

        static void log(const Emitter &emitter, string_literal message, opaque::Dict params = {}) noexcept;

        static void logRaw(const Emitter &emitter, std::string_view copy_message, opaque::Dict params = {}) noexcept;

        static void flush() noexcept;

        class Handler;
    };

    PPR_PRAGMA_WARNING_POP()
}
