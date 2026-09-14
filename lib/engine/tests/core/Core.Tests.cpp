module engine.tests.core;

import engine.core;

namespace pP::tests {
    extern const UnitTest enums;
    extern const UnitTest math;
    extern const UnitTest strings;
    extern const UnitTest opaque;
    extern const UnitTest channel;
    extern const UnitTest event;
    extern const UnitTest context;
    extern const UnitTest io;
    extern const UnitTest file_watcher;
    extern const UnitTest hal;
    extern const UnitTest utility;
    extern const UnitTest service;
    extern const UnitTest page_pool;
    extern const UnitTest arena;
    extern const UnitTest slab;
    extern const UnitTest allocator;
    extern const UnitTest safe_ptr_test;
    extern const UnitTest poisoning;
    extern const UnitTest relocatable;
    extern const UnitTest hash;
    extern const UnitTest sort;
    extern const UnitTest bitmask;
    extern const UnitTest pointers;
    extern const UnitTest iterators;
    extern const UnitTest stack;
    extern const UnitTest ring_buffer;
    extern const UnitTest stable_vector;
    extern const UnitTest sparse_vector;
    extern const UnitTest flat_map;
    extern const UnitTest hash_map;
    extern const UnitTest memory = UnitTest::Named("memory") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            page_pool,
            arena,
            slab,
            allocator,
            safe_ptr_test,
            poisoning,
        });
    };
    extern const UnitTest containers = UnitTest::Named("containers") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            relocatable,
            hash,
            sort,
            bitmask,
            pointers,
            iterators,
            stack,
            ring_buffer,
            stable_vector,
            sparse_vector,
            flat_map,
            hash_map,
        });
    };
    // Defined here rather than as an inline constexpr umbrella in Core.Tests.cppm:
    // MSVC 14.51 ICEs (C1001 function-signature.cpp:213) when a consumer TU
    // deserializes this initializer from the IFC. Same test set and order as before.
    extern const UnitTest core = UnitTest::Named("core") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            enums,
            math,
            memory,
            strings,
            containers,
            opaque,
            channel,
            event,
            context,
            io,
            file_watcher,
            hal,
            utility,
            service,
        });
    };
} // namespace pP::tests
