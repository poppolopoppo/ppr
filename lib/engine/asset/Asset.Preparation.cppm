module;
#include "pP/Macros.h"

export module engine.asset:preparation;

import :pack;
import engine.core;
import std;

namespace pP::asset::preparation_detail {
    // ------------------------------------------------------------------
    // retained outcomes shared by every typed request
    // ------------------------------------------------------------------

    struct Outcome {
        config::Diagnostics m_diagnostics{16u};
        std::exception_ptr m_exception{};
        std::error_code m_error{};
    };

    [[nodiscard]] bool allocationFailure(const std::exception_ptr &exception) noexcept;
    void retainException(std::exception_ptr &retained, std::exception_ptr exception) noexcept;
    [[nodiscard]] std::error_code exceptionError(const std::exception_ptr &exception);
    void appendDiagnostics(config::Diagnostics &destination, const config::Diagnostics &source);
}

export namespace pP::asset {
    // ------------------------------------------------------------------
    // typed preparation contracts
    // ------------------------------------------------------------------

    enum class EPreparationStep : u8 {
        again,
        defer,
        complete
    };

    struct PreparationProgress {
        std::size_t m_admitted{}, m_completed{}, m_active{}, m_peak_active{};
    };

    /// Result owns all retained data or source handles. It must remain usable
    /// after request destruction and may be move-only or nonresident.
    template<typename Result>
    struct PreparationResult {
        std::shared_ptr<const PackSet> m_packs{};
        Result m_value;
        RequestGeneration m_generation{};
        u32 m_processing_revision{};
    };

    /// Processor declares Item, Result, maximum_active and processing_revision.
    /// prepare(packs, context, diagnostics) owns/validates inputs before admission;
    /// it performs metadata work only, without binary processing or pool waits.
    /// static process(item, packs, context, diagnostics) executes one worker step.
    /// advance(index, item, succeeded, diagnostics) runs on the external owner.
    /// again continues the same occupied slot; complete releases it. defer may
    /// wait only for another already-admitted item in this request to progress.
    /// A failed step receives one bookkeeping-only advance(..., false); its
    /// failure cannot be cleared or resubmitted. finish(items, diagnostics) runs
    /// once after successful settlement and capture destruction, producing Result.
    ///
    /// One external owner serializes all request methods. Workers capture only
    /// independent owned item/outcome state, the immutable packs and child context.
    /// The injected scheduler must outlive the request and its accepted captures.
    /// The request never cancels or joins that scheduler. No request method blocks
    /// or destroys a pending request on one of that scheduler's workers.
    template<typename Processor>
    class PreparationRequest {
        using Item = typename Processor::Item;

        using Outcome = preparation_detail::Outcome;
        static_assert(std::is_nothrow_move_constructible_v<Outcome>);
        static_assert(std::is_nothrow_move_assignable_v<Outcome>);
        static_assert(std::is_nothrow_destructible_v<Outcome>);

        enum class ESlotState : u8 {
            pending,
            running,
            advance,
            deferred,
            done
        };

        struct Slot {
            std::future<Outcome> m_task{};
            std::size_t m_index{(std::numeric_limits<std::size_t>::max)()};
            std::size_t m_advance_epoch{(std::numeric_limits<std::size_t>::max)()};
            ESlotState m_state{ESlotState::pending};
            bool m_admitted{};
        };

        struct State {
            static constexpr auto unused = (std::numeric_limits<std::size_t>::max)();

            // owned processing and worker state:
            std::shared_ptr<const PackSet> m_packs{};
            // Destroy processor state before its pinned descriptor owner.
            Processor m_processor;
            Array<std::shared_ptr<Item>> m_items{};
            Array<Outcome> m_outcomes{};
            Array<Slot> m_slots{};
            SharedContext m_context{};
            context::CancelFunc m_cancel{};

            // external execution dependency and capture settlement:
            safe_ptr<ParallelScheduler> m_pool{};
            WaitGroup m_group{};

            // retained reporting state:
            std::exception_ptr m_exception{};
            std::error_code m_wait_error{};

            // request identity and admission progress:
            RequestGeneration m_generation{};
            PreparationProgress m_progress{};
            std::size_t m_next{}, m_epoch{};
            bool m_stop{}, m_abort{};

            explicit State(Processor processor) : m_processor(std::move(processor)) {
            }

            [[nodiscard]] bool terminal() const noexcept {
                const bool occupied = std::ranges::any_of(
                    m_slots, [](const Slot &slot) { return slot.m_index != unused; });

                return not occupied and (m_stop or m_next == m_items.size()) and m_group.isReady();
            }

