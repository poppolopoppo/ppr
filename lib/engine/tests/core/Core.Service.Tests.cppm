module;
#include "pP/Macros.h"

export module engine.tests.core:service;

import std;
import engine.core;

export namespace pP::tests {
    namespace Service {
        PPR_UNIT_TEST(type_uid_identity) {
            constexpr pP::hash_t id_a = typeUid<int>();
            constexpr pP::hash_t id_b = typeUid<int>();
            PPR_ASSERT(id_a == id_b);
        };

        PPR_UNIT_TEST(type_uid_unique_types) {
            constexpr pP::hash_t id_int = typeUid<int>();
            constexpr pP::hash_t id_float = typeUid<float>();
            constexpr pP::hash_t id_double = typeUid<double>();
            constexpr pP::hash_t id_char = typeUid<char>();
            PPR_ASSERT(id_int != id_float);
            PPR_ASSERT(id_int != id_double);
            PPR_ASSERT(id_float != id_double);
            PPR_ASSERT(id_int != id_char);
        };

        PPR_UNIT_TEST(type_uid_template_identity) {
            constexpr pP::hash_t id_a = typeUid<std::pair<int, float>>();
            constexpr pP::hash_t id_b = typeUid<std::pair<int, float>>();
            PPR_ASSERT(id_a == id_b);
        };

        PPR_UNIT_TEST(type_uid_unique_templates) {
            constexpr pP::hash_t id_pair = typeUid<std::pair<int, float>>();
            constexpr pP::hash_t id_tuple = typeUid<std::tuple<int, float>>();
            constexpr pP::hash_t id_vector = typeUid<std::vector<int>>();
            PPR_ASSERT(id_pair != id_tuple);
            PPR_ASSERT(id_pair != id_vector);
            PPR_ASSERT(id_tuple != id_vector);
        };

        PPR_UNIT_TEST(type_uid_cv_qualified) {
            constexpr pP::hash_t id_int = typeUid<int>();
            constexpr pP::hash_t id_const_int = typeUid<const int>();
            constexpr pP::hash_t id_volatile_int = typeUid<volatile int>();
            constexpr pP::hash_t id_ref = typeUid<int&>();
            PPR_ASSERT(id_int != id_const_int);
            PPR_ASSERT(id_int != id_volatile_int);
            PPR_ASSERT(id_int != id_ref);
        };

        PPR_UNIT_TEST(type_uid_pointer_types) {
            constexpr pP::hash_t id_int_ptr = typeUid<int*>();
            constexpr pP::hash_t id_float_ptr = typeUid<float*>();
            constexpr pP::hash_t id_int_ptr_ptr = typeUid<int**>();
            PPR_ASSERT(id_int_ptr != id_float_ptr);
            PPR_ASSERT(id_int_ptr != id_int_ptr_ptr);
        };
    }

    PPR_UNIT_TEST(type_uid) {
        _.recurse({
            Service::type_uid_identity,
            Service::type_uid_unique_types,
            Service::type_uid_template_identity,
            Service::type_uid_unique_templates,
            Service::type_uid_cv_qualified,
            Service::type_uid_pointer_types,
        });
    };

    namespace ServiceLocatorTests {
        struct MockServiceA : IService {
            int value{};
        };

        struct MockServiceB : IService {
            float fvalue{};
        };

        PPR_UNIT_TEST(empty) {
            ServicesStore loc;
            PPR_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST(insert_and_try_get) {
            MockServiceA a;
            a.value = 42;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            auto retrieved = loc.tryGet<MockServiceA>();
            PPR_ASSERT(retrieved.isValid());
            PPR_ASSERT(retrieved->value == 42);
        };

        PPR_UNIT_TEST(insert_and_get) {
            MockServiceA a;
            a.value = 99;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            auto retrieved = loc.get<MockServiceA>();
            PPR_ASSERT(retrieved.isValid());
            PPR_ASSERT(retrieved->value == 99);
        };

        PPR_UNIT_TEST(erase) {
            MockServiceA a;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));
            PPR_ASSERT(loc.tryGet<MockServiceA>().isValid());

            PPR_ASSERT(loc.erase<MockServiceA>());
            PPR_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST(erase_nonexistent) {
            ServicesStore loc;
            PPR_ASSERT(not loc.erase<MockServiceA>());
        };

        PPR_UNIT_TEST(reset) {
            MockServiceA a;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));

            loc.reset();
            PPR_ASSERT(not loc.tryGet<MockServiceA>().isValid());
        };

        PPR_UNIT_TEST(duplicate_insert) {
            MockServiceA a1, a2;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a1)));
            PPR_ASSERT(not loc.insert(safe_ptr<MockServiceA>(&a2)));
        };

        PPR_UNIT_TEST(multi_type_routing) {
            MockServiceA a;
            a.value = 10;
            MockServiceB b;
            b.fvalue = 3.14f;
            ServicesStore loc;
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceA>(&a)));
            PPR_ASSERT(loc.insert(safe_ptr<MockServiceB>(&b)));

            PPR_ASSERT(loc.tryGet<MockServiceA>().isValid());
            PPR_ASSERT(loc.tryGet<MockServiceA>()->value == 10);
            PPR_ASSERT(loc.tryGet<MockServiceB>().isValid());

            const float v = loc.tryGet<MockServiceB>()->fvalue;
            PPR_ASSERT(v > 3.139f && v < 3.141f);
        };

        PPR_UNIT_TEST(parent_fallback) {
            MockServiceA a;
            a.value = 7;
            ServicesStore parent;
            PPR_ASSERT(parent.insert(safe_ptr<MockServiceA>(&a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            auto retrieved = child.tryGet<MockServiceA>();
            PPR_ASSERT(retrieved.isValid());
            PPR_ASSERT(retrieved->value == 7);
        };

        PPR_UNIT_TEST(child_override) {
            MockServiceA parent_a, child_a;
            parent_a.value = 1;
            child_a.value = 2;
            ServicesStore parent;
            PPR_ASSERT(parent.insert(safe_ptr<MockServiceA>(&parent_a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            PPR_ASSERT(child.insert(safe_ptr<MockServiceA>(&child_a)));

            auto retrieved = child.tryGet<MockServiceA>();
            PPR_ASSERT(retrieved.isValid());
            PPR_ASSERT(retrieved->value == 2);
        };

        PPR_UNIT_TEST(child_erase_does_not_affect_parent) {
            MockServiceA a;
            ServicesStore parent;
            PPR_ASSERT(parent.insert(safe_ptr<MockServiceA>(&a)));

            ServicesStore child{safe_ptr<ServicesStore>(&parent)};
            PPR_ASSERT(not child.erase<MockServiceA>());
            PPR_ASSERT(parent.tryGet<MockServiceA>().isValid());

            auto retrieved = child.tryGet<MockServiceA>();
            PPR_ASSERT(retrieved.isValid());
        };
    }

    PPR_UNIT_TEST(serviceLocator) {
        _.recurse({
            ServiceLocatorTests::empty,
            ServiceLocatorTests::insert_and_try_get,
            ServiceLocatorTests::insert_and_get,
            ServiceLocatorTests::erase,
            ServiceLocatorTests::erase_nonexistent,
            ServiceLocatorTests::reset,
            ServiceLocatorTests::duplicate_insert,
            ServiceLocatorTests::multi_type_routing,
            ServiceLocatorTests::parent_fallback,
            ServiceLocatorTests::child_override,
            ServiceLocatorTests::child_erase_does_not_affect_parent,
        });
    };
}
