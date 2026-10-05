module;
#include "pP/Macros.h"

module engine.core;
import :concurrency.scheduler;
import :concurrency.thread;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Scheduler, debug, none);

    namespace {
        class SchedulerErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "scheduler";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<ParallelScheduler::errc>(ev)) {
                    case ParallelScheduler::err_scheduler_joined: return "scheduler was already joined";
                    case ParallelScheduler::err_task_exception: return "scheduler task threw an exception";
                    default: return std::format("unknown scheduler result ({})", ev);
                }
            }
        };

        const SchedulerErrorCategory g_scheduler_error_category{};
    }

    std::error_code make_error_code(const ParallelScheduler::errc err) noexcept {
        return std::error_code{static_cast<int>(err), g_scheduler_error_category};
    }

    // ------------------------------------------------------------------
    // multi-tasking scheduler for fixed-size pool of parallel workers
    // ------------------------------------------------------------------

    thread_local const ParallelScheduler::WorkerState_ *ParallelScheduler::g_worker_state_tls_ = nullptr;

    bool ParallelScheduler::isWorkerThread() const noexcept {
        return g_worker_state_tls_ != nullptr and g_worker_state_tls_->m_owner == this;
    }

    ParallelScheduler::ParallelScheduler( // NOLINT(*-pro-type-member-init)
        string_literal description, const std::size_t num_workers, const std::size_t queue_bytes_per_worker, SharedContext parent,
        hal::EPriority thread_priority)
        : m_description(description), m_thread_priority(thread_priority) {
        std::error_code err{};
        if (num_workers == 0u) [[unlikely]] {
            err = std::make_error_code(std::errc::invalid_argument);
        }
        if (num_workers > (std::numeric_limits<u32>::max)()) [[unlikely]] {
            err = std::make_error_code(std::errc::not_enough_memory);
        }
        if (queue_bytes_per_worker == 0u) [[unlikely]] {
            err = std::make_error_code(std::errc::invalid_argument);
        }
        if (err) [[unlikely]] {
            PPR_THROW(Scheduler, std::system_error(err)); // NOLINT(*-exception-baseclass)
        }

        m_workers.resize(num_workers);
        if (not parent) [[unlikely]] {
            parent = context::background();
        }

        std::tie(m_lifecycle, m_request_stop) = context::withCancelClause(std::move(parent));

        // RawChannel requires a power-of-two capacity; round up to at least one
        // HAL page. Validation above never enters the RawChannel lane itself.
        const std::size_t queue_bytes = alignForward(queue_bytes_per_worker, hal::page_granularity);

        for (const u32 worker_index: std::views::iota(0u, checked_cast<u32>(num_workers))) {
            m_workers[worker_index] = std::make_unique<WorkerState_>(
                safe_ptr<const ParallelScheduler>(this), worker_index, queue_bytes);
        }

        PPR_LOG(
            Scheduler, info, "started new task scheduler",
            {{"description", m_description}, {"num_workers", m_workers.size()},
                {"priority", enumOrd(m_thread_priority)}});
    }

    ParallelScheduler::~ParallelScheduler() {
        PPR_LOG(
            Scheduler, info, "stopped task scheduler",
            {{"description", m_description}, {"num_workers", m_workers.size()},
                {"priority", enumOrd(m_thread_priority)}});

        PPR_LOG_WARNING_ON_FAIL(Scheduler, join());
        PPR_ASSERT(getNumOutstandingTasks() == 0u);
    }

    std::size_t ParallelScheduler::getNumOutstandingTasks() const noexcept {
        std::size_t total_num_outstanding = 0u;
        for (const std::unique_ptr<WorkerState_> &worker: m_workers) {
            total_num_outstanding += worker->m_num_outstanding.load(std::memory_order_acquire);
        }
        return total_num_outstanding;
    }

    std::error_code ParallelScheduler::join(const std::error_code cause) {
        // A worker must never join the pool: reject before any lock or shutdown side effect.
        if (isWorkerThread()) [[unlikely]] {
            return std::make_error_code(std::errc::resource_deadlock_would_occur);
        }

        // A nonzero cause requests cancellation; empty cause drains gracefully
        // without firing the lifecycle event (channel close records alone shut workers down).
        if (cause) {
            m_request_stop(cause);
        }

        // Let in-flight admissions finish before closing, while consumers keep draining.
        const std::unique_lock producer_guard{m_producer_mutex};

        m_has_joined = true;

        // Close each channel once; already-closed is ignored.
        for (const std::unique_ptr<WorkerState_> &worker: m_workers) {
            std::ignore = worker->m_tasks_queued.close();
        }

        // Join worker threads while holding the producer guard.
        for (const std::unique_ptr<WorkerState_> &worker: m_workers) {
            if (worker->m_thread.joinable()) {
                worker->m_thread.join();
            }
        }

        // Workers consume-and-reset the lifecycle event flag while draining,
        // so gate the result on the stable recorded error, not on pollEvent().
        if (const std::error_code err = m_lifecycle->error()) {
            return err;
        }
        if (m_lifecycle->pollEvent()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_canceled);
        }
        return default_value_v;
    }

    ParallelScheduler::WorkerState_::WorkerState_( // NOLINT(*-pro-type-member-init)
        safe_ptr<const ParallelScheduler> owner, const std::size_t index, const std::size_t capacity_bytes)
        : m_tasks_queued(std::bit_ceil(capacity_bytes)), m_lifecycle(owner->m_lifecycle),
          m_index(checked_cast<u32>(index)), m_thread(&WorkerState_::workerLoop, this), m_owner(std::move(owner)) {
    }

    [[nodiscard]] static std::error_code runTaskUnderSafetyNet_(
        const IContext &context, ParallelScheduler::TaskEntryPoint work) noexcept {
        try {
            work(context);
        } catch (const std::system_error &e) {
            PPR_LOG(
                Scheduler, warning, "caught system error while running task",
                {{"error_code", e.code().value()}, {"message", e.code().message()}});

            return e.code() ? e.code() : make_error_code(ParallelScheduler::err_task_exception);
        } catch (const std::exception &e) {
            PPR_LOG(Scheduler, warning, "caught exception while running task", {{"what", e.what()}});

            return make_error_code(ParallelScheduler::err_task_exception);
        } catch (...) {
            return make_error_code(ParallelScheduler::err_task_exception);
        }
        return default_value_v;
    }

    void ParallelScheduler::WorkerState_::workerLoop() noexcept {
        hal::setThreadPriority(m_owner->m_thread_priority);
#if PPR_ENABLE_LOGGING
        hal::setThreadName(std::format("{}#{}", m_owner->m_description.view(), m_index));
#endif

        this_thread::setGroup(thread::worker_group);
        PPR_ASSERT(thread::worker_group());

        g_worker_state_tls_ = this;
        PPR_DEFER {
            g_worker_state_tls_ = nullptr;
        };

        bool should_drain = false;
        for (auto signal: select(*m_lifecycle, m_tasks_queued)) {
            should_drain |= std::visit(
                overloaded(
                    [&](IContext *) -> bool { return true; },
                    [&](RawChannel *) -> bool {
                        const auto record = m_tasks_queued.consumerAcquire(RawChannel::block_until_available);
                        if (not record.has_value()) [[unlikely]] {
                            if (record.error() != RawChannel::error_closed) {
                                const std::error_code err = make_error_code(record.error());
                                PPR_LOG_WARNING_ON_FAIL(Scheduler, err);
                                m_owner->m_request_stop(err);
                            }
                            return true;
                        }

                        PPR_ASSERT(record->size() >= sizeof(TaskEnvelope_) && "corrupted task payload");
                        auto *const work = std::launder(static_cast<TaskEnvelope_ *>(record->data()));

                        std::error_code err{};
                        PPR_DEFER {
                            const std::optional<WaitGroup> wg = std::move(work->m_wait_group);

                            m_tasks_queued.consumerRelease(*record);
                            m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);

                            if (wg) {
                                std::ignore = wg->done(err);
                            }
                        };

                        err = runTaskUnderSafetyNet_(*m_lifecycle, work->m_run);
                        if (err) [[unlikely]] {
                            m_owner->m_request_stop(err);
                            return true;
                        }
                        return false;
                    }),
                signal);

            if (should_drain) [[unlikely]] {
                // Must drain all registered or currently registering tasks before closing the worker, all the channel
                // could leak memory through the tasks copied or moved. Note that the current TaskEnvelope_ API doesn't
                // allow to call the deleter outside of the task entry point, but even with another refactor not running
                // the tasks could yield to other categories of leak.
                if (m_num_outstanding.load(std::memory_order_acquire) == 0u) {
                    break;
                }
            }
        }
    }

    std::error_code ParallelScheduler::shouldCancel_(const std::optional<WaitGroup> &wg) const noexcept {
        // Workers consume-and-reset the lifecycle event flag while draining,
        // so gate admission on the stable recorded error, not on pollEvent().
        if (const std::error_code err = m_lifecycle->error()) [[unlikely]] {
            return err;
        }
        if (m_lifecycle->pollEvent()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_canceled);
        }

        // Cancel if provided WaitGroup has already caught an error or an exception
        if (wg.has_value() and wg->error()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_canceled);
        }

        // Task should be scheduled
        return default_value_v;
    }

    std::pair<std::size_t, std::size_t> ParallelScheduler::selectNextWorkers_() const noexcept {
        const std::size_t num_workers = m_workers.size();
        PPR_ASSERT(num_workers > 0u);
        if (num_workers == 1u) {
            return {0u, 0u};
        }

        // Power-of-two choices: sample two distinct workers, prefer the less loaded one.
        // Both attempts are non-blocking; the fallback runs only when the first is full.
        thread_local std::mt19937_64 rng{randomNumberGenerator()};

        const std::size_t a = std::uniform_int_distribution<std::size_t>{0u, num_workers - 1u}(rng);
        const std::size_t offset = std::uniform_int_distribution<std::size_t>{1u, num_workers - 1u}(rng);
        const std::size_t b = (a + offset) % num_workers; // distinct from a

        const u32 a_num_tasks = m_workers[a]->m_num_outstanding.load(std::memory_order_acquire);
        const u32 b_num_tasks = m_workers[b]->m_num_outstanding.load(std::memory_order_acquire);

        if (b_num_tasks < a_num_tasks) {
            return {b, a};
        }
        return {a, b};
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    std::error_code ParallelScheduler::dispatchWork_(
        WorkerState_ &worker, const TaskEntryPoint &work, std::optional<WaitGroup> &&wg, const EProducerPolicy policy) {
        // Register the intent asap to signal the worker loop than more work is coming
        worker.m_num_outstanding.fetch_add(1u, std::memory_order_acq_rel);

        // Parent cancellation rejects admission; queued work still drains elsewhere.
        if (const std::error_code err = shouldCancel_(wg)) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return err;
        }

        constexpr std::size_t kEnvelopeSize = sizeof(TaskEnvelope_);

        // Non-blocking admission only: a full queue reports error_full to the caller.
        const auto reserved = worker.m_tasks_queued.producerReserve(kEnvelopeSize, policy);
        if (not reserved.has_value()) [[unlikely]] {
            worker.m_num_outstanding.fetch_sub(1u, std::memory_order_acq_rel);
            return make_error_code(reserved.error());
        }

        if (const auto *const p_task = new (reserved->data())
                TaskEnvelope_{.m_run = work, .m_wait_group = std::move(wg)};
            p_task->m_wait_group) {
            p_task->m_wait_group->add();
        }

        worker.m_tasks_queued.producerSubmit(*reserved);
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // parallel task helpers
    // ------------------------------------------------------------------

    ParallelScheduler &parallel::foregroundPool() {
        static const std::size_t kNumWorkers = std::thread::hardware_concurrency() > 2u
                                                   ? std::thread::hardware_concurrency() - 2u
                                                   : 1u;
        static const std::size_t kWorkerQueueSizeBytes = 4u * static_cast<std::size_t>(hal::page_granularity);
        static ParallelScheduler g_instance{
            "Foreground", kNumWorkers, kWorkerQueueSizeBytes, context::application(), hal::EPriority::normal};
        return g_instance;
    }

    ParallelScheduler &parallel::backgroundPool() {
        static const std::size_t kNumWorkers = std::thread::hardware_concurrency() > 2u
                                                   ? (std::thread::hardware_concurrency() - 1u) / 2u
                                                   : 1u;
        static const std::size_t kWorkerQueueSizeBytes = 4u * static_cast<std::size_t>(hal::page_granularity);
        static ParallelScheduler g_instance{
            "Background", kNumWorkers, kWorkerQueueSizeBytes, context::application(), hal::EPriority::low};
        return g_instance;
    }

    static std::error_code forRangeInternal_(
        std::size_t first, const std::size_t last, const std23::function_ref<void(std::size_t index)> closure,
        ParallelScheduler &scheduler, const WaitGroup &wg) {
        // Serial nested fallback: a worker participates synchronously instead of scheduling.
        if (not scheduler.isWorkerThread()) [[likely]] {
            const std::size_t total = last - first;
            // Calling thread also participates, hence +1
            const std::size_t lanes = scheduler.getNumWorkers() + 1u;
            // Dispatch evenly and by excess, last slice would be smaller for the callee, at least 1 per worker
            std::size_t chunk = std::max(total / lanes, static_cast<std::size_t>(1u));
            if ((total & 1u) == 1u) {
                ++chunk; // rounding by excess
            }

            for (; chunk < last - first; first += chunk) {
                const std::error_code chunk_err = scheduler.schedule(
                    [closure, i_first{first}, i_last{first + chunk}](const IContext &) {
                        for (std::size_t i = i_first; i < i_last; ++i) {
                            closure(i);
                        }
                    },
                    wg, ParallelScheduler::drop_if_full);

                if (chunk_err) [[unlikely]] {
                    if (chunk_err == make_error_code(RawChannel::error_full)) {
                        break; // Fallback on the caller drain when channel is full
                    }
                    return chunk_err;
                }
            }
        }

        // Caller participates: drain the remaining items
        return runTaskUnderSafetyNet_(scheduler.getContext(), [&](const IContext &) {
            for (; first < last; ++first) {
                closure(first);
            }
        });
    }

    std::error_code parallel::forRange(
        const std::size_t first, const std::size_t last, const std23::function_ref<void(std::size_t index)> closure,
        ParallelScheduler &scheduler) {
        if (first > last) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (first == last) {
            return default_value_v;
        }
        if (const std::error_code err = scheduler.error()) [[unlikely]] {
            return err;
        }

        const WaitGroup wg;
        std::error_code err = forRangeInternal_(first, last, closure, scheduler, wg);
        err = err | wg.wait();
        err = err | scheduler.error();
        return err;
    }
}
