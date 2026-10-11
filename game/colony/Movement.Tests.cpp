module;
#include "pP/UnitTest.h"

module game.tests;
import engine.core;
import engine.sim;
import game.colony.agents;
import game.colony.buildings;
import game.colony.driver;
import std;

namespace pP::game::tests::detail::movement {
    using namespace colony;

    PPR_UNIT_TEST (fixed_tick_control_bypasses_planning_and_clears_held_commands) {
        ColonyDriver driver;
        PPR_TEST_ASSERT(not driver.init({.m_seed = 42u, .m_agent_count = 2u}));
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
                .m_print = {.m_min = {40u, 40u}, .m_width = 60u, .m_height = 30u, .m_solid = false, .m_ladder = true}});
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
        PPR_TEST_ASSERT(not driver.init({.m_seed = 42u, .m_agent_count = 2u}));
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
                .m_print = {.m_min = {40u, 40u}, .m_width = 30u, .m_height = 30u, .m_solid = false, .m_ladder = true}});
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
        PPR_TEST_ASSERT(not driver.init({.m_seed = 42u, .m_agent_count = 1u}));
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
            {detail::movement::fixed_tick_control_bypasses_planning_and_clears_held_commands,
                detail::movement::unplanned_actor_is_idle_but_missing_required_state_fails,
                detail::movement::movement_preserves_falling_momentum_and_restores_ladder_gravity,
                detail::movement::restoration_releases_previous_terrain_chains});
    };
    const UnitTest &gameMovementTests() noexcept {
        return movement;
    }
}
