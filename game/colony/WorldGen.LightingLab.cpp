module;
#include "pP/Macros.h"
module game.colony.worldgen;

import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.metric;

import std;

namespace pP::colony {
    PPR_DECLARE_LOG_CATEGORY(WorldPreparation)

    // ------------------------------------------------------------------
    // fixed Lighting Lab recipe
    // ------------------------------------------------------------------

    namespace {
        constexpr u32 kLabEdge = kLightingLabEdge;
        constexpr float kNormalTemperatureK = 300.0f;
        constexpr float kMagmaTemperatureK = 1800.0f;
        constexpr u32 kBasinWidthCells = 48u;
        constexpr u32 kBasinHeightCells = 16u;
        constexpr u32 kBasinSubdivision = 2u;
        constexpr u32 kGasColumns = 8u;
        constexpr u32 kGasRows = 8u;

        struct CellRect {
            u32 m_x{}, m_y{}, m_width{}, m_height{};
        };

        struct ShellRoom {
            u32 m_x{}, m_y{}, m_width{}, m_height{}, m_thickness{};
        };

        constexpr CellRect floorRect{0u, 0u, kLabEdge, 2u};

        // Two-cell shells except the explicitly identified one-cell enclosure.
        constexpr ShellRoom shellRooms[]{
            {32u, 768u, 208u, 176u, 2u},
            {272u, 768u, 208u, 176u, 2u},
            {512u, 768u, 208u, 176u, 2u},
            {32u, 512u, 208u, 176u, 2u},
            {272u, 512u, 208u, 176u, 2u},
            {512u, 512u, 208u, 176u, 2u},
            {752u, 512u, 208u, 176u, 1u},
            {32u, 256u, 416u, 192u, 2u},
            {32u, 32u, 208u, 176u, 2u},
            {272u, 32u, 208u, 176u, 2u},
            {512u, 32u, 208u, 176u, 2u},
        };

        // The colored L owns a separate shell identity and carve union.
        constexpr ShellRoom coloredLShellRoom{752u, 768u, 208u, 176u, 2u};
        constexpr CellRect coloredLHorizontalCorridor{754u, 770u, 204u, 48u};
        constexpr CellRect coloredLVerticalCorridor{910u, 770u, 48u, 172u};
        constexpr CellRect coloredLRedWall{800u, 818u, 110u, 2u};
        constexpr CellRect coloredLGreenWall{908u, 820u, 2u, 100u};

        // Enclosed Cornell proof room: a two-cell shell, a colored inner-wall
        // pair, a ceiling emitter band and two blocks. The camera views the x-y
        // slice, so the enclosure is already a cutaway; the closed boundary is
        // what makes multi-bounce transport observable.
        constexpr CellRect cornellShell{800u, 20u, 64u, 64u};
        constexpr CellRect cornellInterior{802u, 22u, 60u, 60u};
        constexpr CellRect cornellRedWall{800u, 22u, 2u, 60u};
        constexpr CellRect cornellGreenWall{862u, 22u, 2u, 60u};
        constexpr CellRect cornellCeilingEmitter{816u, 82u, 32u, 2u};
        constexpr CellRect cornellFloorBlock{820u, 22u, 8u, 8u};
        constexpr CellRect cornellFloatingBlock{838u, 48u, 6u, 6u};

        constexpr CellRect matchedDoors[]{
            {32u, 512u, 208u, 176u},
            {272u, 512u, 208u, 176u},
            {512u, 512u, 208u, 176u},
        };

        constexpr CellRect basinShells[]{
            {32u, 32u, 208u, 176u},
            {272u, 32u, 208u, 176u},
            {512u, 32u, 208u, 176u},
        };
        constexpr u32 kBasinCount = 3u;

        constexpr CellRect chunkSeamBarrierVertical{127u, 280u, 2u, 144u};
        constexpr CellRect chunkSeamBarrierHorizontal{224u, 383u, 192u, 2u};
        constexpr CellRect chunkSeamSource{64u, 328u, 8u, 8u};

        constexpr CellRect oneCellExteriorEmitter{740u, 588u, 4u, 16u};
        constexpr CellRect thermalSlab{512u, 256u, 448u, 2u};
        constexpr u32 kHotMetalTemperatureCount = 5u;
        constexpr u32 kPbrSquareCount = 4u;