            [[nodiscard]] bool schedule(Slot &slot, const ParallelScheduler::EProducerPolicy policy) {
                const auto item = m_items[slot.m_index];
                auto work = [item, packs = m_packs, context = m_context](const IContext &) -> Outcome {
                    // Each tentative submission owns a fresh step outcome. A
                    // rejected queue reservation cannot discard cumulative data.
                    Outcome outcome;

                    try {
                        outcome.m_error = Processor::process(*item, *packs, *context, outcome.m_diagnostics);
                    } catch (...) {
                        // Allocation failure stays in the typed future rather
                        // than escaping into the scheduler's generic safety net.
                        outcome.m_exception = std::current_exception();
                    }

                    return outcome;
                };

                auto task = [&] {
                    if (policy == ParallelScheduler::wait_if_full) {
                        const hal::ProfileScope scope{"Asset.Preparation.Admit", hal::profile_blocking_category};
                        return m_pool->schedule(std::move(work), m_group, policy);
                    }

                    return m_pool->schedule(std::move(work), m_group, policy);
                }();
                if (not task and task.error() == make_error_code(RawChannel::error_full)) {
                    return false;
                }

                if (not task) {
                    m_outcomes[slot.m_index].m_error = task.error();
                    slot.m_state = ESlotState::done;
                    m_stop = true;
                    return true;
                }

                slot.m_task = std::move(*task);
                slot.m_state = ESlotState::running;
                if (not slot.m_admitted) {
                    slot.m_admitted = true;
                    ++m_progress.m_admitted;
                    ++m_progress.m_active;
                    m_progress.m_peak_active = (std::max) (m_progress.m_peak_active, m_progress.m_active);
                }

                return true;
            }

            void releaseCompleted() noexcept {
                for (auto &slot: m_slots) {
                    if (slot.m_index == unused or slot.m_state != ESlotState::done) {
                        continue;
                    }

                    if (slot.m_admitted) {
                        --m_progress.m_active;
                        ++m_progress.m_completed;
                    }

                    slot.m_index = unused;
                    slot.m_advance_epoch = unused;
                    slot.m_admitted = false;
                }
            }

            void pump(const ParallelScheduler::EProducerPolicy policy) noexcept {
                std::size_t current = unused;
                try {
                    const bool canceled = static_cast<bool>(m_context->error()) or m_abort;
                    m_stop = m_stop or canceled or static_cast<bool>(m_exception);

                    for (auto &slot: m_slots) {
                        if (not slot.m_task.valid() or
                            slot.m_task.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                            continue;
                        }

                        current = slot.m_index;
                        auto &outcome = m_outcomes[current];

                        // Consumption ends running before get/diagnostic merge:
                        // an exception cannot strand an invalid future in drain.
                        slot.m_state = ESlotState::advance;
                        ++m_epoch;

                        try {
                            auto step = slot.m_task.get();
                            if (not outcome.m_error) {
                                outcome.m_error = step.m_error;
                            }

                            preparation_detail::retainException(outcome.m_exception, std::move(step.m_exception));
                            preparation_detail::appendDiagnostics(outcome.m_diagnostics, step.m_diagnostics);
                        } catch (...) {
                            preparation_detail::retainException(outcome.m_exception, std::current_exception());
                        }
                    }

                    bool changed;
                    do {
                        changed = false;

                        for (auto &slot: m_slots) {
                            if (slot.m_index == unused or
                                slot.m_state == ESlotState::running or
                                slot.m_state == ESlotState::done) {
                                continue;
                            }

                            current = slot.m_index;
                            auto &outcome = m_outcomes[current];
                            const bool failed = outcome.m_error or outcome.m_exception;
                            if (canceled or m_exception) {
                                slot.m_state = ESlotState::done;
                                changed = true;
                                continue;
                            }

                            if (failed) {
                                // The processor can advance ordered bookkeeping,
                                // but cannot recover or resubmit failed work.
                                m_stop = true;
                                slot.m_state = ESlotState::done;
                                std::ignore = m_processor.advance(
                                    current, *m_items[current], false, outcome.m_diagnostics);
                                changed = true;
                                ++m_epoch;
                                continue;
                            }

                            if (slot.m_state == ESlotState::pending and m_stop and not slot.m_admitted) {
                                slot.m_state = ESlotState::done;
                                changed = true;
                                continue;
                            }

                            if (slot.m_state == ESlotState::pending) {
                                continue;
                            }

                            if (slot.m_state == ESlotState::deferred and slot.m_advance_epoch == m_epoch) {
                                continue;
                            }

                            slot.m_advance_epoch = m_epoch;
                            auto next = m_processor.advance(current, *m_items[current], true, outcome.m_diagnostics);
                            if (not next) {
                                outcome.m_error = next.error();
                                slot.m_state = ESlotState::done;
                                m_stop = true;
                                changed = true;
                                ++m_epoch;
                                continue;
                            }

                            if (*next == EPreparationStep::defer) {
                                slot.m_state = ESlotState::deferred;
                            } else {
                                slot.m_state = *next == EPreparationStep::again ? ESlotState::pending
                                                                                : ESlotState::done;
                                changed = true;
                                ++m_epoch;
                            }
                        }
                    } while (changed);

                    releaseCompleted();

                    const bool continuing = std::ranges::any_of(m_slots, [](const Slot &slot) {
                        return slot.m_index != unused and
                               (slot.m_state == ESlotState::running or
                                   (slot.m_state == ESlotState::pending and slot.m_admitted));
                    });
                    const auto deferred = std::ranges::min_element(m_slots, {}, [](const Slot &slot) {
                        return slot.m_state == ESlotState::deferred ? slot.m_index : unused;
                    });
                    if (not continuing and deferred != m_slots.end() and deferred->m_state == ESlotState::deferred) {
                        current = deferred->m_index;
                        auto &outcome = m_outcomes[current];
                        outcome.m_error = std::make_error_code(std::errc::state_not_recoverable);
                        outcome.m_diagnostics.add(
                            "asset preparation", std::to_string(current),
                            "Processor deferred without another admitted item able to progress", outcome.m_error);
                        m_stop = true;
                        m_abort = true;
                        m_cancel();
                        return;
                    }

                    for (auto &slot: m_slots) {
                        if (slot.m_index == unused and not m_stop and m_next < m_items.size()) {
                            slot.m_index = m_next++;
                            slot.m_state = ESlotState::pending;
                        }

                        if (slot.m_index == unused or slot.m_state != ESlotState::pending) {
                            continue;
                        }

                        current = slot.m_index;
                        if (not schedule(slot, policy)) {
                            break;
                        }
                    }

                    releaseCompleted();

                    current = unused;
                } catch (...) {
                    if (current != unused) {
                        preparation_detail::retainException(m_outcomes[current].m_exception, std::current_exception());
                    } else {
                        preparation_detail::retainException(m_exception, std::current_exception());
                    }

                    m_stop = true;
                    m_abort = true;
                    m_cancel();
                }
            }
        };

