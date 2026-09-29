module;
#include <box2d/box2d.h>
#include "pP/Macros.h"

module engine.physics;

import :scene;
import std;

namespace pP::physics {
    namespace {
        [[nodiscard]] std::error_code error(const std::errc code) noexcept {
            return std::make_error_code(code);
        }

        [[nodiscard]] bool validGeometry(const EGeometry geometry, const float width, const float height) noexcept {
            return (geometry == EGeometry::box or
                geometry == EGeometry::circle) and
                std::isfinite(width) and width > 0.0f and
                std::isfinite(height) and height > 0.0f;
        }

        [[nodiscard]] b2ShapeId makeShape(const b2BodyId body, const EGeometry geometry,
                                          const float width, const float height, const b2ShapeDef &definition) noexcept {
            if (geometry == EGeometry::circle) {
                const b2Circle circle{{0.0f, 0.0f}, width};
                return b2CreateCircleShape(body, &definition, &circle);
            }
            const b2Polygon box = b2MakeBox(width, height);
            return b2CreatePolygonShape(body, &definition, &box);
        }
    }

    struct Scene::Runtime {
        struct SensorShape {
            b2ShapeId m_shape{b2_nullShapeId};
            u32 m_kind{};
        };

        struct Entry {
            b2BodyId m_body{b2_nullBodyId};
            u64 m_serial{};
            std::vector<SensorShape> m_sensors;
        };

        b2WorldId m_world{b2_nullWorldId};
        float m_dt{};
        int m_substeps{};
        std::unordered_map<u64, Entry> m_bodies;
        std::vector<SensorEvent> m_events;
        bool m_contacts_drained{true};
        b2BodyId m_chain_body{b2_nullBodyId};
        std::unordered_map<u64, b2ChainId> m_chains;

        [[nodiscard]] Entry *find(const BodyHandle handle) noexcept {
            const auto it = m_bodies.find(handle.m_entity);
            const bool matching_serial = it != m_bodies.end() and it->second.m_serial == handle.m_serial;
            return matching_serial and
                handle.m_serial != 0u
                ? &it->second
                : nullptr;
        }

        [[nodiscard]] const Entry *find(const BodyHandle handle) const noexcept {
            const auto it = m_bodies.find(handle.m_entity);
            const bool matching_serial = it != m_bodies.end() and it->second.m_serial == handle.m_serial;
            return matching_serial and
                handle.m_serial != 0u
                ? &it->second
                : nullptr;
        }

        [[nodiscard]] u64 entityOf(const b2ShapeId shape) const noexcept {
            if (not b2Shape_IsValid(shape)) {
                return 0u;
            }
            const b2BodyId body = b2Shape_GetBody(shape);
            for (const auto &[entity, entry]: m_bodies) {
                if (B2_ID_EQUALS(entry.m_body, body)) {
                    return entity;
                }
            }
            return 0u;
        }

        [[nodiscard]] u32 kindOf(const b2ShapeId shape) const noexcept {
            for (const auto &[entity, entry]: m_bodies) {
                for (const SensorShape &sensor: entry.m_sensors) {
                    if (B2_ID_EQUALS(sensor.m_shape, shape)) {
                        return sensor.m_kind;
                    }
                }
            }
            return 0u;
        }
    };

    Scene::Scene() noexcept = default;

    Scene::~Scene() noexcept {
        (void) shutdown();
    }

    std::error_code Scene::initialize(const Desc desc) noexcept {
        const bool invalid_gravity = not std::isfinite(desc.m_gravity_x) or not std::isfinite(desc.m_gravity_y);
        const bool invalid_timestep = not std::isfinite(desc.m_fixed_dt) or desc.m_fixed_dt <= 0.0f;
        const bool invalid_substeps = desc.m_substeps == 0u or desc.m_substeps > static_cast<u32>(std::numeric_limits<int>::max());
        if (m_runtime or invalid_gravity or invalid_timestep or invalid_substeps) {
            return error(std::errc::invalid_argument);
        }

        std::unique_ptr<Runtime> runtime{new(std::nothrow) Runtime{}};
        if (not runtime) {
            return error(std::errc::not_enough_memory);
        }
        b2WorldDef definition = b2DefaultWorldDef();
        definition.gravity = {desc.m_gravity_x, desc.m_gravity_y};
        definition.enableSleep = desc.m_enable_sleep;
        runtime->m_world = b2CreateWorld(&definition);
        if (B2_IS_NULL(runtime->m_world)) {
            return error(std::errc::resource_unavailable_try_again);
        }
        runtime->m_dt = desc.m_fixed_dt;
        runtime->m_substeps = static_cast<int>(desc.m_substeps);
        m_runtime = std::move(runtime);
        return {};
    }

