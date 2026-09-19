module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Poisoning {
        namespace details {
            [[maybe_unused]] volatile int sink = 0;

            [[maybe_unused]] void access_after_poison(void *ptr) {
                const auto *byte_ptr = static_cast<std::byte *>(ptr);
                sink = byte_ptr[0] != std::byte{0};
            }

            [[maybe_unused]] void write_after_poison(void *ptr) {
                auto *byte_ptr = static_cast<std::byte *>(ptr);
                byte_ptr[0] = std::byte{0xAB};
            }
        }

        PPR_UNIT_TEST(child_process_without_error, UnitTest::fork) {
            // simple test to make sure the process is not failing when not expected to
        };

        PPR_UNIT_TEST(poison_destroyed_then_read_triggers_asan, UnitTest::expect_crash) {
            std::byte buffer[64u]{};
            mem::poisonDestroyed(buffer, sizeof(buffer));
            details::access_after_poison(buffer);
        };

        PPR_UNIT_TEST(poison_reserved_then_write_triggers_asan, UnitTest::expect_crash) {
            std::byte buffer[64u]{};
            mem::poisonReserved(buffer, sizeof(buffer));
            details::write_after_poison(buffer);
        };

        PPR_UNIT_TEST (poison_nullptr_safe) {
            mem::unpoisonUninitialized(nullptr, 0u);
            mem::poisonDestroyed(nullptr, 0u);
            mem::poisonReserved(nullptr, 0u);
        };

        PPR_UNIT_TEST(gpa_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            const auto [ptr, count] = mem::GPA::allocateRaw(32u, max_align_v);
            mem::GPA::deallocateRaw(ptr, count, max_align_v);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(os_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            if (const auto [ptr, count] = mem::OS::allocateRaw(4096u, std::align_val_t{4096u}); ptr) [[likely]] {
                mem::OS::deallocateRaw(ptr, count, std::align_val_t{4096u});
                details::access_after_poison(ptr);
            }
        };

        PPR_UNIT_TEST(pooling_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            mem::Pooling<64u, mem::GPA, 256u> pool{};
            void *block = pool.allocateRaw(64u, max_align_v).ptr;
            pool.deallocateRaw(block, 64u, max_align_v);
            details::access_after_poison(block);
        };

        PPR_UNIT_TEST(arena_poison_on_dealloc_triggers_asan, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{4096u};
            auto *const alloc = arena.allocateRaw(64u, max_align_v).ptr;
            arena.deallocateRaw(alloc, 64u, max_align_v);
            details::access_after_poison(alloc);
        };

        PPR_UNIT_TEST(in_situ_poison_on_dealloc_triggers_asan, UnitTest::expect_crash) {
            mem::InSitu<128u> buffer{};
            const auto [ptr, count] = buffer.allocateRaw(64u, max_align_v);
            buffer.deallocateRaw(ptr, count, max_align_v);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stable_vector_asan_on_erase, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.erase(0);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stable_vector_asan_multi_slice, UnitTest::expect_crash) {
            StableVector<int> sv;
            for (std::size_t i = 0; i < 100; ++i) {
                sv.pushBack(static_cast<int>(i));
            }
            int *ptr = &sv[50];
            sv.erase(50);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stable_vector_asan_on_clear, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(hash_map_asan_on_clear, UnitTest::expect_crash) {
            HashMap<int, int> hm;
            hm.insert({1, 10});
            auto it = hm.find(1);
            PPR_TEST_ASSERT(it != hm.end());
            int *ptr = &it->second;
            hm.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(sparse_vector_asan_on_erase, UnitTest::expect_crash) {
            SparseVector<int> sv;
            const auto key = sv.add(42);
            int *ptr = &sv[key];
            sv.erase(key);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(arena_asan_on_restore, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{4096u};
            const void *mark = arena.watermark();
            auto *const alloc = static_cast<std::byte *>(arena.allocateRaw(64u, max_align_v).ptr);
            alloc[0] = std::byte{42};
            arena.restore(mark);
            details::access_after_poison(alloc);
        };

        PPR_UNIT_TEST(arena_cross_slab_restore_triggers_asan, UnitTest::expect_crash) {
            // Exhaust first slab to trigger pushSlab_, then restore past it
            mem::Arena<mem::GPA> arena{128u};
            const void *mark = arena.watermark();
            [[maybe_unused]] const auto f1 = arena.allocateRaw(64u, max_align_v);
            [[maybe_unused]] const auto f2 = arena.allocateRaw(64u, max_align_v);
            auto *const cross_slab = static_cast<std::byte *>(arena.allocateRaw(1u, max_align_v).ptr);
            arena.restore(mark);
            details::access_after_poison(cross_slab);
        };

        PPR_UNIT_TEST(pooling_pool_level_poison, UnitTest::expect_crash) {
            mem::Pooling<64u, mem::GPA, 128u> pool;
            std::byte *blocks[128];
            for (std::size_t i = 0u; i < 128u; ++i) {
                blocks[i] = static_cast<std::byte *>(pool.allocateRaw(64u, max_align_v).ptr);
            }
            for (std::size_t i = 0u; i < 64u; ++i) {
                pool.deallocateRaw(blocks[i], 64u, max_align_v);
            }
            for (std::size_t i = 64u; i < 127u; ++i) {
                pool.deallocateRaw(blocks[i], 64u, max_align_v);
            }
            pool.deallocateRaw(blocks[127], 64u, max_align_v);
            details::access_after_poison(blocks[64]);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest poisoning = UnitTest::Named("poisoning") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Poisoning::child_process_without_error,
        });
        if constexpr (mem::is_asan_enabled_v) {
            _.recurse({
                detail::Poisoning::poison_destroyed_then_read_triggers_asan,
                detail::Poisoning::poison_reserved_then_write_triggers_asan,
                detail::Poisoning::poison_nullptr_safe,
                detail::Poisoning::gpa_poison_on_free_triggers_asan,
                detail::Poisoning::os_poison_on_free_triggers_asan,
                detail::Poisoning::pooling_poison_on_free_triggers_asan,
                detail::Poisoning::arena_poison_on_dealloc_triggers_asan,
                detail::Poisoning::in_situ_poison_on_dealloc_triggers_asan,
                detail::Poisoning::stable_vector_asan_on_erase,
                detail::Poisoning::stable_vector_asan_multi_slice,
                detail::Poisoning::stable_vector_asan_on_clear,
                detail::Poisoning::hash_map_asan_on_clear,
                detail::Poisoning::sparse_vector_asan_on_erase,
                detail::Poisoning::arena_asan_on_restore,
                detail::Poisoning::arena_cross_slab_restore_triggers_asan,
                detail::Poisoning::pooling_pool_level_poison,
            });
        }
    };

    const UnitTest &poisoningTests() noexcept {
        return poisoning;
    }
} // namespace pP::tests
