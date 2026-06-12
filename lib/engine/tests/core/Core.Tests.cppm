module;
#include "pP/Macros.h"

export module engine.tests;

import engine.core;

import :core_arena;
import :core_channel;
import :core_containers;
import :core_hash_map;
import :core_enums;
import :core_memory;
import :core_page_pool;
import :core_opaque;
import :core_sparse_vector;
import :core_stable_vector;
import :core_strings;
import :core_event;

export namespace pP::tests {
    PPR_UNIT_TEST(memory) {
        _.recurse(pagePool);
        _.recurse(arena);
        _.recurse(slab);
        _.recurse(allocator);
        _.recurse(poisoning);
    };

    PPR_UNIT_TEST(containers) {
        _.recurse(relocatable);
        _.recurse(hash);
        _.recurse(sort);
        _.recurse(bitmask);
        _.recurse(pointers);
        _.recurse(iterators);
        _.recurse(stack);
        _.recurse(ring_buffer);
        _.recurse(stableVector);
        _.recurse(sparseVector);
        _.recurse(hashMap);
    };

    PPR_UNIT_TEST(core) {
        _.recurse(enums);
        _.recurse(memory);
        _.recurse(strings);
        _.recurse(containers);
        _.recurse(opaque);
        _.recurse(channel);
        _.recurse(event);
    };
}
