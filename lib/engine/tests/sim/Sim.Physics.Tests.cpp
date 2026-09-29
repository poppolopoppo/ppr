module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.physics;
import engine.sim;
import std;

namespace pP::tests::detail {
    namespace PhysicsSuite {
        PPR_UNIT_TEST (fixed_replay_produces_identical_body_states) {
            physics::Scene first;
            physics::Scene second;
            const physics::Scene::Desc desc{0.0f, -9.8f, 1.0f / 60.0f, 4u, true};
            PPR_TEST_ASSERT(not first.initialize(desc));
            PPR_TEST_ASSERT(not second.initialize(desc));

            const std::array<u64, 2> entities{1u, 2u};
            std::array<physics::BodyDefinition, 2> definitions{};
            definitions[0].m_motion = physics::EMotion::static_body;
            definitions[0].m_y = -2.0f;
            definitions[1].m_motion = physics::EMotion::dynamic;
            definitions[1].m_y = 4.0f;
            definitions[1].m_velocity_x = 0.5f;
            std::array<physics::BodyHandle, 2> a{};
            std::array<physics::BodyHandle, 2> b{};
            PPR_TEST_ASSERT(not first.createBodies(entities, definitions, a));
            PPR_TEST_ASSERT(not second.createBodies(entities, definitions, b));

            for (int i = 0; i < 120; ++i) {
                PPR_TEST_ASSERT(not first.step());
                PPR_TEST_ASSERT(not second.step());
                PPR_TEST_ASSERT(first.state(a[0]) == second.state(b[0]));
                PPR_TEST_ASSERT(first.state(a[1]) == second.state(b[1]));
            }
            PPR_TEST_ASSERT(not first.shutdown());
            PPR_TEST_ASSERT(not second.shutdown());
        };

