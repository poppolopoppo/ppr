module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.asset;
import engine.core;
import std;

namespace pP::tests::detail::Preparation {
    // ------------------------------------------------------------------
    // owned nonresident processor fixture
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(AssetPreparationTests, verbose, none)

    struct TaggedBadAlloc final : std::bad_alloc {
        u32 m_tag{};

        explicit TaggedBadAlloc(const u32 tag) noexcept : m_tag(tag) {
        }
    };

    struct Control {
        std::weak_ptr<const asset::PackSet> m_packs{};
        std::size_t m_finish_owners{}, m_finish_item_owners{}, m_deferred_calls{};
        std::array<std::atomic<u32>, 4u> m_calls{};
        std::array<u32, 4u> m_failed_advances{};
        std::atomic<u32> m_active{}, m_peak{};
        std::array<std::atomic<bool>, 4u> m_allowed{}, m_started{}, m_finished{};
        bool m_first_advanced{}, m_finish_called{}, m_fatal_finish{};

        Control() {
            for (auto &allowed: m_allowed) {
                allowed.store(true);
            }
        }

        void release() noexcept {
            for (auto &allowed: m_allowed) {
                allowed.store(true);
            }
        }
    };

    struct StreamProcessor {
        struct Item {
            std::filesystem::path m_path{};
            std::shared_ptr<Control> m_control{};
            std::unique_ptr<std::ifstream> m_source{};
            std::size_t m_index{}, m_phase{};
            u32 m_fatal_tag{}, m_failed_advance_tag{}, m_notes_per_phase{1u};
            std::errc m_ordinary_error{std::errc::invalid_argument};
            bool m_ordinary{}, m_fatal{}, m_throw_advance{}, m_defer_forever{}, m_wait_first{}, m_fail_again{},
                m_emit_notes{};
        };

        struct Result {
            Array<std::unique_ptr<std::ifstream>> m_sources{};

            Result() = default;
            Result(Result &&) = default;
            Result &operator=(Result &&) = default;

            Result(const Result &) = delete;
            Result &operator=(const Result &) = delete;
        };

        static constexpr std::size_t maximum_active = 2u;
        static constexpr u32 processing_revision = 3u;

        Array<std::shared_ptr<Item>> m_inputs{};
        std::shared_ptr<Control> m_control{};

        [[nodiscard]] Expected<Array<std::shared_ptr<Item>>> prepare(
            const asset::PackSet &, const IContext &, config::Diagnostics &) {
            return std::move(m_inputs);
        }

        [[nodiscard]] static std::error_code process(
            Item &item, const asset::PackSet &, const IContext &context, config::Diagnostics &diagnostics) {
            auto &control = *item.m_control;
            const auto active = control.m_active.fetch_add(1u) + 1u;
            auto peak = control.m_peak.load();
            while (peak < active and not control.m_peak.compare_exchange_weak(peak, active)) {
            }

            PPR_DEFER {
                control.m_active.fetch_sub(1u);
            };

            control.m_started[item.m_index].store(true);
            control.m_calls[item.m_index].fetch_add(1u);

            while (not control.m_allowed[item.m_index].load() and not context.error()) {
                std::this_thread::yield();
            }

            if (item.m_fatal) {
                control.m_finished[item.m_index].store(true);
                throw std::bad_alloc{};
            }

            if (item.m_fatal_tag != 0u) {
                control.m_finished[item.m_index].store(true);
                throw TaggedBadAlloc{item.m_fatal_tag};
            }

            if (item.m_ordinary) {
                const auto error = std::make_error_code(item.m_ordinary_error);
                diagnostics.add("owned stream " + std::to_string(item.m_index), "source", "Rejected stream", error);
                control.m_finished[item.m_index].store(true);
                PPR_RETURN_ERROR_ON_FAIL(AssetPreparationTests, error);
            }

            PPR_RETURN_ERROR_ON_FAIL(AssetPreparationTests, context.error());

            if (item.m_phase == 0u) {
                item.m_source = std::make_unique<std::ifstream>(item.m_path, std::ios::binary);
                PPR_RETURN_ERROR_ON_FAIL(
                    AssetPreparationTests,
                    not *item.m_source ? std::make_error_code(std::errc::io_error) : std::error_code{});
            }

            if (item.m_emit_notes) {
                for (u32 note = 0u; note < item.m_notes_per_phase; ++note) {
                    diagnostics.add(
                        "owned stream " + std::to_string(item.m_index),
                        "phase." + std::to_string(item.m_phase) + ".note." + std::to_string(note),
                        "Prepared owned stream phase", {});
                }
            }

            ++item.m_phase;
            control.m_finished[item.m_index].store(true);
            return {};
        }

