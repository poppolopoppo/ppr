module;
#include "pP/Macros.h"

export module engine.core:memory.buffer;

import :assert;
import :enums;
import :hal;
import :hashing;
import :memory;
import :memory.allocator;
import :memory.pointer;
import :types;
import :utility;

import std;

export namespace pP::mem {
    using MutableBufferView = std::span<std::byte>;
    using SharedBufferView = std::span<const std::byte>;

    // ------------------------------------------------------------------
    // BufferOwner owns an allocation & deleter to deallocate when destroyed
    // ------------------------------------------------------------------

    namespace details {
        class BufferOwner final : public safe_object {
        public:
            using Deleter = void (*)(void *user_data, std::allocation_result<void *>);
            using Materializer = Expected<std::allocation_result<void *> > (*)(void *user_data, std::size_t size_bytes);

            enum EFlags : std::size_t {
                none = 0u,

                immutable = 0b001u,
                materialized = 0b010u,
                owned = 0b100u,

                all = immutable | materialized | owned
            };

            BufferOwner(const BufferOwner &) = delete;

            BufferOwner &operator =(const BufferOwner &) = delete;

            BufferOwner(BufferOwner &&other) noexcept { // NOLINT(*-pro-type-member-init)
                operator=(std::move(other));
            }

            BufferOwner &operator =(BufferOwner &&other) noexcept;

            ~BufferOwner() {
                reset();
            }

            explicit BufferOwner(MutableBufferView mutable_view) noexcept;

            explicit BufferOwner(SharedBufferView immutable_view) noexcept;

            BufferOwner(void *mutable_data, std::size_t size_bytes,
                        Deleter deleter, void *user_data = nullptr,
                        EFlags flags = none) noexcept;

            BufferOwner(const void *immutable_data, std::size_t size_bytes,
                        Deleter deleter, void *user_data = nullptr,
                        EFlags flags = none) noexcept;

            BufferOwner(Materializer materialize, std::size_t size_bytes,
                        Deleter deleter, void *user_data = nullptr,
                        EFlags flags = none) noexcept;

            BufferOwner(const std::allocation_result<void *> alloc,
                        const Deleter deleter, void *user_data = nullptr,
                        const EFlags flags = none) noexcept
                : BufferOwner(alloc.ptr, alloc.count, deleter, user_data, flags) {
            }

            BufferOwner(const std::allocation_result<const void *> alloc,
                        const Deleter deleter, void *user_data = nullptr,
                        const EFlags flags = none) noexcept
                : BufferOwner(alloc.ptr, alloc.count, deleter, user_data, flags) {
            }

            template<typename T> requires std::is_trivially_destructible_v<T>
            BufferOwner(std::allocation_result<T *> alloc,
                        Deleter deleter, void *user_data = nullptr,
                        EFlags flags = none) noexcept
                : BufferOwner(alloc.ptr, alloc.count * sizeof(T), deleter, user_data, flags) {
            }

            [[nodiscard]] bool isImmutable() const noexcept { return m_flags & immutable; }
            [[nodiscard]] bool isMaterialized() const noexcept { return m_flags & materialized; }
            [[nodiscard]] bool isOwned() const noexcept { return m_flags & owned; }

            [[nodiscard]] Expected<MutableBufferView> getMutableData() noexcept;

            [[nodiscard]] MutableBufferView getBufferData() noexcept {
                if (const Expected<MutableBufferView> buffer_data = getMutableData(); buffer_data.has_value()) {
                    return *buffer_data;
                }
                return default_value_v;
            }

            [[nodiscard]] SharedBufferView getBufferData() const noexcept {
                if (isMaterialized()) [[likely]] {
                    return {static_cast<const std::byte *>(isImmutable() ? m_immutable_storage : m_mutable_storage), m_size_bytes};
                }
                return default_value_v;
            }

            [[nodiscard]] std::error_code materialize();

            void reset();

        private:
            union {
                void *m_mutable_storage{};
                const void *m_immutable_storage;
                Materializer m_materialize;
            };

            std::size_t m_size_bytes: bit_count_v<std::size_t> - 3u {0u};
            EFlags m_flags: 3u {none};

            Deleter m_delete{};
            void *m_user_data{};
        };
    }

    // ------------------------------------------------------------------
    // UniqueBuffer is mutable and can't be shared
    // ------------------------------------------------------------------

    class SharedBuffer;

    class UniqueBuffer final {
        using BufferOwner = details::BufferOwner;
        using EFlags = details::BufferOwner::EFlags;

        std::unique_ptr<BufferOwner> m_owner{};

    public:
        UniqueBuffer() noexcept = default;

        explicit UniqueBuffer(BufferOwner &&owner);

        explicit UniqueBuffer(std::unique_ptr<BufferOwner> &&owner_ptr) noexcept
            : m_owner(std::move(owner_ptr)) {
        }

        UniqueBuffer(const UniqueBuffer &) = delete;

        UniqueBuffer &operator =(const UniqueBuffer &) = delete;

        UniqueBuffer(UniqueBuffer &&) noexcept = default;

