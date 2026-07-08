module;
#include "pP/Macros.h"

module engine.core;

import :timer;
import :hal;
import :assert;
import :containers.stable_vector;

import std;

namespace pP {

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    ITimerClock &ITimerClock::steady() noexcept {
        class SteadyClock final : public ITimerClock {
        public:
            TimePoint now() noexcept override {
                return std::chrono::steady_clock::now();
            }
        };
        static SteadyClock g_instance;
        return g_instance;
    }

    TimerManager::TimerManager(ITimerClock &clock) noexcept
        : m_clock(std::addressof(clock)) {
        updateLastTick_();
    }

    TimePoint TimerManager::now() const noexcept {
        return TimePoint(TimeSpan(m_last_tick.load(std::memory_order_acquire)));
    }

    void TimerManager::schedule(const TimePoint date, Callback &&callback) noexcept {
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

        callback(current_tick);
    }

    void TimerManager::tick() noexcept {
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

        for (Callback &callback : ready_callbacks) {
            callback(current_tick);
        }
    }

    TimerManager &TimerManager::mainTimer() noexcept {
        static TimerManager g_instance{ITimerClock::steady()};
        return g_instance;
    }

    TimePoint TimerManager::updateLastTick_() noexcept {
        const TimePoint current_tick = m_clock->now();
        m_last_tick.store(current_tick.time_since_epoch().count(), std::memory_order::release);
        return current_tick;
    }

}
