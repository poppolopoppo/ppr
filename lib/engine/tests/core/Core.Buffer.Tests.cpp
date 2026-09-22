module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Buffer {
        // ------------------------------------------------------------------
        // API traits (new Buffer API — Unique/Shared/WeakSharedBuffer)
        // ------------------------------------------------------------------

        static_assert(std::default_initializable<mem::UniqueBuffer>);
        static_assert(std::default_initializable<mem::SharedBuffer>);
        static_assert(std::default_initializable<mem::WeakSharedBuffer>);
        static_assert(not std::copyable<mem::UniqueBuffer>);
        static_assert(std::movable<mem::UniqueBuffer>);
        static_assert(std::copyable<mem::SharedBuffer>);
        static_assert(std::movable<mem::SharedBuffer>);
        static_assert(std::copyable<mem::WeakSharedBuffer>);
        static_assert(std::movable<mem::WeakSharedBuffer>);
        static_assert(std::constructible_from<mem::WeakSharedBuffer, const mem::SharedBuffer &>);
        static_assert(not std::constructible_from<mem::WeakSharedBuffer, mem::UniqueBuffer &>);
        static_assert(not std::constructible_from<mem::WeakSharedBuffer, mem::UniqueBuffer &&>);
        static_assert(not std::constructible_from<mem::WeakSharedBuffer, mem::SharedBufferView>);

        static_assert(std::same_as<decltype(std::declval<mem::UniqueBuffer &>().getMutableData()), Expected<mem::MutableBufferView> >);
        static_assert(std::same_as<decltype(std::declval<const mem::UniqueBuffer &>().getBufferData()), mem::MutableBufferView>);
        static_assert(std::same_as<decltype(std::declval<mem::UniqueBuffer &&>().makeOwned()), mem::UniqueBuffer>);
        static_assert(std::same_as<decltype(std::declval<mem::UniqueBuffer &>().moveToShared(nullptr)), std::error_code>);
        static_assert(std::same_as<decltype(std::declval<mem::UniqueBuffer &>().materialize()), std::error_code>);
        static_assert(std::same_as<decltype(std::declval<const mem::SharedBuffer &>().getBufferData()), mem::SharedBufferView>);
        static_assert(std::same_as<decltype(std::declval<const mem::SharedBuffer &>().makeOwned()), mem::SharedBuffer>);
        static_assert(std::same_as<decltype(std::declval<mem::SharedBuffer &&>().makeOwned()), mem::SharedBuffer>);
        static_assert(std::same_as<decltype(std::declval<mem::SharedBuffer &>().moveToUnique()), mem::UniqueBuffer>);
        static_assert(std::same_as<decltype(std::declval<const mem::SharedBuffer &>().subspan(0u)), mem::SharedBuffer>);
        static_assert(std::same_as<decltype(std::declval<const mem::WeakSharedBuffer &>().pin()), mem::SharedBuffer>);
        static_assert(std::same_as<decltype(mem::UniqueBuffer::clone(mem::SharedBufferView{})), mem::UniqueBuffer>);
        static_assert(std::same_as<decltype(mem::SharedBuffer::clone(mem::SharedBufferView{})), mem::SharedBuffer>);
        static_assert(std::same_as<decltype(mem::UniqueBuffer::mapFile(std::filesystem::path{})), Expected<mem::UniqueBuffer> >);
        static_assert(std::same_as<decltype(mem::SharedBuffer::mapFile(std::filesystem::path{})), Expected<mem::SharedBuffer> >);

        // ------------------------------------------------------------------
        // Helpers (GLFW-free, deterministic, engine allocators only)
        // ------------------------------------------------------------------

        constexpr std::byte kTestBytes[]{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

        [[nodiscard]] mem::UniqueBuffer makeUnique(const std::size_t size_bytes) {
            mem::Allocation<std::byte, mem::GPA> allocation{size_bytes};
            return mem::UniqueBuffer{std::move(allocation)};
        }

        // Immutable, non-owned buffer via an explicit deleter-owning BufferOwner.
        // (Allocation<const T> is rejected by Allocation's own static_assert.)
        // Built directly on the heap: moving an immutable BufferOwner trips
        // PPR_ASSERT(isMaterialized()) in BufferOwner::operator=, which throws
        // inside noexcept and terminates assert-enabled builds.
        [[nodiscard]] mem::UniqueBuffer makeImmutableUnique(const std::span<const std::byte> source) {
            std::byte *const raw = static_cast<std::byte *>(std::malloc(source.size()));
            PPR_TEST_ASSERT(raw != nullptr);
            std::ranges::copy(source, raw);
            auto owner = std::make_unique<mem::details::BufferOwner>(
                static_cast<const void *>(raw),
                source.size(),
                [](void *, const std::allocation_result<void *> alloc) { std::free(alloc.ptr); });
            return mem::UniqueBuffer{std::move(owner)};
        }

        void writeBytes(mem::UniqueBuffer &buffer, const std::span<const std::byte> source) {
            auto view = buffer.getMutableData();
            PPR_TEST_ASSERT(view.has_value());
            PPR_TEST_ASSERT(view->size() == source.size());
            std::ranges::copy(source, view->data());
        }

        // Minimal stateful allocator (non-empty => stateful Allocation path).
        struct StatefulAllocator {
            std::size_t live_bytes{0u};
            std::size_t allocate_calls{0u};
            std::size_t deallocate_calls{0u};

            [[nodiscard]] std::allocation_result<void *> allocateRaw(const std::size_t bytes, const std::align_val_t) {
                ++allocate_calls;
                void *const ptr = std::malloc(bytes == 0u ? 1u : bytes);
                if (ptr != nullptr) {
                    live_bytes += bytes;
                }
                return {ptr, ptr != nullptr ? bytes : 0u};
            }

            void deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t) noexcept {
                ++deallocate_calls;
                live_bytes -= bytes;
                std::free(ptr);
            }
        };

        enum class EMaterializationResult {
            throwing,
            null,
            short_allocation
        };

        struct MaterializationFailureAllocator {
            EMaterializationResult result{};
            std::size_t deallocate_calls{0u};

            [[nodiscard]] std::allocation_result<void *> allocateRaw(const std::size_t, const std::align_val_t) {
                switch (result) {
                    case EMaterializationResult::throwing:
                        throw std::bad_alloc{};
                    case EMaterializationResult::null:
                        return {};
                    case EMaterializationResult::short_allocation:
                        return {std::malloc(1u), 1u};
                }
                std::unreachable();
            }

            void deallocateRaw(void *const ptr, const std::size_t, const std::align_val_t) noexcept {
                ++deallocate_calls;
                std::free(ptr);
            }
        };

        [[nodiscard]] mem::SharedBuffer toShared(mem::UniqueBuffer &buffer) {
            mem::SharedBuffer shared{};
            PPR_TEST_ASSERT(not buffer.moveToShared(&shared));
            return shared;
        }

        [[nodiscard]] std::filesystem::path writeTempMapFile(const std::span<const std::byte> content) {
            const auto path = std::filesystem::temp_directory_path() / "ppr_buffer_test_map.bin";
            {
                std::ofstream out{path, std::ios::binary | std::ios::trunc};
                PPR_TEST_ASSERT(out.is_open());
                out.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
                PPR_TEST_ASSERT(static_cast<bool>(out));
            }
            return path;
        }

        void removeTempMapFile(const std::filesystem::path &path) noexcept {
            std::error_code ec{};
            std::filesystem::remove(path, ec);
        }

        // ------------------------------------------------------------------
        // UniqueBuffer
        // ------------------------------------------------------------------

        PPR_UNIT_TEST(unique_default_is_empty) {
            mem::UniqueBuffer empty{};
            PPR_TEST_ASSERT(not empty.isValid());
            PPR_TEST_ASSERT(not static_cast<bool>(empty));
            PPR_TEST_ASSERT(empty.getBufferData().empty());
            PPR_TEST_ASSERT(empty.isImmutable());
            PPR_TEST_ASSERT(empty.isMaterialized());
            PPR_TEST_ASSERT(empty.isOwned());

            const auto mutable_data = empty.getMutableData();
            PPR_TEST_ASSERT(not mutable_data.has_value());
            PPR_TEST_ASSERT(mutable_data.error() == std::make_error_code(std::errc::not_connected));

            PPR_TEST_ASSERT(not empty.materialize());

            empty.reset();
            PPR_TEST_ASSERT(not empty.isValid());
        };

        PPR_UNIT_TEST(unique_best_fit_allocate_zero_is_invalid) {
            const mem::UniqueBuffer empty = mem::UniqueBuffer::allocate(0u);
            PPR_TEST_ASSERT(not empty.isValid());
            PPR_TEST_ASSERT(not static_cast<bool>(empty));

            const mem::UniqueBuffer cloned = mem::UniqueBuffer::clone(mem::SharedBufferView{});
            PPR_TEST_ASSERT(not cloned.isValid());
        };

        PPR_UNIT_TEST(unique_allocation_ctor_stateless_roundtrip) {
            mem::Allocation<std::byte, mem::GPA> allocation{4u};
            mem::UniqueBuffer buffer{std::move(allocation)};
            PPR_TEST_ASSERT(buffer.isValid());
            PPR_TEST_ASSERT(static_cast<bool>(buffer));
            PPR_TEST_ASSERT(not buffer.isImmutable());
            PPR_TEST_ASSERT(buffer.isMaterialized());
            PPR_TEST_ASSERT(buffer.isOwned());

            writeBytes(buffer, kTestBytes);
            const mem::MutableBufferView view = buffer.getBufferData();
            PPR_TEST_ASSERT(view.size() == 4u);
            PPR_TEST_ASSERT(std::ranges::equal(view, std::span<const std::byte>{kTestBytes}));
        };

        PPR_UNIT_TEST(unique_allocation_ctor_stateful) {
            StatefulAllocator allocator{};
            {
                mem::Allocation<std::byte, StatefulAllocator> allocation{4u, allocator};
                mem::UniqueBuffer buffer{std::move(allocation), allocator};
                PPR_TEST_ASSERT(buffer.isValid());
                PPR_TEST_ASSERT(buffer.isOwned());
                PPR_TEST_ASSERT(buffer.isMaterialized());
                PPR_TEST_ASSERT(allocator.allocate_calls == 1u);

                writeBytes(buffer, kTestBytes);
                PPR_TEST_ASSERT(std::ranges::equal(buffer.getBufferData(), std::span<const std::byte>{kTestBytes}));
            }
            PPR_TEST_ASSERT(allocator.deallocate_calls == 1u);
            PPR_TEST_ASSERT(allocator.live_bytes == 0u);
        };

        PPR_UNIT_TEST(unique_immutable_owner_rejects_mutable_access) {
            mem::UniqueBuffer buffer = makeImmutableUnique(kTestBytes);
            PPR_TEST_ASSERT(buffer.isValid());
            PPR_TEST_ASSERT(buffer.isImmutable());
            PPR_TEST_ASSERT(buffer.isMaterialized());
            PPR_TEST_ASSERT(not buffer.isOwned());

            const auto mutable_data = buffer.getMutableData();
            PPR_TEST_ASSERT(not mutable_data.has_value());
            PPR_TEST_ASSERT(mutable_data.error() == std::make_error_code(std::errc::operation_not_permitted));

            mem::SharedBuffer shared = toShared(buffer);
            PPR_TEST_ASSERT(shared.isValid());
            PPR_TEST_ASSERT(shared.isImmutable());
            PPR_TEST_ASSERT(not shared.isUniqueOwnedMutable());
        };

        PPR_UNIT_TEST(unique_clone_empty_is_invalid) {
            mem::GPA allocator{};
            const mem::UniqueBuffer cloned = mem::UniqueBuffer::clone(allocator, mem::SharedBufferView{});
            PPR_TEST_ASSERT(not cloned.isValid());

            const mem::SharedBuffer shared_clone = mem::SharedBuffer::clone(mem::SharedBufferView{});
            PPR_TEST_ASSERT(not shared_clone.isValid());
        };

        PPR_UNIT_TEST(unique_lazy_allocate_stays_unmaterialized) {
            // allocate(allocator, size) builds a lazy (unmaterialized) owner.
            // Data views stay untouched here: materialization is deferred.
            mem::GPA allocator{};
            mem::UniqueBuffer buffer = mem::UniqueBuffer::allocate(allocator, 16u);
            PPR_TEST_ASSERT(buffer.isValid());
            PPR_TEST_ASSERT(buffer.isOwned());
            PPR_TEST_ASSERT(not buffer.isImmutable());
            PPR_TEST_ASSERT(not buffer.isMaterialized());

            const std::error_code ec = buffer.materialize();
            PPR_TEST_ASSERT(not ec);
        };

        PPR_UNIT_TEST(unique_lazy_materialization_failures_are_invalid) {
            for (const EMaterializationResult result: {EMaterializationResult::throwing, EMaterializationResult::null, EMaterializationResult::short_allocation}) {
                MaterializationFailureAllocator allocator{.result = result};
                mem::UniqueBuffer buffer = mem::UniqueBuffer::allocate(allocator, 4u);

                const auto mutable_data = buffer.getMutableData();
                PPR_TEST_ASSERT(not mutable_data.has_value());
                PPR_TEST_ASSERT(mutable_data.error() == std::make_error_code(std::errc::not_enough_memory));
                PPR_TEST_ASSERT(not buffer.isMaterialized());
                PPR_TEST_ASSERT(buffer.getBufferData().empty());
                PPR_TEST_ASSERT(not buffer.isMaterialized());
                PPR_TEST_ASSERT(allocator.deallocate_calls == (result == EMaterializationResult::short_allocation ? 2u : 0u));
            }
        };

        PPR_UNIT_TEST(unique_lazy_move_to_shared_materializes_readable_data) {
            StatefulAllocator allocator{};
            mem::UniqueBuffer unique = mem::UniqueBuffer::allocate(allocator, 4u);
            PPR_TEST_ASSERT(not unique.isMaterialized());

            mem::SharedBuffer shared{};
            const std::error_code error_code = unique.moveToShared(&shared);
            PPR_TEST_ASSERT(not error_code);
            PPR_TEST_ASSERT(not unique.isValid());
            PPR_TEST_ASSERT(shared.isValid());
            PPR_TEST_ASSERT(shared.isMaterialized());
            PPR_TEST_ASSERT(shared.getBufferData().size_bytes() == 4u);
            PPR_TEST_ASSERT(allocator.allocate_calls == 1u);
        };

        PPR_UNIT_TEST(unique_lazy_move_to_shared_propagates_materialization_failure) {
            MaterializationFailureAllocator allocator{.result = EMaterializationResult::null};
            mem::UniqueBuffer unique = mem::UniqueBuffer::allocate(allocator, 4u);
            mem::SharedBuffer shared{};

            const std::error_code error_code = unique.moveToShared(&shared);
            PPR_TEST_ASSERT(error_code == std::make_error_code(std::errc::not_enough_memory));
            PPR_TEST_ASSERT(unique.isValid());
            PPR_TEST_ASSERT(not unique.isMaterialized());
            PPR_TEST_ASSERT(not shared.isValid());
        };

        PPR_UNIT_TEST(unique_scratch_is_lazy_and_empty_clone_is_invalid) {
            mem::UniqueBuffer scratched = mem::UniqueBuffer::scratch(16u);
            PPR_TEST_ASSERT(scratched.isValid());
            PPR_TEST_ASSERT(scratched.isOwned());
            PPR_TEST_ASSERT(not scratched.isMaterialized());

            const mem::UniqueBuffer cloned = mem::UniqueBuffer::scratch(mem::SharedBufferView{});
            PPR_TEST_ASSERT(not cloned.isValid());
        };

        PPR_UNIT_TEST(unique_make_owned_passthrough_moves_owned) {
            mem::UniqueBuffer buffer = makeUnique(4u);
            writeBytes(buffer, kTestBytes);
            const auto *const data = buffer.getBufferData().data();

            mem::UniqueBuffer owned = std::move(buffer).makeOwned();
            PPR_TEST_ASSERT(owned.isValid());
            PPR_TEST_ASSERT(owned.getBufferData().data() == data);
            PPR_TEST_ASSERT(std::ranges::equal(owned.getBufferData(), std::span<const std::byte>{kTestBytes}));
            PPR_TEST_ASSERT(not buffer.isValid());

            mem::UniqueBuffer empty{};
            mem::UniqueBuffer still_empty = std::move(empty).makeOwned();
            PPR_TEST_ASSERT(not still_empty.isValid());
        };

        PPR_UNIT_TEST(unique_move_to_shared_transfers_ownership) {
            mem::UniqueBuffer buffer = makeUnique(4u);
            writeBytes(buffer, kTestBytes);
            const auto *const data = buffer.getBufferData().data();

            mem::SharedBuffer shared = toShared(buffer);
            PPR_TEST_ASSERT(not buffer.isValid());
            PPR_TEST_ASSERT(shared.isValid());
            PPR_TEST_ASSERT(shared.getBufferData().data() == data);
            PPR_TEST_ASSERT(std::ranges::equal(shared.getBufferData(), std::span<const std::byte>{kTestBytes}));

            mem::UniqueBuffer empty{};
            mem::SharedBuffer empty_shared{};
            const std::error_code error_code = empty.moveToShared(&empty_shared);
            PPR_TEST_ASSERT(error_code == std::make_error_code(std::errc::not_connected));
            PPR_TEST_ASSERT(not empty_shared.isValid());
        };

        PPR_UNIT_TEST(unique_move_assign_and_swap) {
            mem::UniqueBuffer source = makeUnique(2u);
            const auto *const source_data = source.getBufferData().data();
            mem::UniqueBuffer destination{};

            destination = std::move(source);
            PPR_TEST_ASSERT(not source.isValid());
            PPR_TEST_ASSERT(destination.isValid());
            PPR_TEST_ASSERT(destination.getBufferData().data() == source_data);

            mem::UniqueBuffer other = makeUnique(1u);
            const auto *const other_data = other.getBufferData().data();
            swap(destination, other);
            PPR_TEST_ASSERT(destination.getBufferData().data() == other_data);
            PPR_TEST_ASSERT(other.getBufferData().data() == source_data);

            destination.reset();
            PPR_TEST_ASSERT(not destination.isValid());
            destination.reset();
            PPR_TEST_ASSERT(not destination.isValid());
        };

        PPR_UNIT_TEST(unique_map_file_error_paths) {
            const auto missing = std::filesystem::temp_directory_path() / "ppr_buffer_test_missing.bin";
            removeTempMapFile(missing);

            const auto expected = mem::UniqueBuffer::mapFile(missing);
            PPR_TEST_ASSERT(not expected.has_value());
            PPR_TEST_ASSERT(static_cast<bool>(expected.error()));

            mem::UniqueBuffer out{};
            const std::error_code ec = mem::UniqueBuffer::mapFile(missing, &out);
            PPR_TEST_ASSERT(static_cast<bool>(ec));
            PPR_TEST_ASSERT(not out.isValid());
        };

        // ------------------------------------------------------------------
        // SharedBuffer
        // ------------------------------------------------------------------

        PPR_UNIT_TEST(shared_default_is_empty) {
            const mem::SharedBuffer empty{};
            PPR_TEST_ASSERT(not empty.isValid());
            PPR_TEST_ASSERT(not static_cast<bool>(empty));
            PPR_TEST_ASSERT(empty.getBufferData().empty());
            PPR_TEST_ASSERT(empty.isImmutable());
            PPR_TEST_ASSERT(empty.isMaterialized());
            PPR_TEST_ASSERT(empty.isOwned());
            PPR_TEST_ASSERT(not empty.isUniqueOwnedMutable());

            mem::SharedBuffer resettable{};
            resettable.reset();
            PPR_TEST_ASSERT(not resettable.isValid());
        };

        PPR_UNIT_TEST(shared_move_to_shared_and_copy_share_data) {
            mem::UniqueBuffer unique = makeUnique(4u);
            writeBytes(unique, kTestBytes);
            const auto *const data = unique.getBufferData().data();

            mem::SharedBuffer shared = toShared(unique);
            PPR_TEST_ASSERT(shared.isValid());
            PPR_TEST_ASSERT(shared.getBufferData().data() == data);
            PPR_TEST_ASSERT(std::ranges::equal(shared.getBufferData(), std::span<const std::byte>{kTestBytes}));
            PPR_TEST_ASSERT(shared.isUniqueOwnedMutable());

            mem::SharedBuffer copy{shared};
            PPR_TEST_ASSERT(copy == shared);
            PPR_TEST_ASSERT(copy.getBufferData().data() == shared.getBufferData().data());
            PPR_TEST_ASSERT(hashValue(copy) == hashValue(shared));
            PPR_TEST_ASSERT(not shared.isUniqueOwnedMutable());
            PPR_TEST_ASSERT(not copy.isUniqueOwnedMutable());

            mem::SharedBuffer assigned{};
            assigned = copy;
            PPR_TEST_ASSERT(assigned == shared);
        };

        PPR_UNIT_TEST(shared_is_unique_owned_mutable_matrix) {
            const mem::SharedBuffer empty{};
            PPR_TEST_ASSERT(not empty.isUniqueOwnedMutable());

            mem::UniqueBuffer unique = makeUnique(2u);
            mem::SharedBuffer single = toShared(unique);
            PPR_TEST_ASSERT(single.isUniqueOwnedMutable());

            const mem::SharedBuffer second{single};
            PPR_TEST_ASSERT(not single.isUniqueOwnedMutable());

            mem::UniqueBuffer immutable_unique = makeImmutableUnique(kTestBytes);
            mem::SharedBuffer immutable = toShared(immutable_unique);
            PPR_TEST_ASSERT(immutable.isImmutable());
            PPR_TEST_ASSERT(not immutable.isUniqueOwnedMutable());
        };

        PPR_UNIT_TEST(shared_make_owned_passthrough_on_owned) {
            mem::UniqueBuffer unique = makeUnique(4u);
            writeBytes(unique, kTestBytes);
            mem::SharedBuffer shared = toShared(unique);

            const mem::SharedBuffer lvalue_owned = shared.makeOwned();
            PPR_TEST_ASSERT(lvalue_owned == shared);
            PPR_TEST_ASSERT(shared.isValid());

            const auto *const data = shared.getBufferData().data();
            mem::SharedBuffer rvalue_owned = std::move(shared).makeOwned();
            PPR_TEST_ASSERT(rvalue_owned.isValid());
            PPR_TEST_ASSERT(rvalue_owned.getBufferData().data() == data);
            PPR_TEST_ASSERT(std::ranges::equal(rvalue_owned.getBufferData(), std::span<const std::byte>{kTestBytes}));

            const mem::SharedBuffer empty{};
            const mem::SharedBuffer empty_owned = empty.makeOwned();
            PPR_TEST_ASSERT(not empty_owned.isValid());
        };

        PPR_UNIT_TEST(shared_move_to_unique_takes_single_owned_mutable) {
            mem::UniqueBuffer unique = makeUnique(4u);
            writeBytes(unique, kTestBytes);
            const auto *const data = unique.getBufferData().data();
            mem::SharedBuffer shared = toShared(unique);
            const mem::WeakSharedBuffer weak{shared};

            mem::UniqueBuffer taken = shared.moveToUnique();
            PPR_TEST_ASSERT(taken.isValid());
            PPR_TEST_ASSERT(taken.getBufferData().data() == data);
            PPR_TEST_ASSERT(std::ranges::equal(taken.getBufferData(), std::span<const std::byte>{kTestBytes}));
            PPR_TEST_ASSERT(not shared.isValid());
            PPR_TEST_ASSERT(not weak.pin().isValid());

            mem::SharedBuffer empty{};
            mem::UniqueBuffer empty_taken = empty.moveToUnique();
            PPR_TEST_ASSERT(not empty_taken.isValid());
        };

        PPR_UNIT_TEST(shared_subspan_empty_and_out_of_range) {
            mem::UniqueBuffer unique = makeUnique(4u);
            const mem::SharedBuffer base = toShared(unique);

            const mem::SharedBuffer at_end = base.subspan(4u);
            PPR_TEST_ASSERT(not at_end.isValid());
            const mem::SharedBuffer empty_span = base.subspan(0u, 0u);
            PPR_TEST_ASSERT(not empty_span.isValid());

            const mem::SharedBuffer empty{};
            PPR_TEST_ASSERT(not empty.subspan(0u).isValid());
        };

        PPR_UNIT_TEST(shared_subspan_flags_and_parent_lifetime) {
            mem::UniqueBuffer unique = makeUnique(4u);
            writeBytes(unique, kTestBytes);
            mem::SharedBuffer base = toShared(unique);

            const mem::SharedBuffer slice = base.subspan(1u, 2u);
            PPR_TEST_ASSERT(slice.isValid());
            PPR_TEST_ASSERT(slice.isImmutable());
            PPR_TEST_ASSERT(slice.isMaterialized());
            PPR_TEST_ASSERT(not slice.isOwned());
            PPR_TEST_ASSERT(not slice.isUniqueOwnedMutable());
            PPR_TEST_ASSERT(not (slice == base));

            const mem::SharedBuffer whole = base.subspan(0u);
            PPR_TEST_ASSERT(whole.isValid());
            PPR_TEST_ASSERT(not whole.isOwned());

            mem::SharedBuffer orphan{};
            {
                orphan = base.subspan(0u, 2u);
                PPR_TEST_ASSERT(orphan.isValid());
            }
            base.reset();
            PPR_TEST_ASSERT(orphan.isValid());
            PPR_TEST_ASSERT(not orphan.isOwned());
        };

        PPR_UNIT_TEST(shared_equality_ordering_hash_and_swap) {
            mem::UniqueBuffer first_unique = makeUnique(2u);
            mem::UniqueBuffer second_unique = makeUnique(2u);
            mem::SharedBuffer first = toShared(first_unique);
            mem::SharedBuffer second = toShared(second_unique);

            PPR_TEST_ASSERT(first == first);
            PPR_TEST_ASSERT(not (first == second));
            PPR_TEST_ASSERT((first <=> first) == std::strong_ordering::equal);
            PPR_TEST_ASSERT((first <=> second) != std::strong_ordering::equal);
            PPR_TEST_ASSERT(hashValue(first) == hashValue(first));

            const auto *const first_data = first.getBufferData().data();
            swap(first, second);
            PPR_TEST_ASSERT(first.getBufferData().data() != first_data);

            first.reset();
            PPR_TEST_ASSERT(not first.isValid());
            PPR_TEST_ASSERT(second.isValid());
        };

        PPR_UNIT_TEST(shared_map_file_error_paths) {
            const auto missing = std::filesystem::temp_directory_path() / "ppr_buffer_test_missing.bin";
            removeTempMapFile(missing);

            const auto expected = mem::SharedBuffer::mapFile(missing);
            PPR_TEST_ASSERT(not expected.has_value());
            PPR_TEST_ASSERT(static_cast<bool>(expected.error()));

            mem::SharedBuffer out{};
            const std::error_code ec = mem::SharedBuffer::mapFile(missing, &out);
            PPR_TEST_ASSERT(static_cast<bool>(ec));
            PPR_TEST_ASSERT(not out.isValid());
        };

        // ------------------------------------------------------------------
        // Known impl-bug probes — expect_crash (fail-fast, forked child).
        // Native facility is UnitTest::expect_crash (= expect_fail | fork):
        // plain expect_fail only catches C++ exceptions in-process, while these
        // paths terminate the process (CRT fail-fast / AV), so they must run
        // forked. Each documents its root cause; if the impl is fixed the test
        // will exit 0 and the runner will fail with "expected to fail",
        // signalling the probe should become a normal passing test.
        // ------------------------------------------------------------------

        PPR_UNIT_TEST(unique_map_file_roundtrip_reads_bytes) {
            const auto path = writeTempMapFile(kTestBytes);
            const auto expected = mem::UniqueBuffer::mapFile(path);
            PPR_TEST_ASSERT(expected.has_value());
            PPR_TEST_ASSERT(expected->isValid());
            PPR_TEST_ASSERT(std::ranges::equal(expected->getBufferData(), std::span<const std::byte>{kTestBytes}));

            mem::UniqueBuffer out{};
            const std::error_code ec = mem::UniqueBuffer::mapFile(path, &out);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(out.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(out.getBufferData(), std::span<const std::byte>{kTestBytes}));

            removeTempMapFile(path);
        };

        PPR_UNIT_TEST(shared_map_file_roundtrip_reads_bytes) {
            const auto path = writeTempMapFile(kTestBytes);

            const auto expected = mem::SharedBuffer::mapFile(path);
            PPR_TEST_ASSERT(expected.has_value());
            PPR_TEST_ASSERT(expected->isValid());
            PPR_TEST_ASSERT(std::ranges::equal(expected->getBufferData(), std::span<const std::byte>{kTestBytes}));

            mem::SharedBuffer out{};
            const std::error_code ec = mem::SharedBuffer::mapFile(path, &out);
            PPR_TEST_ASSERT(not ec);
            PPR_TEST_ASSERT(out.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(out.getBufferData(), std::span<const std::byte>{kTestBytes}));

            removeTempMapFile(path);
        };

        PPR_UNIT_TEST(unique_bestfit_clone_nonempty) {
            const mem::UniqueBuffer cloned =
                    mem::UniqueBuffer::clone(mem::SharedBufferView{std::span<const std::byte>{kTestBytes}});
            PPR_TEST_ASSERT(cloned.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(cloned.getBufferData(), std::span<const std::byte>{kTestBytes}));
        };

        PPR_UNIT_TEST(unique_scratch_view_nonempty) {
            const mem::UniqueBuffer cloned =
                    mem::UniqueBuffer::scratch(mem::SharedBufferView{std::span<const std::byte>{kTestBytes}});
            PPR_TEST_ASSERT(cloned.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(cloned.getBufferData(), std::span<const std::byte>{kTestBytes}));
        };

        PPR_UNIT_TEST(unique_make_owned_immutable_clone) {
            mem::UniqueBuffer immutable = makeImmutableUnique(kTestBytes);
            PPR_TEST_ASSERT(immutable.isImmutable());
            PPR_TEST_ASSERT(not immutable.isOwned());
            mem::UniqueBuffer owned = std::move(immutable).makeOwned();
            PPR_TEST_ASSERT(owned.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(owned.getBufferData(), std::span<const std::byte>{kTestBytes}));
        };

        PPR_UNIT_TEST(shared_make_owned_unowned_clone) {
            mem::UniqueBuffer immutable_unique = makeImmutableUnique(kTestBytes);
            mem::SharedBuffer unowned = toShared(immutable_unique);
            PPR_TEST_ASSERT(unowned.isValid());
            PPR_TEST_ASSERT(unowned.isImmutable());
            PPR_TEST_ASSERT(not unowned.isOwned());
            const mem::SharedBuffer owned = unowned.makeOwned();
            PPR_TEST_ASSERT(owned.isValid());
            PPR_TEST_ASSERT(std::ranges::equal(owned.getBufferData(), std::span<const std::byte>{kTestBytes}));
        };

        PPR_UNIT_TEST(shared_subspan_out_of_range_throws, UnitTest::expect_fail) {
            mem::UniqueBuffer unique = makeUnique(4u);
            const mem::SharedBuffer base = toShared(unique);
            (void) base.subspan(99u);
        };

        // ------------------------------------------------------------------
        // WeakSharedBuffer
        // ------------------------------------------------------------------

        PPR_UNIT_TEST(weak_default_pin_is_invalid) {
            const mem::WeakSharedBuffer empty{};
            const mem::SharedBuffer pinned = empty.pin();
            PPR_TEST_ASSERT(not pinned.isValid());
        };

        PPR_UNIT_TEST(weak_pin_shares_parent_data) {
            mem::UniqueBuffer unique = makeUnique(4u);
            writeBytes(unique, kTestBytes);
            mem::SharedBuffer shared = toShared(unique);

            const mem::WeakSharedBuffer weak{shared};
            const mem::SharedBuffer pinned = weak.pin();
            PPR_TEST_ASSERT(pinned.isValid());
            PPR_TEST_ASSERT(pinned == shared);
            PPR_TEST_ASSERT(pinned.getBufferData().data() == shared.getBufferData().data());
        };

        PPR_UNIT_TEST(weak_assign_and_reset) {
            mem::UniqueBuffer unique = makeUnique(2u);
            mem::SharedBuffer shared = toShared(unique);

            mem::WeakSharedBuffer weak{};
            weak = shared;
            PPR_TEST_ASSERT(weak.pin().isValid());

            weak.reset();
            PPR_TEST_ASSERT(not weak.pin().isValid());
            PPR_TEST_ASSERT(shared.isValid());
        };

        PPR_UNIT_TEST(weak_parent_drop_invalidates_pin) {
            const mem::WeakSharedBuffer weak = [] {
                mem::UniqueBuffer unique = makeUnique(2u);
                mem::SharedBuffer shared = toShared(unique);
                return mem::WeakSharedBuffer{shared};
            }();
            PPR_TEST_ASSERT(not weak.pin().isValid());
        };

        PPR_UNIT_TEST(weak_move_and_swap) {
            mem::UniqueBuffer unique = makeUnique(2u);
            mem::SharedBuffer shared = toShared(unique);

            mem::WeakSharedBuffer source{shared};
            mem::WeakSharedBuffer destination{std::move(source)};
            PPR_TEST_ASSERT(not source.pin().isValid());
            PPR_TEST_ASSERT(destination.pin().isValid());

            mem::WeakSharedBuffer other{};
            swap(destination, other);
            PPR_TEST_ASSERT(not destination.pin().isValid());
            PPR_TEST_ASSERT(other.pin() == shared);
        };

        PPR_UNIT_TEST(weak_subspan_keeps_slice_alive) {
            mem::WeakSharedBuffer weak{};
            {
                mem::UniqueBuffer unique = makeUnique(4u);
                mem::SharedBuffer base = toShared(unique);
                const mem::SharedBuffer slice = base.subspan(0u, 2u);
                weak = slice;
                PPR_TEST_ASSERT(weak.pin().isValid());
            }
            PPR_TEST_ASSERT(not weak.pin().isValid());
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest buffer = UnitTest::Named("buffer") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Buffer::unique_default_is_empty,
            detail::Buffer::unique_best_fit_allocate_zero_is_invalid,
            detail::Buffer::unique_allocation_ctor_stateless_roundtrip,
            detail::Buffer::unique_allocation_ctor_stateful,
            detail::Buffer::unique_immutable_owner_rejects_mutable_access,
            detail::Buffer::unique_clone_empty_is_invalid,
            detail::Buffer::unique_lazy_allocate_stays_unmaterialized,
            detail::Buffer::unique_lazy_materialization_failures_are_invalid,
            detail::Buffer::unique_lazy_move_to_shared_materializes_readable_data,
            detail::Buffer::unique_lazy_move_to_shared_propagates_materialization_failure,
            detail::Buffer::unique_scratch_is_lazy_and_empty_clone_is_invalid,
            detail::Buffer::unique_make_owned_passthrough_moves_owned,
            detail::Buffer::unique_move_to_shared_transfers_ownership,
            detail::Buffer::unique_move_assign_and_swap,
            detail::Buffer::unique_map_file_error_paths,
            detail::Buffer::shared_default_is_empty,
            detail::Buffer::shared_move_to_shared_and_copy_share_data,
            detail::Buffer::shared_is_unique_owned_mutable_matrix,
            detail::Buffer::shared_make_owned_passthrough_on_owned,
            detail::Buffer::shared_move_to_unique_takes_single_owned_mutable,
            detail::Buffer::shared_subspan_empty_and_out_of_range,
            detail::Buffer::shared_subspan_flags_and_parent_lifetime,
            detail::Buffer::shared_equality_ordering_hash_and_swap,
            detail::Buffer::shared_map_file_error_paths,
            detail::Buffer::unique_map_file_roundtrip_reads_bytes,
            detail::Buffer::shared_map_file_roundtrip_reads_bytes,
            detail::Buffer::unique_bestfit_clone_nonempty,
            detail::Buffer::unique_scratch_view_nonempty,
            detail::Buffer::unique_make_owned_immutable_clone,
            detail::Buffer::shared_make_owned_unowned_clone,
            detail::Buffer::shared_subspan_out_of_range_throws,
            detail::Buffer::weak_default_pin_is_invalid,
            detail::Buffer::weak_pin_shares_parent_data,
            detail::Buffer::weak_assign_and_reset,
            detail::Buffer::weak_parent_drop_invalidates_pin,
            detail::Buffer::weak_move_and_swap,
            detail::Buffer::weak_subspan_keeps_slice_alive,
        });
    };

    const UnitTest &bufferTests() noexcept {
        return buffer;
    }
} // namespace pP::tests
