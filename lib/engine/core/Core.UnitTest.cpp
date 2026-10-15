module;
#include "pP/Macros.h"

module engine.core;

import :unit_test;
import :assert;
import :hal;
import :function.ref;
import :utility;

import std;

namespace std {
    template<pP::details::TChar CharT>
    struct formatter<pP::UnitTest::Id, CharT>
        : formatter<std::basic_string<CharT>, CharT> // ← inherit parse + all specs
    {
        template<typename FormatContextT>
        auto format(const pP::UnitTest::Id &value, FormatContextT &ctx) const -> decltype(ctx.out()) {
            std::basic_string<CharT> buf;
            auto output(std::back_inserter(buf));
            value.format<CharT>(output);
            return formatter<std::basic_string<CharT>, CharT>::format(buf, ctx);
        }
    };
}

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(UnitTest, debug, none)

    // ------------------------------------------------------------------
    // Unit test watchdog detects stalls
    // ------------------------------------------------------------------

    struct UnitTest::Watchdog {
        explicit Watchdog(const TimeSpan timeout, const TimeSpan repeat) noexcept
            : m_timeout(timeout), m_repeat(repeat) {
            if (m_timeout <= TimeSpan{}) {
                m_disabled = true;
                return;
            }

            try {
                m_thread = std::jthread([this](const std::stop_token &st) noexcept { loop(st); });
            } catch (...) {
                // Best-effort diagnostics only: run without watchdog.
                m_disabled = true;
            }
        }

        ~Watchdog() noexcept = default;

        Watchdog(const Watchdog &) = delete;
        Watchdog &operator=(const Watchdog &) = delete;

        void push(const UnitTest &unit_test) noexcept {
            if (m_disabled) {
                return;
            }

            try {
                const auto now = std::chrono::steady_clock::now();

                const std::lock_guard scope_lock(m_mutex);
                m_stack.push_back(Entry{.m_test = std::addressof(unit_test), .m_started = now, .m_last_report = now});
                ++m_gen;
            } catch (...) {
                // Best-effort diagnostics only: skip this report.
            }

            m_cv.notify_all();
        }

        void pop() noexcept {
            if (m_disabled) {
                return;
            }

            try {
                const std::lock_guard scope_lock(m_mutex);
                if (not m_stack.empty()) {
                    m_stack.pop_back();
                    ++m_gen;
                }
            } catch (...) {
            }

            m_cv.notify_all();
        }

    private:
        struct Entry {
            const UnitTest *m_test;
            std::chrono::steady_clock::time_point m_started{};
            std::chrono::steady_clock::time_point m_last_report{};
        };

        // ReSharper disable once CppPassValueParameterByConstReference
        void loop(const std::stop_token st) noexcept {
            hal::setThreadName("UnitTestWatchDog");
            hal::setThreadPriority(hal::EPriority::low);

            std::unique_lock lock(m_mutex);

            while (not st.stop_requested()) {
                if (m_stack.empty()) {
                    m_cv.wait(lock, st, [&] { return st.stop_requested() or not m_stack.empty(); });
                    continue;
                }

                const hal::ProfileScope profile_scope{"Loop", "UnitTestWatchDog"};

                const u64 gen = m_gen;
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = now - m_stack.back().m_started;

                if (elapsed >= m_timeout and now - m_stack.back().m_last_report >= m_repeat) {
                    std::string path;
                    try {
                        for (const Entry &entry: m_stack) {
                            if (not path.empty()) {
                                path += '/';
                            }

                            const char *name = entry.m_test != nullptr ? entry.m_test->m_name : nullptr;
                            path += name != nullptr ? std::string_view(name) : std::string_view("<unknown>");
                        }

                        m_stack.back().m_last_report = now;
                    } catch (...) {
                        // Best-effort diagnostics only (includes bad_alloc here).
                        lock.unlock();
                        m_cv.notify_all();
                        lock.lock();
                        continue;
                    }

                    lock.unlock();
                    try {
                        std::println(
                            std::cerr, " ⏳ test stalled: {} elapsed={} (timeout={})", path, elapsed,
                            m_timeout);
                        std::cerr.flush();
                    } catch (...) {
                    }
                    lock.lock();
                    continue;
                }

                const auto next_due = elapsed >= m_timeout ? m_stack.back().m_last_report + m_repeat
                                                           : m_stack.back().m_started + m_timeout;

                (void) m_cv.wait_until(lock, st, next_due, [&] { return st.stop_requested() or gen != m_gen; });
            }
        }

        mutable std::mutex m_mutex{};
        std::condition_variable_any m_cv{};

        std::vector<Entry> m_stack{};
        u64 m_gen{0u};

        const TimeSpan m_timeout{};
        const TimeSpan m_repeat{};

        bool m_disabled{false};
        std::jthread m_thread{};
    };

    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

    [[noreturn]] void onTestAssertionFailure(const char *message, const std::source_location &site) {
        const std::string text = std::format(
            "{}({}): TEST assert failed: \"{}\"\n\tin function: {}\n", site.file_name(), site.line(), message,
            site.function_name());
        PPR_THROW(UnitTest, std::logic_error{text});
    }

    bool UnitTest::startInChildProcess_(RunImpl &run) const {
        if (run.m_context.isChildRun()) {
            m_run(run);
            return true;
        }

        const auto exe_path = hal::process::currentExecutablePath();
        const std::string test_path = run.getCurrentPath();
        const std::vector<std::string> child_args{"--child-run", "--run-test", test_path};
        const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
        return (exit_code == 0);
    }

    std::string UnitTest::Id::path() const noexcept {
        return m_run->getCurrentPath();
    }

    void UnitTest::run(IRun &run) const noexcept {
        auto &impl = checked_cast<RunImpl>(run);
        impl.start();
        PPR_DEFER {
            impl.stop();
        };

        bool failed = false;
        std::string failure_message;

        try {
            if ((m_flags & fork) == none) [[likely]] {
                if (m_run_ec != nullptr) {
                    const auto ec = m_run_ec(run);
                    if (hasFailed(ec)) {
                        failed = true;
                        failure_message = ec.message();
                    }
                } else {
                    m_run(run);
                }
            } else {
                PPR_ASSERT(m_run_ec == nullptr && "error-code tests cannot be forked");
                const bool child_ok = startInChildProcess_(impl);
                if (not child_ok) {
                    failed = true;
                    failure_message = "child process exited with an error";
                }
            }
        } catch (const std::exception &e) {
            if (isExpectedToFail()) {
                run.success();
                return;
            }
            run.failWith(e.what());
            return;
        }

        if (failed) {
            if (isExpectedToFail()) {
                run.success();
            } else if (impl.m_status != pass) {
                // already recorded (group propagation / handler)
            } else {
                run.failWith(failure_message.empty() ? "test failed" : failure_message.c_str());
            }
        } else {
            if (isExpectedToFail()) {
                run.failWith("test succeeded, but it was expected to fail");
            } else if (impl.m_status == pass) {
                run.success();
            }
        }
    }

    void UnitTest::Context::setFilter(std::string_view path) noexcept {
        m_filter_path = path;
    }

    bool UnitTest::Context::hasFilter() const noexcept {
        return !m_filter_path.empty();
    }

    bool UnitTest::Context::isChildRun() const noexcept {
        return m_is_child_run;
    }

    void UnitTest::Context::markAsChildRun() noexcept {
        m_is_child_run = true;
    }

    bool UnitTest::Context::filterMatches(const std::string_view path) const noexcept {
        std::string_view remaining = m_filter_path;
        while (not remaining.empty()) {
            const auto comma = remaining.find(',');
            const std::string_view token = remaining.substr(0, comma);
            if (not token.empty() and (path.starts_with(token) || token.starts_with(path))) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
        return false;
    }

    void UnitTest::run(const Context &context, const UnitTest &test) noexcept {
        RunImpl first_run{context, test};
        test.run(first_run);
    }

    UnitTest::RunImpl::RunImpl(const Context &context, const UnitTest &test) noexcept
        : m_context(context), m_test(test), m_root(this) {

        if (m_context.m_watchdog_repeat > TimeSpan{} and m_context.m_watchdog_timeout > TimeSpan{}) {
            m_watchdog = std::make_unique<Watchdog>(m_context.m_watchdog_timeout, m_context.m_watchdog_repeat);
        }
    }

    UnitTest::RunImpl::RunImpl(const Context &context, const UnitTest &test, RunImpl &parent) noexcept
        : m_context(context), m_test(test), m_parent(&parent), m_root(parent.m_root), m_depth(parent.m_depth + 1u) {
    }

    UnitTest::Id UnitTest::RunImpl::getTestId() const noexcept {
        return Id(*this);
    }

    std::string UnitTest::RunImpl::getCurrentPath() const {
        std::string result;
        auto it = std::back_inserter(result);
        getTestId().format<char>(it);
        return result;
    }

    void UnitTest::RunImpl::logEntry(const Log::Entry &entry) {
        // NOTE: the logger is optional/best-effort and must never decide the verdict.
        // Route every entry (including error) to the normal log/capture path so it is
        // still reported, without marking the test failed. Failures come only from
        // explicit paths: PPR_TEST_ASSERT / onAssertFailure, expected-exception
        // assertions, and unexpected escapes handled in UnitTest::run.
        log(entry.m_message.data(), entry.m_site.m_location);
    }

    void UnitTest::RunImpl::log(const char *msg, const std::source_location &location) {
        if (msg == nullptr) [[unlikely]] {
            return;
        }

        if (m_context.m_log.has_value()) {
            (*m_context.m_log)(*this, msg, location);
        } else {
            std::println(
                m_retained_log, "{}: {}({}): {}", std::string_view(m_test.m_name), location.file_name(),
                location.line(), std::string_view(msg));
        }
    }

    void UnitTest::RunImpl::failWith(const char *msg, const std::source_location &location) noexcept(false) {
        if (m_num_failed++ == 0u) {
            m_status = fail;
        }

        if (msg != nullptr and not m_failure.has_value()) {
            m_failure.emplace(location, msg);
        }

        if (m_parent != nullptr) {
            m_parent->failWith(msg, location);
        } else if (m_context.m_fail_with.has_value()) {
            (*m_context.m_fail_with)(*this, msg, location);
        } else {
            PPR_THROW(UnitTest, std::logic_error{msg});
        }
    }

    void UnitTest::RunImpl::recurse(const UnitTest &test) {
        if (m_context.hasFilter()) {
            const std::string child_path = getCurrentPath() + "/" + test.m_name;
            if (!m_context.filterMatches(child_path)) {
                return;
            }
        }

        RunImpl new_run{m_context, test, *this};
        test.run(new_run);
    }

    void UnitTest::RunImpl::recurse(std::initializer_list<const UnitTest> tests) {
        auto order = std::vector<std::size_t>(tests.size());
        std::iota(order.begin(), order.end(), std::size_t{0});

        if (auto seed = m_context.m_shuffle_seed) {
            std::minstd_rand rng(*seed);
            std::ranges::shuffle(order, rng);
        }

        for (const auto idx: order) {
            recurse(tests.begin()[static_cast<std::ptrdiff_t>(idx)]);
        }
    }

    void UnitTest::RunImpl::success() {
        ++m_num_passed;
        if (m_parent != nullptr) {
            m_parent->success();
        }
    }

#if PPR_ENABLE_ASSERTIONS
    void UnitTest::RunImpl::onAssertFailure(const Assertion &condition) const {
#if defined(__cpp_lib_stacktrace)
        const std::stacktrace backtrace = std::stacktrace::current(9);
#endif

        if (not m_test.isExpectedToFail()) {
#if defined(__cpp_lib_stacktrace)
            std::println(
                std::cerr,
                "{}({}): Assertion failed with \"{}\"\n"
                "\tin function: {}\n"
                "\tin test: {}\n\n"
                "Callstack:\n{}",
                condition.m_site.file_name(), condition.m_site.line(), condition.m_message,
                condition.m_site.function_name(), getTestId(), backtrace);
#else
            std::println(
                std::cerr,
                "{}({}): Assertion failed with \"{}\"\n"
                "\tin function: {}\n"
                "\tin test: {}",
                condition.m_site.file_name(), condition.m_site.line(), condition.m_message,
                condition.m_site.function_name(), getTestId());
#endif
            std::cerr.flush();
        }

        PPR_THROW(UnitTest, std::logic_error(condition.m_message));
    }
#endif

    void UnitTest::RunImpl::start() noexcept {
#if PPR_ENABLE_ASSERTIONS
        Assertion::Policy new_policy(std23::nontype<&RunImpl::onAssertFailure>, this);
        m_prev_assert_policy = Assertion::setFailurePolicy(std::move(new_policy));
#endif

        m_retained_log.clear();

        m_prev_logger_verbosity = Log::setMinimumVerboseLevel(Log::ELevel::warning);
        m_prev_logger_policy = Log::setWriterPolicy({std23::nontype<&RunImpl::logEntry>, this});

        if (m_parent != nullptr) {
            m_context.m_num_executed++;
        }

        if (m_root and m_root->m_watchdog) {
            m_root->m_watchdog->push(m_test);
        }

        hal::profileBegin(m_test.m_name, m_parent ? m_parent->getCurrentPath().c_str() : "UnitTests");

        m_start_time = std::chrono::steady_clock::now();
    }

    void UnitTest::RunImpl::stop() noexcept {
        m_end_time = std::chrono::steady_clock::now();

        hal::profileEnd();

        if (m_root and m_root->m_watchdog) {
            m_root->m_watchdog->pop();
        }

        const TimeSpan test_duration{m_end_time - m_start_time};

        PPR_DEFER {
            m_retained_log.clear();
        };

        // NOTE: the writer policy must be restored in every configuration. Gating this on
        // PPR_ENABLE_ASSERTIONS (via the assert policy) left the per-test writer installed in
        // release builds, so the async log worker later dispatched into a destroyed RunImpl.
        if (m_prev_logger_policy.has_value()) [[likely]] {
            Log::setWriterPolicy(m_prev_logger_policy.value());
            m_prev_logger_policy.reset();
        }

        if (m_prev_logger_verbosity.has_value()) [[likely]] {
            Log::setMinimumVerboseLevel(m_prev_logger_verbosity.value());
            m_prev_logger_verbosity.reset();
        }

#if PPR_ENABLE_ASSERTIONS
        PPR_DEFER {
            if (m_prev_assert_policy.has_value()) [[likely]] {
                Assertion::setFailurePolicy(std::move(m_prev_assert_policy.value()));
                m_prev_assert_policy.reset();
            }
        };
#endif

        const Id test_id{*this};
        const bool is_group = m_num_passed + m_num_failed > 1u;
        const char *bullet = is_group ? "\u21B3" : "\u2022";
        const char *icon = m_status == pass ? "\u2705" : "\u274C";

        std::println(
            std::cout, " {} {:>3}/{:<3}  {} {:<90} ({})", icon, m_num_passed, m_num_passed + m_num_failed, bullet,
            test_id, test_duration);

        if (m_status == fail) {
            std::cout << m_retained_log.rdbuf();

            if (m_failure.has_value()) {
                std::println(
                    std::cout, "    \u2514\u2500 {}({}): {}", m_failure->first.file_name(), m_failure->first.line(),
                    m_failure->second);
            }
        }
        if (is_group) {
            std::println(std::cout);
        }

        std::cout.flush();
    }
}
