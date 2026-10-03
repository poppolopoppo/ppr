module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

// Scheduler tests against the approved Oracle contract:
// - schedule/broadcast are nonblocking and take an optional tracking
//   group; a saturated queue reports RawChannel::error_full instead of
//   blocking. Admission failure touches neither the group counter nor its
//   error slot; success registers exactly once and the worker envelope
//   settles the group via done(err) exactly once.
// - WaitGroup::done(err) records the first error and reports whether the
//   caller was the last completer; wait() returns the recorded error
//   (empty on success) once the count reaches zero.
// - Task exceptions surface as ParallelScheduler::err_task_exception.
// - join gracefully DRAINS accepted tasks before returning; a nonzero
//   cause cancels first, an empty cause does not.
// - Cancelling the parent still drains accepted tasks but rejects later
//   admission.
// - Constructor failures throw (nonzero translated error); a null parent
//   is normalized to the background context.
// - forRange composes caller-lane error, then group error, then scheduler
//   error (first failure wins); queue-full falls back to the caller lane;
//   nested same-scheduler calls run serially. There is no parallel::sort
//   in the scheduler API.
//
// Synchronization rules used throughout:
// - PPR_TEST_ASSERT throws std::logic_error on failure, so worker bodies
//   never assert: they record into atomics and the parent thread asserts.
// - Gates are deterministic (std::latch arrival, atomic wait/notify) and
//   every gate has a PPR_DEFER scope-guard release, so a throwing assert
//   on the test thread can never strand a worker.

namespace pP::tests::detail {
    namespace Scheduler {
        [[nodiscard]] std::size_t queueBytes() noexcept {
            return static_cast<std::size_t>(hal::page_granularity);
        }

        constexpr int kMaxFillAttempts = 4096;

        void openGate(std::atomic<bool> &gate) noexcept {
            gate.store(true, std::memory_order_release);
            gate.notify_all();
        }

        void awaitGate(const std::atomic<bool> &gate) noexcept {
            gate.wait(false, std::memory_order_acquire);
        }

        PPR_UNIT_TEST (waitgroup_empty_wait_returns) {
            const WaitGroup wg{};
            PPR_TEST_ASSERT(not wg.wait());
        };

        PPR_UNIT_TEST (waitgroup_single_add_done) {
            WaitGroup wg{};
            wg.add(1u);
            wg.done();
            PPR_TEST_ASSERT(not wg.wait());
        };

        PPR_UNIT_TEST (waitgroup_multiple_outstanding) {
            WaitGroup wg{};
            constexpr std::size_t n = 8u;
            wg.add(n);
            for (std::size_t i = 0u; i < n; ++i) {
                wg.done();
            }
            PPR_TEST_ASSERT(not wg.wait());
        };

        PPR_UNIT_TEST (waitgroup_concurrent_done) {
            WaitGroup wg{};
            constexpr int n = 8;
            wg.add(static_cast<std::size_t>(n));

            std::vector<std::jthread> threads;
            threads.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                threads.emplace_back([&wg] { wg.done(); });
            }
            PPR_TEST_ASSERT(not wg.wait());
        };

        PPR_UNIT_TEST (waitgroup_reuse_after_wait) {
            WaitGroup wg{};
            for (int round = 0; round < 3; ++round) {
                wg.add(2u);
                wg.done();
                wg.done();
                PPR_TEST_ASSERT(not wg.wait());
            }
        };

        PPR_UNIT_TEST (waitgroup_done_error_non_last_records_without_completing) {
            // done(err) on a non-last task records the error without
            // completing: it reports false, the last done() reports true,
            // and wait() surfaces the recorded error.
            const std::error_code expected = make_error_code(ParallelScheduler::err_task_exception);
            WaitGroup wg{};
            wg.add(2u);
            PPR_TEST_ASSERT(not wg.done(expected));
            PPR_TEST_ASSERT(wg.done());
            PPR_TEST_ASSERT(wg.wait() == expected);
        };

        PPR_UNIT_TEST (exact_worker_count) {
            for (const std::size_t n: {1u, 2u, 4u}) {
                ParallelScheduler sched{"scheduler-test", n, queueBytes()};
                PPR_TEST_ASSERT(sched.getNumWorkers() == n);
                PPR_TEST_ASSERT(not sched.join());
            }
        };

