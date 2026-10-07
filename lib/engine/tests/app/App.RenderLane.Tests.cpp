module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace RenderLaneGate {
        // ------------------------------------------------------------------
        // P2 demo tests per the wait/wake mechanism spec:
        // - 2.A pacer-seam tests (event-serving slices, posted wake,
        //   hint-wins) through the production FramePacer wait path;
        // - 2.B lane scenarios (exit wakeup, credit-return wakeup,
        //   real-jthread wake, full-queue exit) through the real RenderLane;
        // - channel precedents (control independence, distinguishable
        //   full/empty/closed outcomes, nonblocking discipline).
        //
        // No GPU, no window: the executor counts frames instead of
        // submitting, and the sweep reports a fixed empty ledger. What IS
        // real: the worker thread, all three bounded channels, the Signal
        // multiplex wait, the pacer slicing, and the teardown order.
        // ------------------------------------------------------------------

        struct ManualClock final : ITimerClock {
            TimePoint m_now{};

            [[nodiscard]] TimePoint now() const noexcept override {
                return m_now;
            }
        };

        PPR_UNIT_TEST (pacer_seam_serves_event_slices) {
            // 2.A: one installed sleep slice = one pump + one native wait.
            // A 10ms deadline at the 2ms quantum yields exactly 5 slices
            // through the PRODUCTION wait path (fake clock, no threads).
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            FramePacer pacer{clock};

            u32 pumps = 0u;
            u32 waits = 0u;
            TimeSpan waited{};
            pacer.setSleepFunction(FramePacer::SleepFunction{[&](const TimeSpan slice) {
                ++pumps;
                ++waits;
                waited += slice;
                clock.m_now += slice;
            }});

            pacer.waitForTarget(std::chrono::milliseconds{10}, clock.m_now);
            PPR_TEST_ASSERT(pumps == 5u);
            PPR_TEST_ASSERT(waits == 5u);
            PPR_TEST_ASSERT(waited == std::chrono::milliseconds{10});
            PPR_TEST_ASSERT(clock.m_now == TimePoint{} + std::chrono::milliseconds{1010});
        };

        PPR_UNIT_TEST (pacer_seam_posted_wake_wins) {
            // 2.A hint-wins: a posted wake (predicate latch, standing in
            // for PulseEvent + postEmptyEvent) stops the wait after 2
            // slices instead of running the full deadline.
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            FramePacer pacer{clock};

            u32 slices = 0u;
            bool wake_posted = false;
            pacer.setSleepFunction(FramePacer::SleepFunction{[&](const TimeSpan slice) {
                ++slices;
                clock.m_now += slice;
                if (slices == 2u) {
                    wake_posted = true;
                }
            }});

            pacer.waitForTarget(std::chrono::milliseconds{200}, clock.m_now, FramePacer::WakePredicate{[&]() -> bool {
                return wake_posted;
            }});
            PPR_TEST_ASSERT(slices == 2u);
            PPR_TEST_ASSERT(clock.m_now < TimePoint{} + std::chrono::milliseconds{1200});
        };

        PPR_UNIT_TEST (pacer_seam_real_thread_wake) {
            // 2.A with a real poster thread: the predicate observes an
            // atomic set from a jthread mid-wait, bounding the slices well
            // below the full 200ms deadline (generous bound: scheduling
            // jitter must not flake this).
            ManualClock clock{};
            clock.m_now = TimePoint{} + std::chrono::milliseconds{1000};
            FramePacer pacer{clock};

            std::atomic<bool> exit_hint{false};
            std::atomic<u32> slices{0u};
            pacer.setSleepFunction(FramePacer::SleepFunction{[&](const TimeSpan slice) {
                slices.fetch_add(1u, std::memory_order_acq_rel);
                clock.m_now += slice;
                // Real (short) sleep: fake-clock slices alone execute in
                // microseconds and would deterministically outrun any real
                // poster thread, making cross-thread observation unprovable.
                // 1ms sleeps against the 5ms poster keep the <50u bound at
                // ~20x margin without wall-clock gambling.
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }});

            // Deterministic poster: fire as soon as two slices ran (no
            // wall-clock gambling under load); the bound below then fails
            // only if the predicate itself stops observing the flag.
            // Bounded spin as a backstop so a never-advancing clock cannot
            // hang the join.
            std::jthread poster{[&]() {
                for (u32 spin = 0u; spin < 1000000u; ++spin) {
                    if (slices.load(std::memory_order_acquire) >= 2u) {
                        break;
                    }
                    std::this_thread::yield();
                }
                exit_hint.store(true, std::memory_order_release);
            }};
            pacer.waitForTarget(std::chrono::milliseconds{200}, clock.m_now, FramePacer::WakePredicate{[&]() -> bool {
                return exit_hint.load(std::memory_order_acquire);
            }});
            poster.join();
            PPR_TEST_ASSERT(slices.load(std::memory_order_acquire) < 50u);
            PPR_TEST_ASSERT(exit_hint.load(std::memory_order_acquire));
        };

        struct CountingExecutor {
            std::atomic<u64> m_executed{0u};
            std::atomic<u64> m_last_frame{0u};
            // When set, the executor parks until released (saturation rig).
            std::binary_semaphore m_gate{0};
            bool m_block{false};

            std::error_code operator()(const RenderFrameRequest &request, RenderFrameResult &result) {
                if (m_block) {
                    m_gate.acquire();
                }
                if (request.m_packet) {
                    m_last_frame.store(request.m_packet->m_frame_id, std::memory_order_release);
                }
                result.m_submit_value = kFenceValueNone;
                m_executed.fetch_add(1u, std::memory_order_acq_rel);
                return default_value_v;
            }
        };

        [[nodiscard]] RenderFrameRequest makeRequest(const u64 frame_id) {
            RenderFrameRequest request{};
            auto packet = std::make_shared<FramePacket>();
            packet->m_frame_id = frame_id;
            packet->m_framebuffer_size = int2{64, 64};
            request.m_packet = std::move(packet);
            return request;
        }

        // Drains until `frame_id` completes or the bound expires.
        [[nodiscard]] bool awaitResult(RenderLane &lane, const u64 frame_id, Array<RenderFrameResult> &scratch) {
            for (int spin = 0; spin < 500; ++spin) {
                scratch.clear();
                std::ignore = lane.drainResults(scratch);
                for (const RenderFrameResult &result: scratch) {
                    if (result.m_frame_id == frame_id and not result.m_dropped) {
                        return true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            return false;
        }

        PPR_UNIT_TEST (lane_exit_wakeup_joins_promptly) {
            // 2.B exit wakeup: a parked worker (no frames queued) observes
            // the exit signal and joins; the lane reports stopped.
            RenderLane lane{};
            CountingExecutor executor{};
            std::atomic<u32> wakes{0u};
            PPR_TEST_ASSERT(not lane.start(
                RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                RenderFrameExecutor{[&](const RenderFrameRequest &request, RenderFrameResult &result) {
                    return executor(request, result);
                }},
                RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                    out_outstanding = 0u;
                    return default_value_v;
                }},
                RenderLaneWake{[&]() noexcept { wakes.fetch_add(1u, std::memory_order_acq_rel); }}));
            PPR_TEST_ASSERT(lane.isRunning());

            PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(7u)));
            Array<RenderFrameResult> scratch{};
            PPR_TEST_ASSERT(awaitResult(lane, 7u, scratch));
            PPR_TEST_ASSERT(executor.m_executed.load(std::memory_order_acquire) == 1u);

            lane.requestStop();
            lane.join();
            PPR_TEST_ASSERT(not lane.isRunning());
            PPR_TEST_ASSERT(wakes.load(std::memory_order_acquire) >= 1u);
        };

        PPR_UNIT_TEST (lane_credit_return_wakes_main) {
            // 2.B credit-return wakeup: every executed frame wakes main
            // through the hint callback (the admission pulse in production).
            RenderLane lane{};
            CountingExecutor executor{};
            std::atomic<u32> wakes{0u};
            PPR_TEST_ASSERT(not lane.start(
                RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                RenderFrameExecutor{[&](const RenderFrameRequest &request, RenderFrameResult &result) {
                    return executor(request, result);
                }},
                RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                    out_outstanding = 0u;
                    return default_value_v;
                }},
                RenderLaneWake{[&]() noexcept { wakes.fetch_add(1u, std::memory_order_acq_rel); }}));
            PPR_DEFER {
                lane.requestStop();
                lane.join();
            };

            const u32 wakes_before = wakes.load(std::memory_order_acquire);
            PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(11u)));
            Array<RenderFrameResult> scratch{};
            PPR_TEST_ASSERT(awaitResult(lane, 11u, scratch));
            PPR_TEST_ASSERT(wakes.load(std::memory_order_acquire) > wakes_before);
        };

        PPR_UNIT_TEST (lane_control_wake_without_work) {
            // 2.B real-jthread wake: a control-only wake cuts the
            // multiplex wait short without any frame queued; the lane keeps
            // serving frames afterwards (control is independent of frame
            // capacity in both directions).
            RenderLane lane{};
            CountingExecutor executor{};
            std::atomic<u32> wakes{0u};
            PPR_TEST_ASSERT(not lane.start(
                RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                RenderFrameExecutor{[&](const RenderFrameRequest &request, RenderFrameResult &result) {
                    return executor(request, result);
                }},
                RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                    out_outstanding = 0u;
                    return default_value_v;
                }},
                RenderLaneWake{[&]() noexcept { wakes.fetch_add(1u, std::memory_order_acq_rel); }}));
            PPR_DEFER {
                lane.requestStop();
                lane.join();
            };

            const u32 wakes_before = wakes.load(std::memory_order_acquire);
            lane.postControl(RenderControl::Kind::kConfigChanged);
            // The config wake arrives promptly (bounded spin); it must not
            // disturb later frames.
            bool config_observed = false;
            for (int spin = 0; spin < 500 and not config_observed; ++spin) {
                config_observed = wakes.load(std::memory_order_acquire) > wakes_before;
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            PPR_TEST_ASSERT(config_observed);

            PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(13u)));
            Array<RenderFrameResult> scratch{};
            PPR_TEST_ASSERT(awaitResult(lane, 13u, scratch));
        };

        PPR_UNIT_TEST (lane_full_queue_exit_never_blocks) {
            // 2.B full-queue exit: with the executor parked and the frame
            // transport saturated, the third submit fails closed with
            // capacity-busy (drop the frame, never block), and signalling
            // exit still returns immediately — the close/wait/flush hole
            // (which would strand the producer behind a full queue) is
            // never used on this path.
            RenderLane lane{};
            CountingExecutor executor{};
            executor.m_block = true;
            PPR_TEST_ASSERT(not lane.start(
                RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                RenderFrameExecutor{[&](const RenderFrameRequest &request, RenderFrameResult &result) {
                    return executor(request, result);
                }},
                RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                    out_outstanding = 0u;
                    return default_value_v;
                }},
                RenderLaneWake{[]() noexcept {}}));

            bool executor_released = false;
            PPR_DEFER {
                // Best-effort unpark: an early assert must not strand the
                // worker in the executor gate (~RenderLane joins it and
                // would hang forever with no output).
                if (not executor_released) {
                    executor.m_block = false;
                    executor.m_gate.release();
                    executor_released = true;
                }
            };

            PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(21u)));
            PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(22u)));
            // Transport capacity is page-rounded well above the requested
            // depth, so fill until the first busy: deterministic (the parked
            // worker drains nothing, reserves never block) and bounded by
            // construction. Saturation is transient backpressure with the
            // busy code (never an app failure, never a block).
            std::error_code busy{};
            for (u32 frame = 23u; frame < 23u + 65536u; ++frame) {
                busy = lane.submitFrame(makeRequest(frame));
                if (busy == std::make_error_code(std::errc::device_or_resource_busy)) {
                    break;
                }
                PPR_TEST_ASSERT(not busy);
            }
            PPR_TEST_ASSERT(busy == std::make_error_code(std::errc::device_or_resource_busy));

            // Exit signalling is nonblocking even behind the full queue...
            lane.requestStop();
            // ...so the test can release the rig and join deterministically:
            // parked frames finish, queued work either runs or settles as
            // dropped on the way out.
            executor.m_block = false;
            executor.m_gate.release();
            executor.m_gate.release();
            executor_released = true;
            lane.join();
            PPR_TEST_ASSERT(not lane.isRunning());
            // The busy submit above counted as a transport drop.
            PPR_TEST_ASSERT(lane.droppedCount() >= 1u);
        };

        // G2-3 wake boundary incl. exit: with work outstanding (lane parked
        // in the bounded waitFor quantum loop) AND at the idle wait, the
        // exit control cuts the park short in both cases — every notify is
        // published under the park mutex, so a signal between the predicate
        // check and the park can never be lost.
        PPR_UNIT_TEST (lane_exit_wake_covers_idle_and_inflight_parks) {
            {
                // Inflight case: the executor parks, so the frame is never
                // retired and the lane's estimate stays nonzero — the main
                // wait loop uses waitFor. A stop from another thread must
                // still terminate the worker promptly.
                RenderLane lane{};
                std::atomic<u64> entered{0u};
                std::binary_semaphore gate{0};
                PPR_TEST_ASSERT(not lane.start(
                    RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                    RenderFrameExecutor{[&](const RenderFrameRequest &, RenderFrameResult &result) {
                        entered.fetch_add(1u, std::memory_order_acq_rel);
                        gate.acquire();
                        result.m_submit_value = kFenceValueNone;
                        return default_value_v;
                    }},
                    RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                        out_outstanding = 1u;
                        return default_value_v;
                    }},
                    RenderLaneWake{[]() noexcept {}}));
                PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(51u)));
                for (int spin = 0; spin < 500 and entered.load(std::memory_order_acquire) == 0u; ++spin) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
                PPR_TEST_ASSERT(entered.load(std::memory_order_acquire) == 1u);
                lane.requestStop();
                gate.release();
                const auto started = std::chrono::steady_clock::now();
                std::ignore = lane.join();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                PPR_TEST_ASSERT(not lane.isRunning());
                PPR_TEST_ASSERT(elapsed.count() < 25000);
            }
            {
                // Idle case: no frames, estimate zero — the lane parks in
                // the plain wait. Exit must still wake it.
                RenderLane lane{};
                PPR_TEST_ASSERT(not lane.start(
                    RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                    RenderFrameExecutor{
                        [](const RenderFrameRequest &, RenderFrameResult &) { return default_value_v; }},
                    RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                        out_outstanding = 0u;
                        return default_value_v;
                    }},
                    RenderLaneWake{[]() noexcept {}}));
                lane.requestStop();
                const auto started = std::chrono::steady_clock::now();
                std::ignore = lane.join();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                PPR_TEST_ASSERT(not lane.isRunning());
                PPR_TEST_ASSERT(elapsed.count() < 25000);
            }
        };

        // G2-5 destruction order: the worker is joined while every mutex,
        // cv, callback, and atomic it touches is still alive — the join
        // now happens before those members are destroyed. A lane destroyed
        // with a frame in flight must stop, join, and tear down cleanly.
        PPR_UNIT_TEST (destruction_with_live_work_joins_before_members_die) {
            std::atomic<u64> entered{0u};
            std::binary_semaphore gate{0};
            std::atomic<u64> completed{0u};
            {
                RenderLane lane{};
                PPR_TEST_ASSERT(not lane.start(
                    RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                    RenderFrameExecutor{[&](const RenderFrameRequest &, RenderFrameResult &result) {
                        entered.fetch_add(1u, std::memory_order_acq_rel);
                        gate.acquire();
                        result.m_submit_value = kFenceValueNone;
                        completed.fetch_add(1u, std::memory_order_acq_rel);
                        return default_value_v;
                    }},
                    RenderLaneSweep{[](u32 &out_outstanding) -> std::error_code {
                        out_outstanding = 0u;
                        return default_value_v;
                    }},
                    RenderLaneWake{[]() noexcept {}}));
                PPR_TEST_ASSERT(not lane.submitFrame(makeRequest(61u)));
                for (int spin = 0; spin < 500 and entered.load(std::memory_order_acquire) == 0u; ++spin) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
                PPR_TEST_ASSERT(entered.load(std::memory_order_acquire) == 1u);
                gate.release();
                // Let the in-flight frame drain, then destroy WITHOUT an
                // explicit requestStop/join: ~RenderLane must still join
                // the worker before its sync/callback members die.
                for (int spin = 0; spin < 500 and completed.load(std::memory_order_acquire) == 0u; ++spin) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
            }
            PPR_TEST_ASSERT(completed.load(std::memory_order_acquire) == 1u);
        };

        // SUPERSEDED as Gate-2 item-4 evidence (kept as a lane-transport
        // unit precedent only): this test proves the admission estimate
        // with a synthetic sweep/executor and counts construction INSIDE
        // the lane executor. Item 4 is now proven instead by the
        // lane-enabled offscreen run-gate fixture in App.RunGate.Tests.cpp
        // (real Application::run() admission, real offscreen submissions
        // held GPU-pending, construction counted at the START of the
        // derived client update). Do not cite this test for item 4.
        //
        // G2-6 lane-enabled admission window (third-construction gate):
        // the real lane worker plus the deterministic queue-gate fence
        // design (host-signalled retirement ledger + gated executor —
        // the same shape as the B3' queue-gate evidence in
        // App.FrameFence.Tests.cpp) proves the lane-published admission
        // estimate holds the construction count at two across the
        // paused-worker and CPU-settled/GPU-pending states, and that
        // retirement reopens admission. No window, no GPU: the sweep
        // callback stands in for fence retirement.
        //
        // Exact-once scope: each admission's result echoes its
        // (epoch, frame id) reservation identity exactly once (asserted).
        // Drop/cancel settlement rides the teardown/queued-drain paths
        // already covered above; the ambiguous-submit (possibly-executed)
        // outcome is an Application-side settlement quarantine, covered at
        // that layer — both noted, not duplicated here.
        PPR_UNIT_TEST (third_construction_waits_for_lane_retirement) {
            RenderLane lane{};
            std::atomic<u64> constructed{0u};
            // Charged-but-not-retired ledger standing in for the GPU:
            // the frame leaves exactly once, when the host opens the gate.
            std::atomic<u32> gpu_outstanding{0u};
            std::atomic<bool> pause_on_enter{false};
            std::binary_semaphore executor_pause{0};

            PPR_TEST_ASSERT(not lane.start(
                RenderLaneOperation{[]() -> std::error_code { return default_value_v; }},
                RenderFrameExecutor{[&](const RenderFrameRequest &, RenderFrameResult &result) {
                    const u64 n = constructed.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                    if (pause_on_enter.load(std::memory_order_acquire) and n == 2u) {
                        // Paused-worker state: f2 has been constructed but
                        // the worker stalls before settlement.
                        executor_pause.acquire();
                    }
                    gpu_outstanding.fetch_add(1u, std::memory_order_acq_rel);
                    result.m_submit_value = kFenceValueNone;
                    return default_value_v;
                }},
                RenderLaneSweep{[&](u32 &out_outstanding) -> std::error_code {
                    out_outstanding = gpu_outstanding.load(std::memory_order_acquire);
                    return default_value_v;
                }},
                RenderLaneWake{[]() noexcept {}}));
            PPR_DEFER {
                lane.requestStop();
                lane.join();
            };

            auto makeCharged = [](const u64 frame_id) -> RenderFrameRequest {
                RenderFrameRequest request = makeRequest(frame_id);
                request.m_reservation_epoch = 7u;
                request.m_reservation_frame_id = frame_id;
                return request;
            };

            // Main-side admission policy mirrors the production charge
            // bound (probeFrameAdmissionPreUpdate_: no third construction
            // while kMaxOutstandingFrames credits are charged): submit only
            // while the estimate-published window has room.
            auto admit = [&](const u64 frame_id) -> std::error_code {
                if (lane.inflightEstimate() >= kMaxOutstandingFrames) {
                    return std::make_error_code(std::errc::device_or_resource_busy);
                }
                return lane.submitFrame(makeCharged(frame_id));
            };

            Array<RenderFrameResult> results{};
            auto waitDrain = [&](const u64 frame_id) -> bool {
                for (int spin = 0; spin < 500; ++spin) {
                    Array<RenderFrameResult> batch{};
                    std::ignore = lane.drainResults(batch);
                    for (RenderFrameResult &r: batch) {
                        results.push_back(std::move(r));
                    }
                    for (const RenderFrameResult &r: results) {
                        if (r.m_frame_id == frame_id) {
                            return true;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
                return false;
            };

            // Frame 1: executes and settles (CPU done), GPU still pending —
            // the sweep pins outstanding==1, the estimate holds one slot.
            PPR_TEST_ASSERT(not lane.submitFrame(makeCharged(101u)));
            PPR_TEST_ASSERT(waitDrain(101u));
            PPR_TEST_ASSERT(constructed.load(std::memory_order_acquire) == 1u);
            PPR_TEST_ASSERT(lane.inflightEstimate() == 1u);

            // Frame 2: constructed, then the worker pauses. With f1 still
            // GPU-pending the estimate is executing(1) + outstanding(1)
            // == 2 and a third admission is refused — construction stays
            // at two across BOTH states.
            pause_on_enter.store(true, std::memory_order_release);
            PPR_TEST_ASSERT(not lane.submitFrame(makeCharged(102u)));
            for (int spin = 0; spin < 500 and constructed.load(std::memory_order_acquire) == 1u; ++spin) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            PPR_TEST_ASSERT(constructed.load(std::memory_order_acquire) == 2u);
            for (int spin = 0; spin < 500 and lane.inflightEstimate() != 2u; ++spin) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            PPR_TEST_ASSERT(lane.inflightEstimate() == 2u);

            PPR_TEST_ASSERT(admit(103u) == std::make_error_code(std::errc::device_or_resource_busy));
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            PPR_TEST_ASSERT(constructed.load(std::memory_order_acquire) == 2u);

            // Retire f1 (the gate opens) and release f2. f2 settles into
            // the same CPU-settled/GPU-pending state: the estimate holds
            // exactly one charged slot again.
            const u32 retired = gpu_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            PPR_TEST_ASSERT(retired == 1u);
            executor_pause.release();
            PPR_TEST_ASSERT(waitDrain(102u));
            PPR_TEST_ASSERT(lane.inflightEstimate() == 1u);

            // Retirement reopens admission: the third frame constructs.
            PPR_TEST_ASSERT(not admit(103u));
            for (int spin = 0; spin < 500 and constructed.load(std::memory_order_acquire) < 3u; ++spin) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            PPR_TEST_ASSERT(constructed.load(std::memory_order_acquire) == 3u);
            PPR_TEST_ASSERT(waitDrain(103u));

            // Exact-once: one settlement per admitted frame, echoing the
            // admission's reservation identity — drop/cancel/error slots
            // ride the same echo (teardown paths above), so a count by
            // (epoch, frame id) is the whole contract.
            std::map<u64, u64> echoes{};
            for (const RenderFrameResult &r: results) {
                PPR_TEST_ASSERT(r.m_reservation_epoch == 7u);
                ++echoes[r.m_reservation_frame_id];
            }
            PPR_TEST_ASSERT(echoes.size() == 3u);
            for (const auto &[frame_id, count]: echoes) {
                std::ignore = frame_id;
                PPR_TEST_ASSERT(count == 1u);
            }
            gpu_outstanding.store(0u, std::memory_order_release);
        };

        PPR_UNIT_TEST (channel_control_independent_of_frame_capacity) {
            // Precedent: a saturated frame transport never delays control.
            // Direct RawChannel proof (the lane's own post path relies on
            // it): drop_if_full reserve fails closed instantly on full.
            RawChannel frames{1u, sizeof(u64)};
            // Requested depth is page-rounded: fill until full. Deterministic
            // (finite capacity, nonblocking reserves, no consumer running).
            bool saw_full = false;
            for (u32 fill = 0u; fill < 1048576u; ++fill) {
                auto reserved = frames.producerReserve(sizeof(u64), RawChannel::drop_if_full);
                if (not reserved.has_value()) {
                    PPR_TEST_ASSERT(reserved.error() == RawChannel::error_full);
                    saw_full = true;
                    break;
                }
                new (reserved->data()) u64{1u};
                frames.producerSubmit(*reserved);
            }
            PPR_TEST_ASSERT(saw_full);
            RawChannel control{8u, sizeof(RenderControl)};
            {
                auto reserved = control.producerReserve(sizeof(RenderControl), RawChannel::drop_if_full);
                PPR_TEST_ASSERT(reserved.has_value());
                new (reserved->data()) RenderControl{.m_kind = RenderControl::Kind::kExit};
                control.producerSubmit(*reserved);
            }
            {
                auto acquired = control.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(acquired.has_value());
                PPR_TEST_ASSERT(
                    static_cast<const RenderControl *>(acquired->data())->m_kind == RenderControl::Kind::kExit);
                control.consumerRelease(*acquired);
            }
            // Drain everything so destruction is quiet (the destructor
            // closes, and closing behind a full queue would block).
            for (;;) {
                auto acquired = frames.consumerAcquire(RawChannel::peek_without_blocking);
                if (not acquired.has_value()) {
                    break;
                }
                std::destroy_at(static_cast<u64 *>(acquired->data()));
                frames.consumerRelease(*acquired);
            }
        };

        PPR_UNIT_TEST (channel_outcomes_stay_distinguishable) {
            // Precedent: full/empty/closed outcomes never alias (the lane
            // maps them to busy/cancelled without inventing completion).
            // Note the close-hole this design avoids: RawChannel::close()
            // reserves with wait_if_full, so closing behind a FULL queue
            // blocks until it drains — the lane therefore never closes (or
            // flushes, or wait-reserves) on any signalling path.
            RawChannel channel{1u, sizeof(u64)};
            {
                const auto empty = channel.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(not empty.has_value());
                PPR_TEST_ASSERT(empty.error() == RawChannel::error_empty);
            }
            {
                auto reserved = channel.producerReserve(sizeof(u64), RawChannel::drop_if_full);
                PPR_TEST_ASSERT(reserved.has_value());
                new (reserved->data()) u64{9u};
                channel.producerSubmit(*reserved);
            }
            // Requested depth is page-rounded: fill until full (deterministic:
            // finite capacity, nonblocking reserves, no consumer running).
            bool saw_full = false;
            for (u32 fill = 0u; fill < 1048576u; ++fill) {
                auto reserved = channel.producerReserve(sizeof(u64), RawChannel::drop_if_full);
                if (not reserved.has_value()) {
                    PPR_TEST_ASSERT(reserved.error() == RawChannel::error_full);
                    saw_full = true;
                    break;
                }
                new (reserved->data()) u64{8u};
                channel.producerSubmit(*reserved);
            }
            PPR_TEST_ASSERT(saw_full);
            {
                auto acquired = channel.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(acquired.has_value());
                PPR_TEST_ASSERT(*static_cast<const u64 *>(acquired->data()) == 9u);
                std::destroy_at(static_cast<u64 *>(acquired->data()));
                channel.consumerRelease(*acquired);
            }
            // Drain the rest before close (closing behind a full queue
            // would block in the reserve).
            for (;;) {
                auto acquired = channel.consumerAcquire(RawChannel::peek_without_blocking);
                if (not acquired.has_value()) {
                    break;
                }
                std::destroy_at(static_cast<u64 *>(acquired->data()));
                channel.consumerRelease(*acquired);
            }
            PPR_TEST_ASSERT(not channel.close());
            {
                // Producers observe the close immediately and nonblockingly.
                const auto refused = channel.producerReserve(sizeof(u64), RawChannel::drop_if_full);
                PPR_TEST_ASSERT(not refused.has_value());
                PPR_TEST_ASSERT(refused.error() == RawChannel::error_closed);
            }
            {
                // The close marker surfaces exactly once...
                const auto closed = channel.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(not closed.has_value());
                PPR_TEST_ASSERT(closed.error() == RawChannel::error_closed);
            }
            {
                // ...then the drained queue reads empty (never a second
                // close, never invented completion).
                const auto drained = channel.consumerAcquire(RawChannel::peek_without_blocking);
                PPR_TEST_ASSERT(not drained.has_value());
                PPR_TEST_ASSERT(drained.error() == RawChannel::error_empty);
            }
        };
    }
}

namespace pP::tests {
    const UnitTest render_lane = UnitTest::Named("render_lane") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::RenderLaneGate::pacer_seam_serves_event_slices,
            detail::RenderLaneGate::pacer_seam_posted_wake_wins,
            detail::RenderLaneGate::pacer_seam_real_thread_wake,
            detail::RenderLaneGate::lane_exit_wakeup_joins_promptly,
            detail::RenderLaneGate::lane_credit_return_wakes_main,
            detail::RenderLaneGate::lane_control_wake_without_work,
            detail::RenderLaneGate::lane_full_queue_exit_never_blocks,
            detail::RenderLaneGate::lane_exit_wake_covers_idle_and_inflight_parks,
            detail::RenderLaneGate::destruction_with_live_work_joins_before_members_die,
            detail::RenderLaneGate::third_construction_waits_for_lane_retirement,
            detail::RenderLaneGate::channel_control_independent_of_frame_capacity,
            detail::RenderLaneGate::channel_outcomes_stay_distinguishable,
        });
    };

    const UnitTest &render_laneTests() noexcept {
        return render_lane;
    }
} // namespace pP::tests
