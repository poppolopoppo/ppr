module;
#include "pP/Macros.h"
export module engine.core:unit_test;

import :assert;
import :function.ref;
import :hal;

import std;

namespace pP {
    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

    export class UnitTest {
    protected:
        struct RunImpl;

        [[nodiscard]] bool startInChildProcess_(RunImpl &run) const;

    public:
        struct TimeDuration {
            std::chrono::steady_clock::duration m_value;

            explicit constexpr TimeDuration(const std::chrono::steady_clock::duration value) noexcept
                : m_value(value) {
            }
        };

        enum EFlags : u32 {
            none = 0,
            expect_fail = 1 << 0,
            fork = 1 << 1,

            expect_crash = expect_fail | fork,
        };

        [[nodiscard]] friend constexpr EFlags operator|(EFlags a, EFlags b) noexcept {
            return static_cast<EFlags>(static_cast<u32>(a) | static_cast<u32>(b));
        }

        enum EStatus : u8 {
            fail = 0,
            pass = 1,
        };

        struct Id {
            const RunImpl *m_run{nullptr};

            explicit constexpr Id(const RunImpl &run) noexcept
                : m_run(&run) {
            }

            [[nodiscard]] std::string_view name() const noexcept {
                return m_run->m_test.m_name;
            }

            [[nodiscard]] std::string path() const noexcept;

            template<details::TChar CharT, typename OutT>
            auto format(OutT &output) const -> OutT {
                if (m_run->m_parent) {
                    output = Id(*m_run->m_parent).format<CharT>(output);
                    *output++ = CharT('/');
                }
                const std::string_view name{m_run->m_test.m_name};
                return std::format_to(output, "{}", name);
            }
        };

        struct [[nodiscard]] IRun {
        protected:
            ~IRun() = default;

        public:
            [[nodiscard]] virtual Id getTestId() const noexcept = 0;

            virtual void log(const char *msg) = 0;

            virtual void failWith(const char *msg) noexcept(false) = 0;

            virtual void recurse(const UnitTest &test) = 0;
            virtual void recurse(std::initializer_list<const UnitTest> tests) = 0;

            virtual void success() = 0;

            template<typename... ArgsT>
            void logFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept {
                char buffer[2048];
                const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
                *out = char{0};
                log(buffer);
            }

            template<typename... ArgsT>
            void failFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept(false) {
                char buffer[2048];
                const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
                *out = char{0};
                failWith(buffer);
            }
        };

        using test_func_t = void (*)(IRun &run);

        consteval UnitTest(
            const char *const name,
            const test_func_t test,
            const EFlags flags = none) noexcept
            : m_name(name), m_run(test), m_flags(flags) {
        }

        [[nodiscard]] constexpr bool isExpectedToFail() const noexcept {
            return (m_flags & expect_fail) == expect_fail;
        }

        void run(IRun &run) const noexcept;

        struct Context {
            using LogHandler = std23::function_ref<void(const IRun &, const char *)>;

            std::string m_filter_path{};

            std::optional<LogHandler> m_fail_with{};
            std::optional<LogHandler> m_log{};
            std::optional<unsigned> m_shuffle_seed{};

            bool m_is_child_run = false;

            void setFilter(std::string_view path) noexcept;

            [[nodiscard]] bool hasFilter() const noexcept;

            [[nodiscard]] bool isChildRun() const noexcept;

            void markAsChildRun() noexcept;

            [[nodiscard]] bool filterMatches(const std::string_view path) const noexcept;
        };


        static void run(const Context &context, const UnitTest &test) noexcept;

        struct Named {
            const char *m_name{nullptr};
            EFlags m_flags{none};

            template<std::size_t nLen>
            explicit consteval Named(
                const char (&name)[nLen],
                const std::initializer_list<EFlags> flags = {}) noexcept
                : m_name(name), m_flags(aggregateFlags(flags)) {
            }

            template<std::convertible_to<test_func_t> CallbackT>
            consteval UnitTest operator /(CallbackT &&callback) const noexcept {
                return UnitTest(m_name, callback, m_flags);
            }
        };

        static constexpr EFlags aggregateFlags(std::initializer_list<EFlags> flags) noexcept {
            u32 result = 0u;
            for (const auto flag: flags) {
                result |= flag;
            }
            return static_cast<EFlags>(result);
        }

    protected:
        const char *m_name{nullptr};
        test_func_t m_run{nullptr};
        EFlags m_flags{none};

        struct RunImpl final : IRun {
            const Context &m_context;
            const UnitTest &m_test;
            RunImpl *m_parent{nullptr};

#if PPR_ENABLE_ASSERTIONS
            std::optional<Assertion::Policy> m_prev_assert_policy{};
#endif

            std::string m_failure{"???"};

            std::chrono::steady_clock::time_point m_start_time{};

            u32 m_num_passed{0u};
            u32 m_num_failed{0u};

            u32 m_depth{0u};

            EStatus m_status{pass};

            RunImpl(const Context &context, const UnitTest &test) noexcept;

            RunImpl(const Context &context, const UnitTest &test, RunImpl &parent) noexcept;

            ~RunImpl();

            void start() noexcept;

            [[nodiscard]] const RunImpl &getFirstRunImpl() const noexcept;

            [[nodiscard]] Id getTestId() const noexcept override;

            [[nodiscard]] std::string currentPath() const;

            void log(const char *msg) override;

            void failWith(const char *msg) override;

            void recurse(const UnitTest &test) override;
            void recurse(std::initializer_list<const UnitTest> tests) override;

            void success() override;

#if PPR_ENABLE_ASSERTIONS
            void onAssertFailure(const Assertion &condition) const;
#endif
        };
    };
}

namespace std {
    template<pP::details::TChar CharT>
    struct formatter<pP::UnitTest::Id, CharT>
            : formatter<std::basic_string<CharT>, CharT> // ← inherit parse + all specs
    {
        template<typename FormatContextT>
        auto format(const pP::UnitTest::Id &value, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            std::basic_string<CharT> buf;
            auto output(std::back_inserter(buf));
            value.format<CharT>(output);
            return formatter<std::basic_string<CharT>, CharT>::format(buf, ctx);
        }
    };

    template<pP::details::TChar CharT>
    struct formatter<pP::UnitTest::TimeDuration, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::UnitTest::TimeDuration &td, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            using namespace std::chrono;
            const auto ns = duration_cast<nanoseconds>(td.m_value).count();

            if (ns < 1'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{}ns"), ns);
            }
            if (ns < 100'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}µs"), ns / 1'000.0);
            }
            if (ns < 1'000'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{}µs"), duration_cast<microseconds>(td.m_value).count());
            }
            if (ns < 1'000'000'000LL) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}ms"), ns / 1'000'000.0);
            }
            return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.2f}s"), ns / 1'000'000'000.0);
        }
    };
}


