module;

#include "pP/Macros.h"

module game.application;
import :world_job;
import game.colony.sim_packet;
import game.colony.worldgen;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldJobs, info, none)
    struct WorldJob::State {
        ColonyDriverDesc m_request{};
        config::Diagnostics m_diagnostics{};
        std::string m_diagnostic_text{};
        SharedContext m_context{};
        context::CancelClauseFunc m_cancel{};
        std::atomic<sim::EGenerationStage> m_stage{sim::EGenerationStage::layout};
        std::atomic<u32> m_completed{};
        std::unique_ptr<ColonyDriver> m_candidate{};
        std::unique_ptr<ColonyDriver> m_retired{};
        std::unique_ptr<GridPass::GridStagedFrame> m_presentation{};
        std::optional<WorldCheckpoint> m_checkpoint{};
        Capture m_capture{};
        bool m_is_capture{}; // Immutable after admission; cleanup may release the callback concurrently.
    };

    WorldJob::WorldJob(ParallelScheduler &chunks) : WorldJob(chunks, Prepare{}) {
    }

    WorldJob::WorldJob(ParallelScheduler &chunks, Prepare prepare)
        : m_chunks(chunks), m_prepare(std::move(prepare)),
          m_coordinator(
              "World coordinator", 1u, 4u * static_cast<std::size_t>(hal::page_granularity), context::background(),
              hal::EPriority::low) {
        if (not m_prepare) {
            m_prepare = [](const ColonyDriverDesc &request, const sim::GenerationExecution &execution,
                            config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
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
        m_diagnostic_text.clear();
        m_job = std::make_shared<State>();
        m_job->m_request = request;
        std::tie(m_job->m_context, m_job->m_cancel) = context::withCancelClause(context::background());
        m_view = WorldJobView{.m_id = m_next_id++, .m_state = EWorldJobState::queued};
        m_cancelled = false;
        m_publication_committed = false;
        return {};
    }

    bool WorldJob::busy() const noexcept {
        return m_job != nullptr;
    }

    std::error_code WorldJob::requestCapture(std::unique_ptr<ColonyDriver> &frozen, Capture capture) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldJobs, busy() ? std::make_error_code(std::errc::device_or_resource_busy) : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            WorldJobs, frozen and capture ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldJobs, m_next_id != 0u ? std::error_code{} : std::make_error_code(std::errc::value_too_large));

        PPR_RETURN_ERROR_ON_FAIL(
            WorldJobs,
            frozen->captureReady() ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        auto state = std::make_shared<State>();
        std::tie(state->m_context, state->m_cancel) = context::withCancelClause(context::background());
        state->m_capture = std::move(capture);
        state->m_is_capture = true;
        state->m_candidate = std::move(frozen);
        m_job = std::move(state);
        m_view = WorldJobView{
            .m_id = m_next_id++, .m_state = EWorldJobState::queued, .m_stage = EWorldJobStage::capturing};
        m_diagnostic_text.clear();
        m_cancelled = false;
        m_publication_committed = false;
        return {};
    }

    std::error_code WorldJob::updateCapture_(const bool loading_presented) {
        if (m_view.m_state == EWorldJobState::queued) {
            if (m_cancelled) {
                m_view.m_error = std::make_error_code(std::errc::operation_canceled);
                m_view.m_state = EWorldJobState::prepared;
                return {};
            }
            if (not loading_presented) {
                return {};
            }
            const auto state = m_job;
            auto task = m_coordinator.schedule(
                [state](const IContext &) -> std::error_code {
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, state->m_context->error());
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, state->m_capture(*state->m_candidate, *state->m_context));
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, state->m_context->error());
                    return {};
                },
                std::nullopt, ParallelScheduler::drop_if_full);
            if (not task and task.error() == make_error_code(RawChannel::error_full)) {
                return {}; // Keep exclusive ownership and retry without waiting.
            }
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, task);
            m_task = std::move(*task);
            m_view.m_state = EWorldJobState::generating;
        }
        if (m_task.valid() and m_task.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            m_view.m_error = m_task.get(); // Ready; allocation exceptions remain fatal.
            if (m_cancelled) {
                m_view.m_error = std::make_error_code(std::errc::operation_canceled);
            }
            m_view.m_state = EWorldJobState::prepared;
        }
        return {};
    }

    std::error_code WorldJob::requestPreview(
        std::unique_ptr<ColonyDriver> &frozen, const EGridDiagnosticMode mode, const float temperature_min_k,
        const float temperature_max_k) {
        PPR_RETURN_ERROR_ON_FAIL(WorldJobs, requestCapture(frozen, [](const ColonyDriver &, const IContext &) {
            return std::error_code{};
        }));
        State *const state = m_job.get(); // Stable owner outlives callback/task and retained presentation.
        const u64 revision = m_view.m_id;
        state->m_capture = [state, mode, revision, temperature_min_k, temperature_max_k](
                               const ColonyDriver &driver, const IContext &context) {
            const auto *const prepared = driver.preparedWorld();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldJobs, prepared ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            auto presentation = captureDiagnosticPresentation(
                driver, prepared->generated(), mode, revision, temperature_min_k, temperature_max_k, context);
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, presentation);
            state->m_presentation = std::make_unique<GridPass::GridStagedFrame>(std::move(*presentation));
            return std::error_code{};
        };
        return {};
    }

    std::error_code WorldJob::requestCheckpoint(std::unique_ptr<ColonyDriver> &frozen) {
        PPR_RETURN_ERROR_ON_FAIL(WorldJobs, requestCapture(frozen, [](const ColonyDriver &, const IContext &) {
            return std::error_code{};
        }));
        State *const state = m_job.get();
        state->m_capture = [state](const ColonyDriver &driver, const IContext &context) {
            const auto *const prepared = driver.preparedWorld();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldJobs, prepared ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            auto checkpoint = captureWorldCheckpoint(driver, prepared->generated(), &context);
            PPR_RETURN_ERROR_ON_FAIL(WorldJobs, checkpoint);
            state->m_checkpoint.emplace(std::move(*checkpoint));
            return std::error_code{};
        };
        return {};
    }

    WorldCheckpoint WorldJob::takeCheckpoint() {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_view.m_error and m_job->m_checkpoint);
        return std::move(*m_job->m_checkpoint);
    }

    std::unique_ptr<ColonyDriver> WorldJob::takeCaptured(const bool retain_presentation) {
        PPR_ASSERT(m_job and m_job->m_is_capture and m_view.m_state == EWorldJobState::prepared);
        auto owner = std::move(m_job->m_candidate);
        PPR_ASSERT(owner != nullptr);
        if (retain_presentation) {
            return owner;
        }
        m_view.m_state = EWorldJobState::retiring;
        m_view.m_stage = EWorldJobStage::retiring;
        m_cleanup_pending = true;
        return owner;
    }

    void WorldJob::cancel() noexcept {
        if (m_job and not m_publication_committed and m_view.m_state != EWorldJobState::retiring) {
            m_cancelled = true;
            m_job->m_cancel(std::make_error_code(std::errc::operation_canceled));
            if (m_job->m_is_capture and m_view.m_state == EWorldJobState::prepared) {
                m_view.m_error = std::make_error_code(std::errc::operation_canceled);
            }
        }
    }

    WorldJobView WorldJob::view() const noexcept {
        auto result = m_view;
        if (m_job and not m_job->m_is_capture and result.m_state == EWorldJobState::generating) {
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
        if (m_cancelled and
            m_job and
            result.m_state != EWorldJobState::retiring and
            (not m_job->m_is_capture or result.m_state != EWorldJobState::prepared)) {
            result.m_state = EWorldJobState::cancelling;
        }
        return result;
    }

    std::string_view WorldJob::diagnosticText() const noexcept {
        return m_diagnostic_text;
    }

    std::error_code WorldJob::admitCleanup_() {
        const auto state = m_job;
        auto task = m_coordinator.schedule(
            [state](const IContext &) -> std::error_code {
                std::error_code error{};
                state->m_presentation.reset();
                state->m_checkpoint.reset();
                state->m_capture = {}; // Captured output owners are also disposed off-main.
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

    std::error_code WorldJob::update(const bool loading_presented, const bool allow_cleanup) {
        if (not m_job) {
            return {};
        }
        if (m_cleanup_pending) {
            return allow_cleanup ? admitCleanup_() : std::error_code{};
        }
        if (m_job->m_is_capture and m_view.m_state != EWorldJobState::retiring) {
            return updateCapture_(loading_presented);
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
                    auto candidate = m_prepare(state->m_request, execution, state->m_diagnostics);
                    if (not candidate) {
                        state->m_diagnostic_text = state->m_diagnostics.format();
                        if (state->m_diagnostic_text.size() > 64u * 1024u) {
                            state->m_diagnostic_text.resize(64u * 1024u);
                            state->m_diagnostic_text += "\nFurther diagnostics omitted.";
                        }
                        if (not state->m_diagnostic_text.empty()) {
                            PPR_LOG(
                                WorldJobs, error, "world content preparation failed",
                                {{"diagnostics", state->m_diagnostic_text}});
                        }
                    }
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, candidate);
                    state->m_candidate = std::move(*candidate);
                    auto presentation = captureWorldPresentation(*state->m_candidate, execution.m_context);
                    PPR_RETURN_ERROR_ON_FAIL(WorldJobs, presentation);
                    state->m_presentation = std::make_unique<GridPass::GridStagedFrame>(std::move(*presentation));
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
            m_diagnostic_text = std::move(m_job->m_diagnostic_text);
            m_view.m_error = error;
            if (error or m_cancelled) {
                m_view.m_state = EWorldJobState::retiring;
                m_cleanup_pending = true;
                return allow_cleanup ? admitCleanup_() : std::error_code{};
            }
            m_view.m_state = EWorldJobState::prepared;
            m_view.m_stage = EWorldJobStage::publishing;
        }
        if (m_view.m_state == EWorldJobState::prepared and m_cancelled) {
            m_view.m_state = EWorldJobState::retiring;
            m_cleanup_pending = true;
            return allow_cleanup ? admitCleanup_() : std::error_code{};
        }
        return {};
    }

    std::unique_ptr<ColonyDriver> WorldJob::publish() {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_cancelled);
        return std::move(m_job->m_candidate);
    }

    GridPass::GridStagedFrame &WorldJob::presentation() {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and m_job->m_presentation);
        return *m_job->m_presentation;
    }

    void WorldJob::commitPublication() {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_cancelled);
        m_publication_committed = true;
    }

    void WorldJob::discardPrepared(const std::error_code error) {
        PPR_ASSERT(m_job and m_view.m_state == EWorldJobState::prepared and not m_publication_committed and error);
        m_view.m_error = error;
        m_view.m_state = EWorldJobState::retiring;
        m_view.m_stage = EWorldJobStage::retiring;
        m_cleanup_pending = true;
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