        [[nodiscard]] Expected<asset::EPreparationStep> advance(
            const std::size_t index, Item &item, const bool succeeded, config::Diagnostics &) {
            if (not succeeded) {
                ++m_control->m_failed_advances[index];

                if (item.m_failed_advance_tag != 0u) {
                    throw TaggedBadAlloc{item.m_failed_advance_tag};
                }

                return item.m_fail_again ? asset::EPreparationStep::again : asset::EPreparationStep::complete;
            }

            if (item.m_throw_advance) {
                throw std::system_error(std::make_error_code(std::errc::permission_denied));
            }

            if (item.m_defer_forever or (item.m_wait_first and not m_control->m_first_advanced)) {
                ++m_control->m_deferred_calls;
                return asset::EPreparationStep::defer;
            }

            if (index == 0u) {
                m_control->m_first_advanced = true;
            }

            return item.m_phase == 1u ? asset::EPreparationStep::again : asset::EPreparationStep::complete;
        }

        [[nodiscard]] Expected<Result> finish(
            const std::span<const std::shared_ptr<Item>> items, config::Diagnostics &) {
            m_control->m_finish_called = true;
            m_control->m_finish_owners = safe_narrowing{m_control->m_packs.use_count()};
            if (m_control->m_fatal_finish) {
                throw std::bad_alloc{};
            }

            Result result;

            for (const auto &item: items) {
                m_control->m_finish_item_owners = (std::max) (m_control->m_finish_item_owners,
                    static_cast<std::size_t>(item.use_count()));

                result.m_sources.push_back(std::move(item->m_source));
            }

            return result;
        }
    };

    using Request = asset::PreparationRequest<StreamProcessor>;
    static_assert(not std::is_copy_constructible_v<Request::Result>);
    static_assert(std::is_move_constructible_v<Request::Result>);
    static_assert(not std::is_move_assignable_v<Request>);

    struct Fixture {
        [[nodiscard]] static std::size_t queueBytes() noexcept {
            return static_cast<std::size_t>(hal::page_granularity);
        }

        ParallelScheduler m_pool{"Asset preparation fixture", 2u, queueBytes()};
        std::filesystem::path m_root{}, m_source{};
        std::shared_ptr<Control> m_control{std::make_shared<Control>()};

        Fixture() {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();

            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto path = parent / ("asset-request-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(path)) {
                    m_root = path;
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            m_source = m_root / "stream.bin";
            std::ofstream output{m_source, std::ios::binary};
            output << "owned nonresident source";
            PPR_TEST_ASSERT(static_cast<bool>(output));
        }

        ~Fixture() {
            // Best-effort fixture teardown after request and stream owners drain.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            PPR_LOG_WARNING_ON_FAIL(AssetPreparationTests, error);
        }

        [[nodiscard]] std::shared_ptr<const asset::PackSet> packs(config::Diagnostics &diagnostics) {
            asset::PackSource source{m_root, {m_root / "pack.toml", "schema_version = 1\npack_id = \"ppr.streams\"\n"}};
            auto indexed = asset::PackSet::index({std::move(source)}, {}, diagnostics);
            PPR_TEST_ASSERT(indexed.has_value());

            auto result = std::make_shared<const asset::PackSet>(std::move(*indexed));
            m_control->m_packs = result;
            return result;
        }

        [[nodiscard]] StreamProcessor processor(const std::size_t count) const {
            StreamProcessor result;
            result.m_control = m_control;

            for (std::size_t index = 0u; index < count; ++index) {
                auto item = std::make_shared<StreamProcessor::Item>();
                item->m_path = m_source;
                item->m_control = m_control;
                item->m_index = index;
                result.m_inputs.push_back(std::move(item));
            }

            return result;
        }
    };

