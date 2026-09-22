module;
#include "pP/Macros.h"
export module engine.core:memory.pointer;

import :containers;

import std;

#if PPR_ENABLE_DEBUG
import :assert;
#endif
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
import :containers.sparse_vector;
#endif

export namespace pP {
    class safe_object;

    namespace details {
        template<typename T>
        concept TSafeObject = std::is_base_of_v<safe_object, T>;
    }

#if PPR_ENABLE_DEBUG

    // ------------------------------------------------------------------
    // Debug mode: ref-count checks **ENABLED**; callstack map only with ASAN tracking
    // ------------------------------------------------------------------

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    using safe_referencer_key = SparseKeyId;
#endif

    class safe_object {
    protected:
        ~safe_object() noexcept(false);

    public:
        safe_object() noexcept = default;

        // Relocation/copying changes object identity: the new instance starts
        // unobserved, and the source must not be observed at the time of the op.
        // Deliberately non-noexcept: debug PPR_ASSERT throws; release is stateless.
        safe_object(safe_object &&other);

        safe_object &operator=(safe_object &&other);

        safe_object(const safe_object &other);

        safe_object &operator=(const safe_object &other);

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    safe_referencer_key incSafeRef(const void *derived) const noexcept;

    void decSafeRef(const void *derived, safe_referencer_key referencer_key) const noexcept;

    template<details::TSafeObject T>
    PPR_FORCE_INLINE friend std::optional<safe_referencer_key> incSafeRefIFP(const T *ptr) noexcept {
        if (ptr) [[likely]] {
            return ptr->incSafeRef(ptr);
        }
        return std::nullopt;
    }

    template<details::TSafeObject T>
    PPR_FORCE_INLINE friend void decSafeRefIFP(const T *ptr, std::optional<safe_referencer_key> &referencer_key) noexcept {
        if (ptr) [[likely]] {
            PPR_ASSERT(referencer_key.has_value());
            ptr->decSafeRef(ptr, *referencer_key);
            referencer_key.reset();
        }
    }
#else
    void incSafeRef(const void *derived) const noexcept;

    void decSafeRef(const void *derived) const noexcept;

    template<details::TSafeObject T>
    PPR_FORCE_INLINE friend void incSafeRefIFP(const T *ptr) noexcept {
        if (ptr) [[likely]] {
            ptr->incSafeRef(ptr);
        }
    }

    template<details::TSafeObject T>
    PPR_FORCE_INLINE friend void decSafeRefIFP(const T *ptr) noexcept {
        if (ptr) [[likely]] {
            ptr->decSafeRef(ptr);
        }
    }
#endif

    private:
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    struct Referencer {
        const void *m_derived{nullptr};
        std::stacktrace m_callstack{};
    };

    mutable std::shared_mutex m_referencer_barrier{};
    mutable SparseVector<Referencer> m_references{};
#endif

    mutable std::atomic<int> m_safe_ref_count{0};
    };

    template<typename T>
    class [[nodiscard]] safe_ptr {
        template<typename U>
        friend class safe_ptr;

        T *m_ptr{nullptr};
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    std::optional<safe_referencer_key> m_key{};

    explicit safe_ptr(std::in_place_t, T * const ptr, safe_referencer_key key)noexcept
            : m_ptr (ptr), m_key(key) {
    }
#else
    explicit safe_ptr(std::in_place_t, T * const ptr)noexcept
            : m_ptr (ptr){
    }
#endif

    public:
    safe_ptr() noexcept = default;

    // ReSharper disable once CppNonExplicitConvertingConstructor
    safe_ptr(T * const ptr)noexcept
            : m_ptr (ptr){
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    m_key=
#endif
    incSafeRefIFP (m_ptr);
        }

    // ReSharper disable once CppNonExplicitConvertingConstructor
    safe_ptr(const std::unique_ptr<std::remove_const_t<T> > &unique_ptr) noexcept
        : safe_ptr(unique_ptr.get()) {
    }

    ~safe_ptr() noexcept {
        static_assert(details::TSafeObject<T>, "safe_ptr requires safe_object base");
        reset();
    }

    safe_ptr(const safe_ptr &other) noexcept {
        reset(other.m_ptr);
    }

    safe_ptr &operator =(const safe_ptr &other) noexcept {
        if (this != &other) [[likely]] {
            reset(other.m_ptr);
        }
        return *this;
    }

    safe_ptr(safe_ptr &&other) noexcept
        : m_ptr(other.m_ptr)
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    , m_key (other.m_key)
#endif
    {
            other.m_ptr = nullptr;
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    other.m_key.reset();
#endif
    }

