module;
#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;

import std;

#if PPR_ENABLE_ASSERTIONS
namespace pP {
    void Assertion::Handler::defaultAssertFailure_(const Assertion &condition) {
        // Bounded, allocation-free reason record: fixed buffer + format_to_n
        // (no heap, no exceptions). The buffer feeds debug output only; the
        // fatal endpoint below takes no pointers into it.
        char buffer[2048];
        const auto [out, size] = std::format_to_n(
            buffer, sizeof(buffer) - 1,
            "{}({}): {} assert failed: \"{}\"\n"
            "\tin function: {}\n",
            condition.m_site.file_name(), condition.m_site.line(), typeName(condition.m_type), condition.m_message,
            condition.m_site.function_name());
        *out = zero_v;

        hal::outputDebug(buffer);
        hal::breakpointIfDebugging();

        // Fatal by default: the shared fail-fast endpoint captures a WER dump
        // instead of throwing into arbitrary application recovery. The explicit
        // test-policy override (setFailurePolicy, used by the unit-test runner
        // to install a throwing handler) is preserved untouched.
        hal::fatalExit("assert");
    }

    Assertion::Handler &Assertion::Handler::get() noexcept {
        alignas(hal::cacheline_size_v) static Handler g_handler;
        return g_handler;
    }

    Assertion::Policy Assertion::Handler::setFailurePolicy(Policy &&on_assert_failure) noexcept {
        const std::lock_guard guard(m_barrier);
        std::swap(on_assert_failure, m_on_assert_failure);
        return on_assert_failure;
    }

    void Assertion::Handler::onAssertFailure(const Assertion &condition) {
        const std::lock_guard guard(m_barrier);
        return m_on_assert_failure(condition);
    }

    Assertion::Policy Assertion::setFailurePolicy(Policy &&on_assert_failure) noexcept {
        return Handler::get().setFailurePolicy(std::move(on_assert_failure));
    }

    void Assertion::onFailure(const EType type, const char *message, const std::source_location &site) {
        Handler::get().onAssertFailure(Assertion{.m_site = site, .m_message = message, .m_type = type});
    }
}
#endif
