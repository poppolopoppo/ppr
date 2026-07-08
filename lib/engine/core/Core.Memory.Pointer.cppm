module;
#include "pP/Macros.h"
export module engine.core:memory.pointer;

import std;
import :assert;

export namespace pP {
#if PPR_ENABLE_DEBUG
    // ------------------------------------------------------------------
    // Debug mode: lifetime safety checks **ENABLED**
    // ------------------------------------------------------------------

    class safe_object {
    public:
        safe_object() noexcept = default;

        ~safe_object() {
            PPR_ASSERT(m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Checkable destroyed while CheckedPtr references still exist!");
        }

        // Relocation/copying changes object identity: the new instance starts
        // unobserved, and the source must not be observed at the time of the op.
        safe_object(safe_object &&other) noexcept {
            PPR_ASSERT(other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Source of move construction is still observed by a CheckedPtr!");
        }

        safe_object &operator=(safe_object &&other) noexcept {
            if (this != &other) {
                PPR_ASSERT(m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                    "Target of move assignment is still observed by a CheckedPtr!");
                PPR_ASSERT(other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                    "Source of move assignment is still observed by a CheckedPtr!");
            }
            return *this;
        }

        safe_object(const safe_object &other) noexcept {
            PPR_ASSERT(other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Source of copy construction is still observed by a CheckedPtr!");
        }

        safe_object &operator=(const safe_object &other) noexcept {
            if (this != &other) {
                PPR_ASSERT(m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                    "Target of copy assignment is still observed by a CheckedPtr!");
                PPR_ASSERT(other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                    "Source of copy assignment is still observed by a CheckedPtr!");
            }
            return *this;
        }

        void incSafeRef() const noexcept {
            m_safe_ref_count.fetch_add(1, std::memory_order_relaxed);
        }

        void decSafeRef() const noexcept {
            [[maybe_unused]] const size_t prev = m_safe_ref_count.fetch_sub(1, std::memory_order_relaxed);
            PPR_ASSERT(prev != 0 && "Debug ref count underflow!");
        }

        PPR_FORCE_INLINE friend void incSafeRefIFP(const safe_object *ptr) noexcept {
            if (ptr) [[likely]] {
                ptr->incSafeRef();
            }
        }

        PPR_FORCE_INLINE friend void decSafeRefIFP(const safe_object *ptr) noexcept {
            if (ptr) [[likely]] {
                ptr->decSafeRef();
            }
        }

    private:
        mutable std::atomic<size_t> m_safe_ref_count{0};
    };

    template<typename T>
    class [[nodiscard]] safe_ptr {
        template<typename U>
        friend class safe_ptr;

        explicit safe_ptr(std::in_place_t, T *const ptr) noexcept
            : m_ptr(ptr) {
        }

    public:
        safe_ptr() noexcept = default;

        explicit safe_ptr(T *const ptr) noexcept
            : m_ptr(ptr) {
            if (m_ptr) [[likely]] {
                m_ptr->incSafeRef();
            }
        }

        ~safe_ptr() noexcept {
            static_assert(std::is_base_of_v<safe_object, T>, "safe_ptr requires safe_object base");
            if (m_ptr) {
                m_ptr->decSafeRef();
            }

            m_ptr = nullptr;
        }

        safe_ptr(const safe_ptr &other) noexcept
            : m_ptr(other.m_ptr) {
            if (m_ptr) [[likely]] {
                m_ptr->incSafeRef();
            }
        }

        safe_ptr &operator=(const safe_ptr &other) noexcept {
            if (this != &other) {
                if (m_ptr) {
                    m_ptr->decSafeRef();
                }

                m_ptr = other.m_ptr;

                if (m_ptr) {
                    m_ptr->incSafeRef();
                }
            }
            return *this;
        }

        safe_ptr(safe_ptr &&other) noexcept : m_ptr(other.m_ptr) {
            other.m_ptr = nullptr;
        }

        safe_ptr &operator=(safe_ptr &&other) noexcept {
            if (this != &other) {
                if (m_ptr) {
                    m_ptr->decSafeRef();
                }

                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        safe_ptr &operator=(std::nullptr_t) noexcept {
            if (m_ptr) {
                m_ptr->decSafeRef();
            }

            m_ptr = nullptr;
            return *this;
        }

        [[nodiscard]] T *operator->() const noexcept {
            static_assert(std::is_base_of_v<safe_object, T>, "safe_ptr requires safe_object base");
            PPR_ASSERT(m_ptr != nullptr);
            return m_ptr;
        }

        [[nodiscard]] T &operator*() const noexcept {
            static_assert(std::is_base_of_v<safe_object, T>, "safe_ptr requires safe_object base");
            PPR_ASSERT(m_ptr != nullptr);
            return *m_ptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_ptr != nullptr;
        }

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return m_ptr != nullptr;
        }

        [[nodiscard]] constexpr T *get() const noexcept {
            return m_ptr;
        }

        template<typename BaseT>
            requires std::is_base_of_v<BaseT, T>
        [[nodiscard]] constexpr safe_ptr<BaseT> upcast() && noexcept {
            auto *const raw = static_cast<BaseT *>(m_ptr);
            m_ptr = nullptr;
            return safe_ptr<BaseT>(raw);
        }

        friend void swap(safe_ptr &lhs, safe_ptr &rhs) noexcept {
            std::swap(lhs.m_ptr, rhs.m_ptr);
        }

        [[nodiscard]] friend bool operator==(const safe_ptr &lhs, T *rhs) noexcept {
            return lhs.m_ptr == rhs;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, T *rhs) noexcept {
            return lhs.m_ptr <=> rhs;
        }

        [[nodiscard]] friend bool operator==(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
            return lhs.m_ptr == rhs.m_ptr;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
            return lhs.m_ptr <=> rhs.m_ptr;
        }

        template<typename DerivedT>
            requires std::is_base_of_v<T, DerivedT>
        [[nodiscard]] friend safe_ptr<DerivedT> checked_cast(const safe_ptr &safe) noexcept {
            return safe_ptr<DerivedT>(checked_cast<DerivedT>(safe.m_ptr));
        }

    private:
        T *m_ptr{};
    };

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    safe_ptr(T *) -> safe_ptr<T>;

#else
    // ------------------------------------------------------------------
    // Release mode: lifetime safety checks **DISABLED**, revert to raw pointer
    // ------------------------------------------------------------------

    class safe_object {
    public:
        friend constexpr void incSafeRefIFP(const safe_object *) noexcept {
        }

        friend constexpr void decSafeRefIFP(const safe_object *) noexcept {
        }
    };

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    using safe_ptr = std::add_pointer_t<T>;

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    [[nodiscard]] safe_ptr<T> safe_ptr(T *const ptr) noexcept {
        return ptr;
    }
#endif
}
