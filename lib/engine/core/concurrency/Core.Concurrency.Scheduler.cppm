module;
#include "pP/Macros.h"

#include <iso646.h>
export module engine.core:concurrency.scheduler;

import :assert;
import :concurrency.channel;
import :concurrency.context;
import :concurrency.event;
import :concurrency.thread;
import :containers.stl;
import :function.ref;
import :hal;
import :logger;

import std;

export namespace pP {
    PPR_DECLARE_LOG_CATEGORY(Scheduler)

    // ------------------------------------------------------------------
    // multi-tasking scheduler for fixed-size pool of parallel workers
    // ------------------------------------------------------------------

    class ParallelScheduler final {
        template<typename TaskF>
        using task_result_t = std::invoke_result_t<std::decay_t<TaskF> &, const IContext &>;

        template<typename TaskF>
        using schedule_result_t = std::conditional_t<
            std::is_void_v<task_result_t<TaskF>>, std::error_code,
            std::expected<std::future<task_result_t<TaskF>>, std::error_code>>;

        template<typename TaskF>
        using unexpected_result_t =
            std::conditional_t<std::is_void_v<task_result_t<TaskF>>, std::error_code, std::unexpected<std::error_code>>;

    public:
        using TaskEntryPoint = std23::function_ref<void(const IContext &)>;

        enum errc : int {
            err_scheduler_joined = 1,
            err_task_exception,
        };

        using EProducerPolicy = RawChannel::EBackPressure;
        using enum EProducerPolicy;

        friend std::error_code make_error_code(errc err) noexcept;

        ParallelScheduler(
            string_literal description, std::size_t num_workers, std::size_t queue_bytes_per_worker,
            SharedContext parent = context::background(), hal::EPriority thread_priority = hal::EPriority::normal);

        ParallelScheduler(const ParallelScheduler &) = delete;
        ParallelScheduler &operator=(const ParallelScheduler &) = delete;

        // Deleted: WorkerState_ holds a non-owning owner pointer into this object.
        ParallelScheduler(ParallelScheduler &&) noexcept = delete;
        ParallelScheduler &operator=(ParallelScheduler &&) = delete;

        ~ParallelScheduler();

        [[nodiscard]] const IContext &getContext() const noexcept {
            return *m_lifecycle;
        }

        [[nodiscard]] std::size_t getNumWorkers() const noexcept {
            return m_workers.size();
        }

        [[nodiscard]] std::size_t getNumOutstandingTasks() const noexcept;

        // Explicit thread-local worker identity: true when called on a pool worker thread.
        // Nested parallel helpers use this for their serial synchronous fallback.
        [[nodiscard]] bool isWorkerThread() const noexcept;

        [[nodiscard]] std::error_code error() const noexcept {
            return m_lifecycle ? m_lifecycle->error() : default_value_v;
        }

        template<std::invocable<const IContext &> TaskF>
            requires std::is_copy_constructible_v<TaskF> and
                     std::is_nothrow_move_constructible_v<TaskF> and
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                     // ReSharper disable once CppRedundantQualifier
                     std::is_void_v<ParallelScheduler::task_result_t<TaskF>>
        [[nodiscard]] std::error_code broadcast(
            const TaskF &work, const std::optional<WaitGroup> &wg = std::nullopt,
            EProducerPolicy policy = wait_if_full);

        template<std::invocable<const IContext &> TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and std::is_nothrow_destructible_v<std::decay_t<TaskF>>
        [[nodiscard]] schedule_result_t<TaskF> schedule(
            TaskF &&work, std::optional<WaitGroup> wg = std::nullopt, EProducerPolicy policy = wait_if_full);

        template<std::invocable<const IContext &> TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                     // ReSharper disable once CppRedundantQualifier
                     (not std::is_void_v<ParallelScheduler::task_result_t<TaskF>>)
        [[nodiscard]] std::error_code schedule(
            TaskF &&work, std::future<task_result_t<TaskF>> *out_future_ref, std::optional<WaitGroup> wg = std::nullopt,
            EProducerPolicy policy = wait_if_full);

        [[nodiscard]] std::error_code join(std::error_code cause = {});

    private:
        /// Generic entry-point for every task used inside workerLoop
        struct TaskEnvelope_ { // NOLINT(*-pro-type-member-init)
            TaskEntryPoint m_run;
            std::optional<WaitGroup> m_wait_group;
        };

        /// Task adapter used a non-void value must be return through a std::future<>
        template<typename TaskF>
        struct TaskPromise_ {
            using value_type = task_result_t<TaskF>;

            std::promise<value_type> m_promise;
            std::decay_t<TaskF> m_work;

            explicit TaskPromise_(TaskF &&work) : m_work(std::forward<TaskF>(work)) {
            }

            [[nodiscard]] explicit operator std::expected<std::future<value_type>, std::error_code>() noexcept(
                std::declval<std::promise<value_type> &>().get_future()) {
                return m_promise.get_future();
            }

