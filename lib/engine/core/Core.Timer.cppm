module;
#include "pP/Macros.h"
export module engine.core:timer;

import :containers.stl;
import :function.ref;
import :memory.pointer;
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

        [[nodiscard]] double seconds(const TimeDuration duration) noexcept {
            return duration.count();
        }
    }

    // ------------------------------------------------------------------
    // schedule events in the future, at a specific time
    // ------------------------------------------------------------------

    // ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
    class ITimerClock : public safe_object {
    protected:
        ~ITimerClock() = default;

    public:
        [[nodiscard]] virtual TimePoint now() const noexcept = 0;

        [[nodiscard]] static const ITimerClock &steady() noexcept;
    };

    class TimerManager final : public ITimerClock {
    public:
        using Callback = std23::move_only_function<std::error_code(TimePoint)>;

    private:
        struct Event {
            TimePoint m_date{};
            Callback m_callback{};

            [[nodiscard]] constexpr std::strong_ordering operator<=>(const Event &other) const noexcept {
                return m_date <=> other.m_date;
            }
        };

        safe_ptr<const ITimerClock> m_clock{};

        std::atomic<long long> m_last_tick{0};
        std::atomic<long long> m_delta_time{0};
        std::atomic<long long> m_total_elapsed{0};

        std::mutex m_mutex{};
        Array<Event> m_queue{};

    public:
        explicit TimerManager(const ITimerClock &clock = steady()) noexcept;

        [[nodiscard]] TimePoint now() const noexcept override;

        [[nodiscard]] TimeSpan getDeltaTime() const noexcept;

        [[nodiscard]] TimeSpan getTotalElapsed() const noexcept;

        void reset();

        void schedule(TimePoint date, Callback &&callback) noexcept;

        void scheduleAfter(TimeDuration duration, Callback &&callback) noexcept;

        std::error_code tick(TimeSpan *out_delta_time, TimeSpan target_period = {}) noexcept;
    };

    // ------------------------------------------------------------------
    // P0 serial-explicit frame pacer: owns admission timing OUTSIDE
    // TimerManager::tick. The application waits on the pacer, then ticks
    // with a zero target (latch + callbacks only). Waiting never depends
    // on a deadline callback from the same manager (which cannot tick
    // until the wait ends) — the wait here is external to the manager.
    // Min-period policy preserved: the deadline anchors to the previous
    // tick, with no catch-up bursts and no phase-locked scheduling.
    // ------------------------------------------------------------------

    class FramePacer final {
    public:
        // Sleep primitive seam: the fake-clock harness advances its manual
        // clock instead of sleeping. Slicing, deadline, and wake-predicate
        // logic stay in production code so tests exercise the real path.
        using SleepFunction = std23::move_only_function<void(TimeSpan)>;

        // Wake predicate: persistent exit/error state polled between sleep
        // slices (never a same-manager deadline callback). A graceful exit
        // is observed here, never converted into cancellation. Owning
        // move_only_function (not a function_ref view): empty by default,
        // same callable pattern as Callback/SleepFunction above.
        using WakePredicate = std23::move_only_function<bool()>;

        // Long sleeps are sliced throughout at this quantum, so a wake
        // predicate signal (exit, error, or target-eligibility change)
        // wakes within one quantum instead of a full background period.
        static constexpr TimeSpan kWakeQuantum = std::chrono::milliseconds{2};

        explicit FramePacer(const ITimerClock &clock = ITimerClock::steady()) noexcept;

        // Pure deadline computation, anchored to the previous tick.
        [[nodiscard]] static TimePoint deadlineFor(TimePoint previous_tick, TimeSpan target_period) noexcept {
            return previous_tick + target_period;
        }

        // True while the clock is still before the deadline.
        [[nodiscard]] bool shouldWait(TimePoint deadline) const noexcept;

        // External wakeable wait until the previous-tick-anchored deadline.
        // No-op for an empty (uncapped) target or when already late/overrun.
        // Every sleep slice is bounded by kWakeQuantum and the wake
        // predicate is rechecked between slices: a latched exit, an error,
        // or a target-eligibility change observed via the predicate stops
        // the wait within one quantum, without touching callback dispatch.
        // The caller re-samples its target after a predicate wake and
        // rechecks eligibility before admission. Non-const: invoking the stored sleep callable is an observable
        // call-state mutation, while deadline computation stays pure (see
        // shouldWait/deadlineFor).
        void waitForTarget(TimeSpan target_period, TimePoint previous_tick, WakePredicate should_wake = {}) noexcept;

        // Test seam: replaces only the sleep primitive (see SleepFunction).
        void setSleepFunction(SleepFunction &&sleep) noexcept;

    private:
        void sleepFor_(TimeSpan duration) noexcept;

        safe_ptr<const ITimerClock> m_clock{};
        SleepFunction m_sleep{};
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
        auto format(const pP::TimeSpan &td, FormatContextT &ctx) const -> decltype(ctx.out()) {
            using namespace std::chrono;
            const auto ns = duration_cast<nanoseconds>(td).count();

            if (ns < 1'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{}ns"), ns);
            }
            if (ns < 100'000) {
                return std::format_to(ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}µs"), static_cast<double>(ns) / 1'000.0);
            }
            if (ns < 1'000'000) {
                return std::format_to(
                    ctx.out(), PPR_LITERAL_FOR(CharT, "{}µs"), duration_cast<microseconds>(td).count());
            }
            if (ns < 1'000'000'000LL) {
                return std::format_to(
                    ctx.out(), PPR_LITERAL_FOR(CharT, "{:.1f}ms"), static_cast<double>(ns) / 1'000'000.0);
            }
            return std::format_to(
                ctx.out(), PPR_LITERAL_FOR(CharT, "{:.2f}s"), static_cast<double>(ns) / 1'000'000'000.0);
        }
    };

    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimePoint &value) noexcept {
        return value.time_since_epoch().count();
    }

    [[nodiscard]] constexpr pP::opaque::Value opaqueValue(const pP::TimeSpan &value) noexcept {
        return value.count();
    }
}
