module;

#include "Colony.Elements.h"

module game.colony.driver;

import engine.core;
import engine.physics;
import engine.sim;
import game.colony;
import game.colony.agents;
import game.colony.buildings;
import game.colony.digtool;
import game.colony.pathfinding;
import game.colony.translator;

import std;

namespace pP::colony {
    namespace {
        constexpr u32 kMaxSlices = 5u;
        constexpr std::string_view kToolStep = "tool";
        constexpr std::string_view kBuildStep = "build";
        constexpr std::string_view kPathfindStep = "pathfind";
        constexpr std::string_view kAiPlanStep = "aiplan";
        constexpr std::string_view kMoveStep = "movestep";
        constexpr std::string_view kPhysicsStep = "physics";
        constexpr u32 kColliderBudget = 2u;
        constexpr u32 kSpawnColliderBudget = 64u;

        [[nodiscard]] Expected<sim::Entity> recordErrand(sim::Registry &registry, const Footprint &print) {
            const sim::Entity entity = registry.create();
            registry.emplace<Footprint>(entity, print);
            registry.emplace<BuildErrand>(entity,
                BuildErrand{.m_print = print, .m_total = print.m_width * print.m_height});
            return entity;
        }

        [[nodiscard]] physics::BodyDefinition mirrorDefinition(
            const sim::BodyState &pose, const sim::BodyDefinition &def, const float vx, const float vy) noexcept {
            physics::BodyDefinition out{};
            out.m_motion = def.m_motion == sim::EMotionKind::dynamic
                               ? physics::EMotion::dynamic
                               : def.m_motion == sim::EMotionKind::kinematic
                                     ? physics::EMotion::kinematic
                                     : physics::EMotion::static_body;
            out.m_geometry = def.m_geometry == sim::EBodyGeometry::circle
                                 ? physics::EGeometry::circle
                                 : physics::EGeometry::box;
            out.m_x = pose.m_x;
            out.m_y = pose.m_y;
            out.m_angle = pose.m_angle;
            out.m_velocity_x = vx;
            out.m_velocity_y = vy;
            out.m_half_width = def.m_half_width;
            out.m_half_height = def.m_half_height;
            out.m_density = def.m_density;
            out.m_friction = def.m_friction;
            out.m_restitution = def.m_restitution;
            out.m_category_bits = def.m_category_bits;
            out.m_mask_bits = def.m_mask_bits;
            out.m_group_index = def.m_group_index;
            out.m_enable_sleep = def.m_enable_sleep;
            out.m_is_awake = def.m_is_awake;
            out.m_is_bullet = def.m_is_bullet;
            out.m_fixed_rotation = def.m_fixed_rotation;
            return out;
        }

        [[nodiscard]] bool chunkListedU32(const std::span<const sim::ChunkPos> listed, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos entry: listed) {
                if (entry == pos) {
                    return true;
                }
            }
            return false;
        }

        void agentNeighbourhood(sim::Registry &registry, Array<sim::ChunkPos> &out) {
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose == nullptr or pose->m_x < 0.0f or pose->m_y < 0.0f) {
                    continue;
                }

