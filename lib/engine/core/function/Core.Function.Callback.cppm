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
    // function callback with an optional unique subscriber
    // ------------------------------------------------------------------

    namespace details {
        template<TFunction FunctionT, typename ParamsT>
        class DelegateImpl;

        template<TFunction FunctionT, typename... ArgsT>
        class DelegateImpl<FunctionT, std::tuple<ArgsT...> > {
        public:
            using FunctionTraits = FunctionTraits<FunctionT>;
            using Return = typename FunctionTraits::return_type;
            using Subscriber = std23::function_ref<FunctionT>;

            std::optional<Subscriber> m_subscriber{};

            DelegateImpl() noexcept = default;

            explicit DelegateImpl(Subscriber subscriber) noexcept
                : m_subscriber(std::move(subscriber)) {
            }

            void reset() noexcept {
                m_subscriber.reset();
            }

            std::optional<Subscriber> subscribe(Subscriber new_subscriber) noexcept {
                return std::exchange(m_subscriber, std::move(new_subscriber));
            }

            template<auto F, typename T>
                requires requires(T *obj)
                {
                    { Subscriber(std23::nontype<F>, obj) };
                }
            std::optional<Subscriber> subscribe(T *obj) noexcept {
                return subscribe(Subscriber{std23::nontype<F>, obj});
            }

            template<auto F, typename T>
                requires requires(T &&obj)
                {
                    { Subscriber(std23::nontype<F>, std::forward<T>(obj)) };
                }
            std::optional<Subscriber> subscribe(T &&obj) noexcept {
                return subscribe(Subscriber{std23::nontype<F>, std::forward<T>(obj)});
            }

            Return operator()(ArgsT... args) const noexcept(FunctionTraits::is_noexcept_v) {
                if (not m_subscriber.has_value()) {
                    if constexpr (std::is_void_v<Return>) {
                        return;
                    } else {
                        return default_value_v;
                    }
                }
                return (*m_subscriber)(std::forward<ArgsT>(args)...);
            }
        };
    }

    template<details::TFunction FunctionT>
    class Delegate final : public details::DelegateImpl<FunctionT,
                typename details::FunctionTraits<FunctionT>::params_type> {
        using super_t = details::DelegateImpl<FunctionT,
            typename details::FunctionTraits<FunctionT>::params_type>;

    public:
        using super_t::super_t;
        using super_t::operator();
    };

    // ------------------------------------------------------------------
    // broadcast function callback with multiple subscribers
    // ------------------------------------------------------------------

    namespace details {
        template<
            TFunctionReturning<std::error_code> FunctionT,
            mem::details::TAllocator AllocatorT,
            typename ParamsT>
        class BroadcastCallbackImpl;

        template<
            TFunctionReturning<std::error_code> FunctionT,
            mem::details::TAllocator AllocatorT,
            typename... ArgsT>
        class BroadcastCallbackImpl<FunctionT, AllocatorT, std::tuple<ArgsT...> > {
        public:
            using FunctionTraits = FunctionTraits<FunctionT>;
            using Event = std23::function_ref<FunctionT>;

            class [[nodiscard]] Handle final {
                const BroadcastCallbackImpl *m_callback{nullptr};
                SparseKeyId m_event_key{};
                std::shared_ptr<std::atomic<bool> > m_alive{};

            public:
                constexpr Handle() = default;

                Handle(const BroadcastCallbackImpl &callback PPR_LIFETIME_BOUND, const SparseKeyId &event_key) noexcept
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

            BroadcastCallbackImpl() noexcept
                requires std::is_default_constructible_v<AllocatorT>
            = default;

            explicit BroadcastCallbackImpl(const AllocatorT &alloc) noexcept
                : m_subscribers(alloc) {
            }

            explicit BroadcastCallbackImpl(AllocatorT &&alloc) noexcept
                : m_subscribers(std::forward<AllocatorT>(alloc)) {
            }

            ~BroadcastCallbackImpl() noexcept {
                if (m_alive) {
                    m_alive->store(false, std::memory_order_release);
                }
            }

            [[nodiscard]] bool isEmpty() const noexcept {
                return m_subscribers.isEmpty();
            }

            [[nodiscard]] Handle add(Event event) const/* see mutable bellow */ {
                const SparseKeyId event_key = m_subscribers.add(std::forward<Event>(event));
                return Handle(*this, event_key);
            }

            [[nodiscard]] bool remove(const SparseKeyId event_key) const/* see mutable bellow */ {
                return m_subscribers.erase(event_key);
            }

            void clear() noexcept {
                m_subscribers.clear();
            }

            [[nodiscard]] std::error_code operator()(ArgsT... args) noexcept(FunctionTraits::is_noexcept_v) {
                for (const Event &event: m_subscribers) {
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
            mutable SparseVectorInplace<Event, AllocatorT> m_subscribers{};
            // Shared liveness flag: the Callback holds one reference and each Handle
            // holds another. When the Callback is destroyed, it sets the flag to
            // false; the flag's storage outlives the Callback because Handles still
            // hold a shared_ptr to it. This prevents Handle::~Handle() from
            // dereferencing a dangling Callback* when the owning object is destroyed
            // before the Handle.
            std::shared_ptr<std::atomic<bool> > m_alive = std::make_shared<std::atomic<bool> >(true);
        };
    }

    template<
        details::TFunctionReturning<std::error_code> FunctionT,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class BroadcastCallback final : public details::BroadcastCallbackImpl<
                FunctionT, AllocatorT,
                typename details::FunctionTraits<FunctionT>::params_type> {
        using super_t = details::BroadcastCallbackImpl<
            FunctionT, AllocatorT,
            typename details::FunctionTraits<FunctionT>::params_type>;

    public:
        using super_t::super_t;
        using super_t::operator();
    };

    // ------------------------------------------------------------------
    // deferred sink function callback with multiple subscribers
    // ------------------------------------------------------------------

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

        template<
            TFunctionReturning<std::error_code> FunctionT,
            mem::details::TAllocator AllocatorT,
            typename ParamsT>
        class CallbackSinkImpl;

        template<
            TFunctionReturning<std::error_code> FunctionT,
            mem::details::TAllocator AllocatorT,
            typename... ArgsT>
        class CallbackSinkImpl<FunctionT, AllocatorT, std::tuple<ArgsT...> > final {
            using function_traits = FunctionTraits<FunctionT>;
            using broadcast_callback = BroadcastCallback<FunctionT, AllocatorT>;
            using params_type = std::tuple<ArgsT...>;

            broadcast_callback m_callback{};
            std::optional<params_type> m_deferred_params{};

        public:
            using Event = typename broadcast_callback::Event;
            using Handle = typename broadcast_callback::Handle;

            CallbackSinkImpl() noexcept
                requires std::is_default_constructible_v<AllocatorT>
            = default;

            explicit CallbackSinkImpl(const AllocatorT &alloc) noexcept
                : m_callback(alloc) {
            }

            explicit CallbackSinkImpl(AllocatorT &&alloc) noexcept
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

            [[nodiscard]] std::error_code sink() noexcept(function_traits::is_noexcept_v) {
                if (m_deferred_params) {
                    return std::apply(m_callback, std::exchange(m_deferred_params, std::nullopt).value());
                }
                return default_value_v;
            }

            void operator()(ArgsT... args) {
                if (not m_deferred_params.has_value()) {
                    m_deferred_params.emplace(std::forward<ArgsT>(args)...);
                }
            }
        };
    }

    template<
        details::TFunctionReturning<std::error_code> FunctionT,
        mem::details::TAllocator AllocatorT = mem::GPA>
    class CallbackSink final : public details::CallbackSinkImpl<
                FunctionT, AllocatorT,
                typename details::FunctionTraits<FunctionT>::params_type> {
        using super_t = details::CallbackSinkImpl<
            FunctionT, AllocatorT,
            typename details::FunctionTraits<FunctionT>::params_type>;

    public:
        using super_t::super_t;
        using super_t::operator();
    };
}
