module;
#include "pP/Macros.h"
export module engine.core:timer;

import :assert;
import :containers.stable_vector;
import :opaque;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // time point and duration representations
    // ------------------------------------------------------------------

    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeSpan = std::chrono::steady_clock::duration;

    namespace time {
        [[nodiscard]] double seconds(const TimeSpan duration) noexcept {
            return std::chrono::duration_cast<std::chrono::duration<double>>(duration).count();
        }
    }

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
    class ITimerClock {
    public:
        [[nodiscard]] virtual TimePoint now() noexcept = 0;

        [[nodiscard]] static ITimerClock &steady() noexcept;
    };

    class TimerManager final {
        struct Event {
            TimePoint m_date{};
            std::move_only_function<void(TimePoint) noexcept> m_callback{};

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
        using Callback = std::move_only_function<void(TimePoint) noexcept>;

        explicit TimerManager(ITimerClock &clock = ITimerClock::steady()) noexcept;

        [[nodiscard]] TimePoint now() const noexcept;

        void schedule(const TimePoint date, Callback &&callback) noexcept;

        void tick() noexcept;

        static TimerManager &mainTimer() noexcept;
    };
}

export namespace std {
    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimePoint &value) noexcept {
        return value.time_since_epoch().count();
    }

    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimeSpan &value) noexcept {
        return value.count();
    }
}
