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
        auto format(const pP::UnitTest::Id &value, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            std::basic_string<CharT> buf;
            auto output(std::back_inserter(buf));
            value.format<CharT>(output);
            return formatter<std::basic_string<CharT>, CharT>::format(buf, ctx);
        }
    };
}

namespace pP {
    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

    [[noreturn]] void onTestAssertionFailure(const char *message, const std::source_location &site) {
        const std::string text = std::format(
            "{}({}): TEST assert failed: \"{}\"\n\tin function: {}\n",
            site.file_name(), site.line(), message, site.function_name());
        throw std::logic_error{text};
    }

    bool UnitTest::startInChildProcess_(RunImpl &run) const {
        if (run.m_context.isChildRun()) {
            m_run(run);
            return true;
        }

        const auto exe_path = hal::process::currentExecutablePath();
        const std::string test_path = run.currentPath();
        const std::vector<std::string> child_args{"--child-run", "--run-test", test_path};
        const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
        return (exit_code == 0);
    }

    std::string UnitTest::Id::path() const noexcept {
        return m_run->currentPath();
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
        return path.starts_with(m_filter_path) || m_filter_path.starts_with(path);
    }

    void UnitTest::run(const Context &context, const UnitTest &test) noexcept {
        RunImpl first_run{context, test};
        test.run(first_run);
    }

    UnitTest::RunImpl::RunImpl(const Context &context, const UnitTest &test) noexcept
        : m_context(context),
          m_test(test) {
    }

    UnitTest::RunImpl::RunImpl(const Context &context, const UnitTest &test, RunImpl &parent) noexcept
        : m_context(context),
          m_test(test),
          m_parent(&parent),
          m_depth(parent.m_depth + 1u) {
    }

    const UnitTest::RunImpl &UnitTest::RunImpl::getFirstRunImpl() const noexcept {
        if (m_parent) {
            return m_parent->getFirstRunImpl();
        }
        return *this;
    }

    UnitTest::Id UnitTest::RunImpl::getTestId() const noexcept {
        return Id(*this);
    }

    std::string UnitTest::RunImpl::currentPath() const {
        std::string result;
        auto it = std::back_inserter(result);
        getTestId().format<char>(it);
        return result;
    }

    void UnitTest::RunImpl::log(const char *msg) {
        if (m_context.m_log.has_value()) {
            (*m_context.m_log)(*this, msg);
        } else {
            std::println("{}: {}", std::string_view(m_test.m_name), std::string_view(msg));
        }
    }

    void UnitTest::RunImpl::failWith(const char *msg) noexcept(false) {
        if (m_num_failed++ == 0u) {
            m_failure = msg;
            m_status = fail;
        }

        if (m_parent != nullptr) {
            m_parent->failWith(msg);
        } else if (m_context.m_fail_with.has_value()) {
            (*m_context.m_fail_with)(*this, msg);
        } else {
            throw std::logic_error{msg};
        }
    }

    void UnitTest::RunImpl::recurse(const UnitTest &test) {
        if (m_context.hasFilter()) {
            const std::string child_path = currentPath() + "/" + test.m_name;
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
        const std::stacktrace backtrace = std::stacktrace::current(9);

        std::println(std::cerr, "{}({}): Assertion failed with \"{}\"\n"
                     "\tin function: {}\n"
                     "\tin test: {}\n\n"
                     "Callstack:\n{}",
                     condition.m_site.file_name(), condition.m_site.line(), condition.m_message,
                     condition.m_site.function_name(), getTestId(),
                     backtrace);
        std::cerr.flush();

        throw std::logic_error(condition.m_message);
    }
#endif

    void UnitTest::RunImpl::start() noexcept {
#if PPR_ENABLE_ASSERTIONS
        Assertion::Policy new_policy(std23::nontype<&RunImpl::onAssertFailure>, this);
        m_prev_assert_policy = Assertion::setFailurePolicy(std::move(new_policy));
#endif

        m_start_time = std::chrono::steady_clock::now();
    }

    void UnitTest::RunImpl::stop() noexcept {
        m_end_time = std::chrono::steady_clock::now();
        const TimeSpan test_duration{m_end_time - m_start_time};

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

        std::println(std::cout, " {} {:>3}/{:<3}  {} {:<70} ({})",
                     icon,
                     m_num_passed,
                     m_num_passed + m_num_failed,
                     bullet,
                     test_id,
                     test_duration);

        if (m_status == fail) {
            std::println(std::cout, "    \u2514\u2500 {}", m_failure);
        }
        if (is_group) {
            std::println(std::cout, "-----------------------------------------------------------------------------------------------");
        }

        std::cout.flush();
    }
}