            void operator()(const IContext &context) {
                try {
                    if constexpr (std::is_void_v<value_type>) {
                        std::invoke(m_work, context);
                        m_promise.set_value();
                    } else {
                        m_promise.set_value(std::invoke(m_work, context));
                    }

                } catch (...) {
                    m_promise.set_exception(std::current_exception());
                    throw; // preserve safety-net + WaitGroup::done(err)
                }
            }
        };

        /// Internal state of each worker with task queue and std::thread
        struct alignas(hal::cacheline_size_v) WorkerState_ final { // NOLINT(*-pro-type-member-init)
            RawChannel m_tasks_queued;

            SharedContext m_lifecycle;
            std::atomic<u32> m_num_outstanding{0u};
            const u32 m_index;

            const ParallelScheduler *m_owner;
            // Keep close notifications valid after workerLoop exits; join precedes signal/source destruction.
            Signal<IContext, RawChannel> m_signal;
            std::jthread m_thread;

            WorkerState_(const ParallelScheduler *owner, std::size_t index, std::size_t capacity_bytes);

            void workerLoop() noexcept;
        };

        /// Set by every owner worker thread, allows to detect nested dispatch
        static thread_local const WorkerState_ *g_worker_state_tls_;

        [[nodiscard]] std::error_code shouldCancel_(const std::optional<WaitGroup> &wg) const noexcept;

        [[nodiscard]] std::pair<std::size_t, std::size_t> selectNextWorkers_() const noexcept;

        template<typename TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                     std::invocable<std::decay_t<TaskF> &, const IContext &>
        [[nodiscard]] std::error_code dispatchWork_(
            std::pair<std::size_t, std::size_t> worker_indices, TaskF &&work, std::optional<WaitGroup> &&wg,
            EProducerPolicy policy);

        template<typename TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                     std::invocable<std::decay_t<TaskF> &, const IContext &>
        [[nodiscard]] std::error_code dispatchWork_(
            WorkerState_ &worker, TaskF &&work, std::optional<WaitGroup> &&wg, EProducerPolicy policy);

        [[nodiscard]] std::error_code dispatchWork_(
            WorkerState_ &worker, const TaskEntryPoint &work, std::optional<WaitGroup> &&wg, EProducerPolicy policy);

        Array<std::unique_ptr<WorkerState_>> m_workers;

        std::shared_mutex m_producer_mutex{};
        bool m_has_joined{false}; // Guarded by m_producer_mutex; set by join() under the exclusive lock.

        context::CancelClauseFunc m_request_stop;
        SharedContext m_lifecycle;
        const string_literal m_description;
        const hal::EPriority m_thread_priority;
    };

    // ------------------------------------------------------------------
    // parallel task helpers
    // ------------------------------------------------------------------

    namespace parallel {
        [[nodiscard]] ParallelScheduler &foregroundPool();
        [[nodiscard]] ParallelScheduler &backgroundPool();

        [[nodiscard]] std::error_code forRange(
            std::size_t first, std::size_t last, std23::function_ref<void(std::size_t index)> closure,
            ParallelScheduler &scheduler = foregroundPool(), std::size_t min_job_size = 1u);

        template<
            std::ranges::random_access_range ElementsT, std::invocable<std::ranges::range_reference_t<ElementsT>> EachF>
            requires std::ranges::sized_range<ElementsT>
        [[nodiscard]] std::error_code forEach(
            ElementsT &&elements, EachF &&each, ParallelScheduler &scheduler = foregroundPool(),
            std::size_t min_job_size = 1u) {
            return forRange(
                0u, std::ranges::size(elements), [&](const std::size_t index) { each(elements[index]); }, scheduler,
                min_job_size);
        }
    }

    // ------------------------------------------------------------------
    // template definitions
    // ------------------------------------------------------------------

    template<std::invocable<const IContext &> TaskF>
        requires std::is_copy_constructible_v<TaskF> and
                 std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                 std::is_void_v<ParallelScheduler::task_result_t<TaskF>>
    std::error_code ParallelScheduler::broadcast(
        const TaskF &work, const std::optional<WaitGroup> &wg, const EProducerPolicy policy) {
        const std::shared_lock join_guard{m_producer_mutex};

        if (m_has_joined) [[unlikely]] {
            return make_error_code(RawChannel::error_closed);
        }
        if (const std::error_code err = shouldCancel_(wg)) [[unlikely]] {
            return err;
        }

        // Independent copies per worker; partial successes are kept, first error reported.
        std::error_code first_err{};
        for (const std::unique_ptr<WorkerState_> &worker: m_workers) {
            if (const std::error_code err = dispatchWork_(*worker, work, std::optional{wg}, policy)) [[unlikely]] {
                first_err = first_err ? first_err : err;
            }
        }
        return first_err;
    }

