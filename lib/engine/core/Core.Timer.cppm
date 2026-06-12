module;
#include "pP/Macros.h"
export module engine.core:timer;

import :assert;
import :stable_vector;

import std;

export namespace pP {

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeDuration = std::chrono::steady_clock::duration;

    // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
    class ITimerClock {
    public:
        [[nodiscard]] virtual TimePoint now() noexcept = 0;

        [[nodiscard]] static ITimerClock &steady() noexcept {
            // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
            class SteadyClock final : public ITimerClock {
            public:
                TimePoint now() noexcept override {
                    return std::chrono::steady_clock::now();
                }
            };
            static SteadyClock g_instance;
            return g_instance;
        }
    };

    class TimerManager final {
        struct Event {
            TimePoint m_date{};
            std::move_only_function<void(TimePoint) noexcept> m_callback{};

            [[nodiscard]] constexpr std::strong_ordering operator<=>(const Event &other) const noexcept {
                return m_date <=> other.m_date;
            }
        };

        TimePoint updateLastTick_() noexcept {
            const TimePoint current_tick = m_clock->now();
            m_last_tick.store(current_tick.time_since_epoch().count(), std::memory_order::release);
            return current_tick;
        }

        ITimerClock *const m_clock{nullptr};
        std::atomic<long long> m_last_tick{0};

        std::mutex m_mutex{};
        std::vector<Event> m_queue{};

    public:
        using Callback = std::move_only_function<void(TimePoint) noexcept>;

        explicit TimerManager(ITimerClock &clock = ITimerClock::steady()) noexcept
            : m_clock(std::addressof(clock)) {
            updateLastTick_();
        }

        [[nodiscard]] TimePoint now() const noexcept {
            return TimePoint(TimeDuration(m_last_tick.load(std::memory_order_acquire)));
        }

        void schedule(const TimePoint date, Callback &&callback) noexcept {
            TimePoint current_tick;
            {
                const std::lock_guard scope_lock(m_mutex);
                current_tick = now();
                if (PPR_ENSURE(current_tick < date)) [[likely]] {
                    m_queue.push_back(Event{date, std::move(callback)});
                    std::ranges::push_heap(m_queue, std::greater{});
                    return;
                }
            }

            // Execute callbacks OUTSIDE the lock to prevent deadlocks
            // if a callback tries to schedule a new timer.
            callback(current_tick);
        }

        void tick() noexcept {
            StableVectorInplace<Callback> ready_callbacks{};

            TimePoint current_tick;
            {
                const std::lock_guard scope_lock(m_mutex);
                current_tick = updateLastTick_();

                while (not m_queue.empty() && m_queue.front().m_date <= current_tick) {
                    ready_callbacks.pushBack(std::move(m_queue.front().m_callback));
                    std::pop_heap(m_queue.begin(), m_queue.end(), std::greater{});
                    m_queue.pop_back();
                }
            }

            // Execute callbacks OUTSIDE the lock to prevent deadlocks
            // if a callback tries to schedule a new timer.
            for (Callback &callback : ready_callbacks) {
                callback(current_tick);
            }
        }

        static TimerManager &mainTimer() noexcept {
            static TimerManager g_instance{ITimerClock::steady()};
            return g_instance;
        }
    };
}
