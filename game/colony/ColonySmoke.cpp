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
import game.colony.worldgen;
import game.colony.metric;
import game.colony.content;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonySmoke, debug, none)

    // ------------------------------------------------------------------
    // smoke helpers
    // ------------------------------------------------------------------

    namespace {
        constexpr u64 kSeed = 1234567u;
        constexpr u32 kExtent = 256u;
        constexpr u32 kCenter = kExtent / 2u;

        // ------------------------------------------------------------------
        // prepared particles
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code checkPreparedParticles(const ColonyDriver &driver) {
            struct Totals {
                double m_volume{}, m_mass{};
            };
            const auto &generated = driver.preparedWorld()->generated();
            const auto &recipe = generated.m_plan.recipe();
            std::map<u16, Totals> expected, actual;
            u64 expected_count{};
            for (const auto &region: generated.m_matter.m_regions) {
                auto &totals = expected[region.m_material];
                totals.m_volume += region.m_volume_m3;
                totals.m_mass += region.m_mass_kg;
                for (const auto &span: region.m_spans) {
                    expected_count += span.m_end_x - span.m_begin_x;
                }
            }

            const auto particles = driver.captureParticles();
            PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, particles);
            PPR_RETURN_ERROR_ON_FAIL(
                ColonySmoke,
                sim::validateMatterParticles(
                    *particles, recipe.m_materials, recipe.m_cell_size_m, nullptr, recipe.m_reference_thickness_m));
            u64 count{}, liquids{}, gases{};

            for (const auto &batch: particles->m_chunks) {
                for (const auto &particle: batch.records()) {
                    auto &totals = actual[particle.m_material];
                    totals.m_volume += particle.m_volume_m3;
                    totals.m_mass += particle.m_mass_kg;
                    ++count;
                    liquids += particle.m_phase == sim::EGenerationMatter::liquid;
                    gases += particle.m_phase == sim::EGenerationMatter::gas;
                }
            }

            if (count == 0u or
                count != expected_count or
                count != driver.particleCount() or
                liquids == 0u or
                gases == 0u or
                actual.size() != expected.size()) {
                PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
            }

            // Iterates regions rather than ranging over the map (MSVC function-signature.cpp:213 module ICE family).
            // Coverage is identical: every expected key comes from a region, and stray actual-only keys trip the
            // size check above.
            for (const auto &region: generated.m_matter.m_regions) {
                const auto expected_found = expected.find(region.m_material);
                const auto actual_found = actual.find(region.m_material);
                if (expected_found == expected.end() or
                    actual_found == actual.end() or
                    std::abs(actual_found->second.m_volume - expected_found->second.m_volume) >
                        1.0e-8 * (std::max) (1.0, expected_found->second.m_volume) or
                    std::abs(actual_found->second.m_mass - expected_found->second.m_mass) >
                        1.0e-8 * (std::max) (1.0, expected_found->second.m_mass)) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
                }
            }

            std::println(
                "SMOKE-PARTICLES count={} liquids={} gases={} materials={} result=PASS", count, liquids, gases,
                actual.size());
            return {};
        }

        // ------------------------------------------------------------------
        // prepared session
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code checkPreparedSession() {
            const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
            config::Diagnostics diagnostics{};
            auto &pool = parallel::backgroundPool();
            auto prepared = prepareWorld(
                root, "frontier", 42u, ITimerClock::steady(), diagnostics, {.m_scheduler = &pool});
            PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, prepared);

            const auto terrain_checksum = prepared->generated().m_report.m_terrain_checksum;
            const auto start = prepared->generated().m_start;
            const auto resident = prepared->grid().residentCells({15u, 23u});
            if (not resident) {
                PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
            }

            const auto *const payload = resident->data();
            auto driver = std::make_unique<ColonyDriver>();
            PPR_RETURN_ERROR_ON_FAIL(
                ColonySmoke, driver->initPrepared(
                                 {.m_seed = 42u, .m_agent_count = 1u, .m_world_generation = 7u}, std::move(*prepared)));

            const auto actor = driver->controlledEntity();
            if (not actor or
                driver->cellsPerMetre() != 3u or
                driver->worldGeneration() != 7u or
                driver->agentSummaries().size() != 1u or
                driver->grid().residentCells({15u, 23u})->data() != payload) {
                PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
            }

            // Adoption must preserve the complete owner and payload, including
            // its validated start, rather than rerasterizing or copying the grid.
            const auto *const owner = driver->preparedWorld();
            PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, checkPreparedParticles(*driver));
            const auto failed = prepareWorld(owner->catalog(), "unknown", 42u, ITimerClock::steady());
            PPR_RETURN_ERROR_ON_FAIL(
                ColonySmoke, not failed and driver->preparedWorld() == owner and driver->tickCount() == 0u
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));

            PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, driver->shutdown());
            if (driver->preparedWorld() or driver->controlledEntity()) {
                PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
            }

            std::println(
                "SMOKE-PREPARED profile=frontier seed=42 terrain={:016x} ticks=0 scope=cpu-adoption result=PASS",
                terrain_checksum);
            return {};
        }

        // ------------------------------------------------------------------
        // center pixel
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code readCenterPixel(
            rhi::IDevice &device, rhi::ITexture &target, std::array<u8, 4u> &pixel) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err = make_error_code(
                    device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) [[unlikely]] {
                return err;
            }

            const std::size_t offset = static_cast<std::size_t>(kCenter) * static_cast<std::size_t>(layout.rowPitch) +
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

        // ------------------------------------------------------------------
        // pathfinding component budget and persistence
        // ------------------------------------------------------------------

        [[nodiscard]] Expected<PathCounts> checkPathfinding() {
            sim::ChunkGrid grid;
            sim::Registry registry;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, registerColonyComponents(registry));

            Pathfinder finder;
            const sim::GlobalCellPos from{2048u, 2048u};
            const sim::GlobalCellPos to{1748u, 2048u};
            const auto entity = registry.create();
            registry.emplace<PathReq>(entity, PathReq{entity, from, to, kPathCapsWalk});

            stepPathfinding(grid, registry, finder, kExpansionsPerTick);
            const PathComp *result = registry.get<PathComp>(entity);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, result == nullptr or result->m_partial
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));

            for (u32 tick = 0u; tick < 200u and result == nullptr; ++tick) {
                stepPathfinding(grid, registry, finder, kExpansionsPerTick);
                result = registry.get<PathComp>(entity);
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, result and result->m_count <= kMaxPathPts
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));
            if (not result->m_partial) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonySmoke, result->m_count != 0u and result->m_pts[result->m_count - 1u] == to
                                     ? std::error_code{}
                                     : std::make_error_code(std::errc::state_not_recoverable));
            }

            const auto checkChain = [&](const PathComp &chain) -> std::error_code {
                auto previous = from;

                for (u32 point = 0u; point < chain.m_count; ++point) {
                    const auto cell = chain.m_pts[point];
                    const u32 dx = cell.m_x >= previous.m_x ? cell.m_x - previous.m_x : previous.m_x - cell.m_x;
                    const u32 dy = cell.m_y >= previous.m_y ? cell.m_y - previous.m_y : previous.m_y - cell.m_y;
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonySmoke, dx + dy == 1u and isWalkable(grid, cell)
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::state_not_recoverable));

                    previous = cell;
                }
                return {};
            };

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, checkChain(*result));

            const PathReq want_req = *registry.get<PathReq>(entity);
            const PathComp want_comp = *result;

            const auto nearby = registry.create();
            const sim::GlobalCellPos near{2043u, 2048u};
            registry.emplace<PathReq>(nearby, PathReq{nearby, from, near, kPathCapsWalk});
            const PathComp *done = nullptr;

            for (u32 tick = 0u; tick < 100u and done == nullptr; ++tick) {
                stepPathfinding(grid, registry, finder, kExpansionsPerTick);
                done = registry.get<PathComp>(nearby);
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke,
                done and not done->m_partial and done->m_count != 0u and done->m_pts[done->m_count - 1u] == near
                    ? std::error_code{}
                    : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, checkChain(*done));

            const sim::Snapshot snapshot = sim::capture(grid, registry, kSeed);
            const auto bytes = sim::save(snapshot);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, bytes);

            const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, loaded);

            sim::Registry restored;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, registerColonyComponents(restored));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, sim::restore(restored, *loaded));

            const auto *const request = restored.get<PathReq>(entity);
            const auto *const completed = restored.get<PathComp>(entity);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, request and completed and *request == want_req and *completed == want_comp
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));
            Pathfinder again;
            stepPathfinding(grid, restored, again, kExpansionsPerTick * 4u);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, restored.get<PathComp>(entity) and *restored.get<PathComp>(entity) == want_comp
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));
            return pathCounts(registry);
        }

        // ------------------------------------------------------------------
        // progressive errand component and snapshot deltas
        // ------------------------------------------------------------------

        [[nodiscard]] Expected<u32> checkBuildings() {
            sim::ChunkGrid grid;
            sim::Registry registry;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, registerColonyComponents(registry));

            Pathfinder finder;
            Array<sim::ChunkPos> touched;
            const auto entity = registry.create();
            const Footprint print{.m_min = {120u, 120u}, .m_width = 8u, .m_height = 8u, .m_element = kElementRock};
            registry.emplace<BuildErrand>(entity, BuildErrand{print, 0u, 64u});

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, advanceErrands(grid, registry, finder, touched, 64u));

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, registry.get<BuildErrand>(entity)->m_progress == 64u and liveErrandCount(registry) == 0u
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));

            for (u32 y = 120u; y < 128u; ++y) {
                for (u32 x = 120u; x < 128u; ++x) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonySmoke, grid.getCell({x, y})->m_element == kElementRock
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::state_not_recoverable));
                }
            }

            auto demolition = print;
            demolition.m_solid = false;
            const auto removal = registry.create();
            registry.emplace<BuildErrand>(removal, BuildErrand{demolition, 0u, 64u});

            grid.clearAllDirty();
            touched.clear();

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, advanceErrands(grid, registry, finder, touched, 64u));

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonySmoke, registry.get<BuildErrand>(removal)->m_progress == 64u and liveErrandCount(registry) == 0u
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::state_not_recoverable));

            const auto snapshot = sim::capture(grid, registry, kSeed);
            const auto bytes = sim::save(snapshot);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, bytes);

            const auto loaded = sim::load(std::span<const u8>{bytes->data(), bytes->size()});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, loaded);

            sim::ChunkGrid applied;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, sim::apply(applied, *loaded));

            for (u32 y = 120u; y < 128u; ++y) {
                for (u32 x = 120u; x < 128u; ++x) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonySmoke, applied.getCell({x, y})->m_element == kElementVacuum
                                         ? std::error_code{}
                                         : std::make_error_code(std::errc::state_not_recoverable));
                }
            }

            const u32 count = safe_narrowing(snapshot.m_deltas.size());
            return count;
        }

        // ------------------------------------------------------------------
        // dig primitive validation, temperatures, no-op and ordered seams
        // ------------------------------------------------------------------

        [[nodiscard]] Expected<u32> checkDigCells() {
            const auto fail = [](const char *const sub, const char *const what) -> Expected<u32> {
                std::println(stderr, "SMOKE-DIG-FAIL {} {}", sub, what);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, std::errc::state_not_recoverable);
                return 0u;
            };

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
                        PPR_RETURN_ERROR_ON_FAIL(
                            ColonySmoke, probe.setCell({x0 + col, y0 + row}, sim::Cell{kElementRock, temp}));
                    }
                }
                return {};
            };

            const auto rockTemp = [](const float base, const u32 index) noexcept {
                return base + static_cast<float>(index);
            };

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, seedRock(64u, 64u, 8u, 8u, 300.0f));
            probe.clearAllDirty();

            if (probe.dirtyChunkCount() != 0u) {
                return fail("D1", "seed dirt");
            }
            const u32 clean_dirt = probe.dirtyChunkCount();
            const Expected<DigResult> zero_dig = digCells(
                probe, DigRect{.m_min = {64u, 64u}, .m_width = 0u, .m_height = 8u});
            const bool zero_rejected = not zero_dig;
            const bool zero_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not zero_rejected or not zero_clean) {
                return fail("D1", "zero edge");
            }
            const Expected<DigResult> wide_dig = digCells(
                probe, DigRect{.m_min = {64u, 64u}, .m_width = kMaxDigEdge + 1u, .m_height = 8u});
            const bool wide_rejected = not wide_dig;
            const bool wide_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not wide_rejected or not wide_clean) {
                return fail("D1", "oversized edge");
            }
            const Expected<DigResult> oob_dig = digCells(
                probe, DigRect{.m_min = {sim::kWorldEdge - 4u, sim::kWorldEdge - 4u}, .m_width = 8u, .m_height = 8u});
            const bool oob_rejected = not oob_dig;
            const bool oob_clean = probe.dirtyChunkCount() == clean_dirt;
            if (not oob_rejected or not oob_clean) {
                return fail("D1", "boundary overflow");
            }
            const auto witness = probe.getCell({64u, 64u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, witness);
            const bool witness_intact = witness->m_element == kElementRock and witness->m_temperature == 300.0f;
            if (not witness_intact) {
                return fail("D1", "rejected dirtied grid");
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, seedRock(200u, 200u, 1u, 1u, 310.0f));
            probe.clearAllDirty();
            const Expected<DigResult> single_dig = digCells(
                probe, DigRect{.m_min = {200u, 200u}, .m_width = 1u, .m_height = 1u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, single_dig);
            const bool single_count = single_dig->m_dug == 1u and
                                      single_dig->m_touched.size() == 1u and
                                      single_dig->m_touched[0u] == sim::chunkOf(sim::GlobalCellPos{200u, 200u});
            if (not single_count) {
                return fail("D1", "single count");
            }
            const auto single_cell = probe.getCell({200u, 200u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, single_cell);
            const bool single_kept = single_cell->m_element == kElementVacuum and single_cell->m_temperature == 310.0f;
            if (not single_kept) {
                return fail("D1", "single temp");
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, seedRock(256u, 256u, 32u, 32u, 200.0f));
            probe.clearAllDirty();
            const Expected<DigResult> full_dig = digCells(
                probe, DigRect{.m_min = {256u, 256u}, .m_width = 32u, .m_height = 32u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, full_dig);
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
                    if (not cell or
                        cell->m_element != kElementVacuum or
                        cell->m_temperature != rockTemp(200.0f, row * 32u + col)) {
                        full_kept = false;
                        break;
                    }
                }
            }

            if (not full_kept) {
                return fail("D1", "full temp");
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, seedRock(500u, 500u, 4u, 4u, 280.0f));
            for (u32 row = 0u; row < 4u; ++row) {
                for (u32 col = 0u; col < 4u; ++col) {
                    const bool vacuum_seed = (row + col) % 2u == 0u;
                    if (vacuum_seed) {
                        const float temp = rockTemp(280.0f, row * 4u + col);
                        if (const std::error_code error = probe.setCell(
                                {500u + col, 500u + row}, sim::Cell{kElementVacuum, temp})) {
                            return std::unexpected{error};
                        }
                    }
                }
            }
            probe.clearAllDirty();
            const Expected<DigResult> mixed_dig = digCells(
                probe, DigRect{.m_min = {500u, 500u}, .m_width = 4u, .m_height = 4u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, mixed_dig);
            const bool mixed_count = mixed_dig->m_dug == 8u and mixed_dig->m_touched.size() == 1u;
            if (not mixed_count) {
                return fail("D1", "mixed count");
            }
            bool mixed_kept = true;
            for (u32 row = 0u; row < 4u and mixed_kept; ++row) {
                for (u32 col = 0u; col < 4u; ++col) {
                    const auto cell = probe.getCell({500u + col, 500u + row});
                    if (not cell or
                        cell->m_element != kElementVacuum or
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
            const Expected<DigResult> repeat_dig = digCells(
                probe, DigRect{.m_min = {500u, 500u}, .m_width = 4u, .m_height = 4u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, repeat_dig);

            const bool repeat_noop = repeat_dig->m_dug == 0u and
                                     repeat_dig->m_touched.empty() and
                                     probe.dirtyChunkCount() == mixed_dirt;
            if (not repeat_noop) {
                return fail("D1", "repeat no-op");
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, seedRock(124u, 64u, 8u, 8u, 260.0f));
            probe.clearAllDirty();
            const Expected<DigResult> cross_probe = digCells(
                probe, DigRect{.m_min = {124u, 64u}, .m_width = 8u, .m_height = 8u});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonySmoke, cross_probe);

            const sim::ChunkPos cross_a = sim::chunkOf(sim::GlobalCellPos{124u, 64u});
            const sim::ChunkPos cross_b = sim::chunkOf(sim::GlobalCellPos{131u, 71u});
            const bool cross_ordered = cross_probe->m_dug == 64u and
                                       cross_probe->m_touched.size() == 2u and
                                       cross_probe->m_touched[0u] == cross_a and
                                       cross_probe->m_touched[1u] == cross_b;
            if (not cross_ordered) {
                return fail("D1", "cross-chunk order");
            }

            return cross_probe->m_dug;
        }

        // ------------------------------------------------------------------
        // standalone offscreen renderer component proof
        // ------------------------------------------------------------------

        class ColonySmokeApp final : public Application {
        public:
            explicit ColonySmokeApp(const std::span<const char *const> argv)
                : Application(
                      ApplicationDomain{
                          .m_is_headless = true,
                          .m_is_interactive = false,
                          .m_needs_presence = false,
                          .m_needs_rendering = true,
                          .m_needs_user_interface = false,
                      },
                      "colony-smoke", argv) {
                // P2 posture (deliberate): headless single-frame offscreen
                // host — the render lane stays off by domain default and
                // this host's own render() override runs inline on main
                // with identical stages. No per-frame transport, no thread.
            }

            [[nodiscard]] u32 visibleTiles() const noexcept {
                return m_visible_tiles;
            }

            [[nodiscard]] const std::array<u8, 4u> &pixel() const noexcept {
                return m_pixel;
            }

            [[nodiscard]] bool verified() const noexcept {
                return m_verified;
            }

            [[nodiscard]] std::error_code shutdownError() const noexcept {
                return m_shutdown_error;
            }

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
                if (const std::error_code error = m_pass.initialize(
                        *rhi_service, *shader_service, getContentDir().path())) [[unlikely]] {
                    return error;
                }

                m_core = {2048u, 2048u};
                PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, m_grid.setCell(m_core, {kElementRock, 293.0f}));
                const auto core = std::optional{m_core};

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
                camera.updateModel(
                    TimeSpan{}, model, Viewport{PixelRect{0, 0, static_cast<int>(kExtent), static_cast<int>(kExtent)}});
                m_camera_view = camera.getSnapshot();
                return {};
            }

            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                if (const std::error_code error = Application::update(dt)) [[unlikely]] {
                    return error;
                }
                if (const std::error_code error = m_translator.submit(m_grid, m_pass)) [[unlikely]] {
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
                    .m_tile_range =
                        {left, bottom, left + static_cast<i32>(sim::kChunkEdge),
                            bottom + static_cast<i32>(sim::kChunkEdge)},
                    .m_chunk_id = sim::chunkIndexOf(chunk),
                };
                const auto plan = GridPass::planTiles(m_camera_view, std::span<const GridTileSubmission>{&sample, 1u});
                if (not plan) [[unlikely]] {
                    return plan.error();
                }
                m_visible_tiles = static_cast<u32>(plan->m_tiles.size());
                if (m_visible_tiles == 0u or m_pass.stagedTileCount() == 0u) [[unlikely]] {
                    return std::make_error_code(std::errc::operation_not_supported);
                }

                for (;;) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, renderer.retireCompletedFrames());

                    const auto prepared = m_pass.tryPrepareCellUploads(device, renderer.completedFenceValue());
                    PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, prepared);

                    if (*prepared)
                        break;

                    PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, renderer.waitOnHost());
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
                if (const std::error_code error = make_error_code(
                        device.createTexture(desc, nullptr, m_target.writeRef()))) [[unlikely]] {
                    return error;
                }

                if (const std::error_code error = renderer.renderToTexture(
                        *m_target, {DrawSubmission{m_pass}}, ColorAttachmentOps{})) [[unlikely]] {
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
                if (m_pixel[0u] < 160u or m_pixel[1u] > 85u or m_pixel[2u] > 85u or m_pixel[3u] < 240u) [[unlikely]] {
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
                if (m_base_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(ColonySmoke, first_error, Application::shutdown());
                    m_base_started = false;
                    m_base_ready = false;
                }
                m_shutdown_error = first_error;
                return first_error;
            }

        private:
            sim::ChunkGrid m_grid{};
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
            bool m_verified{false};
        };
    }

    // ------------------------------------------------------------------
    // headless smoke
    // ------------------------------------------------------------------

    std::error_code runColonySmoke(const SmokeOptions &options) {
        // Opt-in GPU-assisted validation (default off everywhere; accepted
        // and ignored in Release). The typed value arrives from the shared
        // game invocation resolution before any stage, so the RHI service
        // carries the effective value into Application::initialize().
        if (options.m_gpu_validation) {
            IRhiService::get()->setGpuAssistedValidation(true);
        }

        // Stage entries go to stderr (unbuffered): on abort/exception the
        // last entered stage survives, which buffered stdout would lose.
        std::println(stderr, "SMOKE-STAGE prepared-session");
        PPR_RETURN_ERROR_ON_FAIL(ColonySmoke, checkPreparedSession());
        std::println(stderr, "SMOKE-STAGE pathfinding");
        const auto paths = checkPathfinding();
        if (not paths) {
            std::println(
                "SMOKE seed={} tiles=0 pixel=0,0,0,0 paths=ERR result=FAIL error={}", kSeed, paths.error().message());
            return paths.error();
        }

        std::println(stderr, "SMOKE-STAGE buildings");
        ColonySmokeApp app{std::span<const char *const>{}};
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
            PPR_RETHROW(ColonySmoke);
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
        std::println(
            "SMOKE seed={} tiles={} pixel={},{},{},{} paths={}/{}/{} result={} error={}", kSeed, app.visibleTiles(),
            pixel[0u], pixel[1u], pixel[2u], pixel[3u], paths->m_paths, paths->m_partials, paths->m_blocked,
            result ? "FAIL" : "PASS", result ? result.message() : "-");
        return result;
    }

}
