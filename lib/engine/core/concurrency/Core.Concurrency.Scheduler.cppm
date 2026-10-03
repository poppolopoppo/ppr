module;
#include "pP/Macros.h"
export module engine.core:concurrency.scheduler;

import :assert;
import :concurrency.channel;
import :concurrency.context;
import :concurrency.thread;
import :containers.stl;
import :function.ref;
import :hal;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // multi-tasking scheduler for fixed-size pool of parallel workers
    // ------------------------------------------------------------------

    class ParallelScheduler final : public safe_object {
    public:
        using TaskEntryPoint = std23::function_ref<void(const IContext &)>;

        enum errc : int {
            err_scheduler_joined = 1,
            err_task_exception,
        };

        [[nodiscard]] friend std::error_code make_error_code(errc err) noexcept;

        ParallelScheduler(
            string_literal description, std::size_t num_workers, std::size_t queue_bytes_per_worker,
            SharedContext parent = context::background());

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
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>>
        [[nodiscard]] std::error_code broadcast(const TaskF &work, std::optional<WaitGroup> wg = std::nullopt);

        template<std::invocable<const IContext &> TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and std::is_nothrow_destructible_v<std::decay_t<TaskF>>
        [[nodiscard]] std::error_code schedule(TaskF &&work, std::optional<WaitGroup> wg = std::nullopt);

        [[nodiscard]] std::error_code join(std::error_code cause = {});

    private:
        struct TaskEnvelope_ {
            TaskEntryPoint m_run;
            std::optional<WaitGroup> m_wait_group;
        };

        struct alignas(hal::cacheline_size_v) WorkerState_ final { // NOLINT(*-pro-type-member-init)
            RawChannel m_tasks_queued;
            SharedContext m_lifecycle;

            std::atomic<u32> m_num_outstanding{0u};
            const u32 m_index;

            std::jthread m_thread;
            safe_ptr<const ParallelScheduler> m_owner;

            WorkerState_(safe_ptr<const ParallelScheduler> owner, std::size_t index, std::size_t capacity_bytes);

            void workerLoop() noexcept;
        };

        static thread_local const WorkerState_ *g_worker_state_tls_;

        [[nodiscard]] std::error_code shouldCancel_() const noexcept;

        [[nodiscard]] std::error_code selectWorker_(
            std23::function_ref<std::error_code(WorkerState_ &)> producer) noexcept;

        template<typename TaskF>
            requires std::is_nothrow_move_constructible_v<TaskF> and
                     std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                     std::invocable<std::decay_t<TaskF> &, const IContext &>
        [[nodiscard]] std::error_code dispatchWork_(WorkerState_ &worker, TaskF &&work, std::optional<WaitGroup> wg);
        [[nodiscard]] std::error_code dispatchWork_(
            WorkerState_ &worker, const TaskEntryPoint &work, std::optional<WaitGroup> wg);

        Array<std::unique_ptr<WorkerState_>> m_workers;

        std::shared_mutex m_producer_mutex{};
        bool m_joined{false}; // Guarded by m_producer_mutex; set by join() under the exclusive lock.
        context::CancelClauseFunc m_request_stop;
        SharedContext m_lifecycle;
        const string_literal m_description;
    };

    // ------------------------------------------------------------------
    // parallel task helpers
    // ------------------------------------------------------------------

    namespace parallel {
        [[nodiscard]] std::error_code forRange(
            ParallelScheduler &scheduler, std::size_t first, std::size_t last,
            std23::function_ref<void(std::size_t index)> closure);

        template<
            std::ranges::random_access_range ElementsT, std::invocable<std::ranges::range_reference_t<ElementsT>> EachF>
            requires std::ranges::sized_range<ElementsT>
        [[nodiscard]] std::error_code forEach(ParallelScheduler &scheduler, ElementsT &&elements, EachF &&each) {
            return forRange(
                scheduler, 0u, std::ranges::size(elements), [&](const std::size_t index) { each(elements[index]); });
        }
    }

    // ------------------------------------------------------------------
    // template definitions
    // ------------------------------------------------------------------

    template<std::invocable<const IContext &> TaskF>
        requires std::is_copy_constructible_v<TaskF> and
                 std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>>
    std::error_code ParallelScheduler::broadcast(const TaskF &work, std::optional<WaitGroup> wg) {
        const std::shared_lock join_guard{m_producer_mutex};

        if (m_joined) [[unlikely]] {
            return make_error_code(RawChannel::error_closed);
        }

        if (const std::error_code err = shouldCancel_()) [[unlikely]] {
            return err;
        }

        // Independent copies per worker; partial successes are kept, first error reported.
        std::error_code first_err{};
        for (const std::unique_ptr<WorkerState_> &worker: m_workers) {
            if (const std::error_code err = dispatchWork_(*worker, work, wg)) [[unlikely]] {
                first_err = first_err ? first_err : err;
            }
        }
        return first_err;
    }

    template<std::invocable<const IContext &> TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and std::is_nothrow_destructible_v<std::decay_t<TaskF>>
    std::error_code ParallelScheduler::schedule(TaskF &&work, std::optional<WaitGroup> wg) {
        const std::shared_lock join_guard{m_producer_mutex};

        if (m_joined) [[unlikely]] {
            return make_error_code(RawChannel::error_closed);
        }

        if (const std::error_code err = shouldCancel_()) [[unlikely]] {
            return err;
        }

        // The callable is only moved after a reservation succeeds, so a failed
        // first choice leaves it intact for the fallback worker.
        return selectWorker_([&](WorkerState_ &worker) -> std::error_code {
            return dispatchWork_(worker, std::forward<TaskF>(work), wg);
        });
    }

    template<typename TaskF>
        requires std::is_nothrow_move_constructible_v<TaskF> and
                 std::is_nothrow_destructible_v<std::decay_t<TaskF>> and
                 std::invocable<std::decay_t<TaskF> &, const IContext &>
    std::error_code ParallelScheduler::dispatchWork_(WorkerState_ &worker, TaskF &&work, std::optional<WaitGroup> wg) {
        using DecayedT = std::decay_t<TaskF>;

        // Register the intent asap to signal the worker loop than more work is coming
        worker.m_num_outstanding.fetch_add(1u, std::memory_order_acq_rel);

        // Parent cancellation rejects admission; queued work still drains elsewhere.
        if (const std::error_code err = shouldCancel_()) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return err;
        }

        constexpr std::size_t kEnvelopeSize = sizeof(TaskEnvelope_) + alignof(DecayedT);
        constexpr std::size_t kPayloadSize = sizeof(DecayedT);
        const std::size_t reserve_bytes = kEnvelopeSize + kPayloadSize;

        // Non-blocking admission only: a full queue reports error_full to the caller.
        const auto reserved = worker.m_tasks_queued.producerReserve(reserve_bytes, RawChannel::drop_if_full);
        if (not reserved.has_value()) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return make_error_code(reserved.error());
        }

        // Align the payload from the actual record base; the offset travels with the task.
        auto *const base_address = static_cast<u8 *>(reserved->data());

        struct dispatch_t {
            static void run(DecayedT &payload, const IContext &context) {
                PPR_DEFER {
                    std::destroy_at(std::addressof(payload));
                };

                payload(context);
            }
        };

        DecayedT *p_payload{};
        try {
            p_payload = new (alignForward(base_address + sizeof(TaskEnvelope_), alignof_v<DecayedT>))
                DecayedT(std::forward<TaskF>(work));
        } catch (...) {
            worker.m_tasks_queued.producerDiscard(*reserved);
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            throw;
        }

        const auto *const p_task = new (reserved->data()) TaskEnvelope_{
            .m_run = TaskEntryPoint(std23::nontype<&dispatch_t::run>, *p_payload), .m_wait_group = std::move(wg)};

        if (p_task->m_wait_group) {
            p_task->m_wait_group->add();
        }

        worker.m_tasks_queued.producerSubmit(*reserved);
        return default_value_v;
    }
}

export template<>
struct std::is_error_code_enum<pP::ParallelScheduler::errc> : std::true_type { // NOLINT(*-dcl58-cpp)
};