        UniqueBuffer &operator =(UniqueBuffer &&) noexcept = default;

        [[nodiscard]] bool isValid() const noexcept { return !!m_owner; }

        [[nodiscard]] explicit operator bool() const noexcept { return !!m_owner; }

        [[nodiscard]] bool isImmutable() const noexcept { return not m_owner or m_owner->isImmutable(); }
        [[nodiscard]] bool isMaterialized() const noexcept { return not m_owner or m_owner->isMaterialized(); }
        [[nodiscard]] bool isOwned() const noexcept { return not m_owner or m_owner->isOwned(); }

        [[nodiscard]] Expected<MutableBufferView> getMutableData() noexcept;

        [[nodiscard]] MutableBufferView getBufferData() const noexcept;

        [[nodiscard]] UniqueBuffer makeOwned() &&/* only on ref values */;

        [[nodiscard]] std::error_code moveToShared(SharedBuffer *out_write_ref);

        [[nodiscard]] std::error_code materialize();

        void reset();

        friend void swap(UniqueBuffer &lhs, UniqueBuffer &rhs) noexcept {
            std::swap(lhs.m_owner, rhs.m_owner);
        }

        template<typename T, details::TAllocator AllocatorT, std::align_val_t AlignmentV>
            requires std::is_trivially_destructible_v<T> and
                     Allocation<T, AllocatorT, AlignmentV>::is_stateless_v
        explicit UniqueBuffer(Allocation<T, AllocatorT, AlignmentV> &&allocation) noexcept
            : UniqueBuffer(BufferOwner(
                allocation.discard(),
                [](void *const, const std::allocation_result<void *> alloc) {
                    AllocatorT{}.deallocateRaw(alloc.ptr, alloc.count, AlignmentV);
                },
                nullptr,
                (std::is_const_v<T> ? EFlags::immutable : EFlags::none) |
                EFlags::materialized | EFlags::owned)) {
        }

        template<typename T, details::TAllocator AllocatorT, std::align_val_t AlignmentV>
            requires (std::is_trivially_destructible_v<T> and
                      not (Allocation<T, AllocatorT, AlignmentV>::is_stateless_v))
        UniqueBuffer(Allocation<T, AllocatorT, AlignmentV> &&allocation, AllocatorT &allocator) noexcept
            : UniqueBuffer(BufferOwner(
                allocation.discard(),
                [](void *const al, const std::allocation_result<void *> alloc) {
                    static_cast<AllocatorT *>(al)->deallocateRaw(alloc.ptr, alloc.count, AlignmentV);
                },
                &allocator,
                (std::is_const_v<T> ? EFlags::immutable : EFlags::none) |
                EFlags::materialized | EFlags::owned)) {
        }

        template<details::TAllocator AllocatorT>
        [[nodiscard]] static UniqueBuffer allocate(AllocatorT &allocator, const std::size_t size_bytes) noexcept {
            return UniqueBuffer(BufferOwner(
                [](void *const al, const std::size_t sz) -> Expected<std::allocation_result<void *> > {
                    if constexpr (Allocator<AllocatorT>::is_stateless_v) {
                        return AllocatorT{}.allocateRaw(sz, max_align_v);
                    } else {
                        return static_cast<AllocatorT *>(al)->allocateRaw(sz, max_align_v);
                    }
                },
                size_bytes,
                [](void *const al, const std::allocation_result<void *> alloc) {
                    if constexpr (Allocator<AllocatorT>::is_stateless_v) {
                        AllocatorT{}.deallocateRaw(alloc.ptr, alloc.count, max_align_v);
                    } else {
                        static_cast<AllocatorT *>(al)->deallocateRaw(alloc.ptr, alloc.count, max_align_v);
                    }
                },
                Allocator<AllocatorT>::is_stateless_v ? nullptr : &allocator,
                EFlags::owned));
        }

        template<details::TAllocator AllocatorT = GPA>
            requires Allocator<std::remove_cvref_t<AllocatorT> >::is_stateless_v
        [[nodiscard]] static UniqueBuffer allocate(AllocatorT &&allocator, const std::size_t size_bytes) noexcept {
            AllocatorT al{std::forward<AllocatorT>(allocator)};
            return allocate(al, size_bytes);
        }

        template<details::TAllocator AllocatorT>
        [[nodiscard]] static UniqueBuffer clone(AllocatorT &allocator, const SharedBufferView raw_data) {
            if (not raw_data.empty()) {
                UniqueBuffer cpy = allocate(allocator, raw_data.size_bytes());
                if (Expected<MutableBufferView> buffer_data = cpy.getMutableData(); buffer_data.has_value()) {
                    std::ranges::copy(raw_data, buffer_data->begin());
                    return cpy;
                }
            }
            return default_value_v;
        }

        template<details::TAllocator AllocatorT>
            requires Allocator<std::remove_cvref_t<AllocatorT> >::is_stateless_v
        [[nodiscard]] static UniqueBuffer clone(AllocatorT &&allocator, const SharedBufferView raw_data) {
            AllocatorT al{std::forward<AllocatorT>(allocator)};
            return clone(al, raw_data);
        }

