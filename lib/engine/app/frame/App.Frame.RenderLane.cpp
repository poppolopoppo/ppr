module;

#include "pP/Macros.h"

#include <cstdio>

module engine.app;

import :frame.packet;
import :frame.render_lane;

import engine.core;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(WorldControls, info, none)
    namespace {
        // Maps transport outcomes to the producer contract: full stays the
        // transient capacity-busy code (drop the frame, never fail the app),
        // closed is cancellation, anything else keeps its own code. Never
        // blocks, never allocates.
        [[nodiscard]] std::error_code translateProduceError_(const RawChannel::errc error) noexcept {
            switch (error) {
                case RawChannel::error_full: return std::make_error_code(std::errc::device_or_resource_busy);
                case RawChannel::error_closed: return std::make_error_code(std::errc::operation_canceled);
                default: return std::make_error_code(std::errc::invalid_argument);
            }
        }

        constexpr std::chrono::milliseconds kLaneSyncWaitTimeout{30000};
        constexpr std::chrono::milliseconds kLaneSyncWaitSlice{2};

        [[nodiscard]] int genericErrcValue_(const std::error_code error) noexcept {
            if (error.category() != std::generic_category()) {
                return static_cast<int>(std::errc::state_not_recoverable);
            }
            return error.value();
        }
    }

    namespace {
        [[nodiscard]] std::error_code laneExceptionError_() noexcept {
            try {
                throw;
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (const std::system_error &error) {
                return error.code();
            } catch (...) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }
    }

    // ------------------------------------------------------------------
    // correlated operation storage
    // ------------------------------------------------------------------

    struct RenderLaneOperationTicket::Record {
        enum class Phase : u8 {
            queued,
            running,
            settling,
            completed
        };

        std::unique_ptr<RenderLaneOperation> m_operation{};
        std::error_code m_result{};
        u64 m_sequence{};
        Phase m_phase{Phase::queued};
        bool m_waiting{};
        bool m_ran{};
        bool m_cancel_requested{};
        bool m_abandoned_wait{};
    };

    RenderLaneOperationTicket::RenderLaneOperationTicket() noexcept = default;

    // ------------------------------------------------------------------
    // render lane
    // ------------------------------------------------------------------

    RenderLane::RenderLane()
        : m_frames(std::make_shared<RawChannel>(kFrameDepth, sizeof(std::shared_ptr<const RenderFrameRequest>))),
          m_results(std::make_shared<RawChannel>(kResultDepth, sizeof(RenderFrameResult))),
          m_controls(std::make_shared<RawChannel>(kControlDepth, sizeof(RenderControl))) {
        // Overflow stash is pre-sized so the bounded 2-permit settlement
        // path stays allocation-free in practice: pushes never allocate
        // until the stash itself overflows (terminal path, above).
        m_result_stash.reserve(kResultDepth * 4u);
    }

    RenderLane::~RenderLane() noexcept {
        requestStop();
        // Join explicitly while every member the worker may touch is still
        // alive (the implicit jthread destruction would join moments later
        // under the same ordering). RawChannel runs no element dtors, so
        // any record still aboard (queued requests, undrained results,
        // control hints) is destroyed below — but only once the worker is
        // proven gone: draining behind a wedged worker would race its
        // producers.
        //
        // Fail-stop on bounded-join timeout (never retain-then-skip): when
        // the worker is still live past the deadline, falling through would
        // either hang inside the jthread destructor (stuck worker) or run
        // late record dtors during member destruction. Terminate here with
        // a deterministic diagnostic instead, consistent with the
        // Application-level UNKNOWN fail-stop (0xB1): no live resource is
        // freed behind the wedged worker. noexcept escape is by
        // construction (fputs/fflush cannot throw; terminate never
        // returns).
        if (const std::error_code join_err = join()) {
            std::fputs("RenderLane: worker join failed; fail-stop (no drain behind live work)\n", stderr);
            std::fflush(nullptr);
            std::terminate();
        }
        drainTransportsDestroy_();
        // Channels die after the thread (member order): emitters are
        // joined before transports die.
    }

    std::error_code RenderLane::start(
        RenderLaneOperation init, RenderFrameExecutor executor, RenderLaneSweep sweep, RenderLaneWake wake) {
        bool expected = false;
        if (not m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) [[unlikely]] {
            return std::make_error_code(std::errc::already_connected);
        }
        bool observed = false;
        if (not m_started_once.compare_exchange_strong(observed, true, std::memory_order_acq_rel)) [[unlikely]] {
            // Same-instance restart is unsupported (see m_started_once):
            // restore the running guard and fail closed.
            m_running.store(false, std::memory_order_release);
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        {
            const std::lock_guard guard{m_callback_mutex};
            m_executor = std::move(executor);
            m_sweep = std::move(sweep);
            m_wake = std::move(wake);
        }
        m_exit_requested.store(false, std::memory_order_release);
        m_stop_initiated.store(false, std::memory_order_release);
        m_inflight_estimate.store(0u, std::memory_order_release);
        m_queued_frames.store(0u, std::memory_order_release);
        m_executing_frames.store(0u, std::memory_order_release);
        m_first_error.store(0, std::memory_order_release);
        m_terminal_error.store(0, std::memory_order_release);
        m_final_sweep_error.store(0, std::memory_order_release);
        m_last_sweep_outstanding.store(0u, std::memory_order_release);
        m_last_execute_us.store(0u, std::memory_order_relaxed);
        m_worker_finished.store(false, std::memory_order_release);

        // Synchronous init ON the lane: the owner id is established there
        // and never migrates. The promise carries only the init outcome.
        std::promise<std::error_code> init_ack{};
        std::future<std::error_code> init_done = init_ack.get_future();
        m_thread = std::jthread{[this, init_op = std::move(init), ack = std::move(init_ack)]() mutable noexcept {
            hal::setThreadName("RenderLane");
            {
                // The actual promise owner dies before runLane_: publication failure breaks the future
                // rather than leaving start() blocked while the worker enters its long-lived loop.
                auto phase_ack = std::move(ack);
                std::error_code init_err{};
                try {
                    if (init_op) {
                        init_err = init_op();
                    }
                } catch (...) {
                    init_err = laneExceptionError_();
                }
                if (init_err) {
                    noteFirstError_(init_err);
                    setTerminal_(init_err);
                }
                try {
                    phase_ack.set_value(init_err);
                } catch (...) {
                    const std::error_code error = laneExceptionError_();
                    noteFirstError_(error);
                    setTerminal_(error);
                }
                init_op = nullptr;
            }
            runLane_();
            m_worker_finished.store(true, std::memory_order_release);
        }};

        std::error_code init_err{};
        try {
            init_err = init_done.get();
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            init_err = terminalError();
            if (not init_err) {
                init_err = laneExceptionError_();
            }
        }
        if (not init_err) {
            init_err = terminalError();
        }
        if (init_err) {
            requestStop();
            std::ignore = join();
        }
        return init_err;
    }

    // ------------------------------------------------------------------
    // main-side submission
    // ------------------------------------------------------------------

    void RenderLane::setExecutor(RenderFrameExecutor executor) {
        const std::lock_guard guard{m_callback_mutex};
        m_executor = std::move(executor);
    }

    void RenderLane::setSurfaceExecutor(RenderSurfaceExecutor executor) {
        const std::lock_guard guard{m_callback_mutex};
        m_surface_executor = std::move(executor);
    }

    void RenderLane::setWorldExecutor(GridWorldExecutor executor) {
        const std::lock_guard guard{m_callback_mutex};
        m_world_executor = std::move(executor);
    }

    std::error_code RenderLane::requestGridWorld(std::shared_ptr<const GridWorldCommand> command) noexcept {
        const std::lock_guard guard{m_result_stash_mutex};
        const bool open = not m_stop_initiated.load(std::memory_order_acquire) and
                          not m_surface_admission_closed.load(std::memory_order_acquire) and
                          m_running.load(std::memory_order_acquire);
        PPR_RETURN_ERROR_ON_FAIL(
            WorldControls, open ? std::error_code{} : std::make_error_code(std::errc::operation_canceled));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldControls,
            command and command->m_sequence != 0u and command->m_job_id != 0u and command->m_world_generation != 0u
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldControls,
            not m_world_command_pending ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        PPR_RETURN_ERROR_ON_FAIL(WorldControls, terminalError());

        auto reserved = m_controls->producerReserve(sizeof(RenderControl), RawChannel::drop_if_full);
        if (not reserved) {
            // Translate bounded transport backpressure; caller keeps its owner for retry.
            PPR_RETURN_ERROR_ON_FAIL(WorldControls, translateProduceError_(reserved.error()));
        }
        new (reserved->data())
            RenderControl{.m_kind = RenderControl::Kind::kGridWorld, .m_world_command = std::move(command)};
        m_world_command_pending = true;
        m_controls->producerSubmit(*reserved);
        return {};
    }

    std::error_code RenderLane::requestSurfaceReconfigure(const RenderSurfaceRequest request) noexcept {
        const std::lock_guard guard{m_result_stash_mutex};
        if (m_stop_initiated.load(std::memory_order_acquire) or
            m_surface_admission_closed.load(std::memory_order_acquire) or
            not m_running.load(std::memory_order_acquire)) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        if (request.m_transaction_id == 0u or
            not request.m_identity.m_handle or
            request.m_identity.m_generation == kSurfaceGenerationNone) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (m_surface_transaction_pending) {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        if (const std::error_code terminal = terminalError()) {
            return terminal;
        }
        auto reserved = m_controls->producerReserve(sizeof(RenderControl), RawChannel::drop_if_full);
        if (not reserved) {
            return translateProduceError_(reserved.error());
        }
        new (reserved->data())
            RenderControl{.m_kind = RenderControl::Kind::kReconfigureSurface, .m_surface_request = request};
        m_surface_transaction_pending = true;
        m_controls->producerSubmit(*reserved);
        return {};
    }

    std::error_code RenderLane::submitFrame(RenderFrameRequest request) noexcept {
        if (m_stop_initiated.load(std::memory_order_acquire) or not m_running.load(std::memory_order_acquire))
            [[unlikely]] {
            const std::error_code closed{std::make_error_code(std::errc::operation_canceled)};

            return closed;
        }
        auto reserved = m_frames->producerReserve(sizeof(request), RawChannel::drop_if_full);
        if (not reserved.has_value()) [[unlikely]] {
            if (reserved.error() == RawChannel::error_full) {
                m_dropped_count.fetch_add(1u, std::memory_order_acq_rel);
            }
            const std::error_code translated = translateProduceError_(reserved.error());

            return translated;
        }
        // Charge the queued window BEFORE publishing: the consumer decrements
        // on acquire, so a post-publish increment is observably low. The
        // aggregate estimate stays worker-published (hint only); the producer
        // never stores it, so a stale producer snapshot cannot overwrite a
        // newer worker outstanding value.
        m_queued_frames.fetch_add(1u, std::memory_order_acq_rel);
        try {
            new (reserved->data()) std::shared_ptr<const RenderFrameRequest>(
                std::make_shared<const RenderFrameRequest>(std::move(request)));
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            m_frames->producerDiscard(*reserved);
            m_queued_frames.fetch_sub(1u, std::memory_order_acq_rel);
            const std::error_code error{std::make_error_code(std::errc::state_not_recoverable)};

            return error;
        }
        m_frames->producerSubmit(*reserved);
        m_submitted_count.fetch_add(1u, std::memory_order_acq_rel);

        return default_value_v;
    }

    void RenderLane::postControl(const RenderControl::Kind kind) noexcept {
        if (kind == RenderControl::Kind::kExit) {
            // Exit survives a full control queue: the atomic is the signal,
            // the message is the wakeup.
            m_exit_requested.store(true, std::memory_order_release);
        }
        if (auto reserved = m_controls->producerReserve(sizeof(RenderControl), RawChannel::drop_if_full)) {
            static_assert(std::is_nothrow_default_constructible_v<RenderControl>);
            new (reserved->data()) RenderControl{.m_kind = kind};
            m_controls->producerSubmit(*reserved);
        }
        // A dropped wake/control is still a hint: the lane rechecks the
        // exit atomic and both queues every iteration, so nothing is lost.
    }

    Expected<std::shared_ptr<RenderLaneOperationTicket::Record>> RenderLane::admitOperation_(
        std::unique_ptr<RenderLaneOperation> &operation) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldControls,
            operation and *operation ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        std::error_code error{};
        std::shared_ptr<RenderLaneOperationTicket::Record> record;
        {
            const std::lock_guard guard{m_sync_mutex};
            if (m_stop_initiated.load(std::memory_order_acquire) or
                not m_running.load(std::memory_order_acquire) or
                not m_thread.joinable()) {
                error = std::make_error_code(std::errc::not_connected);
            } else if (m_operation_record) {
                error = std::make_error_code(std::errc::device_or_resource_busy);
            } else if (m_operation_sequence == std::numeric_limits<u64>::max()) {
                error = std::make_error_code(std::errc::value_too_large);
            } else {
                // The callable already has stable ownership. Only a fixed pointer
                // moves here: a small callable's move/destructor may reenter us.
                // The weak ticket retains this record identity, not the lane.
                record = std::make_shared<RenderLaneOperationTicket::Record>();
                record->m_sequence = m_operation_sequence + 1u;
                record->m_operation = std::move(operation);
                m_operation_sequence = record->m_sequence;
                m_operation_record = record;
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, error);

        postControl(RenderControl::Kind::kWake);
        return record;
    }

    Expected<RenderLaneOperationTicket> RenderLane::tryBeginOperation(std::unique_ptr<RenderLaneOperation> &operation) {
        auto admitted = admitOperation_(operation);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, admitted);

        RenderLaneOperationTicket ticket;
        ticket.m_record = *admitted;
        ticket.m_sequence = (*admitted)->m_sequence;
        return ticket;
    }

    std::optional<RenderLaneOperationCompletion> RenderLane::consumeOperationLocked_(
        const std::shared_ptr<RenderLaneOperationTicket::Record> &record) noexcept {
        PPR_ASSERT(m_operation_record == record);
        if (record->m_phase != RenderLaneOperationTicket::Record::Phase::completed)
            return std::nullopt;

        RenderLaneOperationCompletion completion{
            .m_operation_error = record->m_result,
            .m_transport_error = terminalError(),
            .m_ran = record->m_ran,
            .m_cancel_requested = record->m_cancel_requested};

        // The caller still owns record, whose callback/captures are already gone.
        m_operation_record.reset();
        return completion;
    }

    Expected<std::optional<RenderLaneOperationCompletion>> RenderLane::takeOperation(
        const RenderLaneOperationTicket &ticket) {
        const auto record = ticket.m_record.lock();
        std::error_code error{};
        std::optional<RenderLaneOperationCompletion> completion;
        {
            const std::lock_guard guard{m_sync_mutex};
            if (not record or m_operation_record != record or record->m_sequence != ticket.m_sequence) {
                error = std::make_error_code(std::errc::invalid_argument);
            } else {
                completion = consumeOperationLocked_(record);
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, error);

        if (completion)
            wakeMain_();

        return completion;
    }

    ERenderLaneOperationStatus RenderLane::operationStatus(const RenderLaneOperationTicket &ticket) const noexcept {
        const auto record = ticket.m_record.lock();
        const std::lock_guard guard{m_sync_mutex};
        if (not record or m_operation_record != record or record->m_sequence != ticket.m_sequence) {
            return ERenderLaneOperationStatus::invalid;
        }

        return record->m_phase == RenderLaneOperationTicket::Record::Phase::completed
                   ? ERenderLaneOperationStatus::completed
                   : ERenderLaneOperationStatus::pending;
    }

    RenderLaneOperationAvailability RenderLane::operationAvailability() const noexcept {
        const std::lock_guard guard{m_sync_mutex};
        const bool live = m_running.load(std::memory_order_acquire) and
                          not m_stop_initiated.load(std::memory_order_acquire);
        return {.m_live = live, .m_available = live and not m_operation_record};
    }

    void RenderLane::completeOperation_(const std::shared_ptr<RenderLaneOperationTicket::Record> &record) noexcept {
        {
            const std::lock_guard guard{m_sync_mutex};
            PPR_ASSERT(m_operation_record == record);
            PPR_ASSERT(record->m_phase == RenderLaneOperationTicket::Record::Phase::settling);
            record->m_phase = RenderLaneOperationTicket::Record::Phase::completed;
            if (record->m_abandoned_wait) {
                // A bounded wait abandoned this exact record. Release only now,
                // after actual callback and capture access ends.
                m_operation_record.reset();
            }
        }

        m_sync_condition.notify_all();
        wakeMain_();
    }

    Expected<ERenderLaneCancelDisposition> RenderLane::cancelOperation(const RenderLaneOperationTicket &ticket) {
        using Phase = RenderLaneOperationTicket::Record::Phase;
        const auto record = ticket.m_record.lock();
        std::error_code error{};
        ERenderLaneCancelDisposition disposition{ERenderLaneCancelDisposition::completed};
        {
            const std::lock_guard guard{m_sync_mutex};
            if (not record or m_operation_record != record or record->m_sequence != ticket.m_sequence) {
                error = std::make_error_code(std::errc::invalid_argument);
            } else if (record->m_phase == Phase::queued) {
                record->m_cancel_requested = true;
                record->m_result = std::make_error_code(std::errc::operation_canceled);
                record->m_phase = Phase::settling;
                disposition = ERenderLaneCancelDisposition::cancelled_before_run;
            } else if (record->m_phase == Phase::running) {
                record->m_cancel_requested = true;
                disposition = ERenderLaneCancelDisposition::running;
            } else if (record->m_phase == Phase::settling) {
                record->m_cancel_requested = true;
                disposition = ERenderLaneCancelDisposition::settling;
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, error);

        if (disposition == ERenderLaneCancelDisposition::cancelled_before_run) {
            // Destructors may reenter take/cancel. They see pending/settling,
            // never a false running state or premature no-further-access proof.
            record->m_operation.reset();
            completeOperation_(record);
        }

        return disposition;
    }

    Expected<RenderLaneOperationCompletion> RenderLane::waitOperation(const RenderLaneOperationTicket &ticket) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldControls, m_thread.get_id() != std::this_thread::get_id()
                               ? std::error_code{}
                               : std::make_error_code(std::errc::resource_deadlock_would_occur));

        const auto record = ticket.m_record.lock();
        std::error_code admission_error{};
        {
            const std::lock_guard guard{m_sync_mutex};
            if (not record or m_operation_record != record or record->m_sequence != ticket.m_sequence) {
                admission_error = std::make_error_code(std::errc::invalid_argument);
            } else if (record->m_waiting) {
                admission_error = std::make_error_code(std::errc::device_or_resource_busy);
            } else {
                record->m_waiting = true;
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, admission_error);

        PPR_DEFER {
            const std::lock_guard guard{m_sync_mutex};
            record->m_waiting = false;
        };

        const auto deadline = std::chrono::steady_clock::now() + kLaneSyncWaitTimeout;
        for (;;) {
            std::optional<RenderLaneOperationCompletion> completion;
            bool current = false;
            {
                std::unique_lock guard{m_sync_mutex};
                {
                    const hal::ProfileScope wait_scope{"PPR.Frame.LaneOperationWait", hal::profile_blocking_category};
                    std::ignore = m_sync_condition.wait_for(guard, kLaneSyncWaitSlice, [&] {
                        return m_operation_record != record or
                               record->m_phase == RenderLaneOperationTicket::Record::Phase::completed;
                    });
                }

                current = m_operation_record == record;
                if (current) {
                    completion = consumeOperationLocked_(record);
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldControls, current ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            if (completion) {
                wakeMain_();
                return *completion;
            }

            invokePumpSlice_();
            if (std::chrono::steady_clock::now() < deadline)
                continue;

            const auto timeout = std::make_error_code(std::errc::timed_out);
            noteFirstError_(timeout);
            setTerminal_(timeout);

            bool queued_cancel = false;
            {
                const std::lock_guard guard{m_sync_mutex};
                current = m_operation_record == record;
                if (current) {
                    if (record->m_phase == RenderLaneOperationTicket::Record::Phase::completed) {
                        completion = consumeOperationLocked_(record);
                    } else if (record->m_phase == RenderLaneOperationTicket::Record::Phase::queued) {
                        record->m_result = timeout;
                        record->m_cancel_requested = true;
                        record->m_phase = RenderLaneOperationTicket::Record::Phase::settling;
                        queued_cancel = true;
                    } else {
                        // Unknown completion retains the exact callable and its
                        // captures until actual callback/destruction settlement.
                        record->m_abandoned_wait = true;
                    }
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldControls, current ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            if (queued_cancel) {
                record->m_operation.reset();
                completeOperation_(record);

                const std::lock_guard guard{m_sync_mutex};
                if (m_operation_record == record) {
                    completion = consumeOperationLocked_(record);
                }
            }

            if (completion) {
                wakeMain_();
                return *completion;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldControls, timeout);
        }
    }

    void RenderLane::postResult_(RenderFrameResult &&result) noexcept {
        const std::lock_guard guard{m_result_stash_mutex};
        // Flush the stash in order before admitting the new result, so the
        // owner observes settlements in production order. A full channel
        // parks the remaining stash; the next post (or main's next drain)
        // retries.
        Array<RenderFrameResult> &stash = m_result_stash;
        std::size_t flushed = 0u;
        for (RenderFrameResult &pending: stash) {
            auto reserved = m_results->producerReserve(sizeof(RenderFrameResult), RawChannel::drop_if_full);
            if (not reserved) {
                break;
            }
            new (reserved->data()) RenderFrameResult(std::move(pending));
            m_results->producerSubmit(*reserved);
            ++flushed;
        }
        if (flushed > 0u) {
            stash.erase(stash.begin(), stash.begin() + static_cast<std::ptrdiff_t>(flushed));
        }
        if (stash.empty()) {
            auto reserved = m_results->producerReserve(sizeof(RenderFrameResult), RawChannel::drop_if_full);
            if (reserved) {
                new (reserved->data()) RenderFrameResult(std::move(result));
                m_results->producerSubmit(*reserved);
                return;
            }
        }
        try {
            stash.push_back(std::move(result));
            m_results_stashed.fetch_add(1u, std::memory_order_acq_rel);
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            // Unexpected stash failure: counting is NOT settlement. Durably stop
            // admission and fail visibly — a silently shed settlement would
            // leave the owner's ledger charged forever.
            m_results_stashed.fetch_add(1u, std::memory_order_acq_rel);
            const std::error_code error{std::make_error_code(std::errc::state_not_recoverable)};
            noteFirstError_(error);
            setTerminal_(error);
        }
    }

    void RenderLane::drainTransportsDestroy_() noexcept {
        // Pops every remaining record and runs its dtor: requests hold
        // shared ownership, results hold owned arrays, controls are
        // trivial (destroyed uniformly). Nonblocking peeks only — never
        // waits, never allocates, never touches close().
        for (;;) {
            auto acquired = m_frames->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                break;
            }
            std::destroy_at(static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
            m_frames->consumerRelease(*acquired);
        }
        for (;;) {
            auto acquired = m_results->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                break;
            }
            std::destroy_at(static_cast<RenderFrameResult *>(acquired->data()));
            m_results->consumerRelease(*acquired);
        }
        for (;;) {
            auto acquired = m_controls->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                break;
            }
            std::destroy_at(static_cast<RenderControl *>(acquired->data()));
            m_controls->consumerRelease(*acquired);
        }
        // The overflow stash is typed owned storage: its Array destructor
        // already destroys any parked settlement, so no manual pass runs.
    }

    std::error_code RenderLane::drainResults(Array<RenderFrameResult> &out) noexcept {
        std::error_code first_err{};
        bool failed = false;
        for (;;) {
            auto acquired = m_results->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                break;
            }
            RenderFrameResult *const result = static_cast<RenderFrameResult *>(acquired->data());
            const std::error_code record_err = result->m_error;
            if (result->m_world_result) {
                const std::lock_guard guard{m_result_stash_mutex};
                m_world_command_pending = false;
            }
            if (result->m_surface_result) {
                const std::lock_guard guard{m_result_stash_mutex};
                m_surface_transaction_pending = false;
            }
            try {
                if (not failed) {
                    out.push_back(std::move(*result));
                    if (record_err and not first_err) {
                        first_err = record_err;
                    }
                }
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (...) {
                failed = true;
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::state_not_recoverable);
                }
                // An unexpected output failure is an unrecoverable
                // transport outcome: durably stop admission so the owner
                // never treats a destroyed settlement as accounted-for —
                // and keep draining: destruction must not depend on
                // `out` again, or later records would never retire and
                // the transport would be destroyed with live nontrivial
                // records still aboard.
                noteFirstError_(first_err);
                setTerminal_(first_err);
            }
            std::destroy_at(result);
            m_results->consumerRelease(*acquired);
        }
        // Durable overflow: settlements that could not fit the bounded
        // channel travel with the lane until main reads them here. This is
        // the same exactly-once stream, never shed.
        {
            const std::lock_guard stash_guard{m_result_stash_mutex};
            std::size_t moved = 0u;
            for (RenderFrameResult &pending: m_result_stash) {
                if (pending.m_world_result) {
                    m_world_command_pending = false;
                }
                if (pending.m_surface_result) {
                    m_surface_transaction_pending = false;
                }
                try {
                    const std::error_code pending_err = pending.m_error;
                    if (not failed) {
                        if (pending_err and not first_err) {
                            first_err = pending_err;
                        }
                        out.push_back(std::move(pending));
                    }
                    ++moved;
                } catch (const std::bad_alloc &) {
                    hal::fatalExit("allocation-failure");
                } catch (...) {
                    failed = true;
                    ++moved;
                    if (not first_err) {
                        first_err = std::make_error_code(std::errc::state_not_recoverable);
                    }
                    noteFirstError_(first_err);
                    setTerminal_(first_err);
                }
            }
            if (moved > 0u) {
                m_result_stash.erase(
                    m_result_stash.begin(), m_result_stash.begin() + static_cast<std::ptrdiff_t>(moved));
            }
        }
        return first_err;
    }

    std::error_code RenderLane::terminalError() const noexcept {
        const int terminal = m_terminal_error.load(std::memory_order_acquire);
        if (terminal == 0) {
            return default_value_v;
        }
        return std::error_code{terminal, std::generic_category()};
    }

    std::error_code RenderLane::finalSweepError() const noexcept {
        const int final_error = m_final_sweep_error.load(std::memory_order_acquire);
        if (final_error == 0) {
            return default_value_v;
        }
        return std::error_code{final_error, std::generic_category()};
    }

    void RenderLane::setWaitPump(RenderLanePumpSlice pump) {
        const std::lock_guard guard{m_pump_mutex};
        m_pump_slice = std::move(pump);
    }

    void RenderLane::requestStop() noexcept {
        // Serialize admission publication with exit: every accepted request
        // precedes stop and will receive a canceled or completed ack.
        {
            const std::scoped_lock guard{m_result_stash_mutex, m_sync_mutex};
            m_stop_initiated.store(true, std::memory_order_release);
        }

        postControl(RenderControl::Kind::kExit);
    }

    std::error_code RenderLane::drainQueuedFrames(bool *out_operation_ran) noexcept {
        {
            const std::lock_guard guard{m_result_stash_mutex};
            m_surface_admission_closed.store(true, std::memory_order_release);
        }
        // Runs on the lane (serialized after any executing frame): queued
        // frames can no longer start once the executor is nulled, and the
        // drain below converts them to dropped results. A stopped lane
        // fails the handoff closed — its queues die with it, which settles
        // the same way. The handoff error is returned (never swallowed) and
        // `out_operation_ran` reports acknowledgement for first-error
        // retention/escalation by the teardown caller.
        auto publish_drain_ack_ = [out_operation_ran](const bool ran) noexcept {
            if (out_operation_ran != nullptr) {
                *out_operation_ran = ran;
            }
        };
        try {
            bool handoff_ran = false;
            auto operation_owner = std::make_unique<RenderLaneOperation>([this]() -> std::error_code {
                {
                    const std::lock_guard guard{m_callback_mutex};
                    m_executor = RenderFrameExecutor{};
                    m_world_executor = GridWorldExecutor{};
                }
                u64 drained = 0u;
                (void) drained;
                for (;;) {
                    auto acquired = m_frames->consumerAcquire(RawChannel::peek_without_blocking);
                    if (not acquired.has_value()) {
                        break;
                    }
                    std::shared_ptr<const RenderFrameRequest> request = std::move(
                        *static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
                    std::destroy_at(static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
                    m_frames->consumerRelease(*acquired);
                    ++drained;
                    m_dropped_count.fetch_add(1u, std::memory_order_acq_rel);
                    if (request and request->m_packet) {
                        try {
                            RenderFrameResult dropped{
                                .m_frame_id = request->m_packet->m_frame_id,
                                .m_error = std::make_error_code(std::errc::operation_canceled),
                                .m_dropped = true,
                                .m_reservation_epoch = request->m_reservation_epoch,
                                .m_reservation_frame_id = request->m_reservation_frame_id,
                                .m_grid_reset_caches = request->m_grid_stage.m_reset_caches,
                                .m_grid_full_refresh = request->m_grid_stage.m_full_refresh,
                                .m_grid_ack_seed = request->m_grid_stage.m_ack_seed,
                                .m_grid_ack_chunk_ids = request->m_grid_stage.m_ack_chunk_ids,
                                .m_grid_ack_revisions = request->m_grid_stage.m_ack_revisions,
                            };
                            postResult_(std::move(dropped));
                        } catch (const std::bad_alloc &) {
                            hal::fatalExit("allocation-failure");
                        } catch (...) {
                            // Unexpected manifest-copy failure: still settle the
                            // slot (never shed a credit), but durably stop
                            // admission — the owner must not ack what it
                            // cannot verify.
                            const std::error_code error{std::make_error_code(std::errc::state_not_recoverable)};
                            noteFirstError_(error);
                            setTerminal_(error);
                            static_assert(std::is_nothrow_default_constructible_v<RenderFrameResult>);
                            postResult_(
                                RenderFrameResult{
                                    .m_frame_id = request->m_packet->m_frame_id,
                                    .m_error = std::make_error_code(std::errc::operation_canceled),
                                    .m_dropped = true,
                                    .m_reservation_epoch = request->m_reservation_epoch,
                                    .m_reservation_frame_id = request->m_reservation_frame_id,
                                });
                        }
                    }
                }
                m_queued_frames.store(0u, std::memory_order_release);
                refreshEstimate_();
                std::ignore = handleControlEvent_();
                return default_value_v;
            });

            auto operation_ticket = tryBeginOperation(operation_owner);

            const auto operation_completion = operation_ticket ? waitOperation(*operation_ticket)
                                                               : Expected<RenderLaneOperationCompletion>{
                                                                     std::unexpected{operation_ticket.error()}};

            handoff_ran = operation_completion and operation_completion->m_ran;
            const std::error_code handoff_err = operation_completion
                                                    ? (operation_completion->m_operation_error
                                                              ? operation_completion->m_operation_error
                                                              : operation_completion->m_transport_error)
                                                    : operation_completion.error();
            publish_drain_ack_(handoff_ran);
            return handoff_err;
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            const std::error_code err{std::make_error_code(std::errc::state_not_recoverable)};
            noteFirstError_(err);
            setTerminal_(err);
            publish_drain_ack_(false);
            return err;
        }
    }

    std::error_code RenderLane::join() noexcept {
        try {
            if (m_thread.joinable()) {
                // Pump-compatible quiesce wait: bounded slices so main
                // services platform events while the lane drains on its
                // way out. Escalation is bounded: once the deadline
                // passes the durable first-error + terminal class are
                // latched and pumping stops; the lane object, its
                // transports, and the renderer it owns stay live — the
                // caller is responsible for escalating (teardown routes
                // to the UNKNOWN policy) rather than freeing behind a
                // wedged worker.
                const auto deadline = std::chrono::steady_clock::now() + kLaneSyncWaitTimeout;
                while (not m_worker_finished.load(std::memory_order_acquire)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        const std::error_code timeout{std::make_error_code(std::errc::timed_out)};
                        noteFirstError_(timeout);
                        setTerminal_(timeout);
                        return timeout;
                    }
                    invokePumpSlice_();
                    std::this_thread::sleep_for(kLaneSyncWaitSlice);
                }
                m_thread.join();
            }
        } catch (...) {
            const std::error_code error = laneExceptionError_();
            noteFirstError_(error);
            setTerminal_(error);
            return error;
        }
        m_running.store(false, std::memory_order_release);
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // lane execution
    // ------------------------------------------------------------------

    void RenderLane::invokePumpSlice_() noexcept {
        // Move the slice out (never hold the pump mutex across the
        // pump call itself), invoke it, then restore it so the NEXT
        // wait slice still finds it installed. A setWaitPump() that
        // raced the invoke wins: the restored slot is only refilled
        // when empty.
        RenderLanePumpSlice pump{};
        {
            const std::lock_guard guard{m_pump_mutex};
            pump = std::move(m_pump_slice);
        }
        if (not pump) {
            return;
        }
        try {
            pump();
        } catch (...) {
            const std::error_code error = laneExceptionError_();
            noteFirstError_(error);
            setTerminal_(error);
        }
        const std::lock_guard guard{m_pump_mutex};
        if (not m_pump_slice) {
            m_pump_slice = std::move(pump);
        }
    }

    void RenderLane::noteFirstError_(const std::error_code error) noexcept {
        if (not error) {
            return;
        }
        int expected = 0;
        m_first_error.compare_exchange_strong(expected, genericErrcValue_(error), std::memory_order_acq_rel);
    }

    void RenderLane::setTerminal_(const std::error_code error) noexcept {
        if (not error) {
            return;
        }
        int expected = 0;
        if (m_terminal_error.compare_exchange_strong(expected, genericErrcValue_(error), std::memory_order_acq_rel)) {
            wakeMain_();
        }
    }

    void RenderLane::wakeMain_() noexcept {
        // Invoke outside the callback lock: the wake target (admission
        // pulse/pump) must never be called under m_callback_mutex, or a
        // wedged/escalating owner that needs this lock deadlocks the lane.
        RenderLaneWake *wake = nullptr;
        {
            const std::lock_guard guard{m_callback_mutex};
            wake = m_wake ? &m_wake : nullptr;
        }
        if (wake != nullptr) {
            try {
                (*wake)();
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (...) {
                // Wake is only a hint: terminal/error latches and queued results remain durable.
                // Owner waits recheck those truths on bounded pump slices even when this hint is lost.
            }
        }
    }

    void RenderLane::runOperationIfPending_() noexcept {
        using Phase = RenderLaneOperationTicket::Record::Phase;
        std::shared_ptr<RenderLaneOperationTicket::Record> record;
        {
            const std::lock_guard guard{m_sync_mutex};
            if (not m_operation_record or m_operation_record->m_phase != Phase::queued)
                return;

            record = m_operation_record;
            record->m_phase = Phase::running;
            record->m_ran = true;
        }

        std::error_code result{};
        try {
            result = (*record->m_operation)();
        } catch (...) {
            result = laneExceptionError_();
        }

        {
            const std::lock_guard guard{m_sync_mutex};
            record->m_result = result;
            record->m_phase = Phase::settling;
        }

        record->m_operation.reset();

        if (result)
            noteFirstError_(result);

        completeOperation_(record);
    }

    void RenderLane::handleFrameEvent_() noexcept {
        auto acquired = m_frames->consumerAcquire(RawChannel::peek_without_blocking);
        if (not acquired.has_value()) {
            return;
        }
        // Move only the shared ownership out (noexcept): the request body
        // itself is never moved across the transport, only borrowed through
        // the holder for the synchronous execute below.
        std::shared_ptr<const RenderFrameRequest> holder = std::move(
            *static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
        std::destroy_at(static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
        m_frames->consumerRelease(*acquired);
        if (not holder or not holder->m_packet) [[unlikely]] {

            m_queued_frames.fetch_sub(1u, std::memory_order_acq_rel);
            refreshEstimate_();
            return;
        }
        m_queued_frames.fetch_sub(1u, std::memory_order_acq_rel);
        m_executing_frames.fetch_add(1u, std::memory_order_acq_rel);
        refreshEstimate_();


        RenderFrameResult result{};
        result.m_frame_id = holder->m_packet->m_frame_id;
        // Echo the reservation identity so the owner can pair the result
        // with exactly one construction record — every outcome below
        // (success, drop, cancel, error) carries it, and a stale result
        // never touches a different epoch's accounting.
        result.m_reservation_epoch = holder->m_reservation_epoch;
        result.m_reservation_frame_id = holder->m_reservation_frame_id;
        RenderFrameExecutor *executor = nullptr;
        {
            const std::lock_guard guard{m_callback_mutex};
            executor = m_executor ? &m_executor : nullptr;
        }
        // True lane-execution wall clock for the display-only gauge (see
        // lastExecuteUs). Two steady_clock reads, one relaxed store: no
        // allocation, noexcept, worker thread only.
        const std::chrono::steady_clock::time_point execute_start = std::chrono::steady_clock::now();
        {
            // Executor runs OUTSIDE the callback lock: it may legitimately
            // need the same mutex indirectly (wake/notify/timeout paths), so
            // lock ownership would deadlock escalation. Reseating (via
            // setExecutor) never races a running executor — the contract
            // binds it before the first submit.
            if (executor != nullptr) {
                const hal::ProfileScope scope_lane_execute{"PPR.Frame.LaneExecute"};
                try {
                    result.m_error = (*executor)(*holder, result);
                } catch (const std::bad_alloc &) {
                    hal::fatalExit("allocation-failure");
                } catch (...) {
                    result.m_error = std::make_error_code(std::errc::state_not_recoverable);
                }
            } else {
                result.m_error = std::make_error_code(std::errc::not_connected);
                result.m_dropped = true;
                try {
                    result.m_grid_reset_caches = holder->m_grid_stage.m_reset_caches;
                    result.m_grid_full_refresh = holder->m_grid_stage.m_full_refresh;
                    result.m_grid_ack_seed = holder->m_grid_stage.m_ack_seed;
                    result.m_grid_ack_chunk_ids = holder->m_grid_stage.m_ack_chunk_ids;
                    result.m_grid_ack_revisions = holder->m_grid_stage.m_ack_revisions;
                } catch (const std::bad_alloc &) {
                    hal::fatalExit("allocation-failure");
                } catch (...) {
                    result.m_error = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
        }
        if (result.m_error) {
            noteFirstError_(result.m_error);
        }
        // Gauge write: executor wall time in microseconds, monotonically
        // overwriting the last value. Display only — never read back for
        // admission/retirement. The clamp only guards a non-monotonic
        // clock; steady_clock never goes backwards here.
        const std::chrono::steady_clock::time_point execute_end = std::chrono::steady_clock::now();
        const i64 execute_delta_us =
            std::chrono::duration_cast<std::chrono::microseconds>(execute_end - execute_start).count();
        const u64 execute_us = execute_delta_us > 0 ? static_cast<u64>(execute_delta_us) : 0u;
        m_last_execute_us.store(execute_us, std::memory_order_relaxed);

        m_executing_frames.fetch_sub(1u, std::memory_order_acq_rel);

        // Sweep fence work on the lane (affinity-safe: the Renderer is lane
        // owned) and refresh the main-side admission estimate. A failed sweep
        // latches terminal so main routes to teardown, never spins. The
        // sweep verdict travels with the result: both frame and retirement
        // statuses hold sustained relevance on main.
        u32 outstanding = 0u;
        std::error_code sweep_err{};
        RenderLaneSweep *sweep = nullptr;
        {
            const std::lock_guard guard{m_callback_mutex};
            sweep = m_sweep ? &m_sweep : nullptr;
        }
        try {
            // Sweep runs OUTSIDE the callback lock (same self-deadlock
            // class as the executor above).
            const hal::ProfileScope scope_lane_sweep{"PPR.Frame.LaneSweep"};
            if (sweep != nullptr) {
                sweep_err = (*sweep)(outstanding);
            }
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            sweep_err = std::make_error_code(std::errc::state_not_recoverable);
        }
        result.m_sweep_error = sweep_err;
        m_last_sweep_outstanding.store(outstanding, std::memory_order_release);
        refreshEstimate_();
        if (sweep_err) {
            noteFirstError_(sweep_err);
            setTerminal_(sweep_err);
        }

        postResult_(std::move(result));

        wakeMain_();
    }

    bool RenderLane::handleControlEvent_() noexcept {
        bool serviced = false;
        for (;;) {
            auto acquired = m_controls->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                return serviced;
            }
            const RenderControl control = *static_cast<const RenderControl *>(acquired->data());
            std::destroy_at(static_cast<RenderControl *>(acquired->data()));
            m_controls->consumerRelease(*acquired);
            serviced = true;
            if (control.m_kind == RenderControl::Kind::kConfigChanged) {
                wakeMain_();
            }
            if (control.m_kind == RenderControl::Kind::kGridWorld and control.m_world_command) {
                RenderFrameResult completion{};
                completion.m_world_result.emplace();
                auto &result = *completion.m_world_result;
                const auto &request = *control.m_world_command;
                result.m_kind = request.m_kind;
                result.m_sequence = request.m_sequence;
                result.m_job_id = request.m_job_id;
                result.m_world_generation = request.m_world_generation;
                result.m_content_fingerprint = request.m_content_fingerprint;
                if (m_stop_initiated.load(std::memory_order_acquire) or
                    m_surface_admission_closed.load(std::memory_order_acquire)) {
                    result.m_error = std::make_error_code(std::errc::operation_canceled);
                } else {
                    GridWorldExecutor *executor = nullptr;
                    {
                        const std::lock_guard guard{m_callback_mutex};
                        executor = m_world_executor ? &m_world_executor : nullptr;
                    }
                    try {
                        if (executor != nullptr) {
                            (*executor)(request, result);
                        } else {
                            result.m_error = std::make_error_code(std::errc::not_connected);
                        }
                    } catch (const std::bad_alloc &) {
                        hal::fatalExit("allocation-failure");
                    } catch (const std::system_error &error) {
                        result.m_error = error.code() ? error.code()
                                                      : std::make_error_code(std::errc::state_not_recoverable);
                    } catch (...) {
                        result.m_error = std::make_error_code(std::errc::state_not_recoverable);
                    }
                }
                // Executors report effects; the transport owns correlation identity.
                result.m_kind = request.m_kind;
                result.m_sequence = request.m_sequence;
                result.m_job_id = request.m_job_id;
                result.m_world_generation = request.m_world_generation;
                result.m_content_fingerprint = request.m_content_fingerprint;
                postResult_(std::move(completion));
                wakeMain_();
                if (not m_stop_initiated.load(std::memory_order_acquire)) {
                    // Give a queued UI frame an opportunity between bounded world batches.
                    return serviced;
                }
            }
            if (control.m_kind == RenderControl::Kind::kReconfigureSurface) {
                RenderFrameResult completion{};
                completion.m_surface_result.emplace();
                RenderSurfaceResult &result = *completion.m_surface_result;
                const RenderSurfaceRequest &request = control.m_surface_request;
                result.m_transaction_id = request.m_transaction_id;
                result.m_identity = request.m_identity;
                if (m_stop_initiated.load(std::memory_order_acquire) or
                    m_surface_admission_closed.load(std::memory_order_acquire)) {
                    result.m_error = std::make_error_code(std::errc::operation_canceled);
                } else {
                    // Admission is suspended by the owner before posting. Run
                    // existing queued frames, not a barrier that parks their
                    // executor. GPU idleness is then established by Renderer.
                    while (m_queued_frames.load(std::memory_order_acquire) != 0u) {
                        handleFrameEvent_();
                    }
                    RenderSurfaceExecutor *executor = nullptr;
                    {
                        const std::lock_guard guard{m_callback_mutex};
                        executor = m_surface_executor ? &m_surface_executor : nullptr;
                    }
                    try {
                        if (executor != nullptr) {
                            (*executor)(request, result);
                        } else {
                            result.m_error = std::make_error_code(std::errc::not_connected);
                        }
                    } catch (const std::bad_alloc &) {
                        hal::fatalExit("allocation-failure");
                    } catch (...) {
                        result.m_error = std::make_error_code(std::errc::state_not_recoverable);
                    }
                }
                // Control errors live in the control ack, not the frame error
                // stream. Failed restoration is terminal; accepted GPU submits
                // retain their original fence/credit/staging settlement.
                result.m_transaction_id = request.m_transaction_id;
                result.m_identity = request.m_identity;
                if (result.m_applied.m_restoration_error or
                    (result.m_error and
                        result.m_applied.m_generation != kSurfaceGenerationNone and
                        not result.m_applied.m_available)) {
                    noteFirstError_(result.m_error);
                    setTerminal_(std::make_error_code(std::errc::state_not_recoverable));
                }
                postResult_(std::move(completion));
                wakeMain_();
            }
        }
    }

    void RenderLane::refreshEstimate_() noexcept {
        // Lane thread only (single writer): (queued + executing +
        // outstanding-swept). The producer charges m_queued_frames but never
        // publishes the aggregate, so a stale producer snapshot cannot
        // overwrite newer worker outstanding state. Hint only — never the
        // mechanism behind park or retirement decisions.
        const std::uint64_t queued = m_queued_frames.load(std::memory_order_acquire);
        const std::uint64_t executing = m_executing_frames.load(std::memory_order_acquire);
        const std::uint64_t outstanding = m_last_sweep_outstanding.load(std::memory_order_acquire);
        const std::uint64_t total = queued + executing + outstanding;
        m_inflight_estimate.store(
            total > static_cast<std::uint64_t>(std::numeric_limits<u32>::max()) ? std::numeric_limits<u32>::max()
                                                                                : static_cast<u32>(total),
            std::memory_order_release);
    }

    void RenderLane::sweepEstimateOnLane_() noexcept {
        const hal::ProfileScope scope_lane_sweep{"PPR.Frame.LaneSweep"};
        u32 outstanding = 0u;
        std::error_code sweep_err{};
        RenderLaneSweep *sweep = nullptr;
        {
            const std::lock_guard guard{m_callback_mutex};
            sweep = m_sweep ? &m_sweep : nullptr;
        }
        try {
            if (sweep != nullptr) {
                sweep_err = (*sweep)(outstanding);
            }
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            sweep_err = std::make_error_code(std::errc::state_not_recoverable);
        }
        m_last_sweep_outstanding.store(outstanding, std::memory_order_release);
        refreshEstimate_();
        if (sweep_err) {
            noteFirstError_(sweep_err);
            // The latch records the raw operational error class only.
            // Consumers must NOT reinterpret it as missing proof when a
            // completed lane handoff already classified the same condition
            // as kRemovalConfirmed — that completed classification is the
            // proof, and the sticky latch is then its acknowledgment, never
            // a fresh UNKNOWN.
            setTerminal_(sweep_err);
        }
        wakeMain_();
    }

    namespace {
        // Hand-rolled composite wake for the lane's two inbound queues. A
        // control message (or exit) cuts a frame wait short, and a frame
        // never delays control. This mirrors Signal<RawChannel,RawChannel>'s
        // notify/wait/poll contract exactly, but is defined locally and uses
        // only the public event primitives (subscribeEvent/unsubscribeEvent,
        // pollEvent via poll(), resetEvent) — no Signal/select/CTAD/variadic
        // instantiation in this TU, which trips MSVC C1001 when emitted from
        // an imported module unit.
        struct [[nodiscard]] LaneWakeSignal final : ISignal {
            // Atomics are truth for the pending bits; the mutex+cv only
            // gives the lane a wakeable, optionally deadline-bounded park
            // (hand-rolled multiplex stays: no Signal/select/CTAD from a
            // module TU, per the known MSVC C1001 ICE).
            std::mutex m_mutex{};
            std::condition_variable m_cv{};
            std::atomic<std::size_t> m_pending{0};

            void notify(const std::size_t event_index) noexcept override {
                if (event_index >= 2u) [[unlikely]] {
                    return;
                }
                const std::size_t bit = std::size_t{1u} << event_index;
                {
                    // Publish under the park mutex: the waiter holds the same
                    // mutex across its predicate check and the atomic
                    // wait-release, so a notify can never slip between check
                    // and park and lose the wake (check-to-park race).
                    const std::lock_guard guard{m_mutex};
                    const std::size_t prev = m_pending.fetch_or(bit, std::memory_order_release);
                    if ((prev & bit) != 0u) [[unlikely]] {
                        // Already pending: the waiter's predicate will see it.
                        return;
                    }
                }
                m_cv.notify_one();
            }

            void wait() noexcept override {
                const hal::ProfileScope scope_wait{"PPR.Frame.LaneWait", hal::profile_blocking_category};
                std::unique_lock guard{m_mutex};

                m_cv.wait(guard, [this] { return m_pending.load(std::memory_order_acquire) != 0u; });
            }

            // Bounded park: true when a bit is pending, false on expiry.
            // Used by the lane loop so retirement sweeps progress even when
            // no fresh work arrives (idle progress independent of frames).
            [[nodiscard]] bool waitFor(const std::chrono::milliseconds timeout) noexcept {
                const hal::ProfileScope scope_wait{"PPR.Frame.LaneWaitForRetirement", hal::profile_blocking_category};
                std::unique_lock guard{m_mutex};

                m_cv.wait_for(guard, timeout, [this] { return m_pending.load(std::memory_order_acquire) != 0u; });

                return m_pending.load(std::memory_order_acquire) != 0u;
            }

            [[nodiscard]] std::optional<std::size_t> poll() noexcept {
                std::size_t pending = m_pending.load(std::memory_order_acquire);
                for (;;) {
                    if (pending == 0u) {
                        return std::nullopt;
                    }
                    const std::size_t desired_pending = pending & (pending - 1u);
                    if (m_pending.compare_exchange_weak(
                            pending, desired_pending, std::memory_order_acq_rel, std::memory_order_acquire))
                        [[likely]] {
                        break;
                    }
                }
                return static_cast<std::size_t>(std::countr_zero(pending));
            }
        };
    }

    void RenderLane::runLane_() noexcept {
        // Wakeable multiplex over both inbound queues: a control message
        // (or exit) cuts a frame wait short, and a frame never delays
        // control. Either queue's pulse re-arms with backlog (see
        // RawChannel::resetEvent), so no wakeup is lost between iterations.
        // Same wake semantics and event ordering as the Signal/select walk:
        // serve the lowest pending source each poll, reset the served
        // source before polling again, wait only while nothing is pending.
        alignas(hal::cacheline_size_v) LaneWakeSignal wake{};
        const TagPtr<ISignal> frames_parent = m_frames->subscribeEvent(TagPtr<ISignal>{&wake, 0u});
        const TagPtr<ISignal> controls_parent = m_controls->subscribeEvent(TagPtr<ISignal>{&wake, 1u});

        bool have_event = false;
        std::size_t event_index = 0u;
        auto advance = [&]() noexcept {
            if (have_event) {
                if (event_index == 0u) {
                    m_frames->resetEvent();
                } else {
                    m_controls->resetEvent();
                }
                have_event = false;
            }
            for (;;) {
                if (const auto polled = wake.poll(); polled.has_value()) {
                    event_index = *polled;
                    have_event = true;
                    return;
                }
                // Idle-progress carveout: worker-owned outstanding GPU work parks
                // the lane for at most one quantum, so a genuine mutual-parked
                // state still issues the retirement sweep until the fence
                // completes and main wakes with fresh admissions. Driven by
                // the worker-published outstanding count, never the aggregate
                // hint (which may lag and is not an atomic snapshot).
                if (m_last_sweep_outstanding.load(std::memory_order_acquire) != 0u) {
                    if (not wake.waitFor(
                            std::chrono::duration_cast<std::chrono::milliseconds>(FramePacer::kWakeQuantum))) {
                        sweepEstimateOnLane_();
                        continue;
                    }
                } else {
                    wake.wait();
                }
            }
        };

        advance();
        for (;;) {
            runOperationIfPending_();
            // Control first: exit/config must preempt queued frames.
            const bool control_seen = handleControlEvent_();
            // Control path must sweep: kWake/post-config may have pushed
            // a submission boundary the lane otherwise only sees on the
            // next frame arrival.
            if (control_seen) {
                sweepEstimateOnLane_();
            }
            if (m_exit_requested.load(std::memory_order_acquire)) {
                break;
            }
            if (have_event and event_index == 0u) {
                handleFrameEvent_();
            }
            runOperationIfPending_();
            if (m_exit_requested.load(std::memory_order_acquire)) {
                break;
            }
            advance();
        }

        m_frames->unsubscribeEvent(TagPtr<ISignal>{&wake, 0u}, frames_parent);
        m_controls->unsubscribeEvent(TagPtr<ISignal>{&wake, 1u}, controls_parent);

        // Settle admitted packets on the way out: discard unsubmitted work
        // (posted back as dropped, best-effort) and retire submitted work
        // through one final sweep. Submitted GPU work itself retires in the
        // Renderer shutdown that follows on this same lane.
        const hal::ProfileScope scope_lane_quiesce{"PPR.Frame.LaneQuiesce"};
        u64 settled = 0u;
        for (;;) {
            auto acquired = m_frames->consumerAcquire(RawChannel::peek_without_blocking);
            if (not acquired.has_value()) {
                break;
            }
            std::shared_ptr<const RenderFrameRequest> request = std::move(
                *static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
            std::destroy_at(static_cast<std::shared_ptr<const RenderFrameRequest> *>(acquired->data()));
            m_frames->consumerRelease(*acquired);
            ++settled;
            m_dropped_count.fetch_add(1u, std::memory_order_acq_rel);
            if (request and request->m_packet) {
                try {
                    RenderFrameResult dropped{
                        .m_frame_id = request->m_packet->m_frame_id,
                        .m_error = std::make_error_code(std::errc::operation_canceled),
                        .m_dropped = true,
                        .m_reservation_epoch = request->m_reservation_epoch,
                        .m_reservation_frame_id = request->m_reservation_frame_id,
                        .m_grid_reset_caches = request->m_grid_stage.m_reset_caches,
                        .m_grid_full_refresh = request->m_grid_stage.m_full_refresh,
                        .m_grid_ack_seed = request->m_grid_stage.m_ack_seed,
                        .m_grid_ack_chunk_ids = request->m_grid_stage.m_ack_chunk_ids,
                        .m_grid_ack_revisions = request->m_grid_stage.m_ack_revisions,
                    };
                    postResult_(std::move(dropped));
                } catch (const std::bad_alloc &) {
                    hal::fatalExit("allocation-failure");
                } catch (...) {
                    // Unexpected manifest-copy failure: still settle the slot
                    // (never shed a credit), but durably stop admission.
                    const std::error_code error{std::make_error_code(std::errc::state_not_recoverable)};
                    noteFirstError_(error);
                    setTerminal_(error);
                    static_assert(std::is_nothrow_default_constructible_v<RenderFrameResult>);
                    postResult_(
                        RenderFrameResult{
                            .m_frame_id = request->m_packet->m_frame_id,
                            .m_error = std::make_error_code(std::errc::operation_canceled),
                            .m_dropped = true,
                            .m_reservation_epoch = request->m_reservation_epoch,
                            .m_reservation_frame_id = request->m_reservation_frame_id,
                        });
                }
            }
        }
        m_queued_frames.fetch_sub(settled, std::memory_order_acq_rel);
        refreshEstimate_();
        try {
            RenderLaneSweep *sweep = nullptr;
            {
                const std::lock_guard guard{m_callback_mutex};
                sweep = m_sweep ? &m_sweep : nullptr;
            }
            if (sweep != nullptr) {
                u32 outstanding = 0u;
                const std::error_code final_sweep_err = (*sweep)(outstanding);
                m_last_sweep_outstanding.store(outstanding, std::memory_order_release);
                refreshEstimate_();
                if (final_sweep_err) {
                    const int value = genericErrcValue_(final_sweep_err);
                    int expected = 0;
                    m_final_sweep_error.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
                    noteFirstError_(final_sweep_err);
                }
            }
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            const std::error_code err = std::make_error_code(std::errc::state_not_recoverable);
            const int value = genericErrcValue_(err);
            int expected = 0;
            m_final_sweep_error.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
            noteFirstError_(err);
        }
        runOperationIfPending_();
        // Two-step park: settle an accepted operation before and after the
        // stopped publication. Stop serialized admission earlier; accepted
        // async completions remain occupied and can still be taken after join.
        m_running.store(false, std::memory_order_release);
        // Catch all accepted controls before storage can be released. Stop
        // serializes with request publication; these acknowledgements require
        // neither a new frame nor client update to be consumed.
        std::ignore = handleControlEvent_();
        runOperationIfPending_();
        wakeMain_();
    }
}