        std::unique_ptr<State> m_state{};

        explicit PreparationRequest(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {
        }

    public:
        using Result = PreparationResult<typename Processor::Result>;

        // ownership:
        PreparationRequest(PreparationRequest &&) noexcept = default;
        PreparationRequest &operator=(PreparationRequest &&) = delete;

        PreparationRequest(const PreparationRequest &) = delete;
        PreparationRequest &operator=(const PreparationRequest &) = delete;

        /// Explicit finish/take is the reporting boundary. External-owner fallback
        /// cancels and drains; an unobserved fatal exception terminates after drain.
        ~PreparationRequest() noexcept {
            if (not m_state) {
                return;
            }

            if (m_state->m_pool->isWorkerThread()) {
                hal::fatalExit("asset-request-destroyed-on-background-worker");
            }

            cancel();
            try {
                config::Diagnostics diagnostics{0u};
                const auto result = finish(diagnostics);
                // Best-effort teardown reports recoverable failure. Explicit
                // finish is required when the caller needs detailed diagnostics.
                if (not result and result.error() != std::errc::operation_canceled) {
                    PPR_LOG_WARNING_ON_FAIL(Asset, result);
                }
            } catch (const std::bad_alloc &) {
                if (m_state) {
                    std::ignore = m_state->m_group.wait();
                }

                hal::fatalExit("asset-request-out-of-memory-after-drain");
            } catch (...) {
                if (m_state) {
                    std::ignore = m_state->m_group.wait();
                }

                hal::fatalExit("asset-request-teardown-exception-after-drain");
            }
        }

        /// Metadata validation is complete before the first job can be admitted.
        [[nodiscard]] static Expected<PreparationRequest> start(
            ParallelScheduler &pool, std::shared_ptr<const PackSet> packs, Processor processor,
            const RequestGeneration generation, SharedContext parent, config::Diagnostics &diagnostics) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, pool.isWorkerThread() ? std::make_error_code(std::errc::resource_deadlock_would_occur)
                                             : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, not packs or *generation == 0u or Processor::maximum_active <= 0
                           ? std::make_error_code(std::errc::invalid_argument)
                           : std::error_code{});

