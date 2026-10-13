module;
#include "pP/UnitTest.h"

module game.tests;
import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.buildings;
import game.colony.driver;
import game.colony.metric;
import game.colony.pathfinding;
import std;

namespace pP::game::tests::detail::movement {
    // ------------------------------------------------------------------
    // movement
    // ------------------------------------------------------------------

    using namespace colony;

    PPR_UNIT_TEST (interactive_dig_holds_cells_bodies_and_ticks_until_complete_publication) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        driver.grid() = sim::ChunkGrid{};
        for (u32 x = 100u; x < 150u; ++x) {
            PPR_TEST_ASSERT(not driver.grid().setCell({x, 40u}, {1u, 293.0f}));
        }
        sim::Entity actor{};
        for (const auto &[entity, agent]: driver.registry().view<Agent>()) {
            (void) agent;
            actor = entity;
        }
        *driver.registry().get<sim::BodyState>(actor) = {110.0f, 43.0f};
        PPR_TEST_ASSERT(not driver.restoreBodies());
        PPR_TEST_ASSERT(not driver.assignControl(actor));
        driver.requireEditPublication();
        const auto before = *driver.registry().get<sim::BodyState>(actor);
        PPR_TEST_ASSERT(not driver.requestDig({127u, 40u}, 2u, 1u));
        const auto period = TimeSpan{std::chrono::seconds{1}} / 60u;
        PPR_TEST_ASSERT(not driver.update(period * 3u));
        PPR_TEST_ASSERT(driver.editPending());
        for (u32 batch = 0u; batch < 32u and not driver.editReadyForPresentation(); ++batch) {
            PPR_TEST_ASSERT(not driver.update(period * 5u));
        }
        PPR_TEST_ASSERT(driver.editReadyForPresentation());
        driver.setPaused(true);
        PPR_TEST_ASSERT(driver.restoreBodies() == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(driver.regenerateLegacyFixture(42u) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(driver.tickCount() == 0u and driver.digTotal() == 0u);
        PPR_TEST_ASSERT(driver.initialMatterBaselineValid());
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == 1u);
        const auto held = *driver.registry().get<sim::BodyState>(actor);
        PPR_TEST_ASSERT(held.m_x == before.m_x and held.m_y == before.m_y);
        Array<ColonyDriver::DirtyChunk> manifest(driver.editManifest().begin(), driver.editManifest().end());
        PPR_TEST_ASSERT(manifest.size() == 2u);
        PPR_TEST_ASSERT(not driver.settleEdit(std::span<const ColonyDriver::DirtyChunk>{manifest.data(), 1u}));
        PPR_TEST_ASSERT(driver.editPending());
        auto stale = manifest;
        --stale[0].m_revision;
        PPR_TEST_ASSERT(not driver.settleEdit(stale));
        PPR_TEST_ASSERT(driver.editPending());
        PPR_TEST_ASSERT(not driver.settleEdit(manifest));
        PPR_TEST_ASSERT(not driver.editPending());
        PPR_TEST_ASSERT(driver.tickCount() == 0u and driver.digTotal() == 2u);
        PPR_TEST_ASSERT(not driver.initialMatterBaselineValid());
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({128u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 0u);
        PPR_TEST_ASSERT(not driver.requestDig({129u, 40u}, 1u, 1u));
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.editPending());
        for (u32 batch = 0u; batch < 32u and not driver.editReadyForPresentation(); ++batch) {
            PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        }
        PPR_TEST_ASSERT(driver.editReadyForPresentation());
        manifest.assign(driver.editManifest().begin(), driver.editManifest().end());
        PPR_TEST_ASSERT(not driver.settleEdit(manifest));
        PPR_TEST_ASSERT(driver.tickCount() == 0u and driver.digTotal() == 3u);
        PPR_TEST_ASSERT(not driver.stepOne());
        PPR_TEST_ASSERT(driver.tickCount() == 1u);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 1u);
        driver.setPaused(false);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 3u);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 3u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (interactive_build_progress_is_frozen_and_teardown_cancels_candidate) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        driver.grid() = sim::ChunkGrid{};
        PPR_TEST_ASSERT(not driver.restoreBodies());
        driver.requireEditPublication();
        driver.setPaused(true);
        const auto errand = driver.buildWall({{120u, 40u}, 16u, 8u, 1u});
        PPR_TEST_ASSERT(errand.has_value());
        PPR_TEST_ASSERT(not driver.stepOne());
        for (u32 batch = 0u; batch < 32u and not driver.editReadyForPresentation(); ++batch) {
            PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        }
        PPR_TEST_ASSERT(driver.editReadyForPresentation());
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({120u, 40u})->m_element == 0u);
        Array<ColonyDriver::DirtyChunk> manifest(driver.editManifest().begin(), driver.editManifest().end());
        PPR_TEST_ASSERT(not driver.settleEdit(manifest));
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 64u);
        PPR_TEST_ASSERT(driver.grid().getCell({120u, 40u})->m_element == 1u);
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.tickCount() == 1u);
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 64u);
        PPR_TEST_ASSERT(not driver.stepOne());
        PPR_TEST_ASSERT(driver.editPending());
        PPR_TEST_ASSERT(driver.registry().get<BuildErrand>(*errand)->m_progress == 64u);
        PPR_TEST_ASSERT(not driver.shutdown());
        PPR_TEST_ASSERT(not driver.editPending());
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        PPR_TEST_ASSERT(not driver.update(TimeSpan{std::chrono::seconds{1}} / 60u));
        PPR_TEST_ASSERT(driver.tickCount() == 1u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (held_dig_owns_its_command_while_the_input_fifo_fills) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        driver.grid() = sim::ChunkGrid{};
        for (u32 x = 100u; x <= 102u; ++x) {
            PPR_TEST_ASSERT(not driver.grid().setCell({x, 40u}, {1u, 293.0f}));
        }
        PPR_TEST_ASSERT(not driver.restoreBodies());
        driver.setPaused(true);
        driver.requireEditPublication();
        PPR_TEST_ASSERT(not driver.requestDig({100u, 40u}, 1u, 1u));
        PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
        PPR_TEST_ASSERT(driver.editPending());
        PPR_TEST_ASSERT(not driver.requestDig({101u, 40u}, 1u, 1u));
        for (u32 request = 0u; request < 255u; ++request) {
            PPR_TEST_ASSERT(not driver.requestDig({102u, 40u}, 1u, 1u));
        }
        for (u32 edit = 0u; edit < 2u; ++edit) {
            for (u32 batch = 0u; batch < 32u and not driver.editReadyForPresentation(); ++batch) {
                PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
            }
            PPR_TEST_ASSERT(driver.editReadyForPresentation());
            Array<ColonyDriver::DirtyChunk> manifest(driver.editManifest().begin(), driver.editManifest().end());
            PPR_TEST_ASSERT(not driver.settleEdit(manifest));
            if (edit == 0u) {
                PPR_TEST_ASSERT(not driver.update(TimeSpan{}));
            }
        }
        PPR_TEST_ASSERT(driver.digTotal() == 2u and driver.tickCount() == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({101u, 40u})->m_element == 0u);
        PPR_TEST_ASSERT(driver.grid().getCell({102u, 40u})->m_element == 1u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (build_failure_retains_its_stage_through_later_tick_systems) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        const auto errand = driver.registry().create();
        driver.registry().emplace<BuildErrand>(errand, BuildErrand{Footprint{{30u, 30u}, 1u, 1u, 1u}, 0u, 2u});
        PPR_TEST_ASSERT(driver.update(TimeSpan{std::chrono::seconds{1}} / 60u) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(std::string_view{driver.stepStage()} == "build");
        PPR_TEST_ASSERT(driver.tickCount() == 0u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (moving_interest_materializes_new_terrain_without_edits_and_retires_old_owners) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        driver.grid() = sim::ChunkGrid{};
        for (u32 x = 0u; x < 1024u; ++x) {
            PPR_TEST_ASSERT(not driver.grid().setCell({x, 40u}, {1u, 293.0f}));
        }
        auto &registry = driver.registry();
        sim::Entity actor{};
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            (void) agent;
            actor = entity;
        }
        registry.get<Agent>(actor)->m_speed = 300.0f;
        *registry.get<sim::BodyState>(actor) = {100.0f, 42.0f};
        PPR_TEST_ASSERT(not driver.restoreBodies());
        PPR_TEST_ASSERT(not driver.assignControl(actor));
        driver.grid().clearAllDirty();
        PPR_TEST_ASSERT(driver.chainCount({4u, 0u}) == 0u);
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));
        for (u32 tick = 0u; tick < 80u; ++tick) {
            PPR_TEST_ASSERT(not driver.update(TimeSpan{std::chrono::seconds{1}} / 60u));
        }
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actor)->m_x > 450.0f);
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actor)->m_y > 41.0f);
        PPR_TEST_ASSERT(driver.chainCount({4u, 0u}) > 0u);
        PPR_TEST_ASSERT(driver.chainCount({0u, 0u}) == 0u);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (metric_actor_targets_and_ladders_use_fine_cells_without_scaling_speed_or_body) {
        sim::Registry registry;
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        PPR_TEST_ASSERT(not spawnAgents(grid, registry, 42u, 1u, nullptr, kFineCellsPerMetre));
        sim::Entity actor{};
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            actor = entity;
            const auto *const pose = registry.get<sim::BodyState>(entity);
            PPR_TEST_ASSERT(std::abs(pose->m_x - (agent.m_spawn.m_x + 0.5f) / 3.0f) < 0.0002f);
            const auto *const body = registry.get<sim::BodyDefinition>(entity);
            PPR_TEST_ASSERT(body->m_half_width == 0.4f and body->m_half_height == 0.9f);
        }
        auto *const pose = registry.get<sim::BodyState>(actor);
        *pose = {10.5f, 20.5f};
        registry.get<Agent>(actor)->m_spawn = {50u, 61u};
        registry.get<Needs>(actor)->m_rest = 10.0f;
        planAgent(grid, registry, std::nullopt, kFineCellsPerMetre);
        PPR_TEST_ASSERT((registry.get<PathReq>(actor)->m_from == sim::GlobalCellPos{31u, 61u}));
        Intent intent{};
        PPR_TEST_ASSERT(not agentIntent(registry, actor, intent, kFineCellsPerMetre));
        PPR_TEST_ASSERT(intent.m_velocity_x == kAgentSpeed and intent.m_velocity_y == 0.0f);
        const auto ladder = registry.create();
        registry.emplace<BuildErrand>(
            ladder,
            BuildErrand{
                .m_print = {.m_min = {30u, 60u}, .m_width = 3u, .m_height = 6u, .m_solid = false, .m_ladder = true},
                .m_progress = 18u,
                .m_total = 18u});
        PPR_TEST_ASSERT(not playerIntent(registry, actor, {0, -1}, intent, kFineCellsPerMetre));
        PPR_TEST_ASSERT(intent.m_climbing and intent.m_velocity_y == -kAgentSpeed);
        pose->m_x = 12.0f;
        PPR_TEST_ASSERT(not playerIntent(registry, actor, {1, 1}, intent, kFineCellsPerMetre));
        PPR_TEST_ASSERT(not intent.m_climbing and intent.m_velocity_y == 0.0f and intent.m_velocity_x == kAgentSpeed);
        PPR_TEST_ASSERT(agentIntent(registry, actor, intent, 2u) == std::errc::invalid_argument);
        pose->m_x = std::numeric_limits<float>::quiet_NaN();
        PPR_TEST_ASSERT(playerIntent(registry, actor, {}, intent, kFineCellsPerMetre) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(registry.remove<Plan>(actor));
        planAgent(grid, registry, std::nullopt, kFineCellsPerMetre);
        PPR_TEST_ASSERT(not registry.get<Plan>(actor));
        PPR_TEST_ASSERT(metreToFineCell({10.5, 20.5}, 1u)->m_x == 10u);
        PPR_TEST_ASSERT(metreToFineCell({10.5, 20.5}, 2u).error() == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (fixed_tick_control_bypasses_planning_and_clears_held_commands) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 2u}));
        driver.grid() = sim::ChunkGrid{};
        auto &registry = driver.registry();
        Array<sim::Entity> actors;
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            (void) agent;
            actors.push_back(entity);
            *registry.get<sim::BodyState>(entity) = {50.5f + actors.size() * 10.0f, 50.5f};
            registry.get<Agent>(entity)->m_spawn = {80u, 50u};
        }
        PPR_TEST_ASSERT(actors.size() == 2u);
        PPR_TEST_ASSERT(not driver.restoreBodies());
        PPR_TEST_ASSERT(not driver.assignControl(actors[0u]));
        PPR_TEST_ASSERT(driver.controlledEntity() == actors[0u]);
        const auto tick = TimeSpan{std::chrono::seconds{1}} / 60u;
        const auto controlled = actors[0u];
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_x > 0.0f);
        PPR_TEST_ASSERT(not registry.get<Plan>(controlled) and not registry.get<PathReq>(controlled));
        PPR_TEST_ASSERT(registry.get<Plan>(actors[1u]) != nullptr);
        for (u32 slice = 0u; slice < 20u; ++slice) {
            PPR_TEST_ASSERT(not driver.update(tick));
        }
        const auto *const path = registry.get<PathComp>(actors[1u]);
        PPR_TEST_ASSERT(path != nullptr and path->m_count > 0u and path->m_cursor > 0u);
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actors[1u])->m_velocity_x > 0.0f);
        const auto falling = registry.get<sim::BodyState>(controlled)->m_velocity_y;
        PPR_TEST_ASSERT(not driver.setPlayerCommand({-1, 0}));
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_x < 0.0f);
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_y < falling);
        PPR_TEST_ASSERT(driver.setPlayerCommand({2, 0}) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(driver.assignControl(registry.create()) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(driver.controlledEntity() == controlled);
        driver.setPaused(true);
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));
        PPR_TEST_ASSERT(not driver.stepOne());
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_x == 0.0f);
        driver.setPaused(false);

        const auto ladder = registry.create();
        registry.emplace<BuildErrand>(
            ladder,
            BuildErrand{
                .m_print = {.m_min = {40u, 40u}, .m_width = 60u, .m_height = 30u, .m_solid = false, .m_ladder = true},
                .m_progress = 1800u,
                .m_total = 1800u});
        PPR_TEST_ASSERT(not driver.setPlayerCommand({0, -1}));
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_y == -kAgentSpeed);
        driver.clearPlayerCommand();
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_y == 0.0f);
        PPR_TEST_ASSERT(registry.remove<BuildErrand>(ladder));
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_y < 0.0f);
        PPR_TEST_ASSERT(not driver.setPlayerCommand({1, 0}));
        PPR_TEST_ASSERT(not driver.restoreBodies());
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(controlled)->m_velocity_x == 0.0f);
        PPR_TEST_ASSERT(not driver.setPlayerCommand({-1, 0}));
        PPR_TEST_ASSERT(not driver.assignControl(actors[1u]));
        PPR_TEST_ASSERT(not driver.update(tick));
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actors[1u])->m_velocity_x == 0.0f);
        PPR_TEST_ASSERT(not driver.assignControl(std::nullopt));
        PPR_TEST_ASSERT(driver.setPlayerCommand({1, 0}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.shutdown());
        PPR_TEST_ASSERT(not driver.controlledEntity());
    };

    PPR_UNIT_TEST (unplanned_actor_is_idle_but_missing_required_state_fails) {
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        const auto entity = registry.create();
        registry.emplace<Agent>(entity, Agent{});
        registry.emplace<Needs>(entity, Needs{});
        registry.emplace<sim::BodyState>(entity, sim::BodyState{});
        Intent intent{1.0f, 2.0f, true};
        PPR_TEST_ASSERT(not agentIntent(registry, entity, intent));
        PPR_TEST_ASSERT(intent.m_velocity_x == 0.0f and intent.m_velocity_y == 0.0f and not intent.m_climbing);
        PPR_TEST_ASSERT(registry.remove<Needs>(entity));
        PPR_TEST_ASSERT(agentIntent(registry, entity, intent) == std::errc::invalid_argument);
        registry.emplace<Needs>(entity, Needs{});
        PPR_TEST_ASSERT(registry.remove<sim::BodyState>(entity));
        PPR_TEST_ASSERT(agentIntent(registry, entity, intent) == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (movement_preserves_falling_momentum_and_restores_ladder_gravity) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 2u}));
        driver.grid() = sim::ChunkGrid{};
        auto &registry = driver.registry();
        Array<sim::Entity> actors;
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            (void) agent;
            actors.push_back(entity);
        }
        PPR_TEST_ASSERT(actors.size() == 2u);
        for (u32 index = 0u; index < actors.size(); ++index) {
            const auto entity = actors[index];
            registry.get<Agent>(entity)->m_spawn = {80u, 50u};
            registry.get<Needs>(entity)->m_rest = 10.0f;
            *registry.get<sim::BodyState>(entity) = {50.0f + index * 10.0f, 50.0f, 0.0f, 0.0f, -2.0f, 0.0f};
            Plan plan{.m_goal = EGoal::seek, .m_site = {80u, 50u}};
            plan.m_actions[0u] = {.m_target = {80u, 50u}};
            plan.m_action_count = 1u;
            registry.emplace<Plan>(entity, plan);
        }
        PPR_TEST_ASSERT(not driver.restoreBodies());
        driver.setPaused(true);
        PPR_TEST_ASSERT(not driver.stepOne());
        const auto entity = actors[0u];
        auto pose = *registry.get<sim::BodyState>(entity);
        PPR_TEST_ASSERT(pose.m_velocity_x > 0.0f and pose.m_velocity_y < -2.0f);
        registry.get<Plan>(entity)->m_actions[0u].m_target = {20u, 50u};
        PPR_TEST_ASSERT(not driver.stepOne());
        const auto reversed = *registry.get<sim::BodyState>(entity);
        PPR_TEST_ASSERT(reversed.m_velocity_x < 0.0f and reversed.m_velocity_y < pose.m_velocity_y);

        const auto ladder = registry.create();
        registry.emplace<BuildErrand>(
            ladder,
            BuildErrand{
                .m_print = {.m_min = {40u, 40u}, .m_width = 30u, .m_height = 30u, .m_solid = false, .m_ladder = true},
                .m_progress = 900u,
                .m_total = 900u});
        PPR_TEST_ASSERT(not driver.stepOne());
        const float climbing_speed = registry.get<sim::BodyState>(entity)->m_velocity_y;
        PPR_TEST_ASSERT(climbing_speed == registry.get<Agent>(entity)->m_speed);
        PPR_TEST_ASSERT(registry.remove<BuildErrand>(ladder));
        PPR_TEST_ASSERT(not driver.stepOne());
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(entity)->m_velocity_y < climbing_speed);

        const float other_y = registry.get<sim::BodyState>(actors[1u])->m_y;
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actors[1u])->m_velocity_x > 0.0f);
        registry.get<Plan>(actors[1u])->m_actions[0u].m_target = {20u, 50u};
        const u64 tick = driver.tickCount();
        PPR_TEST_ASSERT(registry.remove<Needs>(entity));
        PPR_TEST_ASSERT(driver.stepOne() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(driver.tickCount() == tick);
        PPR_TEST_ASSERT(std::string_view{driver.stepStage()} == "movestep");
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actors[1u])->m_y != other_y);
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actors[1u])->m_velocity_x < 0.0f);
        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (restoration_releases_previous_terrain_chains) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.initLegacyFixture({.m_seed = 42u, .m_agent_count = 1u}));
        driver.grid() = sim::ChunkGrid{};
        for (u32 x = 40u; x < 70u; ++x) {
            PPR_TEST_ASSERT(not driver.grid().setCell({x, 40u}, {1u, 293.0f}));
        }
        auto &registry = driver.registry();
        sim::Entity actor{};
        for (const auto &[entity, agent]: registry.view<Agent>()) {
            (void) agent;
            actor = entity;
        }
        registry.get<Agent>(actor)->m_speed = 0.0f;
        *registry.get<sim::BodyState>(actor) = {50.0f, 43.0f};
        PPR_TEST_ASSERT(not driver.restoreBodies());
        driver.setPaused(true);
        for (u32 tick = 0u; tick < 60u; ++tick) {
            PPR_TEST_ASSERT(not driver.stepOne());
        }
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actor)->m_y > 41.0f);
        driver.grid() = sim::ChunkGrid{};
        PPR_TEST_ASSERT(not driver.restoreBodies());
        for (u32 tick = 0u; tick < 60u; ++tick) {
            PPR_TEST_ASSERT(not driver.stepOne());
        }
        PPR_TEST_ASSERT(registry.get<sim::BodyState>(actor)->m_y < 40.0f);
        PPR_TEST_ASSERT(not driver.shutdown());
    };
}

namespace pP::game::tests {
    const UnitTest movement = UnitTest::Named("movement") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::movement::held_dig_owns_its_command_while_the_input_fifo_fills,
                detail::movement::interactive_dig_holds_cells_bodies_and_ticks_until_complete_publication,
                detail::movement::interactive_build_progress_is_frozen_and_teardown_cancels_candidate,
                detail::movement::build_failure_retains_its_stage_through_later_tick_systems,
                detail::movement::moving_interest_materializes_new_terrain_without_edits_and_retires_old_owners,
                detail::movement::metric_actor_targets_and_ladders_use_fine_cells_without_scaling_speed_or_body,
                detail::movement::fixed_tick_control_bypasses_planning_and_clears_held_commands,
                detail::movement::unplanned_actor_is_idle_but_missing_required_state_fails,
                detail::movement::movement_preserves_falling_momentum_and_restores_ladder_gravity,
                detail::movement::restoration_releases_previous_terrain_chains});
    };
    const UnitTest &gameMovementTests() noexcept {
        return movement;
    }
}
