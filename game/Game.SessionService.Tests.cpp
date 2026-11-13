module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import game.application;
import game.colony.agents;
import game.colony.buildings;
import game.colony.driver;
import game.colony.matter;
import game.colony.metric;
import game.colony.panel;
import game.colony.pathfinding;
import game.colony.tools;
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

    /// Injected service time for the retry budget: tests advance it explicitly
    /// instead of reading a clock, so expiry is deterministic.
    constexpr TimePoint kToolServiceTime{};

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

    PPR_UNIT_TEST (playable_capture_waits_for_first_present_ack_and_closed_progress) {
        constexpr GridWorldIdentity world{17u, 7u, 101u};
        constexpr u64 epoch = 23u;
        SessionWorldFacts facts{world, true, true, false, true, false};
        PPR_TEST_ASSERT(not canCapturePlayableSessionFrame(facts, false, false));

        RenderFrameResult first{};
        first.m_reservation_epoch = epoch;
        first.m_presented_grid = world;
        first.m_presented = true;
        facts.m_presented = hasSessionWorldPresentation(first, epoch, world, facts.m_published, facts.m_activated);
        PPR_TEST_ASSERT(facts.m_presented);
        PPR_TEST_ASSERT(not canCapturePlayableSessionFrame(facts, true, false));

        // The same first-present facts qualify only after the owner actually
        // dismisses progress and successfully builds the playable panel.
        PPR_TEST_ASSERT(canCapturePlayableSessionFrame(facts, false, false));

        auto stale = first;
        --stale.m_presented_grid.m_world_generation;
        facts.m_presented = hasSessionWorldPresentation(stale, epoch, world, facts.m_published, facts.m_activated);
        PPR_TEST_ASSERT(not canCapturePlayableSessionFrame(facts, false, false));
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

        auto batch = service.beginService(kToolServiceTime);
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
            auto batch = service.beginService(kToolServiceTime);
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
                reentry_rejected = not service.beginService(kToolServiceTime).has_value();
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
            auto batch = service.beginService(kToolServiceTime);
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
            PPR_TEST_ASSERT(not service.beginService(kToolServiceTime));
        }

        {
            auto batch = service.beginService(kToolServiceTime);
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
            PPR_TEST_ASSERT(not service.beginService(kToolServiceTime));
            pumped = true;
        });

        const auto operation = [&]() -> std::error_code {
            auto batch = service.beginService(kToolServiceTime);
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

        auto batch = service.beginService(kToolServiceTime);
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
        PPR_TEST_ASSERT(not service.deferTool(order, kToolServiceTime));
        PPR_TEST_ASSERT(service.retriesPending() and not service.pending());
        PPR_TEST_ASSERT(not service.post(SessionColonyIntent{7u, {.m_kind = colony::EColonyIntent::pause_toggle}}));
        PPR_TEST_ASSERT(service.pending());

        {
            auto batch = service.beginService(kToolServiceTime);
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.size() == 2u);
            PPR_TEST_ASSERT(std::holds_alternative<SessionColonyIntent>(batch->m_deliveries[0u]));
            PPR_TEST_ASSERT(std::get<SessionBuildOrder>(batch->m_deliveries[1u]).m_world == 7u);
            // The budget entry stays owned while its order is in flight; only a
            // repeated deferral keeps it past this service.
            PPR_TEST_ASSERT(service.retriesPending());

            PPR_TEST_ASSERT(not service.deferTool(order, kToolServiceTime));
            PPR_TEST_ASSERT(not service.beginService(kToolServiceTime));
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
            auto batch = service.beginService(kToolServiceTime);
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.size() == 2u and not batch->m_close);
            PPR_TEST_ASSERT(std::get<RenderFrameResult>(batch->m_deliveries[0u]).m_grid_ack_chunk_ids[0u] == 13u);
            PPR_TEST_ASSERT(std::holds_alternative<GridWorldResult>(batch->m_deliveries[1u]));
        }

        PPR_TEST_ASSERT(not service.close());
        PPR_TEST_ASSERT(not service.beginService(kToolServiceTime));
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

    PPR_UNIT_TEST (particle_orders_ignore_stale_and_hidden_worlds_and_report_unavailable_runtime) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        driver.setPaused(true);

        const auto before = driver.grid().getCell({32u, 32u});
        PPR_TEST_ASSERT(before.has_value());
        const auto particle_count = driver.particleCount();
        const auto errands = driver.errandCount();
        const auto dug = driver.digTotal();
        for (const auto kind: {colony::EParticleToolKind::spawn, colony::EParticleToolKind::destroy}) {
            SessionParticleOrder order{
                19u,
                {.m_x_m = 12.0, .m_y_m = 12.0, .m_radius_m = 2.0, .m_count = 1u, .m_material = 1u, .m_kind = kind}};
            const auto stale = applySessionParticleOrder(driver, order, 20u);
            PPR_TEST_ASSERT(stale and not *stale);

            order.m_world = 20u;
            const auto hidden = applySessionParticleOrder(driver, order, 19u);
            PPR_TEST_ASSERT(hidden and not *hidden);

            // The authored CPU fixture deliberately has no resident runtime.
            // A current delivery must expose that absence, never silently edit.
            const auto unavailable = applySessionParticleOrder(driver, order, 20u);
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(driver.captureReady() and driver.paused());
            PPR_TEST_ASSERT(driver.particleCount() == particle_count and driver.tickCount() == 0u);
            PPR_TEST_ASSERT(driver.errandCount() == errands and driver.digTotal() == dug);
        }

        const auto after = driver.grid().getCell({32u, 32u});
        PPR_TEST_ASSERT(
            after and after->m_element == before->m_element and after->m_temperature == before->m_temperature);
    };

    // ------------------------------------------------------------------
    // owned particle-tool delivery and retry
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (particle_orders_keep_owned_parameters_through_delivery_and_retry) {
        SessionService service;
        SessionParticleOrder spawn{
            7u, {.m_x_m = 12.5,
                    .m_y_m = 20.5,
                    .m_radius_m = 3.5,
                    .m_count = 17u,
                    .m_material = 4u,
                    .m_kind = colony::EParticleToolKind::spawn}};
        auto destroy = spawn;
        destroy.m_command.m_kind = colony::EParticleToolKind::destroy;
        destroy.m_command.m_radius_m = 6.0;
        PPR_TEST_ASSERT(not service.post(spawn));
        PPR_TEST_ASSERT(not service.deferTool(destroy, kToolServiceTime));

        spawn.m_world = 99u;
        spawn.m_command.m_count = 4096u;
        destroy.m_command.m_radius_m = 1.0;
        {
            auto batch = service.beginService(kToolServiceTime);
            PPR_TEST_ASSERT(batch.has_value());
            PPR_DEFER {
                service.endService();
            };
            PPR_TEST_ASSERT(batch->m_deliveries.size() == 2u);
            const auto &delivered = std::get<SessionParticleOrder>(batch->m_deliveries[0u]);
            const auto &retried = std::get<SessionParticleOrder>(batch->m_deliveries[1u]);
            PPR_TEST_ASSERT(delivered.m_world == 7u and delivered.m_command.m_count == 17u);
            PPR_TEST_ASSERT(delivered.m_command.m_x_m == 12.5 and delivered.m_command.m_y_m == 20.5);
            PPR_TEST_ASSERT(delivered.m_command.m_radius_m == 3.5 and delivered.m_command.m_material == 4u);
            PPR_TEST_ASSERT(delivered.m_command.m_kind == colony::EParticleToolKind::spawn);
            PPR_TEST_ASSERT(retried.m_world == 7u and retried.m_command.m_kind == colony::EParticleToolKind::destroy);
            PPR_TEST_ASSERT(retried.m_command.m_radius_m == 6.0);

            PPR_TEST_ASSERT(not service.deferTool(retried, kToolServiceTime));
        }

        PPR_TEST_ASSERT(service.retriesPending());
        service.stopControls();
        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(service.post(spawn) == std::errc::operation_canceled);
        PPR_TEST_ASSERT(not service.close());
    };

    // ------------------------------------------------------------------
    // build-order rejection and retry budget
    // ------------------------------------------------------------------

    [[nodiscard]] bool footprintIsVacuum(const sim::ChunkGrid &grid, const colony::Footprint &print) {
        for (u32 y = 0u; y < print.m_height; ++y) {
            for (u32 x = 0u; x < print.m_width; ++x) {
                const auto cell = grid.getCell({print.m_min.m_x + x, print.m_min.m_y + y});
                if (not cell or cell->m_element != 0u) {
                    return false;
                }
            }
        }
        return true;
    }

    /// The authored landing carves vacuum around the start, so a metre-aligned
    /// 3x3 destination exists within a few metres of the spawn.
    [[nodiscard]] std::optional<colony::Footprint> findVacuumFootprint(
        const sim::ChunkGrid &grid, const float x_m, const float y_m, const u32 radius_m) {
        const auto origin_x = static_cast<u32>(std::max(x_m, 0.0f));
        const auto origin_y = static_cast<u32>(std::max(y_m, 0.0f));
        for (u32 step_y = 0u; step_y <= radius_m * 2u; ++step_y) {
            for (u32 step_x = 0u; step_x <= radius_m * 2u; ++step_x) {
                const colony::Footprint print{
                    .m_min = {.m_x = (origin_x + step_x) * 3u, .m_y = (origin_y + step_y) * 3u},
                    .m_width = 3u,
                    .m_height = 3u};
                if (footprintIsVacuum(grid, print)) {
                    return print;
                }
            }
        }
        return std::nullopt;
    }

    /// The fixture creates exactly one agent. It is the only transient occupant
    /// a session test can place and move without a resident matter runtime.
    [[nodiscard]] sim::Entity soleAgent(colony::ColonyDriver &driver) {
        sim::Entity found{};
        for (const auto &[entity, agent]: driver.registry().view<colony::Agent>()) {
            (void) agent;
            found = entity;
        }
        return found;
    }

    /// Authored world plus one liquid particle seeded in a vacuum tile near the
    /// spawn. The restored driver owns that snapshot as its CPU matter inventory.
    struct ParticleSeed {
        std::optional<colony::PreparedWorld> m_world{};
        sim::MatterParticleSnapshot m_particles{};
        sim::GlobalCellPos m_tile{};
        sim::GlobalCellPos m_cell{};
    };

    [[nodiscard]] std::error_code prepareParticleWorld(
        const colony::test_fixture::AuthoredFixture &authored, ParticleSeed &seed) {
        auto world = authored.prepare(17u, 31u);
        PPR_RETURN_ERROR_ON_FAIL(SessionWaitFixture, world);

        const auto &start = world->generated().m_start;
        const auto tile = findVacuumFootprint(
            world->grid(), static_cast<float>(start.m_spawn_x_m), static_cast<float>(start.m_spawn_y_m), 3u);
        PPR_RETURN_ERROR_ON_FAIL(
            SessionWaitFixture,
            tile.has_value() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

        const auto *const water = world->catalog().findMaterial("water");
        PPR_RETURN_ERROR_ON_FAIL(
            SessionWaitFixture,
            water != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

        seed.m_tile = tile->m_min;
        seed.m_cell = {tile->m_min.m_x + 1u, tile->m_min.m_y + 1u};
        const double x_m = (static_cast<double>(seed.m_cell.m_x) + 0.5) * colony::kFineCellMetres;
        const double y_m = (static_cast<double>(seed.m_cell.m_y) + 0.5) * colony::kFineCellMetres;
        Array<sim::MatterParticle> rows;
        rows.push_back(
            {.m_id = 1u,
                .m_x_m = x_m,
                .m_y_m = y_m,
                .m_volume_m3 = 0.1,
                .m_mass_kg = 0.1,
                .m_temperature_k = 293.0,
                .m_bulk_density_kg_m3 = 1.0,
                .m_material = water->m_numeric_id,
                .m_phase = sim::EGenerationMatter::liquid});

        seed.m_particles.m_next_id = 2u;
        seed.m_particles.m_chunks.push_back(
            {sim::chunkOf(seed.m_cell), std::make_shared<const Array<sim::MatterParticle>>(std::move(rows))});
        seed.m_world = std::move(*world);
        return {};
    }

    PPR_UNIT_TEST (permanently_rejected_build_order_is_not_retained) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        const auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto print = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(print.has_value());

        // Completed terrain in one destination cell can never become vacuum by
        // waiting, so the whole order is a permanent rejection.
        PPR_TEST_ASSERT(not driver.grid().setCell(
            {print->m_min.m_x + 1u, print->m_min.m_y + 1u}, {driver.boundaryMaterial(), 300.0f}));

        SessionService service;
        const SessionBuildOrder order{world, *print, colony::EColonyTool::build_wall};
        PPR_TEST_ASSERT(not service.post(order));
        const auto errands = driver.errandCount();

        {
            auto batch = service.beginService(kToolServiceTime);
            PPR_TEST_ASSERT(batch and batch->m_deliveries.size() == 1u);
            PPR_DEFER {
                service.endService();
            };
            const auto placed = applySessionBuildOrder(
                driver, std::get<SessionBuildOrder>(batch->m_deliveries[0u]), world);
            PPR_TEST_ASSERT(not placed);
            PPR_TEST_ASSERT(placed.error() != std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(placed.error() == std::errc::file_exists);
            PPR_TEST_ASSERT(driver.errandCount() == errands);
        }

        // The host acks and drops a non-busy rejection: nothing waits for the
        // next service and no budget is consumed.
        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(service.takeExpiredTools().empty());

        // A second delivery through a fresh batch reaches the same verdict and
        // is not silently applied.
        PPR_TEST_ASSERT(not service.post(order));
        {
            auto batch = service.beginService(kToolServiceTime);
            PPR_TEST_ASSERT(batch and batch->m_deliveries.size() == 1u);
            PPR_DEFER {
                service.endService();
            };
            const auto again = applySessionBuildOrder(
                driver, std::get<SessionBuildOrder>(batch->m_deliveries[0u]), world);
            PPR_TEST_ASSERT(not again and again.error() == std::errc::file_exists);
            PPR_TEST_ASSERT(driver.errandCount() == errands);
        }

        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(not service.close());
    };

    PPR_UNIT_TEST (valid_build_order_still_places_once) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto print = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(print.has_value());

        // Keep the actor clear so only the destination decides.
        *pose = {static_cast<float>(print->m_min.m_x) / 3.0f + 5.0f, pose->m_y};

        SessionService service;
        const SessionBuildOrder order{world, *print, colony::EColonyTool::build_wall};
        const auto errands = driver.errandCount();
        const auto placed = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(placed and placed->has_value());
        PPR_TEST_ASSERT(driver.errandCount() == errands + 1u);

        // The errand owns the destination but has written nothing yet; complete
        // it the way advanceErrands does before the single-shot replay.
        auto *const errand = driver.registry().get<colony::BuildErrand>(**placed);
        PPR_TEST_ASSERT(errand != nullptr);
        const auto *const basalt = authored.m_catalog->findMaterial("basalt");
        PPR_TEST_ASSERT(basalt != nullptr);
        PPR_TEST_ASSERT(errand->m_print.m_element == basalt->m_numeric_id);
        for (u32 y = 0u; y < print->m_height; ++y) {
            for (u32 x = 0u; x < print->m_width; ++x) {
                PPR_TEST_ASSERT(not driver.grid().setCell(
                    {print->m_min.m_x + x, print->m_min.m_y + y}, {driver.boundaryMaterial(), 300.0f}));
            }
        }

        errand->m_progress = errand->m_total;

        const auto replay = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(not replay and replay.error() == std::errc::file_exists);
        PPR_TEST_ASSERT(driver.errandCount() == errands + 1u);
        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(not service.close());
    };

    PPR_UNIT_TEST (lamp_build_order_resolves_the_authored_material_and_progresses) {
        colony::test_fixture::AuthoredFixture authored{true};
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        const auto *const lamp = authored.m_catalog->findMaterial("showcase_amber_lamp");
        PPR_TEST_ASSERT(lamp != nullptr and lamp->m_phase == colony::EMaterialPhase::solid);

        auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto tile = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(tile.has_value());

        // Keep the actor clear so only the raw-cell destination decides.
        *pose = {static_cast<float>(tile->m_min.m_x) / 3.0f + 5.0f, pose->m_y};

        // A click-sized lamp is one raw cell, not a metre-aligned tile.
        const colony::Footprint print{.m_min = tile->m_min, .m_width = 1u, .m_height = 1u};
        const SessionBuildOrder order{world, print, colony::EColonyTool::build_lamp};
        const auto errands = driver.errandCount();
        const auto placed = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(placed and placed->has_value());
        PPR_TEST_ASSERT(driver.errandCount() == errands + 1u);

        auto *const errand = driver.registry().get<colony::BuildErrand>(**placed);
        PPR_TEST_ASSERT(errand != nullptr);
        PPR_TEST_ASSERT(errand->m_print.m_solid and errand->m_print.m_element == lamp->m_numeric_id);
        PPR_TEST_ASSERT(errand->m_total == 1u and errand->m_progress == 0u);

        // The lamp uses the ordinary errand path: one build step writes it.
        colony::Pathfinder finder{};
        Array<sim::ChunkPos> touched{};
        PPR_TEST_ASSERT(not colony::advanceErrands(
            driver.grid(), driver.registry(), finder, touched, colony::kErrandCellsPerTick, authored.m_catalog.get()));
        PPR_TEST_ASSERT(errand->m_progress == errand->m_total);
        const auto cell = driver.grid().getCell(print.m_min);
        PPR_TEST_ASSERT(cell and cell->m_element == lamp->m_numeric_id);
    };

    PPR_UNIT_TEST (occupied_lamp_placement_is_a_permanent_rejection) {
        colony::test_fixture::AuthoredFixture authored{true};
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        const auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto tile = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(tile.has_value());

        // Completed terrain in the lamp's only destination can never become
        // vacuum by waiting: a permanent rejection, never a retry.
        PPR_TEST_ASSERT(not driver.grid().setCell(tile->m_min, {driver.boundaryMaterial(), 300.0f}));

        const colony::Footprint print{.m_min = tile->m_min, .m_width = 1u, .m_height = 1u};
        const SessionBuildOrder order{world, print, colony::EColonyTool::build_lamp};
        const auto errands = driver.errandCount();
        const auto placed = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(not placed);
        PPR_TEST_ASSERT(placed.error() != std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(placed.error() == std::errc::file_exists);
        PPR_TEST_ASSERT(driver.errandCount() == errands);
    };

    PPR_UNIT_TEST (lamp_without_authored_content_is_a_non_retryable_rejection) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));

        const auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto tile = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(tile.has_value());
        const auto errands = driver.errandCount();

        const auto placed = driver.buildLamp({.m_min = tile->m_min, .m_width = 1u, .m_height = 1u});
        PPR_TEST_ASSERT(not placed);
        PPR_TEST_ASSERT(placed.error() != std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(placed.error() == std::errc::no_such_file_or_directory);
        PPR_TEST_ASSERT(driver.errandCount() == errands);
    };

    PPR_UNIT_TEST (build_orders_reject_a_footprint_that_covers_a_particle) {
        colony::test_fixture::AuthoredFixture authored{true};
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };

        ParticleSeed seed;
        PPR_TEST_ASSERT(not prepareParticleWorld(authored, seed));
        PPR_TEST_ASSERT(seed.m_world.has_value());

        constexpr colony::ColonyDriverDesc desc{
            .m_seed = 17u,
            .m_tick_hz = 60u,
            .m_agent_count = 1u,
            .m_world_generation = 31u,
            .m_world_edge = sim::kWorldEdge};
        sim::Registry registry;
        PPR_TEST_ASSERT(not colony::registerColonyComponents(registry));
        const auto snapshot = sim::captureFull(seed.m_world->grid(), registry, desc.m_seed);
        PPR_TEST_ASSERT(snapshot.has_value());

        auto matter = colony::makeMatterState(
            seed.m_particles, colony::kFineCellMetres, colony::kReferenceThicknessMetres, colony::kMatterGravityY,
            desc.m_world_generation, seed.m_world->grid().edge());
        PPR_TEST_ASSERT(matter.has_value());
        matter->m_identity.m_terrain_revision = 1u;

        PPR_TEST_ASSERT(not driver.initRestored(
            desc, std::move(*seed.m_world), *snapshot, seed.m_particles, std::move(*matter), 0u, false, std::nullopt));
        const u64 world = driver.worldGeneration();
        const auto errands = driver.errandCount();

        // The particle's native cell lies in the lamp footprint: a permanent,
        // non-busy rejection that records nothing and never defers a terrain
        // edit on a later frame.
        const SessionBuildOrder lamp_order{
            world, {.m_min = seed.m_cell, .m_width = 1u, .m_height = 1u}, colony::EColonyTool::build_lamp};
        const auto lamp = applySessionBuildOrder(driver, lamp_order, world);
        PPR_TEST_ASSERT(not lamp and lamp.error() == std::errc::file_exists);
        PPR_TEST_ASSERT(driver.errandCount() == errands);
        PPR_TEST_ASSERT(not driver.takeEditRejection());

        // Repeating the order keeps the same one-shot verdict instead of
        // becoming a retryable busy.
        const auto repeated = applySessionBuildOrder(driver, lamp_order, world);
        PPR_TEST_ASSERT(not repeated and repeated.error() == std::errc::file_exists);
        PPR_TEST_ASSERT(driver.errandCount() == errands);
        PPR_TEST_ASSERT(not driver.takeEditRejection());

        // Walls share the clearance rule.
        const SessionBuildOrder wall_order{
            world, {.m_min = seed.m_tile, .m_width = 3u, .m_height = 3u, .m_element = driver.boundaryMaterial()},
            colony::EColonyTool::build_wall};
        const auto wall = applySessionBuildOrder(driver, wall_order, world);
        PPR_TEST_ASSERT(not wall and wall.error() == std::errc::file_exists);
        PPR_TEST_ASSERT(driver.errandCount() == errands);
        PPR_TEST_ASSERT(not driver.takeEditRejection());
    };

    PPR_UNIT_TEST (stuck_transient_busy_is_dropped_after_budget) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto print = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(print.has_value());

        // An actor standing in the destination is a genuine transient busy.
        *pose = {
            static_cast<float>(print->m_min.m_x) / 3.0f + 0.5f, static_cast<float>(print->m_min.m_y) / 3.0f + 0.5f};
        const auto errands = driver.errandCount();

        SessionService service;
        const SessionBuildOrder order{world, *print, colony::EColonyTool::build_wall};
        const auto blocked = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(not blocked and blocked.error() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not service.deferTool(order, kToolServiceTime));

        // The first service is still inside the budget: the order is delivered
        // again and re-deferred exactly as the host would.
        const auto first_service = kToolServiceTime + std::chrono::milliseconds{400};
        {
            auto batch = service.beginService(first_service);
            PPR_TEST_ASSERT(batch and batch->m_deliveries.size() == 1u);
            PPR_DEFER {
                service.endService();
            };
            const auto retried = applySessionBuildOrder(
                driver, std::get<SessionBuildOrder>(batch->m_deliveries[0u]), world);
            PPR_TEST_ASSERT(not retried and retried.error() == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(not service.deferTool(order, first_service));
        }

        PPR_TEST_ASSERT(service.retriesPending());
        PPR_TEST_ASSERT(service.takeExpiredTools().empty());

        // Past the expiry the stuck order is dropped instead of injected, so the
        // 1 ms retry deadline can no longer spin the pump.
        const auto expired_at = kToolServiceTime + std::chrono::milliseconds{2000};
        {
            auto batch = service.beginService(expired_at);
            PPR_TEST_ASSERT(batch and batch->m_deliveries.empty());
            PPR_DEFER {
                service.endService();
            };

            auto expired = service.takeExpiredTools();
            PPR_TEST_ASSERT(expired.size() == 1u);
            const auto &dropped = std::get<SessionBuildOrder>(expired[0u]);
            PPR_TEST_ASSERT(dropped.m_world == world);
            PPR_TEST_ASSERT(dropped.m_footprint == *print);
        }

        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(service.takeExpiredTools().empty());
        PPR_TEST_ASSERT(driver.errandCount() == errands);
        PPR_TEST_ASSERT(not service.close());
    };

    PPR_UNIT_TEST (transient_busy_still_applies_when_the_destination_clears) {
        colony::test_fixture::AuthoredFixture authored;
        colony::ColonyDriver driver;
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver.shutdown());
        };
        PPR_TEST_ASSERT(not authored.initialize(driver, {.m_agent_count = 1u, .m_world_generation = 20u}));
        const u64 world = driver.worldGeneration();

        auto *const pose = driver.registry().get<sim::BodyState>(soleAgent(driver));
        PPR_TEST_ASSERT(pose != nullptr);
        const auto print = findVacuumFootprint(driver.grid(), pose->m_x, pose->m_y, 3u);
        PPR_TEST_ASSERT(print.has_value());

        *pose = {
            static_cast<float>(print->m_min.m_x) / 3.0f + 0.5f, static_cast<float>(print->m_min.m_y) / 3.0f + 0.5f};
        const auto errands = driver.errandCount();

        SessionService service;
        const SessionBuildOrder order{world, *print, colony::EColonyTool::build_wall};
        const auto blocked = applySessionBuildOrder(driver, order, world);
        PPR_TEST_ASSERT(not blocked and blocked.error() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not service.deferTool(order, kToolServiceTime));
        PPR_TEST_ASSERT(service.retriesPending());

        // The actor steps out well before the budget expires.
        *pose = {pose->m_x + 5.0f, pose->m_y + 5.0f};

        const auto next_service = kToolServiceTime + std::chrono::milliseconds{500};
        {
            auto batch = service.beginService(next_service);
            PPR_TEST_ASSERT(batch and batch->m_deliveries.size() == 1u);
            PPR_DEFER {
                service.endService();
            };
            const auto placed = applySessionBuildOrder(
                driver, std::get<SessionBuildOrder>(batch->m_deliveries[0u]), world);
            PPR_TEST_ASSERT(placed and placed->has_value());
            PPR_TEST_ASSERT(driver.errandCount() == errands + 1u);
        }

        // Success resolves the retry: nothing is re-deferred and nothing expires.
        PPR_TEST_ASSERT(not service.retriesPending());
        PPR_TEST_ASSERT(service.takeExpiredTools().empty());
        PPR_TEST_ASSERT(not service.close());
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // session-service registration
    // ------------------------------------------------------------------

    const UnitTest session_service = UnitTest::Named("session_service") / [](UnitTest::IRun &_) {
        _.recurse({
            detail::retained_workshop_selection_follows_bundle_authority,
            detail::playable_capture_waits_for_first_present_ack_and_closed_progress,
            detail::retained_presentation_matches_current_activation_without_consuming_staging,
            detail::world_transition_admission_freezes_selection_and_cancellation_cannot_roll_back,
            detail::pending_owner_extracts_acknowledgments_and_close_without_mutation_overtaking,
            detail::lane_pump_defers_owned_ack_and_ordered_controls_until_next_service,
            detail::acknowledged_precommit_error_releases_service_and_preserves_pumped_arrivals,
            detail::blocked_tool_retries_do_not_make_fresh_service_immediately_due,
            detail::terminal_control_stop_retains_acknowledgments_until_receiver_close,
            detail::stale_build_and_dig_orders_never_touch_replacement_driver,
            detail::particle_orders_ignore_stale_and_hidden_worlds_and_report_unavailable_runtime,
            detail::particle_orders_keep_owned_parameters_through_delivery_and_retry,
            detail::permanently_rejected_build_order_is_not_retained,
            detail::valid_build_order_still_places_once,
            detail::lamp_build_order_resolves_the_authored_material_and_progresses,
            detail::occupied_lamp_placement_is_a_permanent_rejection,
            detail::lamp_without_authored_content_is_a_non_retryable_rejection,
            detail::build_orders_reject_a_footprint_that_covers_a_particle,
            detail::stuck_transient_busy_is_dropped_after_budget,
            detail::transient_busy_still_applies_when_the_destination_clears,
        });
    };

    const UnitTest &gameSessionServiceTests() noexcept {
        return session_service;
    }
}