    void settle(Request &request) {
        while (not request.poll()) {
            std::this_thread::yield();
        }
    }

    // ------------------------------------------------------------------
    // shared lifecycle with a non-image owning result
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (move_only_nonresident_result_outlives_request_and_worker_captures) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto packs = fixture.packs(diagnostics);
        std::optional<Request::Result> result;

        {
            auto request = Request::start(
                fixture.m_pool, packs, fixture.processor(3u), asset::RequestGeneration{17u}, context::background(),
                diagnostics);
            PPR_TEST_ASSERT(request.has_value());
            packs.reset();

            auto prepared = request->finish(diagnostics);
            PPR_TEST_ASSERT(prepared.has_value());
            // Only the request's pinned arrays retain packs/items here; all
            // accepted callable captures have already been destroyed.
            PPR_TEST_ASSERT(fixture.m_control->m_finish_called and fixture.m_control->m_finish_owners == 1u);
            PPR_TEST_ASSERT(fixture.m_control->m_finish_item_owners == 1u);
            result.emplace(std::move(*prepared));
        }

        PPR_TEST_ASSERT(
            *result->m_generation == 17u and result->m_processing_revision == StreamProcessor::processing_revision);
        PPR_TEST_ASSERT(result->m_value.m_sources.size() == 3u and result->m_packs);

        const auto scratch = mem::ScratchPad::open();
        Array<std::byte, mem::ScratchPad> reuse(4096u, std::byte{0x7f});

        std::string bytes;
        std::getline(*result->m_value.m_sources.front(), bytes);
        PPR_TEST_ASSERT(bytes == "owned nonresident source");
        PPR_TEST_ASSERT(fixture.m_control->m_peak.load() <= StreamProcessor::maximum_active);
    };

    PPR_UNIT_TEST (deferred_continuation_holds_slot_and_retries_only_after_progress) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(4u);
        processor.m_inputs[1u]->m_wait_first = true;
        const bool concurrent = fixture.m_pool.getNumWorkers() > 1u;
        fixture.m_control->m_allowed[0u].store(not concurrent);
        PPR_DEFER {
            fixture.m_control->release();
        };

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{1u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 2u and request->progress().m_active == 2u);

        if (concurrent) {
            while (fixture.m_control->m_deferred_calls == 0u) {
                std::ignore = request->poll();
                std::this_thread::yield();
            }

            const auto attempts = fixture.m_control->m_deferred_calls;
            std::ignore = request->poll();
            std::ignore = request->poll();
            PPR_TEST_ASSERT(fixture.m_control->m_deferred_calls == attempts);
            PPR_TEST_ASSERT(not fixture.m_control->m_started[2u].load());
        }

