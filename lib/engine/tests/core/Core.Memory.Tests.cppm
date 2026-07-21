module;
#include "pP/Macros.h"
export module engine.tests.core:memory;
import engine.core;
import std;

export namespace pP::tests {
    namespace Allocator {
        struct Widget {
            inline static std::size_t destroyed = 0u;

            int x{};

            explicit Widget(const int v) noexcept : x(v) {
            }

            Widget(const Widget &) = default;

            Widget(Widget &&) noexcept = default;

            Widget &operator =(const Widget &) = default;

            Widget &operator =(Widget &&) noexcept = default;

            ~Widget() noexcept {
                ++destroyed;
            }
        };

        struct RecordingAllocator {
            struct Block {
                void *ptr{};
                std::size_t bytes{};
                std::align_val_t alignment{max_align_v};
            };

            std::vector<Block> blocks{};
            mutable std::size_t watermark_token{0u};

            mutable std::size_t allocate_calls{0u};
            mutable std::size_t deallocate_calls{0u};
            mutable std::size_t resize_calls{0u};
            mutable std::size_t owns_calls{0u};
            mutable std::size_t watermark_calls{0u};
            mutable std::size_t restore_calls{0u};
            mutable std::size_t reset_calls{0u};

            [[nodiscard]] std::allocation_result<void *>
            allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
                ++allocate_calls;

                void *const ptr = (alignment > max_align_v
                                       ? ::operator new(bytes, alignment, std::nothrow)
                                       : ::operator new(bytes, std::nothrow));
                if (ptr) {
                    blocks.push_back(Block{ptr, bytes, alignment});
                }
                return {ptr, ptr ? bytes : 0u};
            }

            static void freeBlock(const Block &block) noexcept {
                if (block.alignment > max_align_v) {
                    ::operator delete(block.ptr, block.bytes, block.alignment);
                } else {
                    ::operator delete(block.ptr, block.bytes);
                }
            }

            void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
                ++deallocate_calls;

                const auto it = std::ranges::find_if(blocks, [ptr](const Block &block) {
                    return block.ptr == ptr;
                });

                if (it != blocks.end()) {
                    const Block block = *it;
                    blocks.erase(it);
                    freeBlock(block);
                    return;
                }

                if (alignment > max_align_v) {
                    ::operator delete(ptr, bytes, alignment);
                } else {
                    ::operator delete(ptr, bytes);
                }
            }

            [[nodiscard]] bool owns(const void *const ptr, const std::size_t size) const noexcept {
                ++owns_calls;
                return std::ranges::any_of(blocks, [ptr, size](const Block &block) {
                    return block.ptr == ptr && size <= block.bytes;
                });
            }

            [[nodiscard]] bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
                ++resize_calls;

                const auto it = std::ranges::find_if(blocks, [ptr](const Block &block) {
                    return block.ptr == ptr;
                });

                if (it == blocks.end() || new_size == 0u || new_size > old_size) {
                    return false;
                }

