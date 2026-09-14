module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Service {
        PPR_UNIT_TEST (type_uid_identity) {
            const std::type_index id_a = typeid(int);
            const std::type_index id_b = typeid(int);
            PPR_TEST_ASSERT(id_a == id_b);
        };

        PPR_UNIT_TEST (type_uid_unique_types) {
            const std::type_index id_int = typeid(int);
            const std::type_index id_float = typeid(float);
            const std::type_index id_double = typeid(double);
            const std::type_index id_char = typeid(char);
            PPR_TEST_ASSERT(id_int != id_float);
            PPR_TEST_ASSERT(id_int != id_double);
            PPR_TEST_ASSERT(id_float != id_double);
            PPR_TEST_ASSERT(id_int != id_char);
        };

        PPR_UNIT_TEST (type_uid_template_identity) {
            const std::type_index id_a = typeid(std::pair<int, float>);
            const std::type_index id_b = typeid(std::pair<int, float>);
            PPR_TEST_ASSERT(id_a == id_b);
        };

        PPR_UNIT_TEST (type_uid_unique_templates) {
            const std::type_index id_pair = typeid(std::pair<int, float>);
            const std::type_index id_tuple = typeid(std::tuple<int, float>);
            const std::type_index id_vector = typeid(std::vector<int>);
            PPR_TEST_ASSERT(id_pair != id_tuple);
            PPR_TEST_ASSERT(id_pair != id_vector);
            PPR_TEST_ASSERT(id_tuple != id_vector);
        };

        PPR_UNIT_TEST (type_uid_cv_qualified) {
            // [expr.typeid] strips top-level cv-qualifiers and reference-ness:
            // all of these denote the same type_info as int.
            const std::type_index id_int = typeid(int);
            const std::type_index id_const_int = typeid(const int);
            const std::type_index id_volatile_int = typeid(volatile int);
            const std::type_index id_ref = typeid(int &);
            PPR_TEST_ASSERT(id_int == id_const_int);
            PPR_TEST_ASSERT(id_int == id_volatile_int);
            PPR_TEST_ASSERT(id_int == id_ref);
        };

        PPR_UNIT_TEST (type_uid_pointer_types) {
            const std::type_index id_int_ptr = typeid(int *);
            const std::type_index id_float_ptr = typeid(float *);
            const std::type_index id_int_ptr_ptr = typeid(int **);
            PPR_TEST_ASSERT(id_int_ptr != id_float_ptr);
            PPR_TEST_ASSERT(id_int_ptr != id_int_ptr_ptr);
        };
    }

    namespace Service_locator {
        struct MockServiceA : IService {
            int value{};
        };

        struct MockServiceB : IService {
            float fvalue{};
        };

        PPR_UNIT_TEST (empty) {
            ServicesStore loc;
            PPR_TEST_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST (insert_and_try_get) {
            MockServiceA a;
            a.value = 42;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            auto retrieved = loc.tryGet<MockServiceA>();
            PPR_TEST_ASSERT(retrieved.isValid());
            PPR_TEST_ASSERT(retrieved->value == 42);
        };

        PPR_UNIT_TEST (insert_and_get) {
            MockServiceA a;
            a.value = 99;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            auto retrieved = loc.get<MockServiceA>();
            PPR_TEST_ASSERT(retrieved.isValid());
            PPR_TEST_ASSERT(retrieved->value == 99);
        };

        PPR_UNIT_TEST (erase) {
            MockServiceA a;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));
            PPR_TEST_ASSERT(loc.tryGet<MockServiceA>().isValid());

            PPR_TEST_ASSERT(loc.erase<MockServiceA>(a));
            PPR_TEST_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST (erase_nonexistent) {
            MockServiceA a;
            ServicesStore loc;
            PPR_TEST_ASSERT(not loc.erase<MockServiceA>(a));
        };

        PPR_UNIT_TEST (reset) {
            MockServiceA a;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            loc.reset();
            PPR_TEST_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST (duplicate_insert) {
            MockServiceA a1, a2;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a1)));
            PPR_TEST_ASSERT(not loc.insert(safe_ptr<MockServiceA>(&a2)));
        };

        PPR_UNIT_TEST (multi_type_routing) {
            MockServiceA a;
            a.value = 10;
            MockServiceB b;
            b.fvalue = 3.14f;
            ServicesStore loc;
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));
            PPR_TEST_ASSERT(loc.insert(safe_ptr<MockServiceB>(&b)));

            PPR_TEST_ASSERT(loc.tryGet<MockServiceA>().isValid());
            PPR_TEST_ASSERT(loc.tryGet<MockServiceA>()->value == 10);
            PPR_TEST_ASSERT(loc.tryGet<MockServiceB>().isValid());

            const float v = loc.tryGet<MockServiceB>()->fvalue;
            PPR_TEST_ASSERT(v > 3.139f && v < 3.141f);
        };

        PPR_UNIT_TEST (parent_fallback) {
            MockServiceA a;
            a.value = 7;
            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockServiceA>(&a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            auto retrieved = child.tryGet<MockServiceA>();
            PPR_TEST_ASSERT(retrieved.isValid());
            PPR_TEST_ASSERT(retrieved->value == 7);
        };

        PPR_UNIT_TEST (child_override) {
            MockServiceA parent_a, child_a;
            parent_a.value = 1;
            child_a.value = 2;
            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockServiceA>(&parent_a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            PPR_TEST_ASSERT(child.insert(safe_ptr<MockServiceA>(&child_a)));

            auto retrieved = child.tryGet<MockServiceA>();
            PPR_TEST_ASSERT(retrieved.isValid());
            PPR_TEST_ASSERT(retrieved->value == 2);
        };

        PPR_UNIT_TEST (child_erase_does_not_affect_parent) {
            MockServiceA a;
            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockServiceA>(&a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            PPR_TEST_ASSERT(not child.erase<MockServiceA>(a));
            PPR_TEST_ASSERT(parent.tryGet<MockServiceA>().isValid());

            auto retrieved = child.tryGet<MockServiceA>();
            PPR_TEST_ASSERT(retrieved.isValid());
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest type_uid = UnitTest::Named("type_uid") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Service::type_uid_identity,
            detail::Service::type_uid_unique_types,
            detail::Service::type_uid_template_identity,
            detail::Service::type_uid_unique_templates,
            detail::Service::type_uid_cv_qualified,
            detail::Service::type_uid_pointer_types,
        });
    };
    extern const UnitTest service_locator = UnitTest::Named("service_locator") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Service_locator::empty,
            detail::Service_locator::insert_and_try_get,
            detail::Service_locator::insert_and_get,
            detail::Service_locator::erase,
            detail::Service_locator::erase_nonexistent,
            detail::Service_locator::reset,
            detail::Service_locator::duplicate_insert,
            detail::Service_locator::multi_type_routing,
            detail::Service_locator::parent_fallback,
            detail::Service_locator::child_override,
            detail::Service_locator::child_erase_does_not_affect_parent,
        });
    };
    extern const UnitTest service = UnitTest::Named("service") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            type_uid,
            service_locator,
        });
    };
} // namespace pP::tests
