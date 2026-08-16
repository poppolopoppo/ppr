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

export namespace pP::tests {
    PPR_UNIT_TEST(memory) {
        _.recurse({
            pagePool,
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
            stableVector,
            sparseVector,
            flatMap,
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
            file_watcher,
            hal,
            utility,
        });
    };
}
