module;
#include "pP/Macros.h"

module engine.core;

import :enums;
import :memory.buffer;
import :types;
import :utility;

import std;


namespace pP::mem {
    // ------------------------------------------------------------------
    // BufferOwner owns an allocation & deleter to deallocate when destroyed
    // ------------------------------------------------------------------

    static_assert(pP::details::TEnumFlags<details::BufferOwner::EFlags>);

    details::BufferOwner::BufferOwner(const MutableBufferView mutable_view) noexcept
        : m_mutable_storage(mutable_view.data()),
          m_size_bytes(mutable_view.size_bytes()),
          m_flags(materialized) {
    }

    details::BufferOwner::BufferOwner(const SharedBufferView immutable_view) noexcept
        : m_immutable_storage(immutable_view.data()),
          m_size_bytes(immutable_view.size_bytes()),
          m_flags(immutable | materialized) {
    }

    details::BufferOwner::BufferOwner(void *const mutable_data, const std::size_t size_bytes,
                                      const Deleter deleter, void *const user_data,
                                      const EFlags flags) noexcept
        : m_mutable_storage(mutable_data),
          m_size_bytes(size_bytes),
          m_flags(flags + materialized),
          m_delete(deleter), m_user_data(user_data) {
    }

    details::BufferOwner::BufferOwner(const void *const immutable_data, const std::size_t size_bytes,
                                      const Deleter deleter, void *const user_data,
                                      const EFlags flags) noexcept
        : m_immutable_storage(immutable_data),
          m_size_bytes(size_bytes),
          m_flags(flags + materialized + immutable),
          m_delete(deleter),
          m_user_data(user_data) {
    }

    details::BufferOwner::BufferOwner(const Materializer materialize, const std::size_t size_bytes,
                                      const Deleter deleter, void *const user_data,
                                      const EFlags flags) noexcept
        : m_materialize(materialize),
          m_size_bytes(size_bytes),
          m_flags(flags - materialized),
          m_delete(deleter),
          m_user_data(user_data) {
    }

    details::BufferOwner &details::BufferOwner::operator =(BufferOwner &&other) noexcept {
        if (this == &other) [[unlikely]] {
            return *this;
        }

        reset();

        if (other.isImmutable()) {
            PPR_ASSERT(other.isMaterialized());
            m_immutable_storage = other.m_immutable_storage;
        } else if (other.isMaterialized()) {
            m_mutable_storage = other.m_mutable_storage;
        } else {
            m_materialize = other.m_materialize;
        }

        m_size_bytes = other.m_size_bytes;
        m_delete = other.m_delete;
        m_user_data = other.m_user_data;
        m_flags = other.m_flags;

        other.m_mutable_storage = nullptr;
        other.m_size_bytes = 0u;
        other.m_delete = nullptr;
        other.m_user_data = nullptr;
        other.m_flags = materialized;
        return *this;
    }

    Expected<MutableBufferView> details::BufferOwner::getMutableData() noexcept {
        if (isImmutable()) {
            return std::unexpected(make_error_code(std::errc::operation_not_permitted));
        }
        if (not isMaterialized()) [[unlikely]] {
            if (const std::error_code error_code = materialize()) [[unlikely]] {
                return std::unexpected(make_error_code(error_code));
            }
        }
        return MutableBufferView{static_cast<std::byte *>(m_mutable_storage), m_size_bytes};
    }

