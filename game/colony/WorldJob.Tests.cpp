module;
#include "pP/UnitTest.h"

module game.tests;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.application;
import std;

namespace pP::game::tests::detail::world_job {
    using namespace colony;

    class PreparationGate final : public IContext {
        const IContext &m_parent;
        NeverEvent m_done{};
        std::atomic<sim::EGenerationStage> &m_stage;
        std::latch &m_entered;
        std::latch &m_release;
        mutable u32 m_checks{};

    public:
        PreparationGate(
            const IContext &parent, std::atomic<sim::EGenerationStage> &stage, std::latch &entered, std::latch &release)
            : m_parent(parent), m_stage(stage), m_entered(entered), m_release(release) {
        }
        std::error_code error() const noexcept override {
            if (m_stage.load(std::memory_order_acquire) == sim::EGenerationStage::complete and ++m_checks == 2u) {
                m_entered.count_down();
                m_release.wait();
            }
            return m_parent.error();
        }
        std::optional<const opaque::Block::Value *> value(const string_literal key) const noexcept override {
            return m_parent.value(key);
        }
        TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
            return m_done.subscribeEvent(signal);
        }
        void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
            m_done.unsubscribeEvent(signal, restore);
        }
        bool pollEvent() noexcept override {
            return m_done.pollEvent();
        }
        void resetEvent() noexcept override {
            m_done.resetEvent();
        }
    };

    void settle(WorldJob &job) {
        while (job.view().m_state == EWorldJobState::generating or
               job.view().m_state == EWorldJobState::retiring or
               job.view().m_state == EWorldJobState::cancelling) {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        }
    }

    PPR_UNIT_TEST (loading_presentation_gates_admission_and_cancel_is_reusable) {
        ParallelScheduler chunks{"World job test chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        std::atomic<u32> calls{};
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &) -> Expected<std::unique_ptr<ColonyDriver>> {
                calls.fetch_add(1u);
                return std::make_unique<ColonyDriver>();
            }};
        const ColonyDriverDesc request{.m_seed = 42u, .m_world_generation = 1u};
        PPR_TEST_ASSERT(not job.request(request));
        for (u32 frame = 0; frame < 64u; ++frame) {
            PPR_TEST_ASSERT(not job.update(false));
        }
        PPR_TEST_ASSERT(calls.load() == 0u);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::queued);
        const auto first_id = job.view().m_id;
        job.cancel();
        PPR_TEST_ASSERT(not job.update(false));
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(not job.request(request));
        PPR_TEST_ASSERT(job.view().m_id > first_id);
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
        auto candidate = job.publish();
        PPR_TEST_ASSERT(candidate != nullptr);
        job.retire(std::move(candidate));
        settle(job);
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::succeeded);
        PPR_TEST_ASSERT(calls.load() == 1u);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (gated_worker_keeps_control_polling_and_cancellation_live) {
        ParallelScheduler chunks{"World cancel test chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        std::latch entered{1};
        std::latch release{1};
        std::atomic<bool> cancellation_observed{};
        bool released = false;
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &,
                const sim::GenerationExecution &execution) -> Expected<std::unique_ptr<ColonyDriver>> {
                entered.count_down();
                release.wait();
                cancellation_observed.store(execution.m_context->error() == std::errc::operation_canceled);
                return std::make_unique<ColonyDriver>();
            }};
        // Release before the job's destructor even if a parent assertion throws.
        PPR_DEFER {
            if (not released) {
                release.count_down();
            }
        };
        PPR_TEST_ASSERT(not job.request(ColonyDriverDesc{.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        entered.wait();
        for (u32 frame = 0; frame < 64u; ++frame) {
            PPR_TEST_ASSERT(not job.update(true));
            PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::generating);
        }
        PPR_TEST_ASSERT(job.request(ColonyDriverDesc{.m_world_generation = 2u}) == std::errc::device_or_resource_busy);
        job.cancel();
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelling);
        release.count_down();
        released = true;
        settle(job);
        PPR_TEST_ASSERT(cancellation_observed.load());
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
    };

    PPR_UNIT_TEST (cancel_during_real_physics_preparation_discards_candidate) {
        ParallelScheduler chunks{"World physics cancel chunks", 2u, static_cast<std::size_t>(hal::page_granularity)};
        std::latch entered{1};
        std::latch release{1};
        bool released = false;
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &request,
                const sim::GenerationExecution &execution) -> Expected<std::unique_ptr<ColonyDriver>> {
                PreparationGate gate{*execution.m_context, *execution.m_stage, entered, release};
                auto local = execution;
                local.m_context = &gate;
                auto driver = std::make_unique<ColonyDriver>();
                const auto error = driver->init(request, &local);
                if (error) {
                    return std::unexpected{error};
                }
                return driver;
            }};
        PPR_DEFER {
            if (not released) {
                release.count_down();
            }
        };
        PPR_TEST_ASSERT(
            not job.request(ColonyDriverDesc{.m_seed = 42u, .m_agent_count = 100u, .m_world_generation = 7u}));
        PPR_TEST_ASSERT(not job.update(true));
        entered.wait();
        PPR_TEST_ASSERT(job.view().m_stage == EWorldJobStage::physics);
        job.cancel();
        release.count_down();
        released = true;
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (failure_is_retained_and_ready_candidate_can_still_be_cancelled) {
        ParallelScheduler chunks{"World failure test chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        bool fail = true;
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &) -> Expected<std::unique_ptr<ColonyDriver>> {
                if (fail) {
                    return std::unexpected{std::make_error_code(std::errc::io_error)};
                }
                return std::make_unique<ColonyDriver>();
            }};
        PPR_TEST_ASSERT(not job.request(ColonyDriverDesc{.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::failed);
        PPR_TEST_ASSERT(job.view().m_error == std::errc::io_error);
        fail = false; // Prior work has drained; no concurrent access.
        PPR_TEST_ASSERT(not job.request(ColonyDriverDesc{.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
        job.cancel();
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(not job.busy());
    };
}

namespace pP::game::tests {
    const UnitTest world_jobs = UnitTest::Named("world_jobs") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::world_job::loading_presentation_gates_admission_and_cancel_is_reusable,
            detail::world_job::gated_worker_keeps_control_polling_and_cancellation_live,
            detail::world_job::cancel_during_real_physics_preparation_discards_candidate,
            detail::world_job::failure_is_retained_and_ready_candidate_can_still_be_cancelled,
        });
    };
    const UnitTest &gameWorldJobTests() noexcept {
        return world_jobs;
    }
}

