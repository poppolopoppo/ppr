module;
#include "pP/Macros.h"
export module engine.core:timer;

import :assert;
import :containers.stable_vector;

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

        [[nodiscard]] static ITimerClock &steady() noexcept;
    };

    class TimerManager final {
        struct Event {
            TimePoint m_date{};
            pP::unique_function<void(TimePoint)> m_callback{};

            [[nodiscard]] constexpr std::strong_ordering operator<=>(const Event &other) const noexcept {
                return m_date <=> other.m_date;
            }
        };

        TimePoint updateLastTick_() noexcept;

        ITimerClock *const m_clock{nullptr};
        std::atomic<long long> m_last_tick{0};

        std::mutex m_mutex{};
        std::vector<Event> m_queue{};

    public:
        using Callback = pP::unique_function<void(TimePoint)>;

        explicit TimerManager(ITimerClock &clock = ITimerClock::steady()) noexcept;

        [[nodiscard]] TimePoint now() const noexcept;

        void schedule(const TimePoint date, Callback &&callback) noexcept;

        void tick() noexcept;

        static TimerManager &mainTimer() noexcept;
    };
}
