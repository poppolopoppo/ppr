module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace SafePtr {
        PPR_UNIT_TEST (null_copy_remains_null) {
            const safe_ptr<safe_object> a{};
            const safe_ptr<safe_object> b{a};
            PPR_TEST_ASSERT(b.get() == nullptr);
        };

        PPR_UNIT_TEST (nullptr_assignment_clears) {
            safe_ptr<safe_object> a{};
            a = nullptr;
            PPR_TEST_ASSERT(!a.isValid());
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest safe_ptr_test = UnitTest::Named("safe_ptr_test") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SafePtr::null_copy_remains_null,
            detail::SafePtr::nullptr_assignment_clears,
        });
    };
} // namespace pP::tests
