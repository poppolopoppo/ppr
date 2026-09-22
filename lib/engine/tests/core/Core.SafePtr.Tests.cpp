module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace SafePtr {
#if PPR_ENABLE_DEBUG
        static_assert(not pP::details::is_relocatable_v<safe_ptr<safe_object> >);
#else
        static_assert(pP::details::is_relocatable_v<safe_ptr<safe_object> >);
        static_assert(sizeof(safe_ptr<safe_object>) == sizeof(safe_object *));
#endif

        struct TestBase : public safe_object {
            int value{0};

            explicit TestBase(const int v = 0) : value(v) {
            }

            virtual ~TestBase() = default;

            TestBase(const TestBase &) = default;
            TestBase(TestBase &&)      = default;

            TestBase &operator=(const TestBase &) = default;
            TestBase &operator=(TestBase &&)      = default;
        };

        struct TestDerived : public TestBase {
            int extra{0};

            explicit TestDerived(const int v = 0, const int e = 0) : TestBase(v), extra(e) {
            }
        };

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

        PPR_UNIT_TEST (default_is_null) {
            const safe_ptr<TestBase> p{};
            PPR_TEST_ASSERT(p.get() == nullptr);
            PPR_TEST_ASSERT(!p.isValid());
            PPR_TEST_ASSERT(!static_cast<bool>(p));
        };

        PPR_UNIT_TEST (raw_ctor_observes_and_accessors) {
            TestBase obj{7};
            const safe_ptr<TestBase> p{&obj};
            PPR_TEST_ASSERT(p.get() == &obj);
            PPR_TEST_ASSERT(p.isValid());
            PPR_TEST_ASSERT(static_cast<bool>(p));
            PPR_TEST_ASSERT(p->value == 7);
            PPR_TEST_ASSERT((*p).value == 7);
        };

        PPR_UNIT_TEST (unique_ptr_ctor_observes) {
            auto owner = std::make_unique<TestBase>(5);
            safe_ptr<TestBase> p{owner};
            PPR_TEST_ASSERT(p.get() == owner.get());
            PPR_TEST_ASSERT(p->value == 5);
        };

        PPR_UNIT_TEST (copy_shares_observation) {
            TestBase obj{1};
            const safe_ptr<TestBase> a{&obj};
            const safe_ptr<TestBase> b{a};
            PPR_TEST_ASSERT(b.get() == &obj);
            PPR_TEST_ASSERT(a == b);
        };

        PPR_UNIT_TEST (move_transfers_and_clears_source) {
            TestBase obj{2};
            safe_ptr<TestBase> a{&obj};
            safe_ptr<TestBase> b{std::move(a)};
            PPR_TEST_ASSERT(a.get() == nullptr);
            PPR_TEST_ASSERT(!a.isValid());
            PPR_TEST_ASSERT(b.get() == &obj);
            b.reset();
        };

        PPR_UNIT_TEST (converting_derived_to_base_copy) {
            TestDerived obj{3, 4};
            const safe_ptr<TestDerived> d{&obj};
            const safe_ptr<TestBase> b{d};
            PPR_TEST_ASSERT(b.get() == &obj);
            PPR_TEST_ASSERT(b->value == 3);
        };

        PPR_UNIT_TEST (converting_move_derived_to_base) {
            TestDerived obj{3, 4};
            safe_ptr<TestDerived> d{&obj};
            safe_ptr<TestBase> b{std::move(d)};
            PPR_TEST_ASSERT(d.get() == nullptr);
            PPR_TEST_ASSERT(b.get() == &obj);
            b.reset();
        };

        PPR_UNIT_TEST (converting_copy_assign_derived_to_base) {
            TestDerived obj{3, 4};
            const safe_ptr<TestDerived> d{&obj};
            safe_ptr<TestBase> b{};
            b = d;
            PPR_TEST_ASSERT(b.get() == &obj);
            b.reset();
        };

        PPR_UNIT_TEST (raw_reassign_releases_old) {
            TestBase a{1};
            TestBase b{2};
            safe_ptr<TestBase> p{&a};
            PPR_TEST_ASSERT(p.get() == &a);
            p = &b;
            PPR_TEST_ASSERT(p.get() == &b);
            PPR_TEST_ASSERT(p->value == 2);
            p.reset();
        };

        PPR_UNIT_TEST (reset_to_null_and_to_new) {
            TestBase a{1};
            TestBase b{2};
            safe_ptr<TestBase> p{&a};
            p.reset();
            PPR_TEST_ASSERT(!p.isValid());
            p.reset(&b);
            PPR_TEST_ASSERT(p.get() == &b);
            p.reset(nullptr);
            PPR_TEST_ASSERT(p.get() == nullptr);
        };

        PPR_UNIT_TEST (reset_same_pointer_stays_valid) {
            TestBase obj{9};
            safe_ptr<TestBase> p{&obj};
            p.reset(&obj);
            PPR_TEST_ASSERT(p.get() == &obj);
            PPR_TEST_ASSERT(p.isValid());
            p.reset();
        };

        PPR_UNIT_TEST (self_copy_assign_noop) {
            TestBase obj{1};
            safe_ptr<TestBase> p{&obj};
            p = p;
            PPR_TEST_ASSERT(p.get() == &obj);
            p.reset();
        };

        PPR_UNIT_TEST (self_move_assign_noop) {
            TestBase obj{1};
            safe_ptr<TestBase> p{&obj};
            p = std::move(p);
            PPR_TEST_ASSERT(p.get() == &obj);
            p.reset();
        };

        PPR_UNIT_TEST (upcast_consumes_source) {
            TestDerived obj{3, 4};
            safe_ptr<TestDerived> d{&obj};
            safe_ptr<TestBase> b = std::move(d).upcast<TestBase>();
            PPR_TEST_ASSERT(!d.isValid());
            PPR_TEST_ASSERT(d.get() == nullptr);
            PPR_TEST_ASSERT(b.get() == &obj);
            PPR_TEST_ASSERT(b->value == 3);
            b.reset();
        };

        PPR_UNIT_TEST (upcast_null_yields_null) {
            safe_ptr<TestBase> b = safe_ptr<TestDerived>{}.upcast<TestBase>();
            PPR_TEST_ASSERT(!b.isValid());
            PPR_TEST_ASSERT(b.get() == nullptr);
        };

        PPR_UNIT_TEST (checked_cast_valid_downcast) {
            TestDerived obj{3, 4};
            const safe_ptr<TestBase> b{static_cast<TestBase *>(&obj)};
            const safe_ptr<TestDerived> d = checked_cast<TestDerived>(b);
            PPR_TEST_ASSERT(d.get() == &obj);
            PPR_TEST_ASSERT(d->extra == 4);
        };

        PPR_UNIT_TEST (checked_cast_null_propagates) {
            const safe_ptr<TestBase> b{};
            const safe_ptr<TestDerived> d = checked_cast<TestDerived>(b);
            PPR_TEST_ASSERT(d.get() == nullptr);
        };

        PPR_UNIT_TEST (swap_exchanges) {
            TestBase a{1};
            TestBase b{2};
            safe_ptr<TestBase> p{&a};
            safe_ptr<TestBase> q{&b};
            swap(p, q);
            PPR_TEST_ASSERT(p.get() == &b);
            PPR_TEST_ASSERT(q.get() == &a);
            p.reset();
            q.reset();
        };

        PPR_UNIT_TEST (equality_and_ordering) {
            TestBase a{1};
            TestBase b{2};
            const safe_ptr<TestBase> p1{&a};
            const safe_ptr<TestBase> p2{&a};
            const safe_ptr<TestBase> p3{&b};
            PPR_TEST_ASSERT(p1 == p2);
            PPR_TEST_ASSERT(p1 == &a);
            PPR_TEST_ASSERT(&a == p1);
            PPR_TEST_ASSERT(!(p1 == p3));
            PPR_TEST_ASSERT((p1 <=> p2) == std::strong_ordering::equal);
            PPR_TEST_ASSERT((p1 <=> p3) == (&a <=> &b));
            PPR_TEST_ASSERT((p1 <=> &b) == (&a <=> &b));
        };

        PPR_UNIT_TEST (destroy_after_reset_is_clean) {
            TestBase obj{7};
            {
                safe_ptr<TestBase> p{&obj};
                PPR_TEST_ASSERT(p.isValid());
                p.reset();
                PPR_TEST_ASSERT(!p.isValid());
            }
            PPR_TEST_ASSERT(obj.value == 7);
        };

#if PPR_ENABLE_DEBUG
        // Guard probes — expect_crash (fail-fast, forked child).
        // Native facility is UnitTest::expect_crash (= expect_fail | fork):
        // plain expect_fail only catches C++ exceptions in-process, while these
        // paths terminate the process (PPR_ASSERT inside noexcept functions —
        // safe_ptr::operator*/operator-> and checked_cast — cannot throw, so the
        // assertion escalates to terminate/abort), so they must run forked.
        // If the impl is fixed the test will exit 0 and the runner will fail
        // with "expected to fail", signalling the probe should be revisited.
        PPR_UNIT_TEST (null_deref_star_triggers_assert, UnitTest::expect_crash) {
            const safe_ptr<TestBase> p{};
            (void)(*p).value;
        };

        PPR_UNIT_TEST (null_deref_arrow_triggers_assert, UnitTest::expect_crash) {
            const safe_ptr<TestBase> p{};
            (void)p->value;
        };

        PPR_UNIT_TEST (copy_ctor_while_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            const safe_ptr<TestBase> obs{&src};
            const TestBase dst{src};
        };

        PPR_UNIT_TEST (copy_assign_target_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            TestBase dst{2};
            const safe_ptr<TestBase> obs{&dst};
            dst = src;
        };

        PPR_UNIT_TEST (copy_assign_source_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            TestBase dst{2};
            const safe_ptr<TestBase> obs{&src};
            dst = src;
        };

        PPR_UNIT_TEST (move_ctor_while_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            const safe_ptr<TestBase> obs{&src};
            TestBase dst{std::move(src)};
        };

        PPR_UNIT_TEST (move_assign_target_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            TestBase dst{2};
            const safe_ptr<TestBase> obs{&dst};
            dst = std::move(src);
        };

        PPR_UNIT_TEST (move_assign_source_observed_triggers_assert, UnitTest::expect_fail) {
            TestBase src{1};
            TestBase dst{2};
            const safe_ptr<TestBase> obs{&src};
            dst = std::move(src);
        };

        PPR_UNIT_TEST (checked_cast_mismatch_triggers_assert, UnitTest::expect_crash) {
            TestBase base{1};
            const safe_ptr<TestBase> p{&base};
            const safe_ptr<TestDerived> d = checked_cast<TestDerived>(p);
        };
#else
        PPR_UNIT_TEST (checked_cast_mismatch_returns_null) {
            TestBase base{1};
            const safe_ptr<TestBase> p{&base};
            const safe_ptr<TestDerived> d = checked_cast<TestDerived>(p);
            PPR_TEST_ASSERT(d.get() == nullptr);
        };
#endif

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
        PPR_UNIT_TEST (tracking_move_survives_source_scope) {
            TestBase obj{11};
            safe_ptr<TestBase> moved{};
            {
                safe_ptr<TestBase> p{&obj};
                moved = std::move(p);
                PPR_TEST_ASSERT(p.get() == nullptr);
                PPR_TEST_ASSERT(moved.get() == &obj);
            }
            PPR_TEST_ASSERT(moved->value == 11);
            moved.reset();
        };

        PPR_UNIT_TEST (tracking_swap_observed_then_teardown_clean) {
            TestBase a{1};
            TestBase b{2};
            safe_ptr<TestBase> p{&a};
            safe_ptr<TestBase> q{&b};
            swap(p, q);
            PPR_TEST_ASSERT(p.get() == &b);
            PPR_TEST_ASSERT(q.get() == &a);
            q.reset();
            PPR_TEST_ASSERT(p->value == 2);
            p.reset();
        };
#endif
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest safe_ptr_test = UnitTest::Named("safe_ptr_test") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SafePtr::null_copy_remains_null,
            detail::SafePtr::nullptr_assignment_clears,
            detail::SafePtr::default_is_null,
            detail::SafePtr::raw_ctor_observes_and_accessors,
            detail::SafePtr::unique_ptr_ctor_observes,
            detail::SafePtr::copy_shares_observation,
            detail::SafePtr::move_transfers_and_clears_source,
            detail::SafePtr::converting_derived_to_base_copy,
            detail::SafePtr::converting_move_derived_to_base,
            detail::SafePtr::converting_copy_assign_derived_to_base,
            detail::SafePtr::raw_reassign_releases_old,
            detail::SafePtr::reset_to_null_and_to_new,
            detail::SafePtr::reset_same_pointer_stays_valid,
            detail::SafePtr::self_copy_assign_noop,
            detail::SafePtr::self_move_assign_noop,
            detail::SafePtr::upcast_consumes_source,
            detail::SafePtr::upcast_null_yields_null,
            detail::SafePtr::checked_cast_valid_downcast,
            detail::SafePtr::checked_cast_null_propagates,
            detail::SafePtr::swap_exchanges,
            detail::SafePtr::equality_and_ordering,
            detail::SafePtr::destroy_after_reset_is_clean,
#if PPR_ENABLE_DEBUG
            detail::SafePtr::null_deref_star_triggers_assert,
            detail::SafePtr::null_deref_arrow_triggers_assert,
            detail::SafePtr::copy_ctor_while_observed_triggers_assert,
            detail::SafePtr::copy_assign_target_observed_triggers_assert,
            detail::SafePtr::copy_assign_source_observed_triggers_assert,
            detail::SafePtr::move_ctor_while_observed_triggers_assert,
            detail::SafePtr::move_assign_target_observed_triggers_assert,
            detail::SafePtr::move_assign_source_observed_triggers_assert,
            detail::SafePtr::checked_cast_mismatch_triggers_assert,
#else
            detail::SafePtr::checked_cast_mismatch_returns_null,
#endif
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
            detail::SafePtr::tracking_move_survives_source_scope,
            detail::SafePtr::tracking_swap_observed_then_teardown_clean,
#endif
        });
    };

    const UnitTest &safe_ptr_testTests() noexcept {
        return safe_ptr_test;
    }
} // namespace pP::tests