    template<std::invocable<const IContext &> TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                 (not std::is_void_v<ParallelScheduler::task_result_t<TaskF>>)
    std::error_code ParallelScheduler::schedule(
        TaskF &&work, std::future<task_result_t<TaskF>> *out_future_ref, std::optional<WaitGroup> wg,
        const EProducerPolicy policy) {
        std::expected<std::future<task_result_t<TaskF>>, std::error_code> expected = schedule(
            std::forward<TaskF>(work), std::move(wg), policy);

        if (expected.has_value()) [[likely]] {
            *out_future_ref = std::move(*expected);
            return default_value_v;
        }
        return expected.error();
    }

    template<std::invocable<const IContext &> TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and std::is_nothrow_destructible_v<std::decay_t<TaskF>>
    auto ParallelScheduler::schedule(TaskF &&work, std::optional<WaitGroup> wg, const EProducerPolicy policy)
        -> schedule_result_t<TaskF> {
        const std::shared_lock join_guard{m_producer_mutex};
        using unexpected_t = unexpected_result_t<TaskF>;

        if (m_has_joined) [[unlikely]] {
            return unexpected_t{make_error_code(RawChannel::error_closed)};
        }
        if (const std::error_code err = shouldCancel_(wg)) [[unlikely]] {
            return unexpected_t{err};
        }

        // The callable is only moved after a reservation succeeds, so a failed
        // first choice leaves it intact for the fallback worker.
        const auto worker_indices = selectNextWorkers_();

        // Use an additional TaskPromise_<> adapter if task return type is not void
        if constexpr (std::is_void_v<task_result_t<TaskF>>) { // NOLINT(*-branch-clone)
            return dispatchWork_(worker_indices, std::forward<TaskF>(work), std::move(wg), policy);
        } else {
            TaskPromise_ promise{std::forward<TaskF>(work)};
            std::future future = promise.m_promise.get_future();
            if (const std::error_code err = dispatchWork_(worker_indices, std::move(promise), std::move(wg), policy))
                [[unlikely]] {
                return unexpected_t{err};
            }
            return future;
        }
    }

    template<typename TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                 std::invocable<std::decay_t<TaskF> &, const IContext &>
    std::error_code ParallelScheduler::dispatchWork_(
        const std::pair<std::size_t, std::size_t> worker_indices, TaskF &&work, std::optional<WaitGroup> &&wg,
        const EProducerPolicy policy) {
        std::error_code err{
            dispatchWork_(*m_workers[worker_indices.first], std::forward<TaskF>(work), std::move(wg), policy)};

        if (err == make_error_code(RawChannel::error_full)) {
            err = dispatchWork_(*m_workers[worker_indices.second], std::forward<TaskF>(work), std::move(wg), policy);
        }
        return err;
    }

    template<typename TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                 std::invocable<std::decay_t<TaskF> &, const IContext &>
    std::error_code ParallelScheduler::dispatchWork_(
        WorkerState_ &worker, TaskF &&work, std::optional<WaitGroup> &&wg, const EProducerPolicy policy) {
        using decayed_t = std::decay_t<TaskF>;

        // Register the intent asap to signal the worker loop than more work is coming
        worker.m_num_outstanding.fetch_add(1u, std::memory_order_acq_rel);

        // Parent cancellation rejects admission; queued work still drains elsewhere.
        if (const std::error_code err = shouldCancel_(wg)) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return err;
        }

        constexpr std::size_t kEnvelopeSize = sizeof(TaskEnvelope_) + alignof(decayed_t);
        constexpr std::size_t kPayloadSize = sizeof(decayed_t);
        const std::size_t reserve_bytes = kEnvelopeSize + kPayloadSize;

        // Non-blocking admission only: a full queue reports error_full to the caller.
        const auto reserved = worker.m_tasks_queued.producerReserve(reserve_bytes, policy);
        if (not reserved.has_value()) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return make_error_code(reserved.error());
        }

        // Align the payload from the actual record base; the offset travels with the task.
        auto *const base_address = static_cast<u8 *>(reserved->data());

        struct dispatch_t {
            static void run(decayed_t &payload, const IContext &context) {
                PPR_DEFER {
                    std::destroy_at(std::addressof(payload));
                };

                std::invoke(payload, context);
            }
        };

        decayed_t *p_payload{};
        try {
            p_payload = new (alignForward(base_address + sizeof(TaskEnvelope_), alignof_v<decayed_t>))
                decayed_t(std::forward<TaskF>(work));

        } catch (...) {
            worker.m_tasks_queued.producerDiscard(*reserved);
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            throw;
        }

        if (const auto *const p_task =
                new (reserved->data()) TaskEnvelope_{
                    .m_run = TaskEntryPoint(std23::nontype<&dispatch_t::run>, *p_payload),
                    .m_wait_group = std::move(wg)};
            p_task->m_wait_group) {
            p_task->m_wait_group->add();
        }

        worker.m_tasks_queued.producerSubmit(*reserved);
        return default_value_v;
    }
}

export template<>
struct std::is_error_code_enum<pP::ParallelScheduler::errc> : std::true_type { // NOLINT(*-dcl58-cpp)
};
