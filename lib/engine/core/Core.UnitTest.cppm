module;
#include "pP/Macros.h"
export module engine.core:unit_test;

import :assert;
import :function.ref;
import :logger;

import std;

namespace pP {
    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

    export [[noreturn]] void onTestAssertionFailure(const char *message, const std::source_location &site);

    export class UnitTest {
    protected:
        struct RunImpl;

        [[nodiscard]] bool startInChildProcess_(RunImpl &run) const;

    public:
        enum EFlags : u32 {
            none = 0,
            expect_fail = 1 << 0,
            fork = 1 << 1,

            expect_crash = expect_fail | fork,
        };

        [[nodiscard]] friend constexpr EFlags operator|(const EFlags a, const EFlags b) noexcept {
            return static_cast<EFlags>(static_cast<u32>(a) | static_cast<u32>(b));
        }

        enum EStatus : u8 {
            fail = 0,
            pass = 1,
        };

        struct Id {
            const RunImpl *m_run{nullptr};

            explicit constexpr Id(const RunImpl &run) noexcept : m_run(&run) {
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
                return std::ranges::copy(name, output).out;
            }
        };

        struct [[nodiscard]] IRun {
        protected:
            virtual ~IRun() = default;

        public:
            [[nodiscard]] virtual Id getTestId() const noexcept = 0;

            virtual void log(const char *msg, const std::source_location &location = std::source_location::current()) = 0; // NOLINT(*-default-arguments)

            virtual void failWith(const char *msg, const std::source_location &location = std::source_location::current()) noexcept(false) = 0; // NOLINT(*-default-arguments)

            virtual void recurse(const UnitTest &test) = 0;
            virtual void recurse(std::initializer_list<const UnitTest> tests) = 0;

            virtual void success() = 0;
        };

        using test_func_t = void (*)(IRun &run);

        using test_func_ec_t = std::error_code (*)(IRun &run);

        consteval UnitTest(const char *const name, const test_func_t test, const EFlags flags = none) noexcept
            : m_name(name), m_run(test), m_flags(flags) {
        }

        consteval UnitTest(const char *const name, const test_func_ec_t test, const EFlags flags = none) noexcept
            : m_name(name), m_run_ec(test), m_flags(flags) {
        }

        [[nodiscard]] constexpr bool isExpectedToFail() const noexcept {
            return (m_flags & expect_fail) == expect_fail;
        }

        void run(IRun &run) const noexcept;

        struct Context {
            using LogHandler = std23::function_ref<void(const IRun &, const char *, const std::source_location &)>;

            std::string m_filter_path{};

            std::optional<LogHandler> m_fail_with{};
            std::optional<LogHandler> m_log{};
            std::optional<unsigned> m_shuffle_seed{};

            TimeSpan m_watchdog_repeat{std::chrono::seconds(30)};
            TimeSpan m_watchdog_timeout{std::chrono::seconds(30)};

            bool m_is_child_run = false;

            mutable u32 m_num_executed{0u};

            void setFilter(std::string_view path) noexcept;

            [[nodiscard]] bool hasFilter() const noexcept;

            [[nodiscard]] bool isChildRun() const noexcept;

            void markAsChildRun() noexcept;

            [[nodiscard]] bool filterMatches(std::string_view path) const noexcept;

            [[nodiscard]] u32 numExecuted() const noexcept {
                return m_num_executed;
            }
        };

        static void run(const Context &context, const UnitTest &test) noexcept;

        struct Named {
            const char *m_name{nullptr};
            EFlags m_flags{none};

            template<std::size_t nLen>
            explicit consteval Named(const char (&name)[nLen], const std::initializer_list<EFlags> flags = {}) noexcept
                : m_name(name), m_flags(aggregateFlags(flags)) {
            }

            template<std::convertible_to<test_func_t> CallbackT>
            consteval UnitTest operator/(CallbackT &&callback) const noexcept {
                return UnitTest(m_name, callback, m_flags);
            }

            template<std::convertible_to<test_func_ec_t> CallbackT>
            consteval UnitTest operator/(CallbackT &&callback) const noexcept {
                return UnitTest(m_name, callback, m_flags);
            }
        };

        static constexpr EFlags aggregateFlags(const std::initializer_list<EFlags> flags) noexcept {
            u32 result = 0u;
            for (const auto flag: flags) {
                result |= flag;
            }
            return static_cast<EFlags>(result);
        }

    protected:
        const char *m_name{nullptr};
        test_func_t m_run{nullptr};
        test_func_ec_t m_run_ec{nullptr};
        EFlags m_flags{none};

        struct Watchdog;

        struct RunImpl final : IRun {
            const Context &m_context;
            const UnitTest &m_test;

            RunImpl *m_parent{nullptr};
            RunImpl *m_root{nullptr};

            std::unique_ptr<Watchdog> m_watchdog{};
            std::stringstream m_retained_log{};

            std::optional<Log::Policy> m_prev_logger_policy{};
            std::optional<Log::ELevel> m_prev_logger_verbosity{};
#if PPR_ENABLE_ASSERTIONS
            std::optional<Assertion::Policy> m_prev_assert_policy{};
#endif

            std::optional<std::pair<std::source_location, std::string>> m_failure;

            std::chrono::steady_clock::time_point m_start_time{};
            std::chrono::steady_clock::time_point m_end_time{};

            u32 m_num_passed{0u};
            u32 m_num_failed{0u};

            u32 m_depth{0u};

            EStatus m_status{pass};

            RunImpl(const Context &context, const UnitTest &test) noexcept;

            RunImpl(const Context &context, const UnitTest &test, RunImpl &parent) noexcept;

            void start() noexcept;
            void stop() noexcept;

            [[nodiscard]] const RunImpl &getFirstRunImpl() const noexcept { return *m_root; }

            [[nodiscard]] std::string getCurrentPath() const;

            [[nodiscard]] Id getTestId() const noexcept override;

            void logEntry(const Log::Entry &entry);

            void log(const char *msg, const std::source_location &location) override;

            void failWith(const char *msg, const std::source_location &location) override;

            void recurse(const UnitTest &test) override;
            void recurse(std::initializer_list<const UnitTest> tests) override;

            void success() override;

#if PPR_ENABLE_ASSERTIONS
            void onAssertFailure(const Assertion &condition) const;
#endif
        };
    };
}

