module;
#include "pP/Macros.h"
export module engine.core:function.callback;

import :assert;
import :containers.sparse_vector;
import :function.ref;
import :memory.pointer;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // function callback with multiple subscribers
    // ------------------------------------------------------------------

    template<
        details::TFunctionReturning<std::error_code> FunctionT,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class Callback final {
    public:
        using Event = std23::function_ref<FunctionT>;

        class [[nodiscard]] Handle final {
            const Callback *m_callback{nullptr};
            SparseKeyId m_event_key{};
            std::shared_ptr<std::atomic<bool> > m_alive{};

        public:
            constexpr Handle() = default;

            Handle(const Callback &callback, const SparseKeyId &event_key) noexcept
                : m_callback(std::addressof(callback)), m_event_key(event_key), m_alive(callback.m_alive) {
            }

            Handle(const Handle &) = delete;

            Handle &operator=(const Handle &) = delete;

            Handle(Handle &&other) noexcept
                : m_callback(std::exchange(other.m_callback, nullptr)),
                  m_event_key(std::exchange(other.m_event_key, default_value_v)),
                  m_alive(std::exchange(other.m_alive, nullptr)) {
            }

            Handle &operator=(Handle &&other) noexcept {
                if (this != std::addressof(other)) {
                    if (m_callback != nullptr && m_alive && m_alive->load(std::memory_order_acquire)) {
                        std::ignore = m_callback->remove(m_event_key);
                    }
                    m_callback = std::exchange(other.m_callback, nullptr);
                    m_event_key = std::exchange(other.m_event_key, default_value_v);
                    m_alive = std::exchange(other.m_alive, nullptr);
                }
                return *this;
            }

            ~Handle() noexcept {
                // Single-threaded contract: Handle destruction must not race with
                // Callback destruction. The shared liveness flag synchronizes the
                // Callback's release-store with the Handle's acquire-load, but the
                // Callback object itself is not reference-counted — callers must
                // guarantee the Callback outlives all Handles, or destroy Handles
                // before the owning object.
                if (m_callback != nullptr && m_alive && m_alive->load(std::memory_order_acquire)) {
                    std::ignore = m_callback->remove(m_event_key);
                    m_callback = nullptr;
                }
            }

            [[nodiscard]] constexpr bool isValid() const noexcept {
                return m_callback != nullptr;
            }

            SparseKeyId release() noexcept {
                m_callback = nullptr;
                return std::exchange(m_event_key, default_value_v);
            }
        };

        Callback() noexcept
            requires std::is_default_constructible_v<AllocatorT>
        = default;

        explicit Callback(const AllocatorT &alloc) noexcept
            : m_events(alloc) {
        }

        explicit Callback(AllocatorT &&alloc) noexcept
            : m_events(std::forward<AllocatorT>(alloc)) {
        }

        ~Callback() noexcept {
            if (m_alive) {
                m_alive->store(false, std::memory_order_release);
            }
        }

        [[nodiscard]] Handle add(Event event) const/* see mutable bellow */ {
            const SparseKeyId event_key = m_events.add(std::forward<Event>(event));
            return Handle(*this, event_key);
        }

        [[nodiscard]] bool remove(const SparseKeyId event_key) const/* see mutable bellow */ {
            return m_events.erase(event_key);
        }

        void clear() noexcept {
            m_events.clear();
        }

        template<typename... ArgsT>
            requires requires(const Event &event, ArgsT &&... args)
            {
                { event(std::forward<ArgsT>(args)...) } -> std::convertible_to<std::error_code>;
            }
        [[nodiscard]] std::error_code operator()(ArgsT &&... args)
            noexcept(noexcept(std::declval<const Event &>()(std::forward<ArgsT>(args)...))) {
            for (const Event &event: m_events) {
                if (const std::error_code err = event(std::forward<ArgsT>(args)...)) [[unlikely]] {
                    return err;
                }
            }
            return default_value_v;
        }

    private:
        // add()/remove() are const (allow client to subscribe/unsubscribe through
        // a const reference), while clear()/operator()() remain non-const.
        // This is an intentional design choice: a const Callback& allows adding
        // and removing subscribers but not triggering the callback itself.
        // The remove() call during operator()() iteration is unsafe (iterator
        // invalidation) and callers must defer removals outside the dispatch loop.
        mutable SparseVectorInplace<Event, AllocatorT> m_events;
        // Shared liveness flag: the Callback holds one reference and each Handle
        // holds another. When the Callback is destroyed, it sets the flag to
        // false; the flag's storage outlives the Callback because Handles still
        // hold a shared_ptr to it. This prevents Handle::~Handle() from
        // dereferencing a dangling Callback* when the owning object is destroyed
        // before the Handle.
        std::shared_ptr<std::atomic<bool> > m_alive = std::make_shared<std::atomic<bool> >(true);
    };

    namespace details {
        template<typename T>
        struct ForwardAsLValue : std::type_identity<T> {
        };

        template<TSafeObject T>
        struct ForwardAsLValue<T &> : std::type_identity<safe_ptr<T> > {
        };

        template<TSafeObject T>
        struct ForwardAsLValue<T *> : std::type_identity<safe_ptr<T> > {
        };

        template<TSafeObject T>
        struct ForwardAsLValue<const T &> : std::type_identity<safe_ptr<const T> > {
        };

        template<TSafeObject T>
        struct ForwardAsLValue<const T *> : std::type_identity<safe_ptr<const T> > {
        };

        template<typename... ArgsT>
        struct ForwardAsLValue<std::tuple<ArgsT...> > {
            using type = std::tuple<
                typename ForwardAsLValue<ArgsT>::type...
            >;
        };
    }

    template<
        details::TFunctionReturning<std::error_code> FunctionT,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class CallbackSink final {
        using function_traits = details::FunctionTraits<FunctionT>;
#if 0 // forward declaration of pP::Window is breaking std::is_base_of<> :/
        using params_type = details::ForwardAsLValue<typename function_traits::params_type>;
#else
        using params_type = typename function_traits::params_type;
#endif

        Callback<FunctionT, AllocatorT> m_callback{};
        std::optional<params_type> m_deferred_params{};

    public:
        using Event = Callback<FunctionT, AllocatorT>::Event;
        using Handle = Callback<FunctionT, AllocatorT>::Handle;

        CallbackSink() noexcept
            requires std::is_default_constructible_v<AllocatorT>
        = default;

        explicit CallbackSink(const AllocatorT &alloc) noexcept
            : m_callback(alloc) {
        }

        explicit CallbackSink(AllocatorT &&alloc) noexcept
            : m_callback(std::forward<AllocatorT>(alloc)) {
        }

        [[nodiscard]] Handle add(Event event) const/* see mutable bellow */ {
            return m_callback.add(std::move(event));
        }

        [[nodiscard]] bool remove(const SparseKeyId event_key) const/* see mutable bellow */ {
            return m_callback.remove(event_key);
        }

        void clear() noexcept {
            m_callback.clear();
        }

        [[nodiscard]] std::error_code sink() noexcept(function_traits::is_noexcept) {
            if (m_deferred_params) {
                return std::apply(m_callback, std::exchange(m_deferred_params, std::nullopt).value());
            }
            return default_value_v;
        }

        template<typename... ArgsT>
            requires requires(const Event &event, ArgsT &&... args)
            {
                { event(std::forward<ArgsT>(args)...) } -> std::convertible_to<std::error_code>;
            }
        void operator()(ArgsT &&... args) {
            if (not m_deferred_params.has_value()) {
                m_deferred_params.emplace(std::forward<ArgsT>(args)...);
            }
        }
    };
}
