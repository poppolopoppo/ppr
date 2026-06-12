module;
#include "pP/Macros.h"

module engine.core;

import :unit_test;
import :assert;
import :hal;
import :function_ref;

import std;

namespace pP {

    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

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
        try {
            auto &impl = checked_cast<RunImpl>(run);
            impl.start();

            if ((m_flags & fork) == none) [[likely]] {
                m_run(run);
            } else {
                if (not startInChildProcess_(impl)) {
                    if (not isExpectedToFail()) [[unlikely]] {
                        run.failWith("child process exited with an error");
                        return;
                    }
                } else if (isExpectedToFail()) [[unlikely]] {
                    run.failWith("test succeeded, but it was expected to fail");
                    return;
                }
            }

            run.success();
        } catch (std::exception &e) {
            run.failWith(e.what());
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
        : m_context(context), m_test(test) {
    }

    UnitTest::RunImpl::RunImpl(const Context &context, const UnitTest &test, RunImpl &parent) noexcept
        : m_context(context), m_test(test), m_parent(&parent), m_depth(parent.m_depth + 1u) {
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
            hal::outputDebugFmt("{}: {}\n",
                                std::string_view(m_test.m_name),
                                std::string_view(msg));
        }
    }

    void UnitTest::RunImpl::failWith(const char *msg) {
        if (m_num_failed++ == 0u) {
            m_failure = msg;
            m_status = fail;
        }

        if (m_parent != nullptr) {
            m_parent->failWith(msg);
        } else if (m_context.m_fail_with.has_value()) {
            (*m_context.m_fail_with)(*this, msg);
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

    void UnitTest::RunImpl::success() {
        ++m_num_passed;
        if (m_parent != nullptr) {
            m_parent->success();
        }
    }

#if PPR_ENABLE_ASSERTIONS
    void UnitTest::RunImpl::onAssertFailure(const Assertion &condition) const {
        const std::stacktrace backtrace = std::stacktrace::current(9);

        hal::outputDebugFmt("{}({}): Assertion failed with \"{}\"\n"
                            "\tin function: {}\n"
                            "\tin test: {}\n\n"
                            "Callstack:\n{}\n",
                            std::string_view(condition.m_site.file_name()),
                            condition.m_site.line(),
                            std::string_view(condition.m_message),
                            std::string_view(condition.m_site.function_name()),
                            getTestId(),
                            backtrace);

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

    UnitTest::RunImpl::~RunImpl() {
        const std::chrono::steady_clock::time_point end_time = std::chrono::steady_clock::now();
        const std::chrono::steady_clock::duration duration_from_start = end_time - m_start_time;

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

        hal::outputDebugFmt(" {} {:>3}/{:<3}  {} {:<60} ({})\n",
                            icon,
                            m_num_passed,
                            m_num_passed + m_num_failed,
                            bullet,
                            test_id,
                            TimeDuration{duration_from_start});

        if (m_status == fail) {
            hal::outputDebugFmt("    \u2514\u2500 {}\n", m_failure);
        }
        if (is_group) {
            hal::outputDebug("----------------------------------------------------------------------\n");
        }
    }

}
