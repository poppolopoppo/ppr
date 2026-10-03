module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    PPR_DEFINE_LOG_CATEGORY(ThrowProbe, warning, none)

    namespace ThrowPolicy {
        class DerivedException final : public std::runtime_error {
        public:
            using std::runtime_error::runtime_error;
        };

        // C4702: PPR_THROW/PPR_RETHROW always throw, so `reached_end_of_try = true;`
        // below is intentionally unreachable (proves the throw happened).
        PPR_PRAGMA_WARNING_PUSH()
        PPR_PRAGMA_WARNING_DISABLE_MSVC(4702)
        // PPR_THROW must evaluate its operand exactly once, even though the
        // macro wraps the throw in a try/catch that logs the exception.
        PPR_UNIT_TEST (throw_evaluates_operand_once) {
            PPR_DEFER {
                PPR_FLUSH_LOG(true);
            };

            int evaluations = 0;
            auto make = [&]() -> std::runtime_error {
                ++evaluations;
                return std::runtime_error{"single evaluation"};
            };

            bool reached_end_of_try = false;
            bool caught = false;
            try {
                PPR_THROW(ThrowProbe, make());
                reached_end_of_try = true;
            } catch (const std::runtime_error &) {
                caught = true;
            }
            PPR_TEST_ASSERT(not reached_end_of_try);
            PPR_TEST_ASSERT(caught);
            PPR_TEST_ASSERT(evaluations == 1);
        };

        // The dynamic type and what() string must survive the macro's
        // internal catch (const std::exception &) + rethrow.
        PPR_UNIT_TEST (throw_preserves_dynamic_type_and_what) {
            PPR_DEFER {
                PPR_FLUSH_LOG(true);
            };

            constexpr std::string_view kWhat = "derived what-string must survive PPR_THROW";

            bool reached_end_of_try = false;
            bool caught_derived = false;
            std::string observed;
            try {
                PPR_THROW(ThrowProbe, DerivedException{std::string{kWhat}});
                reached_end_of_try = true;
            } catch (const DerivedException &e) {
                caught_derived = true;
                observed = e.what();
            }
            PPR_TEST_ASSERT(not reached_end_of_try);
            PPR_TEST_ASSERT(caught_derived);
            PPR_TEST_ASSERT(observed == kWhat);
        };

        // Rethrowing an active std::exception via the macro must preserve
        // its dynamic type and what() for the outer handler.
        PPR_UNIT_TEST (rethrow_preserves_std_exception) {
            PPR_DEFER {
                PPR_FLUSH_LOG(true);
            };

            constexpr std::string_view kWhat = "std what-string must survive PPR_RETHROW";

            bool reached_end_of_try = false;
            bool caught = false;
            std::string observed;
            try {
                try {
                    PPR_THROW(ThrowProbe, std::runtime_error{std::string{kWhat}});
                } catch (const std::exception &) {
                    PPR_RETHROW(ThrowProbe);
                }
                reached_end_of_try = true;
            } catch (const std::runtime_error &e) {
                caught = true;
                observed = e.what();
            }
            PPR_TEST_ASSERT(not reached_end_of_try);
            PPR_TEST_ASSERT(caught);
            PPR_TEST_ASSERT(observed == kWhat);
        };

        // Rethrowing a foreign (non-std) active exception must deliver the
        // original value intact to the outer handler.
        PPR_UNIT_TEST (rethrow_preserves_foreign_exception) {
            PPR_DEFER {
                PPR_FLUSH_LOG(true);
            };

            constexpr int kValue = 42;

            bool reached_end_of_try = false;
            bool caught = false;
            int observed = 0;
            try {
                try {
                    throw kValue;
                } catch (...) {
                    PPR_RETHROW(ThrowProbe);
                }
                reached_end_of_try = true;
            } catch (int v) {
                caught = true;
                observed = v;
            }
            PPR_TEST_ASSERT(not reached_end_of_try);
            PPR_TEST_ASSERT(caught);
            PPR_TEST_ASSERT(observed == kValue);
        };
        PPR_PRAGMA_WARNING_POP()

        // Logger severity must not decide verdicts: a passing test that
        // emits (and flushes) an error-level log still passes. Failures come
        // only from explicit paths (PPR_TEST_ASSERT, unexpected escapes).
        PPR_UNIT_TEST (error_log_does_not_decide_verdict) {
            PPR_LOG(ThrowProbe, error, "intentional error-level log; a passing test must stay passing");
            PPR_FLUSH_LOG(true);
            PPR_TEST_ASSERT(true);
        };

        // Explicit failures must still fail: this test deliberately fails an
        // assertion and is marked expect_fail, so the runner inverts it.
        // NOTE: the failure's own throw-log entry cannot be flushed (the
        // assertion throw is [[noreturn]]); the pre-flush below only drains
        // entries queued before the intentional failure.
        PPR_UNIT_TEST (explicit_assert_failure_still_fails, UnitTest::expect_fail) {
            PPR_FLUSH_LOG(true);
            PPR_TEST_ASSERT(false);
        };

        // Cold-logger ring-buffer regression (documents the RingBuffer
        // exclusion from PPR_THROW logging): an absurd-but-aligned
        // ringBufferAlloc size must surface as a catchable std::exception,
        // not a hang or crash. Runs forked so a fail-fast would only fail
        // this test. The size stays aligned so the PPR_ASSERT in
        // ringBufferAlloc cannot fire; only the allocation itself may fail.
        PPR_UNIT_TEST (absurd_ring_buffer_alloc_throws_catchable, UnitTest::fork) {
            const std::size_t huge = alignForward(1ULL << 47, hal::page_granularity);
            PPR_TEST_ASSERT(alignForward(huge, hal::page_granularity) == huge);

            bool threw = false;
            try {
                void *const p = hal::ringBufferAlloc(huge);
                hal::ringBufferFree(p, huge);
            } catch (const std::exception &) {
                threw = true;
            }
            PPR_TEST_ASSERT(threw);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest error = UnitTest::Named("error") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::ThrowPolicy::throw_evaluates_operand_once,
            detail::ThrowPolicy::throw_preserves_dynamic_type_and_what,
            detail::ThrowPolicy::rethrow_preserves_std_exception,
            detail::ThrowPolicy::rethrow_preserves_foreign_exception,
            detail::ThrowPolicy::error_log_does_not_decide_verdict,
            detail::ThrowPolicy::explicit_assert_failure_still_fails,
            detail::ThrowPolicy::absurd_ring_buffer_alloc_throws_catchable,
        });
    };

    const UnitTest &errorTests() noexcept {
        return error;
    }
} // namespace pP::tests