                const sim::ChunkPos home{
                    static_cast<u32>(pose->m_x) / sim::kChunkEdge,
                    static_cast<u32>(pose->m_y) / sim::kChunkEdge
                };
                if (not sim::isInsideChunkGrid(home)) {
                    continue;
                }

                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = static_cast<i32>(home.m_x) + dx;
                        const i32 ny = static_cast<i32>(home.m_y) + dy;
                        if (nx < 0 or
                            ny < 0 or
                            static_cast<u32>(nx) >= sim::kChunksPerEdge or
                            static_cast<u32>(ny) >= sim::kChunksPerEdge) {
                            continue;
                        }
                        const sim::ChunkPos near{static_cast<u32>(nx), static_cast<u32>(ny)};
                        if (not chunkListedU32(out, near)) {
                            out.push_back(near);
                        }
                    }
                }
            }
        }

        [[nodiscard]] u64 cellChunkDistanceSquared(
            const u32 cell_x, const u32 cell_y, const sim::ChunkPos chunk) noexcept {
            const i64 min_x = static_cast<i64>(chunk.m_x) * static_cast<i64>(sim::kChunkEdge);
            const i64 min_y = static_cast<i64>(chunk.m_y) * static_cast<i64>(sim::kChunkEdge);
            const i64 max_x = min_x + static_cast<i64>(sim::kChunkEdge) - 1;
            const i64 max_y = min_y + static_cast<i64>(sim::kChunkEdge) - 1;
            const i64 ix = static_cast<i64>(cell_x);
            const i64 iy = static_cast<i64>(cell_y);

            i64 dx = 0;
            if (ix < min_x) {
                dx = min_x - ix;
            } else if (ix > max_x) {
                dx = ix - max_x;
            }
            i64 dy = 0;
            if (iy < min_y) {
                dy = min_y - iy;
            } else if (iy > max_y) {
                dy = iy - max_y;
            }
            return static_cast<u64>(dx * dx + dy * dy);
        }
    }

    std::error_code ColonyDriver::init(const ColonyDriverDesc &desc) {
        if (desc.m_tick_hz == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        const TimeSpan period = std::chrono::duration_cast<TimeSpan>(std::chrono::seconds{1}) /
                                static_cast<TimeSpan::rep>(desc.m_tick_hz);
        if (period <= TimeSpan{}) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        const std::error_code shutdown_error = shutdown();
        if (shutdown_error) [[unlikely]] {
            return shutdown_error;
        }

        const std::error_code init_error = m_colony.init(ColonyDesc{desc.m_seed});
        if (init_error) [[unlikely]] {
            // A failed generation must not expose a partially written world.
            (void) m_colony.shutdown();
            return init_error;
        }

        m_timestep.emplace(period, kMaxSlices, sim::EOverrunPolicy::clamp);
        if (const std::error_code error = registerColonyComponents(m_registry)) [[unlikely]] {
            (void) m_colony.shutdown();
            m_timestep.reset();
            return error;
        }
        if (not m_steps_bound) {
            // Declaration order is execution order: tool edits, build
            // errands, search, plan, move, then physics integration and
            // state mirror.
            const Expected<u32> tool = m_steps.declare(kToolStep);
            if (not tool) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return tool.error();
            }
            const Expected<u32> build = m_steps.declare(kBuildStep);
            if (not build) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return build.error();
            }
            const Expected<u32> declared = m_steps.declare(kPathfindStep);
            if (not declared) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return declared.error();
            }
            const Expected<u32> aiplan = m_steps.declare(kAiPlanStep);
            if (not aiplan) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return aiplan.error();
            }
            const Expected<u32> movestep = m_steps.declare(kMoveStep);
            if (not movestep) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return movestep.error();
            }
            const Expected<u32> physics = m_steps.declare(kPhysicsStep);
            if (not physics) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return physics.error();
            }
            const Expected<void> bound_tool = m_steps.addSystem(*tool, [this](sim::Registry &) {
                m_step_stage = "tool";
                applyToolEdits();
            });
            if (not bound_tool) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_tool.error();
            }
            const Expected<void> bound_build = m_steps.addSystem(*build, [this](sim::Registry &registry) {
                m_step_stage = "build";
                Array<sim::ChunkPos> touched{};
                advanceErrands(m_colony.grid(), registry, m_finder, touched, kErrandCellsPerTick);
                fanOutEditChunks(touched);
            });
            if (not bound_build) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_build.error();
            }
            const Expected<void> bound = m_steps.addSystem(*declared, [this](sim::Registry &registry) {
                m_step_stage = "pathfind";
                stepPathfinding(m_colony.grid(), registry, m_finder, kExpansionsPerTick);
            });
            if (not bound) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound.error();
            }
            const Expected<void> bound_plan = m_steps.addSystem(*aiplan, [this](sim::Registry &registry) {
                m_step_stage = "aiplan";
                planAgent(m_colony.grid(), registry);
            });
            if (not bound_plan) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_plan.error();
            }
            const Expected<void> bound_move = m_steps.addSystem(*movestep, [this](sim::Registry &) {
                m_step_stage = "movestep";
                moveAgents();
            });
            if (not bound_move) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_move.error();
            }
            const Expected<void> bound_physics = m_steps.addSystem(*physics, [this](sim::Registry &) {
                m_step_stage = "physics";
                stepPhysics();
            });
            if (not bound_physics) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_physics.error();
            }
            m_steps_bound = true;
        }
        if (const std::error_code error = initPhysics()) [[unlikely]] {
            (void) shutdown();
            return error;
        }
        if (const std::error_code error = spawnColonyAgents()) [[unlikely]] {
            (void) shutdown();
            return error;
        }
        m_seed = desc.m_seed;
        return {};
    }

    std::error_code ColonyDriver::shutdown() {
        m_timestep.reset();
        shutdownPhysics();
        m_registry.clear();
        m_finder = Pathfinder{};
        clearToolState();
        m_counts = PathCounts{};
        m_errands = 0u;
        m_agents = Array<AgentSummary>{};
        m_step_error = std::error_code{};
        m_step_stage = "";
        const std::error_code error = m_colony.shutdown();
        m_seed = 0u;
        m_tick_count = 0u;
        m_speed = 1u;
        m_paused = false;
        return error;
    }

    std::error_code ColonyDriver::update(const TimeSpan elapsed) {
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (elapsed < TimeSpan{}) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (m_paused) {
            // Paused ticks apply the pending tool intent only: no systems,
            // no physics, no plan advance, and no clock movement.
            applyToolEdits();
            if (m_step_error) {
                return m_step_error;
            }
            return {};
        }

        const auto speed = static_cast<TimeSpan::rep>(m_speed);
        if (elapsed > TimeSpan::max() / speed) [[unlikely]] {
            return std::make_error_code(std::errc::value_too_large);
        }

        // Cap only the whole-slice portion, retaining the exact fractional
        // remainder. This prevents overflow when a stalled frame is enormous.
        TimeSpan scaled = elapsed * speed;
        const TimeSpan budget = m_timestep->period() * static_cast<TimeSpan::rep>(kMaxSlices);
        if (scaled > budget) {
            scaled = budget + scaled % m_timestep->period();
        }

        const auto report = m_timestep->advance(scaled);
        if (not report) [[unlikely]] {
            return report.error();
        }

        // Each committed fixed slice runs the simulation systems once; a
        // system failure aborts the tick count before it advances.
        for (u32 slice = 0u; slice < report->m_slices; ++slice) {
            const std::error_code system_error = m_steps.runSystems(m_registry);
            if (system_error) [[unlikely]] {
                m_counts = pP::colony::pathCounts(m_registry);
                m_errands = liveErrandCount(m_registry);
                refreshAgents();
                return system_error;
            }
            if (m_step_error) [[unlikely]] {
                m_counts = pP::colony::pathCounts(m_registry);
                m_errands = liveErrandCount(m_registry);
                refreshAgents();
                return m_step_error;
            }
        }

        m_tick_count += report->m_slices;
        m_counts = pP::colony::pathCounts(m_registry);
        m_errands = liveErrandCount(m_registry);
        refreshAgents();
        return {};
    }

    void ColonyDriver::setPaused(const bool paused) noexcept {
        m_paused = paused;
    }

    void ColonyDriver::togglePaused() noexcept {
        m_paused = not m_paused;
    }

    std::error_code ColonyDriver::stepOne() noexcept {
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (not m_paused) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        const std::error_code system_error = m_steps.runSystems(m_registry);
        if (system_error) [[unlikely]] {
            return system_error;
        }
        if (m_step_error) [[unlikely]] {
            return m_step_error;
        }

        ++m_tick_count;
        m_counts = pP::colony::pathCounts(m_registry);
        m_errands = liveErrandCount(m_registry);
        refreshAgents();
        return {};
    }

    std::error_code ColonyDriver::setSpeed(const u32 speed) noexcept {
        if (speed < 1u or speed > 3u)
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_speed = speed;
        return {};
    }

    std::error_code ColonyDriver::regenerate(const u64 seed) {
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        m_timestep->reset();
        m_tick_count = 0u;
        m_seed = 0u;
        shutdownPhysics();
        m_registry.clear();
        m_finder = Pathfinder{};
        clearToolState();
        m_counts = PathCounts{};
        m_errands = 0u;
        m_agents = Array<AgentSummary>{};
        m_step_error = std::error_code{};
        m_step_stage = "";
        const std::error_code shutdown_error = m_colony.shutdown();
        if (shutdown_error) [[unlikely]] {
            m_timestep.reset();
            return shutdown_error;
        }

        const std::error_code init_error = m_colony.init(ColonyDesc{seed});
        if (init_error) [[unlikely]] {
            (void) m_colony.shutdown();
            m_timestep.reset();
            return init_error;
        }

        if (const std::error_code error = initPhysics()) [[unlikely]] {
            (void) shutdown();
            return error;
        }
        if (const std::error_code error = spawnColonyAgents()) [[unlikely]] {
            (void) shutdown();
            return error;
        }
        m_seed = seed;
        return {};
    }

    sim::ChunkGrid &ColonyDriver::grid() noexcept {
        return m_colony.grid();
    }

    const sim::ChunkGrid &ColonyDriver::grid() const noexcept {
        return m_colony.grid();
    }

    u64 ColonyDriver::seed() const noexcept {
        return m_seed;
    }

    u64 ColonyDriver::tickCount() const noexcept {
        return m_tick_count;
    }

    double ColonyDriver::simMs() const noexcept {
        if (not m_timestep) {
            return 0.0;
        }
        const std::chrono::duration<double, std::milli> milliseconds{m_timestep->period()};
        return static_cast<double>(m_tick_count) * milliseconds.count();
    }

    bool ColonyDriver::paused() const noexcept {
        return m_paused;
    }

    u32 ColonyDriver::speed() const noexcept {
        return m_speed;
    }

    sim::Registry &ColonyDriver::registry() noexcept {
        return m_registry;
    }

    const sim::Registry &ColonyDriver::registry() const noexcept {
        return m_registry;
    }

    PathCounts ColonyDriver::pathCounts() const noexcept {
        return m_counts;
    }

    u32 ColonyDriver::errandCount() const noexcept {
        return m_errands;
    }

    Expected<sim::Entity> ColonyDriver::buildWall(const Footprint &print) {
        if (not m_timestep) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        if (const std::error_code error = validateFootprint(print)) {
            return std::unexpected{error};
        }
        const auto entity = recordErrand(m_registry, print);
        if (not entity) {
            return std::unexpected{entity.error()};
        }
        m_errands = liveErrandCount(m_registry);
        return entity;
    }

    Expected<sim::Entity> ColonyDriver::demolish(const sim::GlobalCellPos min, const u32 width, const u32 height) {
        if (not m_timestep) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        // Demolition writes vacuum explicitly (never rely on the default).
        const Footprint print{
            .m_min = min, .m_width = width, .m_height = height, .m_element = kElementVacuum, .m_solid = false
        };
        if (const std::error_code error = validateFootprint(print)) {
            return std::unexpected{error};
        }
        const auto entity = recordErrand(m_registry, print);
        if (not entity) {
            return std::unexpected{entity.error()};
        }
        m_errands = liveErrandCount(m_registry);
        return entity;
    }

    std::error_code ColonyDriver::requestDig(const sim::GlobalCellPos min, const u32 width, const u32 height) {
        if (not m_timestep) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (width == 0u or height == 0u or width > kMaxDigEdge or height > kMaxDigEdge) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 far_x = static_cast<u64>(min.m_x) + width;
        const u64 far_y = static_cast<u64>(min.m_y) + height;
        if (far_x > sim::kWorldEdge or far_y > sim::kWorldEdge) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        m_pending_dig = PendingDig{.m_min = min, .m_width = width, .m_height = height};
        return {};
    }

    u32 ColonyDriver::digTotal() const noexcept {
        return m_dig_stats.m_dug_total;
    }

    std::span<const sim::ChunkPos> ColonyDriver::lastEditChunks() const noexcept {
        return std::span<const sim::ChunkPos>{m_dig_stats.m_last_chunks.data(), m_dig_stats.m_last_chunks.size()};
    }

    Expected<sim::Entity> ColonyDriver::requestPath(
        const sim::GlobalCellPos from, const sim::GlobalCellPos to, const u32 caps) {
        if (not m_timestep) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        if (not sim::isInsideWorld(from) or
            not sim::isInsideWorld(to) or
            (caps & kPathCapsWalk) == 0u or
            (caps & ~kPathCapsWalk) != 0u) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        const sim::Entity entity = m_registry.create();
        m_registry.emplace<PathReq>(entity, PathReq{.m_entity = entity, .m_from = from, .m_to = to, .m_caps = caps});
        return entity;
    }

    std::error_code ColonyDriver::presentEdits(ColonyTranslator &translator) noexcept {
        // Presentation owns m_present_dirty only; the collider sets drain in
        // stepPhysics so cleared collider entries always reach a rebuild.
        std::error_code first_error{};
        for (const sim::ChunkPos pos: m_present_dirty) {
            if (const std::error_code error = translator.markChunkChanged(pos)) {
                if (not first_error) {
                    first_error = error;
                }
            }
        }
        m_present_dirty = Array<sim::ChunkPos>{};
        return first_error;
    }

    void ColonyDriver::applyToolEdits() noexcept {
        if (not m_pending_dig) {
            return;
        }

        const PendingDig pending = *m_pending_dig;
        m_pending_dig.reset();
        if (const std::error_code error = applyDig(pending)) {
            if (not m_step_error) {
                m_step_error = error;
            }
        }
    }

    std::error_code ColonyDriver::applyDig(const PendingDig &pending) {
        const DigRect rect{.m_min = pending.m_min, .m_width = pending.m_width, .m_height = pending.m_height};

        const Expected<DigResult> result = digCells(m_colony.grid(), rect);
        if (not result) {
            return result.error();
        }

        const bool changed = result->m_dug > 0u;
        if (not changed) {
            return {};
        }

        fanOutEditChunks(result->m_touched);
        m_finder = Pathfinder{};
        m_dig_stats.m_dug_total += result->m_dug;
        return wakeBodiesNearEdit(result->m_touched);
    }

    std::error_code ColonyDriver::wakeBodiesNearEdit(const std::span<const sim::ChunkPos> touched) noexcept {
        if (touched.empty()) {
            return {};
        }

        // Units: one physics world unit covers one cell, so the cell-space
        // point-to-chunk distance compares directly against kDigWakeCells.
        const u64 radius_squared = static_cast<u64>(kDigWakeCells) * static_cast<u64>(kDigWakeCells);
        std::error_code first_error{};
        for (const AgentBody &entry: m_bodies) {
            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entry.m_entity);
            if (pose == nullptr or pose->m_x < 0.0f or pose->m_y < 0.0f) {
                continue;
            }

            const u32 cell_x = static_cast<u32>(pose->m_x);
            const u32 cell_y = static_cast<u32>(pose->m_y);
            bool within_wake = false;
            for (const sim::ChunkPos chunk: touched) {
                if (cellChunkDistanceSquared(cell_x, cell_y, chunk) <= radius_squared) {
                    within_wake = true;
                    break;
                }
            }
            if (not within_wake) {
                continue;
            }

            if (const std::error_code error = m_scene.setAwake(entry.m_handle, true)) {
                if (not first_error) {
                    first_error = error;
                }
            }
        }
        return first_error;
    }

    void ColonyDriver::fanOutEditChunks(const std::span<const sim::ChunkPos> touched) {
        for (const sim::ChunkPos pos: touched) {
            if (not chunkListedU32(m_present_dirty, pos)) {
                m_present_dirty.push_back(pos);
            }
            if (not chunkListedU32(m_collider_dirty, pos)) {
                m_collider_dirty.push_back(pos);
            }
        }

        if (touched.empty()) {
            return;
        }
        m_dig_stats.m_last_chunks = Array<sim::ChunkPos>{};
        for (const sim::ChunkPos pos: touched) {
            m_dig_stats.m_last_chunks.push_back(pos);
        }
    }

    void ColonyDriver::clearToolState() noexcept {
        m_present_dirty = Array<sim::ChunkPos>{};
        m_collider_dirty = Array<sim::ChunkPos>{};
        m_pending_dig.reset();
        m_dig_stats.m_dug_total = 0u;
        m_dig_stats.m_last_chunks = Array<sim::ChunkPos>{};
    }

    std::error_code ColonyDriver::initPhysics() {
        if (m_physics_ready) {
            return {};
        }
        constexpr physics::Scene::Desc desc{
            .m_gravity_x = 0.0f,
            .m_gravity_y = -9.8f,
            .m_fixed_dt = 1.0f / 60.0f,
            .m_substeps = 4u,
            .m_enable_sleep = false
        };
        if (const std::error_code error = m_scene.initialize(desc)) {
            return error;
        }
        m_physics_ready = true;
        return {};
    }

    void ColonyDriver::shutdownPhysics() noexcept {
        if (not m_physics_ready) {
            return;
        }
        m_bodies = Array<AgentBody>{};
        (void) m_scene.clear();
        (void) m_scene.shutdown();
        (void) m_scene.dispose();
        m_colliders = physics::ChunkColliders{};
        m_collider_covered = Array<sim::ChunkPos>{};
        m_physics_ready = false;
    }

    std::error_code ColonyDriver::spawnColonyAgents() {
        if (const std::error_code error = spawnAgents(m_colony.grid(), m_registry, m_seed, kAgentCount)) {
            return error;
        }
        if (const std::error_code error = createAgentBodies()) {
            return error;
        }
        refreshAgents();
        return {};
    }

    std::error_code ColonyDriver::createAgentBodies() {
        m_bodies = Array<AgentBody>{};
        Array<u64> keys{};
        Array<physics::BodyDefinition> definitions{};
        Array<sim::Entity> owners{};

        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            (void) agent;
            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entity);
            const sim::BodyDefinition *definition = m_registry.get<sim::BodyDefinition>(entity);
            const Agent *identity = m_registry.get<Agent>(entity);
            if (pose == nullptr or definition == nullptr or identity == nullptr) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            keys.push_back(identity->m_key);
            definitions.push_back(
                mirrorDefinition(*pose, *definition, pose->m_velocity_x, pose->m_velocity_y));
            owners.push_back(entity);
        }

        if (not keys.empty()) {
            Array<physics::BodyHandle> handles{};
            handles.resize(keys.size());
            if (const std::error_code error = m_scene.createBodies(
                std::span<const u64>{keys.data(), keys.size()},
                std::span<const physics::BodyDefinition>{definitions.data(), definitions.size()},
                std::span<physics::BodyHandle>{handles.data(), handles.size()})) {
                return error;
            }
            for (std::size_t index = 0u; index < owners.size(); ++index) {
                const Agent *identity = m_registry.get<Agent>(owners[index]);
                const sim::BodyState *restored = m_registry.get<sim::BodyState>(owners[index]);
                m_bodies.push_back(AgentBody{
                    .m_entity = owners[index],
                    .m_key = identity != nullptr ? identity->m_key : 0u,
                    .m_handle = handles[index],
                    .m_velocity_x = restored != nullptr ? restored->m_velocity_x : 0.0f,
                    .m_velocity_y = restored != nullptr ? restored->m_velocity_y : 0.0f
                });
            }
        }
        // Materialize the spawn neighbourhood synchronously so agents never
        // free-fall through unbuilt chains on their first ticks.
        Array<sim::ChunkPos> coverage{};
        agentNeighbourhood(m_registry, coverage);

        return rebuildChunks(coverage, coverage, kSpawnColliderBudget);
    }

    void ColonyDriver::refreshAgents() {
        m_agents = Array<AgentSummary>{};
        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            (void) agent;
            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entity);
            if (pose == nullptr) {
                continue;
            }
            const Plan *plan = m_registry.get<Plan>(entity);
            AgentSummary row{};
            row.m_x = pose->m_x;
            row.m_y = pose->m_y;
            row.m_has_plan = plan != nullptr;
            if (plan != nullptr) {
                row.m_goal = static_cast<u32>(plan->m_goal);
                row.m_pc = plan->m_pc;
                row.m_replans = plan->m_replans;
            }
            m_agents.push_back(row);
        }
    }

    void ColonyDriver::moveAgents() noexcept {
        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            (void) agent;
            Intent intent{};
            if (const std::error_code error = agentIntent(m_registry, entity, intent)) {
                (void) error;
                continue;
            }

            u32 entry = 0u;
            bool known = false;
            for (u32 index = 0u; index < m_bodies.size(); ++index) {
                if (m_bodies[index].m_entity == entity) {
                    entry = index;
                    known = true;
                    break;
                }
            }
            if (known and
                m_bodies[entry].m_velocity_x == intent.m_velocity_x and
                m_bodies[entry].m_velocity_y == intent.m_velocity_y) {
                continue;
            }

            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entity);
            const sim::BodyDefinition *definition = m_registry.get<sim::BodyDefinition>(entity);
            const Agent *identity = m_registry.get<Agent>(entity);
            if (pose == nullptr or definition == nullptr or identity == nullptr) {
                if (not m_step_error) {
                    m_step_error = std::make_error_code(std::errc::invalid_argument);
                }
                continue;
            }

            if (known) {
                if (const std::error_code error = m_scene.destroyBody(m_bodies[entry].m_handle)) {
                    if (not m_step_error) {
                        m_step_error = error;
                    }
                    continue;
                }
            }
            const u64 key = identity->m_key;
            const physics::BodyDefinition body = mirrorDefinition(*pose, *definition, intent.m_velocity_x, intent.m_velocity_y);
            const u64 keys[1] = {key};
            const physics::BodyDefinition bodies[1] = {body};
            physics::BodyHandle made[1] = {};

            if (const std::error_code error = m_scene.createBodies(std::span<const u64>{keys, 1u},
                std::span<const physics::BodyDefinition>{bodies, 1u},
                std::span<physics::BodyHandle>{made, 1u})) {
                if (not m_step_error) {
                    m_step_error = error;
                }
                continue;
            }

            if (known) {
                m_bodies[entry].m_handle = made[0u];
                m_bodies[entry].m_velocity_x = intent.m_velocity_x;
                m_bodies[entry].m_velocity_y = intent.m_velocity_y;
            } else {
                m_bodies.push_back(AgentBody{
                    .m_entity = entity,
                    .m_key = key,
                    .m_handle = made[0u],
                    .m_velocity_x = intent.m_velocity_x,
                    .m_velocity_y = intent.m_velocity_y
                });
            }
        }
    }

    void ColonyDriver::mirrorBodyStates() noexcept {
        for (const AgentBody &entry: m_bodies) {
            const auto state = m_scene.state(entry.m_handle);
            sim::BodyState *pose = m_registry.get<sim::BodyState>(entry.m_entity);
            if (not state or pose == nullptr) {
                if (not m_step_error) {
                    m_step_error = state ? std::make_error_code(std::errc::invalid_argument) : state.error();
                }
                continue;
            }
            pose->m_x = state->m_x;
            pose->m_y = state->m_y;
            pose->m_angle = state->m_angle;
            pose->m_velocity_x = state->m_velocity_x;
            pose->m_velocity_y = state->m_velocity_y;
            pose->m_angular_velocity = state->m_angular_velocity;
        }
    }

    void ColonyDriver::stepPhysics() noexcept {
        if (not m_physics_ready) {
            return;
        }
        Array<sim::ChunkPos> needed{};
        agentNeighbourhood(m_registry, needed);
        m_step_stage = "colliders";
        if (const std::error_code error = rebuildChunks(m_collider_dirty, needed, kColliderBudget)) {
            if (not m_step_error) {
                m_step_error = error;
            }
            return;
        }
        // Split ownership: m_collider_dirty drains here on success only, so a
        // failed rebuild retains it for retry; presentEdits never touches it.
        m_collider_dirty = Array<sim::ChunkPos>{};
        m_step_stage = "scene-step";
        if (const std::error_code error = m_scene.step()) {
            if (not m_step_error) {
                m_step_error = error;
            }
            return;
        }
        m_step_stage = "mirror";
        mirrorBodyStates();
    }

    std::error_code ColonyDriver::rebuildChunks(
        std::span<const sim::ChunkPos> dirty, std::span<const sim::ChunkPos> needed, const u32 max) {
        Array<sim::ChunkPos> listed{};
        const auto include = [&listed](const sim::ChunkPos pos) {
            if (not chunkListedU32(listed, pos)) {
                listed.push_back(pos);
            }
        };
        for (const sim::ChunkPos pos: dirty) {
            include(pos);
        }
        for (const sim::ChunkPos pos: needed) {
            include(pos);
        }
        // The collider queue persists across calls: covered views are
        // maintained by rebuildChunks only (cleared when nothing pends), and
        // m_collider_dirty drains in stepPhysics only — never in presentEdits.
        for (const sim::ChunkPos pos: m_collider_covered) {
            include(pos);
        }
        const u32 listed_count = static_cast<u32>(listed.size());
        for (u32 index = 0u; index < listed_count; ++index) {
            const sim::ChunkPos pos = listed[index];
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 nx = static_cast<i32>(pos.m_x) + dx;
                    const i32 ny = static_cast<i32>(pos.m_y) + dy;
                    if (nx < 0 or
                        ny < 0 or
                        static_cast<u32>(nx) >= sim::kChunksPerEdge or
                        static_cast<u32>(ny) >= sim::kChunksPerEdge) {
                        continue;
                    }
                    include(sim::ChunkPos{static_cast<u32>(nx), static_cast<u32>(ny)});
                }
            }
        }
        std::ranges::sort(listed, {}, [](const sim::ChunkPos pos) {
            return pos.m_y * sim::kChunksPerEdge + pos.m_x;
        });
        Array<Array<u16> > pool{};
        Array<physics::ColliderChunkView> views{};
        for (const sim::ChunkPos pos: listed) {
            const std::optional<std::span<const sim::Cell> > cells = m_colony.grid().residentCells(pos);
            Array<u16> elements{};
            elements.resize(sim::kCellsPerChunk);
            if (cells) {
                for (u32 cell = 0u; cell < sim::kCellsPerChunk; ++cell) {
                    elements[cell] = (*cells)[cell].m_element;
                }
            } else {
                std::fill(elements.data(), elements.data() + elements.size(), u16{});
            }
            const bool wanted = chunkListedU32(needed, pos);
            pool.push_back(std::move(elements));
            const Array<u16> &stored = pool[pool.size() - 1u];
            views.push_back(physics::ColliderChunkView{
                .m_pos = physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)},
                .m_elements = std::span<const u16>{stored.data(), stored.size()},
                .m_needed = wanted
            });
        }
        Array<physics::ColliderChunkPos> dirty_i32{};
        for (const sim::ChunkPos pos: dirty) {
            dirty_i32.push_back(
                physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)});
        }
        u32 processed = 0u;
        const std::error_code rebuilt = m_colliders.rebuildDirtyChunks(m_scene,
            std::span<const physics::ColliderChunkView>{views.data(), views.size()},
            std::span<const physics::ColliderChunkPos>{dirty_i32.data(), dirty_i32.size()},
            sim::kChunkEdge, max, processed);
        if (m_colliders.pendingCount() == 0u) {
            m_collider_covered = Array<sim::ChunkPos>{};
        } else {
            m_collider_covered = std::move(listed);
        }
        return rebuilt;
    }

    std::span<const AgentSummary> ColonyDriver::agentSummaries() const noexcept {
        return std::span<const AgentSummary>{m_agents.data(), m_agents.size()};
    }

    std::error_code ColonyDriver::restoreBodies() {
        if (not m_physics_ready) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        (void) m_scene.clear();
        m_colliders = physics::ChunkColliders{};
        m_collider_covered = Array<sim::ChunkPos>{};
        clearToolState();
        if (const std::error_code error = createAgentBodies()) {
            return error;
        }
        m_finder = Pathfinder{};
        refreshAgents();
        m_counts = pP::colony::pathCounts(m_registry);
        m_errands = liveErrandCount(m_registry);
        return {};
    }

    std::error_code ColonyDriver::rebuildReplayColliders(const sim::Snapshot &snapshot) {
        clearToolState();
        Array<sim::ChunkPos> dirty{};
        for (const sim::ChunkDelta &delta: snapshot.m_deltas) {
            const sim::ChunkPos pos{delta.m_chunk % sim::kChunksPerEdge, delta.m_chunk / sim::kChunksPerEdge};
            if (not chunkListedU32(dirty, pos)) {
                dirty.push_back(pos);
            }
        }
        Array<sim::ChunkPos> needed{};
        agentNeighbourhood(m_registry, needed);
        // Listed below can add a 3x3 ring per entry; budget past it so the
        // call always completes fully (or reports) with no remainder.
        const u64 listed_max = (dirty.size() + needed.size()) * 9u + 1u;
        if (const std::error_code error = rebuildChunks(dirty, needed, safe_narrowing(listed_max))) {
            return error;
        }
        if (m_colliders.pendingCount() != 0u) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        return {};
    }

    sim::Snapshot ColonyDriver::snapshotWithBodies() {
        mirrorBodyStates();
        sim::Snapshot snap = sim::capture(m_colony.grid(), m_registry, m_seed);
        for (auto &record: snap.m_bodies.m_records) {
            for (const AgentBody &entry: m_bodies) {
                if (entry.m_entity == record.m_entity) {
                    const auto awake = m_scene.isAwake(entry.m_handle);
                    if (awake) {
                        record.m_awake = *awake;
                    }
                    break;
                }
            }
        }
        snap.m_bodies.m_replay_ticks = m_tick_count;
        return snap;
    }

    std::size_t ColonyDriver::chainCount(const sim::ChunkPos pos) const noexcept {
        return m_colliders.chainCount(
            physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)});
    }

    std::size_t ColonyDriver::colliderPending() const noexcept {
        return m_colliders.pendingCount();
    }
}
