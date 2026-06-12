module;
#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;

import std;

#if PPR_ENABLE_ASSERTIONS
namespace pP {

    void Assertion::Handler::defaultAssertFailure_(const Assertion &condition) {
        char buffer[2048];
        const auto [out, size] = std::format_to_n(
            buffer, sizeof(buffer) - 1,
            "{}({}): {} assert failed: \"{}\"\n"
            "\tin function: {}\n",
            condition.m_site.file_name(),
            condition.m_site.line(),
            typeName(condition.m_type),
            condition.m_message,
            condition.m_site.function_name());
        *out = zero_v;

        hal::outputDebug(buffer);
        hal::breakpointIfDebugging();

        static volatile bool g_throw_exception = true;
        if (g_throw_exception) {
            throw std::logic_error(buffer);
        }
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

}
#endif