        PPR_UNIT_TEST (constructor_rejects_invalid_arguments) {
            // Zero workers / zero queue bytes throw; the assertion policy
            // may translate the failure to logic_error, so accept any
            // std::exception. Either way construction never half-succeeds.
            bool caught_workers = false;
            try {
                ParallelScheduler sched{"scheduler-test", 0u, queueBytes()};
            } catch (const std::exception &) {
                caught_workers = true;
            }
            PPR_TEST_ASSERT(caught_workers);

            bool caught_queue = false;
            try {
                ParallelScheduler sched{"scheduler-test", 2u, 0u};
            } catch (const std::exception &) {
                caught_queue = true;
            }
            PPR_TEST_ASSERT(caught_queue);
        };

        PPR_UNIT_TEST (null_parent_uses_background) {
            // An empty parent is normalized to the background context:
            // admission works and error() stays empty.
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes(), SharedContext{}};

            WaitGroup wg{};
            std::atomic<int> ran{0};
            wg.add(1u);
            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                ran.fetch_add(1, std::memory_order_acq_rel);
                wg.done();
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.error());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == 1);
        };

        PPR_UNIT_TEST (undersized_and_unaligned_capacity_supported) {
            // Non-page, non-power-of-two capacities round up to at least a
            // page and stay usable, even when the requested size is one byte.
            {
                ParallelScheduler sched{"scheduler-test", 2u, 1000u};
                PPR_TEST_ASSERT(sched.getNumWorkers() == 2u);

                WaitGroup wg{};
                std::atomic<int> ran{0};
                wg.add(1u);
                PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                    ran.fetch_add(1, std::memory_order_acq_rel);
                    wg.done();
                }));
                PPR_TEST_ASSERT(not sched.join());
                PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == 1);
            }
            {
                ParallelScheduler sched{"scheduler-test", 1u, 1u};
                PPR_TEST_ASSERT(not sched.schedule([](const IContext &) {}));
                PPR_TEST_ASSERT(not sched.join());
                PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            }
        };

        PPR_UNIT_TEST (schedule_single_task_runs) {
            WaitGroup wg{};
            std::atomic<int> ran{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            wg.add(1u);
            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                ran.fetch_add(1, std::memory_order_acq_rel);
                wg.done();
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == 1);
        };

        PPR_UNIT_TEST (schedule_move_only_exactly_once) {
            struct Tracked final {
                std::atomic<int> *p_destroyed = nullptr;

                explicit Tracked(std::atomic<int> *p) noexcept : p_destroyed(p) {
                }

                Tracked(const Tracked &) = delete;
                Tracked &operator=(const Tracked &) = delete;
                Tracked(Tracked &&other) noexcept : p_destroyed(std::exchange(other.p_destroyed, nullptr)) {
                }
                Tracked &operator=(Tracked &&) = delete;

                ~Tracked() noexcept {
                    if (p_destroyed != nullptr) {
                        p_destroyed->fetch_add(1, std::memory_order_acq_rel);
                    }
                }
            };

            // Declared before the scheduler so captures outlive its join.
            WaitGroup wg{};
            std::atomic<int> ran{0};
            std::atomic<int> destroyed{0};
            std::atomic<bool> owned_nonnull{false};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            // Move-only payload: unique ownership transfers into the task.
            // broadcast copies per worker, so move-only is schedule-only.
            auto payload = std::make_unique<Tracked>(std::addressof(destroyed));
            wg.add(1u);
            PPR_TEST_ASSERT(
                not sched.schedule([owned(std::move(payload)), &wg, &ran, &owned_nonnull](const IContext &) mutable {
                    owned_nonnull.store(owned != nullptr, std::memory_order_release);
                    ran.fetch_add(1, std::memory_order_acq_rel);
                    wg.done();
                }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == 1);
            PPR_TEST_ASSERT(owned_nonnull.load(std::memory_order_acquire));
            // join drained every accepted task, and the worker destroys the
            // payload right after invoking it, so the count is exact here
            // with no polling.
            PPR_TEST_ASSERT(destroyed.load(std::memory_order_acquire) == 1);
        };

        // Throwing move-constructors are rejected at compile time: both
        // schedule and broadcast constrain their callable to
        // is_nothrow_move_constructible_v (plus invocable), so a lambda
        // holding a throwing-move capture is not a valid expression.
        struct ThrowingCapture final {
            ThrowingCapture() noexcept = default;
            ThrowingCapture(const ThrowingCapture &) = default;
            ThrowingCapture &operator=(const ThrowingCapture &) = default;
            ThrowingCapture(ThrowingCapture &&) noexcept(false) {
            }
            ThrowingCapture &operator=(ThrowingCapture &&) = default;
            ~ThrowingCapture() noexcept = default;
        };

        using ThrowingScheduleTask = decltype([held = ThrowingCapture{}](const IContext &) {});
        using ThrowingBroadcastTask = decltype([held = ThrowingCapture{}](const IContext &) {});

        static_assert(std::invocable<ThrowingScheduleTask, const IContext &>);
        static_assert(std::invocable<ThrowingBroadcastTask, const IContext &>);
        static_assert(std::is_copy_constructible_v<ThrowingBroadcastTask>);
        static_assert(not std::is_nothrow_move_constructible_v<ThrowingScheduleTask>);
        static_assert(not std::is_nothrow_move_constructible_v<ThrowingBroadcastTask>);
        // NOTE: no compound requires-expression check here on purpose: MSVC
        // reports constraint-failed overload resolution as a hard error
        // (C2672) instead of a substitution failure. The trait asserts above
        // decide the conjunction (invocable holds, nothrow-move fails),
        // which is exactly what rejects both overloads.

        PPR_UNIT_TEST (overaligned_payload_many_ring_positions) {
            // Over-aligned payloads stay aligned and intact across many
            // ring-buffer positions as the queue wraps under concurrent
            // drain. Batches keep admission deterministic (never full).
            struct alignas(64) Overaligned final {
                int id = -1;
                unsigned pattern[15] = {};
            };

            struct Observation {
                std::uintptr_t addr = 0u;
                int id = -1;
            };

            constexpr std::size_t kRounds = 16u;
            constexpr std::size_t kBatch = 8u;
            constexpr std::size_t kTotal = kRounds * kBatch;
            std::array<Observation, kTotal> observations{};
            WaitGroup wg{};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            for (std::size_t round = 0u; round < kRounds; ++round) {
                wg.add(kBatch);
                for (std::size_t k = 0u; k < kBatch; ++k) {
                    const std::size_t i = round * kBatch + k;
                    Overaligned payload{};
                    payload.id = static_cast<int>(i);
                    for (unsigned &w: payload.pattern) {
                        w = static_cast<unsigned>(i * 2654435761u);
                    }
                    PPR_TEST_ASSERT(not sched.schedule(
                        [owned(std::move(payload)), i, &observations, &wg](const IContext &) mutable {
                            Observation obs{};
                            obs.addr = reinterpret_cast<std::uintptr_t>(std::addressof(owned));
                            obs.id = owned.id;
                            bool intact = obs.id == static_cast<int>(i);
                            for (const unsigned w: owned.pattern) {
                                intact = intact and w == static_cast<unsigned>(i * 2654435761u);
                            }
                            obs.id = intact ? obs.id : -2;
                            observations[i] = obs;
                            wg.done();
                        }));
                }
                PPR_TEST_ASSERT(not wg.wait());
            }
            PPR_TEST_ASSERT(not sched.join());
            for (std::size_t i = 0u; i < kTotal; ++i) {
                PPR_TEST_ASSERT(observations[i].addr % alignof(Overaligned) == 0u);
                PPR_TEST_ASSERT(observations[i].id == static_cast<int>(i));
            }
        };

        PPR_UNIT_TEST (broadcast_runs_on_all_workers) {
            constexpr std::size_t num_workers = 3u;
            WaitGroup wg{};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", num_workers, queueBytes()};

            wg.add(num_workers);
            PPR_TEST_ASSERT(not sched.broadcast([&](const IContext &) {
                ran.fetch_add(1u, std::memory_order_acq_rel);
                wg.done();
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_workers);
        };

        PPR_UNIT_TEST (broadcast_mutable_lambda_once_per_worker) {
            // broadcast requires a copyable callable; a mutable lambda
            // compiles and each worker runs its own copy exactly once.
            constexpr std::size_t num_workers = 3u;
            WaitGroup wg{};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", num_workers, queueBytes()};

            wg.add(num_workers);
            int per_copy = 0;
            PPR_TEST_ASSERT(not sched.broadcast([count = per_copy, &ran, &wg](const IContext &) mutable {
                ++count;
                ran.fetch_add(1u, std::memory_order_acq_rel);
                wg.done();
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_workers);
        };

        PPR_UNIT_TEST (tracked_schedule_runs_and_settles_group) {
            // Tracked schedule: the caller passes the group without a manual
            // add; admission registers exactly once and the worker envelope
            // settles it with an empty error.
            WaitGroup wg{};
            std::atomic<int> ran{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            PPR_TEST_ASSERT(
                not sched.schedule([&](const IContext &) { ran.fetch_add(1, std::memory_order_acq_rel); }, wg));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == 1);
        };

        PPR_UNIT_TEST (tracked_broadcast_settles_group_on_all_workers) {
            // Tracked broadcast registers once per admitted worker; the
            // group settles with an empty error after every copy ran.
            constexpr std::size_t num_workers = 3u;
            WaitGroup wg{};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", num_workers, queueBytes()};

            PPR_TEST_ASSERT(
                not sched.broadcast([&](const IContext &) { ran.fetch_add(1u, std::memory_order_acq_rel); }, wg));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_workers);
        };

        PPR_UNIT_TEST (tracked_schedule_admission_failure_leaves_group_untouched) {
            // Admission failure touches neither the group counter nor its
            // error slot: the queue is proven saturated with untracked fills
            // while the worker is pinned, so the tracked attempt fails and
            // the never-registered group still waits cleanly. The scope
            // guard releases the worker even if an assert throws.
            std::latch worker_pinned{1};
            std::atomic<bool> release{false};
            WaitGroup wg{};
            ParallelScheduler sched{"scheduler-test", 1u, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                worker_pinned.count_down();
                awaitGate(release);
            }));
            worker_pinned.wait();

            std::error_code saturate_err{};
            for (int i = 0; i < kMaxFillAttempts; ++i) {
                if (const std::error_code err = sched.schedule([](const IContext &) {})) {
                    saturate_err = err;
                    break;
                }
            }
            PPR_TEST_ASSERT(saturate_err == RawChannel::error_full);

            PPR_TEST_ASSERT(sched.schedule([](const IContext &) {}, wg) == RawChannel::error_full);
            PPR_TEST_ASSERT(not wg.wait());

            openGate(release);
            PPR_TEST_ASSERT(not sched.join());
        };

        PPR_UNIT_TEST (tracked_schedule_helper_throw_surfaces_task_exception) {
            // A throwing helper settles its tracked group with
            // err_task_exception; the scheduler records the same failure
            // and join reports it after draining.
            const std::error_code expected = make_error_code(ParallelScheduler::err_task_exception);
            WaitGroup wg{};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            PPR_TEST_ASSERT(
                not sched.schedule([](const IContext &) { throw std::runtime_error("tracked helper failure"); }, wg));
            PPR_TEST_ASSERT(wg.wait() == expected);
            PPR_TEST_ASSERT(sched.error() == expected);
            PPR_TEST_ASSERT(sched.join() == expected);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (idle_join_and_repeated_join) {
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            // Second join must be benign (threads already joined).
            PPR_TEST_ASSERT(not sched.join());
        };

        PPR_UNIT_TEST (join_drains_accepted_tasks) {
            // join is invoked BEFORE any external wait: draining is join's
            // job. The opener thread releases the gate once both workers are
            // executing; the scope guard releases it even if an assert
            // throws, so workers can never strand the suite.
            constexpr std::size_t num_tasks = 16u;
            std::latch entered{2};
            std::atomic<bool> release{false};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            for (std::size_t i = 0u; i < num_tasks; ++i) {
                PPR_TEST_ASSERT(not sched.schedule([&, i](const IContext &) {
                    if (i < 2u) {
                        entered.count_down();
                        awaitGate(release);
                    }
                    ran.fetch_add(1u, std::memory_order_acq_rel);
                }));
            }

            std::jthread opener([&] {
                entered.wait();
                openGate(release);
            });

            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_tasks);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (schedule_racing_join) {
            // Producers race join: every attempt either admits (then the
            // task must run to completion via the drain) or fails with
            // error_closed. Producer threads never assert (an exception
            // there would terminate); all checks happen on the parent.
            constexpr int kProducers = 2;
            std::atomic<int> admitted{0};
            std::atomic<int> ran{0};
            std::atomic<bool> all_rejections_closed{true};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            std::vector<std::jthread> producers;
            producers.reserve(static_cast<std::size_t>(kProducers));
            for (int p = 0; p < kProducers; ++p) {
                producers.emplace_back([&] {
                    for (int i = 0; i < 64; ++i) {
                        const std::error_code err = sched.schedule(
                            [&](const IContext &) { ran.fetch_add(1, std::memory_order_acq_rel); });
                        if (not err) {
                            admitted.fetch_add(1, std::memory_order_acq_rel);
                        } else if (err != RawChannel::error_closed) {
                            all_rejections_closed.store(false, std::memory_order_release);
                        }
                    }
                });
            }

            PPR_TEST_ASSERT(not sched.join());
            for (auto &producer: producers) {
                producer.join();
            }

            PPR_TEST_ASSERT(all_rejections_closed.load(std::memory_order_acquire));
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == admitted.load(std::memory_order_acquire));
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            // Post-join admission is rejected.
            PPR_TEST_ASSERT(bool(sched.schedule([](const IContext &) {})));
        };

        PPR_UNIT_TEST (worker_originated_join_rejected) {
            // A worker must never join its own pool: join from a worker
            // reports resource_deadlock_would_occur without hanging and
            // without transitioning pool state, so the subsequent parent
            // join still succeeds and drains.
            WaitGroup wg{};
            std::atomic<bool> worker_saw_deadlock{false};
            ParallelScheduler sched{"scheduler-test", 1u, queueBytes()};

            wg.add(1u);
            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                PPR_DEFER {
                    wg.done();
                };
                const std::error_code err = sched.join();
                worker_saw_deadlock.store(err == std::errc::resource_deadlock_would_occur, std::memory_order_release);
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(worker_saw_deadlock.load(std::memory_order_acquire));
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (outstanding_count_while_executing) {
            constexpr std::size_t num_workers = 2u;
            constexpr std::size_t num_tasks = 2u;
            std::latch arrived{num_tasks};
            std::atomic<bool> release{false};
            WaitGroup wg{};
            ParallelScheduler sched{"scheduler-test", num_workers, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            wg.add(num_tasks);
            for (std::size_t i = 0u; i < num_tasks; ++i) {
                PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                    arrived.count_down();
                    awaitGate(release);
                    wg.done();
                }));
            }

            // Both tasks are admitted and executing; none can complete yet.
            arrived.wait();
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == num_tasks);

            openGate(release);
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (cancel_parent_drains_accepted_rejects_later) {
            // Parent cancellation still drains accepted tasks; admission
            // after cancellation is rejected. join reports the inherited
            // operation_canceled cause.
            constexpr int num_tasks = 32;
            std::atomic<int> ran{0};
            auto [parent, cancel] = context::withCancel(context::background());
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes(), parent};

            for (int i = 0; i < num_tasks; ++i) {
                PPR_TEST_ASSERT(
                    not sched.schedule([&](const IContext &) { ran.fetch_add(1, std::memory_order_acq_rel); }));
            }

            cancel();
            PPR_TEST_ASSERT(parent->pollEvent());

            const std::error_code join_err = sched.join();
            PPR_TEST_ASSERT(join_err == std::errc::operation_canceled);
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_tasks);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            PPR_TEST_ASSERT(bool(sched.schedule([](const IContext &) {})));
        };

        PPR_UNIT_TEST (join_with_cause_cancels_and_drains) {
            // A nonzero join cause cancels first, yet accepted tasks still
            // drain; the cause is what join reports.
            constexpr int num_tasks = 32;
            std::atomic<int> ran{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            for (int i = 0; i < num_tasks; ++i) {
                PPR_TEST_ASSERT(
                    not sched.schedule([&](const IContext &) { ran.fetch_add(1, std::memory_order_acq_rel); }));
            }

            PPR_TEST_ASSERT(bool(sched.join(std::make_error_code(std::errc::operation_canceled))));
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == num_tasks);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
            PPR_TEST_ASSERT(bool(sched.schedule([](const IContext &) {})));
        };

        PPR_UNIT_TEST (schedule_saturated_returns_full) {
            // One worker pinned in a gated task, then fill its queue until
            // admission is rejected nonblockingly with error_full. The scope
            // guard opens the gate even if an assert throws.
            std::latch worker_pinned{1};
            std::atomic<bool> release{false};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", 1u, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                worker_pinned.count_down();
                awaitGate(release);
                ran.fetch_add(1u, std::memory_order_acq_rel);
            }));
            worker_pinned.wait();

            std::size_t admitted = 0u;
            std::error_code saturate_err{};
            for (int i = 0; i < kMaxFillAttempts; ++i) {
                const std::error_code err = sched.schedule(
                    [&](const IContext &) { ran.fetch_add(1u, std::memory_order_acq_rel); });
                if (err) {
                    saturate_err = err;
                    break;
                }
                ++admitted;
            }

            PPR_TEST_ASSERT(admitted > 0u);
            PPR_TEST_ASSERT(saturate_err == RawChannel::error_full);

            openGate(release);
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == admitted + 1u);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (join_drains_full_channel) {
            // Saturated-close regression: join starts while the channel is
            // still full of queued work and drains every accepted task. The
            // opener releases the pinned worker once join is underway; the
            // scope guard covers any throwing assert before that.
            std::latch worker_pinned{1};
            std::atomic<bool> release{false};
            std::atomic<bool> join_started{false};
            std::atomic<std::size_t> ran{0u};
            ParallelScheduler sched{"scheduler-test", 1u, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                worker_pinned.count_down();
                awaitGate(release);
                ran.fetch_add(1u, std::memory_order_acq_rel);
            }));
            worker_pinned.wait();

            std::size_t admitted = 0u;
            std::error_code saturate_err{};
            for (int i = 0; i < kMaxFillAttempts; ++i) {
                const std::error_code err = sched.schedule(
                    [&](const IContext &) { ran.fetch_add(1u, std::memory_order_acq_rel); });
                if (err) {
                    saturate_err = err;
                    break;
                }
                ++admitted;
            }
            PPR_TEST_ASSERT(admitted > 0u);
            PPR_TEST_ASSERT(saturate_err == RawChannel::error_full);

            std::jthread opener([&] {
                join_started.wait(false, std::memory_order_acquire);
                openGate(release);
            });
            join_started.store(true, std::memory_order_release);
            join_started.notify_all();

            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(ran.load(std::memory_order_acquire) == admitted + 1u);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (burst_admission_with_retry_on_full) {
            // Admission is nonblocking, so a burst may meet a momentarily
            // full queue: retry only on error_full, blocking on observed
            // drain progress (atomic wait/notify, never a yield loop).
            // wg.add precedes each attempt so the group stays balanced even
            // if the retry budget is exhausted and the test fails cleanly.
            constexpr int num_tasks = 256;
            WaitGroup wg{};
            std::atomic<int> completed{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            bool exhausted = false;
            int admitted = 0;
            for (int i = 0; i < num_tasks and not exhausted; ++i) {
                int last_completed = completed.load(std::memory_order_acquire);
                for (int attempt = 0;; ++attempt) {
                    wg.add(1u);
                    const std::error_code err = sched.schedule([&](const IContext &) {
                        const int prior = completed.fetch_add(1, std::memory_order_acq_rel);
                        static_cast<void>(prior);
                        completed.notify_all();
                        wg.done();
                    });
                    if (not err) {
                        ++admitted;
                        break;
                    }
                    wg.done();
                    if (err != RawChannel::error_full or attempt >= kMaxFillAttempts) {
                        exhausted = true;
                        break;
                    }
                    completed.wait(last_completed, std::memory_order_acquire);
                    last_completed = completed.load(std::memory_order_acquire);
                }
            }
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not exhausted);
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(admitted == num_tasks);
            PPR_TEST_ASSERT(completed.load(std::memory_order_acquire) == num_tasks);
        };

        PPR_UNIT_TEST (forrange_empty_invokes_nothing) {
            std::atomic<int> calls{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            PPR_TEST_ASSERT(not parallel::forRange(
                sched, 0u, 0u, [&](std::size_t) { calls.fetch_add(1, std::memory_order_acq_rel); }));
            PPR_TEST_ASSERT(not parallel::forRange(
                sched, 5u, 5u, [&](std::size_t) { calls.fetch_add(1, std::memory_order_acq_rel); }));
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(calls.load(std::memory_order_acquire) == 0);
        };

        PPR_UNIT_TEST (forrange_invalid_range_rejected) {
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            PPR_TEST_ASSERT(parallel::forRange(sched, 5u, 3u, [&](std::size_t) {}) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not sched.join());
        };

        PPR_UNIT_TEST (forrange_uneven_covers_all_exactly_once) {
            // 10 items over 3 workers: uneven split visits each index once.
            constexpr std::size_t n = 10u;
            std::array<std::atomic<int>, n> visits{};
            ParallelScheduler sched{"scheduler-test", 3u, queueBytes()};

            PPR_TEST_ASSERT(not parallel::forRange(
                sched, 0u, n, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_acq_rel); }));
            PPR_TEST_ASSERT(not sched.join());
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 1);
            }
        };

        PPR_UNIT_TEST (forrange_nested_same_scheduler) {
            // A worker thread nesting forRange on its own scheduler must
            // complete with exact-once coverage (serial/inline fallback, never
            // a self-deadlock). The worker records its outcome; only the
            // parent asserts.
            constexpr std::size_t n = 32u;
            WaitGroup wg{};
            std::array<std::atomic<int>, n> visits{};
            std::atomic<bool> nested_ok{false};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            wg.add(1u);
            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                PPR_DEFER {
                    wg.done();
                };
                const std::error_code err = parallel::forRange(
                    sched, 0u, n, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_acq_rel); });
                nested_ok.store(not err, std::memory_order_release);
            }));
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(nested_ok.load(std::memory_order_acquire));
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 1);
            }
        };

        PPR_UNIT_TEST (forrange_cancelled_reports_cancelled) {
            // With a cancelled parent, forRange checks cancellation BEFORE
            // work: it surfaces operation_canceled and no index executes.
            constexpr std::size_t n = 16u;
            std::array<std::atomic<int>, n> visits{};
            auto [parent, cancel] = context::withCancel(context::background());
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes(), parent};

            cancel();
            const std::error_code err = parallel::forRange(
                sched, 0u, n, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_acq_rel); });
            PPR_TEST_ASSERT(err == std::errc::operation_canceled);
            PPR_TEST_ASSERT(sched.join() == std::errc::operation_canceled);
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 0);
            }
        };

        PPR_UNIT_TEST (forrange_nested_cancelled_reports_cancelled) {
            // Nested helpers check cancellation BEFORE work. The worker is
            // gated until after cancellation, so its nested range runs
            // nothing and reports operation_canceled deterministically.
            constexpr std::size_t n = 32u;
            WaitGroup wg{};
            std::array<std::atomic<int>, n> visits{};
            std::atomic<bool> nested_cancelled{false};
            std::latch worker_arrived{1};
            std::atomic<bool> release{false};
            auto [parent, cancel] = context::withCancel(context::background());
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes(), parent};
            PPR_DEFER {
                openGate(release);
            };

            wg.add(1u);
            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                PPR_DEFER {
                    wg.done();
                };
                worker_arrived.count_down();
                awaitGate(release);
                const std::error_code err = parallel::forRange(
                    sched, 0u, n, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_acq_rel); });
                nested_cancelled.store(err == std::errc::operation_canceled, std::memory_order_release);
            }));
            worker_arrived.wait();
            cancel();
            openGate(release);
            PPR_TEST_ASSERT(not wg.wait());
            PPR_TEST_ASSERT(sched.join() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(nested_cancelled.load(std::memory_order_acquire));
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 0);
            }
        };

        PPR_UNIT_TEST (forrange_full_queue_completes_via_caller) {
            // The worker is pinned and the queue is proven saturated, so no
            // feeder can be admitted: the caller lane still completes every
            // index, and forRange reports success once all work finished.
            constexpr std::size_t n = 8u;
            std::array<std::atomic<int>, n> visits{};
            std::latch worker_pinned{1};
            std::atomic<bool> release{false};
            ParallelScheduler sched{"scheduler-test", 1u, queueBytes()};
            PPR_DEFER {
                openGate(release);
            };

            PPR_TEST_ASSERT(not sched.schedule([&](const IContext &) {
                worker_pinned.count_down();
                awaitGate(release);
            }));
            worker_pinned.wait();

            for (int i = 0; i < kMaxFillAttempts; ++i) {
                if (const std::error_code err = sched.schedule([](const IContext &) {})) {
                    break;
                }
            }

            const std::error_code err = parallel::forRange(
                sched, 0u, n, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_acq_rel); });
            PPR_TEST_ASSERT(not err);

            openGate(release);
            PPR_TEST_ASSERT(not sched.join());
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 1);
            }
        };

        PPR_UNIT_TEST (forrange_nested_across_schedulers) {
            // Cross-pool nesting: deadlock-free by construction (a worker of
            // the outer pool blocks on the inner pool, never on itself).
            // Outcomes are recorded; only the parent asserts.
            constexpr std::size_t n = 16u;
            std::array<std::atomic<int>, n> visits{};
            std::atomic<bool> nested_ok{true};
            ParallelScheduler outer{"scheduler-outer", 4u, queueBytes()};
            ParallelScheduler inner{"scheduler-inner", 4u, queueBytes()};

            PPR_TEST_ASSERT(not parallel::forRange(outer, 0u, n, [&](std::size_t i) {
                const std::error_code err = parallel::forRange(
                    inner, 0u, 2u, [&](std::size_t) { visits[i].fetch_add(1, std::memory_order_acq_rel); });
                if (err) {
                    nested_ok.store(false, std::memory_order_release);
                }
            }));
            PPR_TEST_ASSERT(not outer.join());
            PPR_TEST_ASSERT(not inner.join());
            PPR_TEST_ASSERT(nested_ok.load(std::memory_order_acquire));
            for (const auto &v: visits) {
                PPR_TEST_ASSERT(v.load(std::memory_order_acquire) == 2);
            }
        };

        PPR_UNIT_TEST (forrange_caller_error_over_helper_error) {
            // Composition order is caller lane, then group, then scheduler
            // (first failure wins). With 2 workers and 6 even items the
            // chunking is exact: helpers cover [0,4), the caller drains
            // [4,6). Distinct failure codes prove the caller lane wins while
            // the helper failure is still recorded on the scheduler.
            const std::error_code caller_err = std::make_error_code(std::errc::invalid_argument);
            const std::error_code helper_err = make_error_code(ParallelScheduler::err_task_exception);
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            const std::error_code err = parallel::forRange(sched, 0u, 6u, [&](std::size_t i) {
                if (i < 4u) {
                    throw std::runtime_error("helper chunk failure");
                }
                throw std::system_error(caller_err);
            });
            PPR_TEST_ASSERT(err == caller_err);
            PPR_TEST_ASSERT(sched.join() == helper_err);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };

        PPR_UNIT_TEST (forrange_success_returns_empty) {
            std::atomic<int> calls{0};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            PPR_TEST_ASSERT(not parallel::forRange(
                sched, 0u, 16u, [&](std::size_t) { calls.fetch_add(1, std::memory_order_acq_rel); }));
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(calls.load(std::memory_order_acquire) == 16);
        };

        PPR_UNIT_TEST (foreach_borrowed_mutable_span) {
            std::vector<int> values{1, 2, 3, 4, 5, 6, 7};
            const std::vector<int> expected{2, 4, 6, 8, 10, 12, 14};
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes()};

            // Borrowed mutable view: forEach mutates in place, not on a copy.
            PPR_TEST_ASSERT(not parallel::forEach(sched, std::span<int>(values), [](int &x) { x *= 2; }));
            PPR_TEST_ASSERT(not sched.join());
            PPR_TEST_ASSERT(values == expected);
        };

        PPR_UNIT_TEST (empty_clause_cancellation_normalized) {
            // An empty cancellation clause still trips the context event;
            // the scheduler normalizes it to operation_canceled on both
            // admission and join.
            auto [parent, cancel] = context::withCancelClause(context::background());
            ParallelScheduler sched{"scheduler-test", 2u, queueBytes(), parent};

            cancel(std::error_code{});
            PPR_TEST_ASSERT(parent->pollEvent());

            PPR_TEST_ASSERT(sched.schedule([](const IContext &) {}) == std::errc::operation_canceled);
            PPR_TEST_ASSERT(sched.join() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(sched.getNumOutstandingTasks() == 0u);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest scheduler = UnitTest::Named("scheduler") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Scheduler::waitgroup_empty_wait_returns,
            detail::Scheduler::waitgroup_single_add_done,
            detail::Scheduler::waitgroup_multiple_outstanding,
            detail::Scheduler::waitgroup_concurrent_done,
            detail::Scheduler::waitgroup_reuse_after_wait,
            detail::Scheduler::waitgroup_done_error_non_last_records_without_completing,
            detail::Scheduler::exact_worker_count,
            detail::Scheduler::constructor_rejects_invalid_arguments,
            detail::Scheduler::null_parent_uses_background,
            detail::Scheduler::undersized_and_unaligned_capacity_supported,
            detail::Scheduler::schedule_single_task_runs,
            detail::Scheduler::schedule_move_only_exactly_once,
            detail::Scheduler::overaligned_payload_many_ring_positions,
            detail::Scheduler::broadcast_runs_on_all_workers,
            detail::Scheduler::broadcast_mutable_lambda_once_per_worker,
            detail::Scheduler::tracked_schedule_runs_and_settles_group,
            detail::Scheduler::tracked_broadcast_settles_group_on_all_workers,
            detail::Scheduler::tracked_schedule_admission_failure_leaves_group_untouched,
            detail::Scheduler::tracked_schedule_helper_throw_surfaces_task_exception,
            detail::Scheduler::idle_join_and_repeated_join,
            detail::Scheduler::join_drains_accepted_tasks,
            detail::Scheduler::schedule_racing_join,
            detail::Scheduler::worker_originated_join_rejected,
            detail::Scheduler::outstanding_count_while_executing,
            detail::Scheduler::cancel_parent_drains_accepted_rejects_later,
            detail::Scheduler::join_with_cause_cancels_and_drains,
            detail::Scheduler::schedule_saturated_returns_full,
            detail::Scheduler::join_drains_full_channel,
            detail::Scheduler::burst_admission_with_retry_on_full,
            detail::Scheduler::forrange_empty_invokes_nothing,
            detail::Scheduler::forrange_invalid_range_rejected,
            detail::Scheduler::forrange_uneven_covers_all_exactly_once,
            detail::Scheduler::forrange_nested_same_scheduler,
            detail::Scheduler::forrange_cancelled_reports_cancelled,
            detail::Scheduler::forrange_nested_cancelled_reports_cancelled,
            detail::Scheduler::forrange_full_queue_completes_via_caller,
            detail::Scheduler::forrange_nested_across_schedulers,
            detail::Scheduler::forrange_caller_error_over_helper_error,
            detail::Scheduler::forrange_success_returns_empty,
            detail::Scheduler::foreach_borrowed_mutable_span,
            detail::Scheduler::empty_clause_cancellation_normalized,
        });
    };

    const UnitTest &schedulerTests() noexcept {
        return scheduler;
    }
} // namespace pP::tests