    safe_ptr &operator=(safe_ptr &&other) noexcept {
        if (this != &other) [[likely]] {
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    decSafeRefIFP(m_ptr, m_key);

    m_ptr= other.m_ptr;
    m_key= other.m_key;

    other.m_ptr=nullptr;
    other.m_key.reset();
#else
    decSafeRefIFP (m_ptr);
    m_ptr= other.m_ptr;
    other.m_ptr=nullptr;
#endif
    }
            return *this;
        }

    template<typename U>
        requires std::convertible_to<U *, T *>
    // ReSharper disable once CppNonExplicitConvertingConstructor
    safe_ptr(const safe_ptr<U> &other) noexcept
        : safe_ptr(other.m_ptr) {
    }

    template<typename U>
        requires std::convertible_to<U *, T *>
    safe_ptr &operator=(const safe_ptr<U> &other) noexcept {
        reset(other.m_ptr);
        return *this;
    }

    template<typename U>
        requires std::convertible_to<U *, T *>
    // ReSharper disable once CppNonExplicitConvertingConstructor
    safe_ptr(safe_ptr<U> &&other) noexcept
        : m_ptr(other.m_ptr)
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    , m_key (other.m_key)
#endif
    {
            other.m_ptr = nullptr;
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    other.m_key.reset();
#endif
    }

    template<typename U>
        requires std::convertible_to<U *, T *>
    safe_ptr &operator=(safe_ptr<U> &&other) noexcept {

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    decSafeRefIFP(m_ptr, m_key);

    m_ptr= other.m_ptr;
    m_key= other.m_key;

    other.m_ptr=nullptr;
    other.m_key.reset();
#else
    decSafeRefIFP (m_ptr);
    m_ptr= other.m_ptr;
    other.m_ptr=nullptr;
#endif
    return *this;
        }

    safe_ptr &operator=(T * const ptr)noexcept {
            reset(ptr);
            return *this;
        }

    safe_ptr &operator=(std::nullptr_t) noexcept {

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    decSafeRefIFP(m_ptr, m_key);
#else
    decSafeRefIFP (m_ptr);
#endif
    m_ptr=nullptr;
            return *this;
        }

    [[nodiscard]] T *operator->() const noexcept {
        static_assert(details::TSafeObject<T>, "safe_ptr requires safe_object base");
        PPR_ASSERT(m_ptr != nullptr);
        return m_ptr;
    }

    [[nodiscard]] T &operator*() const noexcept {
        static_assert(details::TSafeObject<T>, "safe_ptr requires safe_object base");
        PPR_ASSERT(m_ptr != nullptr);
        // ReSharper disable once CppDFANullDereference
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
        if (m_ptr == nullptr) [[unlikely]] {
            return safe_ptr<BaseT>{};
        }
        auto *const raw = static_cast<BaseT *>(m_ptr);
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    PPR_ASSERT(m_key.has_value());
    const safe_referencer_key key = *m_key;

    m_ptr=nullptr;
    m_key.reset();
            return safe_ptr<BaseT>(std::in_place, raw, key);
#else
    m_ptr=nullptr;
            return safe_ptr<BaseT>(std::in_place, raw);
#endif
    }

    void reset(T * const ptr = nullptr)noexcept {
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    decSafeRefIFP(m_ptr, m_key);
    m_ptr= ptr;
    m_key= incSafeRefIFP(m_ptr);
#else
    decSafeRefIFP (m_ptr);
    m_ptr= ptr;
    incSafeRefIFP (m_ptr);
#endif
    }

    [[nodiscard]] friend bool operator==(const safe_ptr &lhs, const T *rhs) noexcept {
        return lhs.m_ptr == rhs;
    }

    [[nodiscard]] friend bool operator==(const T *lhs, const safe_ptr &rhs) noexcept {
        return lhs == rhs.m_ptr;
    }

    [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, const T *rhs) noexcept {
        return lhs.m_ptr <=> rhs;
    }

    [[nodiscard]] friend bool operator==(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
        return lhs.m_ptr == rhs.m_ptr;
    }

    [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
        return lhs.m_ptr <=> rhs.m_ptr;
    }

    friend void swap(safe_ptr &lhs, safe_ptr &rhs) noexcept {
        std::swap(lhs.m_ptr, rhs.m_ptr);
#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    std::swap (lhs.m_key, rhs.m_key);
#endif
    }

    template<typename DerivedT>
        requires std::is_base_of_v<T, DerivedT>
    [[nodiscard]] friend safe_ptr<DerivedT> checked_cast(const safe_ptr &safe) noexcept {
        return safe_ptr<DerivedT>(checked_cast<DerivedT>(safe.m_ptr));
    }
    };

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    safe_ptr(T *) -> safe_ptr<T>;

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    safe_ptr(const std::unique_ptr<T> &) -> safe_ptr<T>;

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
    class safe_ptr {
        template<typename U>
        friend class safe_ptr;

        using pointer = std::add_pointer_t<T>;

    private:
        pointer m_ptr{nullptr};

