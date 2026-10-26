module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;
import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.application;
import engine.asset;
import engine.image;
import game.colony.content;
import game.colony.worldgen;
import std;

#include "Colony.AuthoredFixture.Tests.h"

namespace pP::game::tests::detail::world_job {
    // ------------------------------------------------------------------
    // world jobs
    // ------------------------------------------------------------------

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

    PPR_UNIT_TEST (authored_fixture_replays_owned_terrain_and_marks_every_chunk_dirty) {
        colony::test_fixture::AuthoredFixture authored;
        auto first = authored.prepare(1234567u, 1u);
        auto second = authored.prepare(1234567u, 2u);
        PPR_TEST_ASSERT(first.has_value() and second.has_value());
        PPR_TEST_ASSERT(first->grid().residentChunkCount() == sim::kChunkCount);
        PPR_TEST_ASSERT(first->grid().dirtyChunkCount() == sim::kChunkCount);
        PPR_TEST_ASSERT(first->grid().dirtyChunks().size() == sim::kChunkCount);

        for (u32 y = 0u; y < sim::kChunksPerEdge; ++y) {
            for (u32 x = 0u; x < sim::kChunksPerEdge; ++x) {
                const auto expected = first->grid().residentCells({x, y});
                const auto actual = second->grid().residentCells({x, y});
                PPR_TEST_ASSERT(expected.has_value() and actual.has_value());
                PPR_TEST_ASSERT(std::ranges::equal(*expected, *actual));
            }
        }

        auto other = authored.prepare(7654321u, 3u);
        PPR_TEST_ASSERT(other.has_value());
        PPR_TEST_ASSERT(
            first->generated().m_report.m_terrain_checksum != other->generated().m_report.m_terrain_checksum);
    };