            auto state = std::make_unique<State>(std::move(processor));
            state->m_packs = std::move(packs);
            state->m_pool = &pool;
            state->m_generation = generation;
            std::tie(state->m_context, state->m_cancel) = context::withCancel(
                parent ? std::move(parent) : context::background());
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, state->m_context->error());

            auto items = state->m_processor.prepare(*state->m_packs, *state->m_context, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, items);

            state->m_items = std::move(*items);
            state->m_outcomes.resize(state->m_items.size());

            for (const auto &item: state->m_items) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, not item ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
            }

            state->m_slots.resize((std::min) (std::size_t{Processor::maximum_active}, state->m_items.size()));
            return PreparationRequest{std::move(state)};
        }

        // polling and cancellation:
        /// True only after all accepted captures are destroyed. Readiness does
        /// not imply success; take/finish must observe errors even for stale work.
        [[nodiscard]] bool poll() noexcept {
            if (not m_state) {
                return true;
            }

            if (m_state->m_pool->isWorkerThread()) {
                hal::fatalExit("asset-request-polled-on-background-worker");
            }

            m_state->pump(ParallelScheduler::drop_if_full);
            return m_state->terminal();
        }

        void cancel() noexcept {
            if (m_state) {
                m_state->m_stop = true;
                m_state->m_cancel();
            }
        }

        [[nodiscard]] PreparationProgress progress() const noexcept {
            return m_state ? m_state->m_progress : PreparationProgress{};
        }

        // settled result reporting:
        [[nodiscard]] Expected<Result> take(config::Diagnostics &diagnostics) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, not m_state ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, m_state->m_pool->isWorkerThread()
                           ? std::make_error_code(std::errc::resource_deadlock_would_occur)
                           : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset,
                not m_state->terminal() ? std::make_error_code(std::errc::operation_in_progress) : std::error_code{});

            // Consume before any exception or diagnostic publication. A fatal
            // result now reaches the caller without a second destructor report.
            auto state = std::move(m_state);

            {
                const hal::ProfileScope scope{"Asset.Preparation.SettleCaptures", hal::profile_blocking_category};
                PPR_RETAIN_ERROR_ON_FAIL(Asset, state->m_wait_error, state->m_group.wait());
            }

            if (preparation_detail::allocationFailure(state->m_exception)) {
                std::rethrow_exception(state->m_exception);
            }

            for (const auto &outcome: state->m_outcomes) {
                if (preparation_detail::allocationFailure(outcome.m_exception)) {
                    std::rethrow_exception(outcome.m_exception);
                }
            }

            for (const auto &outcome: state->m_outcomes) {
                preparation_detail::appendDiagnostics(diagnostics, outcome.m_diagnostics);
            }

            for (const auto &outcome: state->m_outcomes) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, outcome.m_error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, preparation_detail::exceptionError(outcome.m_exception));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, preparation_detail::exceptionError(state->m_exception));
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, state->m_wait_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, state->m_context->error());

            auto result = state->m_processor.finish(
                std::span<const std::shared_ptr<Item>>{state->m_items}, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, result);

            return Result{
                std::move(state->m_packs), std::move(*result), state->m_generation, Processor::processing_revision};
        }

        /// Drain only from an external owner; worker-thread misuse returns an
        /// explicit error while retaining the request for transfer back out.
        [[nodiscard]] Expected<Result> finish(config::Diagnostics &diagnostics) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, not m_state ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, m_state->m_pool->isWorkerThread()
                           ? std::make_error_code(std::errc::resource_deadlock_would_occur)
                           : std::error_code{});

            while (not m_state->terminal()) {
                try {
                    m_state->pump(ParallelScheduler::wait_if_full);

                    for (auto &slot: m_state->m_slots) {
                        if (slot.m_task.valid()) {
                            const hal::ProfileScope scope{"Asset.Preparation.Drain", hal::profile_blocking_category};
                            slot.m_task.wait();
                        }
                    }

                    if (m_state->m_progress.m_active == 0u) {
                        const hal::ProfileScope scope{
                            "Asset.Preparation.SettleCaptures", hal::profile_blocking_category};
                        PPR_RETAIN_ERROR_ON_FAIL(Asset, m_state->m_wait_error, m_state->m_group.wait());
                    }
                } catch (...) {
                    // Reporting itself can allocate. Keep that failure, stop
                    // admissions and drain before take consumes/reports it.
                    preparation_detail::retainException(m_state->m_exception, std::current_exception());
                    m_state->m_stop = true;
                    m_state->m_abort = true;
                    m_state->m_cancel();
                }
            }

            return take(diagnostics);
        }
    };
}
