module;
#include "pP/Macros.h"

module engine.core;

import :timer;
import :assert;
import :containers.stable_vector;

import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Time, debug, none)

    // ------------------------------------------------------------------
    // time point and duration representations
    // ------------------------------------------------------------------

    TimePoint time::now() noexcept {
        return std::chrono::steady_clock::now();
    }

    TimeSpan time::since(const TimePoint started_at) noexcept {
        return now() - started_at;
    }

    double time::seconds(const TimeSpan duration) noexcept {
        return std::chrono::duration_cast<std::chrono::duration<double> >(duration).count();
    }

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    const ITimerClock &ITimerClock::steady() noexcept {
        class SteadyClock final : public ITimerClock {
        public:
            TimePoint now() const noexcept override {
                return std::chrono::steady_clock::now();
            }
        };
        static const SteadyClock g_instance;
        return g_instance;
    }

    TimerManager::TimerManager(const ITimerClock &clock) noexcept
        : m_clock(std::addressof(clock)) {
        reset();
    }

    TimePoint TimerManager::now() const noexcept {
        return TimePoint(TimeSpan(m_last_tick.load(std::memory_order_acquire)));
    }

    TimeSpan TimerManager::getDeltaTime() const noexcept {
        return TimeSpan(m_delta_time.load(std::memory_order::acquire));
    }

    TimeSpan TimerManager::getTotalElapsed() const noexcept {
        return TimeSpan(m_total_elapsed.load(std::memory_order::acquire));
    }

    void TimerManager::reset() {
        const std::lock_guard scope_lock(m_mutex);

        m_queue.clear();

        m_delta_time.store(0);
        m_total_elapsed.store(0);
        m_last_tick.store(m_clock->now().time_since_epoch().count(), std::memory_order::release);
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

    std::error_code TimerManager::tick(TimeSpan *out_delta_time, const TimeSpan target_period) noexcept {
        StableVectorInplace<Callback> ready_callbacks{};

        const TimePoint previous_tick = now();
        TimePoint current_tick = m_clock->now();

        TimeSpan delta_time = current_tick - previous_tick;
        if (delta_time < target_period) {
            std::this_thread::sleep_until(previous_tick + target_period);

            current_tick = m_clock->now();
            delta_time = current_tick - previous_tick;
        }

        m_last_tick.store(current_tick.time_since_epoch().count(), std::memory_order::release);
        m_total_elapsed.fetch_add(delta_time.count());
        m_delta_time.store(delta_time.count(), std::memory_order::release);

        // call every expired queued event:
        {
            const std::lock_guard scope_lock(m_mutex);

            while (not m_queue.empty() && m_queue.front().m_date <= current_tick) {
                ready_callbacks.pushBack(std::move(m_queue.front().m_callback));
                std::ranges::pop_heap(m_queue, std::greater{});
                m_queue.pop_back();
            }
        }

        *out_delta_time = delta_time;

        std::error_code first_err{};
        for (Callback &callback: ready_callbacks) {
            PPR_RETAIN_ERROR_ON_FAIL(Time, first_err, callback(current_tick));
        }

        return first_err;
    }
}