        /// allocate using best-fit allocator between GPA < SmallPage < HugePage
        [[nodiscard]] static UniqueBuffer allocate(std::size_t size_bytes);

        /// clone using best-fit allocator between GPA < SmallPage < HugePage
        [[nodiscard]] static UniqueBuffer clone(SharedBufferView raw_data);

        /// map file into a unique buffer for read/write
        [[nodiscard]] static Expected<UniqueBuffer> mapFile(const std::filesystem::path &file_path, hal::io::OpenFlags flags = {});

        /// map file into a unique buffer for read/write
        [[nodiscard]] static std::error_code mapFile(const std::filesystem::path &file_path, UniqueBuffer *out_write_ref, hal::io::OpenFlags flags = {});

        /// allocate from thread-local scratch pad arena
        [[nodiscard]] static UniqueBuffer scratch(std::size_t size_bytes);

        /// clone using thread-local scratch pad arena
        [[nodiscard]] static UniqueBuffer scratch(SharedBufferView raw_data);
    };

    // ------------------------------------------------------------------
    // SharedBuffer is immutable and can be assigned without copying content
    // ------------------------------------------------------------------

    class WeakSharedBuffer;

    class SharedBuffer final {
        using BufferOwner = details::BufferOwner;
        friend WeakSharedBuffer;

        std::shared_ptr<BufferOwner> m_owner{};

    public:
        SharedBuffer() noexcept = default;

        explicit SharedBuffer(BufferOwner &&owner);

        explicit SharedBuffer(std::shared_ptr<BufferOwner> shared_owner) noexcept
            : m_owner(std::move(shared_owner)) {
        }

        explicit SharedBuffer(const SharedBufferView raw_data)
            : SharedBuffer(BufferOwner(raw_data)) {
        }

        [[nodiscard]] bool isValid() const noexcept { return !!m_owner; }

        [[nodiscard]] explicit operator bool() const noexcept { return !!m_owner; }

        [[nodiscard]] bool isImmutable() const noexcept { return not m_owner or m_owner->isImmutable(); }
        [[nodiscard]] bool isMaterialized() const noexcept { return not m_owner or m_owner->isMaterialized(); }
        [[nodiscard]] bool isOwned() const noexcept { return not m_owner or m_owner->isOwned(); }

        [[nodiscard]] bool isUniqueOwnedMutable() const noexcept;

        [[nodiscard]] SharedBufferView getBufferData() const noexcept;

        [[nodiscard]] SharedBuffer makeOwned() const &/* only on const refs */;

        [[nodiscard]] SharedBuffer makeOwned() &&/* only on ref values */;

        [[nodiscard]] UniqueBuffer moveToUnique();

        [[nodiscard]] SharedBuffer subspan(std::size_t offset, std::size_t count = std::dynamic_extent) const;

        void reset();

        /// clone using best-fit allocator between GPA < SmallPage < HugePage
        [[nodiscard]] static SharedBuffer clone(SharedBufferView raw_data);

        /// map file into a read-only shared buffer
        [[nodiscard]] static Expected<SharedBuffer> mapFile(const std::filesystem::path &file_path);

        /// map file into a read-only shared buffer
        [[nodiscard]] static std::error_code mapFile(const std::filesystem::path &file_path, SharedBuffer *out_write_ref);

        [[nodiscard]] bool operator ==(const SharedBuffer &other) const noexcept {
            return m_owner == other.m_owner;
        }

        [[nodiscard]] std::strong_ordering operator <=>(const SharedBuffer &other) const noexcept {
            return m_owner <=> other.m_owner;
        }

        [[nodiscard]] friend hash_t hashValue(const SharedBuffer &buffer) noexcept {
            return hashValue(buffer.m_owner);
        }

        friend void swap(SharedBuffer &lhs, SharedBuffer &rhs) noexcept {
            std::swap(lhs.m_owner, rhs.m_owner);
        }
    };

    // ------------------------------------------------------------------
    // WeakSharedBuffer is immutable and won't keep the buffer alive
    // ------------------------------------------------------------------

    class WeakSharedBuffer final {
        using BufferOwner = details::BufferOwner;
        std::weak_ptr<BufferOwner> m_owner{};

    public:
        WeakSharedBuffer() noexcept = default;

        explicit WeakSharedBuffer(const SharedBuffer &shared_buffer) noexcept
            : m_owner(shared_buffer.m_owner) {
        }

        WeakSharedBuffer &operator =(const SharedBuffer &shared_buffer) noexcept {
            m_owner = shared_buffer.m_owner;
            return *this;
        }

        [[nodiscard]] SharedBuffer pin() const noexcept {
            return SharedBuffer{m_owner.lock()};
        }

        void reset() {
            m_owner.reset();
        }

        friend void swap(WeakSharedBuffer &lhs, WeakSharedBuffer &rhs) noexcept {
            std::swap(lhs.m_owner, rhs.m_owner);
        }
    };
}
