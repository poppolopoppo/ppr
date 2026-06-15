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
import :core_utility;
import :core_event;
import :core_context;
import :core_io;

export namespace pP::tests {
    PPR_UNIT_TEST(memory) {
        _.recurse({
            pagePool,
            arena,
            slab,
            allocator,
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
            stableVector,
            sparseVector,
            hashMap,
        });
    };

    PPR_UNIT_TEST(core) {
        _.recurse({
            enums,
            memory,
            strings,
            containers,
            opaque,
            channel,
            event,
            context,
            io,
            utility,
        });
    };
}
