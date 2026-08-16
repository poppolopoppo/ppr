#pragma once
#include "pP/Macros.h"   // PPR_STRINGIZE / PPR_ANONYMIZE / PPR_ASSUME / PPR_RETURN_ON_FAIL*

// ---- unit-test framework macros (moved from include/pP/Macros.h) ----
#define PPR_ENABLE_UNIT_TESTS 1

#define PPR_UNIT_TEST(_NAME, ...) \
    inline constexpr auto _NAME = \
        pP::UnitTest::Named(PPR_STRINGIZE(_NAME), {__VA_ARGS__}) / \
            []([[maybe_unused]] pP::UnitTest::IRun & _) -> void

// ---- always-evaluating assert (debug + release) ----
#define PPR_TEST_ASSERT(...)                                                            \
    do {                                                                                \
        const bool PPR_ANONYMIZE(predicate) = (__VA_ARGS__);                            \
        if consteval {                                                                  \
            PPR_ASSUME(PPR_ANONYMIZE(predicate));                                       \
        } else {                                                                        \
            static constexpr auto PPR_ANONYMIZE(assertion_site) = std::source_location::current(); \
            if (not PPR_ANONYMIZE(predicate)) [[unlikely]] {                            \
                ::pP::onTestAssertionFailure(PPR_STRINGIZE(__VA_ARGS__), PPR_ANONYMIZE(assertion_site)); \
            }                                                                           \
        }                                                                               \
    } while (0)

// ---- optional error_code-returning test (usable after Phase 1b) ----
#define PPR_UNIT_TEST_ERRC(_NAME, ...)                                                 \
    inline constexpr auto _NAME =                                                      \
        pP::UnitTest::Named(PPR_STRINGIZE(_NAME), {__VA_ARGS__}) /                     \
            []([[maybe_unused]] pP::UnitTest::IRun & _) -> std::error_code