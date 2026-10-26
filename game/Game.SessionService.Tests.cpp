module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import game.application;
import game.colony.driver;
import game.colony.matter;
import game.colony.panel;
import game.ui;
import engine.app;
import engine.core;
import engine.sim;
import engine.asset;
import engine.image;
import game.colony.content;
import game.colony.worldgen;
import std;

#include "colony/Colony.AuthoredFixture.Tests.h"

namespace pP::game::tests::detail {
    // ------------------------------------------------------------------
    // owned lifecycle wait fixture
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(SessionWaitFixture, debug, none)

    std::error_code finishOwnedOperation(
        RenderLane &lane, std::unique_ptr<RenderLaneOperation> operation, bool *const out_ran) {
        PPR_TEST_ASSERT(out_ran != nullptr);
        *out_ran = false;

        auto ticket = lane.tryBeginOperation(operation);
        PPR_RETURN_ERROR_ON_FAIL(SessionWaitFixture, ticket);

        auto completion = lane.waitOperation(*ticket);
        PPR_RETURN_ERROR_ON_FAIL(SessionWaitFixture, completion);
        *out_ran = completion->m_ran;

        PPR_RETURN_ERROR_ON_FAIL(
            SessionWaitFixture,
            completion->m_operation_error ? completion->m_operation_error : completion->m_transport_error);
        return {};
    }

    // ------------------------------------------------------------------
    // actual activated-world presentation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (retained_workshop_selection_follows_bundle_authority) {
        constexpr GridWorldIdentity game_world{17u, 7u, 101u};
        GridParticleGpuView final{};
        final.m_world = game_world;
        final.m_device_epoch = 24u;
        final.m_publication = 23u;
        final.m_joint_tick = 22u;
        final.m_live_epoch = std::make_shared<std::atomic<u64>>(24u);
        std::optional<GridParticleGpuView> held{final};

        // Entry retains the final committed identity even before any new terrain
        // frame and independently of the temporarily transferred Driver owner.
        const auto selected = selectWorkshopRetainedParticles(held, game_world, true, true, false, false, false);
        PPR_TEST_ASSERT(selected and selected->m_live_epoch == final.m_live_epoch);
        PPR_TEST_ASSERT(
            selected->m_device_epoch == 24u and selected->m_publication == 23u and selected->m_joint_tick == 22u);

