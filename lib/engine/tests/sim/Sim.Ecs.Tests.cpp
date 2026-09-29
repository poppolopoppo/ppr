module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace EcsSuite {
        using namespace pP::sim;

        struct Position {
            i32 m_x{};
            i32 m_y{};
        };

        struct Velocity {
            float m_dx{};
            float m_dy{};
        };

        struct Health {
            u32 m_hp{};
        };

        PPR_UNIT_TEST (lifecycle_recycles_slots_without_aliasing) {
            Registry registry;

            const Entity first = registry.create();
            PPR_TEST_ASSERT(first.m_index == 0u);
            PPR_TEST_ASSERT(first.m_generation == 1u);
            PPR_TEST_ASSERT(first.isValid());
            PPR_TEST_ASSERT(registry.isAlive(first));
            PPR_TEST_ASSERT(registry.aliveCount() == 1u);
            PPR_TEST_ASSERT(registry.slotCount() == 1u);

            registry.destroy(first);
            PPR_TEST_ASSERT(not registry.isAlive(first));
            PPR_TEST_ASSERT(registry.aliveCount() == 0u);
            PPR_TEST_ASSERT(registry.slotCount() == 1u); // dead slots stay in the index domain

            const Entity recycled = registry.create();
            PPR_TEST_ASSERT(recycled.m_index == first.m_index);
            PPR_TEST_ASSERT(recycled.m_generation != first.m_generation); // recycled ids never alias
            PPR_TEST_ASSERT(not registry.isAlive(first)); // the stale handle stays dead
            PPR_TEST_ASSERT(registry.isAlive(recycled));

            const Entity fresh = registry.create();
            PPR_TEST_ASSERT(fresh.m_index != recycled.m_index);
            PPR_TEST_ASSERT(fresh.m_generation == 1u); // fresh slots start at generation 1
            PPR_TEST_ASSERT(registry.slotCount() == 2u);
            PPR_TEST_ASSERT(registry.aliveCount() == 2u);

            // The free list is LIFO: the most recently destroyed slot returns first.
            registry.destroy(fresh);
            registry.destroy(recycled);
            PPR_TEST_ASSERT(registry.aliveCount() == 0u);

            const Entity head = registry.create();
            const Entity next = registry.create();
            PPR_TEST_ASSERT(head.m_index == recycled.m_index);
            PPR_TEST_ASSERT(next.m_index == fresh.m_index);
            PPR_TEST_ASSERT(head.m_index != next.m_index);
            PPR_TEST_ASSERT(registry.aliveCount() == 2u);
        };

        PPR_UNIT_TEST (clear_keeps_component_registration_and_resets_slots) {
            Registry registry{};
            const Expected<ComponentId> id = registry.registerComponent<Position>();
            PPR_TEST_ASSERT(id.has_value());
            const Entity entity = registry.create();
            registry.emplace(entity, Position{1, 2});
            registry.destroy(entity);

            registry.clear();
            PPR_TEST_ASSERT(registry.slotCount() == 0u);
            PPR_TEST_ASSERT(registry.aliveCount() == 0u);
            PPR_TEST_ASSERT(registry.componentCount() == 1u);
            PPR_TEST_ASSERT(registry.componentId<Position>() == *id);
            PPR_TEST_ASSERT(registry.dump().m_columns[0].m_owners.empty());
            const Entity fresh = registry.create();
            PPR_TEST_ASSERT(fresh == (Entity{0u, 1u}));
            registry.emplace(fresh, Position{3, 4});
            PPR_TEST_ASSERT(registry.get<Position>(fresh)->m_x == 3);
        };

        PPR_UNIT_TEST (view_visits_ascending_slot_order) {
            Registry registry;

            const Expected<ComponentId> position = registry.registerComponent<Position>();
            const Expected<ComponentId> velocity = registry.registerComponent<Velocity>();
            PPR_TEST_ASSERT(position.has_value());
            PPR_TEST_ASSERT(velocity.has_value());

            const Entity a = registry.create(); // slot 0
            const Entity b = registry.create(); // slot 1
            const Entity c = registry.create(); // slot 2

            // Insertion order deliberately differs from slot order.
            registry.emplace(b, Position{20, 20});
            registry.emplace(a, Position{10, 10});
            registry.emplace(c, Position{30, 30});
            registry.emplace(c, Velocity{1.0f, 2.0f});

            registry.destroy(b); // survivors keep ascending slot order after swap-remove

            const Entity d = registry.create(); // LIFO recycle of slot 1
            PPR_TEST_ASSERT(d.m_index == 1u);
            registry.emplace(d, Position{40, 40});

            Array<u32> order{};
            for (auto [entity, component]: registry.view<Position>()) {
                order.push_back(entity.m_index);
                component.m_x += 1; // writes through the yielded reference reach the column
            }

            PPR_TEST_ASSERT(order.size() == 3u);
            PPR_TEST_ASSERT(order[0] == a.m_index);
            PPR_TEST_ASSERT(order[1] == d.m_index);
            PPR_TEST_ASSERT(order[2] == c.m_index);
            PPR_TEST_ASSERT(order[0] < order[1]);
            PPR_TEST_ASSERT(order[1] < order[2]);
            PPR_TEST_ASSERT(registry.get<Position>(a)->m_x == 11);
            PPR_TEST_ASSERT(registry.get<Position>(d)->m_x == 41);
            PPR_TEST_ASSERT(registry.get<Position>(c)->m_x == 31);

            // Determinism: a second pass visits the same sequence, and algorithms
            // over the range agree with the range-for pass.
            Array<u32> repeat{};
            for (const auto row: registry.view<Position>()) {
                repeat.push_back(std::get<0>(row).m_index);
            }
            PPR_TEST_ASSERT(repeat == order);
            PPR_TEST_ASSERT(std::ranges::count_if(registry.view<Position>(),
                                [](const auto &) { return true; }) == 3);

            // Only entities holding every requested component appear.
            Array<u32> holders{};
            for (const auto row: registry.view<Position, Velocity>()) {
                holders.push_back(std::get<0>(row).m_index);
            }
            PPR_TEST_ASSERT(holders.size() == 1u);
            PPR_TEST_ASSERT(holders[0] == c.m_index);
        };

        PPR_UNIT_TEST (unregistered_component_type_yields_empty_view) {
            Registry registry;
            PPR_TEST_ASSERT(registry.registerComponent<Position>().has_value());

            const Entity entity = registry.create();
            registry.emplace(entity, Position{5, 6});

            PPR_TEST_ASSERT(registry.componentId<Velocity>() == std::nullopt);

            const auto single = registry.view<Velocity>();
            PPR_TEST_ASSERT(single.begin() == single.end());
            PPR_TEST_ASSERT(std::ranges::count_if(single, [](const auto &) { return true; }) == 0);

            // One unregistered requested type empties the whole view.
            const auto mixed = registry.view<Position, Velocity>();
            PPR_TEST_ASSERT(mixed.begin() == mixed.end());
            PPR_TEST_ASSERT(std::ranges::count_if(mixed, [](const auto &) { return true; }) == 0);

            // The registered type alone still visits its holder.
            PPR_TEST_ASSERT(std::ranges::count_if(registry.view<Position>(),
                                [](const auto &) { return true; }) == 1);
        };

        PPR_UNIT_TEST (emplace_get_remove_round_trip) {
            Registry registry;

            const Expected<ComponentId> position = registry.registerComponent<Position>();
            const Expected<ComponentId> health = registry.registerComponent<Health>();
            PPR_TEST_ASSERT(position.has_value());
            PPR_TEST_ASSERT(health.has_value());

            const Entity holder = registry.create();
            const Entity neighbour = registry.create();

            // Absent paths: nothing was emplaced, so nothing to read or remove.
            PPR_TEST_ASSERT(registry.get<Position>(holder) == nullptr);
            PPR_TEST_ASSERT(not registry.remove<Position>(holder));

            registry.emplace(holder, Position{1, 2});
            registry.emplace(neighbour, Position{30, 40});
            registry.emplace(holder, Health{100});

            const Position *const held = registry.get<Position>(holder);
            PPR_TEST_ASSERT(held != nullptr);
            PPR_TEST_ASSERT(held->m_x == 1 and held->m_y == 2);
            PPR_TEST_ASSERT(registry.mask(holder).test(*position));
            PPR_TEST_ASSERT(registry.mask(holder).test(*health));

            // Mutable access writes straight into the column.
            registry.get<Position>(holder)->m_x = 7;
            PPR_TEST_ASSERT(registry.get<Position>(holder)->m_x == 7);

            // Swap-remove the head row: the neighbour's tail row moves into the hole.
            PPR_TEST_ASSERT(registry.remove<Position>(holder));
            PPR_TEST_ASSERT(registry.get<Position>(holder) == nullptr);
            PPR_TEST_ASSERT(not registry.mask(holder).test(*position));
            PPR_TEST_ASSERT(not registry.remove<Position>(holder)); // removing again is absent
            PPR_TEST_ASSERT(registry.mask(holder).test(*health)); // other columns are untouched

            const Position *const moved = registry.get<Position>(neighbour);
            PPR_TEST_ASSERT(moved != nullptr);
            PPR_TEST_ASSERT(moved->m_x == 30 and moved->m_y == 40);

            // A dead entity reads as absent everywhere.
            registry.destroy(holder);
            PPR_TEST_ASSERT(registry.get<Position>(holder) == nullptr);
            PPR_TEST_ASSERT(registry.get<Health>(holder) == nullptr);
        };

        PPR_UNIT_TEST (register_component_is_idempotent) {
            Registry registry;

            const Expected<ComponentId> first = registry.registerComponent<Position>();
            const Expected<ComponentId> again = registry.registerComponent<Position>();
            PPR_TEST_ASSERT(first.has_value());
            PPR_TEST_ASSERT(again.has_value());
            PPR_TEST_ASSERT(*first == *again);
            PPR_TEST_ASSERT(*first == 0u);
            PPR_TEST_ASSERT(registry.componentCount() == 1u);

            const Expected<ComponentId> velocity = registry.registerComponent<Velocity>();
            PPR_TEST_ASSERT(velocity.has_value());
            PPR_TEST_ASSERT(*velocity == 1u);
            PPR_TEST_ASSERT(registry.componentCount() == 2u);

            // Re-registering the same type reuses its id and adds no column.
            PPR_TEST_ASSERT(registry.registerComponent<Position>() == first);
            PPR_TEST_ASSERT(registry.componentCount() == 2u);

            const std::optional<ComponentId> known = registry.componentId<Position>();
            PPR_TEST_ASSERT(known.has_value());
            PPR_TEST_ASSERT(*known == *first);
            PPR_TEST_ASSERT(not registry.componentId<Health>().has_value());
        };

        PPR_UNIT_TEST (dead_and_stale_entities_report_empty_mask) {
            Registry registry;
            PPR_TEST_ASSERT(registry.registerComponent<Position>().has_value());

            const Entity entity = registry.create();
            registry.emplace(entity, Position{1, 1});
            PPR_TEST_ASSERT(not registry.mask(entity).empty());

            registry.destroy(entity);
            PPR_TEST_ASSERT(not registry.isAlive(entity));
            PPR_TEST_ASSERT(registry.mask(entity).empty());
            PPR_TEST_ASSERT(registry.aliveCount() == 0u);

            // A recycled slot does not resurrect the stale handle's mask.
            const Entity recycled = registry.create();
            PPR_TEST_ASSERT(recycled.m_index == entity.m_index);
            PPR_TEST_ASSERT(recycled.m_generation != entity.m_generation);
            PPR_TEST_ASSERT(registry.mask(entity).empty());

            registry.emplace(recycled, Position{9, 9});
            PPR_TEST_ASSERT(not registry.isAlive(entity));
            PPR_TEST_ASSERT(registry.mask(entity).empty());
            PPR_TEST_ASSERT(not registry.mask(recycled).empty());

            // The default handle is invalid and reads as dead everywhere.
            PPR_TEST_ASSERT(not Entity{}.isValid());
            PPR_TEST_ASSERT(not registry.isAlive(Entity{}));
            PPR_TEST_ASSERT(registry.mask(Entity{}).empty());
        };

        PPR_UNIT_TEST (multi_remove_keeps_columns_dense) {
            Registry registry;
            PPR_TEST_ASSERT(registry.registerComponent<Position>().has_value());

            Array<Entity> entities{};
            for (i32 index = 0; index < 5; ++index) {
                const Entity entity = registry.create();
                registry.emplace(entity, Position{index * 10, index * 10 + 1});
                entities.push_back(entity);
            }

            // Remove the head row then a middle row: both swap-remove paths run
            // back to back, so a stride-unaware truncation diverges here.
            PPR_TEST_ASSERT(registry.remove<Position>(entities[0]));
            PPR_TEST_ASSERT(registry.remove<Position>(entities[2]));

            PPR_TEST_ASSERT(registry.get<Position>(entities[0]) == nullptr);
            PPR_TEST_ASSERT(registry.get<Position>(entities[2]) == nullptr);
            PPR_TEST_ASSERT(registry.mask(entities[0]).empty());
            PPR_TEST_ASSERT(registry.mask(entities[2]).empty());

            for (const u32 survivor: {1u, 3u, 4u}) {
                const Position *const kept = registry.get<Position>(entities[survivor]);
                PPR_TEST_ASSERT(kept != nullptr);
                PPR_TEST_ASSERT(
                    kept->m_x == static_cast<i32>(survivor * 10u) and
                    kept->m_y == static_cast<i32>(survivor * 10u + 1u));
                PPR_TEST_ASSERT(not registry.mask(entities[survivor]).empty());
            }

            // The column must stay consistent across a later emplace/remove
            // cycle: a leaked tail would shift the appended row's offset so the
            // fresh row reads back stale bytes instead of its own value.
            const Entity late = registry.create();
            registry.emplace(late, Position{99, 100});

            const Position *const fresh = registry.get<Position>(late);
            PPR_TEST_ASSERT(fresh != nullptr);
            PPR_TEST_ASSERT(fresh->m_x == 99 and fresh->m_y == 100);

            for (const u32 survivor: {1u, 3u, 4u}) {
                const Position *const kept = registry.get<Position>(entities[survivor]);
                PPR_TEST_ASSERT(kept != nullptr);
                PPR_TEST_ASSERT(
                    kept->m_x == static_cast<i32>(survivor * 10u) and
                    kept->m_y == static_cast<i32>(survivor * 10u + 1u));
            }
            PPR_TEST_ASSERT(
                std::ranges::count_if(registry.view<Position>(), [](const auto &) { return true; }) == 4);

            PPR_TEST_ASSERT(registry.remove<Position>(late));
            PPR_TEST_ASSERT(registry.get<Position>(late) == nullptr);
            PPR_TEST_ASSERT(
                std::ranges::count_if(registry.view<Position>(), [](const auto &) { return true; }) == 3);

            for (const u32 survivor: {1u, 3u, 4u}) {
                const Position *const kept = registry.get<Position>(entities[survivor]);
                PPR_TEST_ASSERT(kept != nullptr);
                PPR_TEST_ASSERT(
                    kept->m_x == static_cast<i32>(survivor * 10u) and
                    kept->m_y == static_cast<i32>(survivor * 10u + 1u));
            }
        };
    } // namespace EcsSuite
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest ecs = UnitTest::Named("ecs") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::EcsSuite::lifecycle_recycles_slots_without_aliasing,
            detail::EcsSuite::clear_keeps_component_registration_and_resets_slots,
            detail::EcsSuite::view_visits_ascending_slot_order,
            detail::EcsSuite::unregistered_component_type_yields_empty_view,
            detail::EcsSuite::emplace_get_remove_round_trip,
            detail::EcsSuite::register_component_is_idempotent,
            detail::EcsSuite::dead_and_stale_entities_report_empty_mask,
            detail::EcsSuite::multi_remove_keeps_columns_dense,
        });
    };

    const UnitTest &ecsTests() noexcept {
        return ecs;
    }
} // namespace pP::tests