    public:
        safe_ptr() noexcept = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr safe_ptr(pointer ptr) noexcept
            : m_ptr(ptr) {
        }

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr safe_ptr(const std::unique_ptr<std::remove_const_t<T> > &unique_ptr) noexcept
            : safe_ptr(unique_ptr.get()) {
        }

        template<typename U>
            requires std::convertible_to<U *, pointer>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        safe_ptr(const safe_ptr<U> &other) noexcept
            : m_ptr(other.m_ptr) {
        }

        template<typename U>
            requires std::convertible_to<U *, pointer>
        // ReSharper disable once CppNonExplicitConvertingConstructor
        safe_ptr(safe_ptr<U> &&other) noexcept
            : m_ptr(other.m_ptr) {
            other.m_ptr = nullptr;
        }

        safe_ptr &operator=(pointer ptr) noexcept {
            m_ptr = ptr;
            return *this;
        }

        safe_ptr &operator=(std::nullptr_t) noexcept {
            m_ptr = nullptr;
            return *this;
        }

        safe_ptr(const safe_ptr &) noexcept = default;

        safe_ptr(safe_ptr &&) noexcept = default;

        safe_ptr &operator=(const safe_ptr &) noexcept = default;

        safe_ptr &operator=(safe_ptr &&) noexcept = default;

        template<typename U>
            requires std::convertible_to<U *, pointer>
        safe_ptr &operator=(const safe_ptr<U> &other) noexcept {
            m_ptr = other.m_ptr;
            return *this;
        }

        template<typename U>
            requires std::convertible_to<U *, pointer>
        safe_ptr &operator=(safe_ptr<U> &&other) noexcept {
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
            return *this;
        }

        [[nodiscard]] pointer operator->() const noexcept {
            return m_ptr;
        }

        [[nodiscard]] T &operator*() const noexcept {
            return *m_ptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_ptr != nullptr;
        }

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return m_ptr != nullptr;
        }

        [[nodiscard]] constexpr pointer get() const noexcept {
            return m_ptr;
        }

        template<typename BaseT>
            requires std::is_base_of_v<BaseT, T>
        [[nodiscard]] constexpr safe_ptr<BaseT> upcast() && noexcept {
            safe_ptr<BaseT> result{static_cast<BaseT *>(m_ptr)};
            m_ptr = nullptr;
            return result;
        }

        void reset(T *const ptr = nullptr) noexcept {
            m_ptr = ptr;
        }

        [[nodiscard]] friend bool operator==(const safe_ptr &lhs, const T *rhs) noexcept {
            return lhs.m_ptr == rhs;
        }

        [[nodiscard]] friend bool operator==(const T *lhs, const safe_ptr &rhs) noexcept {
            return lhs == rhs.m_ptr;
        }

        [[nodiscard]] friend bool operator==(const safe_ptr &lhs, std::nullptr_t) noexcept {
            return lhs.m_ptr == nullptr;
        }

        [[nodiscard]] friend bool operator==(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
            return lhs.m_ptr == rhs.m_ptr;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, pointer rhs) noexcept {
            return lhs.m_ptr <=> rhs;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const safe_ptr &lhs, const safe_ptr &rhs) noexcept {
            return lhs.m_ptr <=> rhs.m_ptr;
        }

        friend void swap(safe_ptr &lhs, safe_ptr &rhs) noexcept {
            std::swap(lhs.m_ptr, rhs.m_ptr);
        }

        template<typename DerivedT>
            requires std::is_base_of_v<T, DerivedT>
        [[nodiscard]] friend safe_ptr<DerivedT> checked_cast(const safe_ptr &safe) noexcept {
            return safe_ptr<DerivedT>(checked_cast<DerivedT>(safe.m_ptr));
        }
    };

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    safe_ptr(T *) -> safe_ptr<T>;

    template<typename T>
        requires std::is_base_of_v<safe_object, T>
    safe_ptr(const std::unique_ptr<T> &) -> safe_ptr<T>;

    // Zero-overhead invariant: in release builds safe_ptr<T> must be a drop-in
    // replacement for a raw T* (pointer-sized, trivially copyable/destructible).
    // A future edit that adds state or non-trivial ops will fail this compile.
    static_assert(sizeof(safe_ptr<safe_object>) == sizeof(safe_object *));
    static_assert(std::is_trivially_copyable_v<safe_ptr<safe_object> >);
    static_assert(std::is_trivially_destructible_v<safe_ptr<safe_object> >);
#endif

    // Debug shape tracks ownership and must never be mem-copied; release shape
    // is a raw pointer and relocates trivially.
#if PPR_ENABLE_DEBUG
    template<typename T>
    struct details::relocatable<safe_ptr<T> > : std::false_type {
    };
#else
    template<typename T>
    struct details::relocatable<safe_ptr<T> > : std::true_type {
    };
#endif
}
