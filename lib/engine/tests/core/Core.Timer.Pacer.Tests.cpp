module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // timer pacer
    // ------------------------------------------------------------------

    namespace Pacer {
        // Fake-clock harness: manual steady-clock seam + recording sleep
        // primitive. Slicing, deadline, and wake-predicate logic stay in
        // production code, so every test below exercises the real wait path.
        // No real sleeping, no threads — fully deterministic.
        struct ManualClock final : ITimerClock {
            TimePoint m_now{};

            [[nodiscard]] TimePoint now() const noexcept override {
                return m_now;
            }
        };

        struct SleepRecorder {
            ManualClock *m_clock{nullptr};
            TimeSpan m_slept{};
            u32 m_calls{0u};
            bool m_stop_after_first{false};
            bool m_stop{false};

            void operator()(const TimeSpan duration) {
                ++m_calls;
                m_slept += duration;
                if (m_clock != nullptr) {
                    m_clock->m_now += duration;
                }
                if (m_stop_after_first) {
                    m_stop = true;
                }
            }

            [[nodiscard]] FramePacer::WakePredicate predicate() {
                return FramePacer::WakePredicate{[this]() -> bool { return m_stop; }};
            }
        };

        constexpr TimeSpan kFrame16 = std::chrono::milliseconds{16};

        PPR_UNIT_TEST (early_waits_to_deadline) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            // Clock is at the previous tick: pacer must wait the full target
            // in wake-quantum-bounded slices throughout (16ms = 8 x 2ms).
            pacer.waitForTarget(kFrame16, timer.now());
            PPR_TEST_ASSERT(recorder.m_calls == 8u);
            PPR_TEST_ASSERT(recorder.m_slept == kFrame16);
            PPR_TEST_ASSERT(clock.m_now == timer.now() + kFrame16);

            // Zero-target tick latches the waited time and reports it as dt.
            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(dt == kFrame16);
            PPR_TEST_ASSERT(timer.getDeltaTime() == kFrame16);
        };

        PPR_UNIT_TEST (late_skips_wait) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            // Already 20ms past the previous tick with a 16ms target.
            clock.m_now += std::chrono::milliseconds{20};
            pacer.waitForTarget(kFrame16, timer.now());
            PPR_TEST_ASSERT(recorder.m_calls == 0u);

            // Overrun is preserved as dt (no catch-up, no clamping).
            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(dt == std::chrono::milliseconds{20});
        };

        PPR_UNIT_TEST (uncapped_never_waits) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            pacer.waitForTarget(TimeSpan{}, timer.now());
            PPR_TEST_ASSERT(recorder.m_calls == 0u);
            PPR_TEST_ASSERT(not pacer.shouldWait(timer.now()));

            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(dt == TimeSpan{});
        };

        PPR_UNIT_TEST (deadline_anchors_to_previous_tick) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            // First frame consumes its 16ms quantum exactly.
            pacer.waitForTarget(kFrame16, timer.now());
            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(recorder.m_slept == kFrame16);

            // 4ms into the next frame the deadline is previous-tick + 16ms
            // (12ms out), not anchored to the original schedule: min-period
            // policy, never a catch-up burst.
            clock.m_now += std::chrono::milliseconds{4};
            const TimePoint previous = timer.now();
            PPR_TEST_ASSERT(pacer.shouldWait(FramePacer::deadlineFor(previous, kFrame16)));
            recorder.m_slept = TimeSpan{};
            recorder.m_calls = 0u;
            pacer.waitForTarget(kFrame16, previous);
            PPR_TEST_ASSERT(recorder.m_slept == std::chrono::milliseconds{12});
            PPR_TEST_ASSERT(clock.m_now == previous + kFrame16);
        };

        PPR_UNIT_TEST (exit_during_wait_stops_admission_early) {
            // Graceful exit arriving mid-wait: the predicate flips after the
            // first quantum slice, so the pacer returns without reaching the
            // deadline and admission stops — no cancellation involved.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            recorder.m_stop_after_first = true;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            pacer.waitForTarget(kFrame16, timer.now(), recorder.predicate());
            PPR_TEST_ASSERT(recorder.m_calls == 1u);
            PPR_TEST_ASSERT(clock.m_now < timer.now() + kFrame16);

            // The abandoned frame still latches honestly: dt is the partial
            // wait, and the loop's post-wait recheck observes termination.
            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(dt == recorder.m_slept);
        };

        PPR_UNIT_TEST (exit_early_in_background_wait_is_bounded) {
            // F2-slicing: an exit arriving near the start of a
            // background-length (200ms) wait must wake within one wake
            // quantum, not after an uninterrupted ~198ms bulk sleep.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            recorder.m_stop_after_first = true;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            constexpr TimeSpan kBackground = std::chrono::milliseconds{200};
            const TimePoint previous = timer.now();
            pacer.waitForTarget(kBackground, previous, recorder.predicate());
            PPR_TEST_ASSERT(recorder.m_calls == 1u);
            PPR_TEST_ASSERT(recorder.m_slept == FramePacer::kWakeQuantum);
            PPR_TEST_ASSERT(recorder.m_slept <= 2 * FramePacer::kWakeQuantum);
            PPR_TEST_ASSERT(clock.m_now < previous + kBackground);
            PPR_TEST_ASSERT(clock.m_now == previous + FramePacer::kWakeQuantum);
        };

        PPR_UNIT_TEST (target_change_wakes_within_bound_and_recomputes) {
            // F2-slicing: a target-eligibility change during an outstanding
            // background wait wakes within one quantum via the predicate;
            // the caller then recomputes the deadline from the same previous
            // tick with the new target before admission.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            recorder.m_stop_after_first = true;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            constexpr TimeSpan kBackground = std::chrono::milliseconds{200};
            const TimePoint previous = timer.now();

            // Outstanding background wait: target change observed after the
            // first quantum wakes within the response bound.
            pacer.waitForTarget(kBackground, previous, recorder.predicate());
            PPR_TEST_ASSERT(recorder.m_calls == 1u);
            PPR_TEST_ASSERT(recorder.m_slept <= 2 * FramePacer::kWakeQuantum);
            PPR_TEST_ASSERT(clock.m_now < previous + kBackground);

            // Recomputed wait with the new (foreground 16ms) target from the
            // same previous tick fills to the new deadline before admission.
            SleepRecorder second{};
            second.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { second(duration); });
            pacer.waitForTarget(kFrame16, previous);
            PPR_TEST_ASSERT(clock.m_now == previous + kFrame16);
            PPR_TEST_ASSERT(recorder.m_slept + second.m_slept == kFrame16);
        };

        PPR_UNIT_TEST (cancelled_wait_sleeps_nothing) {
            // Cancellation (latched error) reads through the same predicate:
            // an already-signalled stop skips every sleep slice.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            recorder.m_stop = true;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            pacer.waitForTarget(kFrame16, timer.now(), recorder.predicate());
            PPR_TEST_ASSERT(recorder.m_calls == 0u);
            PPR_TEST_ASSERT(clock.m_now == timer.now());
        };

        PPR_UNIT_TEST (target_change_recomputes_deadline) {
            // Focus/background transitions re-evaluate the target every
            // frame: the same previous tick with a new target waits the new
            // quantum, anchored identically.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};
            FramePacer pacer{clock};

            SleepRecorder recorder{};
            recorder.m_clock = &clock;
            pacer.setSleepFunction([&](const TimeSpan duration) { recorder(duration); });

            const TimePoint previous = timer.now();
            pacer.waitForTarget(kFrame16, previous);
            PPR_TEST_ASSERT(recorder.m_slept == kFrame16);

            constexpr TimeSpan kBackground = std::chrono::milliseconds{200};
            pacer.waitForTarget(kBackground, previous);
            PPR_TEST_ASSERT(recorder.m_slept == kBackground);
            PPR_TEST_ASSERT(clock.m_now == previous + kBackground);
        };

        PPR_UNIT_TEST (now_stays_latched_between_ticks) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};

            const TimePoint latched = timer.now();
            clock.m_now += std::chrono::milliseconds{50};
            PPR_TEST_ASSERT(timer.now() == latched);

            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(timer.now() == clock.m_now);
            PPR_TEST_ASSERT(dt == std::chrono::milliseconds{50});
            PPR_TEST_ASSERT(timer.getTotalElapsed() == std::chrono::milliseconds{50});
        };

        PPR_UNIT_TEST (callbacks_run_in_date_order_with_first_error) {
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};

            Array<int> order{};
            const std::error_code kFailure = std::make_error_code(std::errc::io_error);

            TimerManager::Callback later = [&](const TimePoint) -> std::error_code {
                order.push_back(2);
                return default_value_v;
            };
            TimerManager::Callback earlier = [&](const TimePoint) -> std::error_code {
                order.push_back(1);
                return kFailure;
            };
            const TimePoint base = timer.now();
            timer.schedule(base + std::chrono::milliseconds{5}, std::move(later));
            timer.schedule(base + std::chrono::milliseconds{3}, std::move(earlier));

            // Zero-target tick still dispatches every expired callback.
            clock.m_now += std::chrono::milliseconds{10};
            TimeSpan dt{};
            const std::error_code tick_err = timer.tick(&dt, TimeSpan{});
            PPR_TEST_ASSERT(tick_err == kFailure);
            PPR_TEST_ASSERT(order.size() == 2u);
            PPR_TEST_ASSERT(order[0u] == 1);
            PPR_TEST_ASSERT(order[1u] == 2);
        };

        PPR_UNIT_TEST (clock_error_with_exit_request_is_retained) {
            // REG-clock-exit (timer layer): a callback that requests exit
            // AND fails must still report its error — the run loop retains
            // it via m_request_exit(first_err) before retiring the frame
            // instead of discarding it on the exit break. Full run()
            // propagation belongs in tests/app (out of scope for this pass).
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};

            const std::error_code kFailure = std::make_error_code(std::errc::io_error);
            bool exit_requested = false;
            TimerManager::Callback cb = [&](const TimePoint) -> std::error_code {
                exit_requested = true;
                return kFailure;
            };
            const TimePoint base = timer.now();
            timer.schedule(base + std::chrono::milliseconds{5}, std::move(cb));

            clock.m_now += std::chrono::milliseconds{10};
            TimeSpan dt{};
            const std::error_code tick_err = timer.tick(&dt, TimeSpan{});
            PPR_TEST_ASSERT(tick_err == kFailure);
            PPR_TEST_ASSERT(exit_requested);

            // Simulate the run-loop exit path: the clock error is latched
            // for run()'s return value even though admission stops.
            std::error_code latched{};
            if (exit_requested and tick_err) {
                latched = tick_err;
            }
            PPR_TEST_ASSERT(latched == kFailure);
        };

        PPR_UNIT_TEST (deadline_context_fires_on_zero_target_tick) {
            // Cancellation-adjacent: a withDeadline context registered on
            // this manager fires via a zero-target tick — no same-manager
            // deadline wait involved (the pacer never blocks the tick).
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            TimerManager timer{clock};

            const TimePoint deadline = timer.now() + std::chrono::milliseconds{10};
            const SharedContext ctx = context::withDeadline(context::background(), deadline, timer);
            PPR_TEST_ASSERT(ctx->pollEvent() == false);

            TimeSpan dt{};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(ctx->pollEvent() == false);

            clock.m_now += std::chrono::milliseconds{15};
            PPR_TEST_ASSERT(not timer.tick(&dt, TimeSpan{}));
            PPR_TEST_ASSERT(ctx->pollEvent() == true);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest pacer = UnitTest::Named("pacer") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Pacer::early_waits_to_deadline,
            detail::Pacer::late_skips_wait,
            detail::Pacer::uncapped_never_waits,
            detail::Pacer::deadline_anchors_to_previous_tick,
            detail::Pacer::exit_during_wait_stops_admission_early,
            detail::Pacer::exit_early_in_background_wait_is_bounded,
            detail::Pacer::target_change_wakes_within_bound_and_recomputes,
            detail::Pacer::clock_error_with_exit_request_is_retained,
            detail::Pacer::cancelled_wait_sleeps_nothing,
            detail::Pacer::target_change_recomputes_deadline,
            detail::Pacer::now_stays_latched_between_ticks,
            detail::Pacer::callbacks_run_in_date_order_with_first_error,
            detail::Pacer::deadline_context_fires_on_zero_target_tick,
        });
    };

    const UnitTest &pacerTests() noexcept {
        return pacer;
    }
} // namespace pP::tests
