module;
#include "pP/Macros.h"
#include "Colony.Elements.h"
#include <slang.h>
#include <slang-com-ptr.h>

module game.colony.smoke;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.sim;
import game.colony.agents;
import game.colony.buildings;
import game.colony.digtool;
import game.colony.driver;
import game.colony.pathfinding;
import game.colony.translator;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonySmoke, debug, none)

    namespace {
        constexpr u64 kSeed = 1234567u;
        constexpr u32 kExtent = 256u;
        constexpr u32 kCenter = kExtent / 2u;

        [[nodiscard]] Expected<sim::GlobalCellPos> findCoreCell(const sim::ChunkGrid &grid) {
            // Choose an actual generated temperate cell rather than assuming
            // the seed leaves any one coordinate uncarved by a cavern.
            for (u32 y = sim::kWorldEdge / 4u; y < sim::kWorldEdge * 3u / 4u; y += 16u) {
                for (u32 x = sim::kWorldEdge / 4u; x < sim::kWorldEdge * 3u / 4u; x += 16u) {
                    const auto cell = grid.getCell({x, y});
                    if (not cell) [[unlikely]] {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element == kElementRock) {
                        return sim::GlobalCellPos{x, y};
                    }
                }
            }
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }

        [[nodiscard]] std::error_code readCenterPixel(rhi::IDevice &device, rhi::ITexture &target,
                                                      std::array<u8, 4u> &pixel) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err = make_error_code(device.readTexture(&target, 0u, 0u,
                blob.writeRef(), &layout))) [[unlikely]] {
                return err;
            }
            const std::size_t offset = static_cast<std::size_t>(kCenter) *
                                       static_cast<std::size_t>(layout.rowPitch) +
                                       static_cast<std::size_t>(kCenter) * pixel.size();
            if (blob.get() == nullptr or
                blob->getBufferPointer() == nullptr or
                layout.rowPitch < static_cast<u64>(kExtent) * pixel.size() or
                offset > blob->getBufferSize() or
                blob->getBufferSize() - offset < pixel.size()) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
            const auto *const bytes = static_cast<const std::byte *>(blob->getBufferPointer()) + offset;
            for (std::size_t channel = 0u; channel < pixel.size(); ++channel) {
                pixel[channel] = static_cast<u8>(bytes[channel]);
            }
            return {};
        }

        /// Deterministic Slice 2 self-checks: budget slicing, completion or
        /// honest partial, and save→load→tick stability of path state.
        [[nodiscard]] Expected<PathCounts> checkPathfinding() {
            ColonyDriver driver{};
            if (const std::error_code error = driver.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            driver.setPaused(true);
            const sim::ChunkGrid &grid = driver.grid();
            sim::Registry &registry = driver.registry();

            // Deterministic endpoints: first vacuum near the world center,
            // then the first vacuum 300–400 cells away (row-major scans, so
            // the pair is a pure function of the seed). The distance exceeds
            // one tick of expansions, which is what makes the budget check a
            // proof rather than an observation.
            const auto vacuumAt = [&grid](const u32 x, const u32 y) noexcept {
                return x < sim::kWorldEdge and y < sim::kWorldEdge and isWalkable(grid, sim::GlobalCellPos{x, y});
            };
            sim::GlobalCellPos from{sim::kWorldEdge, sim::kWorldEdge};
            for (u32 y = sim::kWorldEdge / 2u; y < sim::kWorldEdge; ++y) {
                for (u32 x = sim::kWorldEdge / 2u; x < sim::kWorldEdge; ++x) {
                    if (vacuumAt(x, y)) {
                        from = {x, y};
                        break;
                    }
                }
                if (from.m_x < sim::kWorldEdge) {
                    break;
                }
            }
            if (from.m_x >= sim::kWorldEdge) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            sim::GlobalCellPos to = from;
            bool distant = false;
            for (u32 y = 0u; y < sim::kWorldEdge and not distant; ++y) {
                for (u32 x = 0u; x < sim::kWorldEdge; ++x) {
                    const u32 dx = x >= from.m_x ? x - from.m_x : from.m_x - x;
                    const u32 dy = y >= from.m_y ? y - from.m_y : from.m_y - y;
                    if (dx + dy >= 300u and dx + dy <= 400u and vacuumAt(x, y)) {
                        to = {x, y};
                        distant = true;
                        break;
                    }
                }
            }
            if (not distant) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }

            const auto requested = driver.requestPath(from, to, kPathCapsWalk);
            if (not requested) {
                return std::unexpected{requested.error()};
            }
            const sim::Entity entity = *requested;

            // C1: one tick cannot clean-complete a 300+ cell trip.
            if (const std::error_code error = driver.stepOne()) {
                return std::unexpected{error};
            }
            const PathComp *early = registry.get<PathComp>(entity);
            if (early != nullptr and not early->m_partial) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }

            // C2: run to a result and validate the chain or the prefix.
            const PathComp *result = early;
            for (u32 tick = 0u; tick < 200u and result == nullptr; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                result = registry.get<PathComp>(entity);
            }
            if (result == nullptr) {
                return std::unexpected{std::make_error_code(std::errc::timed_out)};
            }
            if (result->m_count > kMaxPathPts) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            if (not result->m_partial) {
                if (result->m_count == 0u or not(result->m_pts[result->m_count - 1u] == to)) {
                    return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                }
                sim::GlobalCellPos previous = from;
                for (u32 point = 0u; point < result->m_count; ++point) {
                    const sim::GlobalCellPos cell = result->m_pts[point];
                    const u32 dx = cell.m_x >= previous.m_x ? cell.m_x - previous.m_x : previous.m_x - cell.m_x;
                    const u32 dy = cell.m_y >= previous.m_y ? cell.m_y - previous.m_y : previous.m_y - cell.m_y;
                    if (dx + dy != 1u or not isWalkable(grid, cell)) {
                        return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                    }
                    previous = cell;
                }
            }
            const PathReq *req = registry.get<PathReq>(entity);
            if (req == nullptr) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            // Copy out before any further registry mutation: column appends
            // may reallocate and invalidate outstanding component pointers.
            const PathReq want_req = *req;
            const PathComp want_comp = *result;

            // C2b: a nearby trip must complete cleanly, exercising the
            // goal-reached branch and full chain reconstruction.
            sim::GlobalCellPos near = from;
            bool close = false;
            for (u32 y = 0u; y < sim::kWorldEdge and not close; ++y) {
                for (u32 x = 0u; x < sim::kWorldEdge; ++x) {
                    const u32 dx = x >= from.m_x ? x - from.m_x : from.m_x - x;
                    const u32 dy = y >= from.m_y ? y - from.m_y : from.m_y - y;
                    if (dx + dy >= 5u and dx + dy <= 60u and vacuumAt(x, y)) {
                        near = {x, y};
                        close = true;
                        break;
                    }
                }
            }
            if (not close) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            const auto requested2 = driver.requestPath(from, near, kPathCapsWalk);
            if (not requested2) {
                return std::unexpected{requested2.error()};
            }
            const sim::Entity entity2 = *requested2;
            const PathComp *done = nullptr;
            for (u32 tick = 0u; tick < 100u and done == nullptr; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                done = registry.get<PathComp>(entity2);
            }
            if (done == nullptr or
                done->m_partial or
                done->m_count == 0u or
                not(done->m_pts[done->m_count - 1u] == near)) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            sim::GlobalCellPos previous = from;
            for (u32 point = 0u; point < done->m_count; ++point) {
                const sim::GlobalCellPos cell = done->m_pts[point];
                const u32 dx = cell.m_x >= previous.m_x ? cell.m_x - previous.m_x : previous.m_x - cell.m_x;
                const u32 dy = cell.m_y >= previous.m_y ? cell.m_y - previous.m_y : previous.m_y - cell.m_y;
                if (dx + dy != 1u or not isWalkable(grid, cell)) {
                    return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                }
                previous = cell;
            }

            // C3: path state survives save→load→tick. Capture ECS against a
            // clean grid so the snapshot carries columns only, not the world.
            const sim::ChunkGrid clean{};
            const sim::Snapshot snap = sim::capture(clean, registry, kSeed);
            const auto bytes = sim::save(snap);
            if (not bytes) {
                return std::unexpected{bytes.error()};
            }
            const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
            if (not loaded) {
                return std::unexpected{loaded.error()};
            }
            sim::Registry restored{};
            if (const std::error_code error = registerColonyComponents(restored)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::restore(restored, *loaded)) {
                return std::unexpected{error};
            }
            const PathReq *req2 = restored.get<PathReq>(entity);
            const PathComp *comp2 = restored.get<PathComp>(entity);
            if (req2 == nullptr or comp2 == nullptr or not(*req2 == want_req) or not(*comp2 == want_comp)) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            Pathfinder again{};
            stepPathfinding(grid, restored, again, kExpansionsPerTick * 4u);
            const PathComp *comp3 = restored.get<PathComp>(entity);
            if (comp3 == nullptr or not(*comp3 == want_comp)) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }

            const PathCounts counts = driver.pathCounts();
            if (const std::error_code error = driver.shutdown()) {
                return std::unexpected{error};
            }
            return counts;
        }

        /// Deterministic Slice 3 self-checks: progressive wall placement,
        /// grid solidity, translator sync, search on the edited grid,
        /// grid-delta snapshot round-trip, and demolition back to vacuum.
        [[nodiscard]] Expected<u32> checkBuildings() {
            ColonyDriver driver{};
            if (const std::error_code error = driver.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            driver.setPaused(true);
            ColonyTranslator translator{};
            sim::Registry &registry = driver.registry();

            // First 8x8 all-vacuum block in the central region (row-major, so
            // the rect is a pure function of the seed).
            sim::GlobalCellPos origin{sim::kWorldEdge, sim::kWorldEdge};
            for (u32 y = sim::kWorldEdge / 4u;
                 y < sim::kWorldEdge * 3u / 4u and origin.m_x >= sim::kWorldEdge;
                 ++y) {
                for (u32 x = sim::kWorldEdge / 4u; x < sim::kWorldEdge * 3u / 4u; ++x) {
                    bool open = true;
                    for (u32 dy = 0u; dy < 8u and open; ++dy) {
                        for (u32 dx = 0u; dx < 8u; ++dx) {
                            if (not isWalkable(driver.grid(), {x + dx, y + dy})) {
                                open = false;
                                break;
                            }
                        }
                    }
                    if (open) {
                        origin = {x, y};
                        break;
                    }
                }
            }
            if (origin.m_x >= sim::kWorldEdge) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }

            // B1: progressive placement completes and turns cells solid.
            const auto wall = driver.buildWall(
                Footprint{.m_min = origin, .m_width = 8u, .m_height = 8u, .m_element = kElementRock});
            if (not wall) {
                return std::unexpected{wall.error()};
            }
            if (driver.errandCount() != 1u) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            const BuildErrand *raised = nullptr;
            for (u32 tick = 0u; tick < 10u and raised == nullptr; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                const BuildErrand *errand = registry.get<BuildErrand>(*wall);
                if (errand != nullptr and errand->m_progress >= errand->m_total) {
                    raised = errand;
                }
            }
            if (raised == nullptr or driver.errandCount() != 0u) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            u32 solid = 0u;
            for (u32 dy = 0u; dy < 8u; ++dy) {
                for (u32 dx = 0u; dx < 8u; ++dx) {
                    const auto cell = driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element == kElementRock) {
                        ++solid;
                    }
                }
            }
            if (solid != 64u) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }

            // B2: search stays sound on the edited grid (complete with a
            // valid chain, or an honest partial when the wall disconnects).
            sim::GlobalCellPos side_a = origin;
            sim::GlobalCellPos side_b = origin;
            bool flanks = false;
            for (u32 y = origin.m_y; y < origin.m_y + 8u and not flanks; ++y) {
                for (u32 x = 0u; x < sim::kWorldEdge and not flanks; ++x) {
                    if (x + 20u < sim::kWorldEdge and
                        isWalkable(driver.grid(), {x, y}) and
                        isWalkable(driver.grid(), {x + 20u, y})) {
                        side_a = {x, y};
                        side_b = {x + 20u, y};
                        flanks = true;
                    }
                }
            }
            if (not flanks) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            const auto detour = driver.requestPath(side_a, side_b, kPathCapsWalk);
            if (not detour) {
                return std::unexpected{detour.error()};
            }
            const PathComp *detoured = nullptr;
            for (u32 tick = 0u; tick < 100u and detoured == nullptr; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                detoured = registry.get<PathComp>(*detour);
            }
            if (detoured == nullptr or detoured->m_count > kMaxPathPts) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            if (not detoured->m_partial) {
                if (detoured->m_count == 0u or not(detoured->m_pts[detoured->m_count - 1u] == side_b)) {
                    return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                }
                sim::GlobalCellPos previous = side_a;
                for (u32 point = 0u; point < detoured->m_count; ++point) {
                    const sim::GlobalCellPos cell = detoured->m_pts[point];
                    const u32 dx = cell.m_x >= previous.m_x ? cell.m_x - previous.m_x : previous.m_x - cell.m_x;
                    const u32 dy = cell.m_y >= previous.m_y ? cell.m_y - previous.m_y : previous.m_y - cell.m_y;
                    if (dx + dy != 1u or not isWalkable(driver.grid(), cell)) {
                        return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                    }
                    previous = cell;
                }
            }

            // B3: grid deltas cover exactly the demolished chunks and
            // round-trip through save/load/apply.
            driver.grid().clearAllDirty();
            const auto teardown = driver.demolish(origin, 8u, 8u);
            if (not teardown) {
                return std::unexpected{teardown.error()};
            }
            bool leveled = false;
            for (u32 tick = 0u; tick < 10u and not leveled; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                const BuildErrand *errand = registry.get<BuildErrand>(*teardown);
                leveled = errand != nullptr and errand->m_progress >= errand->m_total;
            }
            if (not leveled or driver.errandCount() != 0u) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            for (u32 dy = 0u; dy < 8u; ++dy) {
                for (u32 dx = 0u; dx < 8u; ++dx) {
                    const auto cell = driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element != kElementVacuum) {
                        return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                    }
                }
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            const sim::Snapshot snap = sim::capture(driver.grid(), registry, kSeed);
            if (snap.m_deltas.empty()) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            const auto bytes = sim::save(snap);
            if (not bytes) {
                return std::unexpected{bytes.error()};
            }
            const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
            if (not loaded) {
                return std::unexpected{loaded.error()};
            }
            sim::ChunkGrid applied{};
            if (const std::error_code error = sim::apply(applied, *loaded)) {
                return std::unexpected{error};
            }
            for (u32 dy = 0u; dy < 8u; ++dy) {
                for (u32 dx = 0u; dx < 8u; ++dx) {
                    const auto cell = applied.getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element != kElementVacuum) {
                        return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                    }
                }
            }
            const u32 delta_chunks = static_cast<u32>(snap.m_deltas.size());
            if (const std::error_code error = driver.shutdown()) {
                return std::unexpected{error};
            }
            return delta_chunks;
        }

        /// Deterministic Slice 5 self-checks: dig-primitive validation,
        /// paused-only application, collider-budget drain, wake radius,
        /// translator refresh, snapshot round-trip, and double-run equality.
        [[nodiscard]] Expected<u32> checkDigCells() {
            constexpr u32 kColliderBudget = 2u;
            constexpr u64 kWakeRadius2 = static_cast<u64>(kDigWakeCells) * static_cast<u64>(kDigWakeCells);
            constexpr TimeSpan kTick = TimeSpan{std::chrono::milliseconds{17}};

            const auto fail = [](const char *const sub, const char *const what) -> Expected<u32> {
                std::println(stderr, "SMOKE-DIG-FAIL {} {}", sub, what);
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            };
            const auto sameBody = [](const sim::BodyState &a, const sim::BodyState &b) noexcept {
                return a.m_x == b.m_x and
                       a.m_y == b.m_y and
                       a.m_angle == b.m_angle and
                       a.m_velocity_x == b.m_velocity_x and
                       a.m_velocity_y == b.m_velocity_y and
                       a.m_angular_velocity == b.m_angular_velocity;
            };
            const auto posesOf = [](sim::Registry &registry) {
                Array<sim::BodyState> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                    if (pose != nullptr) {
                        out.push_back(*pose);
                    }
                }
                return out;
            };
            const auto needsOf = [](sim::Registry &registry) {
                Array<Needs> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const Needs *needs = registry.get<Needs>(entity);
                    if (needs != nullptr) {
                        out.push_back(*needs);
                    }
                }
                return out;
            };
            const auto membersOf = [](sim::Registry &registry) {
                Array<sim::Entity> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    out.push_back(entity);
                }
                return out;
            };
            const auto plansOf = [](sim::Registry &registry) {
                Array<Plan> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const Plan *plan = registry.get<Plan>(entity);
                    if (plan != nullptr) {
                        out.push_back(*plan);
                    }
                }
                return out;
            };
            const auto reqsOf = [](sim::Registry &registry) {
                Array<PathReq> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const PathReq *req = registry.get<PathReq>(entity);
                    if (req != nullptr) {
                        out.push_back(*req);
                    }
                }
                return out;
            };
            const auto compsOf = [](sim::Registry &registry) {
                Array<PathComp> out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const PathComp *comp = registry.get<PathComp>(entity);
                    if (comp != nullptr) {
                        out.push_back(*comp);
                    }
                }
                return out;
            };
            const auto awakeOf = [](ColonyDriver &driver) {
                Array<std::pair<sim::Entity, bool> > out{};
                sim::Snapshot snap = driver.snapshotWithBodies();
                for (const sim::BodyRecord &record: snap.m_bodies.m_records) {
                    out.push_back({record.m_entity, record.m_awake});
                }
                return out;
            };
            const auto chunkDist2 = [](const u32 x, const u32 y, const sim::ChunkPos chunk) noexcept {
                const u32 min_x = chunk.m_x * sim::kChunkEdge;
                const u32 min_y = chunk.m_y * sim::kChunkEdge;
                const u32 max_x = min_x + sim::kChunkEdge - 1u;
                const u32 max_y = min_y + sim::kChunkEdge - 1u;
                const u64 dx = x < min_x
                                   ? static_cast<u64>(min_x - x)
                                   : x > max_x
                                         ? static_cast<u64>(x - max_x)
                                         : 0u;
                const u64 dy = y < min_y
                                   ? static_cast<u64>(min_y - y)
                                   : y > max_y
                                         ? static_cast<u64>(y - max_y)
                                         : 0u;
                return dx * dx + dy * dy;
            };
            const auto gridsEqual = [](const sim::ChunkGrid &a, const sim::ChunkGrid &b) {
                for (u32 cy = 0u; cy < sim::kChunksPerEdge; ++cy) {
                    for (u32 cx = 0u; cx < sim::kChunksPerEdge; ++cx) {
                        const sim::ChunkPos pos{cx, cy};
                        const auto ra = a.residentCells(pos);
                        const auto rb = b.residentCells(pos);
                        if (ra.has_value() != rb.has_value()) {
                            return false;
                        }
                        if (ra and not std::ranges::equal(*ra, *rb)) {
                            return false;
                        }
                    }
                }
                return true;
            };

            ColonyDriver driver{};
            if (const std::error_code error = driver.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            driver.setPaused(true);
            ColonyTranslator translator{};
            sim::Registry &registry = driver.registry();

            // D1: primitive validation on an isolated ChunkGrid via digCells
            // directly (DigTool.cppm:11-15,19-43; DigTool.cpp:24-43,53-88).
            // Seeds are explicit rock cells with analytic temps and dirt is
            // cleared before each dig, so counts, touched order, and preserved
            // temps prove exactly the write set. The area budget is the full
            // 32x32 rect, so no within-edge rect can exceed it: there is no
            // separate oversized-area input.
            std::println(stderr, "SMOKE-DIG-STAGE D1");
            sim::ChunkGrid probe{};
            const auto seedRock = [&](const u32 x0, const u32 y0, const u32 width, const u32 height,
                                      const float base) -> std::error_code {
                for (u32 row = 0u; row < height; ++row) {
                    for (u32 col = 0u; col < width; ++col) {
                        const float temp = base + static_cast<float>(row * width + col);
                        if (const std::error_code error = probe.setCell({x0 + col, y0 + row},
                            sim::Cell{kElementRock, temp})) {
                            return error;
                        }
                    }
                }
                return {};
            };
            const auto rockTemp = [](const float base, const u32 index) noexcept {
                return base + static_cast<float>(index);
            };

            if (const std::error_code error = seedRock(64u, 64u, 8u, 8u, 300.0f)) {
                return std::unexpected{error};
            }
            probe.clearAllDirty();
            if (probe.dirtyChunkCount() != 0u) {
                return fail("D1", "seed dirt");
            }
            const u32 clean_dirt = probe.dirtyChunkCount();
            const Expected<DigResult> zero_dig = digCells(probe,
                DigRect{.m_min = {64u, 64u}, .m_width = 0u, .m_height = 8u});
            const bool zero_rejected = not zero_dig;
            const bool zero_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not zero_rejected or not zero_clean) {
                return fail("D1", "zero edge");
            }
            const Expected<DigResult> wide_dig = digCells(probe,
                DigRect{.m_min = {64u, 64u}, .m_width = kMaxDigEdge + 1u, .m_height = 8u});
            const bool wide_rejected = not wide_dig;
            const bool wide_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not wide_rejected or not wide_clean) {
                return fail("D1", "oversized edge");
            }
            const Expected<DigResult> oob_dig = digCells(probe,
                DigRect{.m_min = {sim::kWorldEdge - 4u, sim::kWorldEdge - 4u}, .m_width = 8u, .m_height = 8u});
            const bool oob_rejected = not oob_dig;
            const bool oob_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not oob_rejected or not oob_clean) {
                return fail("D1", "boundary overflow");
            }
            const auto witness = probe.getCell({64u, 64u});
            if (not witness) {
                return std::unexpected{witness.error()};
            }
            const bool witness_intact = witness->m_element == kElementRock and
                                        witness->m_temperature == 300.0f;
            if (not witness_intact) {
                return fail("D1", "rejected dirtied grid");
            }

            if (const std::error_code error = seedRock(200u, 200u, 1u, 1u, 310.0f)) {
                return std::unexpected{error};
            }
            probe.clearAllDirty();
            const Expected<DigResult> single_dig = digCells(probe,
                DigRect{.m_min = {200u, 200u}, .m_width = 1u, .m_height = 1u});
            if (not single_dig) {
                return std::unexpected{single_dig.error()};
            }
            const bool single_count = single_dig->m_dug == 1u and
                                      single_dig->m_touched.size() == 1u and
                                      single_dig->m_touched[0u] == sim::chunkOf(sim::GlobalCellPos{200u, 200u});
            if (not single_count) {
                return fail("D1", "single count");
            }
            const auto single_cell = probe.getCell({200u, 200u});
            if (not single_cell) {
                return std::unexpected{single_cell.error()};
            }
            const bool single_kept = single_cell->m_element == kElementVacuum and
                                     single_cell->m_temperature == 310.0f;
            if (not single_kept) {
                return fail("D1", "single temp");
            }

            if (const std::error_code error = seedRock(256u, 256u, 32u, 32u, 200.0f)) {
                return std::unexpected{error};
            }
            probe.clearAllDirty();
            const Expected<DigResult> full_dig = digCells(probe,
                DigRect{.m_min = {256u, 256u}, .m_width = 32u, .m_height = 32u});
            if (not full_dig) {
                return std::unexpected{full_dig.error()};
            }
            const bool full_count = full_dig->m_dug == 1024u and
                                    full_dig->m_touched.size() == 1u and
                                    full_dig->m_touched[0u] == sim::chunkOf(sim::GlobalCellPos{256u, 256u});
            if (not full_count) {
                return fail("D1", "full count");
            }
            bool full_kept = true;
            for (u32 row = 0u; row < 32u and full_kept; ++row) {
                for (u32 col = 0u; col < 32u; ++col) {
                    const auto cell = probe.getCell({256u + col, 256u + row});
                    if (not cell or cell->m_element != kElementVacuum or
                        cell->m_temperature != rockTemp(200.0f, row * 32u + col)) {
                        full_kept = false;
                        break;
                    }
                }
            }
            if (not full_kept) {
                return fail("D1", "full temp");
            }

            if (const std::error_code error = seedRock(500u, 500u, 4u, 4u, 280.0f)) {
                return std::unexpected{error};
            }
            for (u32 row = 0u; row < 4u; ++row) {
                for (u32 col = 0u; col < 4u; ++col) {
                    const bool vacuum_seed = (row + col) % 2u == 0u;
                    if (vacuum_seed) {
                        const float temp = rockTemp(280.0f, row * 4u + col);
                        if (const std::error_code error = probe.setCell({500u + col, 500u + row},
                            sim::Cell{kElementVacuum, temp})) {
                            return std::unexpected{error};
                        }
                    }
                }
            }
            probe.clearAllDirty();
            const Expected<DigResult> mixed_dig = digCells(probe,
                DigRect{.m_min = {500u, 500u}, .m_width = 4u, .m_height = 4u});
            if (not mixed_dig) {
                return std::unexpected{mixed_dig.error()};
            }
            const bool mixed_count = mixed_dig->m_dug == 8u and
                                     mixed_dig->m_touched.size() == 1u;
            if (not mixed_count) {
                return fail("D1", "mixed count");
            }
            bool mixed_kept = true;
            for (u32 row = 0u; row < 4u and mixed_kept; ++row) {
                for (u32 col = 0u; col < 4u; ++col) {
                    const auto cell = probe.getCell({500u + col, 500u + row});
                    if (not cell or cell->m_element != kElementVacuum or
                        cell->m_temperature != rockTemp(280.0f, row * 4u + col)) {
                        mixed_kept = false;
                        break;
                    }
                }
            }
            if (not mixed_kept) {
                return fail("D1", "mixed temp");
            }

            const u32 mixed_dirt = probe.dirtyChunkCount();
            const Expected<DigResult> repeat_dig = digCells(probe,
                DigRect{.m_min = {500u, 500u}, .m_width = 4u, .m_height = 4u});
            if (not repeat_dig) {
                return std::unexpected{repeat_dig.error()};
            }
            const bool repeat_noop = repeat_dig->m_dug == 0u and
                                     repeat_dig->m_touched.empty() and
                                     probe.dirtyChunkCount() == mixed_dirt;
            if (not repeat_noop) {
                return fail("D1", "repeat no-op");
            }

            if (const std::error_code error = seedRock(124u, 64u, 8u, 8u, 260.0f)) {
                return std::unexpected{error};
            }
            probe.clearAllDirty();
            const Expected<DigResult> cross_probe = digCells(probe,
                DigRect{.m_min = {124u, 64u}, .m_width = 8u, .m_height = 8u});
            if (not cross_probe) {
                return std::unexpected{cross_probe.error()};
            }
            const sim::ChunkPos cross_a = sim::chunkOf(sim::GlobalCellPos{124u, 64u});
            const sim::ChunkPos cross_b = sim::chunkOf(sim::GlobalCellPos{131u, 71u});
            const bool cross_ordered = cross_probe->m_dug == 64u and
                                       cross_probe->m_touched.size() == 2u and
                                       cross_probe->m_touched[0u] == cross_a and
                                       cross_probe->m_touched[1u] == cross_b;
            if (not cross_ordered) {
                return fail("D1", "cross-chunk order");
            }

            // Driver intake rejects the same shapes before queueing anything:
            // a nonzero error_code is the rejection, so acceptance fails the
            // lane and any state change fails it too.
            const auto spot = driver.grid().getCell({2048u, 2048u});
            if (not spot) {
                return std::unexpected{spot.error()};
            }
            const std::error_code zero_error = driver.requestDig({0u, 0u}, 0u, 8u);
            if (not zero_error) {
                return fail("D1", "zero width accepted");
            }
            const std::error_code wide_error = driver.requestDig({0u, 0u}, kMaxDigEdge + 1u, 8u);
            if (not wide_error) {
                return fail("D1", "oversized accepted");
            }
            const std::error_code oob_error = driver.requestDig(
                {sim::kWorldEdge - 4u, sim::kWorldEdge - 4u}, 8u, 8u);
            if (not oob_error) {
                return fail("D1", "oob accepted");
            }
            if (driver.digTotal() != 0u or not driver.lastEditChunks().empty()) {
                return fail("D1", "invalid changed state");
            }
            const auto spot2 = driver.grid().getCell({2048u, 2048u});
            if (not spot2 or not(*spot2 == *spot)) {
                return fail("D1", "invalid dirtied grid");
            }

            // First single-chunk 8x8 all-rock block in the central region
            // (row-major, so the rect is a pure function of the seed; same
            // loop shape as the all-vacuum scan in checkBuildings).
            sim::GlobalCellPos origin{sim::kWorldEdge, sim::kWorldEdge};
            for (u32 y = sim::kWorldEdge / 4u;
                 y < sim::kWorldEdge * 3u / 4u and origin.m_x >= sim::kWorldEdge;
                 ++y) {
                for (u32 x = sim::kWorldEdge / 4u; x < sim::kWorldEdge * 3u / 4u; ++x) {
                    if (sim::chunkOf(sim::GlobalCellPos{x, y}) !=
                        sim::chunkOf(sim::GlobalCellPos{x + 7u, y + 7u})) {
                        continue;
                    }
                    bool solid = true;
                    for (u32 dy = 0u; dy < 8u and solid; ++dy) {
                        for (u32 dx = 0u; dx < 8u; ++dx) {
                            const auto cell = driver.grid().getCell({x + dx, y + dy});
                            if (not cell or cell->m_element != kElementRock) {
                                solid = false;
                                break;
                            }
                        }
                    }
                    if (solid) {
                        origin = {x, y};
                        break;
                    }
                }
            }
            if (origin.m_x >= sim::kWorldEdge) {
                return fail("D1", "no rock origin");
            }
            Array<float> temps{};
            for (u32 dy = 0u; dy < 8u; ++dy) {
                for (u32 dx = 0u; dx < 8u; ++dx) {
                    const auto cell = driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    temps.push_back(cell->m_temperature);
                }
            }
            if (const std::error_code error = driver.requestDig(origin, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != 64u) {
                return fail("D1", "inexact dug count");
            }
            for (u32 dy = 0u; dy < 8u; ++dy) {
                for (u32 dx = 0u; dx < 8u; ++dx) {
                    const auto cell = driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element != kElementVacuum or
                        cell->m_temperature != temps[dy * 8u + dx]) {
                        return fail("D1", "cell or temperature mismatch");
                    }
                }
            }
            if (driver.lastEditChunks().size() != 1u or
                not(driver.lastEditChunks()[0u] == sim::chunkOf(origin))) {
                return fail("D1", "touched dedupe");
            }

            // Repeat dig on the now-vacuum rect: success with zero new cells.
            if (const std::error_code error = driver.requestDig(origin, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != 64u or driver.lastEditChunks().size() != 1u) {
                return fail("D1", "repeat dig changed state");
            }

            // Cross-chunk fixture: a rock wall straddling the x=2048 chunk
            // boundary, then an 8x8 dig across it. Touched chunks are exactly
            // the pair, deduped, in row-major encounter order.
            std::println(stderr, "SMOKE-DIG-STAGE D1X");
            const auto wall1 = driver.buildWall(Footprint{
                .m_min = {2032u, 2000u},
                .m_width = 32u,
                .m_height = 16u,
                .m_element = kElementRock
            });
            if (not wall1) {
                return std::unexpected{wall1.error()};
            }
            bool wall1_done = false;
            for (u32 tick = 0u; tick < 20u and not wall1_done; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                const BuildErrand *errand = registry.get<BuildErrand>(*wall1);
                wall1_done = errand != nullptr and errand->m_progress >= errand->m_total;
            }
            if (not wall1_done) {
                return fail("D1X", "wall incomplete");
            }
            const sim::GlobalCellPos cross{2044u, 2004u};
            if (const std::error_code error = driver.requestDig(cross, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != 128u) {
                return fail("D1X", "cross-chunk count");
            }
            const sim::ChunkPos want_a{15u, 15u};
            const sim::ChunkPos want_b{16u, 15u};
            const std::span<const sim::ChunkPos> touched = driver.lastEditChunks();
            if (touched.size() != 2u or not(touched[0u] == want_a) or not(touched[1u] == want_b)) {
                return fail("D1X", "cross-chunk touched");
            }

            // D2: paused proof. The footprint is cleared to vacuum first
            // through the checked paused path, so the wall below needs every
            // write: one stepOne lands exactly 64 of 128 (writes are the
            // budgeted quantity; already-rock skips are budget-free per
            // Buildings.cpp:103-108, and none remain). The test dig then
            // applies through the paused update only (driver paused 304-312:
            // pending intent, no systems, no physics, no clock), never
            // stepOne (ticking 365-385), then presentEdits. Cells and
            // digTotal move while tick count, sim time, entity membership,
            // body states (snapshotWithBodies mirrors scene + awake flags),
            // needs, plans/path records, and incomplete errand progress stay
            // frozen behind nonempty moving/dynamic witnesses.
            std::println(stderr, "SMOKE-DIG-STAGE D2");
            const sim::GlobalCellPos build_rect{origin.m_x, origin.m_y};
            if (const std::error_code error = driver.requestDig(build_rect, 16u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            bool footprint_open = true;
            for (u32 row = 0u; row < 8u and footprint_open; ++row) {
                for (u32 col = 0u; col < 16u; ++col) {
                    const auto cell = driver.grid().getCell({build_rect.m_x + col, build_rect.m_y + row});
                    if (not cell or cell->m_element != kElementVacuum) {
                        footprint_open = false;
                        break;
                    }
                }
            }
            if (not footprint_open) {
                return fail("D2", "footprint not cleared");
            }
            const u64 dug_cleared = driver.digTotal();
            const auto wall2 = driver.buildWall(Footprint{
                .m_min = {build_rect.m_x, build_rect.m_y},
                .m_width = 16u,
                .m_height = 8u,
                .m_element = kElementRock
            });
            if (not wall2) {
                return std::unexpected{wall2.error()};
            }
            if (const std::error_code error = driver.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            const BuildErrand *rising = registry.get<BuildErrand>(*wall2);
            if (rising == nullptr or rising->m_progress != 64u or rising->m_total != 128u) {
                return fail("D2", "wall2 first-tick progress");
            }
            const u64 ticks_before = driver.tickCount();
            const double ms_before = driver.simMs();
            const Array<sim::Entity> members_before = membersOf(registry);
            const sim::Snapshot bodies_before = driver.snapshotWithBodies();
            const Array<Needs> needs_before = needsOf(registry);
            const Array<Plan> plans_before = plansOf(registry);
            const Array<PathReq> reqs_before = reqsOf(registry);
            const Array<PathComp> comps_before = compsOf(registry);
            const u32 errands_before = driver.errandCount();
            const u32 progress_before = rising->m_progress;
            u32 dynamic_bodies = 0u;
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyDefinition *def = registry.get<sim::BodyDefinition>(entity);
                if (def != nullptr and def->m_motion == sim::EMotionKind::dynamic) {
                    ++dynamic_bodies;
                }
            }
            u32 awake_or_moving = 0u;
            for (const sim::BodyRecord &record: bodies_before.m_bodies.m_records) {
                const bool moving = record.m_state.m_velocity_x != 0.0f or
                                    record.m_state.m_velocity_y != 0.0f;
                if (record.m_awake or moving) {
                    ++awake_or_moving;
                }
            }
            const bool witnesses_ready = not members_before.empty() and
                                         not needs_before.empty() and
                                         not plans_before.empty() and
                                         not reqs_before.empty() and
                                         not bodies_before.m_bodies.m_records.empty() and
                                         dynamic_bodies > 0u and
                                         awake_or_moving > 0u;
            if (not witnesses_ready) {
                return fail("D2", "empty witnesses");
            }
            const sim::GlobalCellPos paused_rect{2032u, 2004u};
            if (const std::error_code error = driver.requestDig(paused_rect, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != dug_cleared + 64u) {
                return fail("D2", "paused dig count");
            }
            const auto paused_cell = driver.grid().getCell({2032u, 2004u});
            if (not paused_cell or paused_cell->m_element != kElementVacuum) {
                return fail("D2", "paused dig cell");
            }
            const BuildErrand *frozen = registry.get<BuildErrand>(*wall2);
            const sim::Snapshot bodies_after = driver.snapshotWithBodies();
            const Array<sim::Entity> members_after = membersOf(registry);
            const Array<Needs> needs_after = needsOf(registry);
            const Array<Plan> plans_after = plansOf(registry);
            const Array<PathReq> reqs_after = reqsOf(registry);
            const Array<PathComp> comps_after = compsOf(registry);
            const bool errand_frozen = frozen != nullptr and
                                       frozen->m_progress == progress_before;
            if (not errand_frozen) {
                return fail("D2", "build progress moved");
            }
            const bool clock_frozen = driver.tickCount() == ticks_before and
                                      driver.simMs() == ms_before;
            if (not clock_frozen) {
                return fail("D2", "clock moved");
            }
            if (driver.errandCount() != errands_before) {
                return fail("D2", "errand count moved");
            }
            const bool members_frozen = std::ranges::equal(members_after, members_before);
            if (not members_frozen) {
                return fail("D2", "entity membership moved");
            }
            const bool bodies_frozen = bodies_after.m_bodies.m_replay_ticks ==
                                       bodies_before.m_bodies.m_replay_ticks and
                                       std::ranges::equal(bodies_after.m_bodies.m_records, bodies_before.m_bodies.m_records);
            if (not bodies_frozen) {
                return fail("D2", "body states moved");
            }
            const bool needs_frozen = std::ranges::equal(needs_after, needs_before);
            if (not needs_frozen) {
                return fail("D2", "needs moved");
            }
            const bool plans_frozen = std::ranges::equal(plans_after, plans_before);
            if (not plans_frozen) {
                return fail("D2", "plans moved");
            }
            const bool paths_frozen = std::ranges::equal(reqs_after, reqs_before) and
                                      std::ranges::equal(comps_after, comps_before);
            if (not paths_frozen) {
                return fail("D2", "path records moved");
            }
            bool wall2_done = false;
            for (u32 tick = 0u; tick < 30u and not wall2_done; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                const BuildErrand *errand = registry.get<BuildErrand>(*wall2);
                wall2_done = errand != nullptr and errand->m_progress >= errand->m_total;
            }
            if (not wall2_done) {
                return fail("D2", "wall2 incomplete");
            }

            // D3/D5: four isolated 2x2 rock islands around the agent-home
            // chunk corner (cleared moat); one 8x8 dig removes the 16 island
            // cells across 4 touched chunks in the agent neighbourhood.
            std::println(stderr, "SMOKE-DIG-STAGE D3");
            sim::Entity agent0{};
            sim::BodyState anchor{};
            bool anchored = false;
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose == nullptr or pose->m_x < 0.0f or pose->m_y < 0.0f) {
                    return fail("D3", "anchor pose");
                }
                agent0 = entity;
                anchor = *pose;
                anchored = true;
                if (anchored) {
                    break;
                }
            }
            if (not anchored) {
                return fail("D3", "no agents");
            }
            const u32 ax = static_cast<u32>(anchor.m_x);
            const u32 ay = static_cast<u32>(anchor.m_y);
            if (ax >= sim::kWorldEdge or ay >= sim::kWorldEdge) {
                return fail("D3", "anchor outside");
            }
            const u32 home_cx = ax / sim::kChunkEdge;
            const u32 home_cy = ay / sim::kChunkEdge;
            const u32 corner_cx = home_cx == 0u ? 1u : home_cx;
            const u32 corner_cy = home_cy == 0u ? 1u : home_cy;
            const u32 corner_x = corner_cx * sim::kChunkEdge;
            const u32 corner_y = corner_cy * sim::kChunkEdge;
            const u32 dig_x = corner_x - 4u;
            const u32 dig_y = corner_y - 4u;
            const sim::ChunkPos want_tl{corner_cx - 1u, corner_cy - 1u};
            const sim::ChunkPos want_tr{corner_cx, corner_cy - 1u};
            const sim::ChunkPos want_bl{corner_cx - 1u, corner_cy};
            const sim::ChunkPos want_br{corner_cx, corner_cy};
            if (dig_x + 8u > sim::kWorldEdge or dig_y + 8u > sim::kWorldEdge) {
                return fail("D3", "corner outside");
            }

            // Cleared moat: the whole 8x8 straddle goes to vacuum first
            // through the paused path, so the four islands below are the
            // only rock in the rect.
            if (const std::error_code error = driver.requestDig({dig_x, dig_y}, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            bool moat_open = true;
            for (u32 row = 0u; row < 8u and moat_open; ++row) {
                for (u32 col = 0u; col < 8u; ++col) {
                    const auto cell = driver.grid().getCell({dig_x + col, dig_y + row});
                    if (not cell or cell->m_element != kElementVacuum) {
                        moat_open = false;
                        break;
                    }
                }
            }
            if (not moat_open) {
                return fail("D3", "moat not cleared");
            }

            // Four isolated 2x2 rock islands, one per quadrant. Offsets leave
            // a one-cell moat to the rect edge and a two-cell gap across the
            // corner, so each island is its own collider loop.
            const sim::GlobalCellPos island_min[4u] = {
                {corner_x - 3u, corner_y - 3u}, {corner_x + 1u, corner_y - 3u},
                {corner_x - 3u, corner_y + 1u}, {corner_x + 1u, corner_y + 1u}
            };
            Array<sim::Entity> island_errands{};
            for (u32 island = 0u; island < 4u; ++island) {
                const auto made = driver.buildWall(Footprint{
                    .m_min = island_min[island], .m_width = 2u, .m_height = 2u, .m_element = kElementRock
                });
                if (not made) {
                    return std::unexpected{made.error()};
                }
                island_errands.push_back(*made);
            }
            bool islands_done = false;
            for (u32 tick = 0u; tick < 30u and not islands_done; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                islands_done = true;
                for (const sim::Entity errand_entity: island_errands) {
                    const BuildErrand *errand = registry.get<BuildErrand>(errand_entity);
                    if (errand == nullptr or errand->m_progress < errand->m_total) {
                        islands_done = false;
                        break;
                    }
                }
            }
            if (not islands_done) {
                return fail("D3", "islands incomplete");
            }
            u32 island_cells = 0u;
            for (u32 island = 0u; island < 4u; ++island) {
                for (u32 dy = 0u; dy < 2u; ++dy) {
                    for (u32 dx = 0u; dx < 2u; ++dx) {
                        const auto cell =
                                driver.grid().getCell({island_min[island].m_x + dx, island_min[island].m_y + dy});
                        if (not cell) {
                            return std::unexpected{cell.error()};
                        }
                        if (cell->m_element == kElementRock) {
                            ++island_cells;
                        }
                    }
                }
            }
            if (island_cells != 16u) {
                return fail("D3", "island cells");
            }
            u32 settle = 0u;
            while (driver.colliderPending() != 0u and settle < 200u) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                ++settle;
            }
            if (driver.colliderPending() != 0u) {
                return fail("D3", "settle backlog");
            }
            const sim::ChunkPos touched_want[4u] = {want_tl, want_tr, want_bl, want_br};
            std::size_t base_chains[4u] = {0u, 0u, 0u, 0u};
            for (u32 slot = 0u; slot < 4u; ++slot) {
                base_chains[slot] = driver.chainCount(touched_want[slot]);
                if (base_chains[slot] == 0u) {
                    return fail("D3", "baseline chains");
                }
            }

            // D5 baseline on the same fixture: drain setup presentation into
            // a fresh translator/pass, then re-submit unchanged. CPU
            // tile-cache writes (GridPass::uploadCount) must not move while
            // the resident set stays identical. No reset, no regenerate.
            std::println(stderr, "SMOKE-DIG-STAGE D5");
            GridPass pass{};
            ColonyTranslator submitter{};
            if (const std::error_code error = driver.presentEdits(submitter)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = submitter.submit(driver.grid(), pass)) {
                return std::unexpected{error};
            }
            const u32 chunks0 = submitter.submittedChunks();
            const u64 uploads0 = pass.uploadCount();
            if (chunks0 == 0u or uploads0 == 0u) {
                return fail("D5", "baseline empty");
            }
            if (const std::error_code error = submitter.submit(driver.grid(), pass)) {
                return std::unexpected{error};
            }
            if (submitter.submittedChunks() != chunks0 or pass.uploadCount() != uploads0) {
                return fail("D5", "unchanged resubmit work");
            }

            // D3 edit through the paused path only: cells and presentation
            // move while the clock, poses, and collider geometry stay frozen.
            // colliderPending is the subsystem queue, not driver dirt, so no
            // assertion precedes the first physics tick.
            const u64 dug_before = driver.digTotal();
            const u64 d3_ticks_before = driver.tickCount();
            const double d3_ms_before = driver.simMs();
            const Array<sim::BodyState> poses_before = posesOf(registry);
            if (const std::error_code error = driver.requestDig({dig_x, dig_y}, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(submitter)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = submitter.submit(driver.grid(), pass)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != dug_before + 16u) {
                return fail("D3", "island dig count");
            }
            const std::span<const sim::ChunkPos> dug_chunks = driver.lastEditChunks();
            if (dug_chunks.size() != 4u or not(dug_chunks[0u] == want_tl) or not(dug_chunks[1u] == want_tr) or
                not(dug_chunks[2u] == want_bl) or not(dug_chunks[3u] == want_br)) {
                return fail("D3", "touched order");
            }
            for (u32 island = 0u; island < 4u; ++island) {
                for (u32 dy = 0u; dy < 2u; ++dy) {
                    for (u32 dx = 0u; dx < 2u; ++dx) {
                        const auto cell =
                                driver.grid().getCell({island_min[island].m_x + dx, island_min[island].m_y + dy});
                        if (not cell or cell->m_element != kElementVacuum) {
                            return fail("D3", "island not vacuum");
                        }
                    }
                }
            }
            if (submitter.submittedChunks() != chunks0) {
                return fail("D5", "edited resubmit count");
            }
            const u64 uploads1 = pass.uploadCount();
            if (uploads1 - uploads0 != dug_chunks.size()) {
                return fail("D5", "reupload delta");
            }
            for (u32 frame = 0u; frame < 2u; ++frame) {
                if (const std::error_code error = driver.presentEdits(submitter)) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = submitter.submit(driver.grid(), pass)) {
                    return std::unexpected{error};
                }
            }
            if (pass.uploadCount() != uploads1 or submitter.submittedChunks() != chunks0) {
                return fail("D5", "repeat frame work");
            }
            if (driver.tickCount() != d3_ticks_before or driver.simMs() != d3_ms_before) {
                return fail("D3", "clock moved");
            }
            const Array<sim::BodyState> poses_after = posesOf(registry);
            if (poses_after.size() != poses_before.size()) {
                return fail("D3", "pose rows");
            }
            for (u32 row = 0u; row < poses_after.size(); ++row) {
                if (not sameBody(poses_after[row], poses_before[row])) {
                    return fail("D3", "poses moved");
                }
            }
            for (u32 slot = 0u; slot < 4u; ++slot) {
                if (driver.chainCount(touched_want[slot]) != base_chains[slot]) {
                    return fail("D3", "geometry moved paused");
                }
            }
            std::println(stderr, "SMOKE-DIG-STAGE D3 paused-frames=2 uploads={} touched=4 clock-frozen poses={}",
                uploads1 - uploads0, poses_before.size());

            // Resume exactly one tick: the retained collider dirt reaches the
            // subsystem queue and only a budget batch drains.
            if (kColliderBudget != 2u) {
                return fail("D3", "budget");
            }
            driver.setPaused(false);
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            const std::size_t remaining_first = driver.colliderPending();
            if (remaining_first == 0u) {
                return fail("D3", "no retained work");
            }
            if (const std::error_code error = driver.presentEdits(submitter)) {
                return std::unexpected{error};
            }
            u32 drain_ticks = 1u;
            while (driver.colliderPending() != 0u and drain_ticks < 100u) {
                if (const std::error_code error = driver.update(kTick)) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(submitter)) {
                    return std::unexpected{error};
                }
                ++drain_ticks;
            }
            if (driver.colliderPending() != 0u) {
                return fail("D3", "drain backlog");
            }
            if (drain_ticks <= 1u) {
                return fail("D3", "drain single tick");
            }
            for (u32 slot = 0u; slot < 4u; ++slot) {
                if (driver.chainCount(touched_want[slot]) >= base_chains[slot]) {
                    return fail("D3", "island chains remain");
                }
            }
            for (u32 row = 0u; row < 8u; ++row) {
                for (u32 col = 0u; col < 8u; ++col) {
                    const auto cell = driver.grid().getCell({dig_x + col, dig_y + row});
                    if (not cell or cell->m_element != kElementVacuum) {
                        return fail("D3", "hole not vacuum");
                    }
                }
            }
            std::println(stderr, "SMOKE-DIG-STAGE D3 first-remaining={} drain-ticks={} corner={},{} dig={},{}",
                remaining_first, drain_ticks, corner_x, corner_y, dig_x, dig_y);
            driver.setPaused(true);
            // D4: sleeping inside/outside witnesses via fixture BodyDefinitions
            // + restoreBodies. Scene-wide sleep is disabled in the driver, so
            // a public fixture that cannot establish sleeping witnesses is a
            // BLOCKER, never silent all-awake coverage.
            std::println(stderr, "SMOKE-DIG-STAGE D4");
            sim::Entity inside_entity{};
            sim::Entity outside_entity{};
            sim::BodyState inside_pose{};
            sim::BodyState outside_pose{};
            {
                bool have_inside = false;
                bool have_outside = false;
                u32 inside_cx = 0u;
                u32 inside_cy = 0u;
                u32 best_far = 0u;
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                    if (pose == nullptr or pose->m_x < 0.0f or pose->m_y < 0.0f) {
                        return fail("D4", "witness pose");
                    }
                    if (not have_inside) {
                        inside_entity = entity;
                        inside_pose = *pose;
                        inside_cx = static_cast<u32>(pose->m_x);
                        inside_cy = static_cast<u32>(pose->m_y);
                        have_inside = true;
                        continue;
                    }
                    const u32 cx = static_cast<u32>(pose->m_x);
                    const u32 cy = static_cast<u32>(pose->m_y);
                    const u32 dist = (cx >= inside_cx ? cx - inside_cx : inside_cx - cx) +
                                     (cy >= inside_cy ? cy - inside_cy : inside_cy - cy);
                    if (not have_outside or dist > best_far) {
                        best_far = dist;
                        outside_entity = entity;
                        have_outside = true;
                    }
                }
                if (not have_inside or not have_outside) {
                    return fail("D4", "witness count");
                }
                const sim::BodyState *outside_pose_ptr = registry.get<sim::BodyState>(outside_entity);
                if (outside_pose_ptr == nullptr) {
                    return fail("D4", "outside pose");
                }
                outside_pose = *outside_pose_ptr;
            }
            const u32 inside_cx = static_cast<u32>(inside_pose.m_x);
            const u32 inside_cy = static_cast<u32>(inside_pose.m_y);
            const sim::ChunkPos site_home = sim::chunkOf({inside_cx, inside_cy});
            const u32 site_x = site_home.m_x * sim::kChunkEdge + 10u;
            const u32 site_y = site_home.m_y * sim::kChunkEdge + 10u;
            const sim::GlobalCellPos site{site_x, site_y};
            if (site_x + 4u > sim::kWorldEdge or site_y + 4u > sim::kWorldEdge) {
                return fail("D4", "site outside");
            }

            // Fresh 4x4 rock target in the inside chunk: clear, build, settle.
            if (const std::error_code error = driver.requestDig(site, 4u, 4u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            const auto site_wall = driver.buildWall(Footprint{
                .m_min = site, .m_width = 4u, .m_height = 4u, .m_element = kElementRock
            });
            if (not site_wall) {
                return std::unexpected{site_wall.error()};
            }
            bool site_done = false;
            for (u32 tick = 0u; tick < 20u and not site_done; ++tick) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                const BuildErrand *errand = registry.get<BuildErrand>(*site_wall);
                site_done = errand != nullptr and errand->m_progress >= errand->m_total;
            }
            if (not site_done) {
                return fail("D4", "site build");
            }
            u32 site_settle = 0u;
            while (driver.colliderPending() != 0u and site_settle < 100u) {
                if (const std::error_code error = driver.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return std::unexpected{error};
                }
                ++site_settle;
            }
            if (driver.colliderPending() != 0u) {
                return fail("D4", "site settle");
            }
            const sim::BodyState *inside_now = registry.get<sim::BodyState>(inside_entity);
            const sim::BodyState *outside_now = registry.get<sim::BodyState>(outside_entity);
            if (inside_now == nullptr or outside_now == nullptr) {
                return fail("D4", "site poses");
            }
            inside_pose = *inside_now;
            outside_pose = *outside_now;
            const sim::ChunkPos site_chunk = sim::chunkOf(site);
            if (not(site_chunk == sim::chunkOf({static_cast<u32>(inside_pose.m_x), static_cast<u32>(inside_pose.m_y)}))) {
                return fail("D4", "site chunk");
            }
            const u64 outside_best =
                    chunkDist2(static_cast<u32>(outside_pose.m_x), static_cast<u32>(outside_pose.m_y), site_chunk);
            if (outside_best <= kWakeRadius2) {
                return fail("D4", "radius degenerate");
            }

            // Establish sleeping witnesses through the public fixture path.
            const auto sleepWitnesses = [&]() -> Expected<u32> {
                sim::BodyDefinition *inside_def = registry.get<sim::BodyDefinition>(inside_entity);
                sim::BodyDefinition *outside_def = registry.get<sim::BodyDefinition>(outside_entity);
                if (inside_def == nullptr or outside_def == nullptr) {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                inside_def->m_enable_sleep = true;
                inside_def->m_is_awake = false;
                outside_def->m_enable_sleep = true;
                outside_def->m_is_awake = false;
                if (const std::error_code error = driver.restoreBodies()) {
                    return std::unexpected{error};
                }
                const Array<std::pair<sim::Entity, bool> > calm = awakeOf(driver);
                const bool *inside_calm = nullptr;
                const bool *outside_calm = nullptr;
                for (const auto &row: calm) {
                    if (row.first == inside_entity) {
                        inside_calm = &row.second;
                    }
                    if (row.first == outside_entity) {
                        outside_calm = &row.second;
                    }
                }
                if (inside_calm == nullptr or outside_calm == nullptr or *inside_calm or *outside_calm) {
                    return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
                }
                return 2u;
            };
            const auto slept = sleepWitnesses();
            if (not slept) {
                std::println(stderr, "SMOKE-DIG-BLOCKER D4 sleep witness unavailable (world sleep disabled)");
                return fail("D4", "sleep witness blocker");
            }

            // Paused wake: cells change, inside wakes, outside stays asleep,
            // poses and clock frozen. Snapshot immediately, before any tick.
            const u64 wake_dug_before = driver.digTotal();
            const u64 wake_ticks_before = driver.tickCount();
            const double wake_ms_before = driver.simMs();
            const Array<sim::BodyState> wake_poses_before = posesOf(registry);
            if (const std::error_code error = driver.requestDig(site, 4u, 4u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            const sim::Snapshot wake_snap = driver.snapshotWithBodies();
            if (driver.digTotal() != wake_dug_before + 16u) {
                return fail("D4", "wake dig count");
            }
            if (driver.lastEditChunks().size() != 1u or not(driver.lastEditChunks()[0u] == site_chunk)) {
                return fail("D4", "wake touched");
            }
            const bool *inside_awake = nullptr;
            const bool *outside_awake = nullptr;
            for (const sim::BodyRecord &record: wake_snap.m_bodies.m_records) {
                if (record.m_entity == inside_entity) {
                    inside_awake = &record.m_awake;
                }
                if (record.m_entity == outside_entity) {
                    outside_awake = &record.m_awake;
                }
            }
            if (inside_awake == nullptr or outside_awake == nullptr) {
                return fail("D4", "wake record");
            }
            if (not*inside_awake or *outside_awake) {
                return fail("D4", "wake flags");
            }
            if (driver.tickCount() != wake_ticks_before or driver.simMs() != wake_ms_before) {
                return fail("D4", "wake clock");
            }
            const Array<sim::BodyState> wake_poses_after = posesOf(registry);
            if (wake_poses_after.size() != wake_poses_before.size()) {
                return fail("D4", "wake rows");
            }
            for (u32 row = 0u; row < wake_poses_after.size(); ++row) {
                if (not sameBody(wake_poses_after[row], wake_poses_before[row])) {
                    return fail("D4", "wake poses");
                }
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            std::println(stderr, "SMOKE-DIG-STAGE D4 wake inside=1 outside=1 site={},{}", site_x, site_y);

            // Vacuum no-op: re-establish sleep, dig open water, neither wakes.
            const auto slept_again = sleepWitnesses();
            if (not slept_again) {
                std::println(stderr, "SMOKE-DIG-BLOCKER D4 sleep witness unavailable (re-establish)");
                return fail("D4", "sleep witness blocker");
            }
            sim::GlobalCellPos vacuum{sim::kWorldEdge, sim::kWorldEdge};
            for (u32 y = sim::kWorldEdge / 4u;
                 y < sim::kWorldEdge * 3u / 4u and vacuum.m_x >= sim::kWorldEdge;
                 ++y) {
                for (u32 x = sim::kWorldEdge / 4u; x < sim::kWorldEdge * 3u / 4u; ++x) {
                    bool open = true;
                    for (u32 dy = 0u; dy < 8u and open; ++dy) {
                        for (u32 dx = 0u; dx < 8u; ++dx) {
                            if (not isWalkable(driver.grid(), {x + dx, y + dy})) {
                                open = false;
                                break;
                            }
                        }
                    }
                    if (open) {
                        vacuum = {x, y};
                        break;
                    }
                }
            }
            if (vacuum.m_x >= sim::kWorldEdge) {
                return fail("D4", "no vacuum");
            }
            const u64 noop_dug_before = driver.digTotal();
            Array<sim::ChunkPos> noop_touched_before{};
            for (const sim::ChunkPos chunk: driver.lastEditChunks()) {
                noop_touched_before.push_back(chunk);
            }
            const Array<sim::BodyState> noop_poses_before = posesOf(registry);
            if (const std::error_code error = driver.requestDig(vacuum, 8u, 8u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            const sim::Snapshot noop_snap = driver.snapshotWithBodies();
            if (driver.digTotal() != noop_dug_before) {
                return fail("D4", "noop dug");
            }
            if (driver.lastEditChunks().size() != noop_touched_before.size()) {
                return fail("D4", "noop touched");
            }
            for (u32 slot = 0u; slot < noop_touched_before.size(); ++slot) {
                if (not(driver.lastEditChunks()[slot] == noop_touched_before[slot])) {
                    return fail("D4", "noop touched");
                }
            }
            for (const sim::BodyRecord &record: noop_snap.m_bodies.m_records) {
                if ((record.m_entity == inside_entity or record.m_entity == outside_entity) and record.m_awake) {
                    return fail("D4", "noop woke");
                }
            }
            const Array<sim::BodyState> noop_poses_after = posesOf(registry);
            if (noop_poses_after.size() != noop_poses_before.size()) {
                return fail("D4", "noop rows");
            }
            for (u32 row = 0u; row < noop_poses_after.size(); ++row) {
                if (not sameBody(noop_poses_after[row], noop_poses_before[row])) {
                    return fail("D4", "noop poses");
                }
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            std::println(stderr, "SMOKE-DIG-STAGE D4 noop witnesses=2 vacuum={},{}", vacuum.m_x, vacuum.m_y);

            // D6: snapshot v3 pin + capture-is-not-flush + restore-clear +
            // matched-command. sim::apply writes deltas only, never a
            // baseline (Sim.Snapshot.cpp:322-341); capture covers dirty
            // chunks only (:244-260). Blank grids serve delta-cell spot
            // checks; restores apply onto initialized matching-seed drivers.
            std::println(stderr, "SMOKE-DIG-STAGE D6");
            driver.setPaused(true);
            const sim::GlobalCellPos pending_rect{origin.m_x + 12u, origin.m_y + 4u};
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto cell = driver.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not cell) {
                        return std::unexpected{cell.error()};
                    }
                    if (cell->m_element != kElementRock) {
                        return fail("D6", "pending not rock");
                    }
                }
            }
            const u32 dug_before_d6 = driver.digTotal();
            const u64 ticks_before_d6 = driver.tickCount();
            if (const std::error_code error = driver.requestDig(pending_rect, 4u, 4u)) {
                return std::unexpected{error};
            }
            const sim::Snapshot snap = driver.snapshotWithBodies();
            if (snap.m_version != 3u or snap.m_version != sim::kSnapshotVersion) {
                return fail("D6", "snapshot version");
            }
            const auto bytes = sim::save(snap);
            if (not bytes) {
                return std::unexpected{bytes.error()};
            }
            const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
            if (not loaded) {
                return std::unexpected{loaded.error()};
            }
            if (loaded->m_version != 3u or not(*loaded == snap)) {
                return fail("D6", "round trip");
            }

            // Blank-grid delta check: the queued intent is driver-side, so
            // the captured snapshot still carries rock on all 16 cells.
            sim::ChunkGrid fresh{};
            if (const std::error_code error = sim::apply(fresh, *loaded)) {
                return std::unexpected{error};
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto cell = fresh.getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not cell or cell->m_element != kElementRock) {
                        return fail("D6", "intent captured");
                    }
                }
            }

            // Capture-is-not-flush: restore the pre-consume snapshot onto a
            // matching-seed driver with no reissue; it must stay rock while
            // the source consumes the intent on the next paused update.
            ColonyDriver still_rock{};
            if (const std::error_code error = still_rock.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            still_rock.setPaused(true);
            if (const std::error_code error = sim::apply(still_rock.grid(), *loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::restore(still_rock.registry(), *loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = still_rock.restoreBodies()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = still_rock.rebuildReplayColliders(*loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = driver.presentEdits(translator)) {
                return std::unexpected{error};
            }
            if (driver.digTotal() != dug_before_d6 + 16u) {
                return fail("D6", "pending dug count");
            }
            if (driver.tickCount() != ticks_before_d6) {
                return fail("D6", "paused clock moved");
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto cell = driver.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not cell or cell->m_element != kElementVacuum) {
                        return fail("D6", "pending not applied");
                    }
                }
            }
            ColonyTranslator still_present{};
            if (const std::error_code error = still_rock.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = still_rock.presentEdits(still_present)) {
                return std::unexpected{error};
            }
            if (still_rock.digTotal() != 0u) {
                return fail("D6", "restored dug without reissue");
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto cell = still_rock.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not cell or cell->m_element != kElementRock) {
                        return fail("D6", "capture flushed");
                    }
                }
            }

            // Restore-clear: a queued intent on the target before the
            // apply→restore→restoreBodies→rebuildReplayColliders sequence is
            // dropped by the explicit Driver clears, so the next paused
            // update digs nothing.
            ColonyDriver cleared{};
            if (const std::error_code error = cleared.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            cleared.setPaused(true);
            if (const std::error_code error = cleared.requestDig(pending_rect, 4u, 4u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::apply(cleared.grid(), *loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::restore(cleared.registry(), *loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.restoreBodies()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.rebuildReplayColliders(*loaded)) {
                return std::unexpected{error};
            }
            ColonyTranslator cleared_present{};
            if (const std::error_code error = cleared.update(kTick)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.presentEdits(cleared_present)) {
                return std::unexpected{error};
            }
            if (cleared.digTotal() != 0u) {
                return fail("D6", "restore kept intent");
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto cell = cleared.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not cell or cell->m_element != kElementRock) {
                        return fail("D6", "restore cleared cells");
                    }
                }
            }

            // Matched-command next-tick: reissue the same dig to both
            // symmetric restored drivers, step once paired, and require
            // identical vacuum; then serialize the post-tick snapshot.
            if (const std::error_code error = still_rock.requestDig(pending_rect, 4u, 4u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.requestDig(pending_rect, 4u, 4u)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = still_rock.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = still_rock.presentEdits(still_present)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.presentEdits(cleared_present)) {
                return std::unexpected{error};
            }
            if (still_rock.digTotal() != 16u or cleared.digTotal() != 16u) {
                return fail("D6", "matched dug count");
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto left = still_rock.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    const auto right = cleared.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not left or not right or not(*left == *right)) {
                        return fail("D6", "matched next-tick match");
                    }
                    if (left->m_element != kElementVacuum) {
                        return fail("D6", "matched not applied");
                    }
                }
            }
            if (not gridsEqual(still_rock.grid(), cleared.grid())) {
                return fail("D6", "matched grids");
            }
            const sim::Snapshot snap_post = still_rock.snapshotWithBodies();
            if (snap_post.m_version != 3u or snap_post.m_version != sim::kSnapshotVersion) {
                return fail("D6", "post-tick version");
            }
            const auto bytes_post = sim::save(snap_post);
            if (not bytes_post) {
                return std::unexpected{bytes_post.error()};
            }
            const auto loaded_post = sim::load(std::span<const u8>{bytes_post->data(), bytes_post->size()});
            if (not loaded_post or not(*loaded_post == snap_post)) {
                return fail("D6", "post-tick round trip");
            }
            sim::ChunkGrid fresh_post{};
            if (const std::error_code error = sim::apply(fresh_post, *loaded_post)) {
                return std::unexpected{error};
            }
            for (u32 dy = 0u; dy < 4u; ++dy) {
                for (u32 dx = 0u; dx < 4u; ++dx) {
                    const auto live = still_rock.grid().getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    const auto replay = fresh_post.getCell({pending_rect.m_x + dx, pending_rect.m_y + dy});
                    if (not live or not replay or not(*live == *replay)) {
                        return fail("D6", "post-tick match");
                    }
                    if (live->m_element != kElementVacuum) {
                        return fail("D6", "post-tick vacuum");
                    }
                }
            }
            if (snap_post.m_deltas.empty()) {
                return fail("D6", "deltas empty");
            }
            if (const std::error_code error = still_rock.shutdown()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = cleared.shutdown()) {
                return std::unexpected{error};
            }
            std::println(stderr, "SMOKE-DIG-STAGE D6 version=3 pre=rock post=vacuum matched=16");

            // D7: determinism. Identical command schedules on two fresh
            // drivers compare exact entity-keyed state with nonzero agents,
            // then a symmetric restore pair (init→apply→restore→
            // restoreBodies→rebuildReplayColliders) compares immediately and
            // after each paired tick. Live-contact waiver holds: equality is
            // live-live plus restored-restored, never live==restored.
            std::println(stderr, "SMOKE-DIG-STAGE D7");
            ColonyDriver twin_a{};
            ColonyDriver twin_b{};
            if (const std::error_code error = twin_a.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            if (const std::error_code error = twin_b.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            twin_a.setPaused(true);
            twin_b.setPaused(true);
            const auto run_schedule = [](ColonyDriver &twin) -> std::error_code {
                sim::GlobalCellPos site{sim::kWorldEdge, sim::kWorldEdge};
                for (u32 y = sim::kWorldEdge / 4u;
                     y < sim::kWorldEdge * 3u / 4u and site.m_x >= sim::kWorldEdge;
                     ++y) {
                    for (u32 x = sim::kWorldEdge / 4u; x < sim::kWorldEdge * 3u / 4u; ++x) {
                        if (sim::chunkOf(sim::GlobalCellPos{x, y}) !=
                            sim::chunkOf(sim::GlobalCellPos{x + 7u, y + 7u})) {
                            continue;
                        }
                        bool solid = true;
                        for (u32 dy = 0u; dy < 8u and solid; ++dy) {
                            for (u32 dx = 0u; dx < 8u; ++dx) {
                                const auto cell = twin.grid().getCell({x + dx, y + dy});
                                if (not cell or cell->m_element != kElementRock) {
                                    solid = false;
                                    break;
                                }
                            }
                        }
                        if (solid) {
                            site = {x, y};
                            break;
                        }
                    }
                }
                if (site.m_x >= sim::kWorldEdge) {
                    return std::make_error_code(std::errc::state_not_recoverable);
                }
                ColonyTranslator local{};
                if (const std::error_code error = twin.requestDig(site, 8u, 8u)) {
                    return error;
                }
                for (u32 tick = 0u; tick < 3u; ++tick) {
                    if (const std::error_code error = twin.stepOne()) {
                        return error;
                    }
                    if (const std::error_code error = twin.presentEdits(local)) {
                        return error;
                    }
                }
                if (const std::error_code error = twin.requestDig(site, 8u, 8u)) {
                    return error;
                }
                return twin.stepOne();
            };
            if (const std::error_code error = run_schedule(twin_a)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = run_schedule(twin_b)) {
                return std::unexpected{error};
            }
            if (twin_a.digTotal() != twin_b.digTotal() or twin_a.digTotal() != 64u) {
                return fail("D7", "twin totals");
            }

            // Entity-keyed collectors: rows sort by entity so identities
            // compare alongside values. Field-wise == and the 6-field pose
            // compare only, never raw padded-struct memcmp.
            const auto membersKeyed = [&](ColonyDriver &twin) {
                Array<sim::Entity> out = membersOf(twin.registry());
                std::ranges::sort(out);
                return out;
            };
            const auto bodiesKeyed = [&](ColonyDriver &twin) {
                sim::Snapshot snap = twin.snapshotWithBodies();
                Array<sim::BodyRecord> out{};
                for (const sim::BodyRecord &record: snap.m_bodies.m_records) {
                    out.push_back(record);
                }
                std::ranges::sort(out, {}, [](const sim::BodyRecord &record) -> sim::Entity {
                    return record.m_entity;
                });
                return out;
            };
            const auto agentsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, Agent> > out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    out.push_back({entity, agent});
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto needsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, Needs> > out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const Needs *needs = registry.get<Needs>(entity);
                    if (needs != nullptr) {
                        out.push_back({entity, *needs});
                    }
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto plansKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, Plan> > out{};
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    (void) agent;
                    const Plan *plan = registry.get<Plan>(entity);
                    if (plan != nullptr) {
                        out.push_back({entity, *plan});
                    }
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto reqsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, PathReq> > out{};
                for (const auto &[entity, req]: registry.view<PathReq>()) {
                    out.push_back({entity, req});
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto compsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, PathComp> > out{};
                for (const auto &[entity, comp]: registry.view<PathComp>()) {
                    out.push_back({entity, comp});
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto errandsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, BuildErrand> > out{};
                for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
                    out.push_back({entity, errand});
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto printsKeyed = [&](sim::Registry &registry) {
                Array<std::pair<sim::Entity, Footprint> > out{};
                for (const auto &[entity, print]: registry.view<Footprint>()) {
                    out.push_back({entity, print});
                }
                std::ranges::sort(out, {}, [](const auto &row) -> sim::Entity { return row.first; });
                return out;
            };
            const auto checkPair = [&](ColonyDriver &left, ColonyDriver &right,
                                       const char *const stage) -> Expected<u32> {
                const Array<sim::Entity> members_left = membersKeyed(left);
                const Array<sim::Entity> members_right = membersKeyed(right);
                if (members_left.empty() or members_left.size() != members_right.size()) {
                    return fail("D7", stage);
                }
                if (not std::ranges::equal(members_left, members_right)) {
                    return fail("D7", stage);
                }
                const Array<sim::BodyRecord> bodies_left = bodiesKeyed(left);
                const Array<sim::BodyRecord> bodies_right = bodiesKeyed(right);
                if (bodies_left.size() != bodies_right.size() or bodies_left.empty()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < bodies_left.size(); ++row) {
                    if (not(bodies_left[row].m_entity == bodies_right[row].m_entity)) {
                        return fail("D7", stage);
                    }
                    if (bodies_left[row].m_awake != bodies_right[row].m_awake) {
                        return fail("D7", stage);
                    }
                    if (not sameBody(bodies_left[row].m_state, bodies_right[row].m_state)) {
                        return fail("D7", stage);
                    }
                }
                const auto agents_left = agentsKeyed(left.registry());
                const auto agents_right = agentsKeyed(right.registry());
                if (agents_left.size() != agents_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < agents_left.size(); ++row) {
                    if (not(agents_left[row].first == agents_right[row].first) or
                        not(agents_left[row].second == agents_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto needs_left = needsKeyed(left.registry());
                const auto needs_right = needsKeyed(right.registry());
                if (needs_left.size() != needs_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < needs_left.size(); ++row) {
                    if (not(needs_left[row].first == needs_right[row].first) or
                        not(needs_left[row].second == needs_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto plans_left = plansKeyed(left.registry());
                const auto plans_right = plansKeyed(right.registry());
                if (plans_left.size() != plans_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < plans_left.size(); ++row) {
                    if (not(plans_left[row].first == plans_right[row].first) or
                        not(plans_left[row].second == plans_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto reqs_left = reqsKeyed(left.registry());
                const auto reqs_right = reqsKeyed(right.registry());
                if (reqs_left.size() != reqs_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < reqs_left.size(); ++row) {
                    if (not(reqs_left[row].first == reqs_right[row].first) or
                        not(reqs_left[row].second == reqs_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto comps_left = compsKeyed(left.registry());
                const auto comps_right = compsKeyed(right.registry());
                if (comps_left.size() != comps_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < comps_left.size(); ++row) {
                    if (not(comps_left[row].first == comps_right[row].first) or
                        not(comps_left[row].second == comps_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto errands_left = errandsKeyed(left.registry());
                const auto errands_right = errandsKeyed(right.registry());
                if (errands_left.size() != errands_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < errands_left.size(); ++row) {
                    if (not(errands_left[row].first == errands_right[row].first) or
                        not(errands_left[row].second == errands_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                const auto prints_left = printsKeyed(left.registry());
                const auto prints_right = printsKeyed(right.registry());
                if (prints_left.size() != prints_right.size()) {
                    return fail("D7", stage);
                }
                for (u32 row = 0u; row < prints_left.size(); ++row) {
                    if (not(prints_left[row].first == prints_right[row].first) or
                        not(prints_left[row].second == prints_right[row].second)) {
                        return fail("D7", stage);
                    }
                }
                if (not gridsEqual(left.grid(), right.grid())) {
                    return fail("D7", stage);
                }
                return 0u;
            };

            // Same-seed live double-run equality: nonzero agents plus the
            // full entity-keyed state and touched-chunk order.
            if (membersKeyed(twin_a).size() != kAgentCount or membersKeyed(twin_b).size() != kAgentCount) {
                return fail("D7", "twin agents");
            }
            if (const auto live_pair = checkPair(twin_a, twin_b, "twin state")) {
                if (*live_pair != 0u) {
                    return fail("D7", "twin state");
                }
            } else {
                return std::unexpected{live_pair.error()};
            }
            if (not gridsEqual(twin_a.grid(), twin_b.grid())) {
                return fail("D7", "twin grids");
            }
            if (twin_a.lastEditChunks().size() != twin_b.lastEditChunks().size()) {
                return fail("D7", "twin touched");
            }
            for (u32 i = 0u; i < twin_a.lastEditChunks().size(); ++i) {
                if (not(twin_a.lastEditChunks()[i] == twin_b.lastEditChunks()[i])) {
                    return fail("D7", "twin touched");
                }
            }
            std::println(stderr, "SMOKE-DIG-STAGE D7 live twins equal agents={}", membersKeyed(twin_a).size());

            const sim::Snapshot twin_snap = twin_a.snapshotWithBodies();
            const auto twin_bytes = sim::save(twin_snap);
            if (not twin_bytes) {
                return std::unexpected{twin_bytes.error()};
            }
            const auto twin_loaded = sim::load(std::span<const u8>{twin_bytes->data(), twin_bytes->size()});
            if (not twin_loaded) {
                return std::unexpected{twin_loaded.error()};
            }
            ColonyDriver third{};
            ColonyDriver fourth{};
            if (const std::error_code error = third.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            if (const std::error_code error = fourth.init(ColonyDriverDesc{.m_seed = kSeed})) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::apply(third.grid(), *twin_loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::restore(third.registry(), *twin_loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = third.restoreBodies()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = third.rebuildReplayColliders(*twin_loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::apply(fourth.grid(), *twin_loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = sim::restore(fourth.registry(), *twin_loaded)) {
                return std::unexpected{error};
            }
            if (const std::error_code error = fourth.restoreBodies()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = fourth.rebuildReplayColliders(*twin_loaded)) {
                return std::unexpected{error};
            }
            third.setPaused(true);
            fourth.setPaused(true);

            // Symmetric restored-pair equality immediately, then after each
            // paired tick: grid cells, entity identities, 6 pose/vel
            // fields, needs, plans, path/build state, and awake flags.
            // Live-contact waiver: contact-active physics may differ
            // live-vs-restored, so equality is required only within each
            // symmetric pair, never live==restored bit-exact.
            if (const auto restored_now = checkPair(third, fourth, "restored immediate")) {
                if (*restored_now != 0u) {
                    return fail("D7", "restored immediate");
                }
            } else {
                return std::unexpected{restored_now.error()};
            }
            ColonyTranslator replay_present_c{};
            ColonyTranslator replay_present_d{};
            for (u32 tick = 0u; tick < 10u; ++tick) {
                if (const std::error_code error = third.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = fourth.stepOne()) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = third.presentEdits(replay_present_c)) {
                    return std::unexpected{error};
                }
                if (const std::error_code error = fourth.presentEdits(replay_present_d)) {
                    return std::unexpected{error};
                }
                if (const auto replay_pair = checkPair(third, fourth, "restored replay")) {
                    if (*replay_pair != 0u) {
                        return fail("D7", "restored replay");
                    }
                } else {
                    return std::unexpected{replay_pair.error()};
                }
            }
            std::println(stderr, "SMOKE-DIG-STAGE D7 restored pair equal immediate+10 ticks agents={}",
                membersKeyed(third).size());

            // Scoped teardown: every initialized driver shuts down even when an
            // earlier one fails; the first failure is preserved and returned.
            const u32 dug_total = driver.digTotal();
            std::error_code first_error{};
            if (const std::error_code error = driver.shutdown()) {
                first_error = error;
            }
            if (const std::error_code error = twin_a.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (const std::error_code error = twin_b.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (const std::error_code error = third.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (const std::error_code error = fourth.shutdown()) {
                if (not first_error) {
                    first_error = error;
                }
            }
            if (first_error) {
                return std::unexpected{first_error};
            }
            return dug_total;
        }

        class ColonySmokeApp final : public Application {
        public:
            explicit ColonySmokeApp(const std::span<const char *const> argv)
                : Application(ApplicationDomain{
                    .m_is_headless = true,
                    .m_is_interactive = false,
                    .m_needs_presence = false,
                    .m_needs_rendering = true,
                    .m_needs_user_interface = false,
                }, "colony-smoke", argv) {
            }

            [[nodiscard]] u32 visibleTiles() const noexcept { return m_visible_tiles; }
            [[nodiscard]] const std::array<u8, 4u> &pixel() const noexcept { return m_pixel; }
            [[nodiscard]] bool verified() const noexcept { return m_verified; }
            [[nodiscard]] std::error_code shutdownError() const noexcept { return m_shutdown_error; }

        protected:
            [[nodiscard]] std::error_code initialize() override {
                m_base_started = true;
                if (const std::error_code error = Application::initialize()) [[unlikely]] {
                    return error;
                }
                m_base_ready = true;

                const auto rhi_service = getServices().tryGet<IRhiService>();
                const auto shader_service = getServices().tryGet<IShaderService>();
                if (not rhi_service or not shader_service) [[unlikely]] {
                    return std::make_error_code(std::errc::not_connected);
                }

                m_pass_started = true;
                if (const std::error_code error = m_pass.initialize(*rhi_service, *shader_service,
                    getContentDir().path())) [[unlikely]] {
                    return error;
                }

                m_driver_started = true;
                if (const std::error_code error = m_driver.init(ColonyDriverDesc{.m_seed = kSeed})) [[unlikely]] {
                    return error;
                }

                const auto core = findCoreCell(m_driver.grid());
                if (not core) [[unlikely]] {
                    return core.error();
                }
                m_core = *core;

                // orthoD3D spans [0, width] x [0, height]. Center the actual
                // rock cell at the center of a 256x256 target with 1/4 unit
                // per pixel, well away from cell boundaries.
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_ortho_scale = 0.25f;
                model.m_origin = float3{
                    static_cast<float>(core->m_x) + 0.5f - 32.0f,
                    static_cast<float>(core->m_y) + 0.5f - 32.0f,
                    -0.5f,
                };
                Camera camera{ECameraProjection::orthographic};
                camera.updateModel(TimeSpan{}, model,
                    Viewport{PixelRect{0, 0, static_cast<int>(kExtent), static_cast<int>(kExtent)}});
                m_camera_view = camera.getSnapshot();
                return {};
            }

            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                if (const std::error_code error = Application::update(dt)) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = m_driver.update(dt)) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = m_translator.submit(m_driver.grid(), m_pass)) [[unlikely]] {
                    return error;
                }
                return m_pass.update(dt, m_camera_view);
            }

            [[nodiscard]] std::error_code render() override {
                if (const std::error_code error = Application::render()) [[unlikely]] {
                    return error;
                }

                const auto rhi_service = getServices().tryGet<IRhiService>();
                if (not rhi_service) [[unlikely]] {
                    return std::make_error_code(std::errc::not_connected);
                }
                rhi::IDevice &device = rhi_service->getDevice();
                Renderer &renderer = getRenderer();

                const sim::ChunkPos chunk = sim::chunkOf(m_core);
                const i32 left = static_cast<i32>(chunk.m_x * sim::kChunkEdge);
                const i32 bottom = static_cast<i32>(chunk.m_y * sim::kChunkEdge);
                const GridTileSubmission sample{
                    .m_chunk_id = sim::chunkIndexOf(chunk),
                    .m_tile_range = {
                        left, bottom, left + static_cast<i32>(sim::kChunkEdge),
                        bottom + static_cast<i32>(sim::kChunkEdge)
                    },
                };
                const auto plan = GridPass::planTiles(m_camera_view,
                    std::span<const GridTileSubmission>{&sample, 1u});
                if (not plan) [[unlikely]] {
                    return plan.error();
                }
                m_visible_tiles = static_cast<u32>(plan->m_tiles.size());
                if (m_visible_tiles == 0u or m_pass.stagedTileCount() == 0u)
                [[unlikely]] {
                    return std::make_error_code(std::errc::no_message_available);
                }

                const auto wait_for_gpu_idle = [&renderer]() -> std::error_code {
                    return renderer.waitOnHost();
                };
                if (const std::error_code error = m_pass.prepareCellUploads(device, wait_for_gpu_idle)) [[unlikely]] {
                    return error;
                }

                rhi::TextureDesc desc{};
                desc.type = rhi::TextureType::Texture2D;
                desc.size = {kExtent, kExtent, 1u};
                desc.arrayLength = 1u;
                desc.mipCount = 1u;
                desc.format = rhi::Format::RGBA8Unorm;
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = rhi::TextureUsage::RenderTarget;
                desc.defaultState = rhi::ResourceState::RenderTarget;
                desc.label = "colony smoke";
                if (const std::error_code error = make_error_code(device.createTexture(desc, nullptr,
                    m_target.writeRef()))) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = renderer.renderToTexture(*m_target,
                    {DrawSubmission{m_pass}}, ColorAttachmentOps{})) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = renderer.waitOnHost()) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = readCenterPixel(device, *m_target, m_pixel)) [[unlikely]] {
                    return error;
                }

                // Element 1 (temperate rock) maps to shader tint (0.85,0.15,0.15).
                // This rejects the clear (0.1,0.1,0.2), transparent vacuum,
                // and flat-tile fallback without treating a successful submit
                // as a successful rasterization.
                if (m_pixel[0u] < 160u or m_pixel[1u] > 85u or m_pixel[2u] > 85u or m_pixel[3u] < 240u)
                [[unlikely]] {
                    return std::make_error_code(std::errc::state_not_recoverable);
                }
                m_verified = true;
                requestExit();
                return {};
            }

            [[nodiscard]] std::error_code shutdown() override {
                std::error_code first_error{};
                if (m_base_ready) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonySmoke, first_error, getRenderer().waitOnHost());
                }
                m_target.setNull();
                if (m_pass_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonySmoke, first_error, m_pass.shutdown());
                    m_pass_started = false;
                }
                if (m_driver_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonySmoke, first_error, m_driver.shutdown());
                    m_driver_started = false;
                }
                if (m_base_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonySmoke, first_error, Application::shutdown());
                    m_base_started = false;
                    m_base_ready = false;
                }
                m_shutdown_error = first_error;
                return first_error;
            }

        private:
            ColonyDriver m_driver{};
            ColonyTranslator m_translator{};
            GridPass m_pass{};
            CameraSnapshot m_camera_view{};
            rhi::ComPtr<rhi::ITexture> m_target{};
            sim::GlobalCellPos m_core{};
            std::array<u8, 4u> m_pixel{};
            std::error_code m_shutdown_error{};
            u32 m_visible_tiles{};
            bool m_base_ready{false};
            bool m_base_started{false};
            bool m_pass_started{false};
            bool m_driver_started{false};
            bool m_verified{false};
        };
    }

    /// Deterministic Slice 4 self-checks: seed-identical spawns, plan
        /// coverage, movement, double-run equality, and bodies snapshot
        /// replay continuation.
    [[nodiscard]] Expected<std::array<u32, 3u> > checkAgents(const char *&stage) {
        constexpr u32 kTicks = 100u;
        constexpr u32 kWallEdge = 32u;
        const auto drive = [&](ColonyDriver &driver, sim::GlobalCellPos site, float &peak, u32 &peak_tick,
                               float &final_disp) -> std::error_code {
            driver.setPaused(true);
            ColonyTranslator translator{};
            peak = 0.0f;
            stage = "wall";
            const auto wall = driver.buildWall(Footprint{
                .m_min = site,
                .m_width = kWallEdge,
                .m_height = kWallEdge,
                .m_element = kElementRock,
                .m_solid = true
            });
            if (not wall) {
                return wall.error();
            }
            // One wall per remaining agent at its own spawn: with
            // nearest-errand assignment every agent gets a short completable
            // trip instead of all piling onto the first wall. Slot order is
            // deterministic, so double-run equality is preserved.
            // Collect first, mutate after: buildWall creates entities, which
            // reallocates registry storage and invalidates a live view.
            Array<sim::GlobalCellPos> extra_mins{};
            {
                sim::Registry &seeds = driver.registry();
                u32 slot = 0u;
                for (const auto &[entity, agent]: seeds.view<Agent>()) {
                    (void) entity;
                    ++slot;
                    if (slot <= 1u) {
                        continue;
                    }
                    const u32 wx = agent.m_spawn.m_x <= 4038u ? agent.m_spawn.m_x + 25u : agent.m_spawn.m_x - 57u;
                    const u32 wy = agent.m_spawn.m_y <= 4063u ? agent.m_spawn.m_y : 4063u;
                    extra_mins.push_back(sim::GlobalCellPos{wx, wy});
                }
            }
            for (const sim::GlobalCellPos &extra_min: extra_mins) {
                const auto extra = driver.buildWall(Footprint{
                    .m_min = extra_min,
                    .m_width = kWallEdge,
                    .m_height = kWallEdge,
                    .m_element = kElementRock,
                    .m_solid = true
                });
                if (not extra) {
                    return extra.error();
                }
            }
            sim::Registry &registry = driver.registry();
            // Writes, not progress, are the budgeted quantity: skipped
            // already-rock cells advance progress for free.
            const auto rockInWall = [&](const sim::GlobalCellPos min) -> Expected<u32> {
                u32 rock = 0u;
                for (u32 dy = 0u; dy < kWallEdge; ++dy) {
                    for (u32 dx = 0u; dx < kWallEdge; ++dx) {
                        const auto cell = driver.grid().getCell({min.m_x + dx, min.m_y + dy});
                        if (not cell) {
                            return std::unexpected{cell.error()};
                        }
                        if (cell->m_element == kElementRock) {
                            ++rock;
                        }
                    }
                }
                return rock;
            };
            const auto rock_before = rockInWall(site);
            if (not rock_before) {
                return rock_before.error();
            }
            peak_tick = 0u;
            final_disp = 0.0f;
            for (u32 tick = 0u; tick < kTicks; ++tick) {
                stage = "step";
                if (const std::error_code error = driver.stepOne()) {
                    if (driver.stepStage()[0] != '\0') {
                        stage = driver.stepStage();
                    }
                    return error;
                }
                stage = "present";
                // Mirror production ordering (main.cpp): systems consume the
                // edit-dirty list through the collider rebuild, then the
                // translator drains it. Without the drain the dirty set
                // re-expands every tick and the pending queue never empties.
                if (const std::error_code error = driver.presentEdits(translator)) {
                    return error;
                }
                // Budget compliance: exactly one 64-write tick must be
                // visible — never zero (build stalled) and never more than
                // the per-tick cell budget. The wall is served first with a
                // full budget, so all 64 writes land in its footprint.
                if (tick == 0u) {
                    const BuildErrand *raised = registry.get<BuildErrand>(*wall);
                    const auto rock_after = rockInWall(site);
                    if (raised == nullptr or raised->m_total != 1024u or not rock_after or
                        *rock_after != *rock_before + 64u) {
                        stage = "budget";
                        return std::make_error_code(std::errc::state_not_recoverable);
                    }
                }
                for (const auto &[entity, agent]: registry.view<Agent>()) {
                    const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                    if (pose == nullptr) {
                        return std::make_error_code(std::errc::state_not_recoverable);
                    }
                    const float dx = pose->m_x - (static_cast<float>(agent.m_spawn.m_x) + 0.5f);
                    const float dy = pose->m_y - (static_cast<float>(agent.m_spawn.m_y) + 0.5f);
                    const float displacement = dx * dx + dy * dy;
                    if (displacement > peak) {
                        peak = displacement;
                        peak_tick = tick;
                    }
                }
            }
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose == nullptr) {
                    return std::make_error_code(std::errc::state_not_recoverable);
                }
                const float dx = pose->m_x - (static_cast<float>(agent.m_spawn.m_x) + 0.5f);
                const float dy = pose->m_y - (static_cast<float>(agent.m_spawn.m_y) + 0.5f);
                const float displacement = dx * dx + dy * dy;
                if (displacement > final_disp) {
                    final_disp = displacement;
                }
            }
            return {};
        };
        const auto poses = [](ColonyDriver &driver) {
            Array<sim::BodyState> states{};
            sim::Registry &registry = driver.registry();
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                const Agent *identity = registry.get<Agent>(entity);
                if (pose == nullptr or identity == nullptr) {
                    continue;
                }
                states.push_back(*pose);
            }
            return states;
        };
        const auto same_pose = [](const sim::BodyState &a, const sim::BodyState &b) noexcept {
            return a.m_x == b.m_x and
                   a.m_y == b.m_y and
                   a.m_angle == b.m_angle and
                   a.m_velocity_x == b.m_velocity_x and
                   a.m_velocity_y == b.m_velocity_y and
                   a.m_angular_velocity == b.m_angular_velocity;
        };

        stage = "spawn";
        ColonyDriver first{};
        if (const std::error_code error = first.init(ColonyDriverDesc{.m_seed = kSeed})) {
            return std::unexpected{error};
        }
        if (first.agentSummaries().size() != kAgentCount) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        stage = "site";
        // Staging in BFS budget, wall derived from it: BFS needs O(d^2)
        // expansions in open space, so a far goal can never complete within
        // a smoke-sized tick budget at 256 expansions/tick. A walkable cell
        // 12-30 cells from agent 0's spawn completes in a few ticks and the
        // agent walks it; the wall sits adjacent for errand mechanics.
        // Pure function of the seed (spawn is seed-derived).
        sim::GlobalCellPos site{sim::kWorldEdge, sim::kWorldEdge};
        {
            sim::Registry &registry = first.registry();
            auto agents = registry.view<Agent>();
            const auto end = agents.end();
            const auto it = agents.begin();
            if (it != end) {
                const auto &[entity, agent] = *it;
                (void) entity;
                sim::GlobalCellPos staging{sim::kWorldEdge, sim::kWorldEdge};
                for (u32 y = 16u; y <= 4063u and staging.m_x >= sim::kWorldEdge; ++y) {
                    for (u32 x = 0u; x <= 4054u; ++x) {
                        const u32 dx = x >= agent.m_spawn.m_x ? x - agent.m_spawn.m_x : agent.m_spawn.m_x - x;
                        const u32 dy = y >= agent.m_spawn.m_y ? y - agent.m_spawn.m_y : agent.m_spawn.m_y - y;
                        if (dx + dy >= 12u and dx + dy <= 30u and isWalkable(first.grid(), {x, y})) {
                            staging = {x, y};
                            break;
                        }
                    }
                }
                if (staging.m_x < sim::kWorldEdge) {
                    // Carvable wall near staging: the tick-0 budget check
                    // needs a full 64-write first tick, so the footprint must
                    // hold at least 64 non-rock cells. Row-major first fit in
                    // a bounded window keeps trips short and deterministic.
                    constexpr u32 kSiteWindow = 48u;
                    const u32 x_lo = staging.m_x > kSiteWindow ? staging.m_x - kSiteWindow : 0u;
                    const u32 y_lo = staging.m_y > kSiteWindow ? staging.m_y - kSiteWindow : 0u;
                    const u32 x_hi = staging.m_x + kSiteWindow <= sim::kWorldEdge - kWallEdge
                                         ? staging.m_x + kSiteWindow
                                         : sim::kWorldEdge - kWallEdge;
                    const u32 y_hi = staging.m_y + kSiteWindow <= sim::kWorldEdge - kWallEdge
                                         ? staging.m_y + kSiteWindow
                                         : sim::kWorldEdge - kWallEdge;
                    for (u32 wy = y_lo; wy <= y_hi and site.m_x >= sim::kWorldEdge; ++wy) {
                        for (u32 wx = x_lo; wx <= x_hi; ++wx) {
                            u32 open = 0u;
                            for (u32 dy = 0u; dy < kWallEdge and open < 64u; ++dy) {
                                for (u32 dx = 0u; dx < kWallEdge; ++dx) {
                                    const auto cell = first.grid().getCell({wx + dx, wy + dy});
                                    if (not cell) {
                                        return std::unexpected{cell.error()};
                                    }
                                    if (cell->m_element != kElementRock) {
                                        ++open;
                                    }
                                }
                            }
                            if (open >= 64u) {
                                site = {wx, wy};
                                break;
                            }
                        }
                    }
                }
            }
        }
        stage = "drive";
        float peak = 0.0f;
        u32 peak_tick = 0u;
        float final_disp = 0.0f;
        if (const std::error_code error = drive(first, site, peak, peak_tick, final_disp)) {
            return std::unexpected{error};
        }
        stage = "planned";
        const Array<sim::BodyState> first_poses = poses(first);
        {
            sim::Registry &agent_rows = first.registry();
            u32 index = 0u;
            for (const auto &[entity, agent]: agent_rows.view<Agent>()) {
                const Plan *plan = agent_rows.get<Plan>(entity);
                const PathComp *comp = agent_rows.get<PathComp>(entity);
                const sim::BodyState *pose = agent_rows.get<sim::BodyState>(entity);
                const Needs *needs = agent_rows.get<Needs>(entity);
                std::println(
                    "AGENTS-ROW {} spawn={}/{} pos={:.1f}/{:.1f} plan={} pc={} comp={} cursor={} vel={:.2f}/{:.2f} site={}/{} o2={:.0f} rest={:.0f}",
                    index, agent.m_spawn.m_x, agent.m_spawn.m_y, pose != nullptr ? pose->m_x : -1.0f,
                    pose != nullptr ? pose->m_y : -1.0f, plan != nullptr, plan != nullptr ? plan->m_pc : 0u,
                    comp != nullptr ? comp->m_count : 0u, comp != nullptr ? comp->m_cursor : 0u,
                    pose != nullptr ? pose->m_velocity_x : 0.0f, pose != nullptr ? pose->m_velocity_y : 0.0f,
                    plan != nullptr ? plan->m_site.m_x : 0u, plan != nullptr ? plan->m_site.m_y : 0u,
                    needs != nullptr ? needs->m_o2 : -1.0f, needs != nullptr ? needs->m_rest : -1.0f);
                ++index;
            }
        }
        u32 planned = 0u;
        for (const AgentSummary &row: first.agentSummaries()) {
            if (row.m_has_plan) {
                ++planned;
            }
        }
        if (planned != kAgentCount or peak <= 4.0f) {
            std::println("SMOKE-AGENTS stage=planned planned={} peak={} peak_tick={} final_disp={} result=FAIL",
                planned, peak, peak_tick, final_disp);
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        if (first.colliderPending() != 0u) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        bool chained = false;
        {
            sim::Registry &registry = first.registry();
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                (void) agent;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose == nullptr or pose->m_x < 0.0f or pose->m_y < 0.0f) {
                    continue;
                }
                if (first.chainCount(sim::ChunkPos{
                        static_cast<u32>(pose->m_x) / sim::kChunkEdge,
                        static_cast<u32>(pose->m_y) / sim::kChunkEdge
                    })
                    > 0u) {
                    chained = true;
                }
            }
        }
        if (not chained) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }

        stage = "double";
        // Double-run equality: same seed, same wall, same ticks.
        ColonyDriver second{};
        if (const std::error_code error = second.init(ColonyDriverDesc{.m_seed = kSeed})) {
            return std::unexpected{error};
        }
        sim::GlobalCellPos site2{sim::kWorldEdge, sim::kWorldEdge};
        {
            sim::Registry &registry = second.registry();
            auto agents = registry.view<Agent>();
            const auto end = agents.end();
            const auto it = agents.begin();
            if (it != end) {
                const auto &[entity, agent] = *it;
                (void) entity;
                sim::GlobalCellPos staging{sim::kWorldEdge, sim::kWorldEdge};
                for (u32 y = 16u; y <= 4063u and staging.m_x >= sim::kWorldEdge; ++y) {
                    for (u32 x = 0u; x <= 4054u; ++x) {
                        const u32 dx = x >= agent.m_spawn.m_x ? x - agent.m_spawn.m_x : agent.m_spawn.m_x - x;
                        const u32 dy = y >= agent.m_spawn.m_y ? y - agent.m_spawn.m_y : agent.m_spawn.m_y - y;
                        if (dx + dy >= 12u and dx + dy <= 30u and isWalkable(second.grid(), {x, y})) {
                            staging = {x, y};
                            break;
                        }
                    }
                }
                if (staging.m_x < sim::kWorldEdge) {
                    // Same carvable-wall search as site (must agree exactly).
                    constexpr u32 kSiteWindow = 48u;
                    const u32 x_lo = staging.m_x > kSiteWindow ? staging.m_x - kSiteWindow : 0u;
                    const u32 y_lo = staging.m_y > kSiteWindow ? staging.m_y - kSiteWindow : 0u;
                    const u32 x_hi = staging.m_x + kSiteWindow <= sim::kWorldEdge - kWallEdge
                                         ? staging.m_x + kSiteWindow
                                         : sim::kWorldEdge - kWallEdge;
                    const u32 y_hi = staging.m_y + kSiteWindow <= sim::kWorldEdge - kWallEdge
                                         ? staging.m_y + kSiteWindow
                                         : sim::kWorldEdge - kWallEdge;
                    for (u32 wy = y_lo; wy <= y_hi and site2.m_x >= sim::kWorldEdge; ++wy) {
                        for (u32 wx = x_lo; wx <= x_hi; ++wx) {
                            u32 open = 0u;
                            for (u32 dy = 0u; dy < kWallEdge and open < 64u; ++dy) {
                                for (u32 dx = 0u; dx < kWallEdge; ++dx) {
                                    const auto cell = second.grid().getCell({wx + dx, wy + dy});
                                    if (not cell) {
                                        return std::unexpected{cell.error()};
                                    }
                                    if (cell->m_element != kElementRock) {
                                        ++open;
                                    }
                                }
                            }
                            if (open >= 64u) {
                                site2 = {wx, wy};
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (not(site2 == site)) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        float peak2 = 0.0f;
        u32 peak_tick2 = 0u;
        float final_disp2 = 0.0f;
        if (const std::error_code error = drive(second, site2, peak2, peak_tick2, final_disp2)) {
            return std::unexpected{error};
        }
        if (peak2 != peak) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        const Array<sim::BodyState> second_poses = poses(second);
        if (first_poses.size() != second_poses.size() or first_poses.size() != kAgentCount) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        for (u32 index = 0u; index < first_poses.size(); ++index) {
            if (not same_pose(first_poses[index], second_poses[index])) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
        }

        stage = "replay";
        // Replay continuation: restore bodies snapshot into a third
        // driver, step paired ticks, compare.
        const sim::Snapshot snap = first.snapshotWithBodies();
        const auto bytes = sim::save(snap);
        if (not bytes) {
            return std::unexpected{bytes.error()};
        }
        const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
        if (not loaded) {
            return std::unexpected{loaded.error()};
        }
        ColonyDriver third{};
        if (const std::error_code error = third.init(ColonyDriverDesc{.m_seed = kSeed})) {
            return std::unexpected{error};
        }
        stage = "replay-apply";
        // sim::restore is ECS-only; the grid must travel separately or the
        // replay driver steps a fresh wall-less world.
        if (const std::error_code error = sim::apply(third.grid(), *loaded)) {
            return std::unexpected{error};
        }
        stage = "replay-restore";
        if (const std::error_code error = sim::restore(third.registry(), *loaded)) {
            return std::unexpected{error};
        }
        stage = "replay-bodies";
        if (const std::error_code error = third.restoreBodies()) {
            return std::unexpected{error};
        }
        stage = "replay-colliders";
        if (const std::error_code error = third.rebuildReplayColliders(*loaded)) {
            return std::unexpected{error};
        }
        // Symmetric restore: a fourth driver replays the identical restore
        // protocol. Bit-exact third/fourth equality proves post-restore
        // determinism. Comparing against the live first driver is explicitly
        // out of scope: box2d contact resolution is order-sensitive, and the
        // restore boundary cannot reproduce the live creation history
        // (bulk vs incremental statics, fresh vs churned dynamics), so a
        // contact-active agent diverges while determinism holds. Double-run
        // above remains the live-trajectory determinism proof.
        stage = "replay-fourth";
        ColonyDriver fourth{};
        if (const std::error_code error = fourth.init(ColonyDriverDesc{.m_seed = kSeed})) {
            return std::unexpected{error};
        }
        if (const std::error_code error = sim::apply(fourth.grid(), *loaded)) {
            return std::unexpected{error};
        }
        if (const std::error_code error = sim::restore(fourth.registry(), *loaded)) {
            return std::unexpected{error};
        }
        if (const std::error_code error = fourth.restoreBodies()) {
            return std::unexpected{error};
        }
        if (const std::error_code error = fourth.rebuildReplayColliders(*loaded)) {
            return std::unexpected{error};
        }
        fourth.setPaused(true);
        third.setPaused(true);
        first.setPaused(true);
        stage = "replay-step";
        for (u32 tick = 0u; tick < 10u; ++tick) {
            if (const std::error_code error = first.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = third.stepOne()) {
                return std::unexpected{error};
            }
            if (const std::error_code error = fourth.stepOne()) {
                return std::unexpected{error};
            }
        }
        const Array<sim::BodyState> third_replay = poses(third);
        const Array<sim::BodyState> fourth_replay = poses(fourth);
        stage = "replay-size";
        if (third_replay.size() != fourth_replay.size() or third_replay.size() != kAgentCount) {
            return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
        }
        stage = "replay-pose";
        for (u32 index = 0u; index < third_replay.size(); ++index) {
            if (not same_pose(third_replay[index], fourth_replay[index])) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
        }
        if (const std::error_code error = first.shutdown()) {
            return std::unexpected{error};
        }
        if (const std::error_code error = second.shutdown()) {
            return std::unexpected{error};
        }
        if (const std::error_code error = third.shutdown()) {
            return std::unexpected{error};
        }
        if (const std::error_code error = fourth.shutdown()) {
            return std::unexpected{error};
        }
        return std::array<u32, 3u>{static_cast<u32>(peak), planned, static_cast<u32>(third_replay.size())};
    }

    std::error_code runColonySmoke(const std::span<const char *const> argv) {
        // Stage entries go to stderr (unbuffered): on abort/exception the
        // last entered stage survives, which buffered stdout would lose.
        std::println(stderr, "SMOKE-STAGE pathfinding");
        const auto paths = checkPathfinding();
        if (not paths) {
            std::println("SMOKE seed={} tiles=0 pixel=0,0,0,0 paths=ERR result=FAIL error={}", kSeed,
                paths.error().message());
            return paths.error();
        }

        std::println(stderr, "SMOKE-STAGE agents");
        const char *agents_stage = "init";
        const auto agents = checkAgents(agents_stage);
        if (not agents) {
            std::println("SMOKE-AGENTS stage={} result=FAIL error={}", agents_stage, agents.error().message());
            return agents.error();
        }
        std::println("SMOKE-AGENTS agents=3 moved={} planned={} replay={} result=PASS", (*agents)[0u], (*agents)[1u],
            (*agents)[2u]);

        std::println(stderr, "SMOKE-STAGE buildings");
        ColonySmokeApp app{argv};
        const auto built = checkBuildings();
        if (not built) {
            std::println("SMOKE-BUILD result=FAIL error={}", built.error().message());
            return built.error();
        }
        std::println("SMOKE-BUILD solid=64 demolish=vacuum delta_chunks={} result=PASS", *built);

        std::println(stderr, "SMOKE-STAGE dig");
        const auto dug = checkDigCells();
        if (not dug) {
            std::println("SMOKE-DIG result=FAIL error={}", dug.error().message());
            return dug.error();
        }
        // Slice 5 dig evidence is CPU-refresh (D5 translator/GridPass upload
        // deltas on resident chunks); the offscreen GPU readback below proves
        // rasterization independently. Both must pass for the final PASS.
        std::println("SMOKE-DIG dug={} result=PASS", *dug);

        std::error_code result{};
        try {
            result = app.run();
        } catch (const std::system_error &error) {
            result = error.code();
        } catch (const std::invalid_argument &) {
            result = std::make_error_code(std::errc::invalid_argument);
        } catch (const std::bad_alloc &) {
            result = std::make_error_code(std::errc::not_enough_memory);
        } catch (...) {
            result = std::make_error_code(std::errc::state_not_recoverable);
        }
        if (not result) {
            result = app.shutdownError();
        }
        if (not result and not app.verified()) {
            result = std::make_error_code(std::errc::state_not_recoverable);
        }

        const auto &pixel = app.pixel();
        std::println("SMOKE seed={} tiles={} pixel={},{},{},{} paths={}/{}/{} result={} error={}", kSeed,
            app.visibleTiles(), pixel[0u], pixel[1u], pixel[2u], pixel[3u],
            paths->m_paths, paths->m_partials, paths->m_blocked,
            result ? "FAIL" : "PASS", result ? result.message() : "-");
        return result;
    }
}
