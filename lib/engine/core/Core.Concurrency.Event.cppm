// ReSharper disable CppPolymorphicClassWithNonVirtualPublicDestructor
module;
#include "pP/Macros.h"
export module engine.core:event;

import :assert;
import :allocator;
import :containers;
import :hal;
import :stable_vector;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // Event signaling infrastructure
    // ------------------------------------------------------------------

    class IEvent;

    class ISignal {
    public:
        virtual void notify(const std::size_t event_tag) noexcept = 0;

        virtual void wait() noexcept = 0;
    };

    class IEvent {
    public:
        virtual TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept = 0;
        virtual void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept = 0;

        [[nodiscard]] virtual bool pollEvent() noexcept = 0;

        virtual void resetEvent() noexcept = 0;
    };

    // ------------------------------------------------------------------
    // Signal implementation for composite events
    // ------------------------------------------------------------------

    PPR_PRAGMA_WARNING_PUSH()
    PPR_PRAGMA_WARNING_DISABLE_MSVC(4324) //  'pP::Signal<>': structure was padded due to alignment specifier

    template<typename... EventsT>
        requires (sizeof...(EventsT) > 0 &&
                  sizeof...(EventsT) <= bit_count_v<std::size_t> &&
                  std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
    class [[nodiscard]] alignas(hal::cacheline_size_v) Signal final : public ISignal {
        std::counting_semaphore<> m_semaphore{0};
        std::atomic<std::size_t> m_pending{0};

        std::tuple<EventsT &...> m_events;
        std::array<TagPtr<ISignal>, sizeof...(EventsT)> m_parents{};

    public:
        using Event = std::variant<EventsT *...>;

        explicit Signal(EventsT &... events PPR_LIFETIME_BOUND) noexcept
            : m_events(events...) {
            static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
                ((m_parents[event_index] = std::get<event_index>(m_events).subscribeEvent(
                      TagPtr<ISignal>{this, event_index})), ...);
            });
        }

        ~Signal() noexcept {
            static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
                ((std::get<event_index>(m_events).unsubscribeEvent(TagPtr<ISignal>{this, event_index}, m_parents[event_index])), ...);
            });
        }

        void notify(const std::size_t event_index) noexcept override {
            if (PPR_ENSURE(event_index < sizeof...(EventsT))) [[likely]] {
                const std::size_t bit = std::size_t{1u} << event_index;
                const std::size_t prev = m_pending.fetch_or(bit, std::memory_order_release);
                if ((prev & bit) == 0u) [[likely]] {
                    // only release if bit was newly set
                    m_semaphore.release();
                }
            }
        }

        void wait() noexcept override {
            while (m_pending.load(std::memory_order_acquire) == 0u) {
                m_semaphore.acquire();
            }
        }

        void reset(const Event &source) noexcept {
            std::visit([](IEvent *const p_event) noexcept {
                if (PPR_ENSURE(p_event != nullptr)) [[likely]] {
                    p_event->resetEvent();
                }
            }, source);
        }

        [[nodiscard]] std::optional<Event> poll() noexcept {
            std::size_t pending = m_pending.load(std::memory_order_acquire);
            for (;;) {
                if (pending == 0u) {
                    return std::nullopt;
                }

                const std::size_t desired_pending = pending & (pending - 1u);
                if (m_pending.compare_exchange_weak(
                    pending, desired_pending,
                    std::memory_order_acq_rel, std::memory_order_acquire)) [[likely]] {
                    break;
                }
            };
            const std::size_t ready_index = std::countr_zero(pending);
            return getOptionalEvent_(ready_index);
        }

        class [[nodiscard]] iterator {
            Signal *m_signal{nullptr};
            std::optional<Event> m_event{};

            void advance_() noexcept {
                if (m_event.has_value()) {
                    m_signal->reset(*m_event);
                }

                m_event = m_signal->poll();

                while (not m_event.has_value()) {
                    m_signal->wait();
                    m_event = m_signal->poll();
                }
            }

        public:
            using iterator_category = std::input_iterator_tag;
            using value_type = Event;
            using difference_type = std::ptrdiff_t;
            using pointer = Event *;
            using reference = Event &;

            explicit iterator(Signal &signal) noexcept
                : m_signal(std::addressof(signal)) {
                advance_();
            }

            [[nodiscard]] Event &operator*() noexcept {
                PPR_ASSERT(m_event.has_value());
                return m_event.value();
            }

            [[nodiscard]] Event *operator->() noexcept {
                return std::addressof(operator*());
            }

            iterator &operator++() noexcept {
                advance_();
                return *this;
            }

            void operator++(int) noexcept {
                advance_();
            }

            [[nodiscard]] bool isValid() const noexcept {
                return m_event.has_value();
            }

            friend bool operator==(const iterator &lhs, const iterator &rhs) noexcept {
                return &lhs.m_signal == &rhs.m_signal && lhs.m_event == rhs.m_event;
            }

            friend bool operator!=(const iterator &lhs, const iterator &rhs) noexcept {
                return not operator==(lhs, rhs);
            }

            friend bool operator==(const iterator &lhs, const std::default_sentinel_t) noexcept {
                return not lhs.m_event.has_value();
            }

            friend bool operator!=(const iterator &lhs, const std::default_sentinel_t) noexcept {
                return lhs.m_event.has_value();
            }
        };

        [[nodiscard]] iterator begin() noexcept {
            return iterator(*this);
        }

        [[nodiscard]] static constexpr std::default_sentinel_t end() noexcept {
            return std::default_sentinel;
        }

    private:
        [[nodiscard]] IEvent *getAbstractEvent_(std::size_t ready_index) const noexcept {
            IEvent *p_ready_event{nullptr};

            static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
                ([&]() noexcept -> bool {
                    if (event_index == ready_index) {
                        p_ready_event = std::addressof(std::get<event_index>(m_events));
                        return true;
                    }
                    return false;
                }() || ...);
            });

            return p_ready_event;
        }

        [[nodiscard]] std::optional<Event> getOptionalEvent_(std::size_t ready_index) const noexcept {
            std::optional<Event> ready_event;

            static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
                ([&]() noexcept -> bool {
                    if (event_index == ready_index) {
                        ready_event = Event(
                            std::in_place_index<event_index>,
                            std::addressof(std::get<event_index>(m_events)));
                        return true;
                    }
                    return false;
                }() || ...);
            });

            return ready_event;
        }
    };

    PPR_PRAGMA_WARNING_POP()

    template<typename... EventsT>
        requires (sizeof...(EventsT) > 0 &&
                  sizeof...(EventsT) <= bit_count_v<std::size_t> &&
                  std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
    Signal(EventsT &...) -> Signal<EventsT...>;

    // ------------------------------------------------------------------
    // Signal implementation for single event (no dispatch needed)
    // ------------------------------------------------------------------

    PPR_PRAGMA_WARNING_PUSH()
    PPR_PRAGMA_WARNING_DISABLE_MSVC(4324) //  'pP::Signal<>': structure was padded due to alignment specifier

    template<typename EventT>
        requires std::is_base_of_v<IEvent, EventT>
    class [[nodiscard]] alignas(hal::cacheline_size_v) Signal<EventT> final : public ISignal {
        std::counting_semaphore<> m_semaphore{0};

        EventT &m_event;
        TagPtr<ISignal> const m_restore;

    public:
        explicit Signal(EventT &event PPR_LIFETIME_BOUND) noexcept
            : m_event(event),
              m_restore(m_event.subscribeEvent(TagPtr<ISignal>{this, 0u})) {
        }

        ~Signal() noexcept {
            m_event.unsubscribeEvent(TagPtr<ISignal>{this, 0u}, m_restore);
        }

        void notify([[maybe_unused]] const std::size_t event_tag) noexcept override {
            PPR_ASSERT(event_tag == 0u);
            m_semaphore.release();
        }

        void wait() noexcept override {
            while (not m_event.pollEvent()) {
                m_semaphore.acquire();
            }
        }

        void reset() noexcept {
            m_event.resetEvent();
        }

        [[nodiscard]] std::optional<EventT *> poll() noexcept {
            if (m_event.pollEvent()) {
                return std::addressof(m_event);
            }
            return std::nullopt;
        }

        class [[nodiscard]] iterator {
            Signal *m_signal{nullptr};
            std::optional<EventT *> m_event{};

            void advance_() noexcept {
                m_event = m_signal->poll();

                while (not m_event.has_value()) {
                    m_signal->reset();
                    m_signal->wait();
                    m_event = m_signal->poll();
                }
            }

        public:
            using iterator_category = std::input_iterator_tag;
            using value_type = EventT;
            using difference_type = std::ptrdiff_t;
            using pointer = EventT *;
            using reference = EventT &;

            explicit iterator(Signal &signal PPR_LIFETIME_BOUND) noexcept
                : m_signal(std::addressof(signal)) {
                advance_();
            }

            [[nodiscard]] EventT &operator*() noexcept {
                PPR_ASSERT(m_event.has_value());
                return *m_event.value();
            }

            [[nodiscard]] EventT *operator->() noexcept {
                PPR_ASSERT(m_event.has_value());
                return m_event.value();
            }

            iterator &operator++() noexcept {
                advance_();
                return *this;
            }

            void operator++(int) noexcept {
                advance_();
            }

            [[nodiscard]] bool isValid() const noexcept {
                return m_event.has_value();
            }

            friend bool operator==(const iterator &lhs, const iterator &rhs) noexcept {
                return &lhs.m_signal == &rhs.m_signal && lhs.m_event == rhs.m_event;
            }

            friend bool operator!=(const iterator &lhs, const iterator &rhs) noexcept {
                return not operator==(lhs, rhs);
            }

            friend bool operator==(const iterator &lhs, const std::default_sentinel_t) noexcept {
                return not lhs.m_event.has_value();
            }

            friend bool operator!=(const iterator &lhs, const std::default_sentinel_t) noexcept {
                return lhs.m_event.has_value();
            }
        };

        [[nodiscard]] iterator begin() noexcept {
            return iterator(*this);
        }

        [[nodiscard]] static constexpr std::default_sentinel_t end() noexcept {
            return std::default_sentinel;
        }
    };

    PPR_PRAGMA_WARNING_POP()

    // ------------------------------------------------------------------
    // Go-like `select(...)` helper for events multiplexing
    // ------------------------------------------------------------------

    // Example usage:
    //  for (auto &event : select(event_a, event_b)) {
    //       std::visit(overloaded(
    //           [&](EventA *src) { /* ... */ },
    //           [&](EventB *src) { /* ... */ },
    //           event);
    //  }

    template<typename... EventsT>
        requires (sizeof...(EventsT) > 0 &&
                  sizeof...(EventsT) <= bit_count_v<std::size_t> &&
                  std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
    [[nodiscard]] Signal<EventsT...> select(EventsT &... events) noexcept {
        return Signal(events...);
    }

    // ------------------------------------------------------------------
    // Never event never calls notify()
    // ------------------------------------------------------------------

    class [[nodiscard]] NeverEvent final : public IEvent {
    public:
        constexpr NeverEvent() noexcept = default;

        constexpr TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal>) noexcept override {
            return default_value_v;
        }

        constexpr void unsubscribeEvent(const TagPtr<ISignal>, const TagPtr<ISignal>) noexcept override {
        }

        constexpr [[nodiscard]] bool pollEvent() noexcept override {
            return false;
        }

        constexpr void resetEvent() noexcept override {
        }
    };

    // ------------------------------------------------------------------
    // Pulse event only calls notify() once, until reset() is called
    // ------------------------------------------------------------------

    class [[nodiscard]] PulseEvent final : public IEvent {
        static constexpr std::uintptr_t signal_bit_v = static_cast<std::uintptr_t>(1u) << (bit_count_v<std::size_t> - 1u);

        std::atomic<std::uintptr_t> m_signal{};

    public:
        PulseEvent() noexcept = default;

        TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
            PPR_ASSERT((signal.m_packed & signal_bit_v ) == 0u);

            TagPtr<ISignal> parent;
            parent.m_packed = m_signal.exchange(signal.m_packed, std::memory_order_acq_rel);

            if (parent.m_packed & signal_bit_v) [[unlikely]] {
                parent.m_packed &= ~signal_bit_v;
                emitEvent();
            }

            return parent;
        }

        void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
            // unsubscribe <=> subscribe(previous_callback)
            [[maybe_unused]] const auto subscription = subscribeEvent(restore);
            PPR_ASSERT(subscription == signal);
        }

        [[nodiscard]] bool pollEvent() noexcept override {
            return (m_signal.load(std::memory_order_acquire) & signal_bit_v) != 0u;
        }

        void resetEvent() noexcept override {
            m_signal.fetch_and(~signal_bit_v, std::memory_order_release);
        }

        void emitEvent() noexcept {
            TagPtr<ISignal> handler;
            handler.m_packed = m_signal.fetch_or(signal_bit_v, std::memory_order_acq_rel);
            const bool should_notify = (handler.m_packed & signal_bit_v) == 0u;
            handler.m_packed &= ~signal_bit_v;

            if (should_notify && handler.isValid()) {
                const auto [signal, event_tag] = handler.unpack();
                signal->notify(event_tag);
            }
        }
    };

    // ------------------------------------------------------------------
    // Broadcast event has the same behavior than pulse, but supports multiple subscribers
    // ------------------------------------------------------------------

    class [[nodiscard]] BroadcastEvent final : public IEvent {
        std::mutex m_subscriptions_mutex{};
        std::atomic_flag m_signal{};
        StableVectorInplace<TagPtr<ISignal>, mem::Pmr> m_subscriptions;

    public:
        BroadcastEvent() noexcept = default;

        explicit BroadcastEvent(const mem::Pmr &allocator) noexcept
            : m_subscriptions(allocator) {
        }

        TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
            const std::lock_guard scope_lock(m_subscriptions_mutex);
            PPR_ASSERT(m_subscriptions.find(signal) == m_subscriptions.end());
            m_subscriptions.pushBack(signal);
            return default_value_v;
        }

        void unsubscribeEvent(const TagPtr<ISignal> signal, [[maybe_unused]] const TagPtr<ISignal> restore) noexcept override {
            PPR_ASSERT(restore.isNull());

            const std::lock_guard scope_lock(m_subscriptions_mutex);
            const auto it = m_subscriptions.find(signal);
            if (PPR_ENSURE(it != m_subscriptions.end())) [[likely]] {
                m_subscriptions.erase(it);
            }
        }

        [[nodiscard]] bool pollEvent() noexcept override {
            return m_signal.test(std::memory_order_acquire);
        }

        void resetEvent() noexcept override {
            m_signal.clear(std::memory_order_release);
        }

        void emitEvent() noexcept {
            if (not m_signal.test_and_set(std::memory_order_acq_rel)) {
                StableVectorInplace<TagPtr<ISignal>> snapshot;
                {
                    const std::lock_guard scope_lock(m_subscriptions_mutex);
                    snapshot.append(m_subscriptions);
                }
                // if a subscriber's notify() callback attempts to subscribeEvent or
                // unsubscribeEvent on the same BroadcastEvent, it will deadlock.
                for (const TagPtr<ISignal> handler : snapshot) {
                    const auto [signal, event_tag] = handler.unpack();
                    signal->notify(event_tag);
                }
            }
        }
    };

}