        // Activation admission removes authority before its asynchronous ACK;
        // selected previews and Return use their own independently acknowledged view.
        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, true, true, false, false, true));
        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, true, true, true, false, false));
        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, true, true, false, true, false));

        for (const auto other: std::array<GridWorldIdentity, 3u>{{{18u, 7u, 101u}, {17u, 8u, 101u}, {17u, 7u, 102u}}}) {
            PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, other, true, true, false, false, false));
        }

        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, false, true, false, false, false));
        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, true, false, false, false, false));
        held.reset();
        PPR_TEST_ASSERT(not selectWorkshopRetainedParticles(held, game_world, true, true, false, false, false));
        PPR_TEST_ASSERT(selected->m_live_epoch->load(std::memory_order_acquire) == 24u);
    };

    PPR_UNIT_TEST (retained_presentation_matches_current_activation_without_consuming_staging) {
        constexpr GridWorldIdentity requested{17u, 7u, 101u};
        constexpr u64 epoch = 23u;
        RenderFrameResult retained{};
        retained.m_presented_grid = requested;
        retained.m_reservation_epoch = epoch;
        retained.m_presented = true;
        retained.m_grid_reset_caches = true;
        retained.m_grid_full_refresh = true;
        retained.m_grid_ack_seed = 7u;
        retained.m_grid_ack_chunk_ids.push_back(31u);
        retained.m_grid_ack_revisions.push_back(103u);

        SessionService service;
        PPR_TEST_ASSERT(not service.post(retained));

        auto acknowledgments = service.takeAcknowledgments();
        PPR_TEST_ASSERT(acknowledgments.size() == 1u);

        const auto &delivered = std::get<RenderFrameResult>(acknowledgments[0u]);
        PPR_TEST_ASSERT(hasSessionWorldPresentation(delivered, epoch, requested, true, true));
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(delivered, epoch, requested, false, true));
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(delivered, epoch, requested, true, false));

        // Visibility recognition must leave the independent edit manifest intact.
        PPR_TEST_ASSERT(not delivered.m_grid_stage_applied and not delivered.m_grid_staging_applied);
        PPR_TEST_ASSERT(delivered.m_grid_full_refresh and delivered.m_grid_reset_caches);
        PPR_TEST_ASSERT(delivered.m_grid_ack_seed == 7u);
        PPR_TEST_ASSERT(delivered.m_grid_ack_chunk_ids.size() == 1u and delivered.m_grid_ack_chunk_ids[0u] == 31u);
        PPR_TEST_ASSERT(delivered.m_grid_ack_revisions.size() == 1u and delivered.m_grid_ack_revisions[0u] == 103u);

        const auto rejects = [&](const RenderFrameResult &result) {
            PPR_TEST_ASSERT(not hasSessionWorldPresentation(result, epoch, requested, true, true));
        };

        auto stale = retained;
        stale.m_reservation_epoch = epoch - 1u;
        rejects(stale);

        stale.m_reservation_epoch = 0u;
        rejects(stale);

        stale = retained;
        --stale.m_presented_grid.m_job_id;
        rejects(stale); // Preview jobs reuse the same generation.

        stale = retained;
        --stale.m_presented_grid.m_world_generation;
        rejects(stale);

        stale = retained;
        --stale.m_presented_grid.m_content_fingerprint;
        rejects(stale);

        stale = retained;
        stale.m_presented = false;
        stale.m_submit_value = 11u;
        rejects(stale); // Queue submission alone proves no presentation.

        stale = retained;
        stale.m_error = std::make_error_code(std::errc::io_error);
        rejects(stale);

        stale = retained;
        stale.m_dropped = true;
        rejects(stale);

        stale = retained;
        stale.m_presented_grid = {};
        rejects(stale); // Empty/obsolete/minimized/control frames have no world proof.

        PPR_TEST_ASSERT(not hasSessionWorldPresentation(retained, epoch, {}, true, true));

        // A preparation frame delivered after host publication still cannot
        // release resident readiness. Only its exact joint publication can.
        constexpr GridParticlePresentationIdentity resident{requested, 5u, 3u, 2u};
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(retained, epoch, requested, true, true, resident));

        auto resident_frame = retained;
        resident_frame.m_presented_particles = resident;
        PPR_TEST_ASSERT(hasSessionWorldPresentation(resident_frame, epoch, requested, true, true, resident));

        auto old_resident = resident_frame;
        --old_resident.m_presented_particles->m_publication;
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(old_resident, epoch, requested, true, true, resident));

        old_resident = resident_frame;
        --old_resident.m_presented_particles->m_terrain_revision;
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(old_resident, epoch, requested, true, true, resident));

        old_resident = resident_frame;
        --old_resident.m_presented_particles->m_joint_tick;
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(old_resident, epoch, requested, true, true, resident));

        old_resident = resident_frame;
        --old_resident.m_presented_particles->m_world.m_job_id;
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(old_resident, epoch, requested, true, true, resident));

        resident_frame.m_presented = false;
        PPR_TEST_ASSERT(not hasSessionWorldPresentation(resident_frame, epoch, requested, true, true, resident));

        // Exercise the production pending-return state, not a parallel matcher.
        colony::MatterRuntime runtime;
        SessionWorldTransition returning{};
        returning.m_expected = resident;
        returning.m_reservation_epoch = epoch;
        returning.m_phase = ESessionWorldPhase::awaiting_presentation;
        returning.m_returning = true;

        const auto observe = [&](const RenderFrameResult &frame) {
            const auto step = advanceSessionWorldTransition(returning, nullptr, nullptr, runtime, {.m_frame = &frame});
            PPR_TEST_ASSERT(step.has_value());
            return *step;
        };

        resident_frame.m_presented = true;
        const auto pending = [&](const RenderFrameResult &frame) {
            PPR_TEST_ASSERT(observe(frame) == ESessionWorldStep::wait);
            PPR_TEST_ASSERT(returning.m_phase == ESessionWorldPhase::awaiting_presentation);
        };

        stale = resident_frame;
        --stale.m_reservation_epoch;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_grid.m_job_id;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_grid.m_world_generation;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_grid.m_content_fingerprint;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_particles->m_publication;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_particles->m_terrain_revision;
        pending(stale);

        stale = resident_frame;
        --stale.m_presented_particles->m_joint_tick;
        pending(stale);

        stale = resident_frame;
        stale.m_presented_particles.reset();
        pending(stale);

        stale = resident_frame;
        stale.m_presented = false;
        stale.m_submit_value = 91u;
        pending(stale);

        stale = resident_frame;
        stale.m_dropped = true;
        pending(stale);

        stale = resident_frame;
        stale.m_error = std::make_error_code(std::errc::io_error);
        pending(stale);

        const auto activation_only = advanceSessionWorldTransition(
            returning, nullptr, nullptr, runtime, {.m_activated = true});
        PPR_TEST_ASSERT(activation_only and *activation_only == ESessionWorldStep::wait);
        PPR_TEST_ASSERT(observe(resident_frame) == ESessionWorldStep::finish);
        PPR_TEST_ASSERT(returning.m_phase == ESessionWorldPhase::complete);
        PPR_TEST_ASSERT(observe(resident_frame) == ESessionWorldStep::wait);

        std::fprintf(
            stderr, "[presentation-layout] identity=%zu/%zu result=%zu/%zu identity_offset=%td frame_offset=%td\n",
            sizeof(GridWorldIdentity), alignof(GridWorldIdentity), sizeof(RenderFrameResult),
            alignof(RenderFrameResult),
            reinterpret_cast<const std::byte *>(&delivered.m_presented_grid) -
                reinterpret_cast<const std::byte *>(&delivered),
            reinterpret_cast<const std::byte *>(&delivered.m_frame_id) -
                reinterpret_cast<const std::byte *>(&delivered));

        PPR_TEST_ASSERT(not service.close());
    };

    // ------------------------------------------------------------------
    // world activation admission and reversible cancellation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (world_transition_admission_freezes_selection_and_cancellation_cannot_roll_back) {
        colony::MatterRuntime runtime;
        SessionWorldTransition queued{};
        queued.m_phase = ESessionWorldPhase::prepared;
        queued.m_previous = {{71u, 9u, 103u}, true, true, true, true, true};
        const auto retained = queued.m_previous;

        auto step = advanceSessionWorldTransition(queued, nullptr, nullptr, runtime, {.m_cancel = true});
        PPR_TEST_ASSERT(step and *step == ESessionWorldStep::wait);
        PPR_TEST_ASSERT(queued.m_phase == ESessionWorldPhase::cancelling);
        PPR_TEST_ASSERT(canSelectSessionWorldView(queued));

        step = advanceSessionWorldTransition(queued, nullptr, nullptr, runtime, {.m_renderer_idle = true});
        PPR_TEST_ASSERT(step and *step == ESessionWorldStep::release_candidate);
        PPR_TEST_ASSERT(sessionTransitionOwnsDriverService(queued));

        step = advanceSessionWorldTransition(queued, nullptr, nullptr, runtime, {});
        PPR_TEST_ASSERT(step and *step == ESessionWorldStep::restore);
        PPR_TEST_ASSERT(not sessionTransitionOwnsDriverService(queued));
        PPR_TEST_ASSERT(queued.m_previous.m_world == retained.m_world and queued.m_previous.m_paused);
        PPR_TEST_ASSERT(queued.m_previous.m_ready and queued.m_previous.m_presented);

        SessionWorldTransition admitted{};
        admitted.m_phase = ESessionWorldPhase::prepared;
        step = advanceSessionWorldTransition(admitted, nullptr, nullptr, runtime, {.m_activation_admitted = true});
        PPR_TEST_ASSERT(step and *step == ESessionWorldStep::wait);
        PPR_TEST_ASSERT(admitted.m_phase == ESessionWorldPhase::activating);
        PPR_TEST_ASSERT(not canSelectSessionWorldView(admitted));

        step = advanceSessionWorldTransition(
            admitted, nullptr, nullptr, runtime, {.m_cancel = true, .m_renderer_idle = true});
        PPR_TEST_ASSERT(step and *step == ESessionWorldStep::wait);
        PPR_TEST_ASSERT(admitted.m_phase == ESessionWorldPhase::activating and not admitted.m_expected);

        // Malformed post-activation ownership is terminal. The shared advance
        // cannot reinterpret it as a pre-activation restoration of the old world.
        step = advanceSessionWorldTransition(admitted, nullptr, nullptr, runtime, {.m_activated = true});
        PPR_TEST_ASSERT(not step and step.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(admitted.m_phase == ESessionWorldPhase::attaching);
        PPR_TEST_ASSERT(not canSelectSessionWorldView(admitted));

        std::fprintf(
            stderr, "[world-transition-layout] facts=%zu/%zu transition=%zu/%zu input=%zu/%zu\n",
            sizeof(SessionWorldFacts), alignof(SessionWorldFacts), sizeof(SessionWorldTransition),
            alignof(SessionWorldTransition), sizeof(SessionWorldTransitionInput), alignof(SessionWorldTransitionInput));
    };

    // ------------------------------------------------------------------
    // owned acknowledgment extraction
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (pending_owner_extracts_acknowledgments_and_close_without_mutation_overtaking) {
        SessionService service;
        PPR_TEST_ASSERT(not service.post(SessionWorldAction{11u, ui::EWorldAction::cancel}));
        PPR_TEST_ASSERT(not service.post(RenderFrameResult{.m_frame_id = 17u}));
        PPR_TEST_ASSERT(not service.post(SessionWorldAction{12u, ui::EWorldAction::retry}));
        PPR_TEST_ASSERT(not service.post(GridWorldResult{.m_sequence = 19u}));
        service.requestClose();

        PPR_TEST_ASSERT(service.acknowledgmentsPending());
        auto acknowledgments = service.takeAcknowledgments();
        PPR_TEST_ASSERT(acknowledgments.size() == 2u);
        PPR_TEST_ASSERT(std::get<RenderFrameResult>(acknowledgments[0]).m_frame_id == 17u);
        PPR_TEST_ASSERT(std::get<GridWorldResult>(acknowledgments[1]).m_sequence == 19u);
        PPR_TEST_ASSERT(service.takeClose());
        PPR_TEST_ASSERT(not service.takeClose() and not service.acknowledgmentsPending());

        auto batch = service.beginService();
        PPR_TEST_ASSERT(batch and batch->m_deliveries.size() == 2u and not batch->m_close);
        PPR_TEST_ASSERT(std::get<SessionWorldAction>(batch->m_deliveries[0]).m_job == 11u);
        PPR_TEST_ASSERT(std::get<SessionWorldAction>(batch->m_deliveries[1]).m_job == 12u);
        service.endService();
    };

    // ------------------------------------------------------------------
    // real lane-pumped exclusive session delivery
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (lane_pump_defers_owned_ack_and_ordered_controls_until_next_service) {
        SessionService service;
        RenderLane lane;
        std::binary_semaphore release{0};
        bool released = false, pumped = false, reentry_rejected = false;
        PPR_DEFER {
            lane.setWaitPump({});
            if (not released) {
                release.release();
            }

            lane.requestStop();
            PPR_TEST_ASSERT(not lane.join());
        };

        PPR_TEST_ASSERT(not lane.start(
            {}, [](const RenderFrameRequest &, RenderFrameResult &) { return std::error_code{}; }, {}, {}));

        {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.empty() and not batch->m_close);

            lane.setWaitPump([&] {
                if (pumped) {
                    return;
                }

                PPR_DEFER {
                    release.release();
                    released = true;
                };
                RenderFrameResult result{};
                result.m_grid_ack_seed = 7u;
                result.m_grid_ack_chunk_ids.push_back(31u);
                result.m_grid_ack_revisions.push_back(101u);
                PPR_TEST_ASSERT(not service.post(result));
                result.m_grid_ack_chunk_ids[0u] = 99u;

                PPR_TEST_ASSERT(not service.post(SessionWorldAction{9u, ui::EWorldAction::cancel}));
                PPR_TEST_ASSERT(
                    not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::regenerate}}));
                for (u32 repeat = 0u; repeat < 2u; ++repeat) {
                    PPR_TEST_ASSERT(
                        not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::pause_toggle}}));
                    PPR_TEST_ASSERT(not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::step}}));
                }

                service.requestClose();
                reentry_rejected = not service.beginService().has_value();
                pumped = true;
            });

            bool acknowledged = false;
            PPR_TEST_ASSERT(not finishOwnedOperation(
                lane, std::make_unique<RenderLaneOperation>([&] {
                    release.acquire();
                    return std::error_code{};
                }),
                &acknowledged));
            PPR_TEST_ASSERT(acknowledged and pumped and reentry_rejected);
            PPR_TEST_ASSERT(batch->m_deliveries.empty() and not batch->m_close);
        }

        {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_close and batch->m_deliveries.size() == 7u);
            const auto &ack = std::get<RenderFrameResult>(batch->m_deliveries[0u]);
            PPR_TEST_ASSERT(ack.m_grid_ack_seed == 7u and ack.m_grid_ack_chunk_ids[0u] == 31u);
            PPR_TEST_ASSERT(ack.m_grid_ack_revisions[0u] == 101u);
            PPR_TEST_ASSERT(std::get<SessionWorldAction>(batch->m_deliveries[1u]).m_action == ui::EWorldAction::cancel);
            PPR_TEST_ASSERT(
                std::get<SessionColonyIntent>(batch->m_deliveries[2u]).m_intent.m_kind ==
                colony::EColonyIntent::regenerate);
            for (std::size_t row = 3u; row < 7u; row += 2u) {
                PPR_TEST_ASSERT(
                    std::get<SessionColonyIntent>(batch->m_deliveries[row]).m_intent.m_kind ==
                    colony::EColonyIntent::pause_toggle);
                PPR_TEST_ASSERT(
                    std::get<SessionColonyIntent>(batch->m_deliveries[row + 1u]).m_intent.m_kind ==
                    colony::EColonyIntent::step);
            }

            // A callback arriving during delivery remains in the next batch;
            // rejected reentry consumes neither that payload nor native close.
            PPR_TEST_ASSERT(not service.post(SessionWorldAction{10u, ui::EWorldAction::retry}));
            service.requestClose();
            PPR_TEST_ASSERT(not service.beginService());
        }

        {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_close and batch->m_deliveries.size() == 1u);
            PPR_TEST_ASSERT(std::get<SessionWorldAction>(batch->m_deliveries[0u]).m_job == 10u);
        }
    };

    PPR_UNIT_TEST (acknowledged_precommit_error_releases_service_and_preserves_pumped_arrivals) {
        SessionService service;
        RenderLane lane;
        std::binary_semaphore release{0};
        const auto cause = std::make_error_code(std::errc::io_error);
        bool released = false, pumped = false, acknowledged = false;
        PPR_DEFER {
            lane.setWaitPump({});
            if (not released) {
                release.release();
            }

            lane.requestStop();
            const auto joined = lane.join();
            PPR_TEST_ASSERT(not joined or joined == cause);
        };

        PPR_TEST_ASSERT(not lane.start(
            {}, [](const RenderFrameRequest &, RenderFrameResult &) { return std::error_code{}; }, {}, {}));

        lane.setWaitPump([&] {
            if (pumped) {
                return;
            }

            PPR_DEFER {
                release.release();
                released = true;
            };
            RenderFrameResult arrival{};
            arrival.m_grid_ack_chunk_ids.push_back(42u);
            arrival.m_grid_ack_revisions.push_back(117u);
            PPR_TEST_ASSERT(not service.post(arrival));
            service.requestClose();
            PPR_TEST_ASSERT(not service.beginService());
            pumped = true;
        });

        const auto operation = [&]() -> std::error_code {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };

            return finishOwnedOperation(
                lane, std::make_unique<RenderLaneOperation>([&] {
                    release.acquire();
                    return cause; // Acknowledged rejection before any publication.
                }),
                &acknowledged);
        };

        PPR_TEST_ASSERT(operation() == cause);
        PPR_TEST_ASSERT(acknowledged and pumped and not service.active());

        auto batch = service.beginService();
        PPR_TEST_ASSERT(batch.has_value());
        PPR_DEFER {
            service.endService();
        };
        PPR_TEST_ASSERT(batch->m_close and batch->m_deliveries.size() == 1u);
        const auto &arrival = std::get<RenderFrameResult>(batch->m_deliveries[0u]);
        PPR_TEST_ASSERT(arrival.m_grid_ack_chunk_ids[0u] == 42u and arrival.m_grid_ack_revisions[0u] == 117u);
    };

    PPR_UNIT_TEST (blocked_tool_retries_do_not_make_fresh_service_immediately_due) {
        SessionService service;
        const SessionBuildOrder order{
            7u, {.m_min = {1u, 2u}, .m_width = 1u, .m_height = 1u}, colony::EColonyTool::demolish};
        PPR_TEST_ASSERT(not service.deferTool(order));
        PPR_TEST_ASSERT(service.retriesPending() and not service.pending());
        PPR_TEST_ASSERT(not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::pause_toggle}}));
        PPR_TEST_ASSERT(service.pending());

        {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.size() == 2u);
            PPR_TEST_ASSERT(std::holds_alternative<SessionColonyIntent>(batch->m_deliveries[0u]));
            PPR_TEST_ASSERT(std::get<SessionBuildOrder>(batch->m_deliveries[1u]).m_world == 7u);
            PPR_TEST_ASSERT(not service.retriesPending());

            PPR_TEST_ASSERT(not service.deferTool(order));
            PPR_TEST_ASSERT(not service.beginService());
            PPR_TEST_ASSERT(service.retriesPending() and not service.pending());
        }

        PPR_TEST_ASSERT(service.retriesPending() and not service.pending());

        service.stopControls();
        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(not service.close());
    };

    PPR_UNIT_TEST (terminal_control_stop_retains_acknowledgments_until_receiver_close) {
        std::fprintf(
            stderr,
            "[session-layout] owner=%zu/%zu batch=%zu/%zu delivery=%zu/%zu colony=%zu/%zu build=%zu/%zu dig=%zu/%zu world=%zu/%zu retry=%zu/%zu\n",
            sizeof(SessionService), alignof(SessionService), sizeof(SessionServiceBatch), alignof(SessionServiceBatch),
            sizeof(SessionDelivery), alignof(SessionDelivery), sizeof(SessionColonyIntent),
            alignof(SessionColonyIntent), sizeof(SessionBuildOrder), alignof(SessionBuildOrder),
            sizeof(SessionDigOrder), alignof(SessionDigOrder), sizeof(SessionWorldAction), alignof(SessionWorldAction),
            sizeof(SessionToolOrder), alignof(SessionToolOrder));

        SessionService service;
        PPR_TEST_ASSERT(not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::step}}));
        RenderFrameResult result{};
        result.m_grid_ack_chunk_ids.push_back(13u);
        PPR_TEST_ASSERT(not service.post(result));

        service.stopControls();
        PPR_TEST_ASSERT(
            service.post(SessionWorldAction{1u, ui::EWorldAction::cancel}) == std::errc::operation_canceled);
        PPR_TEST_ASSERT(not service.post(GridWorldResult{}));
        PPR_TEST_ASSERT(service.close() == std::errc::device_or_resource_busy);
        {
            auto batch = service.beginService();
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.size() == 2u and not batch->m_close);
            PPR_TEST_ASSERT(std::get<RenderFrameResult>(batch->m_deliveries[0u]).m_grid_ack_chunk_ids[0u] == 13u);
            PPR_TEST_ASSERT(std::holds_alternative<GridWorldResult>(batch->m_deliveries[1u]));
        }

        PPR_TEST_ASSERT(not service.close());
        PPR_TEST_ASSERT(not service.beginService());
        PPR_TEST_ASSERT(service.post(result) == std::errc::operation_canceled);
        service.requestClose();
        PPR_TEST_ASSERT(not service.pending());
    };

    // ------------------------------------------------------------------
    // stale displayed-world tool rejection
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (stale_build_and_dig_orders_never_touch_replacement_driver) {
        colony::test_fixture::AuthoredFixture authored;

        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };

        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const auto before = driver.grid().getCell({32u, 32u});
        PPR_TEST_ASSERT(before.has_value());
        const auto errands = driver.errandCount();
        const auto dug = driver.digTotal();

        const SessionBuildOrder build{
            19u, {.m_min = {32u, 32u}, .m_width = 1u, .m_height = 1u, .m_element = 1u},
            colony::EColonyTool::build_wall};
        const SessionDigOrder dig{19u, {{32u, 32u}, 1u, 1u}};
        const auto stale_build = applySessionBuildOrder(driver, build, 20u);
        const auto stale_dig = applySessionDigOrder(driver, dig, 20u);
        PPR_TEST_ASSERT(stale_build and not stale_build->has_value());
        PPR_TEST_ASSERT(stale_dig and not *stale_dig);

        auto undisplayed_build = build;
        auto undisplayed_dig = dig;
        undisplayed_build.m_world = 20u;
        undisplayed_dig.m_world = 20u;
        const auto hidden_build = applySessionBuildOrder(driver, undisplayed_build, 19u);
        const auto hidden_dig = applySessionDigOrder(driver, undisplayed_dig, 19u);
        PPR_TEST_ASSERT(hidden_build and not hidden_build->has_value());
        PPR_TEST_ASSERT(hidden_dig and not *hidden_dig);
        PPR_TEST_ASSERT(driver.errandCount() == errands and driver.digTotal() == dug);
        PPR_TEST_ASSERT(driver.tickCount() == 0u and driver.captureReady());
        const auto after = driver.grid().getCell({32u, 32u});
        PPR_TEST_ASSERT(
            after and after->m_element == before->m_element and after->m_temperature == before->m_temperature);
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // session-service registration
    // ------------------------------------------------------------------

    const UnitTest session_service = UnitTest::Named("session_service") / [](UnitTest::IRun &_) {
        _.recurse({
            detail::retained_workshop_selection_follows_bundle_authority,
            detail::retained_presentation_matches_current_activation_without_consuming_staging,
            detail::world_transition_admission_freezes_selection_and_cancellation_cannot_roll_back,
            detail::pending_owner_extracts_acknowledgments_and_close_without_mutation_overtaking,
            detail::lane_pump_defers_owned_ack_and_ordered_controls_until_next_service,
            detail::acknowledged_precommit_error_releases_service_and_preserves_pumped_arrivals,
            detail::blocked_tool_retries_do_not_make_fresh_service_immediately_due,
            detail::terminal_control_stop_retains_acknowledgments_until_receiver_close,
            detail::stale_build_and_dig_orders_never_touch_replacement_driver,
        });
    };

    const UnitTest &gameSessionServiceTests() noexcept {
        return session_service;
    }
}