    std::error_code Scene::shutdown() noexcept {
        if (not m_runtime) {
            return {};
        }
        m_runtime->m_events.clear();
        m_runtime->m_bodies.clear();
        b2DestroyWorld(m_runtime->m_world);
        m_runtime.reset();
        return {};
    }

    std::error_code Scene::dispose() noexcept {
        return shutdown();
    }

    std::error_code Scene::clear() noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        m_runtime->m_events.clear();
        for (const auto &[entity, entry]: m_runtime->m_bodies) {
            b2DestroyBody(entry.m_body);
        }
        m_runtime->m_bodies.clear();
        return {};
    }

    std::error_code Scene::step() noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }

        m_runtime->m_events.clear();
        m_runtime->m_contacts_drained = false;
        b2World_Step(m_runtime->m_world, m_runtime->m_dt, m_runtime->m_substeps);
        const b2SensorEvents events = b2World_GetSensorEvents(m_runtime->m_world);

        try {
            m_runtime->m_events.reserve(static_cast<std::size_t>(events.beginCount + events.endCount));
            const auto append = [this](const b2ShapeId sensor, const b2ShapeId visitor, const bool begin) {
                const u64 owner = m_runtime->entityOf(sensor);
                const u64 other = m_runtime->entityOf(visitor);
                if (owner != 0u and other != 0u) {
                    m_runtime->m_events.push_back({owner, other, m_runtime->kindOf(sensor), begin});
                }
            };
            for (int i = 0; i < events.beginCount; ++i) {
                append(events.beginEvents[i].sensorShapeId, events.beginEvents[i].visitorShapeId, true);
            }
            for (int i = 0; i < events.endCount; ++i) {
                append(events.endEvents[i].sensorShapeId, events.endEvents[i].visitorShapeId, false);
            }
        } catch (const std::bad_alloc &) {
            m_runtime->m_events.clear();
            return error(std::errc::not_enough_memory);
        }
        return {};
    }

    std::error_code Scene::createBodies(const std::span<const u64> entities,
                                        const std::span<const BodyDefinition> definitions, const std::span<BodyHandle> handles) {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        const bool mismatched_definitions = entities.size() != definitions.size();
        const bool mismatched_handles = entities.size() != handles.size();
        if (mismatched_definitions or mismatched_handles) {
            return error(std::errc::invalid_argument);
        }
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const BodyDefinition &def = definitions[i];
            const bool invalid_entity = entities[i] == 0u or m_runtime->m_bodies.contains(entities[i]);
            const bool repeated_entity = std::find(entities.begin(), entities.begin() + i, entities[i]) != entities.begin() + i;
            const bool invalid_motion = def.m_motion > EMotion::dynamic;
            const bool invalid_geometry = not validGeometry(def.m_geometry, def.m_half_width, def.m_half_height);
            const bool invalid_position = not std::isfinite(def.m_x) or not std::isfinite(def.m_y);
            const bool invalid_rotation = not std::isfinite(def.m_angle) or not std::isfinite(def.m_angular_velocity);
            const bool invalid_velocity = not std::isfinite(def.m_velocity_x) or not std::isfinite(def.m_velocity_y);
            const bool invalid_density = not std::isfinite(def.m_density) or def.m_density <= 0.0f;
            const bool invalid_friction = not std::isfinite(def.m_friction) or def.m_friction < 0.0f;
            const bool invalid_restitution = not std::isfinite(def.m_restitution) or def.m_restitution < 0.0f;
            const bool invalid_body = invalid_motion or invalid_position or invalid_rotation or invalid_velocity;
            const bool invalid_shape = invalid_geometry or invalid_density or invalid_friction or invalid_restitution;
            const bool invalid_filter = def.m_category_bits == 0u;
            const bool invalid_serial = m_next_serial == 0u or entities.size() > std::numeric_limits<u64>::max() - m_next_serial;
            if (invalid_entity or repeated_entity or invalid_body or invalid_shape or invalid_filter or invalid_serial) {
                return error(std::errc::invalid_argument);
            }
        }

        std::size_t created{};
        std::error_code failure{};
        try {
            for (std::size_t i = 0; i < entities.size(); ++i) {
                const BodyDefinition &def = definitions[i];
                b2BodyDef body_def = b2DefaultBodyDef();
                body_def.type = static_cast<b2BodyType>(def.m_motion);
                body_def.position = {def.m_x, def.m_y};
                body_def.rotation = b2MakeRot(def.m_angle);
                body_def.linearVelocity = {def.m_velocity_x, def.m_velocity_y};
                body_def.angularVelocity = def.m_angular_velocity;
                body_def.enableSleep = def.m_enable_sleep;
                body_def.isAwake = def.m_is_awake;
                body_def.isBullet = def.m_is_bullet;
                body_def.fixedRotation = def.m_fixed_rotation;

                const b2BodyId body = b2CreateBody(m_runtime->m_world, &body_def);
                if (B2_IS_NULL(body)) {
                    failure = error(std::errc::resource_unavailable_try_again);
                    break;
                }
                b2ShapeDef shape_def = b2DefaultShapeDef();
                shape_def.density = def.m_density;
                shape_def.material.friction = def.m_friction;
                shape_def.material.restitution = def.m_restitution;
                shape_def.filter.categoryBits = def.m_category_bits;
                shape_def.filter.maskBits = def.m_mask_bits;
                shape_def.filter.groupIndex = def.m_group_index;
                shape_def.enableContactEvents = true;
                shape_def.enableHitEvents = true;
                // Box2D 3.1.1 requires the non-sensor visitor to opt in to sensor events too.
                shape_def.enableSensorEvents = true;
                const b2ShapeId shape = makeShape(body, def.m_geometry, def.m_half_width, def.m_half_height, shape_def);
                if (B2_IS_NULL(shape)) {
                    b2DestroyBody(body);
                    failure = error(std::errc::resource_unavailable_try_again);
                    break;
                }
                try {
                    m_runtime->m_bodies.emplace(entities[i], Runtime::Entry{body, m_next_serial});
                } catch (...) {
                    b2DestroyBody(body);
                    throw;
                }
                handles[i] = {entities[i], m_next_serial++};
                ++created;
            }
        } catch (const std::bad_alloc &) {
            failure = error(std::errc::not_enough_memory);
        }
        if (failure) {
            for (std::size_t i = 0; i < created; ++i) {
                (void) destroyBody(handles[i]);
                handles[i] = {};
            }
        }
        return failure;
    }

    std::error_code Scene::destroyBody(const BodyHandle handle) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return error(std::errc::invalid_argument);
        }
        b2DestroyBody(entry->m_body);
        m_runtime->m_bodies.erase(handle.m_entity);
        m_runtime->m_events.clear();
        return {};
    }

    std::expected<ChainHandle, std::error_code> Scene::createChunkChain(const ChainDefinition definition) noexcept {
        if (not m_runtime) {
            return std::unexpected{error(std::errc::operation_not_permitted)};
        }
        const bool invalid_serial = m_next_serial == 0u;
        const bool invalid_material = definition.m_material_count != 1u or definition.m_material_index != 0u;
        const bool too_few_points = definition.m_points.size() < (definition.m_loop ? 4u : 2u);
        const bool too_many_points = definition.m_points.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - 2);
        if (invalid_serial or invalid_material or too_few_points or too_many_points) {
            return std::unexpected{error(std::errc::invalid_argument)};
        }
        const auto valid = [](const ChainPoint p) {
            return std::isfinite(p.m_x) and std::isfinite(p.m_y);
        };
        if (not std::ranges::all_of(definition.m_points, valid) or
            (not definition.m_loop and (not valid(definition.m_ghost_before) or not valid(definition.m_ghost_after)))) {
            return std::unexpected{error(std::errc::invalid_argument)};
        }

        try {
            std::vector<b2Vec2> points;
            points.reserve(definition.m_points.size() + (definition.m_loop ? 0u : 2u));
            const bool open_chain = not definition.m_loop;
            if (open_chain) {
                points.push_back({definition.m_ghost_before.m_x, definition.m_ghost_before.m_y});
            }
            for (const ChainPoint point: definition.m_points) {
                points.push_back({point.m_x, point.m_y});
            }
            if (open_chain) {
                points.push_back({definition.m_ghost_after.m_x, definition.m_ghost_after.m_y});
            }

            for (std::size_t i = 1; i < points.size(); ++i) {
                const float dx = points[i].x - points[i - 1].x;
                const float dy = points[i].y - points[i - 1].y;
                if (dx * dx + dy * dy < 0.0001f) {
                    return std::unexpected{error(std::errc::invalid_argument)};
                }
            }
            if (definition.m_loop) {
                const float dx = points.front().x - points.back().x;
                const float dy = points.front().y - points.back().y;
                if (dx * dx + dy * dy < 0.0001f) {
                    return std::unexpected{error(std::errc::invalid_argument)};
                }
            }
            if (B2_IS_NULL(m_runtime->m_chain_body)) {
                const b2BodyDef body_def = b2DefaultBodyDef();
                m_runtime->m_chain_body = b2CreateBody(m_runtime->m_world, &body_def);
                if (B2_IS_NULL(m_runtime->m_chain_body)) {
                    return std::unexpected{error(std::errc::resource_unavailable_try_again)};
                }
            }

            b2SurfaceMaterial material{};
            material.friction = 0.6f;
            b2ChainDef chain_def = b2DefaultChainDef();
            chain_def.points = points.data();
            chain_def.count = static_cast<int>(points.size());
            chain_def.isLoop = definition.m_loop;
            chain_def.materials = &material;
            chain_def.materialCount = 1;
            const b2ChainId chain = b2CreateChain(m_runtime->m_chain_body, &chain_def);
            if (B2_IS_NULL(chain)) {
                return std::unexpected{error(std::errc::resource_unavailable_try_again)};
            }
            try {
                m_runtime->m_chains.emplace(m_next_serial, chain);
            } catch (...) {
                b2DestroyChain(chain);
                throw;
            }
            return ChainHandle{m_next_serial++};
        } catch (const std::bad_alloc &) {
            return std::unexpected{error(std::errc::not_enough_memory)};
        }
    }

    std::expected<ChainHandle, std::error_code> Scene::replaceChunkChain(const ChainHandle old,
                                                                         const ChainDefinition definition) noexcept {
        if (not m_runtime) {
            return std::unexpected{error(std::errc::operation_not_permitted)};
        }
        if (old.m_serial == 0u or not m_runtime->m_chains.contains(old.m_serial)) {
            return std::unexpected{error(std::errc::invalid_argument)};
        }
        auto replacement = createChunkChain(definition);
        if (not replacement) {
            return replacement;
        }
        const std::error_code destroyed = destroyChunkChain(old);
        PPR_ASSERT(not destroyed);
        if (destroyed) {
            (void) destroyChunkChain(*replacement);
            return std::unexpected{destroyed};
        }
        return replacement;
    }

    std::error_code Scene::destroyChunkChain(const ChainHandle handle) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        const auto it = m_runtime->m_chains.find(handle.m_serial);
        if (handle.m_serial == 0u or it == m_runtime->m_chains.end()) {
            return error(std::errc::invalid_argument);
        }
        b2DestroyChain(it->second);
        m_runtime->m_chains.erase(it);
        return {};
    }

    std::error_code Scene::attachSensor(const BodyHandle handle, const SensorDefinition definition) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return error(std::errc::invalid_argument);
        }
        const bool invalid_geometry = not validGeometry(definition.m_geometry, definition.m_half_width, definition.m_half_height);
        const bool invalid_filter = definition.m_category_bits == 0u;
        if (invalid_geometry or invalid_filter) {
            return error(std::errc::invalid_argument);
        }
        const bool sensor_disabled = not definition.m_enabled;
        if (sensor_disabled) {
            return {};
        }

        b2ShapeDef shape_def = b2DefaultShapeDef();
        shape_def.isSensor = true;
        shape_def.enableSensorEvents = true;
        shape_def.density = 0.0f;
        shape_def.filter.categoryBits = definition.m_category_bits;
        shape_def.filter.maskBits = definition.m_mask_bits;
        shape_def.filter.groupIndex = definition.m_group_index;
        const b2ShapeId shape = makeShape(entry->m_body, definition.m_geometry,
            definition.m_half_width, definition.m_half_height, shape_def);
        if (B2_IS_NULL(shape)) {
            return error(std::errc::resource_unavailable_try_again);
        }

        try {
            entry->m_sensors.push_back({shape, definition.m_kind});
        } catch (const std::bad_alloc &) {
            b2DestroyShape(shape, true);
            return error(std::errc::not_enough_memory);
        }
        return {};
    }

    std::error_code Scene::drainSensorEvents(const std::span<SensorEvent> destination, std::size_t &written) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        if (destination.size() < m_runtime->m_events.size()) {
            return error(std::errc::no_buffer_space);
        }
        std::ranges::copy(m_runtime->m_events, destination.begin());
        written = m_runtime->m_events.size();
        m_runtime->m_events.clear();
        return {};
    }

    std::error_code Scene::drainContactEvents(const std::span<ContactEvent> destination, std::size_t &written) noexcept {
        written = 0u;
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        if (m_runtime->m_contacts_drained) {
            return {};
        }

        const b2ContactEvents events = b2World_GetContactEvents(m_runtime->m_world);
        try {
            std::vector<ContactEvent> contacts;
            contacts.reserve(static_cast<std::size_t>(events.beginCount + events.endCount + events.hitCount));
            const auto append = [this, &contacts](const b2ShapeId shape_a, const b2ShapeId shape_b,
                                                  const b2Vec2 point, const b2Vec2 normal,
                                                  const bool begin, const bool hit, const float approach_speed) {
                const bool valid_a = b2Shape_IsValid(shape_a);
                const bool valid_b = b2Shape_IsValid(shape_b);
                if (not valid_a or not valid_b) {
                    return;
                }
                const b2BodyId body_a = b2Shape_GetBody(shape_a);
                const b2BodyId body_b = b2Shape_GetBody(shape_b);
                const u64 entity_a = m_runtime->entityOf(shape_a);
                const u64 entity_b = m_runtime->entityOf(shape_b);
                const bool chain_a = not B2_IS_NULL(m_runtime->m_chain_body) and B2_ID_EQUALS(body_a, m_runtime->m_chain_body);
                const bool chain_b = not B2_IS_NULL(m_runtime->m_chain_body) and B2_ID_EQUALS(body_b, m_runtime->m_chain_body);
                if ((entity_a == 0u and not chain_a) or (entity_b == 0u and not chain_b)) {
                    return;
                }
                const bool swap = entity_b < entity_a;
                contacts.push_back({
                    swap ? entity_b : entity_a, swap ? entity_a : entity_b,
                    point.x, point.y, swap ? -normal.x : normal.x, swap ? -normal.y : normal.y,
                    begin, hit, approach_speed
                });
            };
            for (int i = 0; i < events.beginCount; ++i) {
                const auto &event = events.beginEvents[i];
                const b2Vec2 point = event.manifold.pointCount > 0 ? event.manifold.points[0].point : b2Vec2{};
                append(event.shapeIdA, event.shapeIdB, point, event.manifold.normal, true, false, 0.0f);
            }
            for (int i = 0; i < events.endCount; ++i) {
                const auto &event = events.endEvents[i];
                append(event.shapeIdA, event.shapeIdB, {}, {}, false, false, 0.0f);
            }
            for (int i = 0; i < events.hitCount; ++i) {
                const auto &event = events.hitEvents[i];
                append(event.shapeIdA, event.shapeIdB, event.point, event.normal, false, true, event.approachSpeed);
            }
            std::ranges::sort(contacts, {}, [](const ContactEvent &event) {
                const int kind = event.m_hit ? 2 : event.m_begin ? 1 : 0;
                return std::tuple{
                    event.m_entity_a, event.m_entity_b, kind, event.m_x, event.m_y,
                    event.m_normal_x, event.m_normal_y, event.m_approach_speed
                };
            });
            if (destination.size() < contacts.size()) {
                return error(std::errc::no_buffer_space);
            }
            std::ranges::copy(contacts, destination.begin());
            written = contacts.size();
            m_runtime->m_contacts_drained = true;
            return {};
        } catch (const std::bad_alloc &) {
            return error(std::errc::not_enough_memory);
        }
    }

    std::error_code Scene::enableSleep(const BodyHandle handle, const bool enabled) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return error(std::errc::invalid_argument);
        }
        b2Body_EnableSleep(entry->m_body, enabled);
        return {};
    }

    std::error_code Scene::setAwake(const BodyHandle handle, const bool awake) noexcept {
        if (not m_runtime) {
            return error(std::errc::operation_not_permitted);
        }
        Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return error(std::errc::invalid_argument);
        }
        b2Body_SetAwake(entry->m_body, awake);
        return {};
    }

    std::expected<bool, std::error_code> Scene::isAwake(const BodyHandle handle) const noexcept {
        if (not m_runtime) {
            return std::unexpected{error(std::errc::operation_not_permitted)};
        }
        const Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return std::unexpected{error(std::errc::invalid_argument)};
        }
        return b2Body_IsAwake(entry->m_body);
    }

    std::expected<BodyState, std::error_code> Scene::state(const BodyHandle handle) const noexcept {
        if (not m_runtime) {
            return std::unexpected{error(std::errc::operation_not_permitted)};
        }
        const Runtime::Entry *const entry = m_runtime->find(handle);
        if (not entry) {
            return std::unexpected{error(std::errc::invalid_argument)};
        }
        const b2Vec2 position = b2Body_GetPosition(entry->m_body);
        const b2Vec2 velocity = b2Body_GetLinearVelocity(entry->m_body);
        return BodyState{
            position.x, position.y, b2Rot_GetAngle(b2Body_GetRotation(entry->m_body)),
            velocity.x, velocity.y, b2Body_GetAngularVelocity(entry->m_body)
        };
    }
}
