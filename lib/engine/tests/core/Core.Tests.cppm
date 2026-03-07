module;
#include "pP/Macros.h"

export module engine.tests;

import engine.core;

import :core_arena;
import :core_containers;
import :core_hash_map;
import :core_enums;
import :core_memory;
import :core_page_pool;
import :core_opaque;
import :core_sparse_vector;
import :core_stable_vector;
import :core_strings;

export namespace pP::tests {
    PPR_UNIT_TEST(containers) {
        _.recurse(relocatable);
        _.recurse(bitmask);
        _.recurse(pointers);
        _.recurse(iterators);
        _.recurse(stack);
        _.recurse(ring_buffer);
        _.recurse(sort);
        _.recurse(hash);
        _.recurse(recycler);
        _.recurse(stableVector);
        _.recurse(sparseVector);
        _.recurse(hashMap);
    };

    PPR_UNIT_TEST(memory) {
        _.recurse(pagePool);
        _.recurse(arena);
        _.recurse(allocator);
    };

    PPR_UNIT_TEST(core) {
        _.recurse(enums);
        _.recurse(memory);
        _.recurse(strings);
        _.recurse(containers);
        _.recurse(opaque);
    };
}