                //it->bytes = new_size; // do not resize the tracked block, since it will break deallocateRaw()
                return true;
            }

            [[nodiscard]] const void *watermark() const noexcept {
                ++watermark_calls;
                watermark_token = blocks.size();
                return std::addressof(watermark_token);
            }

            void restore(const void *const mark) noexcept {
                ++restore_calls;
                const std::size_t target = *static_cast<const std::size_t *>(mark);

                while (blocks.size() > target) {
                    const Block block = blocks.back();
                    blocks.pop_back();
                    freeBlock(block);
                }
            }

            void reset() noexcept {
                ++reset_calls;
                while (!blocks.empty()) {
                    const Block block = blocks.back();
                    blocks.pop_back();
                    freeBlock(block);
                }
            }
        };

        static_assert(mem::details::TArenaAllocator<RecordingAllocator>);

        struct ResizeOnlyAllocator {
            struct Block {
                void *ptr{};
                std::size_t bytes{};
                std::align_val_t alignment{max_align_v};
            };

            std::vector<Block> blocks{};

            [[nodiscard]] std::allocation_result<void *>
            allocateRaw(const std::size_t bytes, const std::align_val_t alignment) noexcept {
                void *const ptr = (alignment > max_align_v
                                       ? ::operator new(bytes, alignment, std::nothrow)
                                       : ::operator new(bytes, std::nothrow));
                if (ptr) {
                    blocks.push_back(Block{ptr, bytes, alignment});
                }
                return {ptr, ptr ? bytes : 0u};
            }

            static void freeBlock(const Block &block) noexcept {
                if (block.alignment > max_align_v) {
                    ::operator delete(block.ptr, block.bytes, block.alignment);
                } else {
                    ::operator delete(block.ptr, block.bytes);
                }
            }

            void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
                const auto it = std::ranges::find_if(blocks, [ptr](const Block &block) {
                    return block.ptr == ptr;
                });

                if (it != blocks.end()) {
                    const Block block = *it;
                    blocks.erase(it);
                    freeBlock(block);
                    return;
                }

                if (alignment > max_align_v) {
                    ::operator delete(ptr, bytes, alignment);
                } else {
                    ::operator delete(ptr, bytes);
                }
            }

            [[nodiscard]] bool resizeRaw(void *const ptr, const std::size_t old_size, const std::size_t new_size) noexcept {
                const auto it = std::ranges::find_if(blocks, [ptr](const Block &block) {
                    return block.ptr == ptr;
                });

                if (it == blocks.end() || new_size == 0u || new_size > old_size) {
                    return false;
                }

                //it->bytes = new_size; // do not resize the tracked block, since it will break deallocateRaw()
                return true;
            }
        };

        static_assert(mem::details::TResizableAllocator<ResizeOnlyAllocator>);
        PPR_UNIT_TEST(overlap_boundaries) {
            const std::array<std::byte, 16u> storage{};
            const std::array<std::byte, 16u> other{};

            PPR_ASSERT(mem::overlap(storage.data(), storage.size(), storage.data()));
            PPR_ASSERT(mem::overlap(storage.data(), storage.size(), storage.data() + 15u));
            PPR_ASSERT(!mem::overlap(storage.data(), storage.size(), storage.data() + 16u));

            PPR_ASSERT(mem::overlap(storage.data(), storage.size(), storage.data(), 1u));
            PPR_ASSERT(mem::overlap(storage.data(), storage.size(), storage.data() + 8u, 4u));
            PPR_ASSERT(!mem::overlap(storage.data(), storage.size(), other.data(), other.size()));
        };

        PPR_UNIT_TEST(gpa_alignment_paths) {
            const auto normal = mem::GPA::allocateRaw(sizeof(int), std::align_val_t{alignof(int)});
            PPR_ASSERT(normal.ptr != nullptr);
            mem::GPA::deallocateRaw(normal.ptr, normal.count, std::align_val_t{alignof(int)});

            const auto overaligned = mem::GPA::allocateRaw(64u, std::align_val_t{64u});
            PPR_ASSERT(overaligned.ptr != nullptr);
            PPR_ASSERT(std::bit_cast<std::uintptr_t>(overaligned.ptr) % 64u == 0u);
            mem::GPA::deallocateRaw(overaligned.ptr, overaligned.count, std::align_val_t{64u});
        };

        PPR_UNIT_TEST(insitu_one_shot_and_reuse) {
            mem::InSitu<64u> insitu{};

            const auto first = insitu.allocateRaw(24u, max_align_v);
            PPR_ASSERT(first.ptr != nullptr);
            PPR_ASSERT(insitu.owns(first.ptr, 24u));

            const auto second = insitu.allocateRaw(8u, max_align_v);
            PPR_ASSERT(second.ptr == nullptr);

            insitu.deallocateRaw(first.ptr, first.count, max_align_v);

            const auto third = insitu.allocateRaw(8u, max_align_v);
            PPR_ASSERT(third.ptr != nullptr);
            insitu.deallocateRaw(third.ptr, third.count, max_align_v);
        };

        PPR_UNIT_TEST(fallback_prefers_primary_then_secondary) {
            mem::Fallback<mem::InSitu<32u>, ResizeOnlyAllocator> alloc{};

            const auto primary = alloc.allocateRaw(16u, max_align_v);
            PPR_ASSERT(primary.ptr != nullptr);

            const auto secondary = alloc.allocateRaw(16u, max_align_v);
            PPR_ASSERT(secondary.ptr != nullptr);

            PPR_ASSERT(alloc.resizeRaw(primary.ptr, 16u, 24u));
            PPR_ASSERT(!alloc.resizeRaw(primary.ptr, 24u, 48u));
            PPR_ASSERT(alloc.resizeRaw(secondary.ptr, 16u, 8u));

            alloc.deallocateRaw(secondary.ptr, 8u, max_align_v);
            alloc.deallocateRaw(primary.ptr, 24u, max_align_v);
        };

        PPR_UNIT_TEST(threshold_routes_and_resizes_within_bucket) {
            mem::Threshold<mem::InSitu<64u>, 64u, ResizeOnlyAllocator> alloc{};

            const auto under = alloc.allocateRaw(64u, max_align_v);
            const auto above = alloc.allocateRaw(80u, max_align_v);

            PPR_ASSERT(under.ptr != nullptr);
            PPR_ASSERT(above.ptr != nullptr);

            PPR_ASSERT(alloc.resizeRaw(under.ptr, 64u, 32u));
            PPR_ASSERT(!alloc.resizeRaw(under.ptr, 32u, 96u));
            PPR_ASSERT(alloc.resizeRaw(above.ptr, 80u, 72u));

            alloc.deallocateRaw(under.ptr, 32u, max_align_v);
            alloc.deallocateRaw(above.ptr, 72u, max_align_v);
        };

        PPR_UNIT_TEST(allocator_wrapper_forwards_and_force_ref) {
            RecordingAllocator backend{};
            mem::Allocator wrapped{backend};

            const auto &const_wrapped = wrapped;
            PPR_ASSERT(&const_wrapped.materialize() == &backend);
            PPR_ASSERT(&wrapped.materialize() == &backend);

            const auto block = wrapped.allocateRaw(16u, max_align_v);
            PPR_ASSERT(block.ptr != nullptr);
            PPR_ASSERT(wrapped.owns(block.ptr, 16u));

            auto forced = wrapped.forceRef();
            PPR_ASSERT(&forced.materialize() == &backend);

            const auto extra = forced.allocateRaw(8u, max_align_v);
            PPR_ASSERT(extra.ptr != nullptr);
            PPR_ASSERT(forced.resizeRaw(extra.ptr, 8u, 4u));
            forced.deallocateRaw(extra.ptr, 4u, max_align_v);

            const void *const mark = wrapped.watermark();
            const auto tmp = wrapped.allocateRaw(8u, max_align_v);
            PPR_ASSERT(tmp.ptr != nullptr);
            wrapped.restore(mark);
            PPR_ASSERT(!wrapped.owns(tmp.ptr, 8u));

            wrapped.reset();
            PPR_ASSERT(backend.blocks.empty());
        };

        PPR_UNIT_TEST(pmr_erasure_and_equality) {
            RecordingAllocator backend{};

            const mem::PMR stateful{backend};
            const mem::PMR erased{mem::Allocator{backend}};
            PPR_ASSERT(stateful == erased);

            const auto shrinkable = stateful.allocateRaw(24u, max_align_v);
            PPR_ASSERT(shrinkable.ptr != nullptr);
            PPR_ASSERT(stateful.resizeRaw(shrinkable.ptr, 24u, 16u));
            stateful.deallocateRaw(shrinkable.ptr, 16u, max_align_v);

            const mem::PMR stateless{mem::GPA{}};
            const auto overaligned = stateless.allocateRaw(32u, std::align_val_t{64u});
            PPR_ASSERT(overaligned.ptr != nullptr);
            PPR_ASSERT(!stateless.resizeRaw(overaligned.ptr, overaligned.count, overaligned.count * 2u));
            stateless.deallocateRaw(overaligned.ptr, overaligned.count, std::align_val_t{64u});
        };

        PPR_UNIT_TEST(allocator_traits_operations) {
            RecordingAllocator backend{};
            mem::Allocator wrapped{backend};

            auto *const ints = wrapped.allocate<int>(2u);
            PPR_ASSERT(ints != nullptr);
            ints[0] = 7;
            ints[1] = 11;
            wrapped.deallocate<int>(ints, 2u);

            const auto at_least = wrapped.allocate_at_least<int>(3u);
            PPR_ASSERT(at_least.ptr != nullptr);
            PPR_ASSERT(at_least.count >= 3u);
            wrapped.deallocate<int>(at_least.ptr, 3u);

            const auto span = wrapped.span<int>(4u);
            PPR_ASSERT(span.size() >= 4u);
            span[0] = 1;
            span[3] = 4;
            wrapped.deallocate<int>(span.data(), 4u);

            Widget::destroyed = 0u;
            Widget *const widget = wrapped.create<Widget>(42);
            PPR_ASSERT(widget != nullptr);
            PPR_ASSERT(widget->x == 42);
            wrapped.destroy(widget);
            PPR_ASSERT(Widget::destroyed == 1u);

            PPR_ASSERT(!wrapped.template relocate<int>(nullptr, 0u, 0u).ptr);

            const auto fresh = wrapped.relocate<int>(nullptr, 0u, 3u);
            PPR_ASSERT(fresh.ptr != nullptr);
            fresh.ptr[0] = 1;
            fresh.ptr[1] = 2;
            fresh.ptr[2] = 3;

            const auto shrunk = wrapped.relocate<int>(fresh.ptr, 3u, 2u);
            PPR_ASSERT(shrunk.ptr == fresh.ptr);
            PPR_ASSERT(shrunk.count == 2u);
            PPR_ASSERT(shrunk.ptr[0] == 1);
            PPR_ASSERT(shrunk.ptr[1] == 2);

            auto grown = wrapped.relocate<int>(shrunk.ptr, 2u, 5u);
            PPR_ASSERT(grown.ptr != nullptr);
            PPR_ASSERT(grown.count == 5u);
            PPR_ASSERT(grown.ptr[0] == 1);
            PPR_ASSERT(grown.ptr[1] == 2);

            grown = wrapped.relocate<int>(grown.ptr, 5u, 0u);
            PPR_ASSERT(backend.blocks.empty());
        };

        PPR_UNIT_TEST(allocation_stateful_resize_create_destroy) {
            RecordingAllocator backend{};

            mem::Allocation<int, RecordingAllocator> empty{};
            PPR_ASSERT(!empty.isValid());
            PPR_ASSERT(!empty.resize(backend, 0u));
            PPR_ASSERT(empty.resize(backend, 4u));
            PPR_ASSERT(empty.isValid());
            PPR_ASSERT(empty.count() == 4u);
            empty.deallocateAssumeNotEmpty(backend);
            PPR_ASSERT(!empty.isValid());

            mem::Allocation<int, RecordingAllocator> alloc(4u, backend);
            PPR_ASSERT(mem::Allocation<int, RecordingAllocator>::alignment() == std::align_val_t{alignof(int)});
            PPR_ASSERT(alloc.isValid());
            PPR_ASSERT(alloc.count() == 4u);
            PPR_ASSERT(alloc.size_bytes() == sizeof(int) * 4u);
            PPR_ASSERT(alloc.view().size() == 4u);
            PPR_ASSERT(alloc.owns(alloc.data(), sizeof(int) * 2u));

            for (std::size_t i = 0u; i < alloc.count(); ++i) {
                alloc.data()[i] = static_cast<int>(i);
            }

            PPR_ASSERT(!alloc.resize(backend, 8u));
            PPR_ASSERT(alloc.resize(backend, 2u));
            PPR_ASSERT(alloc.count() == 2u);
            PPR_ASSERT(alloc.data()[0] == 0);
            PPR_ASSERT(alloc.data()[1] == 1);
            alloc.deallocateAssumeNotEmpty(backend);
            PPR_ASSERT(!alloc.isValid());

            Widget::destroyed = 0u;
            mem::Allocation<Widget, RecordingAllocator> widget_alloc{};
            const auto *const created = widget_alloc.create(backend, 99);
            PPR_ASSERT(created != nullptr);
            PPR_ASSERT(created->x == 99);
            widget_alloc.destroy(backend);
            PPR_ASSERT(Widget::destroyed == 1u);
        };

        PPR_UNIT_TEST(allocation_raii_and_relocate) {
            mem::Allocation<int, mem::GPA> alloc(6u);
            PPR_ASSERT(alloc.isValid());
            PPR_ASSERT(alloc.count() == 6u);

            for (std::size_t i = 0u; i < alloc.count(); ++i) {
                alloc.data()[i] = static_cast<int>(i);
            }

            PPR_ASSERT(alloc.view().size() == 6u);

            mem::Allocation moved{std::move(alloc)};
            PPR_ASSERT(!alloc.isValid());
            PPR_ASSERT(moved.isValid());
            PPR_ASSERT(moved.data()[5] == 5);

            mem::Allocation<int, mem::GPA> assigned{};
            assigned = std::move(moved);
            PPR_ASSERT(!moved.isValid());
            PPR_ASSERT(assigned.isValid());
            PPR_ASSERT(assigned.data()[5] == 5);

            const auto relocated = assigned.relocate(12u);
            PPR_ASSERT(relocated.ptr == assigned.data());
            PPR_ASSERT(assigned.count() == 12u);
            PPR_ASSERT(assigned.data()[5] == 5u);

            assigned.deallocate();
            PPR_ASSERT(!assigned.isValid());
        };

        PPR_UNIT_TEST(allocation_create_destroy_non_trivial) {
            Widget::destroyed = 0u;

            mem::Allocation<Widget, mem::GPA> alloc{};
            PPR_ASSERT(!alloc.isValid());
            alloc.destroy();

            const Widget *const widget = alloc.create(17);
            PPR_ASSERT(widget != nullptr);
            PPR_ASSERT(widget->x == 17);
            PPR_ASSERT(alloc.isValid());

            alloc.destroy();
            PPR_ASSERT(!alloc.isValid());
            PPR_ASSERT(Widget::destroyed == 1u);
        };

        PPR_UNIT_TEST(allocation_index_operator) {
            mem::Allocation<int, mem::GPA> alloc(4u);
            PPR_ASSERT(alloc.isValid());
            PPR_ASSERT(alloc.count() == 4u);

            alloc[0] = 10;
            alloc[1] = 20;
            alloc[2] = 30;
            alloc[3] = 40;

            PPR_ASSERT(alloc[0] == 10);
            PPR_ASSERT(alloc[1] == 20);
            PPR_ASSERT(alloc[2] == 30);
            PPR_ASSERT(alloc[3] == 40);

            const auto &const_alloc = alloc;
            PPR_ASSERT(const_alloc[0] == 10);

            alloc.deallocate();
            PPR_ASSERT(!alloc.isValid());
        };
    }

    PPR_UNIT_TEST(allocator) {
        _.recurse({
            Allocator::overlap_boundaries,
            Allocator::gpa_alignment_paths,
            Allocator::insitu_one_shot_and_reuse,
            Allocator::fallback_prefers_primary_then_secondary,
            Allocator::threshold_routes_and_resizes_within_bucket,
            Allocator::allocator_wrapper_forwards_and_force_ref,
            Allocator::pmr_erasure_and_equality,
            Allocator::allocator_traits_operations,
            Allocator::allocation_stateful_resize_create_destroy,
            Allocator::allocation_raii_and_relocate,
            Allocator::allocation_create_destroy_non_trivial,
            Allocator::allocation_index_operator,
        });
    };

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

        PPR_UNIT_TEST(poison_nullptr_safe) {
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

        PPR_UNIT_TEST(insitu_poison_on_dealloc_triggers_asan, UnitTest::expect_crash) {
            mem::InSitu<128u> buffer{};
            const auto [ptr, count] = buffer.allocateRaw(64u, max_align_v);
            buffer.deallocateRaw(ptr, count, max_align_v);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stablevector_asan_on_erase, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.erase(0);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stablevector_asan_multi_slice, UnitTest::expect_crash) {
            StableVector<int> sv;
            for (std::size_t i = 0; i < 100; ++i) {
                sv.pushBack(static_cast<int>(i));
            }
            int *ptr = &sv[50];
            sv.erase(50);
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(stablevector_asan_on_clear, UnitTest::expect_crash) {
            StableVector<int> sv;
            sv.pushBack(42);
            int *ptr = &sv[0];
            sv.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(hashmap_asan_on_clear, UnitTest::expect_crash) {
            HashMap<int, int> hm;
            hm.insert({1, 10});
            auto it = hm.find(1);
            PPR_ASSERT(it != hm.end());
            int *ptr = &it->second;
            hm.clear();
            details::access_after_poison(ptr);
        };

        PPR_UNIT_TEST(sparsevector_asan_on_erase, UnitTest::expect_crash) {
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

    namespace SafePtr {
        struct TestObject : safe_object {
            int m_value{};
        };

        PPR_UNIT_TEST(copy_construction_preserves_target) {
            TestObject obj{};
            safe_ptr<TestObject> a{&obj};
            PPR_ASSERT(a.isValid());
            const safe_ptr<TestObject> b{a};
            PPR_ASSERT(b.isValid());
            PPR_ASSERT(b.get() == &obj);
            PPR_ASSERT(a.get() == b.get());
        };

        PPR_UNIT_TEST(copy_assignment_switches_target) {
            TestObject obj_a{};
            TestObject obj_b{};
            safe_ptr<TestObject> a{&obj_a};
            safe_ptr<TestObject> b{&obj_b};
            PPR_ASSERT(a.get() == &obj_a);
            PPR_ASSERT(b.get() == &obj_b);
            b = a;
            PPR_ASSERT(b.get() == &obj_a);
            PPR_ASSERT(a.get() == b.get());
        };

        PPR_UNIT_TEST(self_assignment_is_safe) {
            TestObject obj{};
            safe_ptr<TestObject> a{&obj};
            a = a;
            PPR_ASSERT(a.isValid());
            PPR_ASSERT(a.get() == &obj);
        };

        PPR_UNIT_TEST(null_copy_remains_null) {
            const safe_ptr<TestObject> a{};
            const safe_ptr<TestObject> b{a};
            PPR_ASSERT(b.get() == nullptr);
        };

        PPR_UNIT_TEST(nullptr_assignment_clears) {
            TestObject obj{};
            safe_ptr<TestObject> a{&obj};
            PPR_ASSERT(a.isValid());
            a = nullptr;
            PPR_ASSERT(!a.isValid());
        };

        PPR_UNIT_TEST(safe_object_destroy_with_live_ref, UnitTest::expect_crash) {
            auto *obj = new TestObject{};
            safe_ptr<TestObject> ptr{obj};
            delete obj;
        };

        PPR_UNIT_TEST(safe_object_move_with_live_ref, UnitTest::expect_crash) {
            TestObject src{};
            safe_ptr<TestObject> ptr{&src};
            TestObject dst{std::move(src)};
        };

        PPR_UNIT_TEST(safe_object_copy_with_live_ref, UnitTest::expect_crash) {
            TestObject src{};
            safe_ptr<TestObject> ptr{&src};
            TestObject dst{src};
        };
    }

    PPR_UNIT_TEST(safe_ptr_test) {
        _.recurse({
            SafePtr::copy_construction_preserves_target,
            SafePtr::copy_assignment_switches_target,
            SafePtr::self_assignment_is_safe,
            SafePtr::null_copy_remains_null,
            SafePtr::nullptr_assignment_clears,
        });

        if constexpr (PPR_ENABLE_ASSERTIONS) {
            _.recurse({
                SafePtr::safe_object_destroy_with_live_ref,
                SafePtr::safe_object_move_with_live_ref,
                SafePtr::safe_object_copy_with_live_ref,
            });
        }
    };

    PPR_UNIT_TEST(poisoning) {
        _.recurse({
            Poisoning::child_process_without_error,
        });

        if constexpr (mem::is_asan_enabled_v) {
            _.recurse({
                Poisoning::poison_destroyed_then_read_triggers_asan,
                Poisoning::poison_reserved_then_write_triggers_asan,
                Poisoning::poison_nullptr_safe,
                Poisoning::gpa_poison_on_free_triggers_asan,
                Poisoning::os_poison_on_free_triggers_asan,
                Poisoning::pooling_poison_on_free_triggers_asan,
                Poisoning::arena_poison_on_dealloc_triggers_asan,
                Poisoning::insitu_poison_on_dealloc_triggers_asan,
                Poisoning::stablevector_asan_on_erase,
                Poisoning::stablevector_asan_multi_slice,
                Poisoning::stablevector_asan_on_clear,
                Poisoning::hashmap_asan_on_clear,
                Poisoning::sparsevector_asan_on_erase,
                Poisoning::arena_asan_on_restore,
                Poisoning::arena_cross_slab_restore_triggers_asan,
                Poisoning::pooling_pool_level_poison,
            });
        }
    };
}