        PPR_UNIT_TEST (quiescent_dynamic_body_sleeps_and_explicitly_wakes) {
            physics::Scene scene;
            PPR_TEST_ASSERT(not scene.initialize({0.0f, 0.0f, 1.0f / 60.0f, 4u, true}));
            const std::array<u64, 1> entities{19u};
            std::array<physics::BodyDefinition, 1> definitions{};
            definitions[0].m_motion = physics::EMotion::dynamic;
            std::array<physics::BodyHandle, 1> handles{};
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));
            for (int i = 0; i < 180; ++i) {
                PPR_TEST_ASSERT(not scene.step());
            }
            PPR_TEST_ASSERT(scene.isAwake(handles[0]).has_value());
            PPR_TEST_ASSERT(not * scene.isAwake(handles[0]));
            PPR_TEST_ASSERT(not scene.setAwake(handles[0], true));
            PPR_TEST_ASSERT(*scene.isAwake(handles[0]));
            PPR_TEST_ASSERT(not scene.enableSleep(handles[0], false));
            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (destroy_and_reset_invalidate_handles_without_aliasing) {
            physics::Scene scene;
            PPR_TEST_ASSERT(not scene.initialize({}));
            const std::array<u64, 1> entities{7u};
            const std::array<physics::BodyDefinition, 1> definitions{};
            std::array<physics::BodyHandle, 1> handles{};
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));
            const physics::BodyHandle destroyed = handles[0];
            PPR_TEST_ASSERT(not scene.destroyBody(destroyed));
            PPR_TEST_ASSERT(scene.destroyBody(destroyed) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not scene.state(destroyed).has_value());
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));
            PPR_TEST_ASSERT(handles[0] != destroyed);
            PPR_TEST_ASSERT(scene.setAwake(destroyed, true) == std::errc::invalid_argument);
            const physics::BodyHandle cleared = handles[0];
            PPR_TEST_ASSERT(not scene.clear());
            PPR_TEST_ASSERT(not scene.isAwake(cleared).has_value());
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));
            PPR_TEST_ASSERT(handles[0] != cleared);
            PPR_TEST_ASSERT(scene.attachSensor(cleared, {}) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not scene.shutdown());
            PPR_TEST_ASSERT(scene.step() == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(not scene.initialize({}));
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));
            PPR_TEST_ASSERT(handles[0] != cleared);
            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (dig_and_build_replace_chunk_boundaries) {
            physics::Scene scene;
            physics::ChunkColliders colliders;
            PPR_TEST_ASSERT(not scene.initialize({}));
            constexpr physics::ColliderChunkPos pos{0, 0};
            std::array<u16, 64> elements{};
            for (int x = 0; x < 8; ++x) {
                elements[x] = 1u;
            }
            const std::array chunks{physics::ColliderChunkView{pos, elements, true}};
            const std::array dirty{pos};
            u32 processed{};
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 8u, 1u, processed));
            PPR_TEST_ASSERT(processed == 1u and colliders.chainCount(pos) != 0u);

            const std::array<u64, 1> entities{101u};
            std::array<physics::BodyDefinition, 1> bodies{};
            bodies[0].m_motion = physics::EMotion::dynamic;
            bodies[0].m_x = 3.5f;
            bodies[0].m_y = 3.0f;
            bodies[0].m_half_width = 0.3f;
            bodies[0].m_half_height = 0.3f;
            std::array<physics::BodyHandle, 1> handles{};
            PPR_TEST_ASSERT(not scene.createBodies(entities, bodies, handles));
            for (int i = 0; i < 120; ++i) {
                PPR_TEST_ASSERT(not scene.step());
            }
            PPR_TEST_ASSERT(scene.state(handles[0])->m_y > 1.2f);

            elements[3] = 0u;
            elements[4] = 0u;
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 8u, 1u, processed));
            PPR_TEST_ASSERT(processed == 1u);
            PPR_TEST_ASSERT(not scene.setAwake(handles[0], true));
            for (int i = 0; i < 120; ++i) {
                PPR_TEST_ASSERT(not scene.step());
            }
            PPR_TEST_ASSERT(scene.state(handles[0])->m_y < 0.0f);

            elements[3] = 1u;
            elements[4] = 1u;
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 8u, 1u, processed));
            PPR_TEST_ASSERT(processed == 1u and colliders.chainCount(pos) != 0u);
            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (cross_chunk_seam_slide_uses_ghost_vertices) {
            physics::Scene scene;
            physics::ChunkColliders colliders;
            PPR_TEST_ASSERT(not scene.initialize({}));
            std::array<u16, 64> left{};
            std::array<u16, 64> right{};
            for (int x = 0; x < 8; ++x) {
                left[x] = 1u;
                right[x] = 1u;
            }
            const std::array chunks{
                physics::ColliderChunkView{{0, 0}, left, true},
                physics::ColliderChunkView{{1, 0}, right, true}
            };
            const std::array dirty{physics::ColliderChunkPos{0, 0}};
            u32 processed{};
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 8u, 2u, processed));
            PPR_TEST_ASSERT(processed == 2u and colliders.chainCount({1, 0}) != 0u);

            const std::array<u64, 1> entities{102u};
            std::array<physics::BodyDefinition, 1> bodies{};
            bodies[0].m_motion = physics::EMotion::dynamic;
            bodies[0].m_x = 6.0f;
            bodies[0].m_y = 2.0f;
            bodies[0].m_velocity_x = 5.0f;
            bodies[0].m_half_width = 0.3f;
            bodies[0].m_half_height = 0.3f;
            std::array<physics::BodyHandle, 1> handles{};
            PPR_TEST_ASSERT(not scene.createBodies(entities, bodies, handles));
            for (int i = 0; i < 120; ++i) {
                PPR_TEST_ASSERT(not scene.step());
            }
            const auto state = scene.state(handles[0]);
            PPR_TEST_ASSERT(state.has_value() and state->m_x > 8.0f and state->m_y > 1.1f);
            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (lazy_activation_and_bounded_dirty_slices) {
            physics::Scene scene;
            physics::ChunkColliders colliders;
            PPR_TEST_ASSERT(not scene.initialize({}));
            std::array<std::array<u16, 64>, 5> cells{};
            for (auto &chunk: cells) {
                chunk[0] = 1u;
            }
            std::array<physics::ColliderChunkView, 5> chunks{};
            std::array<physics::ColliderChunkPos, 5> dirty{};
            for (int i = 0; i < 5; ++i) {
                dirty[i] = {i, 0};
                chunks[i] = {dirty[i], cells[i], false};
            }
            u32 processed{};
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 8u, 2u, processed));
            PPR_TEST_ASSERT(processed == 2u and colliders.pendingCount() == 3u);
            PPR_TEST_ASSERT(colliders.chainCount({0, 0}) == 0u);
            PPR_TEST_ASSERT(not colliders.materializeForQuery(scene, chunks, {0, 0}, 8u));
            PPR_TEST_ASSERT(colliders.chainCount({0, 0}) != 0u);
            chunks[3].m_needed = true;
            chunks[4].m_needed = true;
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, {}, 8u, 2u, processed));
            PPR_TEST_ASSERT(processed == 2u and colliders.pendingCount() == 1u);
            PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, {}, 8u, 2u, processed));
            PPR_TEST_ASSERT(processed == 1u and colliders.pendingCount() == 0u);
            PPR_TEST_ASSERT(colliders.chainCount({3, 0}) != 0u and colliders.chainCount({4, 0}) != 0u);
            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (ladder_door_vent_sensor_begin_end_are_published) {
            physics::Scene scene;
            physics::Coupling coupling;
            PPR_TEST_ASSERT(not scene.initialize({0.0f, 0.0f, 1.0f / 60.0f, 4u, true}));
            const std::array<u64, 4> entities{11u, 12u, 13u, 99u};
            std::array<physics::BodyDefinition, 4> definitions{};

            for (std::size_t i = 0u; i < 3u; ++i) {
                definitions[i].m_x = static_cast<float>(i) * 2.0f;
                definitions[i].m_y = 0.0f;
                definitions[i].m_half_width = 0.1f;
                definitions[i].m_half_height = 0.1f;
                definitions[i].m_mask_bits = 0u;
            }
            definitions[3].m_motion = physics::EMotion::dynamic;
            definitions[3].m_x = -2.0f;
            definitions[3].m_velocity_x = 4.0f;
            definitions[3].m_half_width = 0.2f;
            definitions[3].m_half_height = 0.2f;
            definitions[3].m_enable_sleep = false;
            std::array<physics::BodyHandle, 4> handles{};
            PPR_TEST_ASSERT(not scene.createBodies(entities, definitions, handles));

            for (u32 i = 0u; i < 3u; ++i) {
                physics::SensorDefinition sensor{};
                sensor.m_half_width = 0.4f;
                sensor.m_half_height = 0.5f;
                sensor.m_kind = i + 1u;
                PPR_TEST_ASSERT(not scene.attachSensor(handles[i], sensor));
            }
            std::array<std::array<bool, 2>, 3> observed{};

            for (u32 tick = 0u; tick < 125u; ++tick) {
                PPR_TEST_ASSERT(not scene.step());
                std::array<physics::BodyState, 4> states{};
                std::array<physics::SensorEvent, 16> sensors{};
                std::array<physics::Coupling::WakeRecord, 4> wakes{};
                std::size_t sensor_count{};
                std::size_t wake_count{};
                std::size_t contact_count{};
                PPR_TEST_ASSERT(not coupling.publishUpdates(scene, handles, states, sensors, sensor_count,
                    wakes, wake_count, {}, contact_count));
                for (std::size_t index = 0u; index < sensor_count; ++index) {
                    const physics::SensorEvent &event = sensors[index];
                    PPR_TEST_ASSERT(event.m_visitor_entity == 99u);
                    PPR_TEST_ASSERT(event.m_sensor_entity == entities[event.m_kind - 1u]);
                    observed[event.m_kind - 1u][event.m_begin ? 0u : 1u] = true;
                }
            }
            for (const auto &kind: observed) {
                PPR_TEST_ASSERT(kind[0] and kind[1]);
            }

            PPR_TEST_ASSERT(not scene.shutdown());
        };

        PPR_UNIT_TEST (v3_active_contact_continues_by_deterministic_replay) {
            sim::Registry source;
            sim::Registry restored;
            PPR_TEST_ASSERT(source.registerComponent<sim::BodyState>().has_value());
            PPR_TEST_ASSERT(restored.registerComponent<sim::BodyState>().has_value());
            PPR_TEST_ASSERT(source.registerComponent<sim::BodyDefinition>().has_value());
            PPR_TEST_ASSERT(restored.registerComponent<sim::BodyDefinition>().has_value());
            std::array<sim::Entity, 3> entities{};
            for (sim::Entity &entity: entities) {
                entity = source.create();
                source.emplace(entity, sim::BodyState{});
            }
            const sim::Entity dead_a = source.create();
            const sim::Entity dead_b = source.create();
            source.destroy(dead_a);
            source.destroy(dead_b);
            physics::Scene original;
            physics::Scene resumed;
            const physics::Scene::Desc desc{0.0f, -9.8f, 1.0f / 60.0f, 4u, true};
            PPR_TEST_ASSERT(not original.initialize(desc));
            PPR_TEST_ASSERT(not resumed.initialize(desc));
            const std::array<physics::ChainPoint, 2> floor{{{-5.0f, 0.0f}, {5.0f, 0.0f}}};
            const physics::ChainDefinition chain{floor, {-6.0f, 0.0f}, {6.0f, 0.0f}, 0u, 1u, false};
            PPR_TEST_ASSERT(original.createChunkChain(chain).has_value());
            PPR_TEST_ASSERT(resumed.createChunkChain(chain).has_value());
            const std::array<u64, 3> keys{1u, 2u, 3u};
            std::array<physics::BodyDefinition, 3> definitions{};
            for (auto &definition: definitions) {
                definition.m_motion = physics::EMotion::dynamic;
            }
            definitions[0].m_y = 2.0f;
            definitions[1].m_y = 3.1f;
            definitions[2].m_x = 20.0f;
            definitions[2].m_y = 10.0f;
            definitions[2].m_is_awake = false;
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                const physics::BodyDefinition &spawn = definitions[index];
                sim::BodyDefinition footprint{};
                footprint.m_motion = static_cast<sim::EMotionKind>(spawn.m_motion);
                footprint.m_geometry = static_cast<sim::EBodyGeometry>(spawn.m_geometry);
                footprint.m_half_width = spawn.m_half_width;
                footprint.m_half_height = spawn.m_half_height;
                footprint.m_density = spawn.m_density;
                footprint.m_friction = spawn.m_friction;
                footprint.m_restitution = spawn.m_restitution;
                footprint.m_category_bits = spawn.m_category_bits;
                footprint.m_mask_bits = spawn.m_mask_bits;
                footprint.m_group_index = spawn.m_group_index;
                footprint.m_enable_sleep = spawn.m_enable_sleep;
                footprint.m_is_awake = spawn.m_is_awake;
                footprint.m_is_bullet = spawn.m_is_bullet;
                footprint.m_fixed_rotation = spawn.m_fixed_rotation;
                source.emplace(entities[index], footprint);
            }
            std::array<physics::BodyHandle, 3> old_handles{};
            std::array<physics::BodyHandle, 3> new_handles{};
            PPR_TEST_ASSERT(not original.createBodies(keys, definitions, old_handles));
            for (u32 tick = 0u; tick < 180u; ++tick) {
                PPR_TEST_ASSERT(not original.step());
            }
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                const auto state = original.state(old_handles[index]);
                PPR_TEST_ASSERT(state.has_value());
                *source.get<sim::BodyState>(entities[index]) = {
                    state->m_x, state->m_y, state->m_angle,
                    state->m_velocity_x, state->m_velocity_y,
                    state->m_angular_velocity
                };
            }
            PPR_TEST_ASSERT(not * original.isAwake(old_handles[2]));
            sim::ChunkGrid grid;
            sim::Snapshot checkpoint = sim::capture(grid, source, 77u);
            checkpoint.m_bodies.m_replay_ticks = 180u;
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                checkpoint.m_bodies.m_records[index].m_awake = *original.isAwake(old_handles[index]);
            }
            const auto saved = sim::save(checkpoint);
            PPR_TEST_ASSERT(saved.has_value());
            const auto loaded = sim::load(std::span<const u8>{*saved});
            PPR_TEST_ASSERT(loaded.has_value());
            PPR_TEST_ASSERT(*loaded == checkpoint);
            PPR_TEST_ASSERT(not sim::restore(restored, *loaded));
            const auto footprints_match =
                    [](const sim::BodyDefinition &left, const sim::BodyDefinition &right) noexcept -> bool {
                return left.m_motion == right.m_motion and
                    left.m_geometry == right.m_geometry and
                    left.m_half_width == right.m_half_width and
                    left.m_half_height == right.m_half_height and
                    left.m_density == right.m_density and
                    left.m_friction == right.m_friction and
                    left.m_restitution == right.m_restitution and
                    left.m_category_bits == right.m_category_bits and
                    left.m_mask_bits == right.m_mask_bits and
                    left.m_group_index == right.m_group_index and
                    left.m_enable_sleep == right.m_enable_sleep and
                    left.m_is_awake == right.m_is_awake and
                    left.m_is_bullet == right.m_is_bullet and
                    left.m_fixed_rotation == right.m_fixed_rotation;
            };
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                const sim::BodyDefinition *expected = source.get<sim::BodyDefinition>(entities[index]);
                const sim::BodyDefinition *actual = restored.get<sim::BodyDefinition>(entities[index]);
                PPR_TEST_ASSERT(expected and actual and footprints_match(*expected, *actual));
            }

            // Replay reconstructs the original insertion order and Box2D's
            // contact/sleep solver caches; body values alone cannot restore them.
            // Placement-agnostic spawn params are reconstructed from the
            // snapshot-restored footprints and checked against the hand-written
            // oracle before creating bodies. Spawn placement itself is not
            // retained by the snapshot — the restored BodyState holds post-sim
            // poses (asserted against post-replay state below) — so placement
            // stays oracle-sourced by construction.
            std::array<physics::BodyDefinition, 3> replay_definitions{};
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                const sim::BodyDefinition *footprint = restored.get<sim::BodyDefinition>(entities[index]);
                PPR_TEST_ASSERT(footprint);
                const physics::BodyDefinition &spawn = definitions[index];
                physics::BodyDefinition &replay = replay_definitions[index];
                replay.m_motion = static_cast<physics::EMotion>(footprint->m_motion);
                replay.m_geometry = static_cast<physics::EGeometry>(footprint->m_geometry);
                replay.m_half_width = footprint->m_half_width;
                replay.m_half_height = footprint->m_half_height;
                replay.m_density = footprint->m_density;
                replay.m_friction = footprint->m_friction;
                replay.m_restitution = footprint->m_restitution;
                replay.m_category_bits = footprint->m_category_bits;
                replay.m_mask_bits = footprint->m_mask_bits;
                replay.m_group_index = footprint->m_group_index;
                replay.m_enable_sleep = footprint->m_enable_sleep;
                replay.m_is_awake = footprint->m_is_awake;
                replay.m_is_bullet = footprint->m_is_bullet;
                replay.m_fixed_rotation = footprint->m_fixed_rotation;
                replay.m_x = spawn.m_x;
                replay.m_y = spawn.m_y;
                replay.m_angle = spawn.m_angle;
                replay.m_velocity_x = spawn.m_velocity_x;
                replay.m_velocity_y = spawn.m_velocity_y;
                replay.m_angular_velocity = spawn.m_angular_velocity;
                PPR_TEST_ASSERT(replay.m_motion == spawn.m_motion and
                    replay.m_geometry == spawn.m_geometry and
                    replay.m_half_width == spawn.m_half_width and
                    replay.m_half_height == spawn.m_half_height and
                    replay.m_density == spawn.m_density and
                    replay.m_friction == spawn.m_friction and
                    replay.m_restitution == spawn.m_restitution and
                    replay.m_category_bits == spawn.m_category_bits and
                    replay.m_mask_bits == spawn.m_mask_bits and
                    replay.m_group_index == spawn.m_group_index and
                    replay.m_enable_sleep == spawn.m_enable_sleep and
                    replay.m_is_awake == spawn.m_is_awake and
                    replay.m_is_bullet == spawn.m_is_bullet and
                    replay.m_fixed_rotation == spawn.m_fixed_rotation);
            }
            PPR_TEST_ASSERT(not resumed.createBodies(keys, replay_definitions, new_handles));
            for (u64 tick = 0u; tick < loaded->m_bodies.m_replay_ticks; ++tick) {
                PPR_TEST_ASSERT(not resumed.step());
            }
            for (std::size_t index = 0u; index < entities.size(); ++index) {
                const auto old_state = original.state(old_handles[index]);
                const auto new_state = resumed.state(new_handles[index]);
                PPR_TEST_ASSERT(old_state == new_state);
                PPR_TEST_ASSERT(*original.isAwake(old_handles[index]) == *resumed.isAwake(new_handles[index]));
                const sim::BodyState *value = restored.get<sim::BodyState>(entities[index]);
                PPR_TEST_ASSERT(value and value->m_x == new_state->m_x and value->m_y == new_state->m_y);
            }
            for (u32 tick = 0u; tick < 60u; ++tick) {
                PPR_TEST_ASSERT(not original.step());
                PPR_TEST_ASSERT(not resumed.step());
                for (std::size_t index = 0u; index < entities.size(); ++index) {
                    PPR_TEST_ASSERT(original.state(old_handles[index]) == resumed.state(new_handles[index]));
                    PPR_TEST_ASSERT(original.isAwake(old_handles[index]) == resumed.isAwake(new_handles[index]));
                }
            }
            for (u32 allocation = 0u; allocation < 3u; ++allocation) {
                PPR_TEST_ASSERT(source.create() == restored.create());
            }
            PPR_TEST_ASSERT(not original.shutdown());
            PPR_TEST_ASSERT(not resumed.shutdown());
        };
    }
}

namespace pP::tests {
    const UnitTest physics = UnitTest::Named("physics") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::PhysicsSuite::fixed_replay_produces_identical_body_states,
            detail::PhysicsSuite::quiescent_dynamic_body_sleeps_and_explicitly_wakes,
            detail::PhysicsSuite::destroy_and_reset_invalidate_handles_without_aliasing,
            detail::PhysicsSuite::dig_and_build_replace_chunk_boundaries,
            detail::PhysicsSuite::cross_chunk_seam_slide_uses_ghost_vertices,
            detail::PhysicsSuite::lazy_activation_and_bounded_dirty_slices,
            detail::PhysicsSuite::ladder_door_vent_sensor_begin_end_are_published,
            detail::PhysicsSuite::v3_active_contact_continues_by_deterministic_replay
        });
    };

    const UnitTest &physicsTests() noexcept {
        return physics;
    }
}
