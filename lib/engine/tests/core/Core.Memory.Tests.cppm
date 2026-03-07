module;
#include "pP/Macros.h"
export module engine.tests:core_memory;
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
            std::array<std::byte, 16u> storage{};
            std::array<std::byte, 16u> other{};

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

            mem::Pmr stateful{backend};
            mem::Pmr erased{mem::Allocator{backend}};
            PPR_ASSERT(stateful == erased);

            const auto shrinkable = stateful.allocateRaw(24u, max_align_v);
            PPR_ASSERT(shrinkable.ptr != nullptr);
            PPR_ASSERT(stateful.resizeRaw(shrinkable.ptr, 24u, 16u));
            stateful.deallocateRaw(shrinkable.ptr, 16u, max_align_v);

            mem::Pmr stateless{mem::GPA{}};
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

            const auto atleast = wrapped.allocate_at_least<int>(3u);
            PPR_ASSERT(atleast.ptr != nullptr);
            PPR_ASSERT(atleast.count >= 3u);
            wrapped.deallocate<int>(atleast.ptr, 3u);

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
            auto *const created = widget_alloc.create(backend, 99);
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

            Widget *const widget = alloc.create(17);
            PPR_ASSERT(widget != nullptr);
            PPR_ASSERT(widget->x == 17);
            PPR_ASSERT(alloc.isValid());

            alloc.destroy();
            PPR_ASSERT(!alloc.isValid());
            PPR_ASSERT(Widget::destroyed == 1u);
        };
    }

    PPR_UNIT_TEST(allocator) {
        _.recurse(Allocator::overlap_boundaries);
        _.recurse(Allocator::gpa_alignment_paths);
        _.recurse(Allocator::insitu_one_shot_and_reuse);
        _.recurse(Allocator::fallback_prefers_primary_then_secondary);
        _.recurse(Allocator::threshold_routes_and_resizes_within_bucket);
        _.recurse(Allocator::allocator_wrapper_forwards_and_force_ref);
        _.recurse(Allocator::pmr_erasure_and_equality);
        _.recurse(Allocator::allocator_traits_operations);
        _.recurse(Allocator::allocation_stateful_resize_create_destroy);
        _.recurse(Allocator::allocation_raii_and_relocate);
        _.recurse(Allocator::allocation_create_destroy_non_trivial);
    };
}
