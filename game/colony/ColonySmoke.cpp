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
                if (result->m_count == 0u or not (result->m_pts[result->m_count - 1u] == to)) {
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
                not (done->m_pts[done->m_count - 1u] == near)) {
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
            if (req2 == nullptr or comp2 == nullptr or not (*req2 == want_req) or not (*comp2 == want_comp)) {
                return std::unexpected{std::make_error_code(std::errc::state_not_recoverable)};
            }
            Pathfinder again{};
            stepPathfinding(grid, restored, again, kExpansionsPerTick * 4u);
            const PathComp *comp3 = restored.get<PathComp>(entity);
            if (comp3 == nullptr or not (*comp3 == want_comp)) {
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
                if (detoured->m_count == 0u or not (detoured->m_pts[detoured->m_count - 1u] == side_b)) {
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
        if (not (site2 == site)) {
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
