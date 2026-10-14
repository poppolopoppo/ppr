module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // poisoning
    // ------------------------------------------------------------------

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

            // Deterministic retaining store for LocalCache poison-retention
            // probes. Models PagePool bundle retention: freed blocks stay mapped
            // and poisoned until reuse. Vectors hold pointers outside the payload.
            // The store owns every block; caches observe it. Declare the store
            // before the caches so caches flush before the store frees.
            struct RetainingStore {
                static constexpr std::size_t kBlockSize = 64u;

                std::vector<void *> m_owned{};
                std::vector<void *> m_free{};

                RetainingStore() = default;

                RetainingStore(const RetainingStore &) = delete;

                RetainingStore &operator=(const RetainingStore &) = delete;

                ~RetainingStore() noexcept {
                    for (void *const ptr: m_owned) {
                        mem::GPA::deallocateRaw(ptr, kBlockSize, max_align_v);
                    }
                }

                [[nodiscard]] std::allocation_result<void *> allocateRaw(
                    const std::size_t bytes, const std::align_val_t alignment) noexcept {
                    static_cast<void>(bytes);

                    if (!m_free.empty()) {
                        void *const ptr = m_free.back();
                        m_free.pop_back();

                        return {ptr, kBlockSize};
                    }

                    const auto block = mem::GPA::allocateRaw(kBlockSize, alignment);

                    if (block.ptr != nullptr) {
                        m_owned.push_back(block.ptr);
                    }

                    return {block.ptr, block.ptr != nullptr ? kBlockSize : 0u};
                }

                void deallocateRaw(
                    void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
                    static_cast<void>(bytes);
                    static_cast<void>(alignment);

                    m_free.push_back(ptr);
                }
            };

            struct SharedBacking {
                RetainingStore *m_store{nullptr};

                explicit SharedBacking(RetainingStore &store) noexcept : m_store(&store) {
                }

                [[nodiscard]] std::allocation_result<void *> allocateRaw(
                    const std::size_t bytes, const std::align_val_t alignment) noexcept {
                    return m_store->allocateRaw(bytes, alignment);
                }

                void deallocateRaw(
                    void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
                    m_store->deallocateRaw(ptr, bytes, alignment);
                }
            };
        }

        PPR_UNIT_TEST (child_process_without_error, UnitTest::fork) {
            // simple test to make sure the process is not failing when not expected to
        };

        PPR_UNIT_TEST (poison_destroyed_then_read_triggers_asan, UnitTest::expect_crash) {
            std::byte buffer[64u]{};
            mem::poisonDestroyed(buffer, sizeof(buffer));
            details::access_after_poison(buffer);
        };

        PPR_UNIT_TEST (poison_reserved_then_write_triggers_asan, UnitTest::expect_crash) {
            std::byte buffer[64u]{};
            mem::poisonReserved(buffer, sizeof(buffer));
            details::write_after_poison(buffer);
        };

        PPR_UNIT_TEST (poison_nullptr_safe) {
            mem::unpoisonUninitialized(nullptr, 0u);
            mem::poisonDestroyed(nullptr, 0u);
            mem::poisonReserved(nullptr, 0u);
        };

        PPR_UNIT_TEST (gpa_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            const auto [ptr, count] = mem::GPA::allocateRaw(32u, max_align_v);
            mem::GPA::deallocateRaw(ptr, count, max_align_v);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (os_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            if (const auto [ptr, count] = mem::OS::allocateRaw(4096u, std::align_val_t{4096u}); ptr) [[likely]] {
                mem::OS::deallocateRaw(ptr, count, std::align_val_t{4096u});
                details::access_after_poison(ptr);
            }
        };

        PPR_UNIT_TEST (pooling_poison_on_free_triggers_asan, UnitTest::expect_crash) {
            mem::Pooling<64u, mem::GPA, 256u> pool{};
            void *block = pool.allocateRaw(64u, max_align_v).ptr;
            pool.deallocateRaw(block, 64u, max_align_v);
            details::access_after_poison(block);
        };

        PPR_UNIT_TEST (arena_poison_on_dealloc_triggers_asan, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{4096u};
            auto *const alloc = arena.allocateRaw(64u, max_align_v).ptr;
            arena.deallocateRaw(alloc, 64u, max_align_v);
            details::access_after_poison(alloc);
        };

        PPR_UNIT_TEST (in_situ_poison_on_dealloc_triggers_asan, UnitTest::expect_crash) {
            mem::InSitu<128u> buffer{};
            const auto [ptr, count] = buffer.allocateRaw(64u, max_align_v);
            buffer.deallocateRaw(ptr, count, max_align_v);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (stable_vector_asan_on_erase, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.erase(0);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (stable_vector_asan_multi_slice, UnitTest::expect_crash) {
            StableVector<int> sv;
            for (std::size_t i = 0; i < 100; ++i) {
                sv.pushBack(static_cast<int>(i));
            }
            int *ptr = &sv[50];
            sv.erase(50);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (stable_vector_asan_on_clear, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (hash_map_asan_on_clear, UnitTest::expect_crash) {
            HashMap<int, int> hm;
            hm.insert({1, 10});
            auto it = hm.find(1);
            PPR_TEST_ASSERT(it != hm.end());
            int *ptr = &it->second;
            hm.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (sparse_vector_asan_on_erase, UnitTest::expect_crash) {
            SparseVector<int> sv;
            const auto key = sv.add(42);
            int *ptr = &sv[key];
            sv.erase(key);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST (arena_asan_on_restore, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{4096u};
            const void *mark = arena.watermark();
            auto *const alloc = static_cast<std::byte *>(arena.allocateRaw(64u, max_align_v).ptr);
            alloc[0] = std::byte{42};
            arena.restore(mark);
            details::access_after_poison(alloc);
        };

        PPR_UNIT_TEST (arena_cross_slab_restore_triggers_asan, UnitTest::expect_crash) {
            // Exhaust first slab to trigger pushSlab_, then restore past it
            mem::Arena<mem::GPA> arena{128u};
            const void *mark = arena.watermark();
            [[maybe_unused]] const auto f1 = arena.allocateRaw(64u, max_align_v);
            [[maybe_unused]] const auto f2 = arena.allocateRaw(64u, max_align_v);
            auto *const cross_slab = static_cast<std::byte *>(arena.allocateRaw(1u, max_align_v).ptr);
            arena.restore(mark);
            details::access_after_poison(cross_slab);
        };

        PPR_UNIT_TEST (pooling_pool_level_poison, UnitTest::expect_crash) {
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

        PPR_UNIT_TEST (local_cache_cached_block_stays_poisoned, UnitTest::expect_crash) {
            details::RetainingStore store{};
            details::SharedBacking backing{store};

            mem::LocalCache<64u, details::SharedBacking, 2u> cache{backing};

            const auto block = cache.allocateRaw(64u, max_align_v);

            cache.deallocateRaw(block.ptr, block.count, max_align_v);

            // No legitimate reallocation may unpoison before this probe: the
            // block is still parked in the MRU ring.
            details::access_after_poison(block.ptr);
        };

        PPR_UNIT_TEST (local_cache_flushed_block_stays_poisoned, UnitTest::expect_crash) {
            details::RetainingStore store{};
            details::SharedBacking backing{store};

            mem::LocalCache<64u, details::SharedBacking, 2u> cache{backing};

            const auto block = cache.allocateRaw(64u, max_align_v);

            cache.deallocateRaw(block.ptr, block.count, max_align_v);
            cache.shrinkToFit();

            // Block is free in the shared store; no second cache reallocates it
            // before this probe, so poison must still hold.
            details::access_after_poison(block.ptr);
        };

        PPR_UNIT_TEST (local_cache_evicted_block_stays_poisoned, UnitTest::expect_crash) {
            details::RetainingStore store{};
            details::SharedBacking backing{store};

            mem::LocalCache<64u, details::SharedBacking, 2u> cache{backing};

            const auto block_a = cache.allocateRaw(64u, max_align_v);
            const auto block_b = cache.allocateRaw(64u, max_align_v);
            const auto block_c = cache.allocateRaw(64u, max_align_v);

            // Capacity 2 evicts the oldest (a) into the shared store on the
            // third deallocate; a stays free there until this probe.
            cache.deallocateRaw(block_a.ptr, block_a.count, max_align_v);
            cache.deallocateRaw(block_b.ptr, block_b.count, max_align_v);
            cache.deallocateRaw(block_c.ptr, block_c.count, max_align_v);

            details::access_after_poison(block_a.ptr);
        };

        PPR_UNIT_TEST (arena_initial_slab_unused_capacity_triggers_asan, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{4096u};

            const auto live = arena.allocateRaw(64u, max_align_v);
            auto *const bytes = static_cast<std::byte *>(live.ptr);
            bytes[0] = std::byte{0xAB};

            // Probe 256 bytes past the live top: 32 shadow granules inside the
            // 4096-byte backing, 8-aligned, far from both edges.
            const auto *const probe = static_cast<const std::byte *>(arena.watermark()) + 256u;
            details::access_after_poison(const_cast<std::byte *>(probe));
        };

        PPR_UNIT_TEST (arena_grown_slab_unused_capacity_triggers_asan, UnitTest::expect_crash) {
            mem::Arena<mem::GPA> arena{128u};

            [[maybe_unused]] const auto filler = arena.allocateRaw(64u, max_align_v);

            // Exhausts the 128-byte slab and pushes a fresh 128-byte slab.
            const auto live = arena.allocateRaw(64u, max_align_v);
            static_cast<std::byte *>(live.ptr)[0] = std::byte{0xAB};

            // New slab holds ~80 live bytes; +32 stays 16 bytes inside the
            // 128-byte backing, 8-aligned, clear of partial granules.
            const auto *const probe = static_cast<const std::byte *>(arena.watermark()) + 32u;
            details::access_after_poison(const_cast<std::byte *>(probe));
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
                detail::Poisoning::local_cache_cached_block_stays_poisoned,
                detail::Poisoning::local_cache_flushed_block_stays_poisoned,
                detail::Poisoning::local_cache_evicted_block_stays_poisoned,
                detail::Poisoning::arena_initial_slab_unused_capacity_triggers_asan,
                detail::Poisoning::arena_grown_slab_unused_capacity_triggers_asan,
            });
        }
    };

    const UnitTest &poisoningTests() noexcept {
        return poisoning;
    }
} // namespace pP::tests
