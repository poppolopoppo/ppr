module;

#include "Colony.Elements.h"
#include "pP/Macros.h"

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
import game.colony.worldgen;
import game.colony.metric;
import game.colony.content;

import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyPreparation, info, none)

    // ------------------------------------------------------------------
    // driver helpers
    // ------------------------------------------------------------------

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
            registry.emplace<BuildErrand>(
                entity, BuildErrand{.m_print = print, .m_total = print.m_width * print.m_height});
            return entity;
        }

        [[nodiscard]] physics::BodyDefinition mirrorDefinition(
            const sim::BodyState &pose, const sim::BodyDefinition &def, const float vx, const float vy) noexcept {
            physics::BodyDefinition out{};
            out.m_motion = def.m_motion == sim::EMotionKind::dynamic     ? physics::EMotion::dynamic
                           : def.m_motion == sim::EMotionKind::kinematic ? physics::EMotion::kinematic
                                                                         : physics::EMotion::static_body;
            out.m_geometry = def.m_geometry == sim::EBodyGeometry::circle ? physics::EGeometry::circle
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

        [[nodiscard]] bool chunkListedU32(
            const std::span<const sim::ChunkPos> listed, const sim::ChunkPos pos) noexcept {
            for (const sim::ChunkPos entry: listed) {
                if (entry == pos) {
                    return true;
                }
            }
            return false;
        }

        void agentNeighbourhood(sim::Registry &registry, Array<sim::ChunkPos> &out, const u32 cells_per_metre) {
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose == nullptr) {
                    continue;
                }
                const auto cell = metreToFineCell(
                    {static_cast<double>(pose->m_x), static_cast<double>(pose->m_y)}, cells_per_metre);
                if (not cell) {
                    continue;
                }
                const sim::ChunkPos home{cell->m_x / sim::kChunkEdge, cell->m_y / sim::kChunkEdge};
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

    // ------------------------------------------------------------------
    // driver initialization
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::initLegacyFixture(
        const ColonyDriverDesc &desc, const sim::GenerationExecution *const execution) {
        if (desc.m_tick_hz == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (desc.m_agent_count > kAgentCountMax) [[unlikely]] {
            return std::make_error_code(std::errc::value_too_large);
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

        const hal::ProfileScope scope_worldgen{"Colony.WorldGen"};
        const std::error_code init_error = m_colony.initLegacyFixture(ColonyDesc{desc.m_seed}, execution);
        if (init_error) [[unlikely]] {
            // A failed generation must not expose a partially written world.
            (void) m_colony.shutdown();
            return init_error;
        }

        std::error_code result = initializeRuntime(desc, period, execution);
        if (result) {
            PPR_RETAIN_ERROR_ON_FAIL(ColonyPreparation, result, shutdown());
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, result);
        }
        return {};
    }

    std::error_code ColonyDriver::initPrepared(
        const ColonyDriverDesc &desc, PreparedWorld world, const sim::GenerationExecution *const execution) {
        if (desc.m_tick_hz == 0u or desc.m_seed != world.descriptor().m_seed or desc.m_agent_count != 1u) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
        }
        const TimeSpan period = TimeSpan{std::chrono::seconds{1}} / desc.m_tick_hz;
        if (period <= TimeSpan{}) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
        }

        PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, shutdown());
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation, m_colony.install(std::move(world), nullptr, execution ? execution->m_context : nullptr));

        std::error_code result = initializeRuntime(desc, period, execution);
        if (result) {
            PPR_RETAIN_ERROR_ON_FAIL(ColonyPreparation, result, shutdown());
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, result);
        }
        return {};
    }

    // ------------------------------------------------------------------
    // driver metadata
    // ------------------------------------------------------------------

    const PreparedWorld *ColonyDriver::preparedWorld() const noexcept {
        return m_colony.preparedWorld();
    }

    sim::MatterParticleSnapshot ColonyDriver::captureParticles() const {
        return m_colony.particles().capture();
    }

    std::span<const sim::MatterParticle> ColonyDriver::particleChunk(const sim::ChunkPos position) const noexcept {
        return m_colony.particles().chunk(position);
    }

    u32 ColonyDriver::particleCount() const noexcept {
        return m_colony.particles().count();
    }

    std::span<const sim::MatterParticleBatch> ColonyDriver::editParticles() const noexcept {
        return editReadyForPresentation() ? std::span<const sim::MatterParticleBatch>{m_edit->m_particles}
                                          : std::span<const sim::MatterParticleBatch>{};
    }

    bool ColonyDriver::initialMatterBaselineValid() const noexcept {
        return m_initial_matter_baseline_valid;
    }

    const ContentCatalog *ColonyDriver::contentCatalog() const noexcept {
        const auto *const prepared = preparedWorld();
        return prepared ? &prepared->catalog() : nullptr;
    }

    u16 ColonyDriver::boundaryMaterial() const noexcept {
        const auto *const prepared = preparedWorld();
        if (prepared == nullptr) {
            return 1u;
        }
        const auto *const profile = prepared->catalog().findWorld(prepared->descriptor().m_profile);
        PPR_ASSERT(profile != nullptr);
        return profile->m_default_wall_material_id;
    }

    u32 ColonyDriver::cellsPerMetre() const noexcept {
        return preparedWorld() ? kFineCellsPerMetre : 1u;
    }

    bool ColonyDriver::sampleSurfaceHeights(const std::span<float> heights) const noexcept {
        PPR_ASSERT(heights.size() == sim::kWorldEdge);
        const auto *const prepared = preparedWorld();
        if (prepared == nullptr) {
            return false;
        }
        const auto &plan = prepared->generated().m_plan;
        const auto &recipe = plan.recipe();
        if (not recipe.m_open_surface) {
            return false;
        }

        for (u32 column = 0u; column < sim::kWorldEdge; ++column) {
            const double x_m = (static_cast<double>(column) + 0.5) * recipe.m_cell_size_m;
            heights[column] = static_cast<float>(sim::generationSurfaceHeight(plan, x_m));
        }
        return true;
    }

    double ColonyDriver::referenceThicknessMetres() const noexcept {
        const auto *const prepared = preparedWorld();
        return prepared ? prepared->descriptor().m_metric.m_reference_thickness_m : 1.0;
    }

    std::string_view ColonyDriver::profileKey() const noexcept {
        const auto *const prepared = preparedWorld();
        return prepared ? std::string_view{prepared->descriptor().m_profile} : std::string_view{};
    }

    // ------------------------------------------------------------------
    // runtime setup
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::initializeRuntime(
        const ColonyDriverDesc &desc, const TimeSpan period, const sim::GenerationExecution *const execution,
        const sim::Snapshot *const restored) {
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
                if (m_resume_edit_tick or m_edit or m_step_error) {
                    return;
                }
                if (m_require_edit_publication or preparedWorld() != nullptr) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonyPreparation, m_step_error, prepareEdit(true));
                } else {
                    applyToolEdits();
                }
            });
            if (not bound_tool) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_tool.error();
            }
            const Expected<void> bound_build = m_steps.addSystem(*build, [this](sim::Registry &registry) {
                if (m_require_edit_publication or preparedWorld() != nullptr) {
                    return;
                }
                const hal::ProfileScope scope_build{"Colony.Build"};
                if (not m_step_error) {
                    m_step_stage = "build";
                }
                Array<sim::ChunkPos> touched{};
                const auto *const prepared = preparedWorld();
                PPR_RETAIN_ERROR_ON_FAIL(
                    ColonyPreparation, m_step_error,
                    advanceErrands(
                        m_colony.grid(), registry, m_finder, touched, kErrandCellsPerTick,
                        prepared ? &prepared->catalog() : nullptr));
                fanOutEditChunks(touched);
            });
            if (not bound_build) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_build.error();
            }
            const Expected<void> bound = m_steps.addSystem(*declared, [this](sim::Registry &registry) {
                if (m_edit) {
                    return;
                }
                const hal::ProfileScope scope_pathfind{"Colony.Pathfind"};
                if (not m_step_error) {
                    m_step_stage = "pathfind";
                }
                stepPathfinding(m_colony.grid(), registry, m_finder, kExpansionsPerTick);
            });
            if (not bound) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound.error();
            }
            const Expected<void> bound_plan = m_steps.addSystem(*aiplan, [this](sim::Registry &registry) {
                if (m_edit) {
                    return;
                }
                const hal::ProfileScope scope_plan{"Colony.Plan"};
                if (not m_step_error) {
                    m_step_stage = "aiplan";
                }
                planAgent(m_colony.grid(), registry, m_controlled, cellsPerMetre());
            });
            if (not bound_plan) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_plan.error();
            }
            const Expected<void> bound_move = m_steps.addSystem(*movestep, [this](sim::Registry &) {
                if (m_edit) {
                    return;
                }
                if (not m_step_error) {
                    m_step_stage = "movestep";
                }
                moveAgents();
            });
            if (not bound_move) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_move.error();
            }
            const Expected<void> bound_physics = m_steps.addSystem(*physics, [this](sim::Registry &) {
                if (m_edit) {
                    return;
                }
                if (not m_step_error) {
                    m_step_stage = "physics";
                }
                stepPhysics();
            });
            if (not bound_physics) [[unlikely]] {
                (void) m_colony.shutdown();
                m_timestep.reset();
                return bound_physics.error();
            }
            m_steps_bound = true;
        }

        const IContext *const context = execution ? execution->m_context.get() : nullptr;
        if (context) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, context->error());
        }

        if (const std::error_code error = initPhysics()) [[unlikely]] {
            (void) shutdown();
            return error;
        }

        m_agent_count = desc.m_agent_count == 0u ? kAgentCount : desc.m_agent_count;
        m_seed = desc.m_seed;
        m_tick_hz = desc.m_tick_hz;

        if (restored) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, sim::restore(m_registry, *restored));
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, createAgentBodies(context));
            for (const auto &record: restored->m_bodies.m_records) {
                const auto body = std::ranges::find(m_bodies, record.m_entity, &AgentBody::m_entity);
                PPR_RETURN_ERROR_ON_FAIL(
                    ColonyPreparation,
                    body != m_bodies.end() ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, m_scene.setAwake(body->m_handle, record.m_awake));
            }
            refreshAgents();
        } else if (const std::error_code error = spawnColonyAgents(context)) [[unlikely]] {
            (void) shutdown();
            return error;
        }
        m_seed = desc.m_seed;
        m_world_generation = desc.m_world_generation != 0u ? desc.m_world_generation : m_world_generation + 1u;
        return {};
    }

    std::error_code ColonyDriver::shutdown() {
        m_controlled.reset();
        clearPlayerCommand();
        m_timestep.reset();
        m_held_slices = 0u;
        m_resume_edit_tick = false;
        m_single_step_pending = false;
        shutdownPhysics();
        m_edit.reset();
        m_require_edit_publication = false;
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
        m_agent_count = kAgentCount;
        m_speed = 1u;
        m_paused = false;
        m_initial_matter_baseline_valid = true;
        return error;
    }

    // ------------------------------------------------------------------
    // tick control
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::update(const TimeSpan elapsed) {
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (elapsed < TimeSpan{}) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (m_edit) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, prepareEditColliders());
            return {};
        }

        if (m_single_step_pending and m_held_slices == 0u) {
            m_held_slices = 1u;
        }
        if (m_held_slices != 0u) {
            if (m_paused and not m_single_step_pending) {
                // Hold simulation slices, while preserving paused tool edits.
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, prepareEdit(false));
                return {};
            }
            return runHeldTicks();
        }

        if (m_paused) {
            if (m_require_edit_publication or preparedWorld() != nullptr) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, prepareEdit(false));
                return {};
            }
            // Paused ticks apply the pending tool intent only: no systems,
            // no physics, no plan advance, and no clock movement.
            applyToolEdits();
            if (m_step_error) {
                return m_step_error;
            }
            return {};
        }

        const hal::ProfileScope scope_update{"Colony.Update"};

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

        m_held_slices = report->m_slices;
        return runHeldTicks();
    }

    void ColonyDriver::setPaused(const bool paused) noexcept {
        m_paused = paused;
        clearPlayerCommand();
    }

    void ColonyDriver::togglePaused() noexcept {
        setPaused(not m_paused);
    }

    std::error_code ColonyDriver::assignControl(const std::optional<sim::Entity> entity) {
        if (not m_timestep) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::operation_not_permitted);
        }

        if (entity) {
            Intent intent{};
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, playerIntent(m_registry, *entity, {}, intent, cellsPerMetre()));
            const auto slot = entity->m_index;
            if (slot >= m_body_by_slot.size() or
                m_body_by_slot[slot] == none_v or
                m_body_by_slot[slot] >= m_bodies.size() or
                m_bodies[m_body_by_slot[slot]].m_entity != *entity) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
            }
            (void) m_registry.remove<Plan>(*entity);
            (void) m_registry.remove<PathReq>(*entity);
            (void) m_registry.remove<PathComp>(*entity);
        }
        m_controlled = entity;
        clearPlayerCommand();
        return {};
    }

    std::optional<sim::Entity> ColonyDriver::controlledEntity() const noexcept {
        return m_controlled;
    }

    std::error_code ColonyDriver::setPlayerCommand(const PlayerCommand command) noexcept {
        if (not m_timestep or not m_controlled) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::operation_not_permitted);
        }
        if (command.m_horizontal < -1 or
            command.m_horizontal > 1 or
            command.m_vertical < -1 or
            command.m_vertical > 1) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
        }

        m_player_command = m_paused ? PlayerCommand{} : command;
        return {};
    }

    void ColonyDriver::clearPlayerCommand() noexcept {
        m_player_command = {};
    }

    std::error_code ColonyDriver::stepOne() noexcept {
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (not m_paused) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }

        m_single_step_pending = true;
        if (m_edit) {
            return prepareEditColliders();
        }
        if (m_held_slices == 0u) {
            m_held_slices = 1u;
        }
        return runHeldTicks();
    }

    std::error_code ColonyDriver::setSpeed(const u32 speed) noexcept {
        if (speed < 1u or speed > 3u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_speed = speed;
        return {};
    }

    // ------------------------------------------------------------------
    // world replacement
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::regenerateLegacyFixture(const u64 seed) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation,
            not m_edit ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        // Prepared worlds are replaced transactionally through WorldJob.
        if (preparedWorld()) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::operation_not_permitted);
        }
        if (not m_timestep) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        m_controlled.reset();
        clearPlayerCommand();
        m_timestep->reset();
        m_tick_count = 0u;
        m_seed = 0u;
        shutdownPhysics();
        m_registry.clear();
        m_finder = Pathfinder{};
        clearToolState();
        const hal::ProfileScope scope_worldgen{"Colony.WorldGen"};
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

        const std::error_code init_error = m_colony.initLegacyFixture(ColonyDesc{seed});
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
        ++m_world_generation;
        return {};
    }

    std::error_code ColonyDriver::regenerateLegacyFixture(const u64 seed, const u32 agent_count) {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation,
            not m_edit ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        if (agent_count > kAgentCountMax) [[unlikely]] {
            return std::make_error_code(std::errc::value_too_large);
        }
        m_agent_count = agent_count == 0u ? kAgentCount : agent_count;
        return regenerateLegacyFixture(seed);
    }

    // ------------------------------------------------------------------
    // driver queries
    // ------------------------------------------------------------------

    sim::ChunkGrid &ColonyDriver::grid() noexcept {
        return m_colony.grid();
    }

    const sim::ChunkGrid &ColonyDriver::grid() const noexcept {
        return m_colony.grid();
    }

    u64 ColonyDriver::seed() const noexcept {
        return m_seed;
    }

    u64 ColonyDriver::worldGeneration() const noexcept {
        return m_world_generation;
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

    // ------------------------------------------------------------------
    // construction tools
    // ------------------------------------------------------------------

    Expected<sim::Entity> ColonyDriver::buildWall(const Footprint &print) {
        if (not m_timestep) {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }

        Footprint resolved = print;
        if (const auto *const prepared = preparedWorld()) {
            const auto *const profile = prepared->catalog().findWorld(prepared->descriptor().m_profile);
            PPR_ASSERT(profile != nullptr);
            resolved.m_element = resolved.m_solid ? profile->m_default_wall_material_id : u16{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyPreparation, validateConstruction(m_colony.grid(), m_registry, resolved, prepared->catalog()));
        } else {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyPreparation, validateFootprint(resolved));
        }

        const auto entity = recordErrand(m_registry, resolved);
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
            .m_min = min, .m_width = width, .m_height = height, .m_element = kElementVacuum, .m_solid = false};
        const auto *const prepared = preparedWorld();

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyPreparation, validateFootprint(print, prepared ? &prepared->catalog() : nullptr));

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

        if (m_pending_digs.size() >= kMaxPendingDigs) {
            m_pending_digs.erase(m_pending_digs.begin());
        }
        m_pending_digs.push_back(PendingDig{.m_min = min, .m_width = width, .m_height = height});
        return {};
    }

    u32 ColonyDriver::digTotal() const noexcept {
        return m_dig_stats.m_dug_total;
    }

    std::span<const sim::ChunkPos> ColonyDriver::lastEditChunks() const noexcept {
        return std::span<const sim::ChunkPos>{m_dig_stats.m_last_chunks.data(), m_dig_stats.m_last_chunks.size()};
    }

    std::span<const ColonyDriver::DirtyChunk> ColonyDriver::presentEditsCapture() const noexcept {
        return std::span<const ColonyDriver::DirtyChunk>{m_present_dirty.data(), m_present_dirty.size()};
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
        const hal::ProfileScope scope_invalidate{"Colony.Presentation.Invalidate"};
        // Presentation owns m_present_dirty only; the collider sets drain in
        // stepPhysics so cleared collider entries always reach a rebuild.
        std::error_code first_error{};

        for (const DirtyChunk &row: m_present_dirty) {
            if (const std::error_code error = translator.markChunkChanged(row.m_pos)) {
                if (not first_error) {
                    first_error = error;
                }
            }
        }

        m_present_dirty = Array<DirtyChunk>{};
        return first_error;
    }

    void ColonyDriver::ackPresentChunks(const std::span<const DirtyChunk> acked) noexcept {
        if (m_edit or acked.empty() or m_present_dirty.empty()) {
            return;
        }

        Array<DirtyChunk> kept{};
        kept.reserve(m_present_dirty.size());

        for (const DirtyChunk &pending: m_present_dirty) {
            bool acknowledged = false;
            for (const DirtyChunk &done: acked) {
                if (done.m_pos == pending.m_pos and done.m_revision == pending.m_revision) {
                    acknowledged = true;
                    break;
                }
            }
            if (not acknowledged) {
                kept.push_back(pending);
            }
        }

        m_present_dirty = std::move(kept);
    }

    // ------------------------------------------------------------------
    // tool application
    // ------------------------------------------------------------------

    void ColonyDriver::applyToolEdits() noexcept {
        const hal::ProfileScope scope_tool{"Colony.Tool"};
        if (m_pending_digs.empty()) {
            return;
        }

        const PendingDig pending = m_pending_digs.front();
        m_pending_digs.erase(m_pending_digs.begin());

        if (const std::error_code error = applyDig(pending)) {
            if (not m_step_error) {
                m_step_error = error;
            }
        }
    }

    std::error_code ColonyDriver::applyDig(const PendingDig &pending) {
        const DigRect rect{.m_min = pending.m_min, .m_width = pending.m_width, .m_height = pending.m_height};

        const auto *const prepared = preparedWorld();
        const Expected<DigResult> result = digCells(m_colony.grid(), rect, prepared ? &prepared->catalog() : nullptr);
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
            if (pose == nullptr) {
                continue;
            }
            const auto position =
                metreToFineCell({static_cast<double>(pose->m_x), static_cast<double>(pose->m_y)}, cellsPerMetre());
            if (not position) {
                continue;
            }
            const u32 cell_x = position->m_x;
            const u32 cell_y = position->m_y;
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
            DirtyChunk *existing = nullptr;
            for (DirtyChunk &row: m_present_dirty) {
                if (row.m_pos == pos) {
                    existing = &row;
                    break;
                }
            }
            DirtyChunk *history = nullptr;
            for (DirtyChunk &row: m_present_revision_history) {
                if (row.m_pos == pos) {
                    history = &row;
                    break;
                }
            }
            if (existing != nullptr) {
                existing->m_revision += 1u;
            } else {
                // A row that was acked away restarts from its LAST revision
                // (never 1): revision is chunk identity across removals, so
                // a stale ack for revision N can never match the new N.
                const u64 base = history != nullptr ? history->m_revision : 0u;
                m_present_dirty.push_back(DirtyChunk{.m_pos = pos, .m_revision = base + 1u});
                existing = &m_present_dirty.back();
            }
            if (history != nullptr) {
                history->m_revision = existing->m_revision;
            } else {
                m_present_revision_history.push_back(DirtyChunk{.m_pos = pos, .m_revision = existing->m_revision});
            }
            if (not chunkListedU32(m_collider_dirty, pos)) {
                m_collider_dirty.push_back(pos);
            }
        }

        if (touched.empty()) {
            return;
        }
        // Initial matter describes the generated geometry, never a live solver.
        m_initial_matter_baseline_valid = false;
        // New dirt means grid cells changed, so any cached views are stale.
        invalidateColliderCache();
        m_dig_stats.m_last_chunks = Array<sim::ChunkPos>{};
        for (const sim::ChunkPos pos: touched) {
            m_dig_stats.m_last_chunks.push_back(pos);
        }
    }

    void ColonyDriver::clearToolState() noexcept {
        m_present_dirty = Array<DirtyChunk>{};
        m_present_revision_history = Array<DirtyChunk>{};
        m_collider_dirty = Array<sim::ChunkPos>{};
        m_pending_digs.clear();
        m_dig_stats.m_dug_total = 0u;
        m_dig_stats.m_last_chunks = Array<sim::ChunkPos>{};
        invalidateColliderCache();
    }

    void ColonyDriver::invalidateColliderCache() noexcept {
        // H3a Slice 6: KEPT bump — every caller is real content change
        // (non-empty absorb in fanOutEditChunks; restore/reinit clears).
        ++m_cover_gen;
        m_cache_valid = false;
    }

    // ------------------------------------------------------------------
    // physics lifecycle
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::initPhysics() {
        if (m_physics_ready) {
            return {};
        }

        if (preparedWorld()) {
            m_colliders = physics::ChunkColliders{
                {static_cast<float>(kFineCellMetres), 0.0f, 0.0f, sim::kWorldEdge, sim::kWorldEdge}};
        }
        constexpr physics::Scene::Desc desc{
            .m_gravity_x = 0.0f,
            .m_gravity_y = -9.8f,
            .m_fixed_dt = 1.0f / 60.0f,
            .m_substeps = 4u,
            .m_enable_sleep = false};
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
        // The cohort borrows the Scene and must finish before its runtime dies.
        if (m_edit and m_edit->m_colliders_started) {
            PPR_LOG_WARNING_ON_FAIL(ColonyPreparation, m_colliders.cancelCohort(m_scene));
            m_edit->m_colliders_started = false;
        }
        m_bodies = Array<AgentBody>{};
        (void) m_scene.clear();
        (void) m_scene.shutdown();
        (void) m_scene.dispose();
        m_colliders = physics::ChunkColliders{};
        m_collider_covered = Array<sim::ChunkPos>{};
        m_cached_views = Array<physics::ColliderChunkView>{};
        m_cached_pool = Array<Array<u16>>{};
        m_cached_listed = Array<sim::ChunkPos>{};
        m_cached_needed = Array<sim::ChunkPos>{};
        invalidateColliderCache();
        m_physics_ready = false;
    }

    std::error_code ColonyDriver::spawnColonyAgents(const IContext *const context) {
        std::optional<sim::Entity> controlled;

        if (const auto *const prepared = preparedWorld()) {
            if (context) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, context->error());
            }
            const auto &start = prepared->generated().m_start;
            const auto cell = metreToFineCell({start.m_spawn_x_m, start.m_spawn_y_m});
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, cell);
            const auto entity = m_registry.create();
            const u64 key = (static_cast<u64>(entity.m_generation) << 32u) | entity.m_index;
            m_registry.emplace<Agent>(entity, Agent{.m_key = key, .m_spawn = *cell});
            m_registry.emplace<Needs>(entity, Needs{});
            m_registry.emplace<sim::BodyState>(
                entity, sim::BodyState{static_cast<float>(start.m_spawn_x_m), static_cast<float>(start.m_spawn_y_m)});
            m_registry.emplace<sim::BodyDefinition>(
                entity, sim::BodyDefinition{
                            .m_motion = sim::EMotionKind::dynamic,
                            .m_geometry = sim::EBodyGeometry::box,
                            .m_half_width = static_cast<float>(kWorldCompileParameters.m_actor_half_width_m),
                            .m_half_height = static_cast<float>(kWorldCompileParameters.m_actor_half_height_m),
                            .m_fixed_rotation = true});
            controlled = entity;
        } else {
            PPR_RETURN_ERROR_ON_FAIL(
                ColonyPreparation, spawnAgents(m_colony.grid(), m_registry, m_seed, m_agent_count, context));
        }

        if (const std::error_code error = createAgentBodies(context)) {
            return error;
        }
        if (controlled) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, assignControl(controlled));
        }
        refreshAgents();
        return {};
    }

    std::error_code ColonyDriver::createAgentBodies(const IContext *const context) {
        m_bodies = Array<AgentBody>{};
        Array<u64> keys{};
        Array<physics::BodyDefinition> definitions{};
        Array<sim::Entity> owners{};

        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            if (context) {
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, context->error());
            }
            (void) agent;
            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entity);
            const sim::BodyDefinition *definition = m_registry.get<sim::BodyDefinition>(entity);
            const Agent *identity = m_registry.get<Agent>(entity);
            if (pose == nullptr or definition == nullptr or identity == nullptr) {
                return std::make_error_code(std::errc::invalid_argument);
            }
            keys.push_back(identity->m_key);
            definitions.push_back(mirrorDefinition(*pose, *definition, pose->m_velocity_x, pose->m_velocity_y));
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
                m_bodies.push_back(
                    AgentBody{
                        .m_entity = owners[index],
                        .m_key = identity != nullptr ? identity->m_key : 0u,
                        .m_handle = handles[index],
                        .m_velocity_x = restored != nullptr ? restored->m_velocity_x : 0.0f,
                        .m_velocity_y = restored != nullptr ? restored->m_velocity_y : 0.0f});
            }
        }
        // Materialize the spawn neighbourhood synchronously so agents never
        // free-fall through unbuilt chains on their first ticks.
        // Rebuild the slot-indexed body lookup wholesale (restoreBodies
        // inherits this via createAgentBodies).
        m_body_by_slot.assign(m_registry.slotCount(), none_v);
        for (u32 index = 0u; index < m_bodies.size(); ++index) {
            m_body_by_slot[m_bodies[index].m_entity.m_index] = index;
        }

        Array<sim::ChunkPos> coverage{};
        agentNeighbourhood(m_registry, coverage, cellsPerMetre());

        return rebuildChunks(coverage, coverage, kSpawnColliderBudget, context);
    }

    void ColonyDriver::refreshAgents() {
        m_agents = Array<AgentSummary>{};

        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            const sim::BodyState *pose = m_registry.get<sim::BodyState>(entity);
            if (pose == nullptr) {
                continue;
            }
            const Plan *plan = m_registry.get<Plan>(entity);
            AgentSummary row{};
            row.m_key = agent.m_key;
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

    std::error_code ColonyDriver::moveAgent(const sim::Entity entity) noexcept {
        Intent intent{};
        const auto resolved = m_controlled == entity
                                  ? playerIntent(m_registry, entity, m_player_command, intent, cellsPerMetre())
                                  : agentIntent(m_registry, entity, intent, cellsPerMetre());
        PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, resolved);

        const u32 slot = entity.m_index;
        const bool indexed = slot < m_body_by_slot.size() and m_body_by_slot[slot] != none_v;
        const u32 entry = indexed ? m_body_by_slot[slot] : 0u;
        if (not indexed or entry >= m_bodies.size() or m_bodies[entry].m_entity != entity) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
        }

        AgentBody &body = m_bodies[entry];
        const auto state = m_scene.state(body.m_handle);
        if (not state) {
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, state.error());
        }

        // Horizontal locomotion preserves falling momentum. Ladder intent
        // owns vertical speed only while gravity is explicitly disabled.
        const float velocity_y = intent.m_climbing ? intent.m_velocity_y : state->m_velocity_y;
        PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, m_scene.setGravityEnabled(body.m_handle, not intent.m_climbing));
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation, m_scene.setLinearVelocity(body.m_handle, intent.m_velocity_x, velocity_y));
        body.m_velocity_x = intent.m_velocity_x;
        body.m_velocity_y = velocity_y;
        return {};
    }

    void ColonyDriver::moveAgents() noexcept {
        const hal::ProfileScope scope_move{"Colony.Move"};
        // Finish independent actors while retaining the first tick failure.
        for (const auto &[entity, agent]: m_registry.view<Agent>()) {
            (void) agent;
            PPR_RETAIN_ERROR_ON_FAIL(ColonyPreparation, m_step_error, moveAgent(entity));
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

    void ColonyDriver::collectColliderInterest(Array<sim::ChunkPos> &needed) {
        agentNeighbourhood(m_registry, needed, cellsPerMetre());
    }

    void ColonyDriver::stepPhysics() noexcept {
        const hal::ProfileScope scope_physics{"Colony.Physics"};
        if (not m_physics_ready) {
            return;
        }
        Array<sim::ChunkPos> needed{};
        agentNeighbourhood(m_registry, needed, cellsPerMetre());

        if (not m_step_error) {
            m_step_stage = "colliders";
        }
        if (const std::error_code error = rebuildChunks(m_collider_dirty, needed, kColliderBudget)) {
            if (not m_step_error) {
                m_step_error = error;
            }
            return;
        }

        // Split ownership: m_collider_dirty drains here on success only, so a
        // failed rebuild retains it for retry; presentEdits never touches it.
        m_collider_dirty = Array<sim::ChunkPos>{};

        if (not m_step_error) {
            m_step_stage = "scene-step";
        }
        {
            const hal::ProfileScope scope_step{"Colony.Physics.Step"};
            if (const std::error_code error = m_scene.step()) {
                if (not m_step_error) {
                    m_step_error = error;
                }
                return;
            }
        }

        if (not m_step_error) {
            m_step_stage = "mirror";
        }
        {
            const hal::ProfileScope scope_mirror{"Colony.Physics.Mirror"};
            mirrorBodyStates();
        }
    }

    // ------------------------------------------------------------------
    // collider preparation
    // ------------------------------------------------------------------

    std::error_code ColonyDriver::copyColliderChunk(const sim::ChunkPos position, Array<u16> &occupancy) const {
        occupancy.assign(sim::kCellsPerChunk, u16{});
        const auto cells = grid().residentCells(position);
        if (not cells) {
            return {};
        }
        const auto *const prepared = preparedWorld();

        for (u32 index = 0u; index < sim::kCellsPerChunk; ++index) {
            const auto id = (*cells)[index].m_element;
            if (id == 0u) {
                continue;
            }
            if (prepared) {
                const auto *const material = prepared->catalog().findMaterial(id);
                if (not material or
                    material->m_representation != EMaterialRepresentation::terrain or
                    material->m_phase != EMaterialPhase::solid) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, std::errc::invalid_argument);
                }
            }
            occupancy[index] = 1u;
        }
        return {};
    }

    Expected<bool> ColonyDriver::buildColliderViewsDelta(
        std::span<const sim::ChunkPos> sorted_needed, std::span<const sim::ChunkPos> dirty,
        Array<sim::ChunkPos> &listed_out, Array<Array<u16>> &pool_out, Array<physics::ColliderChunkView> &views_out) {
        // Fallback: no reusable history. Init/shutdown/reinit clear the pool
        // (shutdownPhysics) and invalidate, so the canonical full preparation
        // below runs for every restore/reinit. A dirt invalidation never clears
        // the pool, so small-delta MISSes always reach the delta path.
        if (m_cached_pool.empty()) {
            return false;
        }
        if (m_cached_pool.size() != m_cached_listed.size() or m_cached_views.size() != m_cached_listed.size()) {
            return false;
        }
        for (const Array<u16> &buffer: m_cached_pool) {
            if (buffer.size() != sim::kCellsPerChunk) {
                return false;
            }
        }

        // Indexed sets over the fixed 32x32 chunk grid: every membership probe
        // below is O(1), so a handful-sized delta never rescans the ~347 cached
        // entries. `slot` maps a cached listed key to its pool/views ordinal.
        std::bitset<sim::kChunkCount> need{};
        std::bitset<sim::kChunkCount> prev_need{};
        std::bitset<sim::kChunkCount> kept{};
        std::bitset<sim::kChunkCount> dirt{};
        std::bitset<sim::kChunkCount> grown{};
        std::bitset<sim::kChunkCount> fresh_bits{};
        std::array<i32, sim::kChunkCount> slot{};
        slot.fill(-1);

        for (const sim::ChunkPos pos: sorted_needed) {
            if (not sim::isInsideChunkGrid(pos)) {
                return false;
            }
            need.set(sim::chunkIndexOf(pos));
        }
        for (const sim::ChunkPos pos: m_cached_needed) {
            if (not sim::isInsideChunkGrid(pos)) {
                return false;
            }
            prev_need.set(sim::chunkIndexOf(pos));
        }
        for (std::size_t index = 0u; index < m_cached_listed.size(); ++index) {
            const sim::ChunkPos pos = m_cached_listed[index];
            if (not sim::isInsideChunkGrid(pos)) {
                return false;
            }
            const u32 key = sim::chunkIndexOf(pos);
            if (slot[key] >= 0) {
                return false;
            }
            slot[key] = static_cast<i32>(index);
            kept.set(key);
        }
        for (const sim::ChunkPos pos: dirty) {
            if (not sim::isInsideChunkGrid(pos)) {
                return false;
            }
            dirt.set(sim::chunkIndexOf(pos));
        }

        // Coverage merge constrained to the seed: every stored listed set is
        // ring-closed, so kept chunks already carry their ring and only
        // dirty/added/covered-new chunks can grow the closure. The result is
        // the exact set the full path computes (seed ∪ one 3x3 ring per seed
        // entry), and key-ascending iteration matches its sort order, so no
        // O(n^2) include scan and no sort are needed here.
        const auto grow = [&grown](const sim::ChunkPos pos) {
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
                    grown.set(sim::chunkIndexOf(sim::ChunkPos{static_cast<u32>(nx), static_cast<u32>(ny)}));
                }
            }
        };
        for (const sim::ChunkPos pos: dirty) {
            grow(pos);
        }
        for (const sim::ChunkPos pos: sorted_needed) {
            grow(pos);
        }
        for (const sim::ChunkPos pos: m_collider_covered) {
            if (not sim::isInsideChunkGrid(pos)) {
                return false;
            }
            grow(pos);
        }

        // Sorted-merge diff: both the cached and the new listed sets are in
        // key order, so one ascending sweep yields the recopy set
        // (added ∪ dirty ∪ coverage-growth) and the grown size up front.
        Array<u32> fresh_keys{};
        u32 grown_count = 0u;
        for (u32 key = 0u; key < sim::kChunkCount; ++key) {
            if (not grown.test(key)) {
                continue;
            }
            ++grown_count;
            if (dirt.test(key) or (need.test(key) and not prev_need.test(key)) or not kept.test(key)) {
                fresh_keys.push_back(key);
                fresh_bits.set(key);
            }
        }

        // Fresh copies first (verbatim mirror of the full-path cell copy): the
        // cache is untouched until assembly, so an allocation failure here
        // propagates exactly as it would on the full path.
        Array<Array<u16>> fresh{};
        fresh.reserve(fresh_keys.size());
        for (const u32 key: fresh_keys) {
            const sim::ChunkPos pos{key % sim::kChunksPerEdge, key / sim::kChunksPerEdge};
            Array<u16> elements{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyPreparation, copyColliderChunk(pos, elements));
            fresh.push_back(std::move(elements));
        }

        // Assembly moves kept buffers verbatim into key order and splices the
        // fresh copies at their keys. Outputs are reserved up front, so no
        // reallocation can occur mid-assembly. Dropped (removed-and-uncovered)
        // buffers stay in `m_cached_pool` until the shared tail overwrites it.
        //
        // Determinism invariant: reusing a kept buffer yields bit-identical
        // views to recopying whenever the chunk is not in dirty/added/growth.
        // Proof by set: a kept buffer is a verbatim cell copy from an earlier
        // prepare, and every grid-cell write since then fanned out into the
        // collider-dirty set, which clears only after a successful prepare
        // that recopies its whole dirty set — so a chunk absent from the
        // current dirty set has no un-recopied write since its copy. Added and
        // growth chunks hold no reusable buffer and are copied fresh above; a
        // `m_needed` flip on a reused buffer is flag-only over identical
        // content, matching what a recopy would produce.
        listed_out.reserve(grown_count);
        pool_out.reserve(grown_count);
        views_out.reserve(grown_count);
        std::size_t cursor = 0u;
        for (u32 key = 0u; key < sim::kChunkCount; ++key) {
            if (not grown.test(key)) {
                continue;
            }
            const sim::ChunkPos pos{key % sim::kChunksPerEdge, key / sim::kChunksPerEdge};
            if (fresh_bits.test(key)) {
                pool_out.push_back(std::move(fresh[cursor]));
                ++cursor;
            } else {
                pool_out.push_back(std::move(m_cached_pool[static_cast<std::size_t>(slot[key])]));
            }
            listed_out.push_back(pos);
            const Array<u16> &stored = pool_out[pool_out.size() - 1u];
            views_out.push_back(
                physics::ColliderChunkView{
                    .m_pos = physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)},
                    .m_elements = std::span<const u16>{stored.data(), stored.size()},
                    .m_needed = need.test(key)});
        }
        return true;
    }

    std::error_code ColonyDriver::rebuildChunks(
        std::span<const sim::ChunkPos> dirty, std::span<const sim::ChunkPos> needed, const u32 max,
        const IContext *const context) {
        const hal::ProfileScope scope_prepare{"Colony.Colliders.Prepare"};

        // Determinism invariant: every grid-cell write path fans out dirt
        // (advanceErrands `touched`, dig intents) and bumps `m_cover_gen`,
        // or goes through a restore re-init that invalidates the cache. The
        // neighbourhood walk below performs no cell copies, so an intact key
        // (valid flag, unchanged generation, element-wise equal sorted
        // `needed`, empty new dirt) implies a fresh preparation would produce
        // bit-identical views. The cache holds DERIVED state only: it is
        // never snapshotted and never compared for determinism.
        Array<sim::ChunkPos> sorted_needed{};
        for (const sim::ChunkPos pos: needed) {
            sorted_needed.push_back(pos);
        }
        std::ranges::sort(
            sorted_needed, {}, [](const sim::ChunkPos pos) { return pos.m_y * sim::kChunksPerEdge + pos.m_x; });

        // Interest transitions are geometry work even without terrain edits:
        // gained owners need chains and lost owners must release theirs.
        Array<sim::ChunkPos> work_dirty{};
        for (const auto position: dirty) {
            if (not chunkListedU32(work_dirty, position)) {
                work_dirty.push_back(position);
            }
        }
        if (sorted_needed != m_cached_needed) {
            for (const auto position: sorted_needed) {
                if (not chunkListedU32(m_cached_needed, position) and not chunkListedU32(work_dirty, position)) {
                    work_dirty.push_back(position);
                }
            }
            for (const auto position: m_cached_needed) {
                if (not chunkListedU32(sorted_needed, position) and not chunkListedU32(work_dirty, position)) {
                    work_dirty.push_back(position);
                }
            }
        }

        dirty = {work_dirty.data(), work_dirty.size()};
        const bool hit = m_cache_valid and
                         m_cover_gen == m_cached_gen and
                         dirty.empty() and
                         std::ranges::equal(m_cached_needed, sorted_needed);
        if (hit) {
            // Cache HIT: skip the coverage merge, sort, and per-chunk cell
            // copies entirely. The cached views already span every chunk the
            // drain can touch: empty dirt enqueues nothing, and the pending
            // remainder only shrinks, so each pending entry was validated
            // against these exact views when it was enqueued.
            if (dirty.empty() and m_colliders.pendingCount() == 0u) {
                // Proven no-op: empty dirt enqueues nothing, an empty pending
                // queue drains nothing, and identical views build nothing, so
                // the rebuild call would touch neither the scene nor the
                // pending set. Skipping it preserves state bit-identically.
                return {};
            }
            Array<physics::ColliderChunkPos> dirty_i32{};
            u32 processed = 0u;
            std::error_code rebuilt{};
            {
                const hal::ProfileScope scope_rebuild{"Colony.Colliders.Rebuild"};
                rebuilt = m_colliders.rebuildDirtyChunks(
                    m_scene, std::span<const physics::ColliderChunkView>{m_cached_views.data(), m_cached_views.size()},
                    std::span<const physics::ColliderChunkPos>{dirty_i32.data(), dirty_i32.size()}, sim::kChunkEdge,
                    max, processed);
            }
            if (m_colliders.pendingCount() == 0u and not m_collider_covered.empty()) {
                // H3a Slice 6: REMOVED gen bump — clearing removes views, and cached
                // views stay a SUPERSET of what drain needs (pending==0 needs nothing).
                m_collider_covered = Array<sim::ChunkPos>{};
            }
            // Otherwise the covered set is retained as-is: the cached views
            // already span the whole pending remainder, so regrowing the ring
            // here would only add fringe, never drain work.
            return rebuilt;
        }

        Array<sim::ChunkPos> listed{};
        Array<Array<u16>> pool{};
        Array<physics::ColliderChunkView> views{};
        // H4a Slice 6: a delta prepare still counts one Miss; the recopy set
        // is (added ∪ dirty ∪ coverage-growth) while kept buffers move over
        // verbatim. The full preparation inside the branch is the unchanged
        // fallback (same set, same order, same copies, same flags).
        bool delta_ready = false;
        if (not context) {
            const auto delta = buildColliderViewsDelta(sorted_needed, dirty, listed, pool, views);
            PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, delta);
            delta_ready = *delta;
        }

        if (not delta_ready) {
            const auto include = [&listed](const sim::ChunkPos pos) {
                if (not chunkListedU32(listed, pos)) {
                    listed.push_back(pos);
                }
            };
            for (const sim::ChunkPos pos: dirty) {
                include(pos);
            }
            for (const sim::ChunkPos pos: sorted_needed) {
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
            std::ranges::sort(
                listed, {}, [](const sim::ChunkPos pos) { return pos.m_y * sim::kChunksPerEdge + pos.m_x; });
            for (const sim::ChunkPos pos: listed) {
                if (context) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, context->error());
                }
                Array<u16> elements{};
                PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, copyColliderChunk(pos, elements));
                const bool wanted = chunkListedU32(sorted_needed, pos);
                pool.push_back(std::move(elements));
                const Array<u16> &stored = pool[pool.size() - 1u];
                views.push_back(
                    physics::ColliderChunkView{
                        .m_pos = physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)},
                        .m_elements = std::span<const u16>{stored.data(), stored.size()},
                        .m_needed = wanted});
            }
        }

        Array<physics::ColliderChunkPos> dirty_i32{};
        for (const sim::ChunkPos pos: dirty) {
            dirty_i32.push_back(physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)});
        }
        u32 processed = 0u;
        std::error_code rebuilt{};

        {
            const hal::ProfileScope scope_rebuild{"Colony.Colliders.Rebuild"};
            if (context) {
                // Candidate preparation drains one collider chunk at a time so
                // cancellation cannot be hidden behind the entire spawn batch.
                for (u32 index = 0u; index < max; ++index) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, context->error());
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyPreparation,
                        m_colliders.rebuildDirtyChunks(
                            m_scene, std::span<const physics::ColliderChunkView>{views.data(), views.size()},
                            index == 0u ? std::span<const physics::ColliderChunkPos>{dirty_i32.data(), dirty_i32.size()}
                                        : std::span<const physics::ColliderChunkPos>{},
                            sim::kChunkEdge, 1u, processed));
                    if (m_colliders.pendingCount() == 0u) {
                        break;
                    }
                }
            } else {
                rebuilt = m_colliders.rebuildDirtyChunks(
                    m_scene, std::span<const physics::ColliderChunkView>{views.data(), views.size()},
                    std::span<const physics::ColliderChunkPos>{dirty_i32.data(), dirty_i32.size()}, sim::kChunkEdge,
                    max, processed);
            }
        }
        // Store the prepared views for steady-state reuse; the stored
        // generation is the pre-drain value, so covered growth below forces
        // the next tick to re-prepare, while a gen-preserving clear keeps the
        // key intact for a HIT (SUPERSET invariant: pending==0 needs nothing).
        m_cached_needed = std::move(sorted_needed);
        m_cached_listed = listed;
        m_cached_pool = std::move(pool);
        m_cached_views = std::move(views);
        for (std::size_t index = 0u; index < m_cached_views.size(); ++index) {
            m_cached_views[index].m_elements = std::span<const u16>{
                m_cached_pool[index].data(), m_cached_pool[index].size()};
        }
        m_cached_gen = m_cover_gen;
        m_cache_valid = true;

        if (m_colliders.pendingCount() == 0u) {
            if (not m_collider_covered.empty()) {
                // H3a Slice 6: REMOVED gen bump — same SUPERSET invariant as the hit
                // path above; keep the clear itself for memory hygiene.
                m_collider_covered = Array<sim::ChunkPos>{};
            }
        } else {
            // H3a Slice 6: KEPT bump — covered growth to `listed` is real content
            // change; the next tick must re-prepare over the expanded ring.
            if (m_collider_covered != listed) {
                ++m_cover_gen;
            }
            m_collider_covered = std::move(listed);
        }
        return rebuilt;
    }

    // ------------------------------------------------------------------
    // driver diagnostics
    // ------------------------------------------------------------------

    std::span<const AgentSummary> ColonyDriver::agentSummaries() const noexcept {
        return std::span<const AgentSummary>{m_agents.data(), m_agents.size()};
    }

    std::error_code ColonyDriver::restoreBodies() {
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation,
            not m_edit ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        clearPlayerCommand();

        if (not m_physics_ready) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }

        // Scene::clear preserves terrain chains. Recreate the disposable
        // physics world before releasing its collider handle owner.
        shutdownPhysics();
        PPR_RETURN_ERROR_ON_FAIL(ColonyPreparation, initPhysics());
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
        PPR_RETURN_ERROR_ON_FAIL(
            ColonyPreparation,
            not m_edit ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
        clearToolState();

        Array<sim::ChunkPos> dirty{};
        for (const sim::ChunkDelta &delta: snapshot.m_deltas) {
            const sim::ChunkPos pos{delta.m_chunk % sim::kChunksPerEdge, delta.m_chunk / sim::kChunksPerEdge};
            if (not chunkListedU32(dirty, pos)) {
                dirty.push_back(pos);
            }
        }

        Array<sim::ChunkPos> needed{};
        agentNeighbourhood(m_registry, needed, cellsPerMetre());

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
        return m_colliders.chainCount(physics::ColliderChunkPos{static_cast<i32>(pos.m_x), static_cast<i32>(pos.m_y)});
    }

    std::size_t ColonyDriver::colliderPending() const noexcept {
        return m_colliders.pendingCount();
    }
}