    PPR_UNIT_TEST (authored_fixture_cancelled_generation_returns_no_owner_or_completed_chunks) {
        colony::test_fixture::AuthoredFixture authored;
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        std::atomic<u32> completed{};
        const sim::GenerationExecution execution{.m_context = context, .m_completed_chunks = &completed};

        const auto result = authored.prepare(42u, 1u, execution);
        PPR_TEST_ASSERT(not result.has_value() and result.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(completed.load() == 0u);
    };

    PPR_UNIT_TEST (owned_request_override_survives_cancel_and_default_policy_is_unchanged) {
        ParallelScheduler chunks{"Authored override chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        std::atomic<u32> default_calls{};
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                ++default_calls;
                return std::make_unique<ColonyDriver>();
            }};

        auto source = std::make_shared<const std::string>("ppr.authored:chosen");
        const std::weak_ptr<const std::string> observer = source;
        std::latch entered{1}, release{1};
        std::atomic<bool> cancellation_observed{};
        bool released = false;
        PPR_DEFER {
            if (not released) {
                release.count_down();
            }

            // Best-effort assertion unwinding must join before the captured latches disappear.
            std::ignore = job.shutdown();
        };

        PPR_TEST_ASSERT(not job.request(
            {.m_seed = 17u, .m_world_generation = 1u},
            [source, &entered, &release, &cancellation_observed](
                const ColonyDriverDesc &request, const sim::GenerationExecution &execution,
                config::Diagnostics &diagnostics) -> Expected<std::unique_ptr<ColonyDriver>> {
                PPR_TEST_ASSERT(request.m_seed == 17u and *source == "ppr.authored:chosen");
                entered.count_down();
                release.wait();

                const auto error = execution.m_context->error();
                cancellation_observed = error == std::errc::operation_canceled;
                diagnostics.add("owned snapshot", "profile", "cancelled override", error);
                return std::unexpected{error};
            }));

        source.reset();
        PPR_TEST_ASSERT(not observer.expired());

        PPR_TEST_ASSERT(not job.update(true));
        entered.wait();
        PPR_TEST_ASSERT(default_calls == 0u and not observer.expired());

        job.cancel();
        PPR_TEST_ASSERT(not observer.expired());
        release.count_down();
        released = true;
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(cancellation_observed and observer.expired());
        PPR_TEST_ASSERT(job.diagnosticText().find("owned snapshot") != std::string_view::npos);

        PPR_TEST_ASSERT(not job.request({.m_world_generation = 2u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(default_calls == 1u);

        job.cancel();
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (queued_override_payload_is_released_after_coordinator_retirement) {
        ParallelScheduler chunks{"Queued authored chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        PPR_DEFER {
            std::ignore = job.shutdown();
        };

        auto source = std::make_shared<const std::string>("owned draft");
        const std::weak_ptr<const std::string> observer = source;
        PPR_TEST_ASSERT(not job.request(
            {.m_world_generation = 1u},
            [source](const ColonyDriverDesc &, const sim::GenerationExecution &, config::Diagnostics &)
                -> Expected<std::unique_ptr<ColonyDriver>> { return std::make_unique<ColonyDriver>(); }));

        source.reset();

        job.cancel();
        PPR_TEST_ASSERT(not job.update(false));
        settle(job);
        PPR_TEST_ASSERT(not job.busy() and job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(observer.expired());
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (cancelled_header_retains_catalog_until_external_release) {
        ParallelScheduler chunks{"Header retirement chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &, config::Diagnostics &)
                -> Expected<std::unique_ptr<ColonyDriver>> { return std::make_unique<ColonyDriver>(); }};
        PPR_TEST_ASSERT(not job.request({.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);

        auto *const presentation = &job.presentation();
        presentation->m_diagnostic_palette.resize(65536u);

        job.cancel();

        for (u32 frame = 0u; frame < 64u; ++frame) {
            PPR_TEST_ASSERT(not job.update(true, false));
            PPR_TEST_ASSERT(job.busy());
            PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::retiring);
            PPR_TEST_ASSERT(presentation->m_diagnostic_palette.size() == 65536u);
        }

        // The external header is now released; only coordinator-owned payloads remain.
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (loading_presentation_gates_admission_and_cancel_is_reusable) {
        ParallelScheduler chunks{"World job test chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        std::atomic<u32> calls{};
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
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
        PPR_TEST_ASSERT(job.presentation().m_uploads.empty());

        job.commitPublication();
        job.cancel(); // An accepted activation must finish CPU publication even during close.
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
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &execution,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
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

    PPR_UNIT_TEST (frozen_capture_returns_same_owner_after_queued_cancel) {
        ParallelScheduler chunks{"Frozen capture chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        auto driver = std::make_unique<ColonyDriver>();
        const auto address = driver.get();
        std::atomic<u32> calls{};
        PPR_TEST_ASSERT(not job.requestCapture(driver, [&](const ColonyDriver &, const IContext &) {
            calls.fetch_add(1u);
            return std::error_code{};
        }));
        PPR_TEST_ASSERT(not driver);
        PPR_TEST_ASSERT(not job.update(false));
        job.cancel();
        PPR_TEST_ASSERT(not job.update(false));
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
        PPR_TEST_ASSERT(job.view().m_error == std::errc::operation_canceled);
        driver = job.takeCaptured();
        PPR_TEST_ASSERT(driver.get() == address);
        settle(job);
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(calls.load() == 0u);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (frozen_capture_gate_preserves_exclusivity_and_returns_owner_on_failure) {
        ParallelScheduler chunks{"Frozen gated chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        auto driver = std::make_unique<ColonyDriver>();
        const auto address = driver.get();
        std::latch entered{1};
        std::latch release{1};
        bool released = false;
        PPR_DEFER {
            if (not released) {
                release.count_down();
            }
        };

        PPR_TEST_ASSERT(not job.requestCapture(driver, [&](const ColonyDriver &frozen, const IContext &) {
            PPR_TEST_ASSERT(&frozen == address);
            entered.count_down();
            release.wait();
            return std::make_error_code(std::errc::io_error);
        }));
        PPR_TEST_ASSERT(not job.update(true));
        entered.wait();

        for (u32 frame = 0u; frame < 64u; ++frame) {
            PPR_TEST_ASSERT(not job.update(true));
            PPR_TEST_ASSERT(job.view().m_stage == EWorldJobStage::capturing);
        }
        PPR_TEST_ASSERT(job.request(ColonyDriverDesc{.m_world_generation = 2u}) == std::errc::device_or_resource_busy);

        auto other = std::make_unique<ColonyDriver>();
        PPR_TEST_ASSERT(job.requestCapture(other, [](const ColonyDriver &, const IContext &) {
            return std::error_code{};
        }) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(other != nullptr);

        release.count_down();
        released = true;
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
        PPR_TEST_ASSERT(job.view().m_error == std::errc::io_error);
        driver = job.takeCaptured();
        PPR_TEST_ASSERT(driver.get() == address);
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::failed);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (frozen_capture_success_and_prepared_cancel_have_consistent_outcomes) {
        ParallelScheduler chunks{"Frozen completion chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        auto driver = std::make_unique<ColonyDriver>();
        const auto address = driver.get();
        for (const bool cancel_ready: {false, true}) {
            PPR_TEST_ASSERT(not job.requestCapture(
                driver, [](const ColonyDriver &, const IContext &) { return std::error_code{}; }));
            PPR_TEST_ASSERT(not job.update(true));
            settle(job);
            PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
            PPR_TEST_ASSERT(not job.view().m_error);
            if (cancel_ready) {
                job.cancel();
            }
            PPR_TEST_ASSERT(
                job.view().m_error ==
                (cancel_ready ? std::make_error_code(std::errc::operation_canceled) : std::error_code{}));
            driver = job.takeCaptured();
            PPR_TEST_ASSERT(driver.get() == address);
            settle(job);
            PPR_TEST_ASSERT(
                job.view().m_state == (cancel_ready ? EWorldJobState::cancelled : EWorldJobState::succeeded));
        }
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (preview_failure_and_queued_cancel_return_frozen_owner) {
        ParallelScheduler chunks{"Preview failure chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        auto driver = std::make_unique<ColonyDriver>();
        const auto address = driver.get();
        for (const bool cancel_queued: {false, true}) {
            PPR_TEST_ASSERT(not job.requestPreview(driver, EGridDiagnosticMode::biome, 200.0f, 400.0f));
            PPR_TEST_ASSERT(not driver);
            if (cancel_queued) {
                job.cancel();
            }
            PPR_TEST_ASSERT(not job.update(true));
            settle(job);
            PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
            const auto expected = cancel_queued ? std::errc::operation_canceled : std::errc::invalid_argument;
            PPR_TEST_ASSERT(job.view().m_error == expected);
            driver = job.takeCaptured(true);
            PPR_TEST_ASSERT(driver.get() == address);
            PPR_TEST_ASSERT(job.busy());
            job.discardPrepared(job.view().m_error);
            settle(job);
            PPR_TEST_ASSERT(not job.busy());
            PPR_TEST_ASSERT(job.view().m_state == (cancel_queued ? EWorldJobState::cancelled : EWorldJobState::failed));
        }
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (cancel_during_real_physics_preparation_discards_candidate) {
        ParallelScheduler chunks{"World physics cancel chunks", 2u, static_cast<std::size_t>(hal::page_granularity)};
        std::latch entered{1};
        std::latch release{1};
        bool released = false;
        colony::test_fixture::AuthoredFixture authored;
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &request, const sim::GenerationExecution &execution,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                auto local = execution;
                local.m_context = std::make_shared<PreparationGate>(
                    *execution.m_context, *execution.m_stage, entered, release);

                auto driver = std::make_unique<ColonyDriver>();
                const auto error = authored.initialize(*driver, request, &local);
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
            not job.request(ColonyDriverDesc{.m_seed = 42u, .m_agent_count = 1u, .m_world_generation = 7u}));
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

    PPR_UNIT_TEST (candidate_graphics_failure_retires_cpu_owner_and_preserves_failure) {
        ParallelScheduler chunks{"World graphics failure chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &, config::Diagnostics &)
                -> Expected<std::unique_ptr<ColonyDriver>> { return std::make_unique<ColonyDriver>(); }};
        PPR_TEST_ASSERT(not job.request(ColonyDriverDesc{.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared);
        PPR_TEST_ASSERT(job.presentation().m_tiles.empty());
        PPR_TEST_ASSERT(not job.presentationFailed(std::make_error_code(std::errc::io_error)));
        settle(job);
        PPR_TEST_ASSERT(not job.busy());
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::failed);
        PPR_TEST_ASSERT(job.view().m_error == std::errc::io_error);
        PPR_TEST_ASSERT(not job.shutdown());
    };

    PPR_UNIT_TEST (independent_work_admission_prepares_without_loading_presentation) {
        ParallelScheduler chunks{"Independent world admission", 1u, static_cast<std::size_t>(hal::page_granularity)};
        std::atomic<u32> calls{};
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                ++calls;
                return std::make_unique<ColonyDriver>();
            }};
        PPR_DEFER {
            PPR_TEST_ASSERT(not job.shutdown());
        };

        PPR_TEST_ASSERT(not job.request({.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(false));
        PPR_TEST_ASSERT(calls == 0u and job.view().m_state == EWorldJobState::queued);

        do {
            PPR_TEST_ASSERT(not job.update(false, true, true));
            std::this_thread::yield();
        } while (job.view().m_state != EWorldJobState::prepared);

        PPR_TEST_ASSERT(calls == 1u and job.preparedDriver() != nullptr);

        job.cancel();
        while (job.busy()) {
            PPR_TEST_ASSERT(not job.update(false, true, true));
            std::this_thread::yield();
        }

        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
    };

    PPR_UNIT_TEST (postcommit_presentation_failure_retains_live_driver_and_header_until_retirement) {
        ParallelScheduler chunks{"Published presentation failure", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &, config::Diagnostics &)
                -> Expected<std::unique_ptr<ColonyDriver>> { return std::make_unique<ColonyDriver>(); }};
        std::unique_ptr<ColonyDriver> published;
        PPR_DEFER {
            if (published) {
                PPR_TEST_ASSERT(not published->shutdown());
            }

            PPR_TEST_ASSERT(not job.shutdown());
        };

        PPR_TEST_ASSERT(not job.request({.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(false, true, true));
        settle(job);

        auto *const candidate = job.preparedDriver();
        auto *const presentation = &job.presentation();
        presentation->m_diagnostic_palette.resize(65536u);

        job.commitPublication();
        published = job.publish();

        const auto cause = std::make_error_code(std::errc::io_error);
        PPR_TEST_ASSERT(job.presentationFailed(cause) == cause);
        job.cancel();
        PPR_TEST_ASSERT(not job.update(false, false, true));

        PPR_TEST_ASSERT(published.get() == candidate and job.preparedDriver() == nullptr);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::prepared and job.view().m_error == cause);
        PPR_TEST_ASSERT(&job.presentation() == presentation and presentation->m_diagnostic_palette.size() == 65536u);

        // The real transport has now settled. Retire only presentation storage;
        // the published simulation remains owned by the host.
        job.retire({});
        settle(job);
        PPR_TEST_ASSERT(not job.busy() and job.view().m_state == EWorldJobState::failed);
        PPR_TEST_ASSERT(job.view().m_error == cause and published.get() == candidate);
    };

    PPR_UNIT_TEST (failure_is_retained_and_ready_candidate_can_still_be_cancelled) {
        ParallelScheduler chunks{"World failure test chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        bool fail = true;
        WorldJob job{
            chunks,
            [&](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &diagnostics) -> Expected<std::unique_ptr<ColonyDriver>> {
                if (fail) {
                    diagnostics.add(
                        "materials.toml", "terrain.albedo", "Missing authored texture",
                        std::make_error_code(std::errc::io_error));
                    return std::unexpected{std::make_error_code(std::errc::io_error)};
                }
                return std::make_unique<ColonyDriver>();
            }};

        PPR_TEST_ASSERT(not job.request(ColonyDriverDesc{.m_world_generation = 1u}));
        PPR_TEST_ASSERT(not job.update(true));
        settle(job);
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::failed);
        PPR_TEST_ASSERT(job.view().m_error == std::errc::io_error);
        PPR_TEST_ASSERT(job.diagnosticText().find("materials.toml") != std::string_view::npos);
        PPR_TEST_ASSERT(job.diagnosticText().find("terrain.albedo") != std::string_view::npos);

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
            detail::world_job::authored_fixture_replays_owned_terrain_and_marks_every_chunk_dirty,
            detail::world_job::authored_fixture_cancelled_generation_returns_no_owner_or_completed_chunks,
            detail::world_job::owned_request_override_survives_cancel_and_default_policy_is_unchanged,
            detail::world_job::queued_override_payload_is_released_after_coordinator_retirement,
            detail::world_job::loading_presentation_gates_admission_and_cancel_is_reusable,
            detail::world_job::cancelled_header_retains_catalog_until_external_release,
            detail::world_job::gated_worker_keeps_control_polling_and_cancellation_live,
            detail::world_job::frozen_capture_returns_same_owner_after_queued_cancel,
            detail::world_job::preview_failure_and_queued_cancel_return_frozen_owner,
            detail::world_job::frozen_capture_gate_preserves_exclusivity_and_returns_owner_on_failure,
            detail::world_job::frozen_capture_success_and_prepared_cancel_have_consistent_outcomes,
            detail::world_job::cancel_during_real_physics_preparation_discards_candidate,
            detail::world_job::failure_is_retained_and_ready_candidate_can_still_be_cancelled,
            detail::world_job::candidate_graphics_failure_retires_cpu_owner_and_preserves_failure,
            detail::world_job::independent_work_admission_prepares_without_loading_presentation,
            detail::world_job::postcommit_presentation_failure_retains_live_driver_and_header_until_retirement,
        });
    };
    const UnitTest &gameWorldJobTests() noexcept {
        return world_jobs;
    }
}