        [[nodiscard]] std::error_code fillRect(
            sim::ChunkGrid &grid, const CellRect &rect, const u16 material, const float temperature) {
            for (u32 y = rect.m_y; y < rect.m_y + rect.m_height; ++y) {
                for (u32 x = rect.m_x; x < rect.m_x + rect.m_width; ++x) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, grid.setCell({x, y}, {material, temperature}));
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code clearRect(sim::ChunkGrid &grid, const CellRect &rect) {
            return fillRect(grid, rect, 0u, kNormalTemperatureK);
        }

        [[nodiscard]] const MaterialDefinition *terrainMaterial(
            const ContentCatalog &catalog, const std::string_view key) noexcept {
            const auto *const material = catalog.findMaterial(key);
            return material and material->m_representation == EMaterialRepresentation::terrain ? material : nullptr;
        }

        [[nodiscard]] const MaterialDefinition *particleMaterial(
            const ContentCatalog &catalog, const std::string_view key) noexcept {
            const auto *const material = catalog.findMaterial(key);
            return material and material->m_representation == EMaterialRepresentation::particle ? material : nullptr;
        }

        struct LabPalette {
            const MaterialDefinition *m_wall{}, *m_red{}, *m_green{}, *m_emitter{}, *m_glass{};
            const MaterialDefinition *m_hot_metal{}, *m_pbr_0{}, *m_pbr_1{}, *m_pbr_2{}, *m_pbr_3{};
            const MaterialDefinition *m_magma{}, *m_sand{}, *m_water{}, *m_oxygen{};
        };

        [[nodiscard]] Expected<LabPalette> resolveLabPalette(const ContentCatalog &catalog) {
            LabPalette palette{
                .m_wall = terrainMaterial(catalog, "lab_wall"),
                .m_red = terrainMaterial(catalog, "lab_red"),
                .m_green = terrainMaterial(catalog, "lab_green"),
                .m_emitter = terrainMaterial(catalog, "lab_emitter"),
                .m_glass = terrainMaterial(catalog, "lab_glass"),
                .m_hot_metal = terrainMaterial(catalog, "lab_hot_metal"),
                .m_pbr_0 = terrainMaterial(catalog, "lab_pbr_0"),
                .m_pbr_1 = terrainMaterial(catalog, "lab_pbr_1"),
                .m_pbr_2 = terrainMaterial(catalog, "lab_pbr_2"),
                .m_pbr_3 = terrainMaterial(catalog, "lab_pbr_3"),
                .m_magma = particleMaterial(catalog, "lab_magma"),
                .m_sand = particleMaterial(catalog, "sand"),
                .m_water = particleMaterial(catalog, "water"),
                .m_oxygen = particleMaterial(catalog, "oxygen")};

            const bool complete = palette.m_wall and
                                  palette.m_red and
                                  palette.m_green and
                                  palette.m_emitter and
                                  palette.m_glass and
                                  palette.m_hot_metal and
                                  palette.m_pbr_0 and
                                  palette.m_pbr_1 and
                                  palette.m_pbr_2 and
                                  palette.m_pbr_3 and
                                  palette.m_magma and
                                  palette.m_sand and
                                  palette.m_water and
                                  palette.m_oxygen;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldPreparation, complete ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            return palette;
        }

        std::error_code addBasinMatter(
            sim::InitialMatter &matter, const sim::ChunkGrid &grid, const sim::GenerationRecipe &recipe,
            const CellRect &basin, const MaterialDefinition &material, const float temperature, const u64 identity) {
            sim::InitialMatterRegion region{
                .m_identity = identity,
                .m_material = material.m_numeric_id,
                .m_temperature_k = static_cast<double>(temperature),
                .m_bulk_density_kg_m3 = material.m_reference_density_kg_m3,
                .m_spans = {},
                .m_subdivision = kBasinSubdivision};
            const u32 origin_x = basin.m_x + 8u;
            const u32 origin_y = basin.m_y + 80u;

            for (u32 row = 0u; row < kBasinHeightCells; ++row) {
                const u32 y = origin_y + row;
                region.m_spans.push_back({y, origin_x, origin_x + kBasinWidthCells});
                ++matter.m_span_count;
                for (u32 column = 0u; column < kBasinWidthCells; ++column) {
                    const auto area = sim::generationEmptyArea(grid, {origin_x + column, y}, recipe.m_cell_size_m);
                    PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, area ? std::error_code{} : area.error());
                    region.m_volume_m3 += *area * recipe.m_reference_thickness_m;
                }
            }

            region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            matter.m_regions.push_back(std::move(region));
            return {};
        }