        fixture.m_control->release();
        const auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value() and result->m_value.m_sources.size() == 4u);
        PPR_TEST_ASSERT(fixture.m_control->m_calls[1u].load() == 2u);
    };

    PPR_UNIT_TEST (cancel_after_partial_admission_drains_without_publishing_result) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        fixture.m_control->m_allowed[0u].store(false);
        fixture.m_control->m_allowed[1u].store(false);
        PPR_DEFER {
            fixture.m_control->release();
        };

        auto request = Request::start(
            fixture.m_pool, packs, fixture.processor(4u), asset::RequestGeneration{2u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 2u);

        request->cancel();
        settle(*request);

        const auto progress = request->progress();
        PPR_TEST_ASSERT(progress.m_active == 0u and progress.m_completed == progress.m_admitted);
        PPR_TEST_ASSERT(not fixture.m_control->m_started[2u].load() and not fixture.m_control->m_finish_called);

        const auto canceled = request->take(diagnostics);
        PPR_TEST_ASSERT(not canceled and canceled.error() == std::errc::operation_canceled);
    };

    PPR_UNIT_TEST (fatal_worker_outcome_wins_earlier_error_and_cancel_then_is_consumed) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(4u);
        processor.m_inputs[0u]->m_ordinary = true;
        processor.m_inputs[1u]->m_fatal = true;
        fixture.m_control->m_allowed[1u].store(false);
        PPR_DEFER {
            fixture.m_control->release();
        };

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{3u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());
        while (not fixture.m_control->m_finished[0u].load()) {
            std::this_thread::yield();
        }

        std::ignore = request->poll();
        request->cancel();
        bool fatal{};
        try {
            std::ignore = request->finish(diagnostics);
        } catch (const std::bad_alloc &) {
            fatal = true;
        }

        PPR_TEST_ASSERT(fatal and fixture.m_control->m_active.load() == 0u);
        PPR_TEST_ASSERT(fixture.m_control->m_finished[1u].load() and not fixture.m_control->m_finish_called);
        PPR_TEST_ASSERT(request->poll());
        const auto consumed = request->take(diagnostics);
        PPR_TEST_ASSERT(not consumed and consumed.error() == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (settled_failures_publish_all_diagnostics_in_input_order_before_first_error) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(2u);
        processor.m_inputs[0u]->m_ordinary = true;
        processor.m_inputs[1u]->m_ordinary = true;
        processor.m_inputs[1u]->m_ordinary_error = std::errc::permission_denied;
        fixture.m_control->m_allowed[0u].store(false);
        PPR_DEFER {
            fixture.m_control->release();
        };

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{12u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 2u);

        while (not fixture.m_control->m_finished[1u].load()) {
            std::this_thread::yield();
        }

        fixture.m_control->release();
        settle(*request);
        const auto progress = request->progress();
        PPR_TEST_ASSERT(progress.m_completed == 2u and progress.m_active == 0u);

        const auto result = request->take(diagnostics);
        PPR_TEST_ASSERT(not result and result.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(diagnostics.size() == 2u and diagnostics.entries().size() == 2u);

        PPR_TEST_ASSERT(diagnostics.entries()[0u].m_location == "owned stream 0");
        PPR_TEST_ASSERT(diagnostics.entries()[0u].m_code == std::errc::invalid_argument);

        PPR_TEST_ASSERT(diagnostics.entries()[1u].m_location == "owned stream 1");
        PPR_TEST_ASSERT(diagnostics.entries()[1u].m_code == std::errc::permission_denied);

        PPR_TEST_ASSERT(not fixture.m_control->m_finish_called and fixture.m_control->m_active.load() == 0u);
    };

    PPR_UNIT_TEST (first_allocation_failure_survives_failed_step_bookkeeping_exception) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(1u);
        processor.m_inputs.front()->m_fatal_tag = 1u;
        processor.m_inputs.front()->m_failed_advance_tag = 2u;

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{10u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        packs.reset();

        u32 reported_tag{};
        try {
            std::ignore = request->finish(diagnostics);
        } catch (const TaggedBadAlloc &failure) {
            reported_tag = failure.m_tag;
        }

        PPR_TEST_ASSERT(reported_tag == 1u and fixture.m_control->m_failed_advances[0u] == 1u);
        PPR_TEST_ASSERT(fixture.m_control->m_active.load() == 0u and fixture.m_control->m_finished[0u].load());
        PPR_TEST_ASSERT(fixture.m_control->m_packs.expired() and not fixture.m_control->m_finish_called);
        PPR_TEST_ASSERT(request->poll());

        const auto consumed = request->take(diagnostics);
        PPR_TEST_ASSERT(not consumed and consumed.error() == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (owner_exception_is_attached_to_item_and_defer_deadlock_is_reported) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(2u);
        processor.m_inputs[0u]->m_ordinary = true;
        processor.m_inputs[1u]->m_throw_advance = true;
        processor.m_inputs[0u]->m_fail_again = true;

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{4u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        const auto rejected = request->finish(diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_location == "owned stream 0");
        PPR_TEST_ASSERT(fixture.m_control->m_calls[0u].load() == 1u);

        auto stranded = fixture.processor(3u);
        stranded.m_inputs[1u]->m_defer_forever = true;
        auto deferred = Request::start(
            fixture.m_pool, packs, std::move(stranded), asset::RequestGeneration{5u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(deferred.has_value());
        const auto no_progress = deferred->finish(diagnostics);
        PPR_TEST_ASSERT(not no_progress and no_progress.error() == std::errc::state_not_recoverable);
        PPR_TEST_ASSERT(not fixture.m_control->m_finish_called);
        PPR_TEST_ASSERT(not fixture.m_control->m_started[2u].load());
    };

    PPR_UNIT_TEST (result_construction_failure_is_consumed_after_capture_drain) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto packs = fixture.packs(diagnostics);
        fixture.m_control->m_fatal_finish = true;
        auto request = Request::start(
            fixture.m_pool, packs, fixture.processor(2u), asset::RequestGeneration{9u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        packs.reset();

        bool fatal{};
        try {
            std::ignore = request->finish(diagnostics);
        } catch (const std::bad_alloc &) {
            fatal = true;
        }

        PPR_TEST_ASSERT(fatal and fixture.m_control->m_finish_owners == 1u);
        PPR_TEST_ASSERT(fixture.m_control->m_active.load() == 0u and request->poll());
        const auto consumed = request->take(diagnostics);
        PPR_TEST_ASSERT(not consumed and consumed.error() == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (queue_full_poll_retries_without_blocking_or_admitting_extra_items) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        std::optional<Request> request;
        const auto release = std::make_shared<std::atomic<bool>>(false);
        const auto started = std::make_shared<std::atomic<u32>>(0u);
        WaitGroup blockers;
        PPR_DEFER {
            release->store(true);
            std::ignore = blockers.wait();
        };

        const auto fill_queues = [&] {
            started->store(0u);
            PPR_TEST_ASSERT(not fixture.m_pool.broadcast(
                [release, started](const IContext &) {
                    started->fetch_add(1u);

                    while (not release->load()) {
                        std::this_thread::yield();
                    }
                },
                blockers));

            while (started->load() != fixture.m_pool.getNumWorkers()) {
                std::this_thread::yield();
            }

            Array<std::future<bool>> fillers;
            bool full{};
            // Every queued record occupies at least one byte; this bound can
            // exhaust both queues without assuming a payload's physical size.
            const auto maximum_admissions = fixture.m_pool.getNumWorkers() * Fixture::queueBytes() + 1u;

            for (std::size_t index = 0u; index < maximum_admissions; ++index) {
                auto queued = fixture.m_pool.schedule(
                    [release](const IContext &) {
                        while (not release->load()) {
                            std::this_thread::yield();
                        }

                        return true;
                    },
                    std::nullopt, ParallelScheduler::drop_if_full);
                if (not queued) {
                    PPR_TEST_ASSERT(queued.error() == make_error_code(RawChannel::error_full));
                    full = true;
                    break;
                }

                fillers.push_back(std::move(*queued));
            }

            PPR_TEST_ASSERT(full);
            return fillers;
        };

        auto fillers = fill_queues();
        auto processor = fixture.processor(1u);
        processor.m_inputs.front()->m_emit_notes = true;

        auto prepared = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{6u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(prepared.has_value());
        request.emplace(std::move(*prepared));

        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 0u and request->progress().m_active == 0u);
        release->store(true);

        for (auto &filler: fillers) {
            PPR_TEST_ASSERT(filler.get());
        }

        PPR_TEST_ASSERT(not blockers.wait());
        PPR_TEST_ASSERT(not request->poll());
        while (not fixture.m_control->m_finished[0u].load()) {
            std::this_thread::yield();
        }

        // Keep the completed first step unharvested until both workers are
        // blocked again; its continuation must retry without losing that step.
        release->store(false);
        fillers = fill_queues();
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(fixture.m_control->m_calls[0u].load() == 1u);
        PPR_TEST_ASSERT(request->progress().m_admitted == 1u and request->progress().m_active == 1u);

        release->store(true);
        const auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value() and result->m_value.m_sources.size() == 1u);
        PPR_TEST_ASSERT(fixture.m_control->m_calls[0u].load() == 2u);
        PPR_TEST_ASSERT(diagnostics.size() == 2u and diagnostics.omittedCount() == 0u);
        PPR_TEST_ASSERT(diagnostics.entries()[0u].m_property == "phase.0.note.0");
        PPR_TEST_ASSERT(diagnostics.entries()[1u].m_property == "phase.1.note.0");

        for (auto &filler: fillers) {
            PPR_TEST_ASSERT(filler.get());
        }
    };

    PPR_UNIT_TEST (step_diagnostics_accumulate_in_order_with_bounded_retention) {
        Fixture fixture;
        config::Diagnostics diagnostics{64u};
        const auto packs = fixture.packs(diagnostics);
        auto processor = fixture.processor(1u);
        processor.m_inputs.front()->m_emit_notes = true;
        processor.m_inputs.front()->m_notes_per_phase = 20u;

        auto request = Request::start(
            fixture.m_pool, packs, std::move(processor), asset::RequestGeneration{11u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        const auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value() and result->m_value.m_sources.size() == 1u);

        // Each step and the owner collector retain sixteen records, while the
        // complete count includes every omitted record across both steps.
        PPR_TEST_ASSERT(diagnostics.size() == 40u and diagnostics.entries().size() == 16u);
        PPR_TEST_ASSERT(diagnostics.omittedCount() == 24u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "phase.0.note.0");
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "phase.0.note.15");
        PPR_TEST_ASSERT(fixture.m_control->m_calls[0u].load() == 2u);
    };

    PPR_UNIT_TEST (closed_scheduler_after_accepted_steps_rejects_continuation_and_drains) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        auto request = Request::start(
            fixture.m_pool, packs, fixture.processor(4u), asset::RequestGeneration{7u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 2u);

        PPR_TEST_ASSERT(not fixture.m_pool.join());
        const auto rejected = request->finish(diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == make_error_code(RawChannel::error_closed));
        PPR_TEST_ASSERT(not fixture.m_control->m_started[2u].load() and not fixture.m_control->m_finish_called);
        PPR_TEST_ASSERT(fixture.m_control->m_active.load() == 0u);
    };

    PPR_UNIT_TEST (external_destructor_cancels_and_drains_accepted_jobs) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto packs = fixture.packs(diagnostics);
        fixture.m_control->m_allowed[0u].store(false);
        fixture.m_control->m_allowed[1u].store(false);

        {
            auto request = Request::start(
                fixture.m_pool, packs, fixture.processor(4u), asset::RequestGeneration{8u}, context::background(),
                diagnostics);
            PPR_TEST_ASSERT(request.has_value());
            PPR_TEST_ASSERT(not request->poll());
            PPR_TEST_ASSERT(request->progress().m_admitted == 2u);
        }

        PPR_TEST_ASSERT(fixture.m_control->m_active.load() == 0u);
        PPR_TEST_ASSERT(fixture.m_control->m_calls[0u].load() == 1u and fixture.m_control->m_calls[1u].load() == 1u);
        PPR_TEST_ASSERT(not fixture.m_control->m_started[2u].load() and not fixture.m_control->m_finish_called);
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // shared request suite registration
    // ------------------------------------------------------------------

    const UnitTest preparation = UnitTest::Named("preparation") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Preparation::move_only_nonresident_result_outlives_request_and_worker_captures,
            detail::Preparation::deferred_continuation_holds_slot_and_retries_only_after_progress,
            detail::Preparation::cancel_after_partial_admission_drains_without_publishing_result,
            detail::Preparation::fatal_worker_outcome_wins_earlier_error_and_cancel_then_is_consumed,
            detail::Preparation::settled_failures_publish_all_diagnostics_in_input_order_before_first_error,
            detail::Preparation::first_allocation_failure_survives_failed_step_bookkeeping_exception,
            detail::Preparation::owner_exception_is_attached_to_item_and_defer_deadlock_is_reported,
            detail::Preparation::result_construction_failure_is_consumed_after_capture_drain,
            detail::Preparation::queue_full_poll_retries_without_blocking_or_admitting_extra_items,
            detail::Preparation::step_diagnostics_accumulate_in_order_with_bounded_retention,
            detail::Preparation::closed_scheduler_after_accepted_steps_rejects_continuation_and_drains,
            detail::Preparation::external_destructor_cancels_and_drains_accepted_jobs,
        });
    };

    const UnitTest &preparationTests() noexcept {
        return preparation;
    }
}
