module;
#include "pP/Macros.h"
export module engine.core:timer;

import :assert;
import :containers.stable_vector;

import std;

export namespace pP {

    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeDuration = std::chrono::steady_clock::duration;

    // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
    class ITimerClock {
    public:
        virtual ~ITimerClock() noexcept = default;

        [[nodiscard]] virtual TimePoint now() noexcept = 0;

        [[nodiscard]] static ITimerClock &steady() noexcept;
    };

    class TimerManager final {
        struct Event {
            TimePoint m_date{};
            std::function<void(TimePoint)> m_callback{}; // move_only_function unavailable on Clang 20 + libc++ 20.1

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
        using Callback = std::function<void(TimePoint)>; // move_only_function unavailable on Clang 20 + libc++ 20.1

        explicit TimerManager(ITimerClock &clock = ITimerClock::steady()) noexcept;

        [[nodiscard]] TimePoint now() const noexcept;

        void schedule(const TimePoint date, Callback &&callback) noexcept;

        void tick() noexcept;

        static TimerManager &mainTimer() noexcept;
    };
}
