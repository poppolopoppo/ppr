module;

#include "pP/Macros.h"

module game.application;
import :world_job;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldJobs, info, none)
    struct WorldJob::State {
        ColonyDriverDesc m_request{};
        SharedContext m_context{};
        context::CancelClauseFunc m_cancel{};
        std::atomic<sim::EGenerationStage> m_stage{sim::EGenerationStage::layout};
        std::atomic<u32> m_completed{};
        std::unique_ptr<ColonyDriver> m_candidate{};
        std::unique_ptr<ColonyDriver> m_retired{};
    };

    WorldJob::WorldJob(ParallelScheduler &chunks) : WorldJob(chunks, Prepare{}) {
    }

    WorldJob::WorldJob(ParallelScheduler &chunks, Prepare prepare)
        : m_chunks(chunks), m_prepare(std::move(prepare)),
          m_coordinator(
              "World coordinator", 1u, 4u * static_cast<std::size_t>(hal::page_granularity), context::background(),
              hal::EPriority::low) {
        if (not m_prepare) {
            m_prepare = [](const ColonyDriverDesc &request,
                            const sim::GenerationExecution &execution) -> Expected<std::unique_ptr<ColonyDriver>> {
                auto driver = std::make_unique<ColonyDriver>();
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldJobs, driver->init(request, &execution));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldJobs, execution.m_context->error());
                return driver;
            };
        }
    }
    WorldJob::~WorldJob() noexcept {
        // The ordinary close path has already drained through update(). This
        // fallback is for partial initialization or a terminal/fatal run error.
        PPR_LOG_WARNING_ON_FAIL(WorldJobs, shutdown());
    }

    std::error_code WorldJob::request(const ColonyDriverDesc request) {
        if (busy()) {
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, std::errc::device_or_resource_busy);
        }
        if (m_next_id == 0u or request.m_world_generation == 0u) {
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, std::errc::value_too_large);
        }
        m_job = std::make_shared<State>();
        m_job->m_request = request;
        std::tie(m_job->m_context, m_job->m_cancel) = context::withCancelClause(context::background());
        m_view = WorldJobView{.m_id = m_next_id++, .m_state = EWorldJobState::queued};
        m_cancelled = false;
        return {};
    }

    bool WorldJob::busy() const noexcept {
        return m_job != nullptr;
    }

    void WorldJob::cancel() noexcept {
        if (m_job and m_view.m_state != EWorldJobState::retiring) {
            m_cancelled = true;
            m_job->m_cancel(std::make_error_code(std::errc::operation_canceled));
        }
    }

    WorldJobView WorldJob::view() const noexcept {
        auto result = m_view;
        if (m_job and result.m_state == EWorldJobState::generating) {
            switch (m_job->m_stage.load(std::memory_order_acquire)) {
                case sim::EGenerationStage::layout: result.m_stage = EWorldJobStage::layout; break;
                case sim::EGenerationStage::terrain:
                    result.m_stage = EWorldJobStage::terrain;
                    result.m_total = sim::kChunkCount;
                    result.m_completed = m_job->m_completed.load(std::memory_order_relaxed);
                    break;
                case sim::EGenerationStage::caves: result.m_stage = EWorldJobStage::caves; break;
                case sim::EGenerationStage::initial_matter: result.m_stage = EWorldJobStage::initial_matter; break;
                case sim::EGenerationStage::validation: result.m_stage = EWorldJobStage::validation; break;
                case sim::EGenerationStage::complete: result.m_stage = EWorldJobStage::physics; break;
            }
        }
        if (m_cancelled and m_job and result.m_state != EWorldJobState::retiring) {
            result.m_state = EWorldJobState::cancelling;
        }
        return result;
    }

    std::error_code WorldJob::admitCleanup_() {
        const auto state = m_job;
        auto task = m_coordinator.schedule(
            [state](const IContext &) -> std::error_code {
                std::error_code error{};
                if (state->m_candidate) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, state->m_candidate->shutdown());
                    state->m_candidate.reset();
                }
                if (state->m_retired) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, state->m_retired->shutdown());
                    state->m_retired.reset();
                }
                return error;
            },
            std::nullopt, ParallelScheduler::drop_if_full);
        if (not task and task.error() == make_error_code(RawChannel::error_full)) {
            return {}; // Retain both owners and retry next update.
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldJobs, task);
        m_task = std::move(*task);
        m_cleanup_pending = false;
        return {};
    }

    std::error_code WorldJob::update(const bool loading_presented) {
        if (not m_job) {
            return {};
        }
        if (m_cleanup_pending) {
            return admitCleanup_();
        }
        if (m_view.m_state == EWorldJobState::queued) {
            if (m_cancelled) {
                m_view.m_state = EWorldJobState::cancelled;
                m_job.reset(); // No candidate or work was admitted.
                return {};
            }
            if (not loading_presented) {
                return {};
            }
            const auto state = m_job;
            auto task = m_coordinator.schedule(
                [this, state](const IContext &) -> std::error_code {
                    const sim::GenerationExecution execution{
                        .m_pool = &m_chunks,
                        .m_context = state->m_context.get(),
                        .m_completed_chunks = &state->m_completed,
                        .m_stage = &state->m_stage};
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, state->m_context->error());
                    auto candidate = m_prepare(state->m_request, execution);
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, candidate);
                    state->m_candidate = std::move(*candidate);
                    return {};
                },
                std::nullopt, ParallelScheduler::drop_if_full);
            if (not task and task.error() == make_error_code(RawChannel::error_full)) {
                return {};
            }
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, task);
            m_task = std::move(*task);
            m_view.m_state = EWorldJobState::generating;
        }
        if (m_task.valid() and m_task.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            const auto error = m_task.get(); // Ready: transports only an error code; exceptions remain fatal.
            if (m_view.m_state == EWorldJobState::retiring) {
                PPR_RETURN_ERROR_ON_FAIL(WorldJobs, error);
                m_job.reset(); // Worker has already released the large owners.
                m_view.m_state = m_cancelled      ? EWorldJobState::cancelled
                                 : m_view.m_error ? EWorldJobState::failed
                                                  : EWorldJobState::succeeded;
                return {};
            }
            m_view.m_error = error;
            if (error or m_cancelled) {
                m_view.m_state = EWorldJobState::retiring;
                m_cleanup_pending = true;
                return admitCleanup_();
            }
            m_view.m_state = EWorldJobState::prepared;
            m_view.m_stage = EWorldJobStage::publishing;
        }
        if (m_view.m_state == EWorldJobState::prepared and m_cancelled) {
            m_view.m_state = EWorldJobState::retiring;
            m_cleanup_pending = true;
            return admitCleanup_();
        }
        return {};
    }

    std::unique_ptr<ColonyDriver> WorldJob::publish() {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_cancelled);
        return std::move(m_job->m_candidate);
    }

    void WorldJob::retire(std::unique_ptr<ColonyDriver> old) {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_job->m_candidate);
        m_job->m_retired = std::move(old);
        m_view.m_state = EWorldJobState::retiring;
        m_view.m_stage = EWorldJobStage::retiring;
        m_cleanup_pending = true;
    }

    void WorldJob::dispose(std::unique_ptr<ColonyDriver> old) {
        PPR_ASSERT(not busy());
        m_job = std::make_shared<State>();
        m_job->m_retired = std::move(old);
        m_view = WorldJobView{
            .m_id = m_next_id++, .m_state = EWorldJobState::retiring, .m_stage = EWorldJobStage::retiring};
        m_cancelled = false;
        m_cleanup_pending = true;
    }

    std::error_code WorldJob::shutdown() {
        cancel();
        std::error_code error{};
        if (m_task.valid()) {
            PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, m_task.get());
        }
        if (m_job) {
            m_view.m_state = EWorldJobState::retiring;
            m_cleanup_pending = true;
            PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, admitCleanup_());
            if (m_task.valid()) {
                PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, m_task.get());
            }
            m_job.reset();
        }
        PPR_RETAIN_ERROR_ON_FAIL(WorldJobs, error, m_coordinator.join());
        return error;
    }
}
