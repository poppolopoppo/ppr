module;
#include "pP/UnitTest.h"

export module engine.tests.core;

import engine.core;

import :arena;
import :channel;
import :containers;
import :containers.flat_map;
import :containers.hash_map;
import :enums;
import :hal;
import :memory;
import :memory.page_pool;
import :opaque;
import :containers.sparse_vector;
import :containers.stable_vector;
import :strings;
import :utility;
import :event;
import :context;
import :io;
import :io.file_watcher;
import :math;
import :service;

export namespace pP::tests {
    PPR_UNIT_TEST(memory) {
        _.recurse({
            page_pool,
            arena,
            slab,
            allocator,
            safe_ptr_test,
            poisoning,
        });
    };

    PPR_UNIT_TEST(containers) {
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

    // MSVC 14.51 (VS18 Insiders) ICE workaround (C1001 function-signature.cpp:213):
    // a constexpr `core` umbrella forces every consumer TU to deserialize its full
    // initializer from the IFC, which crashes the frontend once the transitive suite
    // graph is large enough. Declared here and defined in Core.Tests.cpp so consumers
    // only see a declaration. Test set, order, and paths are unchanged.
    extern const UnitTest core;
}
