module;
#include "pP/Macros.h"
export module engine.core:timer;

import :assert;
import :containers.stable_vector;
import :containers.stl;
import :opaque;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // time point and duration representations
    // ------------------------------------------------------------------

    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeSpan = std::chrono::steady_clock::duration;
    using TimeDuration = std::chrono::duration<double>;

    namespace time {
        [[nodiscard]] TimePoint now() noexcept;

        [[nodiscard]] TimeSpan since(TimePoint started_at) noexcept;

        [[nodiscard]] double seconds(TimeSpan duration) noexcept;

        [[nodiscard]] double seconds(TimeDuration duration) noexcept {
            return duration.count();
        }
    }

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
    class ITimerClock {
    protected:
        ~ITimerClock() = default;

    public:
        [[nodiscard]] virtual TimePoint now() const noexcept = 0;

        [[nodiscard]] static ITimerClock &steady() noexcept;
    };

    class TimerExplicitClock final : public ITimerClock {
    public:
        TimePoint m_now{};
        TimeSpan m_elapsed{};

        void tick(const TimePoint new_time) noexcept {
            m_elapsed = new_time - m_now;
            m_now = new_time;
        }

        void reset(const TimePoint new_time) noexcept {
            m_elapsed = {};
            m_now = new_time;
        }

        [[nodiscard]] TimePoint now() const noexcept override {
            return m_now;
        }

        [[nodiscard]] TimePoint last() const noexcept {
            return m_now - m_elapsed;
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

        TimePoint updateLastTick_() noexcept;

        ITimerClock *const m_clock{nullptr};
        std::atomic<long long> m_last_tick{0};

        std::mutex m_mutex{};
        Array<Event> m_queue{};

    public:
        using Callback = std::move_only_function<void(TimePoint) noexcept>;

        explicit TimerManager(ITimerClock &clock = ITimerClock::steady()) noexcept;

        [[nodiscard]] TimePoint now() const noexcept;

        void schedule(TimePoint date, Callback &&callback) noexcept;

        void tick() noexcept;

        static TimerManager &mainTimer() noexcept;
    };
}

export namespace std {
    template<pP::details::TChar CharT>
    struct formatter<pP::TimeSpan, CharT> {
        template<typename FormatParseContextT>
        static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
            return ctx.begin();
        }

        template<typename FormatContextT>
        auto format(const pP::TimeSpan &td, FormatContextT &ctx) const
            -> decltype(ctx.out()) {
            using namespace std::chrono;
            const auto ns = duration_cast<nanoseconds>(td).count();

            if (ns < 1'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{}ns"), ns);
            }
            if (ns < 100'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}µs"), ns / 1'000.0);
            }
            if (ns < 1'000'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{}µs"), duration_cast<microseconds>(td).count());
            }
            if (ns < 1'000'000'000LL) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}ms"), ns / 1'000'000.0);
            }
            return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.2f}s"), ns / 1'000'000'000.0);
        }
    };

    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimePoint &value) noexcept {
        return value.time_since_epoch().count();
    }

    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimeSpan &value) noexcept {
        return value.count();
    }
}