        std::error_code addGasMatter(
            sim::InitialMatter &matter, const sim::ChunkGrid &grid, const sim::GenerationRecipe &recipe,
            const MaterialDefinition &material, const u64 identity) {
            sim::InitialMatterRegion region{
                .m_identity = identity,
                .m_material = material.m_numeric_id,
                .m_temperature_k = static_cast<double>(kNormalTemperatureK),
                .m_bulk_density_kg_m3 = material.m_reference_density_kg_m3,
                .m_spans = {}};

            for (u32 row = 0u; row < kGasRows; ++row) {
                const u32 y = 80u + 2u * row;
                for (u32 column = 0u; column < kGasColumns; ++column) {
                    const u32 x = 776u + 2u * column;
                    region.m_spans.push_back({y, x, x + 1u});
                    ++matter.m_span_count;
                    const auto area = sim::generationEmptyArea(grid, {x, y}, recipe.m_cell_size_m);
                    PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, area ? std::error_code{} : area.error());
                    region.m_volume_m3 += *area * recipe.m_reference_thickness_m;
                }
            }

            region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            matter.m_regions.push_back(std::move(region));
            return {};
        }

        [[nodiscard]] sim::GenerationReport summarizeLab(
            const ContentCatalog &catalog, const sim::ChunkGrid &grid, const sim::GenerationStart &start,
            const u64 seed) {
            sim::GenerationReport report{};
            Array<u64> counts(65536u, 0ull);
            sim::definition::SemanticFingerprint terrain{catalog.definition()};
            terrain.string("ppr.authored-lighting-lab.grid.v1");
            terrain.integer(kLabEdge);
            for (u32 cy = 0u; cy < grid.chunksPerEdge(); ++cy) {
                for (u32 cx = 0u; cx < grid.chunksPerEdge(); ++cx) {
                    const auto payload = grid.residentCells({cx, cy});
                    for (u32 index = 0u; index < sim::kCellsPerChunk; ++index) {
                        const auto cell = payload ? (*payload)[index] : sim::Cell{};
                        ++counts[cell.m_element];
                        terrain.integer(cell.m_element);
                        terrain.integer(std::bit_cast<u32>(cell.m_temperature));
                    }
                }
            }

            for (u32 id = 0u; id < counts.size(); ++id) {
                if (counts[id] != 0u) {
                    report.m_material_counts.push_back({static_cast<u16>(id), counts[id]});
                }
            }

            report.m_terrain_checksum = terrain.hash();
            terrain.string("ppr.authored-lighting-lab.output.v1");
            terrain.integer(seed);
            terrain.integer(std::bit_cast<u64>(start.m_spawn_x_m));
            terrain.integer(std::bit_cast<u64>(start.m_spawn_y_m));
            report.m_output_checksum = terrain.hash();
            report.m_grid_payload_bytes = u64(grid.residentChunkCount()) * sim::kCellsPerChunk * sizeof(sim::Cell);
            report.m_retained_known_payload_bytes = grid.retainedPayloadBytes();
            return report;
        }
    }

    // ------------------------------------------------------------------
    // rendered admission
    // ------------------------------------------------------------------

    bool isLightingLabProfile(const std::string_view profile) noexcept {
        const auto separator = profile.find_last_of("/:");
        const auto local = separator == std::string_view::npos ? profile : profile.substr(separator + 1u);
        return local == "lighting_lab";
    }

    bool isLightingLabWorld(const ContentCatalog &catalog, const std::string_view profile) noexcept {
        if (not isLightingLabProfile(profile)) {
            return false;
        }

        constexpr std::array<std::string_view, 11u> terrain_keys{
            "lab_wall", "lab_red", "lab_green", "lab_emitter", "lab_glass", "lab_hot_metal", "lab_pbr_0", "lab_pbr_1",
            "lab_pbr_2", "lab_pbr_3", "lab_magma"};
        for (const auto key: terrain_keys) {
            const auto *const material = catalog.findMaterial(key);
            const auto expected = key == "lab_magma" ? EMaterialRepresentation::particle
                                                     : EMaterialRepresentation::terrain;
            if (material == nullptr or material->m_representation != expected) {
                return false;
            }
        }

        for (const auto key: {"sand", "water", "oxygen"}) {
            const auto *const material = catalog.findMaterial(key);
            if (material == nullptr or material->m_representation != EMaterialRepresentation::particle) {
                return false;
            }
        }

        return true;
    }

    std::error_code validateRenderedWorldEdge(const u32 world_edge) noexcept {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            world_edge == kRenderedWorldEdge ? std::error_code{} : std::make_error_code(std::errc::not_supported));
        return {};
    }

    // ------------------------------------------------------------------
    // recipe
    // ------------------------------------------------------------------

    std::error_code populateLightingLab(sim::GeneratedWorld &world, const ContentCatalog &catalog) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            world.m_grid.edge() == kLabEdge ? std::error_code{} : std::make_error_code(std::errc::not_supported));

        auto palette = resolveLabPalette(catalog);
        PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, palette ? std::error_code{} : palette.error());
        const auto &materials = *palette;
        const auto &recipe = world.m_plan.recipe();
        const double cell = recipe.m_cell_size_m;

        sim::ChunkGrid grid{kLabEdge};

        // ------------------------------------------------------------------
        // shells
        // ------------------------------------------------------------------
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, floorRect, materials.m_wall->m_numeric_id, kNormalTemperatureK));
        for (const auto &room: shellRooms) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, fillRect(
                                      grid, {room.m_x, room.m_y, room.m_width, room.m_height},
                                      materials.m_wall->m_numeric_id, kNormalTemperatureK));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(
                grid,
                {coloredLShellRoom.m_x, coloredLShellRoom.m_y, coloredLShellRoom.m_width, coloredLShellRoom.m_height},
                materials.m_wall->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, cornellShell, materials.m_wall->m_numeric_id, kNormalTemperatureK));

        // ------------------------------------------------------------------
        // carve
        // ------------------------------------------------------------------
        for (const auto &room: shellRooms) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation,
                clearRect(
                    grid, {room.m_x + room.m_thickness, room.m_y + room.m_thickness,
                              room.m_width - 2u * room.m_thickness, room.m_height - 2u * room.m_thickness}));
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, clearRect(grid, coloredLHorizontalCorridor));
        PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, clearRect(grid, coloredLVerticalCorridor));
        PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, clearRect(grid, cornellInterior));

        // ------------------------------------------------------------------
        // divider
        // ------------------------------------------------------------------
        for (const auto &origin: matchedDoors) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, fillRect(
                                      grid, {origin.m_x + 103u, origin.m_y + 2u, 2u, 172u},
                                      materials.m_wall->m_numeric_id, kNormalTemperatureK));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, chunkSeamBarrierVertical, materials.m_wall->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, chunkSeamBarrierHorizontal, materials.m_wall->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, coloredLRedWall, materials.m_red->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, coloredLGreenWall, materials.m_green->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, cornellRedWall, materials.m_red->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, cornellGreenWall, materials.m_green->m_numeric_id, kNormalTemperatureK));

        // ------------------------------------------------------------------
        // aperture
        // ------------------------------------------------------------------
        PPR_RETURN_ERROR_ON_FAIL(WorldPreparation, clearRect(grid, {368u, 942u, 16u, 2u}));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, clearRect(grid, {matchedDoors[0u].m_x + 103u, matchedDoors[0u].m_y + 64u, 2u, 32u}));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(
                                  grid, {matchedDoors[2u].m_x + 103u, matchedDoors[2u].m_y + 64u, 2u, 32u},
                                  materials.m_glass->m_numeric_id, kNormalTemperatureK));
        for (const auto &basin: basinShells) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation,
                clearRect(grid, {basin.m_x + 2u, basin.m_y + basin.m_height - 2u, basin.m_width - 4u, 2u}));
        }

        // ------------------------------------------------------------------
        // emitter and special station blocks
        // ------------------------------------------------------------------
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, {536u, 844u, 8u, 8u}, materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, {772u, 788u, 8u, 8u}, materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        for (const auto &origin: matchedDoors) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, fillRect(
                                      grid, {origin.m_x + 24u, origin.m_y + 76u, 8u, 8u},
                                      materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, oneCellExteriorEmitter, materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, chunkSeamSource, materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, thermalSlab, materials.m_wall->m_numeric_id, kNormalTemperatureK));

        // Cornell proof room: a ceiling emitter band over half the width and
        // two white blocks, one standing on the floor and one floating, which
        // cast the soft floor shadow and split the colored walls' reflection.
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, cornellCeilingEmitter, materials.m_emitter->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, fillRect(grid, cornellFloorBlock, materials.m_wall->m_numeric_id, kNormalTemperatureK));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            fillRect(grid, cornellFloatingBlock, materials.m_wall->m_numeric_id, kNormalTemperatureK));

        constexpr std::array<float, kHotMetalTemperatureCount> hot_temperatures{
            300.0f, 1500.0f, 3000.0f, 6500.0f, 10000.0f};
        for (u32 index = 0u; index < hot_temperatures.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, fillRect(
                                      grid, {528u + 80u * index, 288u, 16u, 16u}, materials.m_hot_metal->m_numeric_id,
                                      hot_temperatures[index]));
        }

        const std::array<const MaterialDefinition *, kPbrSquareCount> pbr_materials{
            materials.m_pbr_0, materials.m_pbr_1, materials.m_pbr_2, materials.m_pbr_3};
        for (u32 index = 0u; index < pbr_materials.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, fillRect(
                                      grid, {528u + 104u * index, 368u, 24u, 24u}, pbr_materials[index]->m_numeric_id,
                                      kNormalTemperatureK));
        }

        // ------------------------------------------------------------------
        // particles
        // ------------------------------------------------------------------
        sim::InitialMatter matter;
        const std::array<const MaterialDefinition *, kBasinCount> basin_materials{
            materials.m_sand, materials.m_water, materials.m_magma};
        const std::array<float, kBasinCount> basin_temperatures{
            kNormalTemperatureK, kNormalTemperatureK, kMagmaTemperatureK};
        u64 identity = 0x6c696768746c6162ull;
        for (u32 index = 0u; index < kBasinCount; ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldPreparation, addBasinMatter(
                                      matter, grid, recipe, basinShells[index], *basin_materials[index],
                                      basin_temperatures[index], identity + index));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, addGasMatter(matter, grid, recipe, *materials.m_oxygen, identity + 0x10u));

        // ------------------------------------------------------------------
        // start and report
        // ------------------------------------------------------------------
        const sim::GenerationStart start{(744.0 + 0.5) * cell, (16.0 + 0.5) * cell, 2.0 * cell, {}};
        auto report = summarizeLab(catalog, grid, start, world.m_plan.seed());
        report.m_retained_known_payload_bytes += matter.m_regions.capacity() * sizeof(sim::InitialMatterRegion);
        for (const auto &region: matter.m_regions) {
            report.m_retained_known_payload_bytes += region.m_spans.capacity() * sizeof(sim::InitialMatterSpan);
        }

        world.m_grid = std::move(grid);
        world.m_geometry = {};
        world.m_matter = std::move(matter);
        world.m_start = start;
        world.m_report = std::move(report);
        return {};
    }

    Expected<sim::GeneratedWorld> buildLightingLabWorld(sim::GenerationPlan plan, const ContentCatalog &catalog) {
        const auto &recipe = plan.recipe();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation,
            recipe.m_world_edge == kLabEdge ? std::error_code{} : std::make_error_code(std::errc::not_supported));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, recipe.m_start_biome < recipe.m_biomes.size()
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        const double cell = recipe.m_cell_size_m;
        const std::array<sim::GenerationSite, 1u> sites{{{744.5 * cell, 16.5 * cell, recipe.m_start_biome}}};
        auto layout = sim::restoreBiomeLayout(plan, sites);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, layout);

        sim::GeneratedWorld world{std::move(plan), std::move(*layout), {}, sim::ChunkGrid{kLabEdge}, {}, {}, {}};
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, populateLightingLab(world, catalog));
        return world;
    }
}