    std::error_code details::BufferOwner::materialize() {
        if (not isMaterialized()) [[unlikely]] {
            if (m_materialize == nullptr) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }

            const std::size_t requested_size_bytes = m_size_bytes;
            try {
                if (const auto expected = m_materialize(m_user_data, requested_size_bytes); expected.has_value()) {
                    if (expected->ptr == nullptr or expected->count < requested_size_bytes) [[unlikely]] {
                        if (expected->ptr != nullptr and m_delete != nullptr) {
                            try {
                                m_delete(m_user_data, *expected);
                            } catch (...) {
                            }
                        }
                        return make_error_code(std::errc::not_enough_memory);
                    }

                    if (isImmutable()) {
                        m_immutable_storage = expected->ptr;
                    } else {
                        m_mutable_storage = expected->ptr;
                    }

                    m_size_bytes = expected->count;
                    m_flags = m_flags + materialized;
                } else {
                    return expected.error();
                }
            } catch (...) {
                return make_error_code(std::errc::not_enough_memory);
            }
        }
        return default_value_v;
    }

    void details::BufferOwner::reset() {
        if (isMaterialized() and m_delete != nullptr) {
            m_delete(m_user_data, {.ptr = m_mutable_storage, .count = m_size_bytes});
        }

        m_mutable_storage = nullptr;
        m_size_bytes = 0u;
        m_delete = nullptr;
        m_user_data = nullptr;
        m_flags = none;
    }

    // ------------------------------------------------------------------
    // UniqueBuffer is mutable and can't be shared
    // ------------------------------------------------------------------

    UniqueBuffer::UniqueBuffer(BufferOwner &&owner)
        : m_owner(std::make_unique<BufferOwner>(std::move(owner))) {
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    Expected<MutableBufferView> UniqueBuffer::getMutableData() noexcept {
        if (BufferOwner *const p_owner = m_owner.get()) [[likely]] {
            return p_owner->getMutableData();
        }
        return std::unexpected{make_error_code(std::errc::not_connected)};
    }

    MutableBufferView UniqueBuffer::getBufferData() const noexcept {
        if (BufferOwner *const p_owner = m_owner.get()) [[likely]] {
            return p_owner->getBufferData();
        }
        return default_value_v;
    }

    UniqueBuffer UniqueBuffer::makeOwned() &&/* only on ref values */ {
        if (isOwned()) {
            return std::move(*this);
        }
        if (BufferOwner *const p_owner = m_owner.get()) {
            if (const Expected<MutableBufferView> buffer_data = p_owner->getMutableData(); buffer_data.has_value()) {
                return clone(*buffer_data);
            }
            return clone(std::as_const(*p_owner).getBufferData());
        }
        return default_value_v;
    }

    std::error_code UniqueBuffer::moveToShared(SharedBuffer *const out_write_ref) {
        if (BufferOwner *const p_owner = m_owner.get()) [[likely]] {
            if (const std::error_code error_code = p_owner->materialize()) [[unlikely]] {
                return error_code;
            }
            try {
                std::shared_ptr<BufferOwner> shared_owner{std::move(m_owner)};
                *out_write_ref = SharedBuffer{std::move(shared_owner)};
                return default_value_v;
            } catch (const std::bad_alloc &) {
                return make_error_code(std::errc::not_enough_memory);
            }
        }
        return make_error_code(std::errc::not_connected);
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    std::error_code UniqueBuffer::materialize() {
        if (BufferOwner *const p_owner = m_owner.get()) [[likely]] {
            return p_owner->materialize();
        }
        return default_value_v;
    }

    void UniqueBuffer::reset() {
        m_owner.reset();
    }

    UniqueBuffer UniqueBuffer::allocate(const std::size_t size_bytes) {
        if (size_bytes == 0u) [[unlikely]] {
            return default_value_v;
        }

        static_assert(Allocator<HugePage>::is_stateless_v);
        if (alignForward(size_bytes, HugePage::block_size_v) == size_bytes) {
            return allocate(HugePage{}, size_bytes);
        }

        static_assert(Allocator<SmallPage>::is_stateless_v);
        if (alignForward(size_bytes, SmallPage::block_size_v) == size_bytes) {
            return allocate(SmallPage{}, size_bytes);
        }

        static_assert(Allocator<GPA>::is_stateless_v);
        return allocate(GPA{}, size_bytes);
    }

    UniqueBuffer UniqueBuffer::clone(const SharedBufferView raw_data) {
        if (not raw_data.empty()) {
            UniqueBuffer cpy = allocate(raw_data.size_bytes());
            if (Expected<MutableBufferView> buffer_data = cpy.getMutableData(); buffer_data.has_value()) {
                std::ranges::copy(raw_data, buffer_data->begin());
                return cpy;
            }
        }
        return default_value_v;
    }

    UniqueBuffer UniqueBuffer::scratch(const std::size_t size_bytes) {
        return allocate(ScratchPad{}, size_bytes);
    }

    UniqueBuffer UniqueBuffer::scratch(const SharedBufferView raw_data) {
        return clone(ScratchPad{}, raw_data);
    }

    // ------------------------------------------------------------------
    // SharedBuffer is immutable and can be assigned without copying content
    // ------------------------------------------------------------------

    SharedBuffer::SharedBuffer(BufferOwner &&owner)
        : m_owner(std::make_shared<BufferOwner>(std::move(owner))) {
    }

    bool SharedBuffer::isUniqueOwnedMutable() const noexcept {
        if (const BufferOwner *const p_owner = m_owner.get()) {
            if (p_owner->isOwned() and not p_owner->isImmutable()) {
                return m_owner.use_count() == 1;
            }
        }
        return false;
    }

    SharedBufferView SharedBuffer::getBufferData() const noexcept {
        if (const BufferOwner *const p_owner = m_owner.get()) {
            return p_owner->getBufferData();
        }
        return default_value_v;
    }

    SharedBuffer SharedBuffer::makeOwned() const & {
        if (isOwned()) {
            return *this;
        }
        if (BufferOwner *const p_owner = m_owner.get()) {
            if (const Expected<MutableBufferView> buffer_data = p_owner->getMutableData(); buffer_data.has_value()) {
                return clone(*buffer_data);
            }
            return clone(std::as_const(*p_owner).getBufferData());
        }
        return default_value_v;
    }

    SharedBuffer SharedBuffer::makeOwned() && {
        if (isOwned()) {
            return std::move(*this);
        }
        if (BufferOwner *const p_owner = m_owner.get()) {
            if (const Expected<MutableBufferView> buffer_data = p_owner->getMutableData(); buffer_data.has_value()) {
                return clone(*buffer_data);
            }
            return clone(std::as_const(*p_owner).getBufferData());
        }
        return default_value_v;
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    UniqueBuffer SharedBuffer::moveToUnique() {
        if (isUniqueOwnedMutable()) {
            UniqueBuffer unique_buffer{std::move(*m_owner)};
            m_owner.reset();
            return unique_buffer;
        }
        return UniqueBuffer::clone(getBufferData());
    }

    SharedBuffer SharedBuffer::subspan(const std::size_t offset, const std::size_t count) const {
        if (not m_owner) [[unlikely]] {
            return default_value_v;
        }

        const SharedBufferView source_view{getBufferData()};
        const std::size_t size = source_view.size_bytes();
        if (offset > size or (count != std::dynamic_extent and count > size - offset)) [[unlikely]] {
            throw std::out_of_range{std::format("SharedBuffer::subspan out of range: offset {} count {} size {}", offset, count, size)};
        }

        const SharedBufferView buffer_view{source_view.subspan(offset, count)};
        if (buffer_view.empty()) [[unlikely]] {
            return default_value_v;
        }

        auto outer_owner = std::make_unique<SharedBuffer>(*this);

        BufferOwner inner_owner{
            buffer_view.data(), buffer_view.size_bytes(),
            [](void *user_data, std::allocation_result<void *>) {
                std::default_delete<SharedBuffer>{}(static_cast<SharedBuffer *>(user_data));
            },
            outer_owner.release(),
            BufferOwner::immutable | BufferOwner::materialized
        };
        PPR_ASSERT(not inner_owner.isOwned());

        return SharedBuffer(std::move(inner_owner));
    }

    void SharedBuffer::reset() {
        m_owner.reset();
    }

    SharedBuffer SharedBuffer::clone(const SharedBufferView raw_data) {
        SharedBuffer shared_buffer{};
        if (not UniqueBuffer::clone(raw_data).moveToShared(&shared_buffer)) {
            return shared_buffer;
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // IO file mapping helpers
    // ------------------------------------------------------------------

    [[nodiscard]] static Expected<details::BufferOwner> mapBufferOwnerToFile_(const std::filesystem::path &file_path, const hal::io::OpenFlags flags) {
        auto mapped_file = io::mapFile(file_path, flags);
        if (not mapped_file.has_value()) {
            return std::unexpected{mapped_file.error()};
        }

        const MutableBufferView mapped_view{mapped_file->span()};

        return details::BufferOwner{
            mapped_view.data(),
            mapped_view.size_bytes(),
            [](void *const map_handle, std::allocation_result<void *>) {
                hal::io::unmapFile(map_handle);
            },
            mapped_file->discard(),
            details::BufferOwner::materialized | details::BufferOwner::owned
        };
    }

    Expected<UniqueBuffer> UniqueBuffer::mapFile(const std::filesystem::path &file_path, const hal::io::OpenFlags flags) {
        if (Expected<BufferOwner> owner = mapBufferOwnerToFile_(file_path, flags); owner.has_value()) {
            return UniqueBuffer(std::move(*owner));
        } else {
            return std::unexpected{owner.error()};
        }
    }

    std::error_code UniqueBuffer::mapFile(const std::filesystem::path &file_path, UniqueBuffer *out_write_ref, const hal::io::OpenFlags flags) {
        if (Expected<UniqueBuffer> mapped = mapFile(file_path, flags); mapped.has_value()) {
            *out_write_ref = std::move(*mapped);
            return default_value_v;
        } else {
            return mapped.error();
        }
    }

    Expected<SharedBuffer> SharedBuffer::mapFile(const std::filesystem::path &file_path) {
        if (Expected<BufferOwner> owner = mapBufferOwnerToFile_(file_path, {}); owner.has_value()) {
            return SharedBuffer(std::move(*owner));
        } else {
            return std::unexpected{owner.error()};
        }
    }

    std::error_code SharedBuffer::mapFile(const std::filesystem::path &file_path, SharedBuffer *out_write_ref) {
        if (Expected<SharedBuffer> mapped = mapFile(file_path); mapped.has_value()) {
            *out_write_ref = std::move(*mapped);
            return default_value_v;
        } else {
            return mapped.error();
        }
    }
}
